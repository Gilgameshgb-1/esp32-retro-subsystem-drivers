#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Opaque double-buffered framebuffer.
 *
 * Two equally sized pixel buffers share a single "front" index.
 *   - Front buffer: currently being read by the display / DMA.
 *   - Back  buffer: being filled by the producer (network, test pattern, etc.).
 *
 * Call fb_swap() once the back buffer is fully written to promote it to front.
 *
 * Memory is allocated with DMA capability on ESP32 so the SPI DMA engine
 * can read directly from either buffer.
 */
typedef struct framebuffer framebuffer_t;

/**
 * @brief Allocate a double-buffered framebuffer.
 *
 * Each buffer is width * height * 2 bytes (RGB565).
 * On ESP32 the memory is allocated with MALLOC_CAP_DMA.
 *
 * @return Pointer to the framebuffer, or NULL if allocation fails.
 */
framebuffer_t *fb_alloc(uint16_t width, uint16_t height);

/**
 * @brief Free the framebuffer and both internal buffers.
 */
void fb_free(framebuffer_t *fb);

/**
 * @brief Get a writable pointer to the back buffer.
 *        Write your next frame here.
 */
uint8_t *fb_get_back_buffer(const framebuffer_t *fb);

/**
 * @brief Get a read-only pointer to the front buffer.
 *        The display task reads from here.
 */
const uint8_t *fb_get_front_buffer(const framebuffer_t *fb);

/**
 * @brief Swap front and back buffers.
 *        After this call the old back buffer becomes the new front
 *        and vice versa.
 */
void fb_swap(framebuffer_t *fb);

/**
 * @brief Buffer size in bytes (width * height * 2).
 */
size_t fb_get_size(const framebuffer_t *fb);

uint16_t fb_get_width(const framebuffer_t *fb);
uint16_t fb_get_height(const framebuffer_t *fb);
