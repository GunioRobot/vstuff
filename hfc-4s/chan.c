#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/config.h>
#include <linux/delay.h>

#include <lapd.h>
#include <visdn.h>

#include "hfc-4s.h"
#include "chan.h"
#include "card.h"
#include "st_port.h"
#include "st_port_inline.h"
#include "fifo.h"
#include "fifo_inline.h"
#include "fsm.h"

void hfc_chan_disable(struct hfc_chan_duplex *chan)
{
	struct hfc_card *card = chan->port->card;

	WARN_ON(!irqs_disabled() && !in_irq());

	hfc_st_port_select(card, chan->port->id);

	if (chan->id == B1 || chan->id == B2) {
		if (chan->id == B1) {
			chan->port->regs.st_ctrl_0 &= ~hfc_A_ST_CTRL0_V_B1_EN;
			chan->port->regs.st_ctrl_2 &= ~hfc_A_ST_CTRL2_V_B1_RX_EN;
		} else if (chan->id == B2) {
			chan->port->regs.st_ctrl_0 &= ~hfc_A_ST_CTRL0_V_B2_EN;
			chan->port->regs.st_ctrl_2 &= ~hfc_A_ST_CTRL2_V_B2_RX_EN;
		}

		hfc_outb(card, hfc_A_ST_CTRL0,
			chan->port->regs.st_ctrl_0);
		hfc_outb(card, hfc_A_ST_CTRL2,
			chan->port->regs.st_ctrl_2);
	}

	// RX
	chan->rx.fifo->bit_reversed = FALSE;
	hfc_fifo_select(chan->rx.fifo);
	hfc_fifo_reset(chan->rx.fifo);
	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_IFF|
		hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_DISABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST);

	hfc_outb(card, hfc_A_IRQ_MSK, 0);

	// TX
	chan->tx.fifo->bit_reversed = FALSE;
	hfc_fifo_select(chan->tx.fifo);
	hfc_fifo_reset(chan->tx.fifo);
	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_IFF|
		hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_DISABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_ST_FIFO_to_PCM);

	hfc_outb(card, hfc_A_IRQ_MSK, 0);
}

void hfc_chan_enable(struct hfc_chan_duplex *chan)
{
	struct hfc_card *card = chan->port->card;

	if (chan->id != B1 && chan->id != B2)
		return;

	hfc_st_port_select(card, chan->port->id);

	if (chan->id == B1) {
		chan->port->regs.st_ctrl_0 |= hfc_A_ST_CTRL0_V_B1_EN;
		chan->port->regs.st_ctrl_2 |= hfc_A_ST_CTRL2_V_B1_RX_EN;
	} else if (chan->id == B2) {
		chan->port->regs.st_ctrl_0 |= hfc_A_ST_CTRL0_V_B2_EN;
		chan->port->regs.st_ctrl_2 |= hfc_A_ST_CTRL2_V_B2_RX_EN;
	}

	hfc_outb(card, hfc_A_ST_CTRL0,
		chan->port->regs.st_ctrl_0);
	hfc_outb(card, hfc_A_ST_CTRL2,
		chan->port->regs.st_ctrl_2);
}


static int hfc_chan_open(struct visdn_chan *visdn_chan)
{
	struct hfc_chan_duplex *chan = visdn_chan->priv;
	struct hfc_card *card = chan->port->card;

	int err;
	unsigned long flags;

	spin_lock_irqsave(&card->lock, flags);

	if (chan->status != HFC_STATUS_FREE) {
		err = -EBUSY;
		goto err_channel_busy;
	}

	if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_TRANS) {
		chan->status = HFC_STATUS_OPEN_TRANS;
	} else if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_HDLC) {
		chan->status = HFC_STATUS_OPEN_HDLC;
	} else {
		err = -EINVAL;
		goto err_invalid_framing;
	}

	struct hfc_fifo *fifo_rx;
	fifo_rx = hfc_allocate_fifo(card, RX);
	if (!fifo_rx) {
		err = -ENOMEM;
		goto err_allocate_fifo_rx;
	}

	if (chan->id != E) {
		struct hfc_fifo *fifo_tx;
		fifo_tx = hfc_allocate_fifo(card, TX);
		if (!fifo_tx) {
			err = -ENOMEM;
			goto err_allocate_fifo_tx;
		}

		chan->tx.fifo = fifo_tx;
		fifo_tx->connected_chan = &chan->tx;
	}

	chan->rx.fifo = fifo_rx;
	fifo_rx->connected_chan = &chan->rx;

	hfc_upload_fsm(card);

	// ------------- RX -----------------------------
	chan->rx.fifo->bit_reversed =
		chan->visdn_chan.bitorder == VISDN_CHAN_BITORDER_MSB;

	hfc_fifo_select(chan->rx.fifo);
	hfc_fifo_reset(chan->rx.fifo);

	if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_TRANS) {

		hfc_outb(card, hfc_A_CON_HDLC,
			hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
			hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
			hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST);

	} else if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_HDLC) {

		hfc_outb(card, hfc_A_CON_HDLC,
			hfc_A_CON_HDCL_V_IFF|
			hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
			hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
			hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST);
	}

	hfc_outb(card, hfc_A_IRQ_MSK,
		hfc_A_IRQ_MSK_V_IRQ);

	// ------------- TX -----------------------------

	if (chan->tx.fifo) {
		chan->tx.fifo->bit_reversed =
			chan->visdn_chan.bitorder == VISDN_CHAN_BITORDER_MSB;

		hfc_fifo_select(chan->tx.fifo);
		hfc_fifo_reset(chan->tx.fifo);

		if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_TRANS) {

			hfc_outb(card, hfc_A_CON_HDLC,
				hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
				hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
				hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_ST_FIFO_to_PCM);

		} else if (chan->visdn_chan.framing == VISDN_CHAN_FRAMING_HDLC) {

			hfc_outb(card, hfc_A_CON_HDLC,
				hfc_A_CON_HDCL_V_IFF|
				hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
				hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
				hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_to_ST_FIFO_to_PCM);
		}

		hfc_outb(card, hfc_A_IRQ_MSK,
			hfc_A_IRQ_MSK_V_IRQ);
	}

	hfc_chan_enable(chan);

	spin_unlock_irqrestore(&card->lock, flags);

	hfc_msg_chan(chan, KERN_INFO, "channel opened.\n");

	return 0;

	hfc_deallocate_fifo(chan->tx.fifo);
err_allocate_fifo_tx:
	hfc_deallocate_fifo(chan->rx.fifo);
err_allocate_fifo_rx:
	chan->status = HFC_STATUS_FREE;
err_invalid_framing:
err_channel_busy:

	spin_unlock_irqrestore(&card->lock, flags);

	return err;
}

static int hfc_chan_close(struct visdn_chan *visdn_chan)
{
	struct hfc_chan_duplex *chan = visdn_chan->priv;
	struct hfc_card *card = chan->port->card;

	unsigned long flags;
	spin_lock_irqsave(&card->lock, flags);

	chan->status = HFC_STATUS_FREE;

	hfc_chan_disable(chan);

	if (chan->rx.fifo)
		hfc_deallocate_fifo(chan->rx.fifo);

	chan->rx.fifo->connected_chan = NULL;
	chan->rx.fifo = NULL;

	if (chan->tx.fifo)
		hfc_deallocate_fifo(chan->tx.fifo);

	chan->tx.fifo->connected_chan = NULL;
	chan->tx.fifo = NULL;

	hfc_upload_fsm(card);

	spin_unlock_irqrestore(&card->lock, flags);

	hfc_msg_chan(chan, KERN_INFO, "channel closed.\n");

	return 0;
}

static int hfc_chan_frame_xmit(struct visdn_chan *visdn_chan, struct sk_buff *skb)
{
	struct hfc_chan_duplex *chan = visdn_chan->priv;
	struct hfc_card *card = chan->port->card;

	unsigned long flags;
	spin_lock_irqsave(&card->lock, flags);
	
	hfc_st_port_select(chan->port->card, chan->port->id);
	hfc_st_port_check_l1_up(chan->port);

	hfc_fifo_select(chan->tx.fifo);
	// hfc_fifo_select() updates F/Z cache, so,
	// size calculations are allowed

	if (!hfc_fifo_free_frames(chan->tx.fifo)) {
		hfc_debug_chan(chan, 3, "TX FIFO frames full, throttling\n");

		visdn_stop_queue(visdn_chan);

		goto err_no_free_frames;
	}

	if (hfc_fifo_free_tx(chan->tx.fifo) < skb->len) {
		hfc_debug_chan(chan, 3, "TX FIFO full, throttling\n");

		visdn_stop_queue(visdn_chan);

		goto err_no_free_tx;
	}

	hfc_fifo_put_frame(chan->tx.fifo, skb->data, skb->len);

	spin_unlock_irqrestore(&card->lock, flags);

	visdn_kfree_skb(skb);

	return 0;

err_no_free_tx:
err_no_free_frames:

	spin_unlock_irqrestore(&card->lock, flags);

	return 0;
}

static struct net_device_stats *hfc_chan_get_stats(struct visdn_chan *visdn_chan)
{
	struct hfc_chan_duplex *chan = visdn_chan->priv;
//	struct hfc_card *card = chan->card;

	return &chan->net_device_stats;
}

/*
static void hfc_set_multicast_list(struct net_device *netdev)
{
	struct hfc_chan_duplex *chan = netdev->priv;
	struct hfc_st_port *port = chan->port;
	struct hfc_card *card = port->card;

	unsigned long flags;
	spin_lock_irqsave(&card->lock, flags);

        if(netdev->flags & IFF_PROMISC && !port->echo_enabled) {
		if (port->nt_mode) {
			hfc_msg_port(port, KERN_INFO,
				"is in NT mode. Promiscuity is useless\n");

			spin_unlock_irqrestore(&card->lock, flags);
			return;
		}

		// Only RX FIFO is needed for E channel
		chan->rx.bit_reversed = FALSE;
		hfc_fifo_select(&port->chans[E].rx);
		hfc_fifo_reset(card);
		hfc_outb(card, hfc_A_CON_HDLC,
			hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
			hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
			hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST);

		hfc_outb(card, hfc_A_SUBCH_CFG,
			hfc_A_SUBCH_CFG_V_BIT_CNT_2);

		hfc_outb(card, hfc_A_IRQ_MSK,
			hfc_A_IRQ_MSK_V_IRQ);

		port->echo_enabled = TRUE;

		hfc_msg_port(port, KERN_INFO,
			"entered in promiscuous mode\n");

        } else if(!(netdev->flags & IFF_PROMISC) && port->echo_enabled) {
		if (!port->echo_enabled) {
			spin_unlock_irqrestore(&card->lock, flags);
			return;
		}

		chan->rx.bit_reversed = FALSE;
		hfc_fifo_select(&port->chans[E].rx);
		hfc_outb(card, hfc_A_CON_HDLC,
			hfc_A_CON_HDCL_V_HDLC_TRP_HDLC|
			hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_DISABLED|
			hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST);

		hfc_outb(card, hfc_A_IRQ_MSK,
			0);

		port->echo_enabled = FALSE;

		hfc_msg_port(port, KERN_INFO,
			"left promiscuous mode.\n");
	}

	spin_unlock_irqrestore(&card->lock, flags);

//	hfc_update_fifo_state(card);
}
*/

static ssize_t hfc_chan_samples_read(
	struct visdn_chan *visdn_chan,
	char __user *buf, size_t count)
{
	struct hfc_chan_duplex *chan = to_chan_duplex(visdn_chan);
	struct hfc_card *card = chan->port->card;

	int err;

	unsigned long flags;

	// Avoid user specifying too big transfers
	if (count > 65536)
		count = 65536;

	// Allocate a big enough buffer
	u8 *buf2 = kmalloc(count, GFP_KERNEL);
	if (!buf2) {
		err = -EFAULT;
		goto err_kmalloc;
	}

	spin_lock_irqsave(&card->lock, flags);

	hfc_fifo_select(chan->rx.fifo);
	hfc_fifo_refresh_fz_cache(chan->rx.fifo);

	int available_octets = hfc_fifo_used_rx(chan->rx.fifo);
	int copied_octets = 0;

	copied_octets = available_octets < count ? available_octets : count;

	// Read from FIFO in atomic context
	// Cannot read directly to user due to put_user sleeping
	hfc_fifo_mem_read(chan->rx.fifo, buf2, copied_octets);

	spin_unlock_irqrestore(&card->lock, flags);

	err = copy_to_user(buf, buf2, copied_octets);
	if (err < 0)
		goto err_copy_to_user;

	kfree(buf2);

	return copied_octets;

err_copy_to_user:
	kfree(buf2);
err_kmalloc:

	return err;
}

static ssize_t hfc_chan_samples_write(
	struct visdn_chan *visdn_chan,
	const char __user *buf, size_t count)
{
	struct hfc_chan_duplex *chan = to_chan_duplex(visdn_chan);
	struct hfc_card *card = chan->port->card;
	int err = 0;

	__u8 buf2[256]; // FIXME TODO

	// Umpf... we need an intermediate buffer... we need to disable interrupts
	// for the whole time since we must ensure that noone selects another FIFO
	// in the meantime, so we may not directly copy from FIFO to user.
	// There is for sure a better solution :)

	int copied_octets = count;
	if (copied_octets > sizeof(buf2))
		copied_octets = sizeof(buf2);

	err = copy_from_user(buf2, buf, copied_octets);
	if (err < 0)
		goto err_copy_to_user;

	unsigned long flags;
	spin_lock_irqsave(&card->lock, flags);

	hfc_fifo_select(chan->tx.fifo);
	hfc_fifo_refresh_fz_cache(chan->tx.fifo);

	int available_octets = hfc_fifo_free_tx(chan->tx.fifo);
	if (copied_octets > available_octets)
		copied_octets = available_octets;

	hfc_fifo_put(chan->rx.fifo, buf2, copied_octets);

	spin_unlock_irqrestore(&card->lock, flags);

	return copied_octets;

err_copy_to_user:

	return err;
}

static int hfc_chan_do_ioctl(struct visdn_chan *visdn_chan,
	struct ifreq *ifr, int cmd)
{
//	struct hfc_chan_duplex *chan = visdn_chan->priv;
//	struct hfc_card *card = chan->port->card;

//	unsigned long flags;
/*

	switch (cmd) {
	case VISDN_SET_BEARER: {

	struct sb_setbearer sb;
	if (copy_from_user(&sb, ifr->ifr_data, sizeof(sb)))
		return -EFAULT;

hfc_msg_chan(chan, KERN_INFO, "VISDN_SET_BEARER %d %d\n", sb.sb_index, sb.sb_bearertype);

	struct hfc_chan_duplex *bchan;
	if (sb.sb_index == 0)
		bchan = &chan->port->chans[B1];
	else if (sb.sb_index == 1)
		bchan = &chan->port->chans[B2];
	else
		return -EINVAL;

	if (sb.sb_bearertype == VISDN_BT_VOICE) {
		
	} else if (sb.sb_bearertype == VISDN_BT_PPP) {

		b1_chan->status = open_ppp;

		spin_unlock_irqrestore(&card->lock, flags);

////////////////////////////
		b1_chan->ppp_chan.private = b1_chan;
		b1_chan->ppp_chan.ops = &hfc_ppp_ops;
		b1_chan->ppp_chan.mtu = 1000; //FIXME
		b1_chan->ppp_chan.hdrlen = 2;

		ppp_register_channel(&b1_chan->ppp_chan);
////////////////////////

hfc_msg_chan(chan, KERN_INFO,
	"PPPPPPPPPPPP: int %d unit %d\n",
	ppp_channel_index(&b1_chan->ppp_chan),
	ppp_unit_number(&b1_chan->ppp_chan));


	break;

	case VISDN_PPP_GET_CHAN:
hfc_msg_chan(chan, KERN_INFO, "VISDN_PPP_GET_CHAN:\n");

		put_user(ppp_channel_index(&bchan->ppp_chan),
			(int __user *)ifr->ifr_data);
	break;

	case VISDN_PPP_GET_UNIT:
hfc_msg_chan(chan, KERN_INFO, "VISDN_PPP_GET_UNIT:\n");

		put_user(ppp_unit_number(&bchan->ppp_chan),
			(int __user *)ifr->ifr_data);
	break;

	default:
		return -ENOIOCTLCMD;
	}
*/

	return 0;
}

static int hfc_bridge(
	struct hfc_chan_duplex *chan,
	struct hfc_chan_duplex *chan2)
{
	struct hfc_card *card = chan->port->card;
	int err;

	unsigned long flags;
	spin_lock_irqsave(&card->lock, flags);

	chan->rx.fifo = hfc_allocate_fifo(card, RX);
	if (!chan->rx.fifo) {
		err = -ENOMEM;
		goto err_allocate_fifo_1_rx;
	}

	chan->rx.fifo = hfc_allocate_fifo(card, TX);
	if (!chan->tx.fifo) {
		err = -ENOMEM;
		goto err_allocate_fifo_1_tx;
	}

	chan2->rx.fifo = hfc_allocate_fifo(card, RX);
	if (!chan2->rx.fifo) {
		err = -ENOMEM;
		goto err_allocate_fifo_2_rx;
	}

	chan2->rx.fifo = hfc_allocate_fifo(card, TX);
	if (!chan2->tx.fifo) {
		err = -ENOMEM;
		goto err_allocate_fifo_2_tx;
	}

	chan->rx.fifo->connected_chan = &chan->rx;
	chan->tx.fifo->connected_chan = &chan->tx;
	chan2->rx.fifo->connected_chan = &chan2->rx;
	chan2->tx.fifo->connected_chan = &chan2->tx;

	hfc_upload_fsm(card);

	hfc_fifo_select(chan->rx.fifo);
	hfc_fifo_reset(chan->rx.fifo);

	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST_ST_from_PCM);

	hfc_fifo_select(chan->tx.fifo);
	hfc_fifo_reset(chan->tx.fifo);

	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_ST_to_PCM);

	hfc_fifo_select(chan2->rx.fifo);
	hfc_fifo_reset(chan2->rx.fifo);

	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_FIFO_from_ST_ST_from_PCM);

	hfc_fifo_select(chan2->tx.fifo);
	hfc_fifo_reset(chan2->tx.fifo);

	hfc_outb(card, hfc_A_CON_HDLC,
		hfc_A_CON_HDCL_V_HDLC_TRP_TRP|
		hfc_A_CON_HDCL_V_TRP_IRQ_FIFO_ENABLED|
		hfc_A_CON_HDCL_V_DATA_FLOW_ST_to_PCM);

	// Slot 0
	hfc_outb(card, hfc_R_SLOT,
		hfc_R_SLOT_V_SL_DIR_RX |
		hfc_R_SLOT_V_SL_NUM(0));

	hfc_outb(card, hfc_A_SL_CFG,
		hfc_A_SL_CFG_V_CH_SDIR_RX |
		hfc_A_SL_CFG_V_CH_NUM(chan->hw_index) |
		hfc_A_SL_CFG_V_ROUT_LOOP);

	hfc_outb(card, hfc_R_SLOT,
		hfc_R_SLOT_V_SL_DIR_TX |
		hfc_R_SLOT_V_SL_NUM(0));

	hfc_outb(card, hfc_A_SL_CFG,
		hfc_A_SL_CFG_V_CH_SDIR_TX |
		hfc_A_SL_CFG_V_CH_NUM(chan2->hw_index) |
		hfc_A_SL_CFG_V_ROUT_LOOP);

	// Slot 1
	hfc_outb(card, hfc_R_SLOT,
		hfc_R_SLOT_V_SL_DIR_RX |
		hfc_R_SLOT_V_SL_NUM(1));

	hfc_outb(card, hfc_A_SL_CFG,
		hfc_A_SL_CFG_V_CH_SDIR_RX |
		hfc_A_SL_CFG_V_CH_NUM(chan2->hw_index) |
		hfc_A_SL_CFG_V_ROUT_LOOP);

	hfc_outb(card, hfc_R_SLOT,
		hfc_R_SLOT_V_SL_DIR_TX |
		hfc_R_SLOT_V_SL_NUM(1));

	hfc_outb(card, hfc_A_SL_CFG,
		hfc_A_SL_CFG_V_CH_SDIR_TX |
		hfc_A_SL_CFG_V_CH_NUM(chan->hw_index) |
		hfc_A_SL_CFG_V_ROUT_LOOP);

	spin_unlock_irqrestore(&card->lock, flags);
	
	return VISDN_CONNECT_BRIDGED;

	hfc_deallocate_fifo(chan2->tx.fifo);
	chan2->tx.fifo = NULL;
err_allocate_fifo_2_tx:
	hfc_deallocate_fifo(chan2->rx.fifo);
	chan2->rx.fifo = NULL;
err_allocate_fifo_2_rx:
	hfc_deallocate_fifo(chan->tx.fifo);
	chan->tx.fifo = NULL;
err_allocate_fifo_1_tx:
	hfc_deallocate_fifo(chan->rx.fifo);
	chan->rx.fifo = NULL;
err_allocate_fifo_1_rx:

	return err;
}

static int hfc_chan_connect_to(
	struct visdn_chan *visdn_chan,
	struct visdn_chan *visdn_chan2,
	int flags)
{
	if (visdn_chan->connected_chan)
		return -EBUSY;

	struct hfc_chan_duplex *chan = visdn_chan->priv;
	struct hfc_chan_duplex *chan2 = visdn_chan2->priv;
	struct hfc_card *card = chan->port->card;

	hfc_debug_card(card, 2, "Connecting chan %s to %s\n",
		visdn_chan->device.bus_id,
		visdn_chan2->device.bus_id);

	if (chan->id != B1 && chan->id != B2) {
		hfc_msg(KERN_ERR, "Cannot connect %s to %s\n",
			chan->name, to_chan_duplex(visdn_chan2)->name);
		return -EINVAL;
	}

	if (visdn_chan->device.parent->parent ==
			visdn_chan2->device.parent->parent) {
		printk(KERN_DEBUG "Both channels belong to the me,"
			" attempting private bridge\n");

		return hfc_bridge(chan, chan2);
	}

	return VISDN_CONNECT_OK;
}

static int hfc_chan_disconnect(struct visdn_chan *visdn_chan)
{
	if (!visdn_chan->connected_chan)
		return 0;

printk(KERN_INFO "hfc-4s chan %s disconnecting from %s\n",
		visdn_chan->device.bus_id,
		visdn_chan->connected_chan->device.bus_id);

	return 0;
}

struct visdn_chan_ops hfc_chan_ops = {
	.open		= hfc_chan_open,
	.close		= hfc_chan_close,
	.frame_xmit	= hfc_chan_frame_xmit,
	.get_stats	= hfc_chan_get_stats,
	.do_ioctl	= hfc_chan_do_ioctl,

	.connect_to	= hfc_chan_connect_to,
	.disconnect	= hfc_chan_disconnect,

	.samples_read	= hfc_chan_samples_read,
	.samples_write	= hfc_chan_samples_write,
};

