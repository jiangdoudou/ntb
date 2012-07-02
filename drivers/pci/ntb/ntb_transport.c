/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 *   redistributing this file, you may do so under either license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright(c) 2012 Intel Corporation. All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *   The full GNU General Public License is included in this distribution
 *   in the file called LICENSE.GPL.
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2012 Intel Corporation. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copy
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Intel PCIe NTB Linux driver
 *
 * Contact Information:
 * Jon Mason <jon.mason@intel.com>
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "ntb_hw.h"
#include "ntb_transport.h"

static int transport_mtu = 0x4014;
module_param(transport_mtu, uint, 0644);
MODULE_PARM_DESC(transport_mtu, "Maximum size of NTB transport packets");

struct ntb_queue_entry {
	/* ntb_queue list reference */
	struct list_head entry;
	/* pointers to data to be transfered */
	void *callback_data;
	void *buf;
	unsigned int len;
	unsigned int flags;
};

struct ntb_transport_qp {
	struct ntb_device *ndev;

	bool client_ready;
	bool qp_link;
	u8 qp_num;	/* Only 64 QP's are allowed.  0-63 */

	void (*tx_handler)(struct ntb_transport_qp *qp);
#if 1
	struct task_struct *tx_work;
	unsigned int tx_ring_timeo;
	struct dentry *debugfs_tx_to;
#else
	struct tasklet_struct tx_work;
#endif
	struct list_head txq;
	struct list_head txc;
	struct list_head txe;
	spinlock_t txq_lock;
	spinlock_t txc_lock;
	spinlock_t txe_lock;
	void *tx_mw_begin;
	void *tx_mw_end;
	void *tx_offset;

	void (*rx_handler)(struct ntb_transport_qp *qp);
	struct tasklet_struct rx_work;
	struct list_head rxq;
	struct list_head rxc;
	struct list_head rxe;
	spinlock_t rxq_lock;
	spinlock_t rxc_lock;
	spinlock_t rxe_lock;
	void *rx_buff_begin;
	void *rx_buff_end;
	void *rx_offset;

	void (*event_handler)(int status);
	struct delayed_work link_work;

	struct dentry *debugfs_dir;
	struct dentry *debugfs_stats;

	struct dentry *debugfs_rx_hdr_dump;
	struct dentry *debugfs_tx_hdr_dump;

	u32 rx_hdr_dump;
	u32 tx_hdr_dump;

	/* Stats */
	u64 rx_bytes;
	u64 rx_pkts;
	u64 rx_ring_empty;
	u64 rx_err_no_buf;
	u64 rx_err_oflow;
	u64 rx_err_ver;
	u64 tx_bytes;
	u64 tx_pkts;
	u64 tx_ring_full;
};

struct ntb_transport_mw {
	size_t size;
	void *virt_addr;
	dma_addr_t dma_addr;
};

struct ntb_transport {
	struct ntb_device *ndev;
	struct ntb_transport_mw mw[NTB_NUM_MW];
	struct ntb_transport_qp *qps;
	unsigned int max_qps;
	unsigned long qp_bitmap;
	bool transport_link;
	struct delayed_work link_work;
	struct dentry *debugfs_dir;
};

enum {
	DESC_DONE_FLAG = 1 << 0,
	LINK_DOWN_FLAG = 1 << 1,
};

struct ntb_payload_header {
	u64 ver;
	unsigned int len;
	unsigned int flags;
};

enum {
	MW0_SZ = 0,
	MW1_SZ,
	NUM_QPS,
	QP_LINKS,
};

#define QP_TO_MW(qp)		((qp) % NTB_NUM_MW)
#if 1
#define	NTB_QP_DEF_RING_TIMEOUT	100
#endif
#define NTB_QP_DEF_NUM_ENTRIES	1000

struct ntb_transport *transport = NULL;

static int debugfs_open(struct inode *inode, struct file *filp)
{
	filp->private_data = inode->i_private;
	return 0;
}

static ssize_t debugfs_read(struct file *filp, char __user *ubuf,
	                    size_t count, loff_t *offp)
{
	struct ntb_transport_qp *qp;
	char buf[256];
	ssize_t ret, out_offset, out_count;

	out_count = 256;

	qp = filp->private_data;
	out_offset = 0;
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"NTB Transport stats\n");
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_bytes - %Ld\n", qp->rx_bytes);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_pkts - %Ld\n", qp->rx_pkts);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_ring_empty - %Ld\n", qp->rx_ring_empty);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_err_no_buf - %Ld\n", qp->rx_err_no_buf);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_er_oflow - %Ld\n", qp->rx_err_oflow);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_err_ver - %Ld\n", qp->rx_err_ver);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"rx_offset - %p\n", qp->rx_offset);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"tx_bytes - %Ld\n", qp->tx_bytes);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"tx_pkts - %Ld\n", qp->tx_pkts);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"tx_ring_full - %Ld\n", qp->tx_ring_full);
	out_offset += snprintf(buf + out_offset, out_count - out_offset,
				"tx_offset - %p\n", qp->tx_offset);

	ret = simple_read_from_buffer(ubuf, count, offp, buf, out_offset);
	return ret;
}

static const struct file_operations ntb_qp_debugfs_stats = {
	.owner = THIS_MODULE,
	.open  = debugfs_open,
	.read  = debugfs_read,
};

static void ntb_list_add_head(spinlock_t *lock, struct list_head *entry,
			      struct list_head *list)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	list_add(entry, list);
	spin_unlock_irqrestore(lock, flags);
}

static void ntb_list_add_tail(spinlock_t *lock, struct list_head *entry,
			      struct list_head *list)
{
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	list_add_tail(entry, list);
	spin_unlock_irqrestore(lock, flags);
}

static struct ntb_queue_entry *ntb_list_rm_head(spinlock_t *lock,
						struct list_head *list)
{
	struct ntb_queue_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(lock, flags);
	if (list_empty(list)) {
		entry = NULL;
		goto out;
	}
	entry = list_first_entry(list, struct ntb_queue_entry, entry);
	list_del(&entry->entry);
out:
	spin_unlock_irqrestore(lock, flags);

	return entry;
}

static int ntb_transport_setup_qp_mw(unsigned int qp_num)
{
	struct ntb_transport_qp *qp = &transport->qps[qp_num];
	u8 mw_num = QP_TO_MW(qp_num);
	unsigned int size, num_qps_mw;

	WARN_ON(transport->mw[mw_num].virt_addr == 0);

	if (transport->max_qps % NTB_NUM_MW && !mw_num)
		num_qps_mw = transport->max_qps / NTB_NUM_MW + (transport->max_qps % NTB_NUM_MW - mw_num);
	else
		num_qps_mw = transport->max_qps / NTB_NUM_MW;

	size = transport->mw[mw_num].size / num_qps_mw;
	pr_debug("orig size = %d, num qps = %d, size = %d\n", (int) transport->mw[mw_num].size, transport->max_qps, size);

	qp->rx_buff_begin = transport->mw[mw_num].virt_addr + (qp_num / NTB_NUM_MW * size);
	qp->rx_buff_end = qp->rx_buff_begin + size;
	pr_info("QP %d - RX Buff start %p end %p\n", qp->qp_num, qp->rx_buff_begin, qp->rx_buff_end);
	qp->rx_offset = qp->rx_buff_begin;

	qp->tx_mw_begin = ntb_get_mw_vbase(transport->ndev, mw_num) + (qp_num / NTB_NUM_MW * size);
	qp->tx_mw_end = qp->tx_mw_begin + size;
	pr_info("QP %d - TX MW start %p end %p\n", qp->qp_num, qp->tx_mw_begin, qp->tx_mw_end);
	qp->tx_offset = qp->tx_mw_begin;

	qp->rx_pkts = 0;
	qp->tx_pkts = 0;

	return 0;
}

static int ntb_set_mw(int num_mw, unsigned int size)
{
	struct ntb_transport_mw *mw = &transport->mw[num_mw];
	struct pci_dev *pdev = ntb_query_pdev(transport->ndev);
	void *offset;

	/* Alloc memory for receiving data.  Must be 4k aligned */
	mw->size = ALIGN(size, 4096);

	mw->virt_addr = dma_alloc_coherent(&pdev->dev, mw->size, &mw->dma_addr, GFP_KERNEL);
	if (!mw->virt_addr) {
		pr_err("Unable to allocate MW buffer of size %d\n", (int) mw->size);
		return -ENOMEM;
	}

	//setup the hdr offsets with 0's
	for (offset = mw->virt_addr; offset + sizeof(struct ntb_payload_header) < mw->virt_addr + size; offset += transport_mtu + sizeof(struct ntb_payload_header))
		memset(offset, 0, sizeof(struct ntb_payload_header));

	/* Notify HW the memory location of the receive buffer */
	ntb_set_mw_addr(transport->ndev, num_mw, mw->dma_addr);

	return 0;
}

static int ntb_hw_link_up(void)
{
	u32 val;
	int rc, i;

//FIXME -handle all of these error cases!

	//send the local info
	rc = ntb_write_remote_spad(transport->ndev, MW0_SZ, ntb_get_mw_size(transport->ndev, 0));
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", (u32) ntb_get_mw_size(transport->ndev, 0), MW0_SZ);

	rc = ntb_write_remote_spad(transport->ndev, MW1_SZ, ntb_get_mw_size(transport->ndev, 1));
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", (u32) ntb_get_mw_size(transport->ndev, 1), MW1_SZ);

	rc = ntb_write_remote_spad(transport->ndev, NUM_QPS, transport->max_qps);
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", transport->max_qps, NUM_QPS);

	rc = ntb_write_remote_spad(transport->ndev, QP_LINKS, 0);
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", 0, QP_LINKS);


	//get remote info
	rc = ntb_read_remote_spad(transport->ndev, NUM_QPS, &val);
	if (rc)
		pr_err("Error reading remote spad %d\n", NUM_QPS);

	pr_info("Remote max number of qps = %d\n", val);
	if (val != transport->max_qps)
		return -EINVAL;

	rc = ntb_read_remote_spad(transport->ndev, MW0_SZ, &val);
	if (rc)
		pr_err("Error reading remote spad %d\n", MW0_SZ);

	pr_info("Remote MW0 size = %d\n", val);
	if (!val)
		return -EINVAL;

	rc = ntb_set_mw(0, val);
	if (rc)
		return rc;

	rc = ntb_read_remote_spad(transport->ndev, MW1_SZ, &val);
	if (rc)
		pr_err("Error reading remote spad %d\n", MW1_SZ);

	pr_info("Remote MW1 size = %d\n", val);
	if (!val)
		return -EINVAL;

	rc = ntb_set_mw(1, val);
	if (rc)
		return rc;

	for (i = 0; i < transport->max_qps; i++) {
		struct ntb_transport_qp *qp = &transport->qps[i];

		rc = ntb_transport_setup_qp_mw(i);
		if (rc)
			return rc;

		if (qp->client_ready)
			schedule_delayed_work(&qp->link_work, 0);
	}

	transport->transport_link = NTB_LINK_UP;

	return 0;
}

static void ntb_transport_event_callback(void *data, unsigned int event)
{
	struct ntb_transport *nt = data;

	if (event == NTB_EVENT_HW_ERROR)
		BUG();

	if (event == NTB_EVENT_HW_LINK_UP)
		schedule_delayed_work(&nt->link_work, 0);

	if (event == NTB_EVENT_HW_LINK_DOWN) {
		int i;

		nt->transport_link = NTB_LINK_DOWN;

		/* Pass along the info to any clients */
		for (i = 0; i < nt->max_qps; i++)
			if (!test_bit(i, &nt->qp_bitmap)) {
				struct ntb_transport_qp *qp = &nt->qps[i];

				if (qp->event_handler && qp->qp_link != NTB_LINK_DOWN)
					qp->event_handler(NTB_LINK_DOWN);

				qp->qp_link = NTB_LINK_DOWN;
			}
	}
}

static void ntb_transport_link_work(struct work_struct *work)
{
	int rc = ntb_hw_link_up(); //why not have this code in this func?
	if (rc && ntb_hw_link_status(transport->ndev))
		schedule_delayed_work(&transport->link_work, msecs_to_jiffies(1000));
}

static void ntb_qp_link_work(struct work_struct *work)
{
	struct ntb_transport_qp *qp = container_of(work, struct ntb_transport_qp,
						   link_work.work);
	int rc, val;

	WARN_ON(!qp->rx_buff_begin);
	WARN_ON(!qp->tx_offset);

	rc = ntb_read_local_spad(transport->ndev, QP_LINKS, &val);
	if (rc) {
		pr_err("Error reading spad %d\n", QP_LINKS);
		return;
	}

	rc = ntb_write_remote_spad(transport->ndev, QP_LINKS, val | 1 << qp->qp_num);
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", val | 1 << qp->qp_num, QP_LINKS);

	//query remote spad for qp ready bit
	rc = ntb_read_remote_spad(transport->ndev, QP_LINKS, &val);
	if (rc)
		pr_err("Error reading remote spad %d\n", QP_LINKS);

	pr_debug("Remote QP link status = %x\n", val);

	/* See if the remote side is up */
	if (1 << qp->qp_num & val) {
		qp->qp_link = NTB_LINK_UP;

		if (qp->event_handler)
			qp->event_handler(NTB_LINK_UP);
	} else if (ntb_hw_link_status(transport->ndev))
		schedule_delayed_work(&qp->link_work, msecs_to_jiffies(1000));
}

static void ntb_transport_init_queue(unsigned int qp_num)
{
	struct ntb_transport_qp *qp;

	qp = &transport->qps[qp_num];
	qp->qp_num = qp_num;
	qp->ndev = transport->ndev;
	qp->qp_link = NTB_LINK_DOWN;

	qp->rx_hdr_dump = 0;
	qp->tx_hdr_dump = 0;
#if 1
	qp->tx_ring_timeo = NTB_QP_DEF_RING_TIMEOUT;
#endif

	if (transport->debugfs_dir) {
		char debugfs_name[4];

		snprintf(debugfs_name, 4, "qp%d", qp_num);
		qp->debugfs_dir = debugfs_create_dir(debugfs_name, transport->debugfs_dir);

		qp->debugfs_stats = debugfs_create_file("stats", S_IRUSR | S_IRGRP | S_IROTH, qp->debugfs_dir, qp, &ntb_qp_debugfs_stats);
		qp->debugfs_rx_hdr_dump = debugfs_create_bool("rx_hdr_dump", S_IRUSR | S_IWUSR, qp->debugfs_dir, &qp->rx_hdr_dump);
		qp->debugfs_tx_hdr_dump = debugfs_create_bool("tx_hdr_dump", S_IRUSR | S_IWUSR, qp->debugfs_dir, &qp->tx_hdr_dump);
#if 1
		qp->debugfs_tx_to = debugfs_create_u32("tx_ring_timeo", S_IRUSR | S_IWUSR, qp->debugfs_dir, &qp->tx_ring_timeo);
#endif
	}

	INIT_DELAYED_WORK(&qp->link_work, ntb_qp_link_work);

	spin_lock_init(&qp->rxc_lock);
	spin_lock_init(&qp->rxq_lock);
	spin_lock_init(&qp->rxe_lock);
	spin_lock_init(&qp->txc_lock);
	spin_lock_init(&qp->txq_lock);
	spin_lock_init(&qp->txe_lock);

	INIT_LIST_HEAD(&qp->rxq);
	INIT_LIST_HEAD(&qp->rxc);
	INIT_LIST_HEAD(&qp->rxe);
	INIT_LIST_HEAD(&qp->txq);
	INIT_LIST_HEAD(&qp->txc);
	INIT_LIST_HEAD(&qp->txe);
}

static int ntb_transport_init(void)
{
	int rc, i;

	transport = kzalloc(sizeof(struct ntb_transport), GFP_KERNEL);
	if (!transport)
		return -ENOMEM;

	if (debugfs_initialized())
		transport->debugfs_dir = debugfs_create_dir(KBUILD_MODNAME, NULL);
	else
		transport->debugfs_dir = NULL;

	transport->ndev = ntb_register_transport(transport);
	if (!transport->ndev) {
		rc = -EIO;
		goto err;
	}

	transport->max_qps = ntb_query_max_cbs(transport->ndev);
	if (!transport->max_qps) {
		rc = -EIO;
		goto err1;
	}

	transport->qps = kcalloc(transport->max_qps,
				 sizeof(struct ntb_transport_qp),
				 GFP_KERNEL);
	if (!transport->qps) {
		rc = -ENOMEM;
		goto err1;
	}

	transport->qp_bitmap = ((u64) 1 << transport->max_qps) - 1;

	for (i = 0; i < transport->max_qps; i++)
		ntb_transport_init_queue(i);

	rc = ntb_register_event_callback(transport->ndev,
					 ntb_transport_event_callback);
	if (rc)
		goto err2;

	INIT_DELAYED_WORK(&transport->link_work, ntb_transport_link_work);
	rc = ntb_hw_link_up(); //why not have this code in this func?
	if (rc && ntb_hw_link_status(transport->ndev))
		schedule_delayed_work(&transport->link_work, msecs_to_jiffies(1000));

	return 0;

err2:
	kfree(transport->qps);
err1:
	ntb_unregister_transport(transport->ndev);
err:
	kfree(transport);
	return rc;
}

static void ntb_transport_free(void)
{
	struct pci_dev *pdev;
	int i;

	if (!transport)
		return;

	transport->transport_link = NTB_LINK_DOWN;

	cancel_delayed_work_sync(&transport->link_work);

	debugfs_remove_recursive(transport->debugfs_dir);

	/* To be here, all of the queues were already free'd.  No need to try and clean them up */

	ntb_unregister_event_callback(transport->ndev);

	pdev = ntb_query_pdev(transport->ndev);

	for (i = 0; i < NTB_NUM_MW; i++)
		if (transport->mw[i].virt_addr)
			dma_free_coherent(&pdev->dev,
					  transport->mw[i].size,
					  transport->mw[i].virt_addr,
					  transport->mw[i].dma_addr);

	kfree(transport->qps);
	ntb_unregister_transport(transport->ndev);
	kfree(transport);
	transport = NULL;
}

static void ntb_rx_copy_task(struct ntb_transport_qp *qp, struct ntb_queue_entry *entry, void *offset)
{
	volatile struct ntb_payload_header *hdr;
	hdr = offset;

	entry->len = hdr->len;
	offset += sizeof(struct ntb_payload_header);
	memcpy(entry->buf, offset, entry->len);
	//print_hex_dump_bytes(" ", 0, entry->buf, entry->len);

	wmb();
	hdr->flags = 0;
	ntb_list_add_tail(&qp->rxc_lock, &entry->entry, &qp->rxc);

	if (qp->rx_handler && qp->client_ready)
		qp->rx_handler(qp);
}

static int ntb_process_rxc(struct ntb_transport_qp *qp)
{
	volatile struct ntb_payload_header *hdr;
	struct ntb_queue_entry *entry;
	void *offset;

	entry = ntb_list_rm_head(&qp->rxq_lock, &qp->rxq);
	if (!entry) {
		hdr = qp->rx_offset;
		pr_info("no buffer - HDR ver %Ld, len %d, flags %x\n", hdr->ver, hdr->len, hdr->flags);
		qp->rx_err_no_buf++;
		return -ENOMEM;
	}

	offset = qp->rx_offset;
	hdr = offset;

	if (qp->rx_hdr_dump)
		pr_info("HDR ver %Ld, len %d, flags %x\n", hdr->ver, hdr->len, hdr->flags);

	//FIXME - do something with the other flags!
	if (!(hdr->flags & DESC_DONE_FLAG)) {
		ntb_list_add_tail(&qp->rxq_lock, &entry->entry, &qp->rxq);
		qp->rx_ring_empty++;
		return -EAGAIN;
	}

	if (hdr->flags & NTB_LINK_DOWN) {
		//FIXME - we should probably do this somewhere else
		pr_info("qp %d: Link Down\n", qp->qp_num);
		qp->qp_link = NTB_LINK_DOWN;
		schedule_delayed_work(&qp->link_work, msecs_to_jiffies(1000));

		if (qp->event_handler)
			qp->event_handler(NTB_LINK_DOWN);

		ntb_list_add_tail(&qp->rxq_lock, &entry->entry, &qp->rxq);
		hdr->flags = 0;
		goto out;
	}

	if (hdr->ver != qp->rx_pkts) {
		pr_debug("qp %d: version mismatch, expected %Ld - got %Ld\n",
		         qp->qp_num, qp->rx_pkts, hdr->ver);
		ntb_list_add_tail(&qp->rxq_lock, &entry->entry, &qp->rxq);
		qp->rx_err_ver++;
		return -EIO;
	}

	pr_debug("rx offset %p, ver %Ld - %d payload received, "
		 "buf size %d\n", qp->rx_offset, hdr->ver, hdr->len,
		 entry->len);

	if (hdr->len <= entry->len)
		ntb_rx_copy_task(qp, entry, offset);
	else {
		ntb_list_add_tail(&qp->rxq_lock, &entry->entry, &qp->rxq);
		hdr->flags = 0;
		qp->rx_err_oflow++;
		pr_err("RX overflow! Wanted %d got %d\n", hdr->len, entry->len);
	}

	qp->rx_bytes += hdr->len;
	qp->rx_pkts++;

out:
	qp->rx_offset = (qp->rx_offset + ((transport_mtu + sizeof(struct ntb_payload_header)) * 2) >= qp->rx_buff_end) ? qp->rx_buff_begin : qp->rx_offset + transport_mtu + sizeof(struct ntb_payload_header);

	return 0;
}

static void ntb_transport_rx(unsigned long data)
{
	struct ntb_transport_qp *qp = (struct ntb_transport_qp *)data;
	int rc;

	do {
		rc = ntb_process_rxc(qp);
	} while (!rc);
}

static void ntb_transport_rxc_db(int db_num)
{
	struct ntb_transport_qp *qp = &transport->qps[db_num];

	pr_debug("%s: doorbell %d received\n", __func__, db_num);

	tasklet_schedule(&qp->rx_work);
}

static void ntb_tx_copy_task(struct ntb_transport_qp *qp, struct ntb_queue_entry *entry, volatile void *offset)
{
	volatile struct ntb_payload_header *hdr = offset;
	int rc;

	offset += sizeof(struct ntb_payload_header);
	//print_hex_dump_bytes(" ", 0, entry->buf, entry->len);
	memcpy_toio(offset, entry->buf, entry->len);

	hdr->len = entry->len;
	hdr->ver = qp->tx_pkts;
	wmb();
	hdr->flags = entry->flags | DESC_DONE_FLAG;

	rc = ntb_ring_sdb(qp->ndev, qp->qp_num);
	if (rc)
		pr_err("%s: error ringing db %d\n", __func__, qp->qp_num);

	if (entry->len > 0) {
		qp->tx_bytes += entry->len;

		/* Add fully transmitted data to completion queue */
		ntb_list_add_tail(&qp->txc_lock, &entry->entry, &qp->txc);

		if (qp->tx_handler)
			qp->tx_handler(qp);
	} else
		ntb_list_add_tail(&qp->txe_lock, &entry->entry, &qp->txe);
}

static int ntb_process_tx(struct ntb_transport_qp *qp,
			  struct ntb_queue_entry *entry)
{
	volatile struct ntb_payload_header *hdr;
	volatile void *offset;

	offset = qp->tx_offset;
	hdr = offset;

	if (qp->tx_hdr_dump)
		pr_info("HDR ver %Ld, len %d, flags %x\n", hdr->ver, hdr->len, hdr->flags);

	pr_debug("%lld - offset %p, tx %p, entry len %d flags %x buff %p\n", qp->tx_pkts, offset, qp->tx_offset, entry->len, entry->flags, entry->buf);
	if (hdr->flags) {
		ntb_list_add_head(&qp->txq_lock, &entry->entry, &qp->txq);
		qp->tx_ring_full++;
		return -EAGAIN;
	}

	if (entry->len > transport_mtu) {
		//FIXME - tossing on the floor, should return pkt with error
		ntb_list_add_tail(&qp->txc_lock, &entry->entry, &qp->txc);
		pr_err("Trying to send pkt size of %d\n", entry->len);
		return 0;
	}

	ntb_tx_copy_task(qp, entry, offset);

	qp->tx_offset = (qp->tx_offset + ((transport_mtu + sizeof(struct ntb_payload_header)) * 2) >= qp->tx_mw_end) ? qp->tx_mw_begin : qp->tx_offset + transport_mtu + sizeof(struct ntb_payload_header);

	qp->tx_pkts++;

	return 0;
}

#if 1
static int ntb_transport_tx(void *data)
{
	struct ntb_transport_qp *qp = data;
	struct ntb_queue_entry *entry;
	int rc;

	while (!kthread_should_stop()) {
		entry = ntb_list_rm_head(&qp->txq_lock, &qp->txq);
		if (!entry) {
			/* Sleep if no tx work */
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			set_current_state(TASK_RUNNING);
			continue;
		}

		rc = ntb_process_tx(qp, entry);
		if (!rc)
			continue;

		schedule_timeout_interruptible(msecs_to_jiffies(qp->tx_ring_timeo));
	}

	return 0;
}
#else
static void ntb_transport_tx(unsigned long data)
{
	struct ntb_transport_qp *qp = (struct ntb_transport_qp *)data;
	struct ntb_queue_entry *entry;
	int rc;

	do {
		entry = ntb_list_rm_head(&qp->txq_lock, &qp->txq);
		if (!entry)
			break;

		rc = ntb_process_tx(qp, entry);
	} while (!rc);
}
#endif

static void ntb_send_link_down(struct ntb_transport_qp *qp)
{
	struct ntb_queue_entry *entry;
	int i;

	if (qp->qp_link == NTB_LINK_DOWN)
		return;

	qp->qp_link = NTB_LINK_DOWN;

	//FIXME - should give this an upper bound...
	for (i = 0; i < 1000 && !(entry = ntb_list_rm_head(&qp->txe_lock, &qp->txe)); i++)
		msleep(1);

	entry->callback_data = NULL;
	entry->buf = NULL;
	entry->len = 0;
	entry->flags = LINK_DOWN_FLAG;

	ntb_list_add_tail(&qp->txq_lock, &entry->entry, &qp->txq);
#if 1
	wake_up_process(qp->tx_work);
#else
	tasklet_schedule(&qp->tx_work);
#endif
}

/**
 * ntb_transport_create_queue - Create a new NTB transport layer queue
 * @rx_handler: receive callback function 
 * @tx_handler: transmit callback function 
 * @event_handler: event callback function 
 *
 * Create a new NTB transport layer queue and provide the queue with a callback
 * routine for both transmit and receive.  The receive callback routine will be
 * used to pass up data when the transport has received it on the queue.   The
 * transmit callback routine will be called when the transport has completed the
 * transmission of the data on the queue and the data is ready to be freed.
 *
 * RETURNS: pointer to newly created ntb_queue, NULL on error.
 */
//FIXME - this might only be permissible in non-interrupt context
struct ntb_transport_qp *ntb_transport_create_queue(handler rx_handler,
						    handler tx_handler,
						    ehandler event_handler)
{
	struct ntb_queue_entry *entry;
	struct ntb_transport_qp *qp;
	unsigned int free_queue;
	int rc, i;

	if (!transport) {
		rc = ntb_transport_init();
		if (rc)
			return NULL;
	}

	//FIXME - need to handhake with remote side to determine matching number or some mapping between the 2
	free_queue = ffs(transport->qp_bitmap);
	if (!free_queue)
		goto err;

	/* decrement free_queue to make it zero based */
	free_queue--;

	clear_bit(free_queue, &transport->qp_bitmap);

	qp = &transport->qps[free_queue];
	qp->rx_handler = rx_handler;
	qp->tx_handler = tx_handler;
	qp->event_handler = event_handler;

	for (i = 0; i < NTB_QP_DEF_NUM_ENTRIES; i++) {
		entry = kzalloc(sizeof(struct ntb_queue_entry), GFP_ATOMIC);
		if (!entry)
			goto err1;

		ntb_list_add_tail(&qp->rxe_lock, &entry->entry, &qp->rxe);
	}

	for (i = 0; i < NTB_QP_DEF_NUM_ENTRIES; i++) {
		entry = kzalloc(sizeof(struct ntb_queue_entry), GFP_ATOMIC);
		if (!entry)
			goto err2;

		ntb_list_add_tail(&qp->txe_lock, &entry->entry, &qp->txe);
	}

	tasklet_init(&qp->rx_work, ntb_transport_rx, (unsigned long) qp);

#if 1
	qp->tx_work = kthread_create(ntb_transport_tx, qp, "ntb_tx%d", free_queue);
	if (IS_ERR(qp->tx_work)) {
		rc = PTR_ERR(qp->tx_work);
		pr_err("Error allocing tx kthread\n");
		goto err2;
	}
#else
	tasklet_init(&qp->tx_work, ntb_transport_tx, (unsigned long) qp);
#endif

	//FIXME - transport->qps[free_queue].queue.hw_caps; is it even necessary for transport client to know?

	rc = ntb_register_db_callback(qp->ndev, free_queue,
				      ntb_transport_rxc_db);
	if (rc)
		goto err3;

	pr_info("NTB Transport QP %d created\n", qp->qp_num);

	return qp;

err3:
	kthread_stop(qp->tx_work);
err2:
	while ((entry = ntb_list_rm_head(&qp->txe_lock, &qp->txe)))
		kfree(entry);
err1:
	while ((entry = ntb_list_rm_head(&qp->rxe_lock, &qp->rxe)))
		kfree(entry);
	set_bit(free_queue, &transport->qp_bitmap);
err:
	return NULL;
}
EXPORT_SYMBOL(ntb_transport_create_queue);

/**
 * ntb_transport_free_queue - Frees NTB transport queue
 * @qp: NTB queue to be freed
 *
 * Frees NTB transport queue 
 */
void ntb_transport_free_queue(struct ntb_transport_qp *qp)
{
	struct ntb_queue_entry *entry;

	if (!qp)
		return;

	cancel_delayed_work_sync(&qp->link_work);

	ntb_unregister_db_callback(qp->ndev, qp->qp_num);
	tasklet_disable(&qp->rx_work);

#if 1
	kthread_stop(qp->tx_work);
#else
	tasklet_disable(&qp->tx_work);
#endif

	while ((entry = ntb_list_rm_head(&qp->rxe_lock, &qp->rxe)))
		kfree(entry);

	while ((entry = ntb_list_rm_head(&qp->rxq_lock, &qp->rxq))) {
		pr_warn("Freeing item from a non-empty queue\n");
		kfree(entry);
	}

	while ((entry = ntb_list_rm_head(&qp->rxc_lock, &qp->rxc))) {
		pr_warn("Freeing item from a non-empty queue\n");
		kfree(entry);
	}

	while ((entry = ntb_list_rm_head(&qp->txe_lock, &qp->txe)))
		kfree(entry);

	while ((entry = ntb_list_rm_head(&qp->txq_lock, &qp->txq))) {
		pr_warn("Freeing item from a non-empty queue\n");
		kfree(entry);
	}

	while ((entry = ntb_list_rm_head(&qp->txc_lock, &qp->txc))) {
		pr_warn("Freeing item from a non-empty queue\n");
		kfree(entry);
	}

	set_bit(qp->qp_num, &transport->qp_bitmap);

	if (transport->qp_bitmap == ((u64) 1 << transport->max_qps) - 1)
		ntb_transport_free();
}
EXPORT_SYMBOL(ntb_transport_free_queue);

/**
 * ntb_transport_rx_remove - Dequeues enqueued rx packet
 * @qp: NTB queue to be freed
 * @len: pointer to variable to write enqueued buffers length
 *
 * Dequeues unused buffers from receive queue.  Should only be used during
 * shutdown of qp.
 *
 * RETURNS: NULL error value on error, or void* for success.
 */
void *ntb_transport_rx_remove(struct ntb_transport_qp *qp, unsigned int *len)
{
	struct ntb_queue_entry *entry;
	void *buf;

	if (!qp || qp->client_ready == NTB_LINK_UP)
		return NULL;

	entry = ntb_list_rm_head(&qp->rxq_lock, &qp->rxq);
	if (!entry)
		return NULL;

	buf = entry->callback_data;
	*len = entry->len;
	kfree(entry);

	return buf;
}
EXPORT_SYMBOL(ntb_transport_rx_remove);

/**
 * ntb_transport_rx_enqueue - Enqueue a new NTB queue entry
 * @qp: NTB transport layer queue the entry is to be enqueued on
 * @cb: per buffer pointer for callback function to use
 * @data: pointer to data buffer that incoming packets will be copied into
 * @len: length of the data buffer
 *
 * Enqueue a new receive buffer onto the transport queue into which a NTB
 * payload can be received into.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int ntb_transport_rx_enqueue(struct ntb_transport_qp *qp, void *cb, void *data, unsigned int len)
{
	struct ntb_queue_entry *entry;

	if (!qp)
		return -EINVAL;

	entry = ntb_list_rm_head(&qp->rxe_lock, &qp->rxe);
	if (!entry)
		return -ENOMEM;

	entry->callback_data = cb;
	entry->buf = data;
	entry->len = len;

	ntb_list_add_tail(&qp->rxq_lock, &entry->entry, &qp->rxq);

	return 0;
}
EXPORT_SYMBOL(ntb_transport_rx_enqueue);

/**
 * ntb_transport_tx_enqueue - Enqueue a new NTB queue entry
 * @qp: NTB transport layer queue the entry is to be enqueued on
 * @cb: per buffer pointer for callback function to use
 * @data: pointer to data buffer that will be sent
 * @len: length of the data buffer
 *
 * Enqueue a new transmit buffer onto the transport queue from which a NTB
 * payload will be transmitted.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int ntb_transport_tx_enqueue(struct ntb_transport_qp *qp, void *cb, void *data, unsigned int len)
{
	struct ntb_queue_entry *entry;

	if (!qp || qp->qp_link != NTB_LINK_UP)
		return -EINVAL;

	entry = ntb_list_rm_head(&qp->txe_lock, &qp->txe);
	if (!entry) {
#if 0
		//ring full, kick it
		tasklet_schedule(&qp->tx_work);
#endif
		return -ENOMEM;
	}

	entry->callback_data = cb;
	entry->buf = data;
	entry->len = len;
	entry->flags = 0;

	ntb_list_add_tail(&qp->txq_lock, &entry->entry, &qp->txq);

#if 1
	wake_up_process(qp->tx_work);
#else
	tasklet_schedule(&qp->tx_work);
#endif

	return 0;
}
EXPORT_SYMBOL(ntb_transport_tx_enqueue);

/**
 * ntb_transport_tx_dequeue - Dequeue a NTB queue entry
 * @qp: NTB transport layer queue to be dequeued from
 * @len: length of the data buffer
 * 
 * This function will dequeue a buffer from the transmit complete queue.
 * Entries will only be enqueued on this queue after having been
 * transfered to the remote side.
 *
 * RETURNS: callback pointer of the buffer from the transport queue, or NULL
 * on empty
 */
void *ntb_transport_tx_dequeue(struct ntb_transport_qp *qp, unsigned int *len)
{
	struct ntb_queue_entry *entry;
	void *buf;

	if (!qp)
		return NULL;

	entry = ntb_list_rm_head(&qp->txc_lock, &qp->txc);
	if (!entry)
		return NULL;

	buf = entry->callback_data;
	*len = entry->len;

	//Sanity check for now
	entry->callback_data = NULL;
	entry->buf = NULL;
	entry->len = 0;

	ntb_list_add_tail(&qp->txe_lock, &entry->entry, &qp->txe);

	return buf;
}
EXPORT_SYMBOL(ntb_transport_tx_dequeue);

/**
 * ntb_transport_rx_dequeue - Dequeue a NTB queue entry
 * @qp: NTB transport layer queue to be dequeued from
 * @len: length of the data buffer
 * 
 * This function will dequeue a buffer from the receive complete queue.
 * Entries will only be enqueued on this queue after having been fully received.
 *
 * RETURNS: callback pointer of the buffer from the transport queue, or NULL
 * on empty
 */
void *ntb_transport_rx_dequeue(struct ntb_transport_qp *qp, unsigned int *len)
{
	struct ntb_queue_entry *entry;
	void *buf;

	if (!qp)
		return NULL;

	entry = ntb_list_rm_head(&qp->rxc_lock, &qp->rxc);
	if (!entry)
		return NULL;

	buf = entry->callback_data;
	*len = entry->len;

	//Sanity check for now
	entry->callback_data = NULL;
	entry->buf = NULL;
	entry->len = 0;

	ntb_list_add_tail(&qp->rxe_lock, &entry->entry, &qp->rxe);

	return buf;
}
EXPORT_SYMBOL(ntb_transport_rx_dequeue);

/**
 * ntb_transport_link_up - Notify NTB transport of client readiness to use queue
 * @qp: NTB transport layer queue to be enabled
 *
 * Notify NTB transport layer of client readiness to use queue
 */
void ntb_transport_link_up(struct ntb_transport_qp *qp)
{
	if (!qp)
		return;

	qp->client_ready = NTB_LINK_UP;

	if (transport->transport_link == NTB_LINK_UP)
		schedule_delayed_work(&qp->link_work, 0);
}
EXPORT_SYMBOL(ntb_transport_link_up);

/**
 * ntb_transport_link_down - Notify NTB transport to no longer enqueue data
 * @qp: NTB transport layer queue to be disabled
 *
 * Notify NTB transport layer of client's desire to no longer receive data on
 * transport queue specified.  It is the client's responsibility to ensure all
 * entries on queue are purged or otherwise handled appropraitely.
 */
void ntb_transport_link_down(struct ntb_transport_qp *qp)
{
	int rc, val;

	if (!qp)
		return;

	qp->client_ready = NTB_LINK_DOWN;

	cancel_delayed_work_sync(&qp->link_work);
	qp->qp_link = NTB_LINK_DOWN;

	rc = ntb_read_local_spad(transport->ndev, QP_LINKS, &val);
	if (rc) {
		pr_err("Error reading spad %d\n", QP_LINKS);
		return;
	}

	rc = ntb_write_remote_spad(transport->ndev, QP_LINKS, val & ~(1 << qp->qp_num));
	if (rc)
		pr_err("Error writing %x to remote spad %d\n", val & ~(1 << qp->qp_num), QP_LINKS);

	if (transport->transport_link == NTB_LINK_UP)//FIXME - only need to send if already connected
		ntb_send_link_down(qp);
}
EXPORT_SYMBOL(ntb_transport_link_down);

/**
 * ntb_transport_link_query - Query transport link state
 * @qp: NTB transport layer queue to be queried
 *
 * Query connectivity to the remote system of the NTB transport queue 
 *
 * RETURNS: true for link up or false for link down
 */
bool ntb_transport_link_query(struct ntb_transport_qp *qp)
{
	return qp->qp_link == NTB_LINK_UP;
}
EXPORT_SYMBOL(ntb_transport_link_query);

/**
 * ntb_transport_qp_num - Query the qp number
 * @qp: NTB transport layer queue to be queried
 *
 * Query qp number of the NTB transport queue
 *
 * RETURNS: a zero based number specifying the qp number
 */
unsigned char ntb_transport_qp_num(struct ntb_transport_qp *qp)
{
	return qp->qp_num;
}
EXPORT_SYMBOL(ntb_transport_qp_num);

/**
 * ntb_transport_max_size - Query the max payload size of a qp
 * @qp: NTB transport layer queue to be queried
 *
 * Query the maximum payload size permissible on the given qp
 *
 * RETURNS: the max payload size of a qp
 */
unsigned int ntb_transport_max_size(struct ntb_transport_qp *qp)
{
	return transport_mtu;
	//return (qp->tx_mw_end - qp->tx_mw_begin) - sizeof(struct ntb_payload_header);
}
EXPORT_SYMBOL(ntb_transport_max_size);
