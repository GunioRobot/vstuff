/*
 * FIXME
 */

#ifndef _HFC_FIFO_H
#define _HFC_FIFO_H

#include "regs.h"
#include "util.h"

#define hfc_SM_D_FIFO_OFF 2
#define hfc_SM_B1_FIFO_OFF 0
#define hfc_SM_B2_FIFO_OFF 1
#define hfc_SM_E_FIFO_OFF 3

#ifdef DEBUG
#define hfc_debug_fifo(fifo, dbglevel, format, arg...)		\
	if (debug_level >= dbglevel)				\
		printk(KERN_DEBUG hfc_DRIVER_PREFIX		\
			"%s:"					\
			"fifo[%d,%s]:"				\
			format,					\
			(fifo)->card->pcidev->dev.bus_id,	\
			(fifo)->hw_index,			\
			(fifo)->direction == RX ? "RX" : "TX",	\
			## arg)
#else
#define hfc_debug_fifo(chan, dbglevel, format, arg...) do {} while (0)
#endif

#define hfc_msg_fifo(fifo, level, format, arg...)	\
	printk(level hfc_DRIVER_PREFIX			\
		"%s:"					\
		"fifo[%d,%s]:"				\
		format,					\
		(fifo)->card->pcidev->dev.bus_id,	\
		(fifo)->hw_index,			\
		(fifo)->direction == RX ? "RX" : "TX",	\
		## arg)

union hfc_zgroup
{
	struct { u16 z1,z2; };
	u32 z1z2;
};

union hfc_fgroup
{
	struct { u8 f1,f2; };
	u16 f1f2;
};

struct hfc_fifo_zone_config
{
	u16 z_min;
	u16 z_max;
};

struct hfc_fifo_config
{
	char v_ram_sz;
	char v_fifo_md;
	char v_fifo_sz;

	u8 f_min;
	u8 f_max;

	int zone2_start_id;
	int num_fifos;

	struct hfc_fifo_zone_config zone1;
	struct hfc_fifo_zone_config zone2;
};

struct hfc_fifo
{
	struct hfc_card *card;
	struct hfc_chan_simplex *connected_chan;
	struct hfc_chan_simplex *looped_chan;

	int hw_index;
	enum hfc_direction direction;

	int bit_reversed;

	int used;

	// Counters cache
	union {
		struct { u16 z1,z2; };
		u32 z1z2;
	};

	union {
		struct { u8 f1,f2; };
		u16 f1f2;
	};

	u16 z_min;
	u16 z_max;
	u16 fifo_size;

	u8 f_min;
	u8 f_max;
	u8 f_num;

	struct tasklet_struct tasklet;
};

int hfc_fifo_mem_read(struct hfc_fifo *fifo,
	void *data, int size);
int hfc_fifo_mem_read_to_user(struct hfc_fifo *fifo,
	void __user *data, int size);

void hfc_fifo_clear_rx(struct hfc_fifo *fifo);
void hfc_fifo_clear_tx(struct hfc_fifo *fifo);
int hfc_fifo_get(struct hfc_fifo *fifo, void *data, int size);
void hfc_fifo_put(struct hfc_fifo *fifo, void *data, int size);
void hfc_fifo_drop(struct hfc_fifo *fifo, int size);
int hfc_fifo_get_frame(struct hfc_fifo *fifo, void *data, int max_size);
void hfc_fifo_drop_frame(struct hfc_fifo *fifo);
void hfc_fifo_put_frame(struct hfc_fifo *fifo, void *data, int size);
void hfc_fifo_init(
	struct hfc_fifo *fifo,
	struct hfc_card *card,
	int hw_index,
	enum hfc_direction direction);

#endif
