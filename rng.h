#if !defined(RNG_H)
#define RNG_H

#include <inttypes.h>

#include "device_testing_context.h"

/**
 * Resets the random number generator and gives it the given seed.
 * 
 * @param device_testing_context  The device whose RNG should be initialized.
 * @param seed                    The seed value to provide to the RNG.
 */
void rng_init(device_testing_context_type *device_testing_context, unsigned int seed);

/**
 * Reseeds the random number generator with the given seed, but without
 * resetting the RNG's state.
 *
 * @param device_testing_context  The device whose RNG should be seeded with the
 *                                given seed value.
 * @param seed                    The seed to provide to the RNG.
 */
void rng_reseed(device_testing_context_type *device_testing_context, unsigned int seed);

/**
 * Obtains a random number from the random number generator.  Since random()
 * only generates 31 bits of random data, this function randomizes the uppermost
 * bit of the result.
 * 
 * @param device_testing_context  The device whose RNG should be used to
 *                                generate the random number.
 *
 * @returns The generated random number.
*/
int32_t rng_get_random_number(device_testing_context_type *device_testing_context);

/**
 * Fills `buffer` with `size` random bytes.
 *
 * @param device_testing_context  The device whose RNG should be used to
 *                                generate random bytes for the buffer.
 * @param buffer                  A pointer to the buffer to be populated with
 *                                random bytes.
 * 
 * @param size  The number of bytes to write to the buffer.  Must be a multiple
 *              of 4 -- if it isn't, it will be rounded up to the next multiple
 *              of 4.
*/
void rng_fill_buffer(device_testing_context_type *device_testing_context, char *buffer, size_t size);

#endif // !defined(RNG_H)
