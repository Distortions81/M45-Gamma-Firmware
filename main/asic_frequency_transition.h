#ifndef M45_ASIC_FREQUENCY_TRANSITION_H_
#define M45_ASIC_FREQUENCY_TRANSITION_H_

typedef float (*set_hash_frequency_fn)(float frequency_mhz);

void do_frequency_transition(void *state, set_hash_frequency_fn set_frequency_fn);

#endif
