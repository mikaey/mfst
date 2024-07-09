#include <stdlib.h>
#include <string.h>

#include "rng.h"

static struct random_data random_state;
static char random_number_state_buf[256];
static unsigned int current_seed;

void rng_init(unsigned int seed) {
    current_seed = seed;
    memset(random_number_state_buf, 0, sizeof(random_number_state_buf));
    initstate_r(seed, random_number_state_buf, sizeof(random_number_state_buf), &random_state);
}

void rng_reseed(unsigned int seed) {
    current_seed = seed;
    srandom_r(seed, &random_state);
}

int32_t rng_get_random_number() {
    int32_t result;
    random_r(&random_state, &result);

    // random() and random_r() generate random numbers between 0 and
    // 0x7FFFFFFF -- which means we're not testing 1 out of every 32 bits on
    // the device if we just accept this value.  To spice things up a little,
    // we'll throw some extra randomness into the top bit.
    result |= ((current_seed & result & 0x00000001) | (~(current_seed & (result >> 1)) & 0x00000001)) << 31;
    return result;
}

void rng_fill_buffer(char *buffer, uint64_t size) {
    int32_t *int_buffer = (int32_t *) buffer;
    uint64_t i;
    for(i = 0; i < (size / 4); i++) {
        int_buffer[i] = rng_get_random_number();
    }
}
