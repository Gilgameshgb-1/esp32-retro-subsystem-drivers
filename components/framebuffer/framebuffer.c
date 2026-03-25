#include "framebuffer.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

struct framebuffer {
    uint8_t  *buffers[2];
    int       front;        /* index (0 or 1) of the buffer the display reads */
    uint16_t  width;
    uint16_t  height;
    size_t    size;          /* width * height * 2 (RGB565) */
};

/* Allocate a buffer that DMA can read from.
 * On ESP32: heap_caps_malloc with MALLOC_CAP_DMA ensures the memory
 * lives in internal SRAM accessible by the DMA controller.
 * On host (tests): plain malloc. */
static void *dma_malloc(size_t bytes)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA);
#else
    return malloc(bytes);
#endif
}

static void dma_free(void *ptr)
{
#ifdef ESP_PLATFORM
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
}

framebuffer_t *fb_alloc(uint16_t width, uint16_t height)
{
    framebuffer_t *fb = calloc(1, sizeof(*fb));
    if (!fb) return NULL;

    fb->width  = width;
    fb->height = height;
    fb->size   = (size_t)width * height * 2;
    fb->front  = 0;

    fb->buffers[0] = dma_malloc(fb->size);
    fb->buffers[1] = dma_malloc(fb->size);

    if (!fb->buffers[0] || !fb->buffers[1]) {
        dma_free(fb->buffers[0]);
        dma_free(fb->buffers[1]);
        free(fb);
        return NULL;
    }

    memset(fb->buffers[0], 0, fb->size);
    memset(fb->buffers[1], 0, fb->size);

    return fb;
}

void fb_free(framebuffer_t *fb)
{
    if (!fb) return;
    dma_free(fb->buffers[0]);
    dma_free(fb->buffers[1]);
    free(fb);
}

uint8_t *fb_get_back_buffer(const framebuffer_t *fb)
{
    return fb->buffers[1 - fb->front];
}

const uint8_t *fb_get_front_buffer(const framebuffer_t *fb)
{
    return fb->buffers[fb->front];
}

void fb_swap(framebuffer_t *fb)
{
    fb->front = 1 - fb->front;
}

size_t fb_get_size(const framebuffer_t *fb)
{
    return fb->size;
}

uint16_t fb_get_width(const framebuffer_t *fb)
{
    return fb->width;
}

uint16_t fb_get_height(const framebuffer_t *fb)
{
    return fb->height;
}
