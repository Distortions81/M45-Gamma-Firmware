#include "asic_pll.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>

#include "esp_log.h"

#define PLL_MATCH_EPSILON 0.0001f

static const char *TAG = "asic_pll";

void pll_get_parameters(float target_freq, uint16_t fb_divider_min, uint16_t fb_divider_max,
                        uint8_t *fb_divider, uint8_t *refdiv, uint8_t *postdiv1,
                        uint8_t *postdiv2, float *actual_freq)
{
    if (fb_divider == NULL || refdiv == NULL || postdiv1 == NULL || postdiv2 == NULL ||
        actual_freq == NULL) {
        return;
    }

    float best_freq = 0.0f;
    uint8_t best_refdiv = 0;
    uint8_t best_fb_divider = 0;
    uint8_t best_postdiv1 = 0;
    uint8_t best_postdiv2 = 0;
    float min_diff = FLT_MAX;
    float min_vco_freq = FLT_MAX;
    uint16_t min_postdiv = UINT16_MAX;

    for (uint8_t candidate_refdiv = 2; candidate_refdiv > 0; --candidate_refdiv) {
        for (uint8_t candidate_postdiv1 = 7; candidate_postdiv1 > 0; --candidate_postdiv1) {
            for (uint8_t candidate_postdiv2 = 7; candidate_postdiv2 > 0; --candidate_postdiv2) {
                if (candidate_postdiv1 <= candidate_postdiv2) {
                    continue;
                }

                const uint16_t divider =
                    (uint16_t)(candidate_refdiv * candidate_postdiv1 * candidate_postdiv2);
                for (uint16_t candidate_fb_divider = fb_divider_min;
                     candidate_fb_divider <= fb_divider_max; ++candidate_fb_divider) {
                    const float candidate_freq =
                        FREQ_MULT * (float)candidate_fb_divider / (float)divider;
                    const float candidate_diff = fabsf(target_freq - candidate_freq);
                    const float candidate_vco_freq =
                        FREQ_MULT * (float)candidate_fb_divider / (float)candidate_refdiv;
                    const uint16_t candidate_postdiv =
                        (uint16_t)(candidate_postdiv1 * candidate_postdiv2);

                    const bool better_frequency = candidate_diff < min_diff;
                    const bool equal_frequency =
                        fabsf(candidate_diff - min_diff) < PLL_MATCH_EPSILON;
                    const bool better_vco = candidate_vco_freq < min_vco_freq;
                    const bool equal_vco =
                        fabsf(candidate_vco_freq - min_vco_freq) < PLL_MATCH_EPSILON;
                    if (better_frequency || (equal_frequency && better_vco) ||
                        (equal_frequency && equal_vco && candidate_postdiv < min_postdiv)) {
                        min_diff = candidate_diff;
                        min_vco_freq = candidate_vco_freq;
                        min_postdiv = candidate_postdiv;
                        best_freq = candidate_freq;
                        best_refdiv = candidate_refdiv;
                        best_fb_divider = (uint8_t)candidate_fb_divider;
                        best_postdiv1 = candidate_postdiv1;
                        best_postdiv2 = candidate_postdiv2;
                    }
                }
            }
        }
    }

    ESP_LOGI(TAG, "Frequency: %g MHz (fb_divider: %u, refdiv: %u, postdiv1: %u, postdiv2: %u)",
             best_freq, best_fb_divider, best_refdiv, best_postdiv1, best_postdiv2);

    *actual_freq = best_freq;
    *fb_divider = best_fb_divider;
    *refdiv = best_refdiv;
    *postdiv1 = best_postdiv1;
    *postdiv2 = best_postdiv2;
}
