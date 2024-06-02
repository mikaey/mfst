#if !defined(UTIL_H)
#define UTIL_H

#include <sys/time.h>

/**
 * Formats `rate` as a string describing a byte rate (e.g., "50 b/s",
 * "1.44 MB/s", etc.) and places it in the buffer pointed to by `buf`.  `buf`
 * should be big enough to hold the resulting string (which should be 13 bytes
 * or less, including the null terminator, in situations where `rate` is less
 * than 10 * 1024^5).  The function will write `buf_size` bytes, at most, to
 * `buf`.  The function will return a pointer to `buf` so that it can be easily
 * used, say, as an argument to a `printf()` call.
 * 
 * @param rate      The rate to be described.
 * @param buf       The buffer which will hold the resulting string.
 * @param buf_size  The maximum number of bytes that should be written to
 *                  `buf`.
 * 
 * @returns A pointer to `buf`.
*/
char *format_rate(double rate, char *buf, size_t buf_size);

/**
 * Calculates the difference between two given struct timeval values.
 * 
 * @param start_time  The beginning time value.
 * @param end_time    The ending time value.  This value should be greater
 *                    than `start_time`.
 * 
 * @returns The number of microseconds between the two time values.
 */
time_t timediff(struct timeval start_time, struct timeval end_time);

/**
 * Frees multiple pointers.
 * 
 * @param num_args  The number of pointers to be freed.
 * @param ...       Pointers to be freed.
*/
void multifree(int num_args, ...);

#endif // !defined(UTIL_H)
