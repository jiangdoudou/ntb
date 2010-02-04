/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2009 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * BSD LICENSE
 *
 * Copyright(c) 2009 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* This file implements network driver over NTB PCIE-LINK */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/inetdevice.h>
#include <net/neighbour.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/skbuff.h>
#include <linux/async_tx.h>
#include <linux/in6.h>
#include <asm/checksum.h>

#include "ntbethcq.h"
#include "ntbdev.h"
#include "ntbethcopier.h"
#include "ntbnet.h"

#define NTBETH_WATCHDOG_PERIOD (100*HZ);

// module parameter that tells if to use CB3 DMA hardware accelaration for memcpying packets from skb to remote NTB queue and from NTB queue to skb.

static int use_cb3_dma_engine = 0;
module_param(use_cb3_dma_engine,int, 0);

// module parameter that tells which doorbell to use to generate interrupt to the remote side after the local side transmitted a packet into to remote CQ. This is a way to exercise all the door bell bits (ofcourse mutually exclusive).

static int rx_int_doorbell_num = 0;
module_param(rx_int_doorbell_num,int, 0);

static int bar23_size = 0x100000;
module_param(bar23_size,int, 0);

static int bar45_size = 0x100000 ;
module_param(bar45_size,int, 0);

static int turnoff_tx = 0;
module_param(turnoff_tx,int, 0);

static int turnoff_pkt_count = 0;
module_param(turnoff_pkt_count,int, 0);

static int tx_pend_pkts = NTBETH_MAX_PEND_PKTS;
module_param(tx_pend_pkts,int, 0);

static int tdelay = 10;
module_param(tdelay,int, 0);

static int numntbs = 1;
module_param(numntbs,int, 0);

struct ntbeth_priv * ntbeth_device[NTBETH_MAX_NTB_DEVICES];

static const struct net_device_ops ntbeth_netdev_ops = {
.ndo_open               = ntbeth_open,
.ndo_stop               = ntbeth_close,
.ndo_start_xmit         = ntbeth_tx,
//.ndo_set_multicast_list = ntbeth_set_multicast_list,
.ndo_set_mac_address    = ntbeth_set_mac_address,
.ndo_change_mtu         = ntbeth_change_mtu,
.ndo_do_ioctl           = ntbeth_do_ioctl,
.ndo_tx_timeout         = ntbeth_tx_timeout,
.ndo_get_stats         =  ntbeth_stats,

};

void ntbeth_set_multicast_list(struct net_device *netdev)
{
	// because ntbeth is a point-to-point interface, no need to implement this.
	NTBETHDEBUG("Made it to ntbet_set_multicast_list\n");
	return ;
}

void ntbeth_tx_timeout(struct net_device *netdev)
{
	struct ntbeth_priv *priv = netdev_priv(netdev);
	spin_lock_bh(&priv->lock);
	priv->tx_timeout_count++;
	NTBETHDEBUG("NTBETH:ERROR: tx timed out%d\n",priv->tx_timeout_count);
	netdev->trans_start = jiffies;
	spin_unlock_bh(&priv->lock);
	return;
}

int ntbeth_open(struct net_device *dev)
{	
	struct ntbeth_priv * priv;
	priv = netdev_priv(dev);
	NTBETHDEBUG("Made it to ntbeth_open\n");
	memcpy(dev->dev_addr, NTBETH_MAC, ETH_ALEN);
	update_peer_status(dev, NTBETH_LOCAL_PEER_UP);
	ntbdev_send_ping_doorbell_interrupt(&priv->ntbdev);
	return 0;
}

int ntbeth_close(struct net_device *dev)
{
	struct ntbeth_priv * priv;
	NTBETHDEBUG("Made it to ntbeth_close\n");
	priv = netdev_priv(dev);
	update_peer_status(dev, NTBETH_LOCAL_PEER_DOWN);
	ntbdev_send_close_interrupt(&priv->ntbdev);
	return 0;
}

void rx_copy_callback(void *pref)
{
	int i,ret = 0;
	struct sk_buff *skb = (struct sk_buff *)pref;
	struct net_device *dev = (struct net_device *)skb->dev;
	struct ntbeth_priv *priv = netdev_priv(dev);
	NTBETHDEBUG("Made it to rx_copy_callback \n");

#ifndef USE_DBG_PKTS

	skb->protocol = eth_type_trans(skb, dev);
	priv->stats.rx_packets++;
	priv->stats.rx_bytes += skb->len;
	ret = netif_rx(skb);
	if (ret) {
		if(ret == NET_RX_DROP)
			printk("ntheth: RX PACKET DROPPED by Kernel\n");
		else
			printk("ntheth: RX PACKET has some problems in the Kernel\n");
	}
#else
	dev_kfree_skb(skb);
#endif
	if (use_cb3_dma_engine) {
		i = cq_get_index(priv->rxcq);
		dma_unmap_single(priv->copier.chan->device->dev, priv->rx_dma_addresses[i], skb->len + 14, DMA_BIDIRECTIONAL);
	}
	cq_update_get_ptr(&priv->rxcq);
	return;
}

void ntbeth_lnkchg_interrupt(void *pref)
{
	struct net_device *dev = (struct net_device *)pref;
	struct ntbeth_priv *priv = netdev_priv(dev);
	update_peer_status(dev, NTBETH_REMOTE_PEER_DOWN);
	ntbdev_send_ping_doorbell_interrupt(&priv->ntbdev);
	return;
}

void ntbeth_ping_ack_interrupt(void *pref)
{
	struct net_device *dev = (struct net_device *)pref;
	//struct ntbeth_priv *priv = netdev_priv(dev);
	NTBETHDEBUG("Made it to ping ack interrupt routine\n");
	update_peer_status(dev, NTBETH_REMOTE_PEER_UP);
	return;
}

void ntbeth_close_interrupt(void *pref)
{
	struct net_device *dev = (struct net_device *)pref;
	//struct ntbeth_priv *priv = netdev_priv(dev);
	update_peer_status(dev, NTBETH_REMOTE_PEER_DOWN);
	return;
}

void ntbeth_ping_interrupt(void *pref)
{
	struct net_device *dev = (struct net_device *)pref;
	struct ntbeth_priv *priv = netdev_priv(dev);
	NTBETHDEBUG("Made it to ping interrupt routine\n");
	update_peer_status(dev, NTBETH_REMOTE_PEER_UP);
	if ((priv->peer_status & NTBETH_LOCAL_MASK) == NTBETH_LOCAL_PEER_UP)
		ntbdev_send_ping_ack_doorbell_interrupt(&priv->ntbdev);
	return;
}

void ntbeth_txack_interrupt(void *pref)
{
	NTBETHDEBUG("Made it to txack interrupt routine\n");
	return;
}

void ntbeth_rx_interrupt(void * pref)
{
	struct net_device *dev = (struct net_device *)pref;
	struct ntbeth_priv *priv = netdev_priv(dev);
	struct sk_buff *skb;
	char *data;
	unsigned int len,avail_index;
	NTBETHDEBUG("ntbeth_rx_interrupt entered with devptr 0x%Lx\n",(unsigned long long)dev);
	NTBETHDEBUG("ntbeth_rx_interrupt entered with priv 0x%Lx\n",(unsigned long long)priv);
	#ifdef DB_INTER_LOCK
		priv->rxcq->rx_db_count++;
	#else
	#endif
	spin_lock_bh(&priv->lock);
	while (cq_is_buf_ready(priv->rxcq)) {
		avail_index = cq_avail_get_index(priv->rxcq);
		data = cq_get_current_get_entry_loc(priv->rxcq);
		if (data == 0) {
			printk(KERN_INFO "ntbeth: ntbeth_rx_interrupt received but rxq is empty\n");
			spin_unlock_bh(&priv->lock);
			return;
		}
		priv->rx_pkt_count++;
		len = *(unsigned int *)data;
		skb = dev_alloc_skb(len+2); // to make IP frame 16B aligned
		if (!skb) {
			if (printk_ratelimit())
				printk(KERN_INFO "ntbeth  rx: low on mem - packet dropped\n");
			priv->stats.rx_dropped++;
			spin_unlock_bh(&priv->lock);
			return;
		}
		skb->dev = dev;
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		skb_reserve(skb, 2);
		//printk(" %d ", priv->ntbdev.instance_id);
		//dump_memory(data+4+26 , 4, "s");
		//dump_memory(data+4+30 , 4, "d");
		ntbeth_copier_copy_to_skb(&priv->copier,data+4, len, skb,&priv->rx_dma_addresses[avail_index], rx_copy_callback, skb);
	}
	spin_unlock_bh(&priv->lock);
	return;
}

void ntbeth_perf_tmr_handler(unsigned long arg)
{
	struct ntbeth_priv *priv = (struct ntbeth_priv *)arg;
//	printk("Timer expired\n");
	if (priv->tx_pending_pkts) {
		priv->tx_pending_pkts = 0;
		ntbdev_send_packet_txed_interrupt(&priv->ntbdev);
	} 
}

/* we expect the callback completes in order of submission */
void tx_copy_callback(void *pref)
{
	struct sk_buff *skb = (struct sk_buff *)pref;
	struct ntbeth_priv *priv = netdev_priv(skb->dev);
	int i;
	NTBETHDEBUG("tx_copy_callback invoked\n");
	// we also need to send interrupt to the remote node
	if (use_cb3_dma_engine) {
		i= cq_put_index(&priv->txcq);
		//dump_memory((((char *)cq_get_buffer(priv->txcq,i)) + 4) , skb->len, "");
		dma_unmap_single(priv->copier.chan->device->dev, priv->tx_dma_addresses[i], skb->len, DMA_BIDIRECTIONAL);
	}
	cq_update_put_ptr(&priv->txcq);
	dev_kfree_skb(skb);
#ifdef DB_INTER_LOCK
	if(priv->txcq->tx_db_count == priv->txcq->rx_db_count) {
		ntbdev_send_packet_txed_interrupt(&priv->ntbdev);
		priv->txcq->tx_db_count++;
	}
#else
	ntbdev_send_packet_txed_interrupt(&priv->ntbdev);

#endif 
	return;
}

/*
* Transmit a packet (called by the kernel)
*/
int ntbeth_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct ntbeth_priv *priv = netdev_priv(dev);
	char *data;
	int len, avail_index;
	NTBETHDEBUG("Made it to ntbeth_tx \n");
	// obtain queue entry in ntb CQ.
	spin_lock_bh(&priv->lock);
	len = skb->len;
	dev->trans_start = jiffies;
	// drop the packet if peer status is not  up
	if (!(priv->peer_status & NTBETH_REMOTE_PEER_UP)) {
		NTBETHDEBUG(" packet dropped because remote peer is down: peer status 0x%x\n", priv->peer_status);
		dev_kfree_skb(skb);
		priv->stats.tx_dropped++;
		spin_unlock_bh(&priv->lock);
		return 0;
	}
	if (turnoff_tx) {
		if (priv->tx_pkt_count == turnoff_pkt_count) {
			dev_kfree_skb(skb);
			priv->stats.tx_dropped++;
			spin_unlock_bh(&priv->lock);
			return 0;
		}
	}
	if (!cq_is_buf_avail(&priv->txcq)) {
		ntbdev_send_packet_txed_interrupt(&priv->ntbdev);
		priv->stats.tx_dropped++;
		dev_kfree_skb(skb);
		spin_unlock_bh(&priv->lock);
		return 0;
	}
	avail_index = cq_avail_put_index(&priv->txcq);
	data = cq_get_current_put_entry_loc(&priv->txcq);
	if (data == NULL) {
		printk("Something wrong while accessing cq \n");
		printk("avail_index is %d\n", avail_index);
		printk(" packet dropped peer_status 0x%x\n", priv->peer_status);
		// no need to release skb
		// notify kernel that it should stop queue
		// just send one more remainder of availability of packets in the queue
		ntbdev_send_packet_txed_interrupt(&priv->ntbdev);
		priv->stats.tx_dropped++;
		dev_kfree_skb(skb);
		spin_unlock_bh(&priv->lock);
	return 0;
	}
	priv->tx_pkt_count++;
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += len;
	*(unsigned int *)data = len;
	ntbeth_copier_copy_from_skb(&priv->copier,skb,&priv->tx_dma_addresses[avail_index], data+4, len, tx_copy_callback, skb);
	//dump_info(priv, DEBUG_TX, data, len);
	spin_unlock_bh(&priv->lock);
	return 0;
}

/*
* Return statistics to the caller
*/
struct net_device_stats *ntbeth_stats(struct net_device *dev)
{
	struct ntbeth_priv *priv = netdev_priv(dev);
	return &priv->stats;
}

int ntbeth_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ntbeth_priv *priv = netdev_priv(dev);
	/* check ranges */
	if ((new_mtu < NTBETH_MIN_MTUSIZE) || (new_mtu > NTBETH_MAX_MTUSIZE))
		return -EINVAL;
	NTBETHDEBUG("Chaning Ntbeth MTU Size to %d\n", new_mtu);	
	spin_lock_bh(&priv->lock);
	dev->mtu = new_mtu;
	spin_unlock_bh(&priv->lock);
	return 0; /* success */
}

int ntbeth_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{

	/* current we do not implement any custom ioctls so we leave this empty.
	* but in future we can implment some ioctls to dump driver/ntb structures
	*/
	NTBETHDEBUG("ntbeth_do_ioctl entered\n");
	return 0;
}

int ntbeth_init(struct net_device *dev)
{
	int err;
	struct ntbeth_priv *priv = netdev_priv(dev);
	NTBETHDEBUG("Made it to ntbeth_init\n");
	ether_setup(dev); /* assign some of the fields */
	printk("NTBETH: Driver Version " NTBETH_VERSION "\n");
	if (use_cb3_dma_engine)
		printk("NTBETH: Using CB3 Driver for Packet Copying\n");
	else
		printk("NTBETH: Using CPU for Packet Copying\n");

	/* Set up some of the private variables now */
	NTBETHDEBUG("private ptr: 0x%Lx\n", (unsigned long long)priv);
	priv->status = 0;
	spin_lock_init(&priv->lock);
	netif_stop_queue(dev);
	update_peer_status(dev,NTBETH_LOCAL_PEER_DOWN);
	update_peer_status(dev,NTBETH_REMOTE_PEER_DOWN);
	dev->mtu		= NTBETH_MAX_MTUSIZE;
	dev->flags		|= IFF_NOARP;
	dev->features		|= NETIF_F_NO_CSUM;
	NTBETHDEBUG("rx_int_doorbell num  %d\n", rx_int_doorbell_num);
	// initialize ntb device info structures.
	if((err = ntbdev_init(&priv->ntbdev, bar23_size, bar45_size, rx_int_doorbell_num))) {
		printk("NTBETH: ntbdev init failed\n");
		return (err);
	}
	if((err = ntbeth_copier_init(&priv->copier, use_cb3_dma_engine, &priv->ntbdev)))
		return (err);
	// Initialize CQ to recv message from remote side
	priv->rxcq = ntbdev_get_bar23_local_memory(&priv->ntbdev);
	priv->txcq = ntbdev_get_bar23_value(&priv->ntbdev);
	priv->txf.cq_size = cq_calculate_num_entries(bar23_size); 
	init_cq(priv->rxcq, cq_calculate_num_entries(bar23_size),NTBETH_RX_CQ);
	priv->tx_dma_addresses = kmalloc(cq_calculate_num_entries(bar23_size) *sizeof(dma_addr_t), GFP_KERNEL);
	priv->rx_dma_addresses = kmalloc(cq_calculate_num_entries(bar23_size) *sizeof(dma_addr_t), GFP_KERNEL);
	memset(priv->tx_dma_addresses, 0, sizeof(dma_addr_t)*cq_calculate_num_entries(bar23_size));
	memset(priv->rx_dma_addresses, 0, sizeof(dma_addr_t)*cq_calculate_num_entries(bar23_size));
	// obtain rxcq  ptr
	NTBETHDEBUG("RxCQ Ptr 0x%Lx\n", (unsigned long long)priv->rxcq);
	NTBETHDEBUG("TxCQ Ptr 0x%Lx\n", (unsigned long long)priv->txcq);
	NTBETHDEBUG("bar23size 0x%Lx\n", (unsigned long long)bar23_size);
	NTBETHDEBUG("bar45size 0x%Lx\n", (unsigned long long)bar45_size);
	cq_dump_debug_data(priv->rxcq,"INIT: ");
	cq_dump_debug_data(priv->txcq,"INIT: ");
	NTBETHDEBUG("tx_pkt_count %d\n", priv->tx_pkt_count);
	NTBETHDEBUG("rx_pkt_count %d\n", priv->rx_pkt_count);
	return 0;
}

int ntbeth_set_mac_address(struct net_device *netdev, void *p)
{
// struct ntbeth_priv *priv = netdev_priv(netdev);
	NTBETHDEBUG("ntbeth_set_mac_address: Copied MAC Address to dev_addr\n");
	memcpy(netdev->dev_addr, p, ETH_ALEN);
	return 0;
}

void ntbeth_cleanup(void)
{
	int i;
	for(i=0; i < numntbs;i++)
	{
		unregister_netdev(ntbeth_device[i]->netdev);
		ntbeth_copier_cleanup(&ntbeth_device[i]->copier);
		// cleanup ntb device info structures.
		ntbdev_cleanup(&ntbeth_device[i]->ntbdev);
		// have to free private structure TBD???
		kfree(ntbeth_device[i]->tx_dma_addresses);
		kfree(ntbeth_device[i]->rx_dma_addresses);
	}
	return;
}

int ntbeth_init_module(void)
{
	int err;
	int i;
	struct net_device * netdev;
	struct ntbeth_priv *priv;
	for(i=0; i < numntbs;i++)
	{
		if (!(netdev = alloc_etherdev(sizeof(struct ntbeth_priv)))) {
			printk(KERN_ERR "Etherdev alloc failed, abort.\n");
			return -ENOMEM;
		}
		NTBETHDEBUG("netdev ptr 0x%Lx\n", (unsigned long long)netdev);
		netdev->netdev_ops = &ntbeth_netdev_ops;
		netdev->watchdog_timeo = NTBETH_WATCHDOG_PERIOD;
		if(i==0)
			strncpy(netdev->name, "ntb1",5);
		else
			strncpy(netdev->name, "ntb2",5);

		if ((err = register_netdev(netdev))) {
			printk(KERN_ERR "Unable to register network device with Kernel\n");
			return err;
		}
		priv = netdev_priv(netdev);
		memset(priv, 0, sizeof(struct ntbeth_priv));
		priv->netdev = netdev;
		priv->ntbdev.instance_id = i;
		ntbeth_device[i] = priv;
		if (ntbeth_init(netdev)) {
			printk("ERROR NTBETH: ntbeth_initialization failed\n");
			return -ENOMEM;
		}
	// subscribe to rx interrupt callback
		ntbdev_subscribe_to_rx_int(&priv->ntbdev, ntbeth_rx_interrupt, netdev);
		ntbdev_subscribe_to_txack_int(&priv->ntbdev, ntbeth_txack_interrupt, netdev);
		ntbdev_subscribe_to_ping_int(&priv->ntbdev, ntbeth_ping_interrupt, netdev);
		ntbdev_subscribe_to_ping_ack_int(&priv->ntbdev, ntbeth_ping_ack_interrupt, netdev);
		ntbdev_subscribe_to_lnkchg_int(&priv->ntbdev, ntbeth_lnkchg_interrupt, netdev);
		ntbdev_subscribe_to_close_int(&priv->ntbdev, ntbeth_close_interrupt, netdev);
		NTBETHDEBUG(" ntbeth_init_module completed successfully\n");
		NTBETHDEBUG( "ntbeth_init_module completed successfully\n");
	}
	return 0;
}

void update_peer_status(struct net_device *netdev, int peer_status)
{
	struct ntbeth_priv *priv = netdev_priv(netdev);
	spin_lock_bh(&priv->lock);
	// clear link status
	switch(peer_status) {
		case NTBETH_REMOTE_PEER_UP:
		case NTBETH_REMOTE_PEER_DOWN:
			priv->peer_status = (~NTBETH_REMOTE_MASK & priv->peer_status)|peer_status;
		break;
		case NTBETH_LOCAL_PEER_UP:
		case NTBETH_LOCAL_PEER_DOWN:
			priv->peer_status = (~NTBETH_LOCAL_MASK & priv->peer_status)|peer_status;
		break;
	}
	if (((priv->peer_status &  NTBETH_LOCAL_MASK) == NTBETH_LOCAL_PEER_UP) && ((priv->peer_status & NTBETH_REMOTE_MASK) == NTBETH_REMOTE_PEER_UP)) {
		NTBETHDEBUG("ntbeth: update_peer_status  both sides are up\n");
		netif_wake_queue(netdev);
	} else {
		NTBETHDEBUG("ntbeth: update_peer_status  at least one side is down\n");
		netif_stop_queue(netdev);
	}
	spin_unlock_bh(&priv->lock);
}

void dump_info(struct ntbeth_priv *priv, int side, void *pkt, int len)
{
	switch(side) {
		case DEBUG_RX:
			printk("\t\t\t\t\tRxed Pkt xCount %d w/len %d\n", priv->rx_pkt_count,len);
			cq_dump_debug_data(priv->rxcq, "\t\t\t\t\t");
		break;
		case DEBUG_TX:
			printk("Txed Pkt Count %d w/len %d\n", priv->tx_pkt_count, len);
			cq_dump_debug_data(priv->txcq, " ");
		break;
	}
}
void dump_memory(char *memoryloc, int size, char *fmtstr)
{
	unsigned char *pBuf = (unsigned char *)memoryloc;
	int i;
	printk("%sMemory Size %d\n", fmtstr,size);
	for(i=0; i < size; i++)
	{
		if (i%16 == 0)
			printk("\n%s0x%02x: ",fmtstr,i);
		if (i%4 == 0)
			printk(" ");
		printk("%02x",pBuf[i]);
	}
	return;
}

module_init(ntbeth_init_module);
module_exit(ntbeth_cleanup);
MODULE_DESCRIPTION(" ntbeth  network driver over NTB link");
MODULE_AUTHOR("Subba  Mungara, Intel Corporation");
