// SPDX-License-Identifier: GPL-2.0-only
/*
 * RME Babyface Pro FS — proprietary-mode USB audio driver
 * Card lifecycle: probe/disconnect, PM, module entry.  See snd-usb-babyface-pro.h for the shared state.
 */
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <sound/control.h>
#include <sound/tlv.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "snd-usb-babyface-pro.h"

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static int frames_per_urb = BF_FRAMES_PER_URB_DEFAULT;
static int nurbs = BF_NURBS_DEFAULT;
static int panel_poll_ms = BF_PANEL_POLL_MS_DEFAULT;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for the Babyface Pro FS sound card.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for the Babyface Pro FS sound card.");
module_param(frames_per_urb, int, 0644);
MODULE_PARM_DESC(frames_per_urb, "Audio frames per URB, 8..1024 (16 = low-latency floor, 256 = default).");
module_param(nurbs, int, 0644);
MODULE_PARM_DESC(nurbs, "URBs in flight per direction, 1..16 (16 = low-latency).");
module_param(panel_poll_ms, int, 0644);
MODULE_PARM_DESC(panel_poll_ms, "Front-panel poll interval in ms, 10..1000 (20 = default, matches Windows' ~50 Hz).");

/* ── USB driver ────────────────────────────────────────────── */

static void babyface_private_free(struct snd_card *card)
{
	struct snd_usb_babyface *chip = card->private_data;
	unsigned int urbsize;
	int i;

	if (!chip)
		return;

	/* The URB arrays are NULL when the probe failed before allocating
	 * them (snd_card_free runs private_free on any probe error).
	 */
	if (chip->urbs_in) {
		urbsize = chip->frame_bytes * chip->frames_per_urb;
		for (i = 0; i < chip->nurbs; i++) {
			if (chip->urbs_in[i]) {
				usb_kill_urb(chip->urbs_in[i]);
				usb_free_urb(chip->urbs_in[i]);
			}
			if (chip->urbs_out[i]) {
				usb_kill_urb(chip->urbs_out[i]);
				usb_free_urb(chip->urbs_out[i]);
			}
			usb_free_coherent(chip->dev, urbsize, chip->buf_in[i],
					  chip->dma_in[i]);
			usb_free_coherent(chip->dev, urbsize, chip->buf_out[i],
					  chip->dma_out[i]);
		}
	}
	kfree(chip->urbs_in);
	kfree(chip->urbs_out);
	kfree(chip->buf_in);
	kfree(chip->buf_out);
	kfree(chip->dma_in);
	kfree(chip->dma_out);
	usb_put_dev(chip->dev);
}

static int babyface_probe(struct usb_interface *intf,
			  const struct usb_device_id *usb_id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct snd_usb_babyface *chip;
	struct snd_card *card;
	struct snd_pcm *pcm;
	unsigned int urbsize;
	u8 st[4];
	int i, err;

	if (intf->cur_altsetting->desc.bInterfaceNumber != BF_IFACE) {
		/* Only the proprietary audio interface is ours; the MIDI
		 * (standard class) and bulk interfaces stay unclaimed so
		 * snd-usb-audio can take the MIDI one.
		 */
		return -ENODEV;
	}

	frames_per_urb = clamp(frames_per_urb, 8, 1024) & ~7;
	nurbs = clamp(nurbs, 1, 16);
	panel_poll_ms = clamp(panel_poll_ms, 10, 1000);

	err = snd_card_new(&intf->dev, index[0], id[0], THIS_MODULE,
			   sizeof(*chip), &card);
	if (err < 0) {
		dev_err(&intf->dev, "snd_card_new failed: %d\n", err);
		return err;
	}
	chip = card->private_data;
	chip->card = card;

	chip->dev = usb_get_dev(dev);
	chip->iface = intf;
	chip->nurbs = nurbs;
	chip->frames_per_urb = frames_per_urb;
	chip->panel_poll_ms = panel_poll_ms;
	chip->rate = 48000;
	chip->alt = BF_ALT_1;
	chip->frame_bytes = 56;
	chip->preamp = BF_PREAMP_BASE;
	mutex_init(&chip->mutex);
	spin_lock_init(&chip->lock);
	atomic_set(&chip->urb_err, 0);
	INIT_WORK(&chip->stream_work, babyface_stream_work);
	INIT_DELAYED_WORK(&chip->panel_work, babyface_panel_work);
	chip->card->private_free = babyface_private_free;

	strscpy(chip->card->driver, "BabyfaceProFS",
		sizeof(chip->card->driver));
	strscpy(chip->card->shortname, "Babyface Pro FS",
		sizeof(chip->card->shortname));
	snprintf(chip->card->longname, sizeof(chip->card->longname),
		 "RME Babyface Pro FS (proprietary mode) at %s",
		 dev_name(&dev->dev));
	strscpy(chip->card->mixername, "Babyface Pro FS",
		sizeof(chip->card->mixername));

	/* alt 1 = the default 48-kHz bandwidth class. */
	err = usb_set_interface(dev, BF_IFACE, BF_ALT_1);
	if (err < 0) {
		dev_err(&intf->dev, "usb_set_interface failed: %d\n", err);
		goto error;
	}

	err = bf_cold_init(chip);
	if (err < 0) {
		dev_err(&intf->dev, "cold init failed: %d\n", err);
		goto error;
	}

	/* Sync the preamp state from the 0x17 readback (byte 0 mirrors
	 * the 48V/PAD bits; it persists across power cycles).
	 */
	err = bf_vendor_read(chip, BF_REQ_PREAMP, BF_REG_PREAMP, st);
	if (err < 0)
		dev_dbg(&intf->dev, "preamp readback failed: %d\n", err);
	else
		chip->preamp = st[0];

	/* Restore the mixer state saved at the last disconnect (if any);
	 * the device keeps its registers across a usbfs detach, but the
	 * cold init above cleared them, so push the user's settings back.
	 */
	err = bf_state_restore(chip);
	if (err == -ENOENT) {
		/* No saved state: the 0x16 clear zeroed the mixer registers,
		 * so restore the factory default routing to keep the outputs
		 * live out of the box.
		 */
		err = babyface_write_default_mixer(chip);
		if (err < 0) {
			dev_err(&intf->dev, "default mixer restore failed: %d\n", err);
			goto error;
		}
	} else if (err < 0) {
		dev_err(&intf->dev, "mixer state restore failed: %d\n", err);
		goto error;
	}

	urbsize = chip->frame_bytes * chip->frames_per_urb;
	chip->urbs_in = kcalloc(chip->nurbs, sizeof(*chip->urbs_in), GFP_KERNEL);
	chip->urbs_out = kcalloc(chip->nurbs, sizeof(*chip->urbs_out), GFP_KERNEL);
	chip->buf_in = kcalloc(chip->nurbs, sizeof(*chip->buf_in), GFP_KERNEL);
	chip->buf_out = kcalloc(chip->nurbs, sizeof(*chip->buf_out), GFP_KERNEL);
	chip->dma_in = kcalloc(chip->nurbs, sizeof(*chip->dma_in), GFP_KERNEL);
	chip->dma_out = kcalloc(chip->nurbs, sizeof(*chip->dma_out), GFP_KERNEL);
	if (!chip->urbs_in || !chip->urbs_out || !chip->buf_in ||
	    !chip->buf_out || !chip->dma_in || !chip->dma_out)
		goto error;

	for (i = 0; i < chip->nurbs; i++) {
		chip->urbs_in[i] = usb_alloc_urb(0, GFP_KERNEL);
		chip->urbs_out[i] = usb_alloc_urb(0, GFP_KERNEL);
		chip->buf_in[i] = usb_alloc_coherent(dev, urbsize, GFP_KERNEL,
						     &chip->dma_in[i]);
		chip->buf_out[i] = usb_alloc_coherent(dev, urbsize, GFP_KERNEL,
						      &chip->dma_out[i]);
		if (!chip->urbs_in[i] || !chip->urbs_out[i] ||
		    !chip->buf_in[i] || !chip->buf_out[i])
			goto error;
	}

	err = snd_pcm_new(chip->card, "Babyface Pro FS", 0, 1, 1, &pcm);
	if (err < 0) {
		dev_err(&intf->dev, "snd_pcm_new failed: %d\n", err);
		goto error;
	}
	pcm->private_data = chip;
	strscpy(pcm->name, "Babyface Pro FS", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &babyface_pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &babyface_pcm_ops);

	/* The PCM buffer is host-side (the URB callbacks copy in/out of
	 * it); vmalloc is the standard choice for that.
	 */
	err = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC,
					      NULL, 0, 1 << 20);
	if (err < 0) {
		dev_err(&intf->dev, "buffer allocation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_controls(chip);
	if (err < 0) {
		dev_err(&intf->dev, "control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_xpoints(chip);
	if (err < 0) {
		dev_err(&intf->dev, "crosspoint creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_flags(chip);
	if (err < 0) {
		dev_err(&intf->dev, "flag control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_panel(chip);
	if (err < 0) {
		dev_err(&intf->dev, "front-panel control creation failed: %d\n", err);
		goto error;
	}

	err = babyface_create_eq(chip);
	if (err < 0) {
		dev_err(&intf->dev, "EQ control creation failed: %d\n", err);
		goto error;
	}

	/* The DSP coefficient stream (EQ, bulk ep 0x0A) lives on interface
	 * 1, which has a single altsetting (alt 0) already active in the
	 * default configuration — the endpoint is scheduled, no
	 * SET_INTERFACE or interface claim is needed (the earlier
	 * -EAGAIN was the on-stack transfer buffer, and SET_INTERFACE on
	 * interface 1 wedged the iface-5 audio stream — playback URBs
	 * never completed).
	 */

	err = snd_card_register(chip->card);
	if (err < 0) {
		dev_err(&intf->dev, "snd_card_register failed: %d\n", err);
		goto error;
	}

	/* The panel poll mirrors the physical buttons/wheel into the
	 * Front Panel controls; it runs for the whole card lifetime.
	 */
	babyface_panel_start(chip);

	usb_set_intfdata(intf, chip);
	dev_info(&intf->dev,
		 "Babyface Pro FS: card %i, %u frames/URB, %u URBs/direction\n",
		 chip->card->number, chip->frames_per_urb, chip->nurbs);
	return 0;

error:
	usb_set_intfdata(intf, NULL);
	snd_card_free(chip->card);
	return err;
}

static void babyface_disconnect(struct usb_interface *intf)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);

	if (!chip)
		return;

	/* Idempotence guard: a disconnect can race a re-probe (usbfs
	 * detach/re-attach) — tear the card down exactly once.
	 */
	usb_set_intfdata(intf, NULL);
	if (chip->shutdown)
		return;

	/* Keep the mixer state for the next probe: a userspace usbfs
	 * claim (PipeWire sink grab, TuxMix daemon) detaches us and the
	 * cold init of the re-probe would otherwise wipe the settings.
	 */
	bf_state_save(chip);

	chip->shutdown = true;
	cancel_work_sync(&chip->stream_work);
	babyface_panel_stop(chip);
	/* Wake apps blocked in read/write: the card is going away. */
	dev_info(&chip->dev->dev, "disconnect: stopping PCM substreams\n");
	babyface_pcm_stop_both(chip, SNDRV_PCM_STATE_DISCONNECTED);
	mutex_lock(&chip->mutex);
	if (chip->streaming)
		babyface_stream_kill(chip);
	mutex_unlock(&chip->mutex);

	snd_card_disconnect(chip->card);
	/* NEVER snd_card_free() here: it blocks until the last user
	 * closes the card, and an open client (e.g. PipeWire) deadlocks
	 * the disconnect (seen live: pipewire stuck in snd_card_free,
	 * D state).  free_when_closed frees on the last close.
	 */
	snd_card_free_when_closed(chip->card);
}

static int babyface_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);

	struct snd_device *sdev;

	if (!chip)
		return 0;
	list_for_each_entry(sdev, &chip->card->devices, list) {
		if (sdev->type == SNDRV_DEV_PCM)
			snd_pcm_suspend_all(sdev->device_data);
	}
	cancel_work_sync(&chip->stream_work);
	babyface_panel_stop(chip);
	mutex_lock(&chip->mutex);
	if (chip->streaming)
		babyface_stream_kill(chip);
	mutex_unlock(&chip->mutex);
	return 0;
}

static int babyface_resume(struct usb_interface *intf)
{
	struct snd_usb_babyface *chip = usb_get_intfdata(intf);
	int err;

	if (!chip)
		return 0;

	/* The device lost its state across the suspend; re-run the cold
	 * init and re-apply the cached mixer state.  Suspended PCM
	 * substreams are woken by the core — apps get -ESTRPIPE and
	 * restart (the trigger re-arms the stream).
	 */
	mutex_lock(&chip->mutex);
	err = usb_set_interface(chip->dev, BF_IFACE, chip->alt);
	if (err < 0)
		goto out;
	err = bf_cold_init(chip);
	if (err < 0)
		goto out;
	err = babyface_restore_state(chip);
out:
	mutex_unlock(&chip->mutex);
	if (!err)
		babyface_panel_start(chip);
	return err;
}

static const struct usb_device_id babyface_ids[] = {
	{ USB_DEVICE(USB_VENDOR_RME, USB_PRODUCT_BABYFACE_PRO_FS) },
	{ }
};
MODULE_DEVICE_TABLE(usb, babyface_ids);

static struct usb_driver babyface_driver = {
	.name = "snd-usb-babyface-pro",
	.probe = babyface_probe,
	.disconnect = babyface_disconnect,
	.suspend = babyface_suspend,
	.resume = babyface_resume,
	.id_table = babyface_ids,
};

static int __init babyface_init(void)
{
	return usb_register(&babyface_driver);
}

static void __exit babyface_exit(void)
{
	bf_state_purge();
	usb_deregister(&babyface_driver);
}

module_init(babyface_init);
module_exit(babyface_exit);

MODULE_AUTHOR("Ismaïl Bahloul <iswadlillah@gmail.com>");
MODULE_DESCRIPTION("RME Babyface Pro FS (proprietary mode) USB audio driver");
MODULE_LICENSE("GPL");
