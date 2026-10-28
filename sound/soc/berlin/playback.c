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
#include <sound/soc.h>


#include "berlin_memmap.h"
#include "api_avio_dhub.h"
#include "api_dhub.h"
#include "api_aio.h"
#include "api_spdif.h"
#include "spdif_enc.h"
#include "api_avpll.h"
#include "api_playback.h"
#include "api_capture.h"

void start_dma_if_needed_capture(struct snd_pcm_substream *substream);
void snd_berlin_capture_trigger_start(struct snd_pcm_substream *substream);
void snd_berlin_capture_trigger_stop(struct snd_pcm_substream *substream);
void snd_berlin_capture_runtime_free(struct snd_pcm_runtime *runtime);


enum {
        SND_BERLIN_OUTPUT_ANALOG = 0x200,
        SND_BERLIN_OUTPUT_SPDIF,
};

#define DMA_BUFFER_SIZE        (2 * 1024)
#define MAX_BUFFER_SIZE        (32 * 1024)

#define DMA_BUFFER_SIZE_HD        (2 * 1024)
#define MAX_BUFFER_SIZE_HD        (64* 1024)
 

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

        /* spdif DMA buffer */
        unsigned char *spdif_dma_area; /* dma buffer for spdif output */
        dma_addr_t spdif_dma_addr;   /* physical address of spdif buffer */
        unsigned int spdif_dma_bytes;  /* size of spdif dma area */

        /* PCM DMA buffer */
        unsigned char *pcm_dma_area;
        dma_addr_t pcm_dma_addr;
        unsigned int pcm_dma_bytes;
        unsigned int pcm_virt_bytes;

        /* hw parameter */
        unsigned int sample_rate;
        unsigned int sample_format;
        unsigned int channel_num;
        unsigned int pcm_ratio;
        unsigned int spdif_ratio;

        /* for spdif encoding */
        unsigned int spdif_frames;
        unsigned char channel_status[24];

        /* playback status */
        bool playing;
        unsigned int output_mode;

        struct snd_pcm_substream *substream;
        struct snd_pcm_indirect pcm_indirect;
};

atomic_t g_output_mode = ATOMIC_INIT(SND_BERLIN_OUTPUT_ANALOG);

/*
 * Applies output configuration of |berlin_pcm| to i2s.
 * Must be called with instance spinock held.
 */
static void berlin_set_aio(const struct snd_berlin_card_pcm *berlin_pcm)
{
		unsigned int analog_div, spdif_div;
        assert_spin_locked(&berlin_pcm->lock);

        AIO_SetAudChMute(AIO_SEC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_DISABLE);

        switch (berlin_pcm->sample_rate) {
        case 8000  :
        case 11025 :
        case 12000 :
				#ifdef MCLK12288000
					analog_div = AIO_DIV16;
					spdif_div  = AIO_DIV8;
				#else
	                analog_div = AIO_DIV32;
	                spdif_div  = AIO_DIV16;
				#endif
				break;
        case 16000 :
        case 22050 :
        case 24000 :
				#ifdef MCLK12288000
				    analog_div = AIO_DIV8;
                	spdif_div  = AIO_DIV4;
				#else
	                analog_div = AIO_DIV16;
	                spdif_div  = AIO_DIV8;
				#endif
                break;
        case 32000 :
        case 44100 :
        case 48000 :
				#ifdef MCLK12288000
					analog_div = AIO_DIV4;
                	spdif_div  = AIO_DIV2;
				#else
                	analog_div = AIO_DIV8;
                	spdif_div  = AIO_DIV4;
				#endif
                break;
        case 64000 :
        case 88200 :
        case 96000 :
				#ifdef MCLK12288000
				   	analog_div = AIO_DIV2;
	                spdif_div  = AIO_DIV1;
				#else
	                analog_div = AIO_DIV4;
	                spdif_div  = AIO_DIV2;
				#endif
                break;
        case 176400 :
        case 192000 :
				#ifdef MCLK12288000
	                analog_div = AIO_DIV1;
	                spdif_div  = AIO_DIV1;
				#else
					analog_div = AIO_DIV2;
	                spdif_div  = AIO_DIV1;
				#endif
                break;
        default:
                break;
        }

        if (berlin_pcm->output_mode == SND_BERLIN_OUTPUT_ANALOG) {
                AIO_SetClkDiv(AIO_SEC, analog_div);
#ifndef ENABLE_HD_PLAYBACK
                AIO_SetCtl(AIO_SEC, AIO_I2S_MODE, AIO_32CFM, AIO_24DFM);
#else
				AIO_SetCtl(AIO_SEC, AIO_I2S_MODE, AIO_32CFM, AIO_32DFM);
#endif
        } else if (berlin_pcm->output_mode == SND_BERLIN_OUTPUT_SPDIF) {
                AIO_SetClkDiv(AIO_SEC, spdif_div);
                AIO_SetCtl(AIO_SEC, AIO_I2S_MODE, AIO_32CFM, AIO_32DFM);
        }

        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);

        // Only unmute if we were playing.
        if (berlin_pcm->playing)
                AIO_SetAudChMute(AIO_SEC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_OFF);
}

void berlin_set_aio_capture(unsigned int sample_rate)
{
        unsigned int div;

        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_DISABLE);
        switch (sample_rate) {
        case 8000  :
        case 11025 :
        case 12000 :
				#ifdef MCLK12288000
					div = AIO_DIV16;
				#else
                	div = AIO_DIV32;
				#endif
                break;
        case 16000 :
        case 22050 :
        case 24000 :
				#ifdef MCLK12288000
					div = AIO_DIV8;
				#else
                	div = AIO_DIV16;
				#endif
                break;
        case 32000 :
        case 44100 :
        case 48000 :
				#ifdef MCLK12288000
					div = AIO_DIV4;
				#else
                	div = AIO_DIV8;
				#endif
                break;
        case 64000 :
        case 88200 :
        case 96000 :
				#ifdef MCLK12288000
					div = AIO_DIV2;
				#else
                	div = AIO_DIV4;
				#endif
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
        case 176400:
			#ifdef MCLK12288000
				apll = 11289600;
			#else
                apll = 22579200;
			#endif
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
        case 192000: 
				#ifdef MCLK12288000
					apll = 12288000;
				#else
                	apll = 24576000;
				#endif
                break;
        default :
				#ifdef MCLK12288000
					apll = 12288000;
				#else
                	apll = 24576000;
				#endif
                break;
        }

        AVPLL_Set(0, 3, apll);
        AVPLL_Set(0, 4, apll);
}

/*
 * Kicks off a DMA transfer to audio IO interface for the |berlin_pcm|.
 * Must be called with instance spinock held.
 * Must be called only when instance is in playing state.
 */
static void start_dma_if_needed(struct snd_berlin_card_pcm *berlin_pcm,struct snd_pcm_substream *substream)
{

		unsigned int mode;
        dma_addr_t dma_source_address;
        int dma_size;
		unsigned int chanId;
		assert_spin_locked(&berlin_pcm->lock);

        if (!berlin_pcm->playing) {
                snd_printd("%s: playing: %u\n", __func__, berlin_pcm->playing);
                return;
        }

        if (berlin_pcm->pcm_indirect.hw_ready < DMA_BUFFER_SIZE) {
                snd_printd("%s: hw_ready: %d\n", __func__,
                        berlin_pcm->pcm_indirect.hw_ready);
                return;
        }
	    if (berlin_pcm->dma_pending)
                return;

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
			chanId = avioDhubChMap_ag_SA0_R_A0;
	        mode = atomic_read(&g_output_mode);
	        if (berlin_pcm->output_mode != mode) {
	                berlin_pcm->output_mode = mode;
	                berlin_set_aio(berlin_pcm);
	        }

	        if (mode == SND_BERLIN_OUTPUT_ANALOG) {
	                dma_source_address = berlin_pcm->pcm_dma_addr +
	                        berlin_pcm->current_dma_offset * berlin_pcm->pcm_ratio;
	                dma_size = DMA_BUFFER_SIZE * berlin_pcm->pcm_ratio;
	        } else {
	                dma_source_address = berlin_pcm->spdif_dma_addr +
	                        berlin_pcm->current_dma_offset * berlin_pcm->spdif_ratio;
	                dma_size = DMA_BUFFER_SIZE * berlin_pcm->spdif_ratio;
	        }

	        berlin_pcm->dma_pending = true;
		}
		else
		{
			chanId = avioDhubChMap_ag_MIC0_W_A0;
			#if 0
        		dma_source_address = runtime->dma_addr +
						berlin_pcm->current_dma_offset;
			#else
				dma_source_address = berlin_pcm->pcm_dma_addr +
                                berlin_pcm->current_dma_offset;
			#endif

	        dma_size = DMA_BUFFER_SIZE;

	        berlin_pcm->dma_pending = true;
		}

		dhub_channel_write_cmd(&AG_dhubHandle.dhub, chanId,
                dma_source_address, dma_size, 0, 0, 0, 1, 0, 0);

		
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

static void snd_berlin_playback_trigger_start(struct snd_pcm_substream *substream)
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
        AIO_SetAudChFlush(AIO_SEC, AIO_TSD0, AUDCH_CTRL_FLUSH_ON);
        AIO_SetAudChFlush(AIO_SEC, AIO_TSD0, AUDCH_CTRL_FLUSH_OFF);
        AIO_SetAudChMute(AIO_SEC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_OFF);
        start_dma_if_needed(berlin_pcm,substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
        snd_printd("%s: finished.\n", __func__);
}


static void snd_berlin_playback_trigger_stop(struct snd_pcm_substream *substream)
{
        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        unsigned long flags;

        spin_lock_irqsave(&berlin_pcm->lock, flags);
		
		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
      		AIO_SetAudChMute(AIO_SEC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
		}
		else
		{
			AIO_SetAudChMute(AIO_MIC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
		}
		
        berlin_pcm->playing = false;
        berlin_pcm->dma_pending = false;
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
        snd_printd("%s: finished.\n", __func__);
}

static const struct snd_pcm_hardware snd_berlin_playback_hw = {
        .info = (SNDRV_PCM_INFO_MMAP
                | SNDRV_PCM_INFO_INTERLEAVED
                | SNDRV_PCM_INFO_MMAP_VALID
                | SNDRV_PCM_INFO_PAUSE
                | SNDRV_PCM_INFO_RESUME),
        .formats = (SNDRV_PCM_FMTBIT_S32_LE
                | SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE),
        .rates = (SNDRV_PCM_RATE_8000_192000
                | SNDRV_PCM_RATE_KNOT),
        .rate_min = 8000,
        .rate_max = 192000,
        .channels_min = 1,
        .channels_max = 2,
#ifdef ENABLE_HD_PLAYBACK
        .buffer_bytes_max = MAX_BUFFER_SIZE_HD, //MAX_BUFFER_SIZE, //64 * 1024 ,//MAX_BUFFER_SIZE,  32kb 
        .period_bytes_min = DMA_BUFFER_SIZE, // 2 * 1024 ,
        .period_bytes_max = MAX_BUFFER_SIZE_HD, //DMA_BUFFER_SIZE, //64 * 1024,
        .periods_min = 1, //MAX_BUFFER_SIZE_HD / DMA_BUFFER_SIZE,  //MAX_BUFFER_SIZE / DMA_BUFFER_SIZE, // 1,
        .periods_max = MAX_BUFFER_SIZE_HD/DMA_BUFFER_SIZE, //MAX_BUFFER_SIZE_HD / DMA_BUFFER_SIZE,  //MAX_BUFFER_SIZE / DMA_BUFFER_SIZE, // 128,
#else
	.buffer_bytes_max = MAX_BUFFER_SIZE,
        .period_bytes_min = DMA_BUFFER_SIZE,
        .period_bytes_max = DMA_BUFFER_SIZE,
        .periods_min = MAX_BUFFER_SIZE / DMA_BUFFER_SIZE,
        .periods_max = MAX_BUFFER_SIZE / DMA_BUFFER_SIZE,
#endif
        .fifo_size = 0
};

static void snd_berlin_runtime_free(struct snd_pcm_runtime *runtime)
{
		
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        if (berlin_pcm) {
			
			    if (berlin_pcm->spdif_dma_area) {
					    dma_free_coherent(NULL, berlin_pcm->spdif_dma_bytes,
                        berlin_pcm->spdif_dma_area,
                        berlin_pcm->spdif_dma_addr);
                        berlin_pcm->spdif_dma_area = NULL;
                        berlin_pcm->spdif_dma_addr = 0;
                }

                if (berlin_pcm->pcm_dma_area) {
					    dma_free_coherent(NULL, berlin_pcm->pcm_dma_bytes,
                                        berlin_pcm->pcm_dma_area,
                                        berlin_pcm->pcm_dma_addr);
	                    berlin_pcm->pcm_dma_area = NULL;
                        berlin_pcm->pcm_dma_addr = 0;
                }

                if (berlin_pcm->entry)
                        snd_info_free_entry(berlin_pcm->entry);

				kfree(berlin_pcm);
        }
}

static void debug_entry(struct snd_info_entry *entry,
                        struct snd_info_buffer *buffer)
{
        struct snd_berlin_card_pcm *berlin_pcm =
                (struct snd_berlin_card_pcm *)entry->private_data;
        unsigned long flags;
        spin_lock_irqsave(&berlin_pcm->lock, flags);
        snd_iprintf(buffer, "playing:\t\t%d\n", berlin_pcm->playing);
        snd_iprintf(buffer, "dma_pending:\t\t%d\n", berlin_pcm->dma_pending);
        snd_iprintf(buffer, "output_mode:\t\t%d\n", berlin_pcm->output_mode);
        snd_iprintf(buffer, "current_dma_offset:\t%u\n",
                berlin_pcm->current_dma_offset);
        snd_iprintf(buffer, "\n");
        snd_iprintf(buffer, "indirect.hw_data:\t%u\n",
                berlin_pcm->pcm_indirect.hw_data);
        snd_iprintf(buffer, "indirect.hw_io:\t\t%u\n",
                berlin_pcm->pcm_indirect.hw_io);
        snd_iprintf(buffer, "indirect.hw_ready:\t%d\n",
                berlin_pcm->pcm_indirect.hw_ready);
        snd_iprintf(buffer, "indirect.sw_data:\t%u\n",
                berlin_pcm->pcm_indirect.hw_data);
        snd_iprintf(buffer, "indirect.sw_io:\t\t%u\n",
                berlin_pcm->pcm_indirect.hw_io);
        snd_iprintf(buffer, "indirect.sw_ready:\t%d\n",
                berlin_pcm->pcm_indirect.sw_ready);
        snd_iprintf(buffer, "indirect.appl_ptr:\t%lu\n",
                berlin_pcm->pcm_indirect.appl_ptr);
        snd_iprintf(buffer, "\n");
        snd_iprintf(buffer, "substream->runtime->control->appl_ptr:\t%lu\n",
                berlin_pcm->substream->runtime->control->appl_ptr);
        snd_iprintf(buffer, "substream->runtime->status->hw_ptr:\t%lu\n",
                berlin_pcm->substream->runtime->status->hw_ptr);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
}


void snd_berlin_capture_runtime_free(struct snd_pcm_runtime *runtime)
{
	struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
	if (berlin_pcm)
		kfree(berlin_pcm);
}

int snd_berlin_playback_open(struct snd_pcm_substream *substream)
{

        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm;

		berlin_pcm = kzalloc(sizeof(*berlin_pcm), GFP_KERNEL);
        if (berlin_pcm == NULL)
                return -ENOMEM;

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
	        berlin_pcm->entry = snd_info_create_card_entry(substream->pcm->card,
	                "debug", substream->proc_root);
	        if (!berlin_pcm->entry) {
	                snd_printk("%s: couldn't create debug entry\n", __func__);
	        } else {
	                snd_info_set_text_ops(berlin_pcm->entry, berlin_pcm, debug_entry);
	                snd_info_register(berlin_pcm->entry);
	        }
		}

        berlin_pcm->dma_pending = false;
        berlin_pcm->playing = false;
        berlin_pcm->pages_allocated = false;
		
		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		   berlin_pcm->output_mode = atomic_read(&g_output_mode);
		
		berlin_pcm->substream = substream;
		spin_lock_init(&berlin_pcm->lock);
        runtime->private_data = berlin_pcm;

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
       		runtime->private_free = snd_berlin_runtime_free;
		else
			runtime->private_free = snd_berlin_capture_runtime_free;
		
        runtime->hw = snd_berlin_playback_hw;

		/* enable audio interrupt */
		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			  DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_SA0_R_A0, 1);
		else
			  DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_MIC0_W_A0, 1);
			

        // Enable i2s channel without corresponding disable in close.
        // This is intentional: Avoid SPDIF sink 'activation delay' problems.
        AIO_SetAudChEn(AIO_SEC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			AIO_SetAudChMute(AIO_SEC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
			
		else
		{	
			AIO_SetRxPortClkSel(AIO_MIC, Gbl_chipCntl_I2S2_CLK_SEL_LOOP_BACK);
			AIO_SetRxPortEn(AIO_MIC, MIC_RXPORT_ENABLE_ENABLE);
       		AIO_SetAudChEn(AIO_MIC, AIO_TSD0, AUDCH_CTRL_ENABLE_ENABLE);
        	AIO_SetAudChMute(AIO_MIC, AIO_TSD0, AUDCH_CTRL_MUTE_MUTE_ON);
		}

        snd_printd("%s: finished.\n", __func__);
        return 0;
}

int snd_berlin_playback_close(struct snd_pcm_substream *substream)
{
	/* disable audio interrupt */
	if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_SA0_R_A0, 0);
	else
    	DhubEnableIntr(0, &AG_dhubHandle, avioDhubChMap_ag_MIC0_W_A0, 0);
    	
	snd_printd("%s: finished.\n", __func__);
	return 0;
}

int snd_berlin_playback_hw_free(struct snd_pcm_substream *substream,__attribute__((unused)) struct snd_soc_dai *dai)
{
		
		struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
			if (berlin_pcm->spdif_dma_area) {
	                dma_free_coherent(NULL, berlin_pcm->spdif_dma_bytes,
	                                berlin_pcm->spdif_dma_area,
	                                berlin_pcm->spdif_dma_addr);
	                berlin_pcm->spdif_dma_area = NULL;
	                berlin_pcm->spdif_dma_addr = 0;
	        }
		}
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

	berlin_pcm->pcm_ratio = 1;
	if (berlin_pcm->sample_format == SNDRV_PCM_FORMAT_S16_LE)
				berlin_pcm->pcm_ratio *= 2;
	if (berlin_pcm->channel_num == 1)
				berlin_pcm->pcm_ratio *= 2;

#ifdef ENABLE_HD_PLAYBACK
	berlin_pcm->pcm_dma_bytes = MAX_BUFFER_SIZE_HD * berlin_pcm->pcm_ratio;
#else
	berlin_pcm->pcm_dma_bytes = MAX_BUFFER_SIZE * berlin_pcm->pcm_ratio;
#endif
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


int snd_berlin_playback_hw_params(struct snd_pcm_substream *substream,
                                        struct snd_pcm_hw_params *params,__attribute__((unused)) struct snd_soc_dai *dai)
{

		if(substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		{
			snd_berlin_capture_hw_params(substream,params);
			return 0;
		}

        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        struct spdif_channel_status *chnsts;
        int err;
        unsigned long flags;

        snd_berlin_playback_hw_free(substream,dai);

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

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
		    berlin_pcm->pcm_ratio = 1;
	        if (berlin_pcm->sample_format == SNDRV_PCM_FORMAT_S16_LE)
	                berlin_pcm->pcm_ratio *= 2;
	        if (berlin_pcm->channel_num == 1)
	                berlin_pcm->pcm_ratio *= 2;

	        berlin_pcm->spdif_ratio = berlin_pcm->pcm_ratio * 2;
#ifdef ENABLE_HD_PLAYBACK
	        berlin_pcm->pcm_virt_bytes = MAX_BUFFER_SIZE_HD;
#else
	        berlin_pcm->pcm_virt_bytes = MAX_BUFFER_SIZE;
#endif
		}
		
       
	   berlin_pcm->pcm_dma_bytes = berlin_pcm->pcm_virt_bytes *
                berlin_pcm->pcm_ratio;
        berlin_pcm->pcm_dma_area = dma_zalloc_coherent(
                NULL, berlin_pcm->pcm_dma_bytes,
                &berlin_pcm->pcm_dma_addr, GFP_KERNEL);
        if (!berlin_pcm->pcm_dma_area) {
                snd_printk("%s: failed to allocate PCM DMA area\n", __func__);
                goto err_pcm_dma;
        }

        berlin_pcm->spdif_dma_bytes = berlin_pcm->pcm_virt_bytes *
                berlin_pcm->spdif_ratio;
        berlin_pcm->spdif_dma_area = dma_zalloc_coherent(
                NULL, berlin_pcm->spdif_dma_bytes,
                &berlin_pcm->spdif_dma_addr, GFP_KERNEL);
        if (!berlin_pcm->spdif_dma_area) {
                snd_printk("%s: failed to allocate SPDIF DMA area\n", __func__);
                goto err_spdif_dma;
        }

        /* initialize spdif channel status */
        chnsts = (struct spdif_channel_status *)&(berlin_pcm->channel_status[0]);
        spdif_init_channel_status(chnsts, berlin_pcm->sample_rate);

        /* AVPLL configuration */
        berlin_set_pll(berlin_pcm->sample_rate);

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        /* AIO configuration */
        berlin_set_aio(berlin_pcm);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

        return 0;

err_spdif_dma:
        if (berlin_pcm->pcm_dma_area)
                dma_free_coherent(NULL, berlin_pcm->pcm_dma_bytes,
                        berlin_pcm->pcm_dma_area, berlin_pcm->pcm_dma_addr);
        berlin_pcm->pcm_dma_area = NULL;
        berlin_pcm->pcm_dma_addr = 0;
err_pcm_dma:
        snd_pcm_lib_free_pages(substream);
        return -ENOMEM;
}


int snd_berlin_playback_prepare(struct snd_pcm_substream *substream,__attribute__((unused)) struct snd_soc_dai *dai)
{
        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        unsigned long flags;

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        berlin_pcm->current_dma_offset = 0;
		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
			memset(&berlin_pcm->pcm_indirect, 0, sizeof(berlin_pcm->pcm_indirect));
#ifdef ENABLE_HD_PLAYBACK
	        berlin_pcm->pcm_indirect.hw_buffer_size = MAX_BUFFER_SIZE_HD;
#else
	        berlin_pcm->pcm_indirect.hw_buffer_size = MAX_BUFFER_SIZE;
#endif
	        berlin_pcm->pcm_indirect.sw_buffer_size = snd_pcm_lib_buffer_bytes(substream);
		}
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		{
			snd_printd("%s finished. buffer_bytes: %d period_bytes: %d\n", __func__,
               		 snd_pcm_lib_buffer_bytes(substream),
                	 snd_pcm_lib_period_bytes(substream));
		}
        return 0;
}


int snd_berlin_playback_trigger(struct snd_pcm_substream *substream, int cmd,__attribute__((unused)) struct snd_soc_dai *dai) //,struct snd_soc_dai *dai
{
        int    ret = 0;
		switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
        case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			{
                snd_berlin_playback_trigger_start(substream);
                break;
			}
			else
			{
				snd_berlin_capture_trigger_start(substream);
				break;
			}
        case SNDRV_PCM_TRIGGER_STOP:
        case SNDRV_PCM_TRIGGER_SUSPEND:
        case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
				snd_berlin_playback_trigger_stop(substream);
				break;
			
		default:
                ret = -EINVAL;
        }

        return ret;
}


snd_pcm_uframes_t
snd_berlin_playback_pointer(struct snd_pcm_substream *substream)
{
		struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        uint32_t buf_pos;
        unsigned long flags;

        spin_lock_irqsave(&berlin_pcm->lock, flags);
        buf_pos = berlin_pcm->current_dma_offset;
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

		if(substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			return snd_pcm_indirect_playback_pointer(substream, &berlin_pcm->pcm_indirect,
                                                buf_pos);
		else
			return bytes_to_frames(runtime, buf_pos);
}

// Encodes |frames| number of stereo S32LE frames from |pcm_in|
// to |spdif_out| SPDIF frames (64-bits per frame)
static void spdif_encode(struct snd_berlin_card_pcm *berlin_pcm,
        const int32_t *pcm_in, int32_t *spdif_out, int frames)
{

        int i;
        for (i = 0; i < frames; ++i) {
                unsigned char channel_status =
                        spdif_get_channel_status(berlin_pcm->channel_status,
                                berlin_pcm->spdif_frames);
                unsigned int sync_word =
                        berlin_pcm->spdif_frames ? TYPE_M : TYPE_B;
                spdif_enc_subframe(&spdif_out[i * 4],
                        pcm_in[i * 2], sync_word, 0, 0, channel_status);

                sync_word = TYPE_W;
                spdif_enc_subframe(&spdif_out[(i * 4) + 2],
                        pcm_in[(i * 2) + 1], sync_word, 0, 0, channel_status);

                ++berlin_pcm->spdif_frames;
                berlin_pcm->spdif_frames %= SPDIF_BLOCK_SIZE;
        }
}

static int snd_berlin_playback_copy(struct snd_pcm_substream *substream,
                                int channel, snd_pcm_uframes_t pos,
                                void *buf, size_t bytes)
{

        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        int32_t *pcm_buf = (int32_t *)(berlin_pcm->pcm_dma_area +
                pos * berlin_pcm->pcm_ratio);
        int32_t *spdif_buf = (int32_t *)(berlin_pcm->spdif_dma_area +
                pos * berlin_pcm->spdif_ratio);
        const int frames = bytes /
                (berlin_pcm->channel_num *
                snd_pcm_format_width(berlin_pcm->sample_format) / 8);
        int i;

        if (pos >= berlin_pcm->pcm_virt_bytes)
                return -EINVAL;

        if (berlin_pcm->pcm_indirect.hw_ready) {
                const unsigned long dma_begin = berlin_pcm->current_dma_offset;
                const unsigned long dma_end = dma_begin + DMA_BUFFER_SIZE - 1;
                const unsigned long write_begin = pos;
                const unsigned long write_end = write_begin + bytes - 1;

                // Write begin position shouldn't be in DMA area.
                if ((dma_begin <= write_begin) && (write_begin <= dma_end)) {
                        snd_printk("%s: dma_begin:%lu <= write_begin:%lu "
                                   "<= dma_end:%lu\n",
                                   __func__, dma_begin, write_begin, dma_end);
                }

                // Write end position shouldn't be in DMA area.
                if ((dma_begin <= write_end) && (write_end <= dma_end)) {
                        snd_printk("%s: dma_begin:%lu <= write_end:%lu <= "
                                   "dma_end:%lu\n",
                                   __func__, dma_begin, write_end, dma_end);
                }

                // Write shouldn't overlap DMA area.
                if ((write_begin <= dma_begin) && (write_end >= dma_end)) {
                        snd_printk("%s: write_begin:%lu <= dma_begin:%lu && "
                                   "write_end:%lu >= dma_end:%lu\n",
                                   __func__,  write_begin, dma_begin,
                                   write_end, dma_end);
                }
        }

        if (berlin_pcm->sample_format == SNDRV_PCM_FORMAT_S16_LE) {
                const int16_t *s16_pcm_source = (int16_t *)buf;

                if (berlin_pcm->channel_num == 1) {
                        // Shift sample to 32-bits, and upmix to stereo.
                        for (i = 0; i < frames; ++i) {
                                const int32_t s32_sample = s16_pcm_source[i] << 16;
                                pcm_buf[i * 2] = s32_sample;
                                pcm_buf[(i * 2) + 1] = s32_sample;
                        }
                } else {
                        // Copy left and right samples while shifting to 32-bits.
                        for (i = 0; i < frames; ++i) {
                                pcm_buf[i * 2] =
                                        s16_pcm_source[i * 2] << 16;
                                pcm_buf[(i * 2) + 1] =
                                        s16_pcm_source[(i * 2) + 1] << 16;
                        }
                }
        } else if (berlin_pcm->sample_format == SNDRV_PCM_FORMAT_S32_LE) {
                const int32_t *s32_pcm_source = (int32_t *)buf;

                if (berlin_pcm->channel_num == 1) {
                        // Upmix each sample to stereo.
                        for (i = 0; i < frames; ++i) {
                                pcm_buf[i * 2] = s32_pcm_source[i];
                                pcm_buf[(i * 2) + 1] = s32_pcm_source[i];
                        }
                } else {
                        // Copy the left and right samples straight over.
                        for (i = 0; i < frames; ++i) {
                                pcm_buf[i * 2] = s32_pcm_source[i * 2];
                                pcm_buf[(i * 2) + 1] = s32_pcm_source[(i * 2) + 1];
                        }
                }
        } else {
                return -EINVAL;
        }

        spdif_encode(berlin_pcm, pcm_buf, spdif_buf, frames);
        return 0;
}

static void berlin_playback_transfer(struct snd_pcm_substream *substream,
                                        struct snd_pcm_indirect *rec, size_t bytes)
{

        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        void *src = (void *)(runtime->dma_area + rec->sw_data);
        if (!src)
                return;

        snd_berlin_playback_copy(substream, berlin_pcm->channel_num,
                rec->hw_data, src, bytes);
}

int snd_berlin_playback_ack(struct snd_pcm_substream *substream)
{
        struct snd_pcm_runtime *runtime = substream->runtime;
        struct snd_berlin_card_pcm *berlin_pcm = runtime->private_data;
        struct snd_pcm_indirect *pcm_indirect = &berlin_pcm->pcm_indirect;
        unsigned long flags;
        pcm_indirect->hw_queue_size = runtime->hw.buffer_bytes_max;
        snd_pcm_indirect_playback_transfer(substream, pcm_indirect,
                                                berlin_playback_transfer);
        spin_lock_irqsave(&berlin_pcm->lock, flags);
        start_dma_if_needed(berlin_pcm,substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
        return 0;
}



int snd_berlin_playback_isr(struct snd_pcm_substream *substream)
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

        /* Roll the DMA pointer, and chain if needed */
        berlin_pcm->current_dma_offset += DMA_BUFFER_SIZE;
        berlin_pcm->current_dma_offset %= berlin_pcm->pcm_virt_bytes;
        berlin_pcm->dma_pending = false;
        start_dma_if_needed(berlin_pcm,substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);

        snd_pcm_period_elapsed(substream);
	return 0;
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
#ifdef ENABLE_HD_PLAYBACK
        berlin_pcm->current_dma_offset %= MAX_BUFFER_SIZE_HD;
#else
        berlin_pcm->current_dma_offset %= MAX_BUFFER_SIZE;
#endif
        berlin_pcm->dma_pending = false;
        start_dma_if_needed_capture(substream);
        spin_unlock_irqrestore(&berlin_pcm->lock, flags);
		
        snd_pcm_period_elapsed(substream);

	return 0;
}


