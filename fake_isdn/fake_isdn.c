/*
 * Copyright (C) 2005 Daniele Orlandi
 *
 * Daniele "Vihai" Orlandi <daniele@orlandi.com> 
 *
 * This program is free software and may be modified and
 * distributed under the terms of the GNU Public License.
 *
 */

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>

#include <vihai_isdn.h>
#include <lapd_user.h>
#include <lapd.h>

#include "fake_isdn.h"

static struct fake_card *card = NULL;

static int fake_open(struct net_device *netdev)
{
	printk(KERN_INFO fake_DRIVER_PREFIX
		"chan %s opened.\n",
		netdev->name);

	return 0;
}

static int fake_close(struct net_device *netdev)
{
	printk(KERN_INFO fake_DRIVER_PREFIX
		"chan %s closed.\n",
		netdev->name);

	return 0;
}

static int fake_xmit_frame(struct sk_buff *skb, struct net_device *netdev)
{
	struct fake_chan *chan = netdev->priv;
	struct fake_card *card = chan->card;

	netdev->trans_start = jiffies;

	struct fake_chan *dst_chan;
	if (chan == &card->chans[0])
		dst_chan = &card->chans[1];
	else
		dst_chan = &card->chans[0];

	struct sk_buff *dst_skb =
		skb_clone(skb, GFP_ATOMIC);

	dst_skb->dev = dst_chan->netdev;
	dst_skb->pkt_type = PACKET_HOST;

	dst_skb->ip_summed = CHECKSUM_UNNECESSARY;

	chan->net_device_stats.tx_packets++;
	chan->net_device_stats.tx_bytes += skb->len;

	dst_chan->net_device_stats.rx_packets++;
	dst_chan->net_device_stats.rx_bytes += skb->len;

	netif_rx(dst_skb);

	dev_kfree_skb(skb);

	return 0;
}

static struct net_device_stats *fake_get_stats(struct net_device *netdev)
{
	struct fake_chan *chan = netdev->priv;

	return &chan->net_device_stats;
}

/******************************************
 * Module initialization and cleanup
 ******************************************/

static void fake_setup_lapd(struct fake_chan *chan)
{
	chan->netdev->priv = chan;
	chan->netdev->open = fake_open;
	chan->netdev->stop = fake_close;
	chan->netdev->hard_start_xmit = fake_xmit_frame;
	chan->netdev->get_stats = fake_get_stats;
	chan->netdev->set_multicast_list = NULL;
	chan->netdev->features = NETIF_F_NO_CSUM;

	memset(chan->netdev->dev_addr, 0x00, sizeof(chan->netdev->dev_addr));

	SET_MODULE_OWNER(chan->netdev);
}

/******************************************
 * Module stuff
 ******************************************/

static int __init fake_init_module(void)
{
	int err;

	printk(KERN_INFO fake_DRIVER_PREFIX
		fake_DRIVER_DESCR " loading\n");

	card = kmalloc(sizeof(struct fake_card), GFP_KERNEL);
	if (!card) {
		printk(KERN_CRIT fake_DRIVER_PREFIX
			"unable to kmalloc!\n");
		err = -ENOMEM;
		goto err_alloc_card;
	}

	memset(card, 0x00, sizeof(struct fake_card));
	spin_lock_init(&card->lock);

	struct fake_chan *chan;

	chan = &card->chans[0];

	chan->card = card;

	chan->netdev = alloc_netdev(0, "fakeisdn%dd", setup_lapd);
	if(!chan->netdev) {
		printk(KERN_ERR fake_DRIVER_PREFIX
			"net_device alloc failed, abort.\n");
		err = -ENOMEM;
		goto err_alloc_netdev_1;
	}

	fake_setup_lapd(chan);

	if((err = register_netdev(chan->netdev))) {
		printk(KERN_INFO fake_DRIVER_PREFIX
			"Cannot register net device %s, aborting.\n",
			chan->netdev->name);
		goto err_register_netdev_1;
	}

	chan = &card->chans[1];

	chan->card = card;

	chan->netdev = alloc_netdev(0, "fakeisdn%dd", setup_lapd);
	if(!chan->netdev) {
		printk(KERN_ERR fake_DRIVER_PREFIX
			"net_device alloc failed, abort.\n");
		err = -ENOMEM;
		goto err_alloc_netdev_2;
	}

	fake_setup_lapd(chan);

	if((err = register_netdev(chan->netdev))) {
		printk(KERN_INFO fake_DRIVER_PREFIX
			"Cannot register net device %s, aborting.\n",
			chan->netdev->name);
		goto err_register_netdev_2;
	}

	return 0;

//	unregister_netdev(card->chans[1].netdev);
err_register_netdev_2:
	unregister_netdev(card->chans[0].netdev);
err_register_netdev_1:
	free_netdev(card->chans[1].netdev);
err_alloc_netdev_1:
	free_netdev(card->chans[0].netdev);
err_alloc_netdev_2:
err_alloc_card:
	return err;
}

module_init(fake_init_module);

static void __exit fake_module_exit(void)
{
	printk(KERN_INFO fake_DRIVER_PREFIX
		fake_DRIVER_DESCR " beginning unload\n");

	unregister_netdev(card->chans[0].netdev);
printk(KERN_DEBUG fake_DRIVER_PREFIX fake_DRIVER_DESCR " step1\n");
	unregister_netdev(card->chans[1].netdev);
printk(KERN_DEBUG fake_DRIVER_PREFIX fake_DRIVER_DESCR " step2\n");

	free_netdev(card->chans[0].netdev);
printk(KERN_DEBUG fake_DRIVER_PREFIX fake_DRIVER_DESCR " step3\n");
	free_netdev(card->chans[1].netdev);
printk(KERN_DEBUG fake_DRIVER_PREFIX fake_DRIVER_DESCR " step4\n");

	kfree(card);

	printk(KERN_INFO fake_DRIVER_PREFIX
		fake_DRIVER_DESCR " unloaded\n");
}

module_exit(fake_module_exit);

MODULE_DESCRIPTION(fake_DRIVER_DESCR);
MODULE_AUTHOR("Daniele (Vihai) Orlandi <daniele@orlandi.com>");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
