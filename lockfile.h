#if !defined(LOCKFILE_H)
#define LOCKFILE_H

#include "device_testing_context.h"

/**
 * Opens the given lockfile.
 *
 * @param filename  The name of the lockfile to be opened.
 *
 * @returns Zero on success, or the contents of errno if an error occurred.
 */
int open_lockfile(device_testing_context_type *device_testing_context, char *filename);

/**
 * Test to see if the lockfile is locked.
 * 
 * @returns Zero if the lockfile is not locked, or non-zero if it is.
 */
int is_lockfile_locked();

/**
 * Locks the lockfile.
 * 
 * @returns Zero if the lockfile was locked successfully, or non-zero if it was
 *          not.
 */
int lock_lockfile(device_testing_context_type *device_testing_context);

/**
 * Unlocks the lockfile.
 * 
 * @returns Zero if the lockfile was unlocked successfully, or non-zero if it
 *          was not.
 */
int unlock_lockfile(device_testing_context_type *device_testing_context);

/**
 * Closes the lockfile.
 */
void close_lockfile();

#endif // !defined(LOCKFILE_H)
