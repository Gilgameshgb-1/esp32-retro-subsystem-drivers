#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int pin_din;
    int pin_bclk;
    int pin_lrc;
    int sample_rate;
} audio_config_t;

/**
 * @brief  Initialise the I2S peripheral for mono PCM output.
 *
 * Allocates the I2S TX channel, configures it in Philips standard mode
 * (16-bit, mono) at the sample rate given in @p config, and enables the
 * channel.  Must be called once before any call to audio_write().
 *
 * @param config  Pin assignments and sample rate.  Must not be NULL.
 */
void audio_init(const audio_config_t *config);

/**
 * @brief  Write raw 16-bit mono PCM samples to the I2S DMA output.
 *
 * Blocks until every byte in @p data has been handed to the DMA engine.
 * Uses portMAX_DELAY internally, so the caller will block when the DMA
 * descriptors are full — rely on this to implement natural flow control
 * when feeding data from a TCP receive loop.
 *
 * @param data  Pointer to 16-bit signed PCM samples (little-endian).
 * @param len   Number of bytes to write (must be a multiple of 2).
 */
void audio_write(const int16_t *data, size_t len);
