#ifndef M45_ASIC_PLL_H_
#define M45_ASIC_PLL_H_

#include <stdint.h>

#define FREQ_MULT 25.0f

void pll_get_parameters(float target_freq, uint16_t fb_divider_min, uint16_t fb_divider_max,
                        uint8_t *fb_divider, uint8_t *refdiv, uint8_t *postdiv1,
                        uint8_t *postdiv2, float *actual_freq);

#endif
