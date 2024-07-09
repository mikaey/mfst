#if !defined(RNG_H)
#define RNG_H

#include <inttypes.h>

/**
 * Resets the random number generator and gives it the given seed.
 * 
 * @param seed  The seed to provide to the random number generator.
 */
void rng_init(unsigned int seed);

/**
 * Reseeds the random number generator with the given seed, but without
 * resetting the RNG's state.
 *
 * @param seed  The seed to provide to the random number generator.
 */
void rng_reseed(unsigned int seed);

/**
 * Obtains a random number from the random number generator.  Since random()
 * only generates 31 bits of random data, this function randomizes the uppermost
 * bit of the result.
 * 
 * @returns The generated random number.
*/
int32_t rng_get_random_number();

/**
 * Fills `buffer` with `size` random bytes.
 * 
 * @param buffer  A pointer to the buffer to be populated with random bytes.
 * 
 * @param size    The number of bytes to write to the buffer.  Must be a
 *                multiple of 4 -- if it isn't, it will be rounded up to the
 *                next multiple of 4.
*/
void rng_fill_buffer(char *buffer, size_t size);

#endif // !defined(RNG_H)
