#if !defined(DEVICE_H)
#define DEVICE_H

#include <libudev.h>

/**
 * Tries to determine whether the device was disconnected from the system.
 *
 * @param device_num  The device number of the device to test.
 *
 * @returns Non-zero if the device was disconnected from the system, or zero if
 *          the device appears to still be present (or if an error occurred).
 */
int did_device_disconnect(dev_t device_num);

/**
 * Looks at all block devices for one that matches the geometry described in
 * device_stats and whose BOD/MOD data matches either bod_buffer or mod_buffer,
 * respectively.  If it finds a single device that matches the given criteria,
 * it opens it, sets *matched_dev_name to the name of the matched device, and
 * sets *newfd to the opened file handle.  If multiple devices or no devices
 * match the given criteria, no action is taken and *matched_dev_name and *newfd
 * are left unmodified.
 *
 * If must_match is set to 0, preferred_dev_name can be used to suggest the name
 * of a device.
 *  - If there is an ambiguity, preferred_dev_name will be used to resolve the
 *    ambiguity (if set).
 *  - If there is an ambiguity, but none of the discovered devices matches the
 *    one set in preferred_dev_name, this function will return an error.
 *  - If there is no ambiguity, preferred_dev_name will be ignored.
 *
 * If must_match is set to a non-zero value, preferred_dev_name must be set to
 * the name of the device to match against.
 *  - If preferred_dev_name is set to NULL, this function returns an error.
 *  - If the specified device does not match the provided parameters, this
 *    function will return an error.
 *
 * @param preferred_dev_name    Indicates the path to the preferred device.
 * @param must_match            Indicates whether the device found must match
 *                              the one specified in preferred_dev_name.
 * @param expected_device_size  The logical size of the expected device (e.g.,
 *                              the size of the device, as reported by the
 *                              device), in bytes.
 * @param physical_device_size  The physical size of the expected device (e.g.,
 *                              the size of the device, as discovered by probing
 *                              the device), in bytes.
 * @param bod_buffer            A pointer to a buffer that holds the data
 *                              expected to be present at the beginning of the
 *                              device.
 * @param mod_buffer            A pointer to a buffer that holds the data
 *                              expected to be present at the middle of the
 *                              device.
 * @param bod_mod_buffer_size   The size of bod_buffer and mod_buffer, in bytes.
 * @param matched_dev_name      A pointer to a char * that will receive the name
 *                              of the matched device.  Memory for this string
 *                              is allocated using malloc() and must be freed
 *                              with free() when no longer needed.  If no
 *                              matching device can be found, or if an error
 *                              occurs, the contents of *matched_dev_name are
 *                              not modified.
 * @param matched_dev_num       A pointer to a dev_t that will receive the
 *                              device number of the matched device.  If no
 *                              matching device can be found, or if an error
 *                              occurs, the contents of *matched_dev_num are not
 *                              modified.
 * @param newfd                 A pointer to an int that will receive the file
 *                              handle for the newly opened device.  If no
 *                              matching device can be found, or if an error
 *                              occurs, the contents of *newfd are not modified.
 *
 * @returns The number of matched devices, or -1 if an error occurred.  In the
 *          event of an error, errno will be set to one of the following values:
 *          - EINVAL   must_match was set to a non-zero value, but
 *                     preferred_dev_name was set to NULL.
 *          - ENOMEM   A call to malloc() or realloc() failed.
 *          - ELIBACC  A call to a udev function returned an unexpected error.
 *          - EINTR    A matching device was found, but an error occurred while
 *                     trying to stat() or open() the device.
 */
int find_device(char   * preferred_dev_name,
                int      must_match,
                size_t   expected_device_size,
                size_t   physical_device_size,
                char   * bod_buffer,
                char   * mod_buffer,
                size_t   bod_mod_buffer_size,
                char   **matched_dev_name,
                dev_t  * matched_dev_num,
                int    * newfd);

/**
 * Monitor for new block devices.  When a new block device is detected, compare
 * it to the information provided.  If it's a match, open a new file handle for
 * it.
 *
 * @param expected_device_size  The logical size of the expected device (e.g.,
 *                              the size of the device, as reported by the
 *                              device), in bytes.
 * @param physical_device_size  The physical size of the expected device (e.g.,
 *                              the size of the device, as discovered by probing
 *                              the device), in bytes.
 * @param bod_buffer            A pointer to a buffer containing the data
 *                              expected to be present at the beginning of the
 *                              device.
 * @param mod_buffer            A pointer to a buffer containing the data
 *                              expected to be present at the middle of the
 *                              device.
 * @param bod_mod_buffer_size   The number of bytes in bod_buffer and mod_buffer
 *                              (each).
 * @param matched_dev_name      A pointer to a char * that will receive the name
 *                              of the matched device.  Note that the memory for
 *                              the new string will be allocated using malloc()
 *                              and must be freed using free() when no longer
 *                              needed.
 * @param matched_dev_num       A pointer to a dev_t that will receive the
 *                              device number of the matched device.
 * @param newfd                 A pointer to an int that will receive the file
 *                              handle for the opened device.
 *
 * @returns 0 if a new device was successfully discovered, or -1 if an error
 *          occurred.  Note that this function blocks until a matching device is
 *          discovered.
 */
int wait_for_device_reconnect(size_t   expected_device_size,
                              size_t   physical_device_size,
                              char   * bod_buffer,
                              char   * mod_buffer,
                              size_t   bod_mod_buffer_size,
                              char   **matched_dev_name,
                              dev_t  * matched_dev_num,
                              int    * newfd);

/**
 * Determines whether the device described in device_num is a device that we
 * know how to reset.
 *
 * @param device_num  The device number of the device to be queried.
 *
 * @returns 1 if we know how to reset the device, or 0 if we don't.
 */
int can_reset_device(dev_t device_num);

/**
 * Resets the given device.
 *
 * @param device_fd  An open file handle to the device which will be reset.
 *
 * @returns A new file handle to the device if the device was successfully
 *          reset, or -1 if it was not.
 */
int reset_device(int device_fd);

#endif // !defined(DEVICE_H)
