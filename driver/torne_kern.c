// SPDX-License-Identifier: GPL-2.0-only
/*
 * torne_kern.c - Linux driver for the Sony CECH-ZD1 J ("torne", PS3 terrestrial tuner)
 *
 * The device appears as /dev/tornevideoN and speaks the same ioctl and read() interface as px4_drv's
 * /dev/px4video* (ptx_chrdev.c is taken from px4_drv, Copyright (c) 2018-2021 nns779), so recisdb, recpt1 and
 * Mirakurun use it like a PLEX tuner. The chip handling is in torne_bridge.c / torne_dib.c (user-space tested).
 *
 * ISDB-T (terrestrial) only. The B-CAS card is read by an external card reader (pcscd) as with the PLEX tuners.
 */
#include "print_format.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>

#include "ptx_chrdev.h"
#include "torne_core.h"
#include "torne_dib.h"
#include "torne_card.h"
#include "torne_ioctl.h"

#define TORNE_MAX_DEVICE 16
#define TORNE_LED_PIN 1 /* DiB8096 GPIO wired to the front LED: low or input = green, high = red */

#define TORNE_URB_NUM 8
#define TORNE_URB_SIZE (1536 * 32)
#define TORNE_STREAM_EP 0x81
#define TORNE_RINGBUF_PACKETS 4096

static int log_level = TORNE_LOG_ERR;
module_param(log_level, int, 0644);
MODULE_PARM_DESC(log_level, "driver core log level (0 error, 1 info, 2 debug)");

/*
 * While capturing, if no TS data comes for this many ms (no signal, or tuned to an empty channel), null packets are fed to the
 * reader so that read() returns. The PLEX tuners keep delivering data without a signal; some programs depend on it:
 * BonDriver_LinuxPTX waits for its read thread to fill a whole buffer before it can stop it. 0 turns this off.
 */
static unsigned int idle_fill_ms = 100;
module_param(idle_fill_ms, uint, 0644);
MODULE_PARM_DESC(idle_fill_ms, "feed null TS packets after this many ms without data while capturing (0: off)");

#define TORNE_IDLE_PERIOD_MS 20
#define TORNE_IDLE_PACKETS 64 /* per period: about 4.8 Mbit/s of null packets */

struct torne_urb {
	struct torne_device *dev;
	struct urb *urb;
	u8 *buf;
	dma_addr_t dma;
};

struct torne_device {
	struct kref kref;
	atomic_t available;
	struct usb_device *udev;
	struct device *dev;
	struct completion *quit_completion;
	struct ptx_chrdev_group *chrdev_group;
	struct ptx_chrdev *chrdev;

	struct mutex lock; /* serializes all access to the bridge and the demodulator */
	u8 *ctrl_buf;
	struct torne_bridge bridge;
	struct torne_dib dib;
	struct torne_card card;
	bool bridge_up; /* bridge started */
	bool ready;	/* demodulator initialized */

	/* card slot (/dev/tornecardN) */
	struct miscdevice misc;
	char misc_name[16];
	bool misc_registered;

	/* streaming */
	struct torne_urb urbs[TORNE_URB_NUM];
	atomic_t stream_stop;
	spinlock_t stream_lock;
	struct torne_framer *framer;
	u8 *ts_out;
	bool urbs_running;
	struct delayed_work idle_work; /* feeds null packets while no data arrives */
	unsigned long last_data;       /* jiffies of the last real TS data, under stream_lock */
};

static struct ptx_chrdev_context *torne_chrdev_ctx;

/* ---- core callbacks ---- */

static int torne_k_control(void *priv, u8 request_type, u16 value, u16 index, void *buf, u16 len)
{
	struct torne_device *t = priv;
	bool in = request_type & USB_DIR_IN;
	int pipe = in ? usb_rcvctrlpipe(t->udev, 0) : usb_sndctrlpipe(t->udev, 0);
	int ret;

	if (!atomic_read(&t->available))
		return -ENODEV;
	if (len > 64)
		return -EINVAL;
	if (!in)
		memcpy(t->ctrl_buf, buf, len);
	ret = usb_control_msg(t->udev, pipe, 0xbc, request_type, value, index, t->ctrl_buf, len, 1000);
	if (in && ret > 0)
		memcpy(buf, t->ctrl_buf, ret);
	return ret;
}

static int torne_k_bulk(void *priv, u8 endpoint, void *buf, int len, unsigned int timeout_ms)
{
	return -ENOSYS; /* the core does not use bulk transfers outside of streaming */
}

static void torne_k_msleep(void *priv, unsigned int ms)
{
	msleep(ms);
}

static u64 torne_k_now_ms(void *priv)
{
	return ktime_get_ns() / NSEC_PER_MSEC;
}

static void torne_k_log(void *priv, int level, const char *fmt, ...)
{
	struct torne_device *t = priv;
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (level <= TORNE_LOG_ERR)
		dev_err(t->dev, "%pV\n", &vaf);
	else
		dev_info(t->dev, "%pV\n", &vaf);
	va_end(args);
}

static const struct torne_ops torne_kops = {
	.control = torne_k_control,
	.bulk = torne_k_bulk,
	.msleep = torne_k_msleep,
	.now_ms = torne_k_now_ms,
	.log = torne_k_log,
};

/* ---- hardware bring-up ---- */

/* starts the bridge (once); the card slot and the tuner both need it. t->lock held */
static int torne_bridge_ensure(struct torne_device *t)
{
	int ret;

	if (t->bridge_up)
		return 0;
	if (!atomic_read(&t->available))
		return -ENODEV;
	ret = usb_set_interface(t->udev, 0, 0);
	if (ret)
		dev_warn(t->dev, "usb_set_interface: %d\n", ret);
	ret = torne_bridge_start(&t->bridge);
	if (ret) {
		dev_err(t->dev, "bridge start failed: %d\n", ret);
		return ret;
	}
	torne_card_setup(&t->card, &t->bridge);
	t->bridge_up = true;
	return 0;
}

/* brings the bridge up and initializes the demodulator, once; later calls only wake what slept. t->lock held */
static int torne_hw_up(struct torne_device *t)
{
	int ret;

	if (t->ready)
		return 0;

	dev_dbg(t->dev, "torne_hw_up\n");
	ret = torne_bridge_ensure(t);
	if (ret)
		return ret;
	ret = torne_bridge_reset_demodulator(&t->bridge);
	if (ret) {
		dev_err(t->dev, "demodulator reset failed: %d\n", ret);
		return ret;
	}
	torne_dib_setup(&t->dib, &t->bridge);
	ret = torne_dib_init(&t->dib);
	if (ret) {
		dev_err(t->dev, "demodulator init failed: %d\n", ret);
		return ret;
	}
	t->ready = true;
	return 0;
}

/* ---- stream ---- */

static void torne_urb_complete(struct urb *urb)
{
	struct torne_urb *tu = urb->context;
	struct torne_device *t = tu->dev;
	int ret;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return;
	default:
		dev_dbg(t->dev, "stream urb status %d\n", urb->status);
		break;
	}

	if (urb->status == 0 && urb->actual_length > 0) {
		unsigned long flags;
		size_t n;

		spin_lock_irqsave(&t->stream_lock, flags);
		n = torne_framer_feed(t->framer, tu->buf, urb->actual_length, t->ts_out);
		if (n) {
			ptx_chrdev_put_stream(t->chrdev, t->ts_out, n);
			t->last_data = jiffies;
		}
		spin_unlock_irqrestore(&t->stream_lock, flags);
	}

	if (atomic_read(&t->stream_stop))
		return;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -ENODEV)
		dev_err(t->dev, "usb_submit_urb: %d\n", ret);
}

/* feeds null packets (PID 0x1fff) while the stream is idle; see idle_fill_ms */
static void torne_idle_work(struct work_struct *work)
{
	struct torne_device *t = container_of(to_delayed_work(work), struct torne_device, idle_work);
	unsigned long flags;
	int i;

	if (atomic_read(&t->stream_stop))
		return;

	spin_lock_irqsave(&t->stream_lock, flags);
	if (t->ts_out && time_after(jiffies, t->last_data + msecs_to_jiffies(idle_fill_ms))) {
		for (i = 0; i < TORNE_IDLE_PACKETS; i++) {
			u8 *p = t->ts_out + i * TORNE_TS_PACKET_SIZE;

			p[0] = 0x47;
			p[1] = 0x1f;
			p[2] = 0xff;
			p[3] = 0x10;
			memset(p + 4, 0xff, TORNE_TS_PACKET_SIZE - 4);
		}
		ptx_chrdev_put_stream(t->chrdev, t->ts_out, TORNE_IDLE_PACKETS * TORNE_TS_PACKET_SIZE);
	}
	spin_unlock_irqrestore(&t->stream_lock, flags);

	if (!atomic_read(&t->stream_stop))
		schedule_delayed_work(&t->idle_work, msecs_to_jiffies(TORNE_IDLE_PERIOD_MS));
}

static int torne_start_urbs(struct torne_device *t)
{
	int i, ret;

	if (!t->framer) {
		t->framer = kzalloc(sizeof(*t->framer), GFP_KERNEL);
		t->ts_out = kmalloc(TORNE_URB_SIZE / TORNE_SOURCE_PACKET_SIZE * TORNE_TS_PACKET_SIZE +
					    TORNE_FRAMER_CAP / TORNE_SOURCE_PACKET_SIZE * TORNE_TS_PACKET_SIZE + 4096,
				    GFP_KERNEL);
		if (!t->framer || !t->ts_out) {
			kfree(t->framer);
			kfree(t->ts_out);
			t->framer = NULL;
			t->ts_out = NULL;
			return -ENOMEM;
		}
	}
	torne_framer_reset(t->framer);
	atomic_set(&t->stream_stop, 0);

	for (i = 0; i < TORNE_URB_NUM; i++) {
		struct torne_urb *tu = &t->urbs[i];

		tu->dev = t;
		tu->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!tu->urb) {
			ret = -ENOMEM;
			goto fail;
		}
		tu->buf = usb_alloc_coherent(t->udev, TORNE_URB_SIZE, GFP_KERNEL, &tu->dma);
		if (!tu->buf) {
			ret = -ENOMEM;
			goto fail;
		}
		usb_fill_bulk_urb(tu->urb, t->udev, usb_rcvbulkpipe(t->udev, TORNE_STREAM_EP & 0x0f), tu->buf, TORNE_URB_SIZE,
				  torne_urb_complete, tu);
		tu->urb->transfer_dma = tu->dma;
		tu->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}
	for (i = 0; i < TORNE_URB_NUM; i++) {
		ret = usb_submit_urb(t->urbs[i].urb, GFP_KERNEL);
		if (ret) {
			dev_err(t->dev, "usb_submit_urb: %d\n", ret);
			goto fail;
		}
	}
	t->urbs_running = true;
	t->last_data = jiffies;
	if (idle_fill_ms)
		schedule_delayed_work(&t->idle_work, msecs_to_jiffies(TORNE_IDLE_PERIOD_MS));
	return 0;

fail:
	atomic_set(&t->stream_stop, 1);
	for (i = 0; i < TORNE_URB_NUM; i++) {
		struct torne_urb *tu = &t->urbs[i];

		if (tu->urb) {
			usb_kill_urb(tu->urb);
			usb_free_urb(tu->urb);
			tu->urb = NULL;
		}
		if (tu->buf) {
			usb_free_coherent(t->udev, TORNE_URB_SIZE, tu->buf, tu->dma);
			tu->buf = NULL;
		}
	}
	return ret;
}

static void torne_stop_urbs(struct torne_device *t)
{
	int i;

	if (!t->urbs_running)
		return;
	atomic_set(&t->stream_stop, 1);
	cancel_delayed_work_sync(&t->idle_work);
	for (i = 0; i < TORNE_URB_NUM; i++) {
		struct torne_urb *tu = &t->urbs[i];

		if (tu->urb) {
			usb_kill_urb(tu->urb);
			usb_free_urb(tu->urb);
			tu->urb = NULL;
		}
		if (tu->buf) {
			usb_free_coherent(t->udev, TORNE_URB_SIZE, tu->buf, tu->dma);
			tu->buf = NULL;
		}
	}
	t->urbs_running = false;
}

/* ---- ptx_chrdev operations ---- */

static void torne_device_release(struct kref *kref);

static struct torne_device *chrdev_to_torne(struct ptx_chrdev *chrdev)
{
	return chrdev->priv;
}

static int torne_chrdev_init(struct ptx_chrdev *chrdev)
{
	chrdev->params.system = PTX_ISDB_T_SYSTEM;
	return 0;
}

static int torne_chrdev_term(struct ptx_chrdev *chrdev)
{
	return 0;
}

static int torne_chrdev_open(struct ptx_chrdev *chrdev)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	int ret;

	mutex_lock(&t->lock);
	ret = torne_hw_up(t);
	if (!ret)
		ret = torne_dib_set_gpio(&t->dib, TORNE_LED_PIN, true);
	mutex_unlock(&t->lock);
	if (ret)
		return ret;

	kref_get(&t->kref);
	return 0;
}

static int torne_chrdev_release(struct ptx_chrdev *chrdev)
{
	struct torne_device *t = chrdev_to_torne(chrdev);

	mutex_lock(&t->lock);
	if (atomic_read(&t->available) && t->ready) {
		torne_dib_set_gpio(&t->dib, TORNE_LED_PIN, false);
		torne_dib_sleep(&t->dib);
	}
	mutex_unlock(&t->lock);

	kref_put(&t->kref, torne_device_release);
	return 0;
}

static int torne_chrdev_tune(struct ptx_chrdev *chrdev, struct ptx_tune_params *params)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	int ret;

	if (params->system != PTX_ISDB_T_SYSTEM)
		return -EINVAL;

	mutex_lock(&t->lock);
	if (!atomic_read(&t->available) || !t->ready) {
		mutex_unlock(&t->lock);
		return -ENODEV;
	}
	/* the stream setup runs ahead of the tune, as coax does in its waits */
	ret = torne_bridge_prepare_stream(&t->bridge);
	if (!ret)
		ret = torne_dib_tune(&t->dib, params->freq * 1000u); /* params->freq is in kHz for ISDB-T */
	mutex_unlock(&t->lock);
	if (ret)
		dev_dbg(t->dev, "tune %u kHz failed: %d\n", params->freq, ret);
	return ret;
}

static int torne_chrdev_check_lock(struct ptx_chrdev *chrdev, bool *locked)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	struct torne_dib_status st;
	int ret;

	mutex_lock(&t->lock);
	if (!atomic_read(&t->available) || !t->ready) {
		mutex_unlock(&t->lock);
		return -ENODEV;
	}
	ret = torne_dib_status(&t->dib, &st);
	mutex_unlock(&t->lock);
	if (ret)
		return ret;
	*locked = st.locked;
	return 0;
}

static int torne_chrdev_start_capture(struct ptx_chrdev *chrdev)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	int ret;

	mutex_lock(&t->lock);
	if (!atomic_read(&t->available) || !t->ready) {
		ret = -ENODEV;
		goto out;
	}
	ret = torne_dib_set_output(&t->dib, TORNE_OUT_PARALLEL_GATED);
	if (ret)
		goto out;
	ret = torne_bridge_start_stream(&t->bridge);
	if (ret)
		goto out;
	ret = torne_start_urbs(t);
	if (ret)
		torne_bridge_stop_stream(&t->bridge);
out:
	mutex_unlock(&t->lock);
	return ret;
}

static int torne_chrdev_stop_capture(struct ptx_chrdev *chrdev)
{
	struct torne_device *t = chrdev_to_torne(chrdev);

	mutex_lock(&t->lock);
	torne_stop_urbs(t);
	if (atomic_read(&t->available) && t->ready) {
		torne_bridge_stop_stream(&t->bridge);
		torne_dib_set_output(&t->dib, TORNE_OUT_HIGHZ);
	}
	mutex_unlock(&t->lock);
	return 0;
}

static int torne_chrdev_set_capture(struct ptx_chrdev *chrdev, bool status)
{
	return status ? torne_chrdev_start_capture(chrdev) : torne_chrdev_stop_capture(chrdev);
}

static int torne_chrdev_read_cnr_raw(struct ptx_chrdev *chrdev, u32 *value)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	struct torne_dib_status st;
	int ret;

	mutex_lock(&t->lock);
	if (!atomic_read(&t->available) || !t->ready) {
		mutex_unlock(&t->lock);
		return -ENODEV;
	}
	ret = torne_dib_status(&t->dib, &st);
	mutex_unlock(&t->lock);
	if (ret)
		return ret;
	*value = st.has_cnr && st.cnr_x100 > 0 ? (u32)st.cnr_x100 : 0; /* 0.01 dB */
	return 0;
}

static int torne_chrdev_read_signal_strength(struct ptx_chrdev *chrdev, u32 *value)
{
	struct torne_device *t = chrdev_to_torne(chrdev);
	struct torne_dib_status st;
	int ret;

	mutex_lock(&t->lock);
	if (!atomic_read(&t->available) || !t->ready) {
		mutex_unlock(&t->lock);
		return -ENODEV;
	}
	ret = torne_dib_status(&t->dib, &st);
	mutex_unlock(&t->lock);
	if (ret)
		return ret;
	*value = st.strength_dbm_x1000 > 0 ? 0 : (u32)(-st.strength_dbm_x1000); /* -dBm * 1000 */
	return 0;
}

static struct ptx_chrdev_operations torne_chrdev_ops = {
	.init = torne_chrdev_init,
	.term = torne_chrdev_term,
	.open = torne_chrdev_open,
	.release = torne_chrdev_release,
	.tune = torne_chrdev_tune,
	.check_lock = torne_chrdev_check_lock,
	.set_stream_id = NULL,
	.set_lnb_voltage = NULL,
	.set_capture = torne_chrdev_set_capture,
	.read_signal_strength = torne_chrdev_read_signal_strength,
	.read_cnr = NULL,
	.read_cnr_raw = torne_chrdev_read_cnr_raw,
};

/* ---- card slot (/dev/tornecardN) ---- */

static atomic_t torne_card_seq = ATOMIC_INIT(0);

static struct torne_device *card_file_to_torne(struct file *file)
{
	struct miscdevice *misc = file->private_data;

	return container_of(misc, struct torne_device, misc);
}

static int torne_card_open(struct inode *inode, struct file *file)
{
	struct torne_device *t = card_file_to_torne(file);

	if (!atomic_read(&t->available))
		return -ENODEV;
	kref_get(&t->kref);
	return 0;
}

static int torne_card_release(struct inode *inode, struct file *file)
{
	struct torne_device *t = card_file_to_torne(file);

	kref_put(&t->kref, torne_device_release);
	return 0;
}

static long torne_card_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct torne_device *t = card_file_to_torne(file);
	long ret;

	mutex_lock(&t->lock);
	/* after a disconnect pcscd keeps polling the slot; answer without touching the USB device (and without logging) */
	if (!atomic_read(&t->available)) {
		ret = -ENODEV;
		goto out;
	}
	ret = torne_bridge_ensure(t);
	if (ret)
		goto out;

	switch (cmd) {
	case TORNE_CARD_IOC_PRESENT: {
		bool present;
		int v;

		ret = torne_card_present(&t->card, &present);
		if (ret)
			break;
		v = present ? 1 : 0;
		if (copy_to_user((void __user *)arg, &v, sizeof(v)))
			ret = -EFAULT;
		break;
	}

	case TORNE_CARD_IOC_RESET: {
		struct torne_card_atr atr;
		unsigned int len = 0;

		memset(&atr, 0, sizeof(atr));
		ret = torne_card_reset(&t->card, atr.atr, &len);
		if (ret)
			break;
		atr.len = len;
		if (copy_to_user((void __user *)arg, &atr, sizeof(atr)))
			ret = -EFAULT;
		break;
	}

	case TORNE_CARD_IOC_TRANSMIT: {
		struct torne_card_apdu *apdu = kzalloc(sizeof(*apdu), GFP_KERNEL);
		unsigned int rx_len;

		if (!apdu) {
			ret = -ENOMEM;
			break;
		}
		if (copy_from_user(apdu, (void __user *)arg, sizeof(*apdu))) {
			ret = -EFAULT;
			kfree(apdu);
			break;
		}
		if (apdu->tx_len == 0 || apdu->tx_len > sizeof(apdu->tx)) {
			ret = -EINVAL;
			kfree(apdu);
			break;
		}
		rx_len = sizeof(apdu->rx);
		ret = torne_card_transmit(&t->card, apdu->tx, apdu->tx_len, apdu->rx, &rx_len);
		if (!ret) {
			apdu->rx_len = rx_len;
			if (copy_to_user((void __user *)arg, apdu, sizeof(*apdu)))
				ret = -EFAULT;
		}
		kfree(apdu);
		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}
out:
	mutex_unlock(&t->lock);
	return ret;
}

static const struct file_operations torne_card_fops = {
	.owner = THIS_MODULE,
	.open = torne_card_open,
	.release = torne_card_release,
	.unlocked_ioctl = torne_card_ioctl,
	.compat_ioctl = torne_card_ioctl,
	.llseek = noop_llseek,
};

/* ---- device lifecycle ---- */

static void torne_device_release(struct kref *kref)
{
	struct torne_device *t = container_of(kref, struct torne_device, kref);

	dev_dbg(t->dev, "torne_device_release\n");
	torne_stop_urbs(t);
	kfree(t->framer);
	kfree(t->ts_out);
	kfree(t->ctrl_buf);
	usb_put_dev(t->udev);
	put_device(t->dev);
	complete(t->quit_completion);
}

struct torne_usb_context {
	struct completion quit_completion;
	struct torne_device dev;
};

static int torne_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct device *dev = &intf->dev;
	struct torne_usb_context *ctx;
	struct torne_device *t;
	struct ptx_chrdev_config chrdev_config;
	struct ptx_chrdev_group_config group_config;
	struct ptx_chrdev_group *group;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	init_completion(&ctx->quit_completion);
	t = &ctx->dev;

	get_device(dev);
	kref_init(&t->kref);
	atomic_set(&t->available, 0);
	t->udev = usb_get_dev(interface_to_usbdev(intf));
	t->dev = dev;
	t->quit_completion = &ctx->quit_completion;
	mutex_init(&t->lock);
	spin_lock_init(&t->stream_lock);
	INIT_DELAYED_WORK(&t->idle_work, torne_idle_work);
	t->ctrl_buf = kmalloc(64, GFP_KERNEL);
	if (!t->ctrl_buf) {
		ret = -ENOMEM;
		goto fail_alloc;
	}
	torne_bridge_init(&t->bridge, &torne_kops, t);
	t->bridge.log_level = log_level;

	if (t->udev->speed < USB_SPEED_HIGH)
		dev_warn(dev, "This device is operating as USB 1.1.\n");

	memset(&chrdev_config, 0, sizeof(chrdev_config));
	chrdev_config.system_cap = PTX_ISDB_T_SYSTEM;
	chrdev_config.ops = &torne_chrdev_ops;
	chrdev_config.options = 0;
	chrdev_config.ringbuf_size = 188 * TORNE_RINGBUF_PACKETS;
	chrdev_config.ringbuf_threshold_size = chrdev_config.ringbuf_size / 10;
	chrdev_config.priv = t;

	group_config.owner_kref = &t->kref;
	group_config.owner_kref_release = torne_device_release;
	group_config.reserved = false;
	group_config.minor_base = 0; /* unused */
	group_config.chrdev_num = 1;
	group_config.chrdev_config = &chrdev_config;

	ret = ptx_chrdev_context_add_group(torne_chrdev_ctx, dev, &group_config, &group);
	if (ret)
		goto fail_alloc;

	t->chrdev_group = group;
	t->chrdev = &group->chrdev[0];
	atomic_set(&t->available, 1);

	/* the card slot: /dev/tornecardN */
	snprintf(t->misc_name, sizeof(t->misc_name), "tornecard%d", atomic_inc_return(&torne_card_seq) - 1);
	t->misc.minor = MISC_DYNAMIC_MINOR;
	t->misc.name = t->misc_name;
	t->misc.fops = &torne_card_fops;
	t->misc.mode = 0660;
	if (misc_register(&t->misc))
		dev_warn(dev, "cannot register the card slot device\n");
	else
		t->misc_registered = true;

	usb_set_intfdata(intf, ctx);
	return 0;

fail_alloc:
	kfree(t->ctrl_buf);
	usb_put_dev(t->udev);
	put_device(dev);
	kfree(ctx);
	return ret;
}

static void torne_disconnect(struct usb_interface *intf)
{
	struct torne_usb_context *ctx = usb_get_intfdata(intf);
	struct torne_device *t;

	if (!ctx)
		return;
	t = &ctx->dev;
	usb_set_intfdata(intf, NULL);

	atomic_set(&t->available, 0);
	if (t->misc_registered)
		misc_deregister(&t->misc);
	ptx_chrdev_group_destroy(t->chrdev_group);
	mutex_lock(&t->lock);
	torne_stop_urbs(t);
	mutex_unlock(&t->lock);

	kref_put(&t->kref, torne_device_release);
	wait_for_completion(&ctx->quit_completion);
	kfree(ctx);
}

static int torne_suspend(struct usb_interface *intf, pm_message_t message)
{
	return -ENOSYS;
}

static int torne_resume(struct usb_interface *intf)
{
	return 0;
}

static const struct usb_device_id torne_ids[] = {
	{ USB_DEVICE(TORNE_USB_VENDOR, TORNE_USB_PRODUCT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, torne_ids);

static struct usb_driver torne_usb_driver = {
	.name = "torne_drv",
	.probe = torne_probe,
	.disconnect = torne_disconnect,
	.suspend = torne_suspend,
	.resume = torne_resume,
	.id_table = torne_ids,
};

static int __init torne_init(void)
{
	int ret;

	ret = ptx_chrdev_context_create("torne", "tornevideo", TORNE_MAX_DEVICE, &torne_chrdev_ctx);
	if (ret) {
		pr_err("ptx_chrdev_context_create failed: %d\n", ret);
		return ret;
	}
	ret = usb_register(&torne_usb_driver);
	if (ret) {
		pr_err("usb_register failed: %d\n", ret);
		ptx_chrdev_context_destroy(torne_chrdev_ctx);
		return ret;
	}
	pr_info("Sony CECH-ZD1 J driver loaded\n");
	return 0;
}

static void __exit torne_exit(void)
{
	usb_deregister(&torne_usb_driver);
	ptx_chrdev_context_destroy(torne_chrdev_ctx);
}

module_init(torne_init);
module_exit(torne_exit);

MODULE_AUTHOR("torne-drv");
MODULE_DESCRIPTION("Driver for the Sony CECH-ZD1 J (PS3 'torne') ISDB-T tuner, px4_drv compatible interface");
MODULE_LICENSE("GPL v2");
