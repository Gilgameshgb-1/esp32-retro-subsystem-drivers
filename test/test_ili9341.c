#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "esp_idf_mock.h"
#include "ili9341.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name)                                          \
    do {                                                    \
        tests_run++;                                        \
        printf("  %-50s ", #name);                          \
    } while (0)

#define PASS()                                              \
    do {                                                    \
        tests_passed++;                                     \
        printf("[PASS]\n");                                 \
    } while (0)

#define ASSERT_EQ(a, b)                                     \
    do {                                                    \
        if ((a) != (b)) {                                   \
            printf("[FAIL]  %s:%d: %lld != %lld\n",         \
                   __FILE__, __LINE__,                      \
                   (long long)(a), (long long)(b));         \
            return;                                         \
        }                                                   \
    } while (0)

#define ASSERT_TRUE(cond)                                   \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("[FAIL]  %s:%d: condition false\n",      \
                   __FILE__, __LINE__);                     \
            return;                                         \
        }                                                   \
    } while (0)

/* -----------------------------------------------------------------------
 * Helper: init the driver so internal state (spi handle, pins) is valid
 * ----------------------------------------------------------------------- */
static void init_driver(void)
{
    mock_reset();
    ili9341_config_t cfg = {
        .pin_mosi     = 23,
        .pin_clk      = 18,
        .pin_cs       =  5,
        .pin_dc       =  2,
        .pin_rst      =  4,
        .spi_clock_hz = 26000000,
    };
    ili9341_init(&cfg);
    /* Reset counters after init so tests only see their own calls */
    mock_reset();
}

/* -----------------------------------------------------------------------
 * Tests: ili9341_rgb565
 * ----------------------------------------------------------------------- */
static void test_rgb565_pure_red(void)
{
    TEST(rgb565_pure_red);
    uint16_t c = ili9341_rgb565(255, 0, 0);
    /* Red: 11111 000000 00000 = 0xF800 */
    ASSERT_EQ(c, 0xF800);
    PASS();
}

static void test_rgb565_pure_green(void)
{
    TEST(rgb565_pure_green);
    uint16_t c = ili9341_rgb565(0, 255, 0);
    /* Green: 00000 111111 00000 = 0x07E0 */
    ASSERT_EQ(c, 0x07E0);
    PASS();
}

static void test_rgb565_pure_blue(void)
{
    TEST(rgb565_pure_blue);
    uint16_t c = ili9341_rgb565(0, 0, 255);
    /* Blue: 00000 000000 11111 = 0x001F */
    ASSERT_EQ(c, 0x001F);
    PASS();
}

static void test_rgb565_white(void)
{
    TEST(rgb565_white);
    uint16_t c = ili9341_rgb565(255, 255, 255);
    ASSERT_EQ(c, 0xFFFF);
    PASS();
}

static void test_rgb565_black(void)
{
    TEST(rgb565_black);
    uint16_t c = ili9341_rgb565(0, 0, 0);
    ASSERT_EQ(c, 0x0000);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: draw_image DMA queue behavior
 * ----------------------------------------------------------------------- */
static void test_draw_image_transaction_count(void)
{
    TEST(draw_image_transaction_count);
    init_driver();

    /* Small 4x3 image = 12 pixels = 24 bytes */
    uint8_t fake_img[4 * 3 * 2];
    memset(fake_img, 0xAA, sizeof(fake_img));

    ili9341_draw_image(0, 0, 4, 3, fake_img);

    /* Should queue exactly 3 transactions (one per row) */
    ASSERT_EQ(g_mock.queued_count, 3);
    PASS();
}

static void test_draw_image_transaction_lengths(void)
{
    TEST(draw_image_transaction_lengths);
    init_driver();

    uint8_t fake_img[10 * 5 * 2];
    memset(fake_img, 0, sizeof(fake_img));

    ili9341_draw_image(0, 0, 10, 5, fake_img);

    /* Each row: 10 pixels * 2 bytes * 8 bits = 160 bits */
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(g_mock.queued[i].length, 160);
    }
    PASS();
}

static void test_draw_image_buffer_pointers(void)
{
    TEST(draw_image_buffer_pointers);
    init_driver();

    uint8_t fake_img[8 * 4 * 2];
    memset(fake_img, 0, sizeof(fake_img));

    ili9341_draw_image(0, 0, 8, 4, fake_img);

    int row_bytes = 8 * 2;
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ((uintptr_t)g_mock.queued[i].tx_buffer,
                  (uintptr_t)(fake_img + i * row_bytes));
    }
    PASS();
}

static void test_draw_image_drains_queue(void)
{
    TEST(draw_image_drains_queue);
    init_driver();

    uint8_t fake_img[4 * 3 * 2];
    memset(fake_img, 0, sizeof(fake_img));

    ili9341_draw_image(0, 0, 4, 3, fake_img);

    /* 3 rows queued, 3 results drained -- every queue_trans has a matching get_result */
    ASSERT_EQ(g_mock.queued_count, g_mock.get_result_count);
    PASS();
}

static void test_draw_image_uses_queue_not_polling(void)
{
    TEST(draw_image_uses_queue_not_polling);
    init_driver();

    uint8_t fake_img[4 * 2 * 2];
    memset(fake_img, 0, sizeof(fake_img));

    ili9341_draw_image(0, 0, 4, 2, fake_img);

    /* Pixel data should use queue, not polling */
    ASSERT_TRUE(g_mock.queued_count > 0);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: draw_image with more rows than queue depth (ring wrapping)
 * ----------------------------------------------------------------------- */
static void test_draw_image_exceeds_queue_depth(void)
{
    TEST(draw_image_exceeds_queue_depth);
    init_driver();

    /* 20 rows -- forces the ring to wrap multiple times (depth is 7) */
    uint8_t fake_img[4 * 20 * 2];
    memset(fake_img, 0, sizeof(fake_img));

    ili9341_draw_image(0, 0, 4, 20, fake_img);

    /* All 20 rows must be queued */
    ASSERT_EQ(g_mock.queued_count, 20);
    /* All 20 must be drained */
    ASSERT_EQ(g_mock.get_result_count, 20);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: fill_rect
 * ----------------------------------------------------------------------- */
static void test_fill_rect_transaction_count(void)
{
    TEST(fill_rect_transaction_count);
    init_driver();

    ili9341_fill_rect(0, 0, 10, 15, 0xF800);

    /* 15 rows queued */
    ASSERT_EQ(g_mock.queued_count, 15);
    PASS();
}

static void test_fill_screen_transaction_count(void)
{
    TEST(fill_screen_transaction_count);
    init_driver();

    ili9341_fill_screen(0x0000);

    /* Default orientation: 320 rows */
    ASSERT_EQ(g_mock.queued_count, 320);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: init uses polling (not queue) for commands
 * ----------------------------------------------------------------------- */
static void test_init_uses_polling_for_commands(void)
{
    TEST(init_uses_polling_for_commands);
    mock_reset();

    ili9341_config_t cfg = {
        .pin_mosi = 23, .pin_clk = 18, .pin_cs = 5,
        .pin_dc = 2, .pin_rst = 4, .spi_clock_hz = 26000000,
    };
    ili9341_init(&cfg);

    /* Init sends commands via polling, never via queue */
    ASSERT_TRUE(g_mock.polled_count > 0);
    ASSERT_EQ(g_mock.queued_count, 0);
    PASS();
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("\n=== ILI9341 Driver Tests ===\n\n");

    /* rgb565 */
    test_rgb565_pure_red();
    test_rgb565_pure_green();
    test_rgb565_pure_blue();
    test_rgb565_white();
    test_rgb565_black();

    /* draw_image */
    test_draw_image_transaction_count();
    test_draw_image_transaction_lengths();
    test_draw_image_buffer_pointers();
    test_draw_image_drains_queue();
    test_draw_image_uses_queue_not_polling();
    test_draw_image_exceeds_queue_depth();

    /* fill_rect */
    test_fill_rect_transaction_count();
    test_fill_screen_transaction_count();

    /* init */
    test_init_uses_polling_for_commands();

    printf("\n=== Results: %d/%d passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
