/*
 * battery backed block-device cache
 * Copyright (c) 2009, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */
#include <linux/platform_device.h>
#include <linux/async_tx.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/sort.h>
#include <linux/bbu.h>
#include <linux/io.h>

#include <asm/cacheflush.h>

#include "bbu.h"

/* protects bbu_device_list, bbu_region configuration, and bbu_cache_dev
 * to bbu_cache_conf conversions
 */
static DEFINE_MUTEX(bbu_lock);

/* each bbu_device manages a set of bbu_region children */
static LIST_HEAD(bbu_device_list);

static struct bbu_cache_conf *bbu_find_cache(const char uuid[16])
{
	struct bbu_device *bdev;
	struct bbu_cache_conf *conf;
	int i;

	list_for_each_entry(bdev, &bbu_device_list, node) {
		for_each_failed_region(i, bdev) {
			struct bbu_region *region = &bdev->region[i];

			/* we failed to allocate a device for this
			 * region at probe time so forward the error
			 */
			if (memcmp(uuid, region->uuid, sizeof(region->uuid)) == 0)
				return ERR_PTR(-ENOMEM);
		}
		list_for_each_entry(conf, &bdev->caches, node) {
			struct bbu_region *region = bbu_conf_to_region(conf);

			if (memcmp(uuid, region->uuid, sizeof(region->uuid)) == 0) {
				dev_dbg(conf_to_dev(conf),
					"%s found %s\n", __func__, conf->name);
				return conf;
			}
		}
	}

	return ERR_PTR(-ENODEV);
}

static int bbud(void *arg);

/* (re)initialize the active portions of a cache */
static int reset_conf(struct bbu_cache_conf *conf, struct bbu_region *region,
		      int activate_thread)
{
	unsigned long desc_pages = bbu_region_to_desc_pages(region);
	struct device *dev = conf_to_dev(conf);

	INIT_LIST_HEAD(&conf->inactive);
	INIT_LIST_HEAD(&conf->inactive_dirty);
	INIT_LIST_HEAD(&conf->handle);
	atomic_set(&conf->active, 0);
	atomic_set(&conf->dirty, 0);
	atomic_set(&conf->writeback_active, 0);
	atomic_set(&conf->active_bypass, 0);
	conf->dirty_merge_bios = NULL;
	conf->inactive_blocked = 0;
	conf->barrier_active = 0;
	conf->requesters = 0;
	memset(conf->hashtbl, 0, PAGE_SIZE);

	if (activate_thread) {
		BUG_ON(conf->task);
		set_bit(BBUD_WAKE, &conf->task_flags);
		conf->task = kthread_run(bbud, conf, "%s", conf->name);
		if (IS_ERR(conf->task)) {
			int err = PTR_ERR(conf->task);

			dev_err(dev, "%s: %s failed to start work thread: %d\n",
				conf->name, __func__, err);
			conf->task = NULL;
			return err;
		}
	}

	dev_info(dev, "%s %s %dMB @ %llx\n",
		 activate_thread ? "activated" : "allocated", conf->name,
		 region->size, (region->start_pfn + desc_pages) << PAGE_SHIFT);

	return 0;
}

static int alloc_bbu_cache(struct bbu_cache_conf *conf)
{
	struct bbu_region *region = bbu_conf_to_region(conf);
	int total_blks = bbu_conf_to_blks(conf);
	int members = conf->stripe_members;
	struct bbu_cache_ent *ent;
	LIST_HEAD(ents);
	size_t ent_size;
	int i, err;

	if (unlikely(conf->state != BBU_inactive)) {
		dev_WARN(conf_to_dev(conf),
			 "%s: already active, failing new registration\n",
			 conf->name);
		return -EBUSY;
	}

	err = reset_conf(conf, region, 1);
	if (err)
		return err;

	conf->total_ents = total_blks / members;

	ent_size = sizeof(*ent)+(members-1)*sizeof(struct bbu_io_ent);
	conf->mem_cache = kmem_cache_create(conf->name, ent_size, 0, 0, NULL);
	if (!conf->mem_cache)
		return -ENOMEM;

	for (i = 0; i < conf->total_ents; i++) {
		ent = kmem_cache_alloc(conf->mem_cache, GFP_KERNEL);
		if (ent) {
			int blk_pages = 1 << conf->blk_order;
			struct bbu_io_ent *blk;
			int j;

			memset(ent, 0, ent_size);
			list_add(&ent->lru, &ents);
			spin_lock_init(&ent->lock);
			atomic_set(&ent->count, 0);
			ent->conf = conf;
			for (j = 0; j < members; j++) {
				blk = &ent->blk[j];
				blk->req = bio_alloc(GFP_KERNEL, blk_pages);
				blk->pfn = BBU_INVALID_PFN;
				atomic_set(&blk->bypass, 0);
				if (!blk->req)
					break;
			}
			if (j < members)
				break;
		} else
			break;
	}
	list_splice(&ents, &conf->inactive);
	if (i < conf->total_ents)
		return -ENOMEM;

	dev_dbg(conf_to_dev(conf),
		"%s: allocated %d ents\n", conf->name, conf->total_ents);

	return 0;
}

static void free_bbu_cache(struct bbu_cache_conf *conf, int stop)
{
	int total_blks = bbu_conf_to_blks(conf);
	struct device *dev = conf_to_dev(conf);
	struct bbu_cache_ent *ent, *e;
	int i;

	if (!stop && (conf->state == BBU_active)) {
		dev_WARN(dev, "%s: cannot release active cache\n", conf->name);
		return;
	}

	if (atomic_read(&conf->active) || atomic_read(&conf->dirty))
		dev_WARN(dev, "%s: %s %d-active and %d-dirty ents\n",
			 conf->name, __func__, atomic_read(&conf->active),
			 atomic_read(&conf->dirty));

	/* debug dump cache state */
	if (list_empty(&conf->inactive) && list_empty(&conf->inactive_dirty))
		total_blks = 0;
	for (i = 0; i < total_blks; i++) {
		int pfn_offset = i * (1 << conf->blk_order);
		unsigned long pfn = conf->data_pfn + pfn_offset;
		u64 desc = readq(conf->desc + pfn_offset);
		enum bbu_blk_state state;
		sector_t sector;

		state = bbu_desc_to_state(desc);
		sector = bbu_desc_to_sector(conf, desc);
		dev_dbg(dev, "%s: free pfn: %lx (%s) sector: %llx\n",
			conf->name, pfn, strstate(state),
			(unsigned long long) sector);
	}

	list_splice_init(&conf->inactive_dirty, &conf->inactive);
	list_for_each_entry_safe(ent, e, &conf->inactive, lru) {
		for (i = 0; i < conf->stripe_members; i++) {
			struct bbu_io_ent *blk = &ent->blk[i];

			if (blk->req) {
				bio_put(blk->req);
				blk->req = NULL;
			}
		}
		list_del(&ent->lru);
		kmem_cache_free(conf->mem_cache, ent);
	}
	atomic_set(&conf->active, 0);
	atomic_set(&conf->dirty, 0);

	if (conf->task) {
		kthread_stop(conf->task);
		conf->task = NULL;
	}

	if (conf->mem_cache) {
		kmem_cache_destroy(conf->mem_cache);
		conf->mem_cache = NULL;
	}

	/* freeing an inactive cache means we never got a block device
	 * reference, hence bdput here.
	 */
	if (conf->state == BBU_inactive && conf->bd) {
		bdput(conf->bd);
		conf->bd = NULL;
	}

	if (conf->state == BBU_active) {
		struct kobject *parent = &conf->dev->device.kobj;

		sysfs_remove_link(parent, "backing_dev");
	}
}

/* takes a backing device relative sector number and returns the sector
 * number of the bbu_cache_ent that contains the specified data block
 * and the corresponding block index
 */
static sector_t bbu_compute_sector(struct bbu_cache_conf *conf, sector_t sector,
				   int *blk_idx)
{
	sector &= ~(blk_sectors(conf) - 1);

	if (conf->stripe_sectors == 0) {
		*blk_idx = 0;

		return sector;
	} else {
		int chunk_offset;
		unsigned long chunk_number;
		sector_t ent_sector;

		chunk_offset = sector_div(sector, conf->stripe_sectors);
		chunk_number = sector;
		BUG_ON(chunk_number != sector);
		*blk_idx = chunk_number % conf->stripe_members;
		ent_sector = (chunk_number - *blk_idx) * conf->stripe_sectors;
		ent_sector += chunk_offset;

		return ent_sector;
	}
}

static struct bbu_cache_ent *bbu_find_ent(struct bbu_cache_conf *conf, sector_t sector)
{
	struct bbu_cache_ent *ent;
	struct hlist_node *hn;

	dev_dbg(conf_to_dev(conf), "%s: %s - ent %llx\n",
		conf->name, __func__, (unsigned long long) sector);

	hlist_for_each_entry(ent, hn, bbu_hash(conf, sector), hash)
		if (ent->sector == sector)
			return ent;

	dev_dbg(conf_to_dev(conf), "%s: %s - ent %llx not in cache\n",
		conf->name, __func__, (unsigned long long) sector);

	return NULL;
}

static struct bbu_cache_ent *get_free_ent(struct bbu_cache_conf *conf)
{
	struct bbu_cache_ent *ent;

	if (list_empty(&conf->inactive))
		return NULL;

	ent = list_entry(conf->inactive.next, typeof(*ent), lru);
	list_del_init(&ent->lru);
	atomic_inc(&conf->active);

	return ent;
}

static int bbu_inactive_ok(struct bbu_cache_conf *conf)
{
	struct device *dev = conf_to_dev(conf);

	dev_dbg(dev, "%s: %s inactive: %s blocked: %d (%d:%d:%d)\n",
		conf->name, __func__,
		list_empty(&conf->inactive) ? "empty" : "busy",
		conf->inactive_blocked, atomic_read(&conf->active),
		atomic_read(&conf->dirty), bbu_watermark(conf));

	if (conf->state == BBU_failed)
		return true;

	if (!list_empty(&conf->inactive) &&
	    (!conf->inactive_blocked ||
	     atomic_read(&conf->active) + atomic_read(&conf->dirty) <
	     bbu_watermark(conf)))
		return true;
	else
		return false;
}

/**
 * bbu_laundry - initiate writeback
 * @all: flag to indicate that all dirty ents should be written back
 *
 * If 'all' is zero then only enough ents to satisfy bbu_inactive_ok
 * will be scheduled for writeback and new requests are allowed while
 * this is happening.  Otherwise, we impose a barrier and guarantee that
 * every dirty ent has had a chance to be written back.
 *
 * After dropping dirty data we drop our reference to the backing device
 * to allow it to be stopped / removed
 */

static void __release_ent(struct bbu_cache_conf *conf, struct bbu_cache_ent *ent);

static void bbu_laundry(struct bbu_cache_conf *conf, int all)
{
	struct device *dev = conf_to_dev(conf);
	int release;
	int i;

	spin_lock_irq(&conf->cache_lock);
	dev_dbg(dev, "%s: all: %d dirty: %d writeback: %d blocked: %d watermark: %d\n",
		conf->name, all, atomic_read(&conf->dirty),
		atomic_read(&conf->writeback_active), conf->inactive_blocked,
		bbu_watermark(conf));
	if (!all) {
		release = atomic_read(&conf->dirty) -
			  atomic_read(&conf->writeback_active) -
			  bbu_watermark(conf) + 1;
	} else {
		/* wait for any pending barrier requests to complete */
		wait_event_lock_irq(conf->wait_for_ent, conf->barrier_active == 0,
				    conf->cache_lock, /* nothing */);

		/* block new requests and wait for the cache to idle */
		conf->barrier_active = 1;
		wait_event_lock_irq(conf->wait_for_ent,
				    conf->requesters == 0 &&
				    atomic_read(&conf->active) == 0,
				    conf->cache_lock, /* nothing */);

		release = atomic_read(&conf->dirty);
	}

	dev_dbg(dev, "%s: %s cleaning %d ent%s\n",
		conf->name, __func__, release > 0 ? release : 0,
		release == 1 ? "" : "s");

	for (i = 0; i < release; i++) {
		struct bbu_cache_ent *ent;

		if (atomic_read(&conf->dirty) == 0 ||
		    list_empty(&conf->inactive_dirty))
			break;

		ent = list_entry(conf->inactive_dirty.next, typeof(*ent), lru);
		list_del_init(&ent->lru);
		atomic_inc(&ent->count);
		atomic_inc(&conf->active);

		BUG_ON(test_and_set_bit(BBU_ENT_WRITEBACK, &ent->state));
		atomic_inc(&conf->writeback_active);

		set_bit(BBU_ENT_HANDLE, &ent->state);
		__release_ent(conf, ent);
	}

	if (all) {
		/* we wait for writeback to be idle rather than dirty == 0
		 * because i/o errors may preclude the dirty data being drained
		 */
		wait_event_lock_irq(conf->wait_for_writeback,
				    atomic_read(&conf->writeback_active) == 0,
				    conf->cache_lock, /* nothing */);
		conf->barrier_active = 0;
		wake_up(&conf->wait_for_ent);
	}

	dev_dbg(dev, "%s: %s wrote back %d ent%s\n",
		conf->name, __func__, i, i == 1 ? "" : "s");

	spin_unlock_irq(&conf->cache_lock);
}

static void wait_for_ent(struct bbu_cache_conf *conf)
{
	/* once we wait for one ent, wait until 25% of the ents are free
	 * before allowing unfettered access to the inactive list
	 */
	conf->inactive_blocked = 1;
	wait_event_lock_irq(conf->wait_for_ent, bbu_inactive_ok(conf),
			    conf->cache_lock, bbu_laundry(conf, 0));
	conf->inactive_blocked = 0;
}

static void remove_hash(struct bbu_cache_ent *ent)
{
	hlist_del_init(&ent->hash);
}

static void insert_hash(struct bbu_cache_conf *conf, struct bbu_cache_ent *ent)
{
	struct hlist_head *hp = bbu_hash(conf, ent->sector);

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long)ent->sector);

	hlist_add_head(&ent->hash, hp);
}

static void init_blk(struct bbu_cache_conf *conf, struct bbu_cache_ent *ent, int i)
{
	struct bbu_io_ent *blk = &ent->blk[i];
	sector_t blk_sector = blk_to_sector(ent, i);

	blk->req->bi_sector = blk_sector;

	if (unlikely(conf->init))
		return;

	blk->state = BBU_unassociated;
	blk->flags = 0;
	BUG_ON(bbu_desc_to_sector(conf, blk_sector | blk->state) != blk_sector);
	write_desc(blk_sector | blk->state, conf, blk);
}

static void init_ent(struct bbu_cache_ent *ent, sector_t sector)
{
	struct bbu_cache_conf *conf = ent->conf;
	struct device *dev = conf_to_dev(conf);
	int i;

	BUG_ON(atomic_read(&ent->count));
	BUG_ON(test_bit(BBU_ENT_DIRTY, &ent->state));

	dev_dbg(dev, "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long) ent->sector);

	remove_hash(ent);

	ent->sector = sector;

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];
		u64 desc;

		if (unlikely(blk->pfn == BBU_INVALID_PFN)) {
			if (unlikely(conf->init))
				desc = 0;
			else
				BUG();
		} else
			desc = read_desc(conf, blk);

		if (blk->toread || blk->read || blk->towrite || blk->written ||
		    bbu_desc_to_state(desc) != blk->state ||
		    is_blk_active(blk) ||
		    test_bit(BLK_F_LOCKED, &blk->flags) ||
		    test_bit(BLK_F_BYPASS, &blk->flags)) {
			dev_err(dev,
				"%s: %s ent %llx blk %d %p %p %p %p %d(%d)\n",
				conf->name, __func__,
				(unsigned long long)ent->sector, i, blk->toread,
				blk->read, blk->towrite, blk->written,
				blk->state, bbu_desc_to_state(desc));
			BUG();
		}
		init_blk(conf, ent, i);
	}
	insert_hash(conf, ent);
	wmb(); /* make cache descriptor updates globally visible */
}

static struct bbu_cache_ent *get_active_ent(struct bbu_cache_conf *conf,
					    sector_t sector, int flags)
{
	struct bbu_cache_ent *ent;

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx flags: %x\n",
		conf->name, __func__, (unsigned long long) sector, flags);

	spin_lock_irq(&conf->cache_lock);

	do {
		ent = bbu_find_ent(conf, sector);
		if (!ent && (!(flags & BBU_GET_F_RECYCLE_OK) ||
			     conf->state == BBU_failed))
			break;

		if (!ent) {
			if (!conf->inactive_blocked)
				ent = get_free_ent(conf);

			if (!ent && !(flags & BBU_GET_F_BLOCK_OK))
				break;

			if (!ent)
				wait_for_ent(conf);
			else
				init_ent(ent, sector);
		} else {
			if (atomic_read(&ent->count))
				BUG_ON(!list_empty(&ent->lru));
			else {
				if (!test_bit(BBU_ENT_HANDLE, &ent->state))
					atomic_inc(&conf->active);
				list_del_init(&ent->lru);
			}
		}
	} while (ent == NULL);

	if (ent)
		atomic_inc(&ent->count);

	spin_unlock_irq(&conf->cache_lock);

	return ent;
}

static void wake_bbud(struct bbu_cache_conf *conf)
{
	set_bit(BBUD_WAKE, &conf->task_flags);
	wake_up(&conf->wait_for_work);
}

static void __release_ent(struct bbu_cache_conf *conf, struct bbu_cache_ent *ent)
{
	int i;

	if (atomic_dec_and_test(&ent->count)) {
		if (unlikely(conf->init)) {
			/* This path only taken during cache state resoration */
			for (i = 0; i < conf->stripe_members; i++) {
				struct bbu_io_ent *blk = &ent->blk[i];

				if (blk->pfn == BBU_INVALID_PFN)
					break;
			}

			if (i < conf->stripe_members)
				list_add_tail(&ent->lru, &conf->init->partial);
			else if (test_bit(BBU_ENT_DIRTY, &ent->state))
				list_add_tail(&ent->lru,
					      &conf->init->complete_dirty);
			else
				list_add_tail(&ent->lru, &conf->init->complete);
			atomic_dec(&conf->active);

			return;
		}

		if (test_bit(BBU_ENT_HANDLE, &ent->state)) {
			list_add_tail(&ent->lru, &conf->handle);
			wake_bbud(conf);
		} else {
			atomic_dec(&conf->active);
			if (test_bit(BBU_ENT_DIRTY, &ent->state))
				list_add_tail(&ent->lru, &conf->inactive_dirty);
			else
				list_add_tail(&ent->lru, &conf->inactive);
			wake_up(&conf->wait_for_ent);
		}
	}
}

static void bbu_release_ent(struct bbu_cache_ent *ent)
{
	struct bbu_cache_conf *conf = ent->conf;
	unsigned long flags;

	spin_lock_irqsave(&conf->cache_lock, flags);
	__release_ent(conf, ent);
	spin_unlock_irqrestore(&conf->cache_lock, flags);
}

static void bbu_blk_set_pfn(struct bbu_cache_conf *conf, struct bbu_io_ent *blk, unsigned long pfn)
{
	int i;

	blk->pfn = pfn;
	for (i = 0; i < 1 << conf->blk_order; i++) {
		struct bio_vec *bvec = &blk->req->bi_io_vec[i];

		bvec->bv_page = pfn_to_page(blk->pfn + i);
		bvec->bv_len = PAGE_SIZE;
		bvec->bv_offset = 0;
	}
}

static int bbu_restore_cache_state(struct bbu_cache_conf *conf)
{
	int total_blks = bbu_conf_to_blks(conf);
	struct device *dev = conf_to_dev(conf);
	u64 mask = blk_sectors(conf) - 1;
	struct bbu_init_lists init;
	int leftovers = total_blks;
	int err;
	int i;

	INIT_LIST_HEAD(&init.complete);
	INIT_LIST_HEAD(&init.complete_dirty);
	INIT_LIST_HEAD(&init.partial);
	conf->init = &init;

	/* pass1 remember allocated blocks */
	for (i = 0; i < total_blks; i++) {
		int pfn_offset = i * (1 << conf->blk_order);
		unsigned long pfn = conf->data_pfn + pfn_offset;
		u64 desc = readq(conf->desc + pfn_offset);
		struct bbu_cache_ent *ent;
		enum bbu_blk_state state;
		struct bbu_io_ent *blk;
		sector_t ent_sector;
		sector_t sector;
		int blk_idx;

		state = bbu_desc_to_state(desc);
		sector = bbu_desc_to_sector(conf, desc);
		BUG_ON(sector & mask);

		/* fixup intermediate states */
		switch (state) {
		case BBU_replace_lock: /* interrupted overwrite, discard */
			state = BBU_unassociated;
			break;
		case BBU_read_lock: /* interrupted read, revert */
			state = BBU_unassociated;
			break;
		case BBU_update_lock: /* interrupted update, take new version */
			state = BBU_dirty;
			break;
		case BBU_writeback_lock: /* interrupted writeback, revert */
			state = BBU_dirty;
			break;
		case BBU_dirty:
		case BBU_sync:
		case BBU_unassociated:
			break;
		default: /* invalid cache state */
			dev_err(dev,
				"%s: descriptor %d invalid state %d (%llx)\n",
				conf->name, i, state,
				(unsigned long long) sector);
			err = -ENXIO;
			goto error;
		}
		writeq(sector | state, conf->desc + pfn_offset);

		if (state == BBU_unassociated)
			continue;

		ent_sector = bbu_compute_sector(conf, sector, &blk_idx);
		ent = get_active_ent(conf, ent_sector,
				     BBU_GET_F_BLOCK_OK | BBU_GET_F_RECYCLE_OK);
		BUG_ON(!ent);
		blk = &ent->blk[blk_idx];

		if (blk->pfn != BBU_INVALID_PFN) {
			dev_err(dev, "%s: duplicate allocation detected for blk%d!\n",
				conf->name, blk_idx);
			err = -ENXIO;
			goto error;
		}

		ent->sector = ent_sector;
		blk->state = state;
		blk->req->bi_private = ent;
		bbu_blk_set_pfn(conf, blk, pfn);
		write_desc(blk_to_sector(ent, blk_idx) | state, conf, blk);

		if (state == BBU_dirty) {
			set_bit(BLK_F_DIRTY, &blk->flags);
			set_bit(BLK_F_UPTODATE, &blk->flags);
			if (!test_and_set_bit(BBU_ENT_DIRTY, &ent->state))
				atomic_inc(&conf->dirty);
		} else if (state == BBU_sync)
			set_bit(BLK_F_UPTODATE, &blk->flags);

		dev_dbg(dev,
			"%s: restore pfn: %lx (%s) sector: %llx (ent: %llx)\n",
			conf->name, pfn, strstate(state),
			(unsigned long long) sector,
			(unsigned long long) ent_sector);

		bbu_release_ent(ent);
		leftovers--;
	}

	/* pass2 start/complete allocation of blocks to cache_ents */
	for (i = 0; i < total_blks; i++) {
		int pfn_offset = i * (1 << conf->blk_order);
		unsigned long pfn = conf->data_pfn + pfn_offset;
		u64 desc = readq(conf->desc + pfn_offset);
		struct bbu_cache_ent *ent = NULL;
		struct list_head *list;
		const enum bbu_blk_state state = bbu_desc_to_state(desc);
		int j;

		if (state != BBU_unassociated)
			continue;

		if (list_empty(&init.partial))
			if (list_empty(&conf->inactive))
				break;
			else
				list = &conf->inactive;
		else
			list = &init.partial;

		ent = list_entry(list->next, typeof(*ent), lru);
		list_del_init(&ent->lru);
		atomic_inc(&ent->count);
		atomic_inc(&conf->active);

		for (j = 0; j < conf->stripe_members; j++) {
			struct bbu_io_ent *blk = &ent->blk[j];

			if (blk->pfn == BBU_INVALID_PFN) {
				sector_t blk_sector = blk_to_sector(ent, j);
				blk->state = state;
				blk->req->bi_private = ent;
				blk->req->bi_sector = blk_sector;
				bbu_blk_set_pfn(conf, blk, pfn);
				write_desc(blk_sector | state, conf, blk);
				break;
			}
		}
		if (j >= conf->stripe_members) {
			dev_err(dev, "%s: failed to assign block %d\n",
				conf->name, i);
			err = -ENXIO;
			goto error;
		}

		dev_dbg(dev,
			"%s: assign pfn: %lx (%s) ent %llx/%d\n",
			conf->name, pfn, strstate(state),
			(unsigned long long) ent->sector, j);

		bbu_release_ent(ent);
		leftovers--;
	}

	dev_dbg(dev, "%s: %s %d leftover%s\n",
		conf->name, __func__, leftovers, leftovers == 1 ? "" : "s");

	/* All done.  Every ent should have a block per disk, the number
	 * of leftover blocks must be less than the number needed to
	 * allocate a new ent, and all ents should be idle.
	 */
	if (list_empty(&init.partial) && list_empty(&conf->inactive) &&
	    leftovers < conf->stripe_members && atomic_read(&conf->active) == 0) {
		dev_info(dev,
			 "%s: successfully restored %d blocks (%d dirty)\n",
			 conf->name, total_blks, atomic_read(&conf->dirty));
		err = 0;
	} else {
		dev_err(dev, "%s: failed to allocate blocks to all ents\n",
			conf->name);
		err = -ENXIO;
	}

 error:
	list_splice(&init.partial, &conf->inactive);
	list_splice(&init.complete, &conf->inactive);
	list_splice(&init.complete_dirty, &conf->inactive_dirty);
	conf->init = NULL;

	return err;
}

/*
 * Each ent/blk can have one or more bion attached.
 * toread/towrite point to the first in a chain.
 * The bi_next chain must be in order.
 */
static int add_ent_bio(struct bbu_cache_ent *ent, struct bio *bi, int blk_idx)
{
	struct bbu_cache_conf *conf = ent->conf;
	struct bbu_io_ent *blk = &ent->blk[blk_idx];
	bool write = bio_data_dir(bi) == WRITE;
	struct device *dev = conf_to_dev(conf);
	struct bio **bip;

	dev_dbg(dev, "%s: %s bio %llx to ent %llx\n",
		conf->name, __func__,  (unsigned long long)bi->bi_sector,
		(unsigned long long)ent->sector);

	spin_lock(&ent->lock);
	spin_lock_irq(&conf->cache_lock);
	if (write) {
		if (test_bit(BBU_ENT_WRITEBACK, &ent->state))
			goto overlap;
		bip = &blk->towrite;
	} else
		bip = &blk->toread;

	while (*bip && (*bip)->bi_sector < bi->bi_sector) {
		if ((*bip)->bi_sector + bio_sectors(*bip) > bi->bi_sector)
			goto overlap;
		bip = &(*bip)->bi_next;
	}
	if (*bip && (*bip)->bi_sector < bi->bi_sector + bio_sectors(bi))
		goto overlap;

	BUG_ON(*bip && bi->bi_next && (*bip) != bi->bi_next);
	if (*bip)
		bi->bi_next = *bip;
	if (write) {
		set_bit(BLK_F_DIRTY, &blk->flags);
		if (!test_and_set_bit(BBU_ENT_DIRTY, &ent->state))
			atomic_inc(&conf->dirty);
	}
	*bip = bi;
	bi->bi_phys_segments++;
	spin_unlock_irq(&conf->cache_lock);
	spin_unlock(&ent->lock);

	dev_dbg(dev, "%s: %s bio %llx to ent %llx (added to blk %d)\n",
		conf->name, __func__,  (unsigned long long)bi->bi_sector,
		(unsigned long long)ent->sector, blk_idx);

	if (write) {
		/* check if blk is covered */
		sector_t s = blk_to_sector(ent, blk_idx);
		sector_t base = s;

		for (bi = blk->towrite;
		     s < base + blk_sectors(conf) &&
		     bi && bi->bi_sector <= s;
		     bi = blk_next_bio(conf, bi, base)) {
			if (bi->bi_sector + bio_sectors(bi) >= s)
				s = bi->bi_sector + bio_sectors(bi);
		}
		if (s >= base + blk_sectors(conf))
			set_bit(BLK_F_OVERWRITE, &blk->flags);
	}
	return 1;

 overlap:
	dev_dbg(dev, "%s: %s bio %llx to ent %llx (overlap)\n",
		conf->name, __func__,  (unsigned long long)bi->bi_sector,
		(unsigned long long)ent->sector);

	set_bit(BLK_F_Overlap, &blk->flags);
	spin_unlock_irq(&conf->cache_lock);
	spin_unlock(&ent->lock);
	return 0;
}

static void bbu_end_bypass(struct bio *bi, int error)
{
	struct bio *orig_bi  = bi->bi_private;
	struct bbu_cache_conf *conf;
	unsigned long flags;
	int remaining;
	int uptodate = test_bit(BIO_UPTODATE, &bi->bi_flags);

	bio_put(bi);
	bi = NULL;

	conf = bbu_get_queuedata(orig_bi->bi_bdev->bd_disk->queue);

	if (!error && uptodate)
		set_bit(BIO_UPTODATE, &orig_bi->bi_flags);
	else
		clear_bit(BIO_UPTODATE, &orig_bi->bi_flags);

	spin_lock_irqsave(&conf->cache_lock, flags);
	remaining = --orig_bi->bi_phys_segments;
	/* check if we have some dirty data to merge into this request */
	if (remaining) {
		orig_bi->bi_next = conf->dirty_merge_bios;
		conf->dirty_merge_bios = orig_bi;
		wake_bbud(conf);
	}
	spin_unlock_irqrestore(&conf->cache_lock, flags);

	dev_dbg(conf_to_dev(conf), "%s: %s sector: %llx (%s) remaining: %d\n",
		conf->name, __func__, (unsigned long long) orig_bi->bi_sector,
		error || !uptodate ? "error" : "success", remaining);

	if (remaining == 0)
		bio_endio(orig_bi, error);

	if (atomic_dec_and_test(&conf->active_bypass))
		wake_up(&conf->wait_for_ent);
}

static void bbu_merge_dirty(struct bbu_cache_conf *conf, struct bio *bi)
{
	sector_t logical_sector;
	sector_t last_sector;

	logical_sector = bi->bi_sector & ~(blk_sectors(conf) - 1);
	last_sector = bi->bi_sector + bio_sectors(bi);

	for (; logical_sector < last_sector; logical_sector += blk_sectors(conf)) {
		struct bbu_cache_ent *ent;
		struct bbu_io_ent *blk;
		struct bio *toread;
		sector_t blk_sector;
		sector_t ent_sector;
		unsigned int blk_idx;

		ent_sector = bbu_compute_sector(conf, logical_sector, &blk_idx);

		ent = get_active_ent(conf, ent_sector, 0);
		if (ent) {
			blk = &ent->blk[blk_idx];
			blk_sector = blk_to_sector(ent, blk_idx);
			spin_lock_irq(&conf->cache_lock);
			toread = blk->toread;
			while (toread && toread->bi_sector <
			       blk_sector + blk_sectors(conf)) {
				if (toread == bi)
					break;
				toread = blk_next_bio(conf, toread, blk_sector);
			}
			spin_unlock_irq(&conf->cache_lock);
		} else {
			blk = NULL;
			toread = NULL;
		}

		dev_dbg(conf_to_dev(conf),
			"%s: %s, ent %llx logical %llx (%s)\n",
			conf->name, __func__, (unsigned long long) ent_sector,
			(unsigned long long) logical_sector,
			toread ? "hit" : "miss");

		if (ent) {
			bbu_release_ent(ent);

			spin_lock(&ent->lock);
			if (blk && atomic_dec_and_test(&blk->bypass))
				clear_bit(BLK_F_BYPASS, &blk->flags);
			spin_unlock(&ent->lock);
		}

		/* signal the completion of the bypass read for this block */
		if (toread) {
			if (!test_bit(BIO_UPTODATE, &toread->bi_flags))
				set_bit(BLK_F_ReadError, &blk->flags);
			set_bit(BBU_ENT_HANDLE, &ent->state);
			bbu_release_ent(ent);
		}
	}
}

static int bbu_make_request(struct request_queue *q, struct bio *bi)
{
	struct bbu_cache_conf *conf = bbu_get_queuedata(q);
	struct device *dev = conf_to_dev(conf);
	struct bio *bypass;
	sector_t logical_sector;
	sector_t last_sector;
	int hit, miss;
	int remaining;

	spin_lock_irq(&conf->cache_lock);
	if (unlikely(conf->barrier_active))
		wait_event_lock_irq(conf->wait_for_ent,
				    conf->barrier_active == 0 &&
				    conf->requesters == 0 &&
				    atomic_read(&conf->active) == 0 &&
				    atomic_read(&conf->active_bypass) == 0,
				    conf->cache_lock, /* nothing */);

	conf->requesters++;
	if (unlikely(bio_barrier(bi))) {
		conf->barrier_active = 1;
		wait_event_lock_irq(conf->wait_for_ent,
				    conf->requesters == 1 &&
				    atomic_read(&conf->active) == 0 &&
				    atomic_read(&conf->active_bypass) == 0,
				    conf->cache_lock, /* nothing */);
		conf->barrier_active = 0;
		wake_up(&conf->wait_for_ent);
	}
	spin_unlock_irq(&conf->cache_lock);

	/* There are 4 cases to handle
	 * 1/ READ: no dirty data in the cache (hit==0) ==> bypass the cache
	 * 2/ READ: some dirty data in the cache (hit != 0 && miss != 0) ==>
	 *    read from backing dev then merge in dirty data
	 * 3/ READ: entire i/o can be satisfied by dirty data, or failed to
	 *    bypass the cache (miss == 0 || bypass == NULL) ==> read
	 *    from cache
	 * 4/ WRITE: write to cache
	 */
	if (bio_data_dir(bi) == READ)
		bypass = bio_clone(bi, GFP_NOIO);
	else
		bypass = NULL;

	/* Scan through the cache and pin any dirty blocks hit by reads.
	 * We only care about dirty data that we may have already been
	 * acknowledged, new write requests that occur while this read
	 * is in flight are ignored.  A barrier is required for strict
	 * ordering otherwise reads will simply return the current disk
	 * contents merged with a snapshot of the data dirty at the time
	 * the request is issued.
	 */
	bi->bi_next = NULL;
	bi->bi_phys_segments = 1;	/* over-loaded to count active ents */
	logical_sector = bi->bi_sector & ~(blk_sectors(conf) - 1);
	last_sector = bi->bi_sector + bio_sectors(bi);
	hit = 0;
	miss = 0;

	dev_dbg(dev, "%s: %s %s (%llx-%llx)\n", conf->name, __func__,
		bio_data_dir(bi) == READ ? "READ" : "WRITE",
		(unsigned long long) bi->bi_sector,
		(unsigned long long) last_sector);
	for (; logical_sector < last_sector; logical_sector += blk_sectors(conf)) {
		struct bbu_cache_ent *ent;
		enum bbu_get_flags flags;
		struct bbu_io_ent *blk;
		unsigned int blk_idx;
		sector_t ent_sector;
		DEFINE_WAIT(w);

 retry:
		prepare_to_wait(&conf->wait_for_overlap, &w, TASK_UNINTERRUPTIBLE);

		ent_sector = bbu_compute_sector(conf, logical_sector, &blk_idx);

		/* don't recycle or wait for an ent when performing a
		 * bypass read as we only need to check for active dirty
		 * data
		 */
		if (bypass)
			flags = 0;
		else
			flags = BBU_GET_F_RECYCLE_OK | BBU_GET_F_BLOCK_OK;

		dev_dbg(dev, "%s: %s bi %p ent %llx/%d%s\n",
			conf->name, __func__, bi,
			(unsigned long long) ent_sector, blk_idx,
			bypass ? " (bypass)" : "");

		ent = get_active_ent(conf, ent_sector, flags);

		/* check if we failed to get an ent due to a failed
		 * cache, but only in the case where we expected
		 * get_active_ent() to succeed
		 */
		if (unlikely(!ent && flags)) {
			WARN_ON_ONCE(conf->state != BBU_failed);
			miss++;
			clear_bit(BIO_UPTODATE, &bi->bi_flags);
			finish_wait(&conf->wait_for_overlap, &w);
			break;
		}

		/* warning, only valid if ent is not NULL */
		blk = &ent->blk[blk_idx];

		/* skip add_ent_bio if there is no recent data in the cache */
		if (bypass) {
			if (!ent || !(test_bit(BLK_F_DIRTY, &blk->flags) ||
			    test_bit(BLK_F_UPTODATE, &blk->flags))) {
				if (ent)
					bbu_release_ent(ent);
				miss++;
				finish_wait(&conf->wait_for_overlap, &w);
				continue;
			}

			spin_lock(&ent->lock);
			atomic_inc(&blk->bypass);
			set_bit(BLK_F_BYPASS, &blk->flags);
			spin_unlock(&ent->lock);
		}

		if (!add_ent_bio(ent, bi, blk_idx)) {
			bbu_release_ent(ent);

			spin_lock(&ent->lock);
			if (bypass && atomic_dec_and_test(&blk->bypass))
				clear_bit(BLK_F_BYPASS, &blk->flags);
			spin_unlock(&ent->lock);

			schedule();
			goto retry;
		}
		finish_wait(&conf->wait_for_overlap, &w);
		hit++;

		/* if there is no bypass i/o to wait for, or this is a
		 * write, then schedule this ent to be handled
		 * immediately, otherwise take an extra reference for
		 * this block which needs to wait for the bypass i/o to
		 * complete
		 */
		if (bypass)
			atomic_inc(&ent->count);
		else
			set_bit(BBU_ENT_HANDLE, &ent->state);
		bbu_release_ent(ent);
	}

	/* If the read operation can be satisfied completely from cache
	 * then cancel the bypass operation and unpin the read-hit-dirty
	 * ents.
	 */
	if (bypass && miss == 0) {
		dev_dbg(dev, "%s: cancel bypass for %llx\n",
			conf->name, (unsigned long long) bi->bi_sector);
		bio_put(bypass);
		bypass = NULL;
		bbu_merge_dirty(conf, bi);
	}

	/* Issue the backing device i/o, we can't use
	 * generic_make_request because it will recurse into
	 * bbu_make_request, instead call the device's make_request_fn
	 * that was specified to bbu_register
	 */
	if (bypass) {
		bypass->bi_bdev = bi->bi_bdev;
		bypass->bi_private = bi;
		bypass->bi_end_io = bbu_end_bypass;
		bypass->bi_flags &= ~(1 << BIO_SEG_VALID);
		atomic_inc(&conf->active_bypass);
		spin_lock_irq(&conf->cache_lock);
		bi->bi_phys_segments++;
		spin_unlock_irq(&conf->cache_lock);
		conf->make_request(conf->queue, bypass);
	}

	spin_lock_irq(&conf->cache_lock);
	if (--conf->requesters == 0)
		wake_up(&conf->wait_for_ent);

	remaining = --bi->bi_phys_segments;
	spin_unlock_irq(&conf->cache_lock);

	if (remaining == 0)
		bio_endio(bi, 0);

	return 0;
}

static int bbu_blkdev_get(void *param)
{
	struct bbu_cache_conf *conf = param;
	struct block_device *bd = conf->bd;
	struct device *dev = conf_to_dev(conf);
	char b[BDEVNAME_SIZE];

	dev_dbg(dev, "%s: %s %s\n", conf->name, __func__, bdevname(bd, b));

	if (blkdev_get(bd, FMODE_READ|FMODE_WRITE) != 0) {
		dev_err(dev, "%s: blkdev_get for '%s' failed\n",
			conf->name, bdevname(bd, b));

		spin_lock_irq(&conf->cache_lock);
		conf->state = BBU_failed;
		spin_unlock_irq(&conf->cache_lock);
		wake_up(&conf->wait_for_ent);
		set_bit(BBU_GET_FAILED, &conf->task_flags);
	}
	clear_bit(BBU_GET_ACTIVE, &conf->task_flags);
	wake_up(&conf->wait_for_work);

	return 0;
}

static make_request_fn *__register(const char uuid[16], struct gendisk *disk,
				   make_request_fn *make_request,
				   struct bbu_device_info *info)
{
	struct bbu_cache_conf *conf;
	unsigned long stripe_sectors;
	struct block_device *bd;
	struct bbu_cache_dev *cdev;
	struct task_struct *task;
	int stripe_members;
	int err;

	if (info) {
		stripe_members = info->stripe_members ?: 1;
		stripe_sectors = stripe_members == 1 ? 0 : info->stripe_sectors;
	} else {
		stripe_members = 1;
		stripe_sectors = 0;
	}

	conf = bbu_find_cache(uuid);
	if (IS_ERR(conf))
		return ERR_PTR(PTR_ERR(conf));

	/* chunk sectors must be a multiple of the block size */
	if (blk_sectors(conf) > stripe_sectors ||
	    stripe_sectors % blk_sectors(conf))
		return ERR_PTR(-EINVAL);

	/* we need at least 1 blk per stripe member */
	if (bbu_conf_to_blks(conf) < stripe_members)
		return ERR_PTR(-EINVAL);

	conf->stripe_members = stripe_members;
	conf->stripe_sectors = stripe_sectors;
	err = alloc_bbu_cache(conf);
	if (err)
		goto error;

	conf->make_request = make_request;
	conf->queue = disk->queue;
	bbu_set_queuedata(disk->queue, conf);

	bd = bdget_disk(disk, 0);
	if (!bd) {
		err = -ENOENT;
		goto error;
	}
	conf->bd = bd;

	err = bbu_restore_cache_state(conf);
	if (err)
		goto error;

	/* we can't call blkdev_get here since it may recurse
	 * into the block device's open() routine, so queue this to a
	 * worker thread.  bbu_make_request will wait until
	 * BBU_GET_ACTIVE is clear before permitting io requests.
	 */
	set_bit(BBU_GET_ACTIVE, &conf->task_flags);
	task = kthread_run(bbu_blkdev_get, conf, "%s-get", conf->name);
	if (!task) {
		err = -ENOMEM;
		goto error;
	}

	cdev = conf->dev;
	if (info) {
		struct kobject *parent = &cdev->device.kobj;
		struct kobject *target = &disk_to_dev(disk)->kobj;

		err = sysfs_create_link(parent, target, "backing_dev");
	}
	if (err)
		goto error;

	conf->state = BBU_active;

	return bbu_make_request;

 error:
	free_bbu_cache(conf, 0);

	dev_dbg(conf_to_dev(conf), "%s: got %d (%s)\n", conf->name, err, __func__);

	return ERR_PTR(err);
}

static int __unregister(const char uuid[16], struct gendisk *disk)
{
	struct bbu_cache_conf *conf;
	struct device *dev;
	char name[BDEVNAME_SIZE];
	char uuid_str[UUID_SIZE];
	int err = 0;

	conf = bbu_find_cache(uuid);
	if (IS_ERR(conf))
		return PTR_ERR(conf);

	dev = conf_to_dev(conf);
	if (conf != bbu_get_queuedata(disk->queue)) {
		if (conf->state != BBU_inactive)
			dev_WARN(dev, "%s: %s is not associated with %s\n",
				 conf->name, uuid_to_string(uuid_str, uuid, 0),
				 __bdevname(disk_devt(disk), name));
		return -ENODEV;
	}

	if (conf->state == BBU_inactive) {
		dev_WARN(dev, "%s: unregister called on inactive cache?\n",
			 conf->name);
		return -ENODEV;
	}

	/* it is up to the caller (userspace) to ensure that the cache
	 * is clean and no new writes appear after this point
	 */
	spin_lock_irq(&conf->cache_lock);
	if (atomic_read(&conf->dirty) || atomic_read(&conf->active) ||
	    conf->requesters)
		err = -EBUSY;
	spin_unlock_irq(&conf->cache_lock);
	if (err)
		return err;

	wait_event(conf->wait_for_work,
		   !test_bit(BBU_GET_ACTIVE, &conf->task_flags));
	if (!test_bit(BBU_GET_FAILED, &conf->task_flags))
		blkdev_put(conf->bd, FMODE_READ|FMODE_WRITE);
	else {
		bdput(conf->bd);
		conf->bd = NULL;
	}
	bbu_set_queuedata(disk->queue, NULL);
	free_bbu_cache(conf, 1);
	conf->state = BBU_inactive;

	return 0;
}

/**
 * bbu_register - associate a block device with its non-volatile write cache
 * @uuid: unique identifier of the cache configuration and block device
 * @disk: gendisk for the backing device
 * @make_request: make_request_fn routine for bbu to read/write backing device
 * @info: optional extra parameters to describe the raid geometry of the device
 *
 * It is expected that the caller precludes any i/o from occurring prior
 * to this routine's return.  disk->queue->queuedata must be a pointer
 * to a location where bbu can store its private data.  The expectation
 * is that the backing device driver uses container_of() to convert this
 * into its local private data.
 */
make_request_fn *bbu_register(const char uuid[16], struct gendisk *disk,
			      make_request_fn *make_request,
			      struct bbu_device_info *info)
{
	make_request_fn *mfn;

	mutex_lock(&bbu_lock);
	mfn = __register(uuid, disk, make_request, info);
	mutex_unlock(&bbu_lock);

	return mfn;
}
EXPORT_SYMBOL_GPL(bbu_register);

/**
 * bbu_unregister - deactivate a bbu cache
 * @uuid: unique identifier of the cache configuration and block device
 * @disk: gendisk for the backing device
 *
 * Note: it is the caller's responsibility to make sure the cache is
 * idle and clean before calling this routine.
 */
int bbu_unregister(const char uuid[16], struct gendisk *disk)
{
	int err;

	mutex_lock(&bbu_lock);
	err = __unregister(uuid, disk);
	mutex_unlock(&bbu_lock);

	return err;
}
EXPORT_SYMBOL_GPL(bbu_unregister);

static int set_pages_wc(struct page *page, int num_pages)
{
	unsigned long addr = (unsigned long) page_address(page);

	return set_memory_wc(addr, num_pages);
}

static u32 calc_checksum(struct bbu_region *region)
{
	u32 checksum = 0;
	int i;

	for (i = 0; i < BBU_REGION_WORDS; i++)
		checksum += ((u32 *) region)[i];

	return checksum;
}

static struct bbu_region *validate_region(struct bbu_region *region)
{
	if (region->magic != BBU_MAGIC)
		return NULL;

	if (region->checksum != calc_checksum(region))
		return NULL;

	return region;
}

static void return_io(struct bio *return_bi)
{
	struct bio *bi = return_bi;

	while (bi) {
		return_bi = bi->bi_next;
		bi->bi_next = NULL;
		bi->bi_size = 0;
		bio_endio(bi, 0);
		bi = return_bi;
	}
}

static void handle_failure(struct bbu_cache_ent *ent,
				  struct live_ent_state *s,
				  struct bio **return_bi)
{
	struct bbu_cache_conf *conf = ent->conf;
	int i;

	for (i = conf->stripe_members; i--; ) {
		sector_t blk_sector = blk_to_sector(ent, i);
		struct bbu_io_ent *blk = &ent->blk[i];
		struct bio *bi;

		if (!test_bit(BLK_F_ReadError, &blk->flags))
			continue;

		/* fail any writes that require data to be read */
		spin_lock_irq(&conf->cache_lock);
		if (!test_bit(BLK_F_OVERWRITE, &blk->flags) &&
		    !test_bit(BLK_F_UPTODATE, &blk->flags)) {
			bi = blk->towrite;
			blk->towrite = NULL;
			if (test_and_clear_bit(BLK_F_Overlap, &blk->flags))
				wake_up(&conf->wait_for_overlap);
			if (bi)
				s->to_write--;
			while (bi && bi->bi_sector <
			       blk_sector + blk_sectors(conf)) {
				struct bio *nextbi;

				nextbi = blk_next_bio(conf, bi, blk_sector);
				clear_bit(BIO_UPTODATE, &bi->bi_flags);
				if (--bi->bi_phys_segments == 0) {
					bi->bi_next = *return_bi;
					*return_bi = bi;
				}
				bi = nextbi;
			}

		}

		/* fail any writeback attempts */
		if (test_bit(BBU_ENT_WRITEBACK, &ent->state) &&
		    !test_bit(BLK_F_UPTODATE, &blk->flags)) {
			clear_bit(BBU_ENT_WRITEBACK, &ent->state);
			wake_up(&conf->wait_for_overlap);
			if (atomic_dec_and_test(&conf->writeback_active))
				wake_up(&conf->wait_for_writeback);
			s->writeback = 0;
			clear_bit(BBU_ENT_DIRTY, &ent->state);
			atomic_dec(&conf->dirty);
		}

		/* fail any reads if the bypass has failed and the data
		 * has not reached the cache yet.
		 */
		if (!test_bit(BLK_F_Wantfill, &blk->flags)) {
			bi = blk->toread;
			blk->toread = NULL;
			if (test_and_clear_bit(BLK_F_Overlap, &blk->flags))
				wake_up(&conf->wait_for_overlap);
			if (bi)
				s->to_read--;
			while (bi && bi->bi_sector <
			       blk_sector + blk_sectors(conf)) {
				struct bio *nextbi;

				nextbi = blk_next_bio(conf, bi, blk_sector);
				clear_bit(BIO_UPTODATE, &bi->bi_flags);
				if (--bi->bi_phys_segments == 0) {
					bi->bi_next = *return_bi;
					*return_bi = bi;
				}
				bi = nextbi;
			}
		}
		spin_unlock_irq(&conf->cache_lock);
	}
}

static void handle_ent_fill(struct bbu_cache_ent *ent, struct live_ent_state *s)
{
	struct bbu_cache_conf *conf = ent->conf;
	int i;

	set_bit(BBU_ENT_HANDLE, &ent->state);
	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		/* is the data in this block needed */
		if (!test_bit(BLK_F_LOCKED, &blk->flags) &&
		    !test_bit(BLK_F_UPTODATE, &blk->flags) &&
		    (blk->toread || s->writeback ||
		     (blk->towrite &&
		      !test_bit(BLK_F_OVERWRITE, &blk->flags)))) {
				sector_t blk_sector = blk_to_sector(ent, i);

				set_bit(BLK_F_LOCKED, &blk->flags);
				set_bit(BLK_F_Wantread, &blk->flags);
				blk->state = BBU_read_lock;
				write_desc(blk_sector | blk->state, conf, blk);
				s->locked++;
				dev_dbg(conf_to_dev(conf),
					"%s: reading ent %llx block %d%s\n",
					conf->name,
					(unsigned long long) ent->sector, i,
					s->writeback ? " (writeback)" : "");
			}
	}
}

static void handle_ent_dirty(struct bbu_cache_ent *ent,
			     struct live_ent_state *s)
{
	struct bbu_cache_conf *conf = ent->conf;
	int i;

	s->run_biodrain = 1;
	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		if (blk->towrite) {
			sector_t blk_sector = blk_to_sector(ent, i);

			dev_dbg(conf_to_dev(conf), "%s: %s ent %llx blk %d\n",
				conf->name, __func__,
				(unsigned long long) ent->sector, i);

			set_bit(BLK_F_Wantdrain, &blk->flags);
			if (test_bit(BLK_F_UPTODATE, &blk->flags))
				blk->state = BBU_update_lock;
			else {
				blk->state = BBU_replace_lock;
				BUG_ON(!test_bit(BLK_F_OVERWRITE, &blk->flags));
			}
			write_desc(blk_sector | blk->state, conf, blk);
		}
	}
}


static void bbu_end_read_request(struct bio *bi, int error)
{
	struct bbu_cache_ent *ent = bi->bi_private;
	struct bbu_cache_conf *conf = ent->conf;
	int uptodate = test_bit(BIO_UPTODATE, &bi->bi_flags);
	struct device *dev = conf_to_dev(conf);
	struct bbu_io_ent *blk = NULL;
	char b[BDEVNAME_SIZE];
	int i;

	for (i = conf->stripe_members; i--; )
		if (ent->blk[i].req == bi) {
			blk = &ent->blk[i];
			break;
		}
	BUG_ON(!blk);

	dev_dbg(dev, "%s: end read request %llx/%d, count: %d, uptodate %d\n",
		conf->name, (unsigned long long) ent->sector, i,
		atomic_read(&ent->count),
		(!error && uptodate));

	clear_bit(BLK_F_LOCKED, &blk->flags);
	if (!error && uptodate) {
		set_bit(BLK_F_UPTODATE, &blk->flags);
		blk->state = BBU_sync;
		write_desc(blk_to_sector(ent, i) | blk->state, conf, blk);
	} else {
		if (printk_ratelimit())
			dev_err(dev, "%s: read error sector %llu on %s\n",
				conf->name,
				(unsigned long long) blk_to_sector(ent, i),
				bdevname(conf->bd, b));
		clear_bit(BLK_F_UPTODATE, &blk->flags);
		set_bit(BLK_F_ReadError, &blk->flags);
	}

	set_bit(BBU_ENT_HANDLE, &ent->state);
	bbu_release_ent(ent);
}

static void bbu_end_write_request(struct bio *bi, int error)
{
	struct bbu_cache_ent *ent = bi->bi_private;
	struct bbu_cache_conf *conf = ent->conf;
	int uptodate = test_bit(BIO_UPTODATE, &bi->bi_flags);
	struct device *dev = conf_to_dev(conf);
	struct bbu_io_ent *blk = NULL;
	char b[BDEVNAME_SIZE];
	int i;

	for (i = conf->stripe_members; i--; )
		if (ent->blk[i].req == bi) {
			blk = &ent->blk[i];
			break;
		}
	BUG_ON(!blk);

	dev_dbg(dev, "%s: end write request %llx/%d, count: %d, uptodate %d\n",
		conf->name, (unsigned long long) ent->sector, i,
		atomic_read(&ent->count),
		(!error && uptodate));

	clear_bit(BLK_F_LOCKED, &blk->flags);
	if (!error && uptodate) {
		clear_bit(BLK_F_DIRTY, &blk->flags);
		blk->state = BBU_sync;
		write_desc(blk_to_sector(ent, i) | blk->state, conf, blk);
	} else {
		unsigned long flags;

		if (printk_ratelimit())
			dev_err(dev, "%s: write error sector %llu on %s\n",
				conf->name,
				(unsigned long long) blk_to_sector(ent, i),
				bdevname(conf->bd, b));

		spin_lock_irqsave(&conf->cache_lock, flags);
		if (conf->state != BBU_failed)
			conf->state = BBU_failed;
		spin_unlock_irqrestore(&conf->cache_lock, flags);
		wake_up(&conf->wait_for_ent);
	}

	set_bit(BBU_ENT_HANDLE, &ent->state);
	bbu_release_ent(ent);
}

static void run_io(struct bbu_cache_ent *ent)
{
	struct bbu_cache_conf *conf = ent->conf;
	int i;

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];
		struct bio *bi;
		int pages = 1 << conf->blk_order;
		int rw;
		int j;

		if (test_and_clear_bit(BLK_F_Wantwrite, &blk->flags))
			rw = WRITE;
		else if (test_and_clear_bit(BLK_F_Wantread, &blk->flags))
			rw = READ;
		else
			continue;

		bi = blk->req;
		bi->bi_rw = rw;
		if (rw == WRITE)
			bi->bi_end_io = bbu_end_write_request;
		else
			bi->bi_end_io = bbu_end_read_request;

		atomic_inc(&ent->count);

		dev_dbg(conf_to_dev(conf), "%s: %s for %llx %s blk %d\n",
			conf->name, __func__, (unsigned long long) ent->sector,
			bi->bi_rw == WRITE ? "write" : "read", i);
		bi->bi_bdev = conf->bd;
		bi->bi_sector = blk_to_sector(ent, i);
		bi->bi_flags = 1 << BIO_UPTODATE;
		bi->bi_vcnt = pages;
		bi->bi_max_vecs = pages;
		bi->bi_idx = 0;
		for (j = 0; j < pages; j++) {
			bi->bi_io_vec[j].bv_len = PAGE_SIZE;
			bi->bi_io_vec[j].bv_offset = 0;
		}
		bi->bi_size = PAGE_SIZE << conf->blk_order;
		bi->bi_next = NULL;

		if (ent->sector == 0)
			dev_dbg(conf_to_dev(conf),
				"%s: %s sector: %llx dev %p flags %lx size %x vec %p\n",
				conf->name, __func__,
				(unsigned long long) bi->bi_sector, bi->bi_bdev,
				bi->bi_flags, bi->bi_size, bi->bi_io_vec);

		wait_event(conf->wait_for_work,
			   !test_bit(BBU_GET_ACTIVE, &conf->task_flags));
		if (test_bit(BBU_GET_FAILED, &conf->task_flags))
			bio_endio(bi, 1);
		else
			conf->make_request(conf->queue, bi);
	}
}

static void bbu_complete_biofill(void *p)
{
	struct bbu_cache_ent *ent = p;
	struct bbu_cache_conf *conf = ent->conf;
	struct bio *return_bi = NULL;
	int i;

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long) ent->sector);

	/* clear completed biofills */
	spin_lock_irq(&conf->cache_lock);
	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		/* acknowledge completion of a biofill operation */
		/* and check if we need to reply to a read request,
		 * new BLK_F_Wantfill requests are held off until
		 * !BBU_ENT_BIOFILL_RUN
		 */
		if (test_and_clear_bit(BLK_F_Wantfill, &blk->flags)) {
			sector_t blk_sector = blk_to_sector(ent, i);
			struct bio *rbi, *rbi2;

			BUG_ON(!blk->read);
			rbi = blk->read;
			blk->read = NULL;
			while (rbi && rbi->bi_sector <
				blk_sector + blk_sectors(conf)) {
				rbi2 = blk_next_bio(conf, rbi, blk_sector);
				if (--rbi->bi_phys_segments == 0) {
					rbi->bi_next = return_bi;
					return_bi = rbi;
				}
				rbi = rbi2;
			}
		}
	}
	spin_unlock_irq(&conf->cache_lock);
	clear_bit(BBU_ENT_BIOFILL_RUN, &ent->state);

	return_io(return_bi);

	set_bit(BBU_ENT_HANDLE, &ent->state);
	bbu_release_ent(ent);
}

static void run_biofill(struct bbu_cache_ent *ent)
{
	struct dma_async_tx_descriptor *tx = NULL;
	struct bbu_cache_conf *conf = ent->conf;
	struct async_submit_ctl submit;
	int i;

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long) ent->sector);

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		if (test_bit(BLK_F_Wantfill, &blk->flags)) {
			sector_t blk_sector = blk_to_sector(ent, i);
			struct bio *rbi;

			spin_lock_irq(&conf->cache_lock);
			blk->read = rbi = blk->toread;
			blk->toread = NULL;
			spin_unlock_irq(&conf->cache_lock);

			if (test_and_clear_bit(BLK_F_Overlap, &blk->flags))
				wake_up(&conf->wait_for_overlap);

			while (rbi && rbi->bi_sector <
				blk_sector + blk_sectors(conf)) {
				tx = async_copy_biodata(0, rbi,
							pfn_to_page(blk->pfn),
							conf->blk_order,
							blk_sector, tx);
				rbi = blk_next_bio(conf, rbi, blk_sector);
			}
		}
	}

	atomic_inc(&ent->count);
	init_async_submit(&submit, ASYNC_TX_ACK, tx, bbu_complete_biofill, ent, NULL);
	async_trigger_callback(&submit);
}

static void bbu_complete_biodrain(void *p)
{
	struct bbu_cache_ent *ent = p;
	struct bbu_cache_conf *conf = ent->conf;
	struct bio *return_bi = NULL;
	int i;

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long) ent->sector);

	/* clear completed biofills */
	spin_lock_irq(&conf->cache_lock);
	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		/* acknowledge completion of a biodrain operation */
		/* and check if we need to reply to a write request,
		 * new BLK_F_Wantdrain requests are held off until
		 * !BBU_ENT_BIODRAIN_RUN
		 */
		if (test_and_clear_bit(BLK_F_Wantdrain, &blk->flags)) {
			sector_t blk_sector = blk_to_sector(ent, i);
			struct bio *wbi, *wbi2;

			BUG_ON(!blk->written);
			wbi = blk->written;
			blk->written = NULL;
			while (wbi && wbi->bi_sector <
				blk_sector + blk_sectors(conf)) {
				wbi2 = blk_next_bio(conf, wbi, blk_sector);
				if (--wbi->bi_phys_segments == 0) {
					wbi->bi_next = return_bi;
					return_bi = wbi;
				}
				wbi = wbi2;
			}
			set_bit(BLK_F_UPTODATE, &blk->flags);
			blk->state = BBU_dirty;
			write_desc(blk_sector | blk->state, conf, blk);
		}
	}
	spin_unlock_irq(&conf->cache_lock);
	clear_bit(BBU_ENT_BIODRAIN_RUN, &ent->state);

	return_io(return_bi);

	set_bit(BBU_ENT_HANDLE, &ent->state);
	bbu_release_ent(ent);
}

static void run_biodrain(struct bbu_cache_ent *ent)
{
	struct dma_async_tx_descriptor *tx = NULL;
	struct bbu_cache_conf *conf = ent->conf;
	struct async_submit_ctl submit;
	int i;

	dev_dbg(conf_to_dev(conf), "%s: %s ent %llx\n",
		conf->name, __func__, (unsigned long long) ent->sector);

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];
		struct bio *chosen;

		if (test_bit(BLK_F_Wantdrain, &blk->flags)) {
			sector_t blk_sector = blk_to_sector(ent, i);
			struct bio *wbi;

			spin_lock(&ent->lock);
			chosen = blk->towrite;
			blk->towrite = NULL;
			BUG_ON(blk->written);
			wbi = blk->written = chosen;
			spin_unlock(&ent->lock);

			if (test_and_clear_bit(BLK_F_Overlap, &blk->flags))
				wake_up(&conf->wait_for_overlap);

			while (wbi && wbi->bi_sector <
				blk_sector + blk_sectors(conf)) {
				tx = async_copy_biodata(1, wbi,
							pfn_to_page(blk->pfn),
							conf->blk_order,
							blk_sector, tx);
				wbi = blk_next_bio(conf, wbi, blk_sector);
			}
		}
	}

	atomic_inc(&ent->count);
	init_async_submit(&submit, ASYNC_TX_ACK, tx, bbu_complete_biodrain, ent, NULL);
	async_trigger_callback(&submit);
}

static void bbu_handle_ent(struct bbu_cache_ent *ent)
{
	struct bbu_cache_conf *conf = ent->conf;
	struct device *dev = conf_to_dev(conf);
	struct bio *return_bi = NULL;
	struct live_ent_state s;
	int i;

	memset(&s, 0, sizeof(s));

	spin_lock(&ent->lock);
	dev_dbg(dev, "%s: %s ent %llx\n", conf->name, __func__,
		(unsigned long long) ent->sector);

	clear_bit(BBU_ENT_HANDLE, &ent->state);
	s.writeback = test_bit(BBU_ENT_WRITEBACK, &ent->state);

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		dev_dbg(dev,
			"%s: check %d: state %lx toread %p read %p write %p "
			"written %p\n",	conf->name, i, blk->flags, blk->toread,
			blk->read, blk->towrite, blk->written);

		/* maybe we can request a biofill operation
		 *
		 * new wantfill requests are only permitted while
		 * ops_complete_biofill is guaranteed to be inactive
		 */
		if (test_bit(BLK_F_UPTODATE, &blk->flags) && blk->toread &&
		    !test_bit(BBU_ENT_BIOFILL_RUN, &ent->state) &&
		    !test_bit(BLK_F_BYPASS, &blk->flags))
			set_bit(BLK_F_Wantfill, &blk->flags);

		/* now count some things */
		if (test_bit(BLK_F_LOCKED, &blk->flags))
			s.locked++;
		if (test_bit(BLK_F_UPTODATE, &blk->flags))
			s.uptodate++;
		if (test_bit(BLK_F_DIRTY, &blk->flags))
			s.dirty++;
		if (test_bit(BLK_F_Wantfill, &blk->flags))
			s.to_fill++;
		else if (blk->toread)
			s.to_read++;
		if (blk->towrite) {
			s.to_write++;
			if (!test_bit(BLK_F_OVERWRITE, &blk->flags))
				s.non_overwrite++;
		}
		if (test_bit(BLK_F_ReadError, &blk->flags))
			s.failed++;
	}

	dev_dbg(dev, "%s: locked=%d uptodate=%d to_read=%d to_write=%d dirty=%d"
		" failed=%d state: %lx\n", conf->name, s.locked, s.uptodate,
		s.to_read, s.to_write, s.dirty, s.failed, ent->state);

	if (s.to_fill && !test_and_set_bit(BBU_ENT_BIOFILL_RUN, &ent->state))
		s.run_biofill = 1;

	if (s.failed && s.to_read+s.to_write)
		handle_failure(ent, &s, &return_bi);

	/* read some blocks if we need to satisfy read requests,
	 * sub-block-length writes, or writebacks (which always rewrite
	 * all blocks regardless of whether they are dirty or not)
	 */
	if (s.to_read || s.non_overwrite ||
	    (s.writeback && s.uptodate < conf->stripe_members))
		handle_ent_fill(ent, &s);

	/* complete writeback and allow new incoming writes for this ent */
	if (s.writeback && s.dirty == 0 && s.locked == 0) {
		clear_bit(BBU_ENT_WRITEBACK, &ent->state);
		wake_up(&conf->wait_for_overlap);
		if (atomic_dec_and_test(&conf->writeback_active))
			wake_up(&conf->wait_for_writeback);
		clear_bit(BBU_ENT_DIRTY, &ent->state);
		atomic_dec(&conf->dirty);
		s.writeback = 0;
	}

	/* check to see if we need to write to the backing dev */
	if (s.writeback && s.locked == 0  && s.to_write == 0 &&
	    s.uptodate == conf->stripe_members &&
	    !test_bit(BBU_ENT_BIODRAIN_RUN, &ent->state))
		for (i = conf->stripe_members; i--; ) {
			struct bbu_io_ent *blk = &ent->blk[i];
			sector_t blk_sector = blk_to_sector(ent, i);

			dev_dbg(dev, "%s: writing block %d\n", conf->name, i);
			set_bit(BLK_F_LOCKED, &blk->flags);
			set_bit(BLK_F_Wantwrite, &blk->flags);
			blk->state = BBU_writeback_lock;
			write_desc(blk_sector | blk->state, conf, blk);
			s.locked++;
		}

	/* allow new writes into the cache */
	if (s.to_write && s.locked == 0 &&
	    !test_bit(BBU_ENT_WRITEBACK, &ent->state) &&
	    !test_and_set_bit(BBU_ENT_BIODRAIN_RUN, &ent->state))
		handle_ent_dirty(ent, &s);

	wmb(); /* make metadata updates globally visible */
	spin_unlock(&ent->lock);

	if (s.run_biofill)
		run_biofill(ent);
	if (s.run_biodrain)
		run_biodrain(ent);

	run_io(ent);

	return_io(return_bi);
}

static void __bbud(struct bbu_cache_conf *conf)
{
	struct bbu_cache_ent *ent;
	struct bio *merge_list;

	spin_lock_irq(&conf->cache_lock);
	merge_list = conf->dirty_merge_bios;
	conf->dirty_merge_bios = NULL;
	dev_dbg(conf_to_dev(conf), "%s: %s merge: %s handle: %s\n",
		conf->name, __func__, merge_list ? "yes" : "no",
		list_empty(&conf->handle) ? "no" : "yes");
	spin_unlock_irq(&conf->cache_lock);

	while (merge_list) {
		struct bio *bi = merge_list;

		bbu_merge_dirty(conf, bi);
		merge_list = bi->bi_next;
		bi->bi_next = NULL;
	}

	spin_lock_irq(&conf->cache_lock);
	while (1) {
		if (list_empty(&conf->handle))
			break;

		ent = list_entry(conf->handle.next, typeof(*ent), lru);
		list_del_init(&ent->lru);
		atomic_inc(&ent->count);

		spin_unlock_irq(&conf->cache_lock);

		bbu_handle_ent(ent);
		bbu_release_ent(ent);

		spin_lock_irq(&conf->cache_lock);
	}
	spin_unlock_irq(&conf->cache_lock);
}

static int bbud(void *arg)
{
	struct bbu_cache_conf *conf = arg;

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {
		if (signal_pending(current))
			flush_signals(current);

		wait_event_interruptible_timeout
			(conf->wait_for_work,
			 test_bit(BBUD_WAKE, &conf->task_flags) ||
			 kthread_should_stop(), MAX_SCHEDULE_TIMEOUT);
		clear_bit(BBUD_WAKE, &conf->task_flags);

		__bbud(conf);
	}

	return 0;
}

static struct bbu_cache_conf *
alloc_add_cache_conf(struct bbu_device *bdev, int idx)
{
	struct device *dev = &bdev->pdev->dev;
	struct bbu_region *region = &bdev->region[idx];
	struct bbu_cache_conf *conf;

	conf = devm_kzalloc(dev, sizeof(*conf), GFP_KERNEL);
	if (conf) {
		conf->hashtbl = devm_kzalloc(dev, PAGE_SIZE, GFP_KERNEL);
		if (!conf->hashtbl) {
			devm_kfree(dev, conf);
			conf = NULL;
		}
	}

	if (!conf)
		dev_err(dev, "bbu/%.16s: failed to allocate resources\n",
			region->name);
	else {
		int desc_pages = bbu_region_to_desc_pages(region);
		unsigned long data_start_pfn = region->start_pfn + desc_pages;

		init_waitqueue_head(&conf->wait_for_ent);
		init_waitqueue_head(&conf->wait_for_work);
		init_waitqueue_head(&conf->wait_for_overlap);
		init_waitqueue_head(&conf->wait_for_writeback);
		spin_lock_init(&conf->cache_lock);
		INIT_LIST_HEAD(&conf->node);
		conf->state = BBU_inactive;
		conf->parent = bdev;
		conf->region_idx = idx;
		conf->desc = page_address(pfn_to_page(region->start_pfn));
		conf->data_pfn = data_start_pfn;
		conf->blk_order = region->blk_order;
		snprintf(conf->name, sizeof(conf->name), "bbu/%.16s",
			 region->name);
		list_add(&conf->node, &bdev->caches);
		reset_conf(conf, region, 0);
	}

	return conf;
}

static void __devexit __bbu_remove(struct bbu_device *bdev, struct bbu_cache_conf *conf)
{
	/* unregister, unlink cache_dev */
	if (conf->dev) {
		device_unregister(&conf->dev->device);
		conf->dev->conf = NULL;
		conf->dev = NULL;
	}

	free_bbu_cache(conf, 0);
}

static int __devexit bbu_remove(struct platform_device *dev)
{
	struct bbu_device *bdev, *b;
	struct bbu_cache_conf *conf, *c;

	mutex_lock(&bbu_lock);
	list_for_each_entry_safe(bdev, b, &bbu_device_list, node) {
		list_for_each_entry_safe(conf, c, &bdev->caches, node) {
			__bbu_remove(bdev, conf);
			list_del(&conf->node);
		}
		list_del(&bdev->node);
	}
	mutex_unlock(&bbu_lock);

	return 0;
}

static ssize_t
state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;

	conf = cdev->conf;
	if (conf) {
		switch (conf->state) {
		case BBU_inactive:
			len += snprintf(buf, PAGE_SIZE, "inactive\n");
			break;
		case BBU_active:
			len += snprintf(buf, PAGE_SIZE, "active\n");
			break;
		case BBU_failed:
			len += snprintf(buf, PAGE_SIZE, "failed\n");
			break;
		}
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
state_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	bool delete = sysfs_streq(buf, "delete");
	int err;

	if (!delete)
		return -EINVAL;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;

	conf = cdev->conf;
	if (conf && conf->state == BBU_inactive) {
		struct bbu_region *region = bbu_conf_to_region(conf);
		struct bbu_device *bdev = conf->parent;
		struct device *dev = &bdev->pdev->dev;

		cdev->conf = NULL;
		conf->dev = NULL;
		list_del(&conf->node);

		region->magic = 0;

		devm_kfree(dev, conf);
		schedule_work(&cdev->del_work);
		dev_info(dev, "%s: removed\n", conf->name);
		err = 0;
	} else if (conf && conf->state != BBU_inactive)
		err = -EBUSY;
	mutex_unlock(&bbu_lock);

	return err ? err : cnt;
}

static ssize_t
size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		struct bbu_region *region = bbu_conf_to_region(conf);

		len += snprintf(buf, PAGE_SIZE, "%d\n", region->size);
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
meta_pfn_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		struct bbu_region *region = bbu_conf_to_region(conf);

		len += snprintf(buf, PAGE_SIZE, "%#llx\n", region->start_pfn);
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
uuid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		struct bbu_region *region = bbu_conf_to_region(conf);

		uuid_to_string(buf, region->uuid, 1);
		err = strlen(buf);
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
uuid_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int uuid[4];
	int err = parse_uuid(uuid, buf);

	if (err)
		return err;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	conf = cdev->conf;
	if (conf && conf->state == BBU_inactive) {
		struct bbu_region *region = bbu_conf_to_region(conf);

		memcpy(region->uuid, uuid, sizeof(uuid));
		region->checksum = calc_checksum(region);
	} else if (conf && conf->state != BBU_inactive)
		err = -EBUSY;
	else
		err = -ENODEV;
	mutex_unlock(&bbu_lock);

	return err ? err : cnt;
}

static ssize_t
order_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		struct bbu_region *region = bbu_conf_to_region(conf);

		len += snprintf(buf, PAGE_SIZE, "%d\n", region->blk_order);
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		len += snprintf(buf, PAGE_SIZE, "%d\n",
				atomic_read(&conf->active));
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
pfn_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		len += snprintf(buf, PAGE_SIZE, "%#lx\n", conf->data_pfn);
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
dirty_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		len += snprintf(buf, PAGE_SIZE, "%d\n",
				atomic_read(&conf->dirty));
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
writeback_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		len += snprintf(buf, PAGE_SIZE, "%d\n",
				atomic_read(&conf->writeback_active));
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
entry_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;
	size_t len = 0;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		len += snprintf(buf, PAGE_SIZE, "%d\n", conf->total_ents);
		err = len;
	}
	mutex_unlock(&bbu_lock);

	return err;
}

static ssize_t
flush_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t cnt)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf && conf->state != BBU_inactive) {
		bbu_laundry(conf, 1);
		err = 0;
	}
	mutex_unlock(&bbu_lock);

	return err ? err : cnt;
}

#ifdef DEBUG
static void dump_ent(struct bbu_cache_conf *conf, struct bbu_cache_ent *ent)
{
	struct device *dev = conf_to_dev(conf);
	int i;

	dev_dbg(dev, "%s: %s ent: %llx state: (%s%s%s%s%s ) count: %d\n",
		conf->name, __func__, (unsigned long long) ent->sector,
		test_bit(BBU_ENT_DIRTY, &ent->state) ? " dirty" : "",
		test_bit(BBU_ENT_HANDLE, &ent->state) ? " handle" : "",
		test_bit(BBU_ENT_WRITEBACK, &ent->state) ? " writeback" : "",
		test_bit(BBU_ENT_BIOFILL_RUN, &ent->state) ? " fill_run" : "",
		test_bit(BBU_ENT_BIODRAIN_RUN, &ent->state) ? " drain_run" : "",
		atomic_read(&ent->count));

	for (i = conf->stripe_members; i--; ) {
		struct bbu_io_ent *blk = &ent->blk[i];

		dev_dbg(dev,
			"%s:    blk%d state %lx toread %p read %p write %p "
			"written %p\n", conf->name, i, blk->flags, blk->toread,
			blk->read, blk->towrite, blk->written);
	}
}

static void dump_cache(struct bbu_cache_conf *conf)
{
	struct device *dev = conf_to_dev(conf);
	struct bbu_cache_ent *ent;
	struct hlist_node *hn;
	int i;

	dev_dbg(dev, "%s: %s lists: (%s%s%s%s ) flags: (%s%s ) requesters: %d"
		" bypass: %d\n", conf->name, __func__,
		list_empty(&conf->inactive) ? "" : " inactive",
		list_empty(&conf->inactive_dirty) ? "" : " inactive_dirty",
		list_empty(&conf->handle) ? "" : " handle",
		conf->dirty_merge_bios ? " dirty_merge" : "",
		conf->inactive_blocked ? " inactive_blocked" : "",
		conf->barrier_active ? " barrier" : "",
		conf->requesters, atomic_read(&conf->active_bypass));

	for (i = 0; i < NR_HASH; i++)
		hlist_for_each_entry(ent, hn, &conf->hashtbl[i], hash)
			dump_ent(conf, ent);
}

static ssize_t
debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);
	struct bbu_cache_conf *conf;
	int err;

	err = mutex_lock_interruptible(&bbu_lock);
	if (err)
		return err;
	err = -ENODEV;
	conf = cdev->conf;
	if (conf) {
		spin_lock_irq(&conf->cache_lock);
		dump_cache(conf);
		wake_up(&conf->wait_for_ent);
		spin_unlock_irq(&conf->cache_lock);

		err = snprintf(buf, PAGE_SIZE, "0\n");
	}
	mutex_unlock(&bbu_lock);

	return err;
}
#endif

static struct device_attribute bbu_attrs[] = {
	#ifdef DEBUG
	__ATTR(debug, S_IRUGO, debug_show, NULL),
	#endif
	__ATTR(state, S_IRUGO|S_IWUSR, state_show, state_store),
	__ATTR(size, S_IRUGO, size_show, NULL),
	__ATTR(meta_pfn, S_IRUGO, meta_pfn_show, NULL),
	__ATTR(uuid, S_IRUGO|S_IWUSR, uuid_show, uuid_store),
	__ATTR(order, S_IRUGO, order_show, NULL),
	__ATTR(active, S_IRUGO, active_show, NULL),
	__ATTR(pfn, S_IRUGO, pfn_show, NULL),
	__ATTR(dirty, S_IRUGO, dirty_show, NULL),
	__ATTR(writeback, S_IRUGO, writeback_show, NULL),
	__ATTR(entry_count, S_IRUGO, entry_count_show, NULL),
	__ATTR(flush, S_IWUSR, NULL, flush_store),
	__ATTR_NULL
};

static void bbu_dev_release(struct device *dev)
{
	struct bbu_cache_dev *cdev = container_of(dev, typeof(*cdev), device);

	kfree(cdev);
}

static struct class bbu_class = {
	.name		= "bbu",
	.dev_attrs	= bbu_attrs,
	.dev_release	= bbu_dev_release,
};

static void del_cache_dev(struct work_struct *work)
{
	struct bbu_cache_dev *cdev = container_of(work, typeof(*cdev), del_work);

	device_unregister(&cdev->device);
}

static int register_cache(struct bbu_device *bdev, struct bbu_cache_conf *conf)
{
	struct bbu_region *region = bbu_conf_to_region(conf);
	struct device *dev = &bdev->pdev->dev;
	struct bbu_cache_dev *cdev;
	int err = -ENOMEM;

	/* can't use devm_kzalloc here as cdev might out live bdev and conf */
	cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
	if (cdev) {
		cdev->device.class = &bbu_class;
		cdev->device.parent = &bdev->pdev->dev;
		INIT_WORK(&cdev->del_work, del_cache_dev);
		err = dev_set_name(&cdev->device, "%.16s", region->name);
		if (!err) {
			err = device_register(&cdev->device);
			if (!err) {
				conf->dev = cdev;
				cdev->conf = conf;
			}
		}

		if (err)
			kfree(cdev);
	}

	if (err)
		dev_warn(dev, "%s: sysfs registration failed: %d\n",
			 conf->name, err);
	return err;
}

static void bbu_mark_failed_region(struct bbu_device *bdev, int idx)
{
	set_bit(idx, (unsigned long *) &bdev->failed_mask);
}

static int __devinit bbu_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	resource_size_t size;
	struct bbu_device *bdev;
	unsigned long end_pfn;
	int err;
	int i;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	size = resource_size(res);
	if (!devm_request_mem_region(dev, res->start, size, pdev->name))
		return -EBUSY;

	bdev = devm_kzalloc(dev, sizeof(*bdev), GFP_KERNEL);
	if (!bdev)
		return -ENOMEM;
	INIT_LIST_HEAD(&bdev->caches);
	bdev->pdev = pdev;

	/* for consistency we do not want bbu memory backed by the cpu cache */
	bdev->start_pfn = res->start >> PAGE_SHIFT;
	end_pfn = (res->start + size - 1) >> PAGE_SHIFT;
	bdev->num_pages = end_pfn - bdev->start_pfn + 1;
	err = set_pages_wc(pfn_to_page(bdev->start_pfn), bdev->num_pages);
	if (err)
		return err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		size = resource_size(res);
		if (devm_request_mem_region(dev, res->start, size, pdev->name))
			bdev->ctrl = devm_ioremap(dev, res->start, size);
	}

	if (!bdev->ctrl)
		dev_dbg(dev, "control interface not found\n");

	mutex_lock(&bbu_lock);
	list_add(&bdev->node, &bbu_device_list);
	bdev->region = page_address(pfn_to_page(bdev->start_pfn));
	for (i = 0; i < BBU_MAX_REGIONS; i++) {
		struct bbu_region *region = &bdev->region[i];
		struct bbu_cache_conf *conf;

		region = validate_region(region);
		if (!region)
			continue;

		conf = alloc_add_cache_conf(bdev, i);
		if (!conf) {
			bbu_mark_failed_region(bdev, i);
			continue;
		}
		register_cache(bdev, conf);
	}
	mutex_unlock(&bbu_lock);

	return 0;
}

static int region_cmp(const void *_a, const void *_b)
{
	const struct bbu_region *a = *(struct bbu_region **) _a;
	const struct bbu_region *b = *(struct bbu_region **) _b;

	if (a->start_pfn < b->start_pfn)
		return -1;
	if (a->start_pfn > b->start_pfn)
		return 1;
	return 0;
}

static int insert_region(struct bbu_device *bdev, struct bbu_region *new)
{
	struct bbu_region *active[BBU_MAX_REGIONS+1];
	struct bbu_region *region = NULL;
	struct bbu_region final_region;
	struct bbu_cache_conf *conf;
	unsigned long maxsize;
	unsigned long start;
	unsigned long pos;
	int alloc_idx = -1;
	int active_count = 0;
	int i;

	/* find a free region slot and collect the active regions to
	 * scan for free space
	 */
	for (i = 0; i < BBU_MAX_REGIONS; i++) {
		struct bbu_region *slot = &bdev->region[i];

		if (validate_region(slot) == NULL) {
			if (!region) {
				alloc_idx = i;
				region = slot;
			}
		} else {
			if (strncmp(new->name, slot->name, sizeof(slot->name)) == 0)
				return -EEXIST;
			active[active_count++] = slot;
		}
	}
	if (!region)
		return -ENOSPC;

	sort(active, active_count, sizeof(active[0]), region_cmp, NULL);

	memset(&final_region, 0, sizeof(final_region));
	sprintf(final_region.name, "final");
	final_region.start_pfn = bdev->start_pfn + bdev->num_pages;
	final_region.size = 0;
	active[active_count] = &final_region;

	/* find the position and size of the largest free region */
	i = 0;
	maxsize = 0;
	pos = bdev->start_pfn + 1;
	start = pos;
	do {
		unsigned long size;

		size = active[i]->start_pfn - pos;
		if (size >= maxsize) {
			maxsize = size;
			start = pos;
		}
		pos = active[i]->start_pfn + bbu_region_to_pages(active[i]);
		i++;
	} while (active[i-1]->size);

	if (bbu_region_to_pages(new) > maxsize)
		return -ENOSPC;

	/* set a default size */
	if (new->size == 0) {
		unsigned long max = maxsize >> (20 - PAGE_SHIFT);
		unsigned long min = 1;

		/* search for the largest size (in megabytes) we can
		 * describe with the available pages and the requested
		 * blk_order
		 */
		do {
			unsigned long mid = (min + max) / 2;

			new->size = mid;
			if (bbu_region_to_pages(new) <= maxsize)
				min = mid + 1;
			else
				max = mid;
		} while (max - min > 1 && min < max);

		new->size = min;
		if (bbu_region_to_pages(new) > maxsize)
			new->size = min - 1;

		if (new->size == 0)
			return -ENOSPC;
	}

	/* initialize cache block descriptors */
	memset(page_address(pfn_to_page(start)), 0,
	       PAGE_SIZE * bbu_region_to_desc_pages(new));

	/* make sure the cache descs are init'd before new is inserted */
	wmb();

	new->magic = BBU_MAGIC;
	new->start_pfn = start;
	new->checksum = calc_checksum(new);
	*region = *new;

	conf = alloc_add_cache_conf(bdev, alloc_idx);
	if (!conf) {
		region->magic = 0; /* invalidate new region */
		return -ENOMEM;
	}

	return register_cache(bdev, conf);
}

static int parse_add_region(const char *input, struct bbu_region *new)
{
	/* tmp - 16 characters for name, 5 characters for size (MB), 1
	 * character for order (2^n pages), two ':' separators and a null
	 * termintator
	 */
	char tmp[16+1+5+1+1+1];
	int input_len = strlen(input);
	unsigned long val;
	char *c1, *c2;
	int i;

	/* input length needs to be > 0 */
	if(!input_len)
		return -EINVAL;

	memset(tmp, 0, sizeof(tmp));
	memset(new, 0, sizeof(*new));

	/* chop trailing newline */
	if (input[input_len - 1] == '\n' && input_len - 1 < sizeof(tmp)) {
		strncpy(tmp, input, sizeof(tmp));
		if (input_len <= sizeof(tmp))
			tmp[input_len - 1] = 0;
	} else if (input_len < sizeof(tmp)) {
		strncpy(tmp, input, input_len);
		tmp[input_len] = '\0';
	}
	else
		return -E2BIG;

	c1 = strchr(tmp, ':');
	/* limit names to 16 characters */
	if (c1 && c1 - tmp > 16)
		return -EINVAL;
	else if (!c1 && strlen(tmp) > 16)
		return -EINVAL;

	for (i = 0; i < (c1 ? c1 - tmp : strlen(tmp)); i++) {
		if (isalnum(tmp[i]))
			new->name[i] = tmp[i];
		else
			return -EINVAL;
	}
	if (strlen(new->name) == 0)
		return -EINVAL;

	if (!c1)
		return 0; /* default size, order */

	c1++;
	c2 = strchr(c1, ':');

	if (c2 && c2 - c1 > 5)
		return -EFBIG;
	else if (!c2 && strlen(c1) > 5)
		return -EFBIG;

	if (c1 == c2)
		*c1 = 0; /* default size */
	else if (c2) {
		*c2 = 0;
		if (strict_strtoul(c1, 10, &val) != 0)
			return -EINVAL;
		new->size = val;
	} else if (strlen(c1)) {
		if (strict_strtoul(c1, 10, &val) != 0)
			return -EINVAL;
		new->size = val;
		return 0;
	} else {
		/* default size, order */
		return 0;
	}
	c2++;

	if (*c2 == 0)
		return 0; /* default order */

	if (strlen(c2) > 1)
		return -EFBIG;

	if (strict_strtoul(c2, 10, &val) != 0)
		return -EINVAL;

	/* order is limited to a 1MB block size */
	if (val + PAGE_SHIFT > 20)
		return -EINVAL;

	new->blk_order = val;

	return 0;
}

static int add_region(const char *val, struct kernel_param *kp)
{
	int err;

	mutex_lock(&bbu_lock);
	if (list_empty(&bbu_device_list))
		err = -ENODEV;
	else {
		struct bbu_device *bdev;
		struct bbu_region new;

		err = parse_add_region(val, &new);
		if (err == 0) {
			/* do a first-fit search to insert the new region */
			list_for_each_entry(bdev, &bbu_device_list, node) {
				err = insert_region(bdev, &new);
				if (err == 0)
					break;
			}
		}
	}
	mutex_unlock(&bbu_lock);

	return err;
}
module_param_call(new_region, add_region, NULL, NULL, S_IWUSR);

static struct platform_driver bbu_driver = {
	.probe		= bbu_probe,
	.remove		= __devexit_p(bbu_remove),
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "adr",
	},
};

static int __devinit bbu_init(void)
{
	int err;

	err = class_register(&bbu_class);
	if (err)
		return err;

	err = platform_driver_register(&bbu_driver);
	if (err)
		class_unregister(&bbu_class);

	return err;
}
/* be ready before block device drivers call bbu_register() */
subsys_initcall(bbu_init);

static void __devexit bbu_exit(void)
{
	class_unregister(&bbu_class);
	platform_driver_unregister(&bbu_driver);
}
module_exit(bbu_exit);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("bbu: battery backed block-device cache");
MODULE_LICENSE("GPL");
