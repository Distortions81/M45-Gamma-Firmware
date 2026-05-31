#include "asic_frequency_transition.h"

#include <math.h>

#include "esp_log.h"
#include "global_state.h"

#define ASIC_FREQUENCY_MATCH_EPSILON 0.0001f
#define ASIC_FREQUENCY_STEP_MHZ 6.25f

static const char *TAG = "asic_frequency";

void do_frequency_transition(void *state, set_hash_frequency_fn set_frequency_fn)
{
    if (state == NULL || set_frequency_fn == NULL) {
        return;
    }

    GlobalState *global_state = (GlobalState *)state;
    const float target_frequency = global_state->POWER_MANAGEMENT_MODULE.frequency_value;
    float current_frequency = global_state->POWER_MANAGEMENT_MODULE.actual_frequency;

    if (fabsf(current_frequency - target_frequency) < ASIC_FREQUENCY_MATCH_EPSILON) {
        return;
    }

    if (fabsf(target_frequency - current_frequency) < ASIC_FREQUENCY_STEP_MHZ) {
        global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
            set_frequency_fn(target_frequency);
        return;
    }

    ESP_LOGI(TAG, "Ramping frequency from %g MHz to %g MHz", current_frequency,
             target_frequency);

    int current_step = target_frequency > current_frequency
                           ? (int)floorf(current_frequency / ASIC_FREQUENCY_STEP_MHZ)
                           : (int)ceilf(current_frequency / ASIC_FREQUENCY_STEP_MHZ);
    const int target_step = target_frequency > current_frequency
                                ? (int)floorf(target_frequency / ASIC_FREQUENCY_STEP_MHZ)
                                : (int)ceilf(target_frequency / ASIC_FREQUENCY_STEP_MHZ);

    if (current_step != target_step) {
        const int step_direction = target_frequency > current_frequency ? 1 : -1;

        while ((step_direction > 0 && current_step < target_step) ||
               (step_direction < 0 && current_step > target_step)) {
            current_step += step_direction;
            current_frequency = (float)current_step * ASIC_FREQUENCY_STEP_MHZ;
            global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
                set_frequency_fn(current_frequency);
        }
    }

    if (fabsf(current_frequency - target_frequency) > ASIC_FREQUENCY_MATCH_EPSILON) {
        global_state->POWER_MANAGEMENT_MODULE.actual_frequency =
            set_frequency_fn(target_frequency);
    }

    ESP_LOGI(TAG, "Frequency transition complete at %g MHz", target_frequency);
}
