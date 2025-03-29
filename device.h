#if !defined(DEVICE_H)
#define DEVICE_H

#include <libudev.h>
#include <uuid/uuid.h>

/**
 * A struct for providing search parameters for locating a device.
 */
typedef struct _device_search_params_t {
    /**
     * The logical size of the device (e.g., the size of the device as reported
     * by the device), in bytes.
     */
    size_t logical_device_size;

    /**
     * The expected physical size of the device (e.g., the actual usable area of
     * the device), in bytes.
     */
    size_t physical_device_size;

    /**
     * A buffer containing the data expected to appear at the beginning of the
     * device.  The buffer is expected to be at least as big as indicated by
     * bod_mod_buffer_size.
     */
    char  *bod_buffer;

    /**
     * A buffer containing the data expected to appear in the middle of the
     * device.  The buffer is expected to be at least as big as indicated by
     * bod_mod_buffer_size.
     */
    char  *mod_buffer;

    /**
     * The size of bod_buffer and mod_buffer, in bytes.
     */
    size_t bod_mod_buffer_size;

    /**
     * The UUID that is expected to be embedded in each sector of the device,
     * or NULL if the device is not expected to have an embedded UUID.
     */
    uuid_t expected_device_uuid;

    /**
     * The device's sector map.
     */
    char  *sector_map;

    /**
     * The preferred path to the device, or NULL if no device should be
     * preferred  If a device search matches multiple devices, and the device
     * specified here is among the matches, the search will succeed and the
     * device specified here will be used.
     *
     * This parameter is only used by find_device().  It is ignored by
     * wait_for_device_reconnect().
     */
    char  *preferred_dev_name;

    /**
     * Non-zero if the device search must match the device specified in
     * preferred_dev_name, or zero if an exact match is not required.  If
     * preferred_dev_name is NULL, this must be set to 0; setting it to a
     * non-zero value results in an error.
     *
     * This parameter is only used by find_device().  It is ignored by
     * wait_for_device_reconnect().
     */
    int    must_match_preferred_dev_name;
} device_search_params_t;

/**
 * A struct used to return info on a matched device.
 */
typedef struct _device_search_result_t {
    /**
     * A pointer to a null-terminated string containing the name of the matched
     * device.
     */
    char *device_name;

    /**
     * The device number of the matched device.
     */
    dev_t device_num;

    /**
     * The file handle of the matched device.
     */
    int   fd;
} device_search_result_t;

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
 * device_search_params.  If it finds a single device that matches the given
 * criteria, it opens it and returns a device_search_result_t containing the
 * info on the device (which includes an open file handle to the device).
 *
 * If must_match_preferred_dev_name is set to 0, preferred dev_name can be used
 * to suggest the path to the device file.
 *  - If there is an ambiguity, preferred_dev_name will be used to resolve the
 *    ambiguity (if set).
 *  - If there is an ambiguity, but none of the discovered devices matches the
 *    one set in preferred_dev_name, this function will return an error.
 *  - If there is no ambiguity, preferred_dev_name will be ignored.
 *
 * If must_match_preferred_dev_name is set to a non-zero value,
 * preferred_dev_name must be set to the name of the device to match against.
 *  - If preferred_dev_name is set to NULL, this function returns an error.
 *  - If the specified device does not match the provided parameters, this
 *    function will return an error.
 *
 * @param device_search_params  A device_search_params_t containing information
 *                              on the device being searched for.
 *
 * @returns A pointer to a device_search_result_t containing the information on
 *          the matched device, or NULL if an error occurred.  In the event of
 *          an error, errno will be set to one of the following values:
 *          - EFAULT   device_search_params was set to NULL.
 *          - ENODEV   No devices were found that matched the given parameters.
 *          - ENOTUNIQ More than one device was found that matched the given
 *                     parameters, but none of the matched devices matched the
 *                     one specified by preferred_dev_name (or
 *                     preferred_dev_name was set to NULL)
 *          - EINVAL   must_match_preferred_dev_name was set to a non-zero
 *                     value, but preferred_dev_name was set to NULL.
 *          - ENOMEM   A memory allocation error occurred.
 *          - ELIBACC  A call to a udev_* function returned an unexpected error.
 *          - EINTR    An error occurred while trying to stat() or open() the
 *                     device.
 *
 *          Note that the returned object should be freed by the caller using
 *          free_device_search_result() once finished.
 */
device_search_result_t *find_device(device_search_params_t *device_search_params);

/**
 * Monitor for new block devices.  When a new block device is detected, compare
 * it to the information provided.  If it's a match, open a new file handle for
 * it.  Note that this function blocks until a matching device is discovered.
 *
 * @param device_search_params  A device_search_params_t containing information
 *                              on the device to search for.
 *
 * @returns A pointer to a new device_search_result_t containing the information
 *          on the matched device, or NULL if an error occurred.  In the event
 *          of an error, errno will be set to one of the following values:
 *          - EFAULT   device_search_params was set to NULL.
 *          - ELIBACC  A call to a udev_* function returned an unexpected error.
 *          - EINTR    An error occurred while calling strdup() to copy the name
 *                     of the found device.
 */
device_search_result_t *wait_for_device_reconnect(device_search_params_t *device_search_params);

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

/**
 * Indicates whether the specified device is a block device.
 *
 * @param filename  The name of the device to query.
 *
 * @returns Non-zero if the device is a block device, 0 if the device is not a
 *          block device, or -1 if an error occurred.
 */
int is_block_device(char *filename);

/**
 * Frees a device_search_result_t object.
 *
 * @param result  A pointer to the device_search_result_t object to be freed.
 */
void free_device_search_result(device_search_result_t *result);

#endif // !defined(DEVICE_H)
