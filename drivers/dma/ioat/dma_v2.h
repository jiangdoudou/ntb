/*
 * Copyright(c) 2004 - 2009 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in the
 * file called COPYING.
 */
#ifndef IOATDMA_V2_H
#define IOATDMA_V2_H

#include <linux/dmaengine.h>
#include <linux/circ_buf.h>
#include "dma.h"
#include "hw.h"


extern int ioat_pending_level;
extern int ioat_ring_alloc_order;

/*
 * workaround for IOAT ver.3.0 null descriptor issue
 * (channel returns error when size is 0)
 */
#define NULL_DESC_BUFFER_SIZE 1

#define IOAT_DIR_PAGE_SHIFT	6
#define IOAT_MAX_ORDER_PER_DIR	15

/* we will restrict max entries to 32k (1 DIR) for now */
#define IOAT_MAX_ORDER IOAT_MAX_ORDER_PER_DIR
#define ioat_get_alloc_order() \
	(min(max(ioat_ring_alloc_order, 8), max(IOAT_MAX_ORDER, 8)))
#define ioat_get_max_alloc_order() \
	(min(ioat_ring_max_alloc_order, IOAT_MAX_ORDER))

/* struct ioat2_dma_chan - ioat v2 / v3 channel attributes
 * @base: common ioat channel parameters
 * @xfercap_log; log2 of channel max transfer length (for fast division)
 * @head: allocated index
 * @issued: hardware notification point
 * @tail: cleanup index
 * @dmacount: identical to 'head' except for occasionally resetting to zero
 * @alloc_order: log2 of the number of allocated descriptors
 * @produce: number of descriptors to produce at submit time
 * @ring: software ring buffer implementation of hardware ring
 * @prep_lock: serializes descriptor preparation (producers)
 * @valcount: number of pending validate operations (ioat3.2+ only)
 * @poll_work: poll for validate operation completion (ioat3.2+ only)
 * @pq_scratch: spare buffer for restarting ioat3.2 channels after error
 * @pq_scratch_dma: dma address of @pq_scratch
 */
struct ioat2_dma_chan {
	struct ioat_chan_common base;
	size_t xfercap_log;
	u16 head;
	u16 issued;
	u16 tail;
	u16 dmacount;
	u16 alloc_order;
	u16 produce;
	struct ioat2_ring_dir *dir[2];
	spinlock_t prep_lock;
	u16 valcount;
	struct work_struct poll_work;
	void *pq_scratch;
	dma_addr_t pq_scratch_dma;
};

static inline struct ioat2_dma_chan *to_ioat2_chan(struct dma_chan *c)
{
	struct ioat_chan_common *chan = to_chan_common(c);

	return container_of(chan, struct ioat2_dma_chan, base);
}

static inline u16 ioat2_ring_size(struct ioat2_dma_chan *ioat)
{
	return 1 << ioat->alloc_order;
}

/* count of descriptors in flight with the engine */
static inline u16 ioat2_ring_active(struct ioat2_dma_chan *ioat)
{
	return CIRC_CNT(ioat->head, ioat->tail, ioat2_ring_size(ioat));
}

/* count of descriptors pending submission to hardware */
static inline u16 ioat2_ring_pending(struct ioat2_dma_chan *ioat)
{
	return CIRC_CNT(ioat->head, ioat->issued, ioat2_ring_size(ioat));
}

static inline u16 ioat2_ring_space(struct ioat2_dma_chan *ioat)
{
	return ioat2_ring_size(ioat) - ioat2_ring_active(ioat);
}

static inline u16 ioat2_xferlen_to_descs(struct ioat2_dma_chan *ioat, size_t len)
{
	u16 num_descs = len >> ioat->xfercap_log;

	num_descs += !!(len & ((1 << ioat->xfercap_log) - 1));
	return num_descs;
}

struct ioat2_ring_ent {
	struct dma_async_tx_descriptor txd;
	enum sum_check_flags *result;
	size_t len;
};

#define IOAT_PAGES_PER_DIR	512
#define IOAT_DESCS_PER_PAGE	64
struct ioat2_ring_page {
	struct ioat2_ring_ent sw[IOAT_DESCS_PER_PAGE];
	union {
		struct ioat_dma_descriptor hw;
		struct ioat_fill_descriptor fill;
		struct ioat_xor_descriptor xor;
		struct ioat_xor_ext_descriptor xor_ex;
		struct ioat_pq_descriptor pq;
		struct ioat_pq_ext_descriptor pq_ex;
		struct ioat_pq_update_descriptor pqu;
		struct ioat_raw_descriptor raw;
	} hw[IOAT_DESCS_PER_PAGE];
};

static inline void *to_hw(struct ioat2_ring_ent *sw)
{
	void *ptr = sw;

	BUILD_BUG_ON(sizeof(struct ioat2_ring_ent) !=
		     sizeof(struct ioat_dma_descriptor));

	return ptr + offsetof(struct ioat2_ring_page, hw);
}

struct ioat2_ring_dir {
	struct ioat2_ring_page *page[512];
};

static inline void
ioat2_inc_head(struct ioat2_dma_chan *ioat, int num)
{
	ioat->head = (ioat->head + num) & (ioat2_ring_size(ioat) - 1);
}

static inline void
ioat2_set_head(struct ioat2_dma_chan *ioat, int head)
{
	ioat->head = head & (ioat2_ring_size(ioat) - 1);
}

static inline void
ioat2_inc_tail(struct ioat2_dma_chan *ioat, int num)
{
	ioat->tail = (ioat->tail + num) & (ioat2_ring_size(ioat) - 1);
}

static inline void
ioat2_set_tail(struct ioat2_dma_chan *ioat, int tail)
{
	ioat->tail = tail & (ioat2_ring_size(ioat) - 1);
}

static inline void
ioat2_set_issued(struct ioat2_dma_chan *ioat, int issued)
{
	ioat->issued = issued & (ioat2_ring_size(ioat) - 1);
}

static inline u16
ioat2_dir_index(u16 idx)
{
	return (idx >> IOAT_MAX_ORDER_PER_DIR);
}

static inline u16
ioat2_page_index(u16 idx)
{
	return ((idx >> IOAT_DIR_PAGE_SHIFT) & (IOAT_PAGES_PER_DIR - 1));
}

static inline struct ioat2_ring_ent *
ioat2_get_ring_ent(struct ioat2_dma_chan *ioat, u16 idx)
{
	u16 i = idx & (ioat2_ring_size(ioat) - 1);
	struct ioat2_ring_dir *dir = ioat->dir[ioat2_dir_index(i)];
	struct ioat2_ring_page *page =
		dir->page[ioat2_page_index(i)];

	/* 512 pages per directory, 64 descriptors per page */
	return &page->sw[i & (IOAT_DESCS_PER_PAGE - 1)];
}

static inline void ioat2_set_chainaddr(struct ioat2_dma_chan *ioat, u64 addr)
{
	struct ioat_chan_common *chan = &ioat->base;

	writel(addr & 0x00000000FFFFFFFF,
	       chan->reg_base + IOAT2_CHAINADDR_OFFSET_LOW);
	writel(addr >> 32,
	       chan->reg_base + IOAT2_CHAINADDR_OFFSET_HIGH);

	ioat_check_armed(chan);
}

int __devinit ioat2_dma_probe(struct ioatdma_device *dev, int dca);
int __devinit ioat3_dma_probe(struct ioatdma_device *dev, int dca);
struct dca_provider * __devinit ioat2_dca_init(struct pci_dev *pdev, void __iomem *iobase);
struct dca_provider * __devinit ioat3_dca_init(struct pci_dev *pdev, void __iomem *iobase);
int ioat2_check_space_lock(struct ioat2_dma_chan *ioat, int num_descs);
int ioat2_enumerate_channels(struct ioatdma_device *device);
struct dma_async_tx_descriptor *
ioat2_dma_prep_memcpy_lock(struct dma_chan *c, dma_addr_t dma_dest,
			   dma_addr_t dma_src, size_t len, unsigned long flags);
void ioat2_issue_pending(struct dma_chan *chan);
int ioat2_alloc_chan_resources(struct dma_chan *c);
void ioat2_free_chan_resources(struct dma_chan *c);
void __ioat2_restart_chan(struct ioat2_dma_chan *ioat);
bool reshape_ring(struct ioat2_dma_chan *ioat, int order);
void __ioat2_issue_pending(struct ioat2_dma_chan *ioat);
void ioat2_cleanup_event(unsigned long data);
void ioat2_timer_event(unsigned long data);
int ioat2_quiesce(struct ioat_chan_common *chan, unsigned long tmo);
int ioat2_reset_sync(struct ioat_chan_common *chan, unsigned long tmo);
extern struct kobj_type ioat2_ktype;
extern struct kmem_cache *ioat2_cache;
#endif /* IOATDMA_V2_H */
