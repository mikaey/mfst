#if !defined(DEVICE_TESTING_CONTEXT_H)
#define DEVICE_TESTING_CONTEXT_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <uuid/uuid.h>

#include "fake_flash_enum.h"

typedef struct _device_info_type {
    char *device_name;             // Current device name (e.g., /dev/sdb)

    uint64_t logical_size;         // Logical size of the device, in bytes (as
                                   // returned by a BLKGETSIZE64 ioctl)

    uint64_t physical_size;        // Physical size of the device, in bytes (as
                                   // specified on the command line; or, if not
                                   // specified on the command line, as
                                   // determined by the capacity test

    uint64_t middle_of_device;     // Byte location of the middle of the device
                                   // (for purposes of keeping track of the data
                                   // we've written to the middle of the device)

    int sector_size;               // Logical sector size, in bytes

    unsigned short max_sectors_per_request;
                                   // Maximum number of sectors per request (as
                                   // returned by a BLKSECTGET ioctl)

    uint64_t num_logical_sectors;  // Logical number of sectors (logical device
                                   // size divided by sector size

    uint64_t num_physical_sectors; // Physical number of sectors (physical
                                   // device size divided by sector size)

    dev_t device_num;              // Device number of the current device

    uuid_t device_uuid;            // UUID assigned to the device

    int fd;                        // Current device handle

    uint64_t optimal_block_size;   // Optimal write block size, in bytes, as
                                   // determined by the optimal block size test;
                                   // or, if the optimal block size test was not
                                   // run, by multiplying sector_size by
                                   // max_sectors_per_request.

    FakeFlashEnum is_fake_flash;   // Whether the device is considered fake
                                   // as determined by the capacity test; or, if
                                   // the capacity test was not run, as
                                   // determined by comparing the value of the
                                   // --force-sectors command-line option.  If
                                   // the capacity test was not run and
                                   // --force-sectors was not provided, this is
                                   // set to FAKE_FLASH_UNKNOWN.

    char *bod_buffer;              // Holds the first 1MB of data written to the
                                   // device (to allow the device to be
                                   // accurately matched when the program is
                                   // aborted/restarted, or when the device is
                                   // disconnected/reconnected).

    char *mod_buffer;              // Holds 1MB of data from the middle of the
                                   // device (to allow the device to be
                                   // accurately matched when the program is
                                   // aborted/restarted, or when the device is
                                   // disconnected/reconnected).

    int bod_mod_buffer_size;       // The size of the BOD/MOD buffers, in bytes.
} device_info_type;

typedef struct _optimal_block_size_test_info_type {
    int perform_test;       // Should the test be run?

    int test_performed;     // Was the test run, and did it complete
                            // successfully?

    int optimal_block_size; // Optimal write block size, in bytes, as determined
                            // by the optimal write block size test

} optimal_block_size_test_info_type;

typedef struct _capacity_test_info_type {
    int perform_test;            // Should the test be run?

    int test_performed;          // Was the test run, and did it complete
                                 // successfully?

    FakeFlashEnum is_fake_flash; // Is the device fake flash?  (E.g., does its
                                 // logical capacity match its physical
                                 // capacity?)

    uint64_t device_size;        // Physical size of the device as determined
                                 // by the capacity test

    uint64_t num_sectors;        // Physical number of sectors (determined from
                                 // the physical device size and the logical
                                 // sector size

} capacity_test_info_type;

typedef struct _performance_test_info_type {
    int perform_test;              // Should the test be run?

    int test_started;              // Was the test started?

    int test_completed;            // Did the test complete successfully?

    double sequential_write_speed; // Measured sequential write speed, in bytes
                                   // per second

    double sequential_read_speed;  // Measured sequential read speed, in bytes
                                   // per second

    double random_write_iops;      // Measured random write speed, in IOPS per
                                   // second

    double random_read_iops;       // Measured random read speed, in IOPS per
                                   // second

} performance_test_info_type;

typedef enum _current_phase_type {
                                  CURRENT_PHASE_UNSET = 0,
                                  CURRENT_PHASE_READING,
                                  CURRENT_PHASE_WRITING
} current_phase_type;

typedef struct _stats_file_counters_type {
                                     // Total number of bytes written to the
                                     // device
    volatile uint64_t total_bytes_written;

                                     // Total number of bytes read from the
                                     // device
    volatile uint64_t total_bytes_read;

    struct timeval last_update_time; // Time of last update

    uint64_t last_bytes_written;     // Total bytes written at last update

    uint64_t last_bytes_read;        // Total bytes read at last update

    uint64_t last_bad_sectors;       // Total number of bad sectors at last
                                     // update

} stats_file_counters_type;

typedef struct _screen_counters_type {
    struct timeval last_update_time; // Time of last update

                                     // Total number of bytes read from/written
                                     // to the device since the last on-screen
                                     // update
    volatile uint64_t bytes_since_last_update;

} screen_counters_type;

typedef struct _rng_state_type {
    unsigned long initial_seed;    // Initial seed for the RNG

    unsigned long current_seed;    // Current seed

    char rng_state_buf[256];       // Buffer for the RNG state

    struct random_data rng_state;  // random_data struct to pass to random()

} rng_state_type;
    
typedef struct _endurance_test_info_type {
    int perform_test;                        // Should the test be performed?

    int test_started;                        // Was the test started?

    int test_completed;                      // Did the test complete
                                             // successfully?

    unsigned long initial_seed;              // Seed used for the RNG

    unsigned long current_seed;            

    current_phase_type current_phase;        // 0 = Reading, 1 = Writing

    volatile uint64_t rounds_completed;      // How many rounds of endurance
                                             // testing have been completed so
                                             // far?

    uint64_t total_bad_sectors;              // How many of the device's sectors
                                             // have been flagged as "bad" so
                                             // far?

    uint64_t num_bad_sectors_this_round;     // How many of the device's sectors
                                             // failed testing this round?

    uint64_t num_new_bad_sectors_this_round; // How many of the device's sectors
                                             // were flagged as "bad" for the
                                             // first time this round?

    unsigned long initial_rng_seed;          // What was the initial RNG seed
                                             // used for this endurance test?

    uint64_t num_good_sectors_this_round;    // How many of the device's
                                             // sectors, that were flagged as
                                             // "bad" during a previous round of
                                             // testing, tested as "good" during
                                             // this round?

    char *sector_map;                        // Pointer to the device's sector
                                             // map


    uint64_t sectors_to_0_1_threshold;       // How many sectors must fail to
                                             // cross the 0.1% threshold?

    uint64_t sectors_to_1_threshold;         // How many sectors must fail to
                                             // cross the 1% threshold?

    uint64_t sectors_to_10_threshold;        // How many sectors must fail to
                                             // cross the 10% threshold?

    uint64_t sectors_to_25_threshold;        // How many sectors must fail to
                                             // cross the 25% threshold?

    uint64_t rounds_to_first_error;          // How many rounds of testing were
                                             // we able to complete before the
                                             // device experienced its first
                                             // error?

    uint64_t rounds_to_0_1_threshold;        // How many rounds of testing were
                                             // we able to complete before 0.1%
                                             // of the device's sectors failed?

    uint64_t rounds_to_1_threshold;          // How many rounds of testing were
                                             // we able to complete before 1%
                                             // of the device's sectors failed?

    uint64_t rounds_to_10_threshold;         // How many rounds of testing were
                                             // we able to complete before 10%
                                             // of the device's sectors failed?

    uint64_t rounds_to_25_threshold;         // How many rounds of testing were
                                             // we able to complete before 25%
                                             // of the device's sectors failed?

                                             // Counters for stats file logging
    stats_file_counters_type stats_file_counters;

    FILE *stats_file_handle;                 // Handle to the stats file

    screen_counters_type screen_counters;    // Counters for updating the screen

    rng_state_type rng_state;                // State for the RNG used with this
                                             // device

} endurance_test_info_type;

typedef struct _device_testing_context_type {
    device_info_type device_info;
    optimal_block_size_test_info_type optimal_block_size_test_info;
    capacity_test_info_type capacity_test_info;
    performance_test_info_type performance_test_info;
    endurance_test_info_type endurance_test_info;
    char *state_file_name;
    char *log_file_name;
    FILE *log_file_handle;
} device_testing_context_type;

/**
 * Creates a new device testing context.  Sets all variables to zeroes,
 * allocates memory for the BOD and MOD buffers, and initializes certain member
 * variables.
 *
 * @param bod_mod_buffer_size  The number of bytes to allocate for the BOD and
 *                             MOD buffers.
 *
 * @returns A pointer to a new device_testing_context_type, or NULL if a memory
 *          allocation error occurred.
 */
device_testing_context_type *new_device_testing_context(int bod_mod_buffer_size);

/**
 * Frees a device testing context and any memory pointers contained within.
 *
 * @param dtc  The device testing context to free.
 */
void delete_device_testing_context(device_testing_context_type *dtc);

int device_info_set_device_name(device_testing_context_type *dtc, char *device_name);
void device_info_invalidate_file_handle(device_testing_context_type *dtc);
void device_info_delete_state_file_name(device_testing_context_type *dtc);

void endurance_test_info_reset_per_round_counters(device_testing_context_type *dtc);

#endif // !defined(DEVICE_TESTING_CONTEXT_H)
