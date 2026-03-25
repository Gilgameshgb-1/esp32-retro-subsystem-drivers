#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "esp_idf_mock.h"
#include "audio.h"

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

static void init_audio(void)
{
    mock_reset();
    audio_config_t cfg = {
        .pin_din     = 25,
        .pin_bclk    = 26,
        .pin_lrc     = 27,
        .sample_rate = 44100,
    };
    audio_init(&cfg);
    mock_reset();
}

/* -----------------------------------------------------------------------
 * Tests: init
 * ----------------------------------------------------------------------- */
static void test_init_creates_channel(void)
{
    TEST(init_creates_channel);
    mock_reset();
    audio_config_t cfg = {
        .pin_din = 25, .pin_bclk = 26, .pin_lrc = 27,
        .sample_rate = 44100,
    };
    audio_init(&cfg);
    ASSERT_EQ(g_mock.i2s_init_count, 1);
    PASS();
}

static void test_init_sets_sample_rate(void)
{
    TEST(init_sets_sample_rate);
    mock_reset();
    audio_config_t cfg = {
        .pin_din = 25, .pin_bclk = 26, .pin_lrc = 27,
        .sample_rate = 16000,
    };
    audio_init(&cfg);
    ASSERT_EQ(g_mock.i2s_sample_rate, 16000);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: write
 * ----------------------------------------------------------------------- */
static void test_write_sends_bytes(void)
{
    TEST(write_sends_bytes);
    init_audio();

    int16_t samples[100];
    audio_write(samples, sizeof(samples));

    ASSERT_EQ(g_mock.i2s_bytes_written, sizeof(samples));
    ASSERT_EQ(g_mock.i2s_write_count, 1);
    PASS();
}

/* -----------------------------------------------------------------------
 * Tests: play_tone
 * ----------------------------------------------------------------------- */
static void test_tone_writes_correct_duration(void)
{
    TEST(tone_writes_correct_duration);
    init_audio();

    /* 440 Hz tone for 1000ms at 44100 Hz sample rate
     * samples_per_period = 44100 / 440 = 100 (truncated)
     * total_samples = 44100 * 1000 / 1000 = 44100
     * periods = 44100 / 100 = 441
     * Each period write = 100 samples * 2 bytes = 200 bytes
     * Total = 441 * 200 = 88200 bytes */
    audio_play_tone(440, 1000, 0.5f);

    int samples_per_period = 44100 / 440;
    int total_samples = (44100 * 1000) / 1000;
    int periods = total_samples / samples_per_period;
    size_t expected_bytes = (size_t)periods * samples_per_period * sizeof(int16_t);

    ASSERT_EQ(g_mock.i2s_bytes_written, expected_bytes);
    ASSERT_EQ(g_mock.i2s_write_count, periods);
    PASS();
}

static void test_tone_short_duration(void)
{
    TEST(tone_short_duration);
    init_audio();

    audio_play_tone(1000, 100, 0.3f);

    /* Should have written some data */
    ASSERT_TRUE(g_mock.i2s_bytes_written > 0);
    ASSERT_TRUE(g_mock.i2s_write_count > 0);
    PASS();
}

static void test_tone_zero_volume_still_writes(void)
{
    TEST(tone_zero_volume_still_writes);
    init_audio();

    audio_play_tone(440, 100, 0.0f);

    /* Even at 0 volume, the function still sends silent samples */
    ASSERT_TRUE(g_mock.i2s_write_count > 0);
    PASS();
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("\n=== Audio Driver Tests ===\n\n");

    test_init_creates_channel();
    test_init_sets_sample_rate();
    test_write_sends_bytes();
    test_tone_writes_correct_duration();
    test_tone_short_duration();
    test_tone_zero_volume_still_writes();

    printf("\n=== Results: %d/%d passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
