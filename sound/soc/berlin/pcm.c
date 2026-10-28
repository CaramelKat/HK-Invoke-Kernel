/*************************************************************************************
*       Copyright (C) 2007-2011
*       Copyright ? 2007 Marvell International Ltd.
*
*       This program is free software; you can redistribute it and/or
*       modify it under the terms of the GNU General Public License
*       as published by the Free Software Foundation; either version 2
*       of the License, or (at your option) any later version.
*
*       This program is distributed in the hope that it will be useful,
*       but WITHOUT ANY WARRANTY; without even the implied warranty of
*       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*       GNU General Public License for more details.
*
*       You should have received a copy of the GNU General Public License
*       along with this program; if not, write to the Free Software
*       Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
***************************************************************************************/
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/socket.h>
#include <linux/spinlock.h>
#include <linux/file.h>
#include <linux/completion.h>
#include <asm/cacheflush.h>
#include <linux/dma-mapping.h>

#include <sound/core.h>
#include <sound/control.h>
#include <sound/hwdep.h>
#include <sound/info.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/pcm-indirect.h>
#include <sound/rawmidi.h>

#include "berlin_memmap.h"
#include "api_avio_dhub.h"
#include "api_dhub.h"
#include "api_avpll.h"
#include "api_playback.h"
#include "api_capture.h"

#define IRQ_GIC_START          (32)
#define IRQ_dHubIntrAvio1      (0x22)
#define IRQ_DHUBINTRAVIO1      (IRQ_GIC_START + IRQ_dHubIntrAvio1)

enum {
        SNDRV_BERLIN_GET_OUTPUT_MODE = 0x1001,
        SNDRV_BERLIN_SET_OUTPUT_MODE,
        SNDRV_BERLIN_GET_CLOCK_PPM,
        SNDRV_BERLIN_SET_CLOCK_PPM,
};

struct snd_berlin_chip {
        struct snd_card *card;
        struct snd_hwdep *hwdep;
        struct snd_pcm *pcm;
};

static struct snd_card *snd_berlin_card;

extern atomic_t g_output_mode;

static irqreturn_t berlin_devices_aout_isr(int irq, void *dev_id)
{
	struct snd_pcm *pcm = (struct snd_pcm*)dev_id;
	HDL_semaphore *pSemHandle = dhub_semaphore(&AG_dhubHandle.dhub);
	unsigned int instat = semaphore_chk_full(pSemHandle, -1);
	struct snd_pcm_substream *substream;
	unsigned int chanId;

	substream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	chanId = avioDhubChMap_ag_SA0_R_A0;
	if (instat & BIT(chanId)) {
		semaphore_pop(pSemHandle, chanId, 1);
		semaphore_clr_full(pSemHandle, chanId);
		if (substream && substream->runtime)
			snd_berlin_playback_isr(substream);
	}

	substream = pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
	chanId = avioDhubChMap_ag_MIC0_W_A0;
	if (instat & BIT(chanId)) {
		semaphore_pop(pSemHandle, chanId, 1);
		semaphore_clr_full(pSemHandle, chanId);
		if (substream && substream->runtime)
			snd_berlin_capture_isr(substream);
	}

	return IRQ_HANDLED;
}

static struct snd_pcm_ops snd_berlin_playback_ops = {
	.open    = snd_berlin_playback_open,
	.close   = snd_berlin_playback_close,
	.ioctl   = snd_pcm_lib_ioctl,
	.hw_params = snd_berlin_playback_hw_params,
	.hw_free   = snd_berlin_playback_hw_free,
	.prepare = snd_berlin_playback_prepare,
	.trigger = snd_berlin_playback_trigger,
	.pointer = snd_berlin_playback_pointer,
	.ack     = snd_berlin_playback_ack,
};

static struct snd_pcm_ops snd_berlin_capture_ops = {
	.open      = snd_berlin_capture_open,
	.close     = snd_berlin_capture_close,
	.ioctl     = snd_pcm_lib_ioctl,
	.hw_params = snd_berlin_capture_hw_params,
	.hw_free   = snd_berlin_capture_hw_free,
	.prepare   = snd_berlin_capture_prepare,
	.trigger   = snd_berlin_capture_trigger,
	.pointer   = snd_berlin_capture_pointer,
};

static int snd_berlin_card_new_pcm(struct snd_berlin_chip *chip)
{
	struct snd_pcm *pcm;
	int err;

	if ((err =
	snd_pcm_new(chip->card, "Berlin ALSA PCM", 0, 1, 1, &pcm)) < 0)
		return err;

	chip->pcm = pcm;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_berlin_playback_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE,
			&snd_berlin_capture_ops);

	pcm->private_data = chip;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Berlin ALSA PCM");
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					snd_dma_continuous_data(GFP_KERNEL), 32*1024, 64*1024);
	return 0;
}

int snd_berlin_hwdep_dummy_op(struct snd_hwdep *hw, struct file *file)
{
	return 0;
}
EXPORT_SYMBOL_GPL(snd_berlin_hwdep_dummy_op);

static int berlin_set_ppm(double ppm)
{

	double ppm_base, ppm_now;
	if ((ppm < -500) || (ppm > 500))
		return -1;
	AVPLL_GetPPM(&ppm_base, &ppm_now);
	AVPLL_AdjustPPM(ppm_base - ppm_now + ppm);
	return 0;
}

static double berlin_get_ppm(void)
{

	double ppm, ppm_base, ppm_now;
	AVPLL_GetPPM(&ppm_base, &ppm_now);
	ppm = ppm_now - ppm_base;
	return ((int)(ppm >= 0 ? ppm + 0.5: ppm - 0.5));
}

 int snd_berlin_hwdep_ioctl(struct snd_hwdep *hw, struct file *file,
				unsigned int cmd, unsigned long arg)
{
	switch(cmd) {
		case SNDRV_BERLIN_GET_OUTPUT_MODE: {
			unsigned int mode = atomic_read(&g_output_mode);
			int bytes_not_copied =
				copy_to_user((void*)arg, &mode, sizeof(mode));
			return bytes_not_copied == 0 ? 0 : -EFAULT;
		}
		case SNDRV_BERLIN_SET_OUTPUT_MODE: {
			unsigned int mode;
			int bytes_not_copied =
				copy_from_user(&mode, (void*)arg, sizeof(mode));
			if (bytes_not_copied)
				return -EFAULT;
			atomic_set(&g_output_mode, mode);
			return 0;
		}
		case SNDRV_BERLIN_GET_CLOCK_PPM: {
			double ppm = berlin_get_ppm();
			int bytes_not_copied =
				copy_to_user((void*)arg, &ppm, sizeof(ppm));
			return bytes_not_copied == 0 ? 0 : -EFAULT;
		}
		case SNDRV_BERLIN_SET_CLOCK_PPM : {
			double ppm;
			int bytes_not_copied =
				copy_from_user(&ppm, (void*)arg, sizeof(ppm));
			if (bytes_not_copied)
				return -EFAULT;
			return berlin_set_ppm(ppm);
		}
		default:
			return -EINVAL;
	}
}
 EXPORT_SYMBOL_GPL(snd_berlin_hwdep_ioctl);

int snd_berlin_card_new_hwdep(struct snd_berlin_chip *chip)
{
	struct snd_hwdep *hw;
	unsigned int err;

	err = snd_hwdep_new(chip->card, "Berlin hwdep", 0, &hw);
	if (err < 0)
		return err;

	chip->hwdep = hw;
	hw->private_data = chip;
	strcpy(hw->name, "Berlin hwdep interface");

	hw->ops.open = snd_berlin_hwdep_dummy_op;
	hw->ops.ioctl = snd_berlin_hwdep_ioctl;
	hw->ops.ioctl_compat = snd_berlin_hwdep_ioctl;
	hw->ops.release = snd_berlin_hwdep_dummy_op;

	return 0;
}

static void snd_berlin_private_free(struct snd_card *card)
{
	struct snd_berlin_chip *chip = card->private_data;

	kfree(chip);
}

static int snd_berlin_card_init(int dev)
{
	return 0;

	struct snd_berlin_chip *chip;
	unsigned int vec_num;
	char id[16];
	int err;

	snd_printd("berlin snd card is probed\n");
	sprintf(id, "MRVLBERLIN");
	err = snd_card_create(-1, id, THIS_MODULE, 0, &snd_berlin_card);
	if (snd_berlin_card == NULL)
		return -ENOMEM;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL) {
		err = -ENOMEM;
		goto __nodev;
	}

	snd_berlin_card->private_data = chip;
	snd_berlin_card->private_free = snd_berlin_private_free;

	chip->card = snd_berlin_card;

	if ((err = snd_berlin_card_new_pcm(chip)) < 0)
		goto __nodev;

	/* Dhub configuration */
	DhubInitialization(0, AG_DHUB_BASE, AG_HBO_SRAM_BASE,
			&AG_dhubHandle, AG_config, AG_NUM_OF_CHANNELS_A0);
	
	/* register and enable audio out ISR */
	vec_num = IRQ_DHUBINTRAVIO1;
	err = request_irq(vec_num, berlin_devices_aout_isr, IRQF_DISABLED,
		"berlin_module_aout", chip->pcm);
	if (unlikely(err < 0)) {
		snd_printk("irq register error: vec_num:%5d, err:%8x\n", vec_num, err);
		goto __nodev;
	}

	strcpy(snd_berlin_card->driver, "Berlin SoC Alsa");
	strcpy(snd_berlin_card->shortname, "Berlin Alsa");
	sprintf(snd_berlin_card->longname, "Berlin Alsa %i", dev + 1);

	if ((err = snd_berlin_card_new_hwdep(chip)) < 0)
		goto __nodev;

	if ((err = snd_card_register(snd_berlin_card)) != 0)
		goto __nodev;
	else {
		snd_printk("berlin snd card device driver registered\n");
		return 0;
	}

__nodev:
	if (chip)
		kfree(chip);

	snd_card_free(snd_berlin_card);

	return err;
}

static void snd_berlin_card_exit(void)
{
	return 0;

	struct snd_berlin_chip *chip;
	unsigned int vec_num = IRQ_DHUBINTRAVIO1;

	/* unregister audio out ISR */
	chip = (struct snd_berlin_chip *)snd_berlin_card->private_data;
	//free_irq(vec_num, chip->pcm);

	snd_card_free(snd_berlin_card);
}

static int __init snd_berlin_init(void)
{
	return 0;
	snd_printd("snd_berlin_init\n");
	return snd_berlin_card_init(0);
}

static void __exit snd_berlin_exit(void)
{
	return 0;

	snd_printd("snd_berlin_exit\n");
	snd_berlin_card_exit();
}

MODULE_LICENSE("GPL");
module_init(snd_berlin_init);
module_exit(snd_berlin_exit);
