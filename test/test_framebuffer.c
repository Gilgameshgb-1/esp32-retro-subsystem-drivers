#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framebuffer.h"

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

#define ASSERT_NOT_NULL(ptr)                                \
    do {                                                    \
        if ((ptr) == NULL) {                                \
            printf("[FAIL]  %s:%d: pointer is NULL\n",      \
                   __FILE__, __LINE__);                     \
            return;                                         \
        }                                                   \
    } while (0)

/* -----------------------------------------------------------------------
 * Tests: allocation and dimensions
 * ----------------------------------------------------------------------- */
static void test_alloc_returns_valid(void)
{
    TEST(alloc_returns_valid);
    framebuffer_t *fb = fb_alloc(240, 320);
    ASSERT_NOT_NULL(fb);
    fb_free(fb);
    PASS();
}

static void test_dimensions_match(void)
{
    TEST(dimensions_match);
    framebuffer_t *fb = fb_alloc(240, 320);
    ASSERT_EQ(fb_get_width(fb),  240);
    ASSERT_EQ(fb_get_height(fb), 320);
    ASSERT_EQ(fb_get_size(fb),   240 * 320 * 2);
    fb_free(fb);
    PASS();
}

static void test_small_dimensions(void)
{
    TEST(small_dimensions);
    framebuffer_t *fb = fb_alloc(10, 5);
    ASSERT_EQ(fb_get_width(fb),  10);
    ASSERT_EQ(fb_get_height(fb), 5);
    ASSERT_EQ(fb_get_size(fb),   100);
    fb_free(fb);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: buffer pointers
 * ----------------------------------------------------------------------- */
static void test_front_and_back_are_different(void)
{
    TEST(front_and_back_are_different);
    framebuffer_t *fb = fb_alloc(10, 10);
    const uint8_t *front = fb_get_front_buffer(fb);
    uint8_t       *back  = fb_get_back_buffer(fb);
    ASSERT_TRUE(front != back);
    fb_free(fb);
    PASS();
}

static void test_buffers_initialized_to_zero(void)
{
    TEST(buffers_initialized_to_zero);
    framebuffer_t *fb = fb_alloc(10, 10);
    const uint8_t *front = fb_get_front_buffer(fb);
    uint8_t       *back  = fb_get_back_buffer(fb);
    size_t sz = fb_get_size(fb);

    for (size_t i = 0; i < sz; i++) {
        ASSERT_EQ(front[i], 0);
        ASSERT_EQ(back[i], 0);
    }
    fb_free(fb);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: swap behavior
 * ----------------------------------------------------------------------- */
static void test_swap_exchanges_buffers(void)
{
    TEST(swap_exchanges_buffers);
    framebuffer_t *fb = fb_alloc(10, 10);

    const uint8_t *front_before = fb_get_front_buffer(fb);
    uint8_t       *back_before  = fb_get_back_buffer(fb);

    fb_swap(fb);

    const uint8_t *front_after = fb_get_front_buffer(fb);
    uint8_t       *back_after  = fb_get_back_buffer(fb);

    /* Old back is now front, old front is now back */
    ASSERT_TRUE(front_after == back_before);
    ASSERT_TRUE(back_after  == (uint8_t *)front_before);
    fb_free(fb);
    PASS();
}

static void test_double_swap_restores(void)
{
    TEST(double_swap_restores);
    framebuffer_t *fb = fb_alloc(10, 10);

    const uint8_t *original_front = fb_get_front_buffer(fb);
    const uint8_t *original_back  = fb_get_back_buffer(fb);

    fb_swap(fb);
    fb_swap(fb);

    ASSERT_TRUE(fb_get_front_buffer(fb) == original_front);
    ASSERT_TRUE(fb_get_back_buffer(fb)  == (uint8_t *)original_back);
    fb_free(fb);
    PASS();
}

static void test_write_to_back_then_swap(void)
{
    TEST(write_to_back_then_swap);
    framebuffer_t *fb = fb_alloc(4, 2);

    /* Write a known pattern into the back buffer */
    uint8_t *back = fb_get_back_buffer(fb);
    size_t sz = fb_get_size(fb);
    memset(back, 0xAB, sz);

    /* Front should still be zeroed */
    const uint8_t *front = fb_get_front_buffer(fb);
    for (size_t i = 0; i < sz; i++) {
        ASSERT_EQ(front[i], 0);
    }

    /* Swap -- the pattern is now in front */
    fb_swap(fb);
    front = fb_get_front_buffer(fb);
    for (size_t i = 0; i < sz; i++) {
        ASSERT_EQ(front[i], 0xAB);
    }

    /* New back (old front) is still zeroed */
    back = fb_get_back_buffer(fb);
    for (size_t i = 0; i < sz; i++) {
        ASSERT_EQ(back[i], 0);
    }

    fb_free(fb);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: free handles NULL
 * ----------------------------------------------------------------------- */
static void test_free_null_is_safe(void)
{
    TEST(free_null_is_safe);
    fb_free(NULL);  /* should not crash */
    PASS();
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("\n=== Framebuffer Tests ===\n\n");

    test_alloc_returns_valid();
    test_dimensions_match();
    test_small_dimensions();
    test_front_and_back_are_different();
    test_buffers_initialized_to_zero();
    test_swap_exchanges_buffers();
    test_double_swap_restores();
    test_write_to_back_then_swap();
    test_free_null_is_safe();

    printf("\n=== Results: %d/%d passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
