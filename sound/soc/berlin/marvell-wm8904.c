/*
 * Copyright (C) 2009, Lars-Peter Clausen <lars@metafoo.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include "../codecs/wm8904.h"
#include <linux/slab.h>

#include "berlin_memmap.h"
#include "api_avio_dhub.h"
#include "api_dhub.h"
#include "api_avpll.h"
#include "api_playback.h"
#include "api_capture.h"

#define FLASH_SIZE 0x10000
#define Sector_ID_Size 4
#define NOT_USED_AREA 0xffff
#define ENV_DATA_VALID 0xFE
#define CENV_HeaderSize 2
#define FENV_HeaderSize 1

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

int v_Mclk1 = 0,v_BtMasterClk1;

static const struct snd_soc_dapm_widget marvell_wm8904_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route marvell_wm8904_routes[] = {
	{"Speaker", NULL, "HP_L"},
	{"Speaker", NULL, "HP_R"},
};

irqreturn_t berlin_devices_aout_isr(int irq, void *dev_id)
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
EXPORT_SYMBOL_GPL(berlin_devices_aout_isr);

#define marvell_DAIFMT (SND_SOC_DAIFMT_I2S | \
			SND_SOC_DAIFMT_NB_NF | \
			SND_SOC_DAIFMT_CBS_CFS)

static int marvell_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	return 0;
}

static int marvell_wm8904_codec_init(struct snd_soc_pcm_runtime *rtd)  //to do remove this
{
	return 0;
}

static struct snd_soc_ops marvell_soc_ops = {
	.hw_params = marvell_hw_params,
};


static struct snd_soc_dai_link marvell_wm8904_dai = {
	.name = "marvell",
	.stream_name = "marvell",
	.init = marvell_wm8904_codec_init,
	.codec_dai_name = "wm8904-hifi",
	.dai_fmt = SND_SOC_DAIFMT_I2S
		| SND_SOC_DAIFMT_NB_NF
	    | SND_SOC_DAIFMT_CBS_CFS, //WM8904 is Bclk & LrClk master
	.ops = &marvell_soc_ops,
};

static struct snd_soc_card marvell_wm8904 = {
	.name = "marvell-wm8904",
	.owner = THIS_MODULE,
	.dai_link = &marvell_wm8904_dai,
	.num_links = 1,

	.dapm_widgets = marvell_wm8904_widgets,
	.num_dapm_widgets = ARRAY_SIZE(marvell_wm8904_widgets),
	.dapm_routes = marvell_wm8904_routes,
	.num_dapm_routes = ARRAY_SIZE(marvell_wm8904_routes),
};

static int marvell_wm8904_probe(struct platform_device *pdev)
{


	struct device_node *np = pdev->dev.of_node;
	struct snd_soc_card *card = &marvell_wm8904;
	int ret;
 	int v1_status;
    unsigned char *String=(char*)kmalloc(20,__GFP_WAIT | __GFP_IO | __GFP_FS);

	printk("Libre :marvell_wm8904_probe called\n");

	/* Dhub configuration */
	DhubInitialization(0, AG_DHUB_BASE, AG_HBO_SRAM_BASE,
				&AG_dhubHandle, AG_config, AG_NUM_OF_CHANNELS_A0);

    card->dev = &pdev->dev;

	marvell_wm8904_dai.cpu_of_node = of_parse_phandle(np, "marvell,ssc-controller", 0);
	marvell_wm8904_dai.codec_of_node = of_parse_phandle(np, "marvell,audio-codec", 0);
	marvell_wm8904_dai.platform_of_node = marvell_wm8904_dai.cpu_of_node;
	
	ret = snd_soc_of_parse_audio_routing(card, "atmel,audio-routing");
	if (ret) {
		printk( "Anoop failed to parse audio routing\n");
		return ret;
	}
		
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card() failed: %d\n",
			ret);
	}
	
	return ret;
}

static int marvell_wm8904_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);
	return 0;
}

static const struct of_device_id marvell_audio_match[] = {
	{ .compatible = "marvell,wm8904-audio" },
	{},
};


static struct platform_driver marvell_wm8904_driver = {
	.driver		= {
		.name	= "wm8904-audio",
		.owner	= THIS_MODULE,
		.of_match_table = marvell_audio_match,
	},
	.probe		= marvell_wm8904_probe,
	.remove		= marvell_wm8904_remove,
};	
MODULE_DEVICE_TABLE(of, marvell_audio_match);
module_platform_driver(marvell_wm8904_driver);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("ALSA SoC QI LB60 Audio support");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qi-lb60-audio");
