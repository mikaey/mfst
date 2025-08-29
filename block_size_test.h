#if !defined(BLOCK_SIZE_TEST_H)
#define BLOCK_SIZE_TEST_H

#include "device_testing_context.h"

/**
 * Probe the device to determine the optimal size of write requests.
 *
 * The test involves writing 256MB of data to the device in a sequential
 * fashion using various block sizes from 512 bytes to 64MB.  Block sizes that
 * are smaller than the device's optimal block size (as returned by the fstat()
 * call) and bigger than the maximum number of sectors per request (as returned
 * by ioctl(BLKSECTGET) call) are excluded.  The amount of time it takes to
 * write the data using each block size is measured.
 *
 * @param fd  The file descriptor of the device being measured.
 *
 * @returns The block size which the device was able to write the quickest, in
 *          bytes.
 */
int probe_for_optimal_block_size(device_testing_context_type *device_testing_context);

#endif // !defined(BLOCK_SIZE_TEST_H)
