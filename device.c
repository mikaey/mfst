#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/fs.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "device.h"
#include "mfst.h"

// A buffer for assembling messages
static char message_buffer[256];

int did_device_disconnect(dev_t device_num) {
    const char *syspath, *device_size_str;
    size_t device_size;
    struct stat sysstat;
    struct udev *udev_handle;
    struct udev_device *udev_dev;

    udev_handle = udev_new();
    if(!udev_handle) {
        return 0;
    }

    udev_dev = udev_device_new_from_devnum(udev_handle, 'b', device_num);
    if(!udev_dev) {
        udev_unref(udev_handle);
        return 1;
    }

    syspath = udev_device_get_syspath(udev_dev);
    if(!syspath) {
        udev_device_unref(udev_dev);
        udev_unref(udev_handle);
        return 1;
    }

    // Check to see if the path still exists
    if(stat(syspath, &sysstat)) {
        // Yeah, device went away...
        udev_device_unref(udev_dev);
        udev_unref(udev_handle);
        return 1;
    }

    // Did the device's size change to 0?  (E.g., did the SD card get pulled
    // out of the reader?)
    device_size_str = udev_device_get_sysattr_value(udev_dev, "size");
    if(!device_size_str) {
        udev_device_unref(udev_dev);
        udev_unref(udev_handle);
        return 1;
    }

    device_size = strtoull(device_size_str, NULL, 10);
    if(!device_size) {
        udev_device_unref(udev_dev);
        udev_unref(udev_handle);
        return 1;
    }

    // Nope, device is still there.
    udev_device_unref(udev_dev);
    udev_unref(udev_handle);
    return 0;
}

/**
 * Compares the two devices and determines whether they are identical.
 *
 * @returns 0 if the two devices are identical; 1 if they are not, if one or
 *          both of the specified devices do not exist, or if one or both of
 *          the specified devices aren't actually devices; or -1 if an error
 *          occurred.
 */
int are_devices_identical(const char *devname1, const char *devname2) {
    struct stat devstat1, devstat2;

    if(stat(devname1, &devstat1)) {
        if(errno == ENOENT) {
            // Non-existant devices shall be considered non-identical.
            return 1;
        } else {
            return -1;
        }
    }

    if(stat(devname2, &devstat2)) {
        if(errno == ENOENT) {
            return 1;
        } else {
            return -1;
        }
    }

    if(!S_ISBLK(devstat1.st_mode) || !S_ISBLK(devstat2.st_mode) || !S_ISCHR(devstat1.st_mode) || !S_ISCHR(devstat2.st_mode)) {
        return 1;
    }

    if((devstat1.st_mode & S_IFMT) != (devstat2.st_mode & S_IFMT)) {
        // Devices aren't the same type
        return 1;
    }

    if(devstat1.st_rdev != devstat2.st_rdev) {
        return 1;
    }

    return 0;
}

/**
 * Reads the beginning-of-device and middle-of-device data and compares it with
 * what is stored in bod_buffer/mod_buffer.
 *
 * Note that this function will move the position of the file pointer
 * represented by fd.  Callers are responsible for saving the position of the
 * file pointer and re-seeking to that position after this call completes.
 *
 * @param fd                   A handle to the device with the data to compare.
 * @param device_size          The physical size of the device, in bytes.
 * @param bod_buffer           A pointer to a buffer containing the data
 *                             expected at the beginning of the device.
 * @param mod_buffer           A pointer to a buffer containing the data
 *                             expected at the middle of the device.
 * @param bod_mod_buffer_size  The amount of data in bod_buffer and mod_buffer
 *                             (each), in bytes.
 * @returns 0 if either the BOD or MOD data is an exact match, 1 if the data
 *          did not match, or -1 if an error occurred.
 */
int compare_bod_mod_data(int fd, size_t device_size, char *bod_buffer, char *mod_buffer, size_t bod_mod_buffer_size) {
    char *read_buffer;
    size_t bytes_left_to_read, middle;
    size_t matching_sectors = 0;
    size_t partial_match_threshold;
    int ret;
    int sector_size;

    if(!(read_buffer = malloc(bod_mod_buffer_size))) {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data: malloc() failed: %s", strerror(errno));
        return -1;
    }

    // Get the device's sector size.
    if(ioctl(fd, BLKSSZGET, &sector_size)) {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): ioctl() failed while trying to get the device's sector size: %s", strerror(errno));
        log_log(message_buffer);
        return -1;
    }

    partial_match_threshold = bod_mod_buffer_size / sector_size; // 50%

    // Make sure we're at the beginning of the device
    if(lseek(fd, 0, SEEK_SET) == -1) {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Failed to seek to the beginning of the device: %s", strerror(errno));
        return -1;
    }

    // Read in the first 1MB.
    bytes_left_to_read = bod_mod_buffer_size;
    while(bytes_left_to_read) {
        if((ret = read(fd, read_buffer + (bod_mod_buffer_size - bytes_left_to_read), bytes_left_to_read)) == -1) {
            // If we get a read error here, we'll just zero out the rest of the sector and move on.
            memset(read_buffer + (bod_mod_buffer_size - bytes_left_to_read), 0, ((bytes_left_to_read % 512) == 0) ? 512 : (bytes_left_to_read % 512));
            bytes_left_to_read -= ((bytes_left_to_read % 512) == 0) ? 512 : (bytes_left_to_read % 512);
            if(lseek(fd, bod_mod_buffer_size - bytes_left_to_read, SEEK_SET) == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Got an error while trying to lseek() on the device: %s", strerror(errno));
                log_log(message_buffer);
                return -1;
            }
        } else {
            bytes_left_to_read -= ret;
        }
    }

    if(!memcmp(read_buffer, bod_buffer, bod_mod_buffer_size)) {
        log_log("compare_bod_mod_data(): Beginning-of-device data matches");
        return 0;
    } else {
      // Do a sector-by-sector comparison and count up the sectors
      for(bytes_left_to_read = 0; bytes_left_to_read < bod_mod_buffer_size; bytes_left_to_read += sector_size) {
        if(!memcmp(read_buffer + bytes_left_to_read, bod_buffer + bytes_left_to_read, sector_size)) {
          matching_sectors++;
        }
      }
    }

    // Read in the middle 1MB.
    bytes_left_to_read = bod_mod_buffer_size;
    middle = device_size / 2;

    if(lseek(fd, middle, SEEK_SET) == -1) {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Got an error while trying to lseek() on the device: %s", strerror(errno));
        log_log(message_buffer);
        return -1;
    }

    while(bytes_left_to_read) {
        if((ret = read(fd, read_buffer + (bod_mod_buffer_size - bytes_left_to_read), bytes_left_to_read)) == -1) {
            memset(read_buffer + (bod_mod_buffer_size - bytes_left_to_read), 0, ((bytes_left_to_read % 512) == 0) ? 512 : (bytes_left_to_read % 512));
            bytes_left_to_read -= ((bytes_left_to_read % 512) == 0) ? 512 : (bytes_left_to_read % 512);
            if(lseek(fd, middle + (bod_mod_buffer_size - bytes_left_to_read), SEEK_SET) == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Got an error while trying to lseek() on the device: %s", strerror(errno));
                log_log(message_buffer);
                return -1;
            }
        } else {
            bytes_left_to_read -= ret;
        }
    }

    if(!memcmp(read_buffer, mod_buffer, bod_mod_buffer_size)) {
        log_log("compare_bod_mod_data(): Middle-of-device data matches");
        return 0;
    } else {
        // Do a sector-by-sector comparison and count up the sectors
        for(bytes_left_to_read = 0; bytes_left_to_read < bod_mod_buffer_size; bytes_left_to_read += sector_size) {
            if(!memcmp(read_buffer + bytes_left_to_read, mod_buffer + bytes_left_to_read, sector_size)) {
                matching_sectors++;
            }
        }

        if(matching_sectors >= partial_match_threshold) {
            log_log("compare_bod_mod_data(): At least 50% of total sectors match");
            return 0;
        }
    }

    if(matching_sectors > 0) {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Device data doesn't match (only %lu sector%s matched)", matching_sectors, matching_sectors == 1 ? "" : "s");
    } else {
        snprintf(message_buffer, sizeof(message_buffer), "compare_bod_mod_data(): Device data doesn't match (no sectors matched)");
    }

    log_log(message_buffer);
    return 1;
}

int find_device(char   * preferred_dev_name,
                int      must_match,
                size_t   expected_device_size,
                size_t   physical_device_size,
                char   * bod_buffer,
                char   * mod_buffer,
                size_t   bod_mod_buffer_size,
                char   **matched_dev_name,
                dev_t  * matched_dev_num,
                int    * newfd) {
    struct udev *udev_handle;
    struct udev_enumerate *udev_enum;
    struct udev_list_entry *list_entry;
    struct udev_device *device;
    const char *dev_size_str, *dev_name;
    size_t reported_device_size;
    int fd, ret, i;
    char **matched_devices = NULL, **new_matched_devices;
    int num_matches = 0, match_index;
    struct stat fs;

    void free_matched_devices() {
        for(int j = 0; j < num_matches; j++) {
            free(matched_devices[j]);
        }

        free(matched_devices);
    }

    if(must_match) {
        if(!preferred_dev_name) {
            log_log("find_device(): must_match set to 1, but preferred_dev_name was set to NULL");
            errno = EINVAL;
            return -1;
        }

        // Let's just probe preferred_dev_name instead of bothering udev
        if((fd = open(preferred_dev_name, O_LARGEFILE | O_RDONLY)) == -1) {
            snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: open() returned an error: %s", preferred_dev_name, strerror(errno));
            log_log(message_buffer);
            return 0;
        }

        if(ioctl(fd, BLKGETSIZE64, &reported_device_size)) {
            snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: an ioctl() call to get the size of the device returned an error: %s", preferred_dev_name, strerror(errno));
            log_log(message_buffer);

            close(fd);
            return 0;
        }

        if(reported_device_size != expected_device_size) {
            snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: device size does not match (expected size = %'lu bytes, reported size = %'lu bytes)", preferred_dev_name,
                     expected_device_size, reported_device_size);
            log_log(message_buffer);

            close(fd);
            return 0;
        }

        ret = compare_bod_mod_data(fd, physical_device_size, bod_buffer, mod_buffer, bod_mod_buffer_size);
        close(fd);

        if(ret) {
            if(ret == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: an error occurred while comparing beginning-of-device and middle-of-device data", preferred_dev_name);
            } else {
                snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: beginning-of-device and middle-of-device data don't match", preferred_dev_name);
            }

            log_log(message_buffer);
            return 0;
        }

        // Add the device to our list of devices
        if(!(matched_devices = malloc(++num_matches * sizeof(char *)))) {
            snprintf(message_buffer, sizeof(message_buffer), "find_device(): malloc() failed: %s", strerror(errno));
            log_log(message_buffer);

            errno = ENOMEM;
            return -1;
        }

        if(!(matched_devices[0] = strdup(preferred_dev_name))) {
            snprintf(message_buffer, sizeof(message_buffer), "find_device(): strdup() failed: %s", strerror(errno));
            log_log(message_buffer);
            return -1;
        }
    } else {
        // Scan through the available block devices
        udev_handle = udev_new();
        if(!udev_handle) {
            log_log("find_device(): udev_new() failed");

            free_matched_devices();

            errno = ELIBACC;
            return -1;
        }

        if(!(udev_enum = udev_enumerate_new(udev_handle))) {
            log_log("find_device(): udev_enumerate_new() failed");
            udev_unref(udev_handle);

            free_matched_devices();

            errno = ELIBACC;
            return -1;
        }

        if(udev_enumerate_add_match_subsystem(udev_enum, "block") < 0) {
            log_log("find_device(): udev_enumerate_add_match_subsystem() failed");
            udev_enumerate_unref(udev_enum);
            udev_unref(udev_handle);
            errno = ELIBACC;
            return -1;
        }

        if(udev_enumerate_scan_devices(udev_enum) < 0) {
            log_log("find_devices(): udev_enumerate_scan_devices() failed");
            udev_enumerate_unref(udev_enum);
            udev_unref(udev_handle);

            free_matched_devices();

            errno = ELIBACC;
            return -1;
        }

        if(!(list_entry = udev_enumerate_get_list_entry(udev_enum))) {
            // This scenario has to be an error.  What system wouldn't have any
            // block devices??
            log_log("find_devices(): udev_enumerate_get_list_entry() failed");
            udev_enumerate_unref(udev_enum);
            udev_unref(udev_handle);

            free_matched_devices();

            errno = ELIBACC;
            return -1;
        }

        while(list_entry) {
            if(!(device = udev_device_new_from_syspath(udev_handle, udev_list_entry_get_name(list_entry)))) {
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            if(!(dev_size_str = udev_device_get_sysattr_value(device, "size"))) {
                udev_device_unref(device);
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            if(!(dev_name = udev_device_get_devnode(device))) {
                udev_device_unref(device);
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            snprintf(message_buffer, sizeof(message_buffer), "find_device(): Looking at device %s", dev_name);
            log_log(message_buffer);

            reported_device_size = strtoull(dev_size_str, NULL, 10) * 512;
            if(reported_device_size != expected_device_size) {
                snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: device size doesn't match", dev_name);
                log_log(message_buffer);

                udev_device_unref(device);
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            if((fd = open(dev_name, O_LARGEFILE | O_RDONLY)) == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: open() returned an error: %s", dev_name, strerror(errno));
                log_log(message_buffer);

                udev_device_unref(device);
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            ret = compare_bod_mod_data(fd, physical_device_size, bod_buffer, mod_buffer, bod_mod_buffer_size);
            close(fd);

            if(ret) {
                if(ret == -1) {
                    snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: an error occurred while comparing beginning-of-device and middle-of-device data", dev_name);
                } else {
                    snprintf(message_buffer, sizeof(message_buffer), "find_device(): Rejecting device %s: beginning-of-device and middle-of-device data doesn't match what was provided", dev_name);
                }

                log_log(message_buffer);
                udev_device_unref(device);
                list_entry = udev_list_entry_get_next(list_entry);
                continue;
            }

            snprintf(message_buffer, sizeof(message_buffer), "find_device(): Got a match on device %s", dev_name);
            log_log(message_buffer);

            // Add the device to our list of devices
            if(!(new_matched_devices = realloc(matched_devices, ++num_matches * sizeof(char *)))) {
                snprintf(message_buffer, sizeof(message_buffer), "find_device(): realloc() failed: %s", strerror(errno));
                log_log(message_buffer);

                udev_device_unref(device);
                udev_enumerate_unref(udev_enum);
                udev_unref(udev_handle);

                free_matched_devices();

                errno = ENOMEM;
                return -1;
            }

            matched_devices = new_matched_devices;

            if(!(matched_devices[num_matches - 1] = strdup(dev_name))) {
                snprintf(message_buffer, sizeof(message_buffer), "find_devices(): strdup() failed: %s", strerror(errno));
                log_log(message_buffer);

                udev_device_unref(device);
                udev_enumerate_unref(udev_enum);
                udev_unref(udev_handle);

                free(matched_devices);

                errno = ENOMEM;
                return -1;
            }

            udev_device_unref(device);
            list_entry = udev_list_entry_get_next(list_entry);
        }

        // We're done with udev now
        udev_enumerate_unref(udev_enum);
        udev_unref(udev_handle);
    }

    if(!num_matches) {
        log_log("find_device(): No matching devices found");
        return 0;
    } else if(num_matches > 1) {
        // We found more than one match.  Was a preferred_dev_name provided?
        match_index = -1;
        if(preferred_dev_name) {
            // Does preferred_dev_name match one of the devices we found?
            for(i = 0; i < num_matches; i++) {
                if(!are_devices_identical(preferred_dev_name, matched_devices[i])) {
                    // Cool, we found a match.
                    match_index = i;
                }
            }
        }

        if(match_index == -1) {
            // We didn't find a match.
            free_matched_devices();

            if(preferred_dev_name) {
                log_log("find_device(): Found multiple matching devices, and preferred_dev_name did not match any of the devices found");
            } else {
                log_log("find_device(): Found multiple matching devices, and preferred_dev_name was not set");
            }

            return num_matches;
        }
    } else {
        match_index = 0;
    }

    // Ok, we have a single match.  Grab the device number for it.
    if(stat(matched_devices[match_index], &fs)) {
        snprintf(message_buffer, sizeof(message_buffer), "find_device(): Got a match on %s, but got an error while trying to stat() it: %s",
            matched_devices[match_index], strerror(errno));
        log_log(message_buffer);
        log_log("find_device(): You can try unplugging/re-plugging it to see if maybe it'll work next time...");

        for(i = 0; i < num_matches; i++) {
            free(matched_devices[i]);
        }

        free(matched_devices);

        errno = EINTR;
        return -1;
    }

    // Ok, we have a single match.  Re-open the device read/write.
    if((fd = open(matched_devices[match_index], O_DIRECT | O_SYNC | O_LARGEFILE | O_RDWR)) == -1) {
        // Well crap.
        snprintf(message_buffer, sizeof(message_buffer), "find_device(): Got a match on %s, but got an error while trying to re-open() it: %s",
            matched_devices[match_index], strerror(errno));
        log_log(message_buffer);
        log_log("find_device(): You can try unplugging/re-plugging it to see if maybe it'll work next time...");

        free_matched_devices();

        errno = EINTR;
        return -1;
    }

    // Set newfd, matched_dev_name, and matched_dev_num
    *newfd            = fd;
    *matched_dev_name = matched_devices[match_index];
    *matched_dev_num  = fs.st_rdev;

    // Free all the matched devices *except* for the one at match_index
    for(i = 0; i < num_matches; i++) {
        if(i != match_index) {
            free(matched_devices[i]);
        }
    }

    free(matched_devices);

    return 0;
}

int wait_for_device_reconnect(size_t   expected_device_size,
                              size_t   physical_device_size,
                              char   * bod_buffer,
                              char   * mod_buffer,
                              size_t   bod_mod_buffer_size,
                              char   **matched_dev_name,
                              dev_t  * matched_dev_num,
                              int    * newfd) {
    struct udev_monitor *monitor;
    struct udev_device *device;
    struct udev *udev_handle;
    const char *dev_size_str;
    const char *dev_name;
    size_t reported_device_size;
    int fd, ret;
    char *new_dev_name;
    struct stat fs;

    udev_handle = udev_new();
    if(!udev_handle) {
        return -1;
    }

    monitor = udev_monitor_new_from_netlink(udev_handle, "udev");
    if(!monitor) {
        snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): udev_monitor_new_from_nelink() returned NULL");
        log_log(message_buffer);

        udev_unref(udev_handle);
        return -1;
    }

    if(udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", "disk") < 0) {
        snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): udev_monitor_filter_add_match_subsystem_devtype() returned an error");
        log_log(message_buffer);

        udev_monitor_unref(monitor);
        udev_unref(udev_handle);
        return -1;
    }

    if(udev_monitor_enable_receiving(monitor) < 0) {
        snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): udev_monitor_enable_receiving() returned an error");
        log_log(message_buffer);

        udev_monitor_unref(monitor);
        udev_unref(udev_handle);
        return -1;
    }

    while(1) {
        while(device = udev_monitor_receive_device(monitor)) {
            dev_name = udev_device_get_devnode(device);
            if(!dev_name) {
                // Can't even get the name of the device... :-(
                udev_device_unref(device);
                continue;
            }

            snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Detected new device %s", dev_name);
            log_log(message_buffer);

            // Check the size of the device.
            dev_size_str = udev_device_get_sysattr_value(device, "size");
            if(!dev_size_str) {
                // Nope, let's move on.
                snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Rejecting device %s: can't get size of device", dev_name);
                log_log(message_buffer);

                udev_device_unref(device);
                continue;
            }

            reported_device_size = strtoull(dev_size_str, NULL, 10) * 512;
            if(reported_device_size != expected_device_size) {
                // Device's reported size doesn't match
                snprintf(message_buffer, sizeof(message_buffer),
                         "wait_for_device_reconnect(): Rejecting device %s: device size doesn't match (this device = %'lu bytes, device we're looking for = %'lu bytes)",
                         dev_name, reported_device_size, expected_device_size);
                log_log(message_buffer);

                udev_device_unref(device);
                continue;
            }

            if((fd = open(dev_name, O_LARGEFILE | O_RDONLY)) == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Rejecting device %s: open() returned an error: %s", dev_name, strerror(errno));
                log_log(message_buffer);

                udev_device_unref(device);
                continue;
            }

            ret = compare_bod_mod_data(fd, physical_device_size, bod_buffer, mod_buffer, bod_mod_buffer_size);
            close(fd);

            if(ret) {
                if(ret == -1) {
                    snprintf(message_buffer, sizeof(message_buffer),
                             "wait_for_device_reconnect(): Rejecting device %s: an error occurred while comparing beginning-of-device and middle-of-device data", dev_name);
                } else {
                    snprintf(message_buffer, sizeof(message_buffer),
                             "wait_for_device_reconnect(): Rejecting device %s: beginning-of-device and middle-of-device data doesn't match", dev_name);
                }

                log_log(message_buffer);
                udev_device_unref(device);
                continue;
            }

            // Ok, we have a match.  Get stats on the device.
            if(stat(dev_name, &fs) == -1) {
                snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Got a match on %s, but got an error while trying to stat() it: %s", dev_name, strerror(errno));
                log_log(message_buffer);
                log_log("You can try unplugging/re-plugging it to see if maybe it'll work next time...");

                udev_device_unref(device);
                continue;
            }

            // Re-open the device read/write.
            if((fd = open(dev_name, O_DIRECT | O_SYNC | O_LARGEFILE | O_RDWR)) == -1) {
                // Well crap.
                snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Got a match on %s, but got an error while trying to re-open() it: %s",
                         dev_name, strerror(errno));
                log_log(message_buffer);
                log_log("You can try unplugging/re-plugging it to see if maybe it'll work next time...");

                udev_device_unref(device);
                continue;
            }

            snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Got a match on device %s", dev_name);
            log_log(message_buffer);

            // Copy the device name over to device_name.
            if(!(new_dev_name = strdup(dev_name))) {
                snprintf(message_buffer, sizeof(message_buffer), "wait_for_device_reconnect(): Got a match on %s, but a call to strdup() failed: %s", dev_name, strerror(errno));
                log_log(message_buffer);

                close(fd);
                udev_device_unref(device);
                udev_monitor_unref(monitor);
                udev_unref(udev_handle);

                return -1;
            }

            *matched_dev_name = new_dev_name;
            *matched_dev_num = fs.st_rdev;
            *newfd = fd;

            // Cleanup and exit
            udev_device_unref(device);
            udev_monitor_unref(monitor);
            udev_unref(udev_handle);
            return 0;
        }

        usleep(100000);
    }
}

int can_reset_device(dev_t device_num) {
    struct udev *udev_handle;
    struct udev_device *child_device, *parent_device;

    udev_handle = udev_new();
    if(!udev_handle) {
        return 0;
    }

    child_device = udev_device_new_from_devnum(udev_handle, 'b', device_num);
    if(!child_device) {
        udev_unref(udev_handle);
        return 0;
    }

    // We only know how to reset USB devices -- so if it's not a USB device, we're gonna fail
    parent_device = udev_device_get_parent_with_subsystem_devtype(child_device, "usb", "usb_device");
    if(!parent_device) {
      udev_device_unref(child_device);
      udev_unref(udev_handle);
      return 0;
    }

    udev_device_unref(child_device);
    udev_unref(udev_handle);
    return 1;
}

int reset_device(int device_fd) {
    struct udev *udev_handle;
    struct udev_device *child_device, *parent_device;
    const char *device_name;
    char *new_device_name;
    dev_t device_num, new_device_num;
    int fd, ret;
    struct stat dev_stat;

    if(ret = fstat(device_fd, &dev_stat)) {
        // Can't stat the device
        return -1;
    }

    if(!S_ISBLK(dev_stat.st_mode)) {
        // Device isn't a block device
        return -1;
    }

    device_num = dev_stat.st_rdev;

    udev_handle = udev_new();
    if(!udev_handle) {
        return -1;
    }

    child_device = udev_device_new_from_devnum(udev_handle, 'b', device_num);
    if(!child_device) {
        udev_unref(udev_handle);
        return -1;
    }

    parent_device = udev_device_get_parent_with_subsystem_devtype(child_device, "usb", "usb_device");
    if(!parent_device) {
        udev_device_unref(child_device);
        udev_unref(udev_handle);
        return -1;
    }

    device_name = udev_device_get_devnode(parent_device);
    if(!device_name) {
        udev_device_unref(child_device);
        udev_unref(udev_handle);
        return -1;
    }

    if((fd = open(device_name, O_WRONLY | O_NONBLOCK)) == -1) {
        udev_device_unref(child_device);
        udev_unref(udev_handle);
        return -1;
    }

    close(device_fd);

    if(ioctl(fd, USBDEVFS_RESET) == -1) {
      close(fd);
      return -1;
    }

    close(fd);

    ret = find_device(program_options.device_name, 0, device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name, &new_device_num, &fd);
    if(ret == -1) {
      return -1;
    } else if(ret == 0) {
        if(wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name, &new_device_num, &fd)) {
            log_log("reset_device(): Error while waiting for device to reconnect");
            return -1;
        }
    } else if(ret > 1) {
        // There is an edge case where two identical devices start a round at
        // the same time, the random seeds end up being the same, and as a
        // result, they end up having the same BOD/MOD data.  At some poiont I
        // need to do something like prompt the user to fix this by selecting
        // the right device or waiting for the other device(s) BOD/MOD data to
        // change on its own (e.g., if another process is testing the device).
    }

    free(program_options.device_name);
    program_options.device_name = new_device_name;
    device_stats.device_num = new_device_num;
    return fd;
}
