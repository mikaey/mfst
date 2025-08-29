#include <stdlib.h>
#include <string.h>

#include "device_testing_context.h"
#include "rng.h"

void rng_init(device_testing_context_type *device_testing_context, unsigned int seed) {
    device_testing_context->endurance_test_info.rng_state.current_seed = seed;
    memset(device_testing_context->endurance_test_info.rng_state.rng_state_buf, 0, sizeof(device_testing_context->endurance_test_info.rng_state.rng_state_buf));
    initstate_r(seed, device_testing_context->endurance_test_info.rng_state.rng_state_buf, sizeof(device_testing_context->endurance_test_info.rng_state.rng_state_buf), &device_testing_context->endurance_test_info.rng_state.rng_state);
}

void rng_reseed(device_testing_context_type *device_testing_context, unsigned int seed) {
    device_testing_context->endurance_test_info.rng_state.current_seed = seed;
    srandom_r(seed, &device_testing_context->endurance_test_info.rng_state.rng_state);
}

int32_t rng_get_random_number(device_testing_context_type *device_testing_context) {
    int32_t result;
    random_r(&device_testing_context->endurance_test_info.rng_state.rng_state, &result);

    // random() and random_r() generate random numbers between 0 and
    // 0x7FFFFFFF -- which means we're not testing 1 out of every 32 bits on
    // the device if we just accept this value.  To spice things up a little,
    // we'll throw some extra randomness into the top bit.
    result |= ((device_testing_context->endurance_test_info.rng_state.current_seed & result & 0x00000001) | (~(device_testing_context->endurance_test_info.rng_state.current_seed & (result >> 1)) & 0x00000001)) << 31;
    return result;
}

void rng_fill_buffer(device_testing_context_type *device_testing_context, char *buffer, uint64_t size) {
    int32_t *int_buffer = (int32_t *) buffer;
    uint64_t i;
    for(i = 0; i < (size / 4); i++) {
        int_buffer[i] = rng_get_random_number(device_testing_context);
    }
}
