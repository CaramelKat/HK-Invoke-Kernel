/*
 *  Copyright (C) 2012, Analog Devices Inc.
 *	Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#ifndef __SOUND_DMAENGINE_PCM_H__
#define __SOUND_DMAENGINE_PCM_H__

#include <sound/pcm.h>
#include <linux/dmaengine.h>

/**
 * snd_pcm_substream_to_dma_direction - Get dma_transfer_direction for a PCM
 *   substream
 * @substream: PCM substream
 */

#define SND_DMAENGINE_PCM_FLAG_HALF_DUPLEX BIT(3)
#define SND_DMAENGINE_PCM_FLAG_NO_RESIDUE BIT(2)

struct snd_dmaengine_dai_dma_data {
	dma_addr_t addr;
	enum dma_slave_buswidth addr_width;
	u32 maxburst;
	unsigned int slave_id;
	void *filter_data;
};


struct snd_dmaengine_pcm_config {
	int (*prepare_slave_config)(struct snd_pcm_substream *substream,
			struct snd_pcm_hw_params *params,
			struct dma_slave_config *slave_config);
	struct dma_chan *(*compat_request_channel)(
			struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_substream *substream);
	dma_filter_fn compat_filter_fn;

	const struct snd_pcm_hardware *pcm_hardware;
	unsigned int prealloc_buffer_size;
};

static inline enum dma_transfer_direction
snd_pcm_substream_to_dma_direction(const struct snd_pcm_substream *substream)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return DMA_MEM_TO_DEV;
	else
		return DMA_DEV_TO_MEM;
}

void snd_dmaengine_pcm_set_data(struct snd_pcm_substream *substream, void *data);
void *snd_dmaengine_pcm_get_data(struct snd_pcm_substream *substream);

int snd_hwparams_to_dma_slave_config(const struct snd_pcm_substream *substream,
	const struct snd_pcm_hw_params *params, struct dma_slave_config *slave_config);
int snd_dmaengine_pcm_trigger(struct snd_pcm_substream *substream, int cmd);
snd_pcm_uframes_t snd_dmaengine_pcm_pointer(struct snd_pcm_substream *substream);
snd_pcm_uframes_t snd_dmaengine_pcm_pointer_no_residue(struct snd_pcm_substream *substream);

int snd_dmaengine_pcm_open(struct snd_pcm_substream *substream,
	dma_filter_fn filter_fn, void *filter_data);
int snd_dmaengine_pcm_close(struct snd_pcm_substream *substream);

struct dma_chan *snd_dmaengine_pcm_get_chan(struct snd_pcm_substream *substream);

#endif
