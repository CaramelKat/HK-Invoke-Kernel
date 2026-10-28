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
#include "api_aio.h"
#include "api_avpll.h"
#include "api_capture.h"

#define DMA_BUFFER_SIZE        (2 * 1024)
#define MAX_BUFFER_SIZE        (32 * 1024)

#if 0
struct snd_berlin_card_pcm {
        /*
         * Driver-specific debug proc entry
         */
        struct snd_info_entry *entry;

        /*
         * Tracks the base address of the last submitted DMA block.
         * Moved to next period in ISR.
         * read in snd_berlin_playback_pointer.
         */
        unsigned int current_dma_offset;

        /*
         * Is there a submitted DMA request?
         * Set when a DMA request is submitted to DHUB.
         * Cleared on 'stop' or ISR.
         */
        bool dma_pending;

        /*
         * Indicates if page memory is allocated
         */
        bool pages_allocated;

        /*
         * Instance lock between ISR and higher contexts.
         */
        spinlock_t   lock;

	/* PCM DMA buffer */
	unsigned char *pcm_dma_area;
	dma_addr_t pcm_dma_addr;
	unsigned int pcm_dma_bytes;

        /* hw parameter */
        unsigned int sample_rate;
        unsigned int sample_format;
        unsigned int channel_num;

        /* playback status */
        bool playing;

        struct snd_pcm_substream *substream;
};
#endif
static void berlin_set_aio_capture(unsigned int sample_rate)
{
        unsigned int div;

        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_DISABLE);
        switch (sample_rate) {
        case 8000  :
        case 11025 :
        case 12000 :
                div = AIO_DIV32;
                break;
        case 16000 :
        case 22050 :
        case 24000 :
                div = AIO_DIV16;
                break;
        case 32000 :
        case 44100 :
        case 48000 :
                div = AIO_DIV8;
                break;
        case 64000 :
        case 88200 :
        case 96000 :
                div = AIO_DIV4;
                break;
        default:
                break;
        }

        AIO_SetClkDiv(AIO_SEC, div);
	AIO_SetClkDiv(AIO_MIC, div);
        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);
	AIO_SetCtl(AIO_MIC, AIO_I2S_MODE, AIO_32CFM, AIO_24DFM);
}

static void berlin_set_pll(unsigned int sample_rate)
{
        int apll;

        switch (sample_rate) {
        case 11025 :
        case 22050 :
        case 44100 :
        case 88200 :
                apll = 22579200;
                break;
        case 8000  :
        case 16000 :
        case 32000 :
        case 64000 :
                apll = 16384000;
                break;
        case 12000 :
        case 24000 :
        case 48000 :
        case 96000 :
                apll = 24576000;
                break;
        default :
                apll = 24576000;
                break;
        }

        AVPLL_Set(0, 3, apll);
        AVPLL_Set(0, 4, apll);
}

void start_dma_if_needed_capture(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        const unsigned int chanId = avioDhubChMap_ag_MIC0_W_A0;
        dma_addr_t dma_source_address;
        int dma_size;

        assert_spin_locked(&berlin_pcm->lock);

        if (!berlin_pcm->playing) {
                snd_printd("%s: playing: %u\n", __func__, berlin_pcm->playing);
                return;
        }

        if (berlin_pcm->dma_pending)
                return;

#if 0
        dma_source_address = runtime->dma_addr +
				berlin_pcm->current_dma_offset;
#else
	dma_source_address = berlin_pcm->pcm_dma_addr +
                                berlin_pcm->current_dma_offset;
#endif

        dma_size = DMA_BUFFER_SIZE;

        berlin_pcm->dma_pending = true;
        dhub_channel_write_cmd(&AG_dhubHandle.dhub, chanId,
                dma_source_address, dma_size, 0, 0, 0, 1, 0, 0);
		
}

void snd_berlin_capture_trigger_start(struct snd_pcm_substream *substream)
{
	
		struct snd_pcm_runtime *runtime = substream->runtime;
		struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        unsigned long flags;
		
        spin_lock_irqsave(&berlin_pcm->lock, flags);
        if (berlin_pcm->playing) {
			    spin_unlock_irqrestore(&berlin_pcm->lock, flags);
                return;
        }

        berlin_pcm->playing = true;
        AIO_SetAudChFlush(AIO_MIC, AIO_TSD0, AUDCH_CTRL_FLUSH_ON);
        AIO_SetAudChFlush(AIO_MIC, AIO_TSD0, AUDCH_CTRL_FLUSH_OFF);
        AIO_SetAudChMute(AIO_MIC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_OFF);
        start_dma_if_needed_capture(substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
		snd_printd("%s: finished.\n", __func__);
}

void snd_berlin_capture_trigger_stop(struct snd_pcm_substream *substream)
{
	
		struct snd_pcm_runtime *runtime = substream->runtime;
		struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        unsigned long flags;
		spin_lock_irqsave(&berlin_pcm->lock, flags);
		AIO_SetAudChMute(AIO_MIC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
        berlin_pcm->playing = false;
        berlin_pcm->dma_pending = false;
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
        snd_printd("%s: finished.\n", __func__);
}

static const struct snd_pcm_hardware snd_berlin_capture_hw = {
	.info = (SNDRV_PCM_INFO_MMAP
		| SNDRV_PCM_INFO_INTERLEAVED
		| SNDRV_PCM_INFO_MMAP_VALID
		| SNDRV_PCM_INFO_PAUSE
		| SNDRV_PCM_INFO_RESUME),
	.formats = (SNDRV_PCM_FMTBIT_S32_LE
		   | SNDRV_PCM_FMTBIT_S16_LE),
	.rates = (SNDRV_PCM_RATE_8000_48000
		| SNDRV_PCM_RATE_KNOT),
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
        .buffer_bytes_max = MAX_BUFFER_SIZE,
        .period_bytes_min = DMA_BUFFER_SIZE,
        .period_bytes_max = DMA_BUFFER_SIZE,
        .periods_min = MAX_BUFFER_SIZE / DMA_BUFFER_SIZE,
        .periods_max = MAX_BUFFER_SIZE / DMA_BUFFER_SIZE,
	.fifo_size = 0
};

void snd_berlin_capture_runtime_free(struct snd_pcm_runtime *runtime)
{
	struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
	if (berlin_pcm)
		kfree(berlin_pcm);
}


int snd_berlin_capture_open(struct snd_pcm_substream *substream)
{
	
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_berlin_card_pcm *berlin_pcm;

	berlin_pcm = kzalloc(sizeof(struct snd_berlin_card_pcm), GFP_KERNEL);
	if (berlin_pcm == NULL)
		return -ENOMEM;

        berlin_pcm->dma_pending = false;
        berlin_pcm->playing = false;
        berlin_pcm->pages_allocated = false;
        berlin_pcm->substream = substream;
        spin_lock_init(&berlin_pcm->lock);

	runtime->private_data = berlin_pcm;
	runtime->private_free = snd_berlin_capture_runtime_free;
	runtime->hw = snd_berlin_capture_hw;
	
	/* enable audio interrupt */
	DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_MIC0_W_A0, 1);

	AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);
	AIO_SetRxPortClkSel(AIO_MIC, Gbl_chipCntl_I2S2_CLK_SEL_LOOP_BACK);
	AIO_SetRxPortEn(AIO_MIC, MIC_RXPORT_ENABLE_ENABLE);
    AIO_SetAudChEn(AIO_MIC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);
    AIO_SetAudChMute(AIO_MIC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
	snd_printd("%s: finished.\n", __func__);
        return 0;
}

int snd_berlin_capture_close(struct snd_pcm_substream *substream)
{
	
	/* disable audio interrupt */
	DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_MIC0_W_A0, 0);
	snd_printd("%s: finished.\n", __func__);
	return 0;
}

int snd_berlin_capture_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
	if (berlin_pcm->pcm_dma_area) {
		dma_free_coherent(NULL, berlin_pcm->pcm_dma_bytes,
				berlin_pcm->pcm_dma_area,
				berlin_pcm->pcm_dma_addr);
		
		berlin_pcm->pcm_dma_area = NULL;
		berlin_pcm->pcm_dma_addr = 0;
	}
    if (berlin_pcm->pages_allocated == true) {
                snd_pcm_lib_free_pages(substream);
                berlin_pcm->pages_allocated = false;
    }
	return 0;
}

int snd_berlin_capture_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
	int err;
	unsigned long flags;

	snd_berlin_capture_hw_free(substream);
	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
	if (err < 0) {
		snd_printk("pcm_lib_malloc failed to allocated pages for buffers\n");
		return err;
	}
	berlin_pcm->pages_allocated = true;

	snd_printk("%s: sample_rate:%d channels:%d format:%s\n", __func__,
                   params_rate(params), params_channels(params),
                   snd_pcm_format_name(params_format(params)));

        berlin_pcm->sample_rate = params_rate(params);
        berlin_pcm->sample_format = params_format(params);
        berlin_pcm->channel_num = params_channels(params);

	berlin_pcm->pcm_dma_bytes = MAX_BUFFER_SIZE;
	berlin_pcm->pcm_dma_area = dma_zalloc_coherent(
			NULL, berlin_pcm->pcm_dma_bytes,
			&berlin_pcm->pcm_dma_addr, GFP_KERNEL);
	if (!berlin_pcm->pcm_dma_area) {
		snd_printk("%s: failed to allocate PCM DMA area\n", __func__);
		goto err_pcm_dma;
	}

        /* AVPLL configuration */
        berlin_set_pll(berlin_pcm->sample_rate);

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        /* AIO configuration */
        berlin_set_aio_capture(berlin_pcm->sample_rate);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

	return 0;

err_pcm_dma:
        snd_pcm_lib_free_pages(substream);
        return -ENOMEM;
}

int snd_berlin_capture_prepare(struct snd_pcm_substream *substream)
{
	
		struct snd_pcm_runtime *runtime = substream->runtime;
		struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
		unsigned long flags;

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        berlin_pcm->current_dma_offset = 0;
		spin_unlock_irqrestore(&berlin_pcm->lock, flags);
		return 0;
}

int snd_berlin_capture_trigger(struct snd_pcm_substream *substream, int cmd)
{
		int  ret = 0;
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			snd_berlin_capture_trigger_start(substream);
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			snd_berlin_capture_trigger_stop(substream);
			break;
		default:
			ret = -EINVAL;
		}
		return ret;
}

snd_pcm_uframes_t
snd_berlin_capture_pointer(struct snd_pcm_substream *substream)
{
		struct snd_pcm_runtime *runtime = substream->runtime;
		struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
		uint32_t buf_pos;
	    unsigned long flags;

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        buf_pos = berlin_pcm->current_dma_offset;
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

		return bytes_to_frames(runtime, buf_pos);
}

int snd_berlin_capture_isr(struct snd_pcm_substream *substream)
{
        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        unsigned long flags;
		
        spin_lock_irqsave(&berlin_pcm->lock, flags);
        /* If we are not running, do not chain, and clear pending */
        if (!berlin_pcm->playing) {
                berlin_pcm->dma_pending = false;
                spin_unlock_irqrestore(&berlin_pcm->lock, flags);
                return IRQ_HANDLED;
        }

        /* If we were not pending, avoid pointer manipulation */
        if (!berlin_pcm->dma_pending) {
                spin_unlock_irqrestore(&berlin_pcm->lock, flags);
                return IRQ_HANDLED;
        }
		
		/* copy data */
		memcpy(runtime->dma_area + berlin_pcm->current_dma_offset,
		berlin_pcm->pcm_dma_area + berlin_pcm->current_dma_offset,
		DMA_BUFFER_SIZE);

        /* Roll the DMA pointer, and chain if needed */
        berlin_pcm->current_dma_offset += DMA_BUFFER_SIZE;
        berlin_pcm->current_dma_offset %= MAX_BUFFER_SIZE;
        berlin_pcm->dma_pending = false;
        start_dma_if_needed_capture(substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
		
        snd_pcm_period_elapsed(substream);

	return 0;
}
