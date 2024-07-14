#if !defined(__MFST_H)
#define __MFST_H

#define VERSION "0.3"
#define PROGRAM_NAME " Mikaey's Flash Stress Test v" VERSION " "
#include <stdio.h>
#include <sys/stat.h>
#include <uuid/uuid.h>


// Size of the beginning-of-device and middle-of-device buffers
#define BOD_MOD_BUFFER_SIZE 1048576

// Number of seconds to profile the RNG
#define RNG_PROFILE_SECS 5

// So that I don't have to memorize the different color pairs
#define BLACK_ON_WHITE   1
#define BLACK_ON_BLUE    2
#define BLACK_ON_GREEN   3
#define BLACK_ON_RED     4
#define GREEN_ON_BLACK   5
#define RED_ON_BLACK     6
#define BLACK_ON_MAGENTA 7
#define BLACK_ON_YELLOW  8

// How many times to retry a given operation before attempting a reset
#define MAX_OP_RETRIES 5

// How many times to try resetting the device before giving up
#define MAX_RESET_RETRIES 5

// Abort reasons
#define ABORT_REASON_READ_ERROR            1
#define ABORT_REASON_WRITE_ERROR           2
#define ABORT_REASON_SEEK_ERROR            3
#define ABORT_REASON_FIFTY_PERCENT_FAILURE 4
#define ABORT_REASON_DEVICE_REMOVED        5

// Minimum screen size we need to display the curses interface
#define MIN_LINES 31
#define MIN_COLS 103

// Starting here: a bunch of constants to define where stuff is on screen

// The coordinates of the program name
#define PROGRAM_NAME_LABEL_Y 0
#define PROGRAM_NAME_LABEL_X 2

// The coordinates of the "Device:" label
#define DEVICE_NAME_LABEL_Y 0
#define DEVICE_NAME_LABEL_X (strlen(PROGRAM_NAME) + 4)

// The coordinates of the "Device Size:" label
#define DEVICE_SIZE_LABEL_Y (LINES - 6)
#define DEVICE_SIZE_LABEL_X 2

// The coordinates of the "Reported:" label
#define REPORTED_DEVICE_SIZE_LABEL_Y (LINES - 5)
#define REPORTED_DEVICE_SIZE_LABEL_X 4

// The coordinates of the "Detected:" label
#define DETECTED_DEVICE_SIZE_LABEL_Y (LINES - 4)
#define DETECTED_DEVICE_SIZE_LABEL_X 4

// The coordinates of the "Is fake flash:" label
#define IS_FAKE_FLASH_LABEL_Y (LINES - 3)
#define IS_FAKE_FLASH_LABEL_X 4

// The coordinates of the "Device speeds:" label
#define DEVICE_SPEEDS_LABEL_Y (LINES - 6)
#define DEVICE_SPEEDS_LABEL_X 50

// The coordinates of the "% Sectors Failed:" label
#define PERCENT_SECTORS_FAILED_LABEL_Y (LINES - 2)
#define PERCENT_SECTORS_FAILED_LABEL_X 2

// The coordinates of the "Sequential read:" label
#define SEQUENTIAL_READ_SPEED_LABEL_Y (LINES - 5)
#define SEQUENTIAL_READ_SPEED_LABEL_X 52

// The coordinates of the "Sequential write:" label
#define SEQUENTIAL_WRITE_SPEED_LABEL_Y (LINES - 4)
#define SEQUENTIAL_WRITE_SPEED_LABEL_X 52

// The coordinates of the "Random read:" label
#define RANDOM_READ_SPEED_LABEL_Y (LINES - 3)
#define RANDOM_READ_SPEED_LABEL_X 52

// The coordinates of the "Random write:" label
#define RANDOM_WRITE_SPEED_LABEL_Y (LINES - 2)
#define RANDOM_WRITE_SPEED_LABEL_X 52

// The coordinates of the white block character in the color key
#define COLOR_KEY_BLOCK_SIZE_BLOCK_Y 2
#define COLOR_KEY_BLOCK_SIZE_BLOCK_X (COLS - 37)

// The coordinates of the blue block character in the color key
#define COLOR_KEY_WRITTEN_BLOCK_Y 3
#define COLOR_KEY_WRITTEN_BLOCK_X (COLS - 37)

// The coordinates of the slash character between the blue and magenta block characters
#define COLOR_KEY_WRITTEN_SLASH_Y 3
#define COLOR_KEY_WRITTEN_SLASH_X (COLS - 36)

// The coordinates of the magenta block character in the color key
#define COLOR_KEY_WRITTEN_BAD_BLOCK_Y 3
#define COLOR_KEY_WRITTEN_BAD_BLOCK_X (COLS - 35)

// The coordinates of the green block character in the color key
#define COLOR_KEY_VERIFIED_BLOCK_Y 4
#define COLOR_KEY_VERIFIED_BLOCK_X (COLS - 37)

// The coordinates of the slash character between the green and yellow block characters
#define COLOR_KEY_VERIFIED_SLASH_Y 4
#define COLOR_KEY_VERIFIED_SLASH_X (COLS - 36)

// The coordinates of the yellow block character in the color key
#define COLOR_KEY_VERIFIED_BAD_BLOCK_Y 4
#define COLOR_KEY_VERIFIED_BAD_BLOCK_X (COLS - 35)

// The coordinates of the red block character in the color key
#define COLOR_KEY_FAILED_BLOCK_Y 5
#define COLOR_KEY_FAILED_BLOCK_X (COLS - 37)

// The coordinates of the slash character between the red block character and the black/yellow diamond character
#define COLOR_KEY_FAILED_SLASH_Y 5
#define COLOR_KEY_FAILED_SLASH_X (COLS - 36)

// The coordinates of the black diamond on yellow background character in the color key
#define COLOR_KEY_FAILED_THIS_ROUND_BLOCK_Y 5
#define COLOR_KEY_FAILED_THIS_ROUND_BLOCK_X (COLS - 35)

// The coordinates of the "=" next to the bytes per block display
#define BLOCK_SIZE_LABEL_Y 2
#define BLOCK_SIZE_LABEL_X (COLS - 33)

// The coordinates of the "= Written" label
#define WRITTEN_BLOCK_LABEL_Y 3
#define WRITTEN_BLOCK_LABEL_X (COLS - 33)

// The coordinates of the "= Verified" label
#define VERIFIED_BLOCK_LABEL_Y 4
#define VERIFIED_BLOCK_LABEL_X (COLS - 33)

// The coordinates of the "= Failed" label
#define FAILED_BLOCK_LABEL_Y 5
#define FAILED_BLOCK_LABEL_X (COLS - 33)

// The coordinates of the "Speed Class Qualifications:" label
#define SPEED_CLASS_QUALIFICATIONS_LABEL_Y 7
#define SPEED_CLASS_QUALIFICATIONS_LABEL_X (COLS - 37)

// The coordinates of the "Class 2:" label
#define SPEED_CLASS_2_LABEL_Y 8
#define SPEED_CLASS_2_LABEL_X (COLS - 35)

// The coordinates of the "Class 4:" label
#define SPEED_CLASS_4_LABEL_Y 9
#define SPEED_CLASS_4_LABEL_X (COLS - 35)

// The coordinates of the "Class 6:" label
#define SPEED_CLASS_6_LABEL_Y 10
#define SPEED_CLASS_6_LABEL_X (COLS - 35)

// The coordinates of the "Class 10:" label
#define SPEED_CLASS_10_LABEL_Y 11
#define SPEED_CLASS_10_LABEL_X (COLS - 35)

// The coordinates of the "U1:" label
#define SPEED_U1_LABEL_Y 13
#define SPEED_U1_LABEL_X (COLS - 35)

// The coordinates of the "U3:" label
#define SPEED_U3_LABEL_Y 14
#define SPEED_U3_LABEL_X (COLS - 35)

// The coordinates of the "V6:" label
#define SPEED_V6_LABEL_Y 16
#define SPEED_V6_LABEL_X (COLS - 35)

// The coordinates of the "V10:" label
#define SPEED_V10_LABEL_Y 17
#define SPEED_V10_LABEL_X (COLS - 35)

// The coordinates of the "V30:" label
#define SPEED_V30_LABEL_Y 18
#define SPEED_V30_LABEL_X (COLS - 35)

// The coordinates of the "V60:" label
#define SPEED_V60_LABEL_Y 19
#define SPEED_V60_LABEL_X (COLS - 35)

// The coordinates of the "V90:" label
#define SPEED_V90_LABEL_Y 20
#define SPEED_V90_LABEL_X (COLS - 35)

// The coordinates of the "A1:" label
#define SPEED_A1_LABEL_Y 22
#define SPEED_A1_LABEL_X (COLS - 35)

// The coordinates of the "A2:" label
#define SPEED_A2_LABEL_Y 23
#define SPEED_A2_LABEL_X (COLS - 35)

// The coordinates of the device name display
#define DEVICE_NAME_DISPLAY_Y 0
#define DEVICE_NAME_DISPLAY_X (strlen(PROGRAM_NAME) + 13)

// The coordinates of the "Reading"/"Writing" status text
#define READWRITE_DISPLAY_Y 0
#define READWRITE_DISPLAY_X (COLS - 30)

// The coordinates of the round number display
#define ROUNDNUM_DISPLAY_Y 0
#define ROUNDNUM_DISPLAY_X(x) (COLS - (x + 32))

// The coordinates of the stress test speed indicator
#define STRESS_TEST_SPEED_DISPLAY_Y 0
#define STRESS_TEST_SPEED_DISPLAY_X (COLS - 19)

// The starting coordinates of the sector map on the display
#define SECTOR_DISPLAY_Y 1
#define SECTOR_DISPLAY_X 2

// The coordinates of where to display the bytes per block on screen.
#define BLOCK_SIZE_DISPLAY_Y 2
#define BLOCK_SIZE_DISPLAY_X (COLS - 31)

// The coordinates of where to print the "Is fake flash" result
#define IS_FAKE_FLASH_DISPLAY_Y (LINES - 3)
#define IS_FAKE_FLASH_DISPLAY_X 19

// The coordinates of where to print the "% sectors failed" result
#define PERCENT_SECTORS_FAILED_DISPLAY_Y (LINES - 2)
#define PERCENT_SECTORS_FAILED_DISPLAY_X 20

// The coordinates of the reported device size
#define REPORTED_DEVICE_SIZE_DISPLAY_Y (LINES - 5)
#define REPORTED_DEVICE_SIZE_DISPLAY_X 19

// The coordinates of the detected device size
#define DETECTED_DEVICE_SIZE_DISPLAY_Y (LINES - 4)
#define DETECTED_DEVICE_SIZE_DISPLAY_X 19

// The coordinates of the sequential read speed result
#define SEQUENTIAL_READ_SPEED_DISPLAY_Y (LINES - 5)
#define SEQUENTIAL_READ_SPEED_DISPLAY_X 70

// The coordinates of the sequential write speed result
#define SEQUENTIAL_WRITE_SPEED_DISPLAY_Y (LINES - 4)
#define SEQUENTIAL_WRITE_SPEED_DISPLAY_X 70

// The coordinates of the random read speed result
#define RANDOM_READ_SPEED_DISPLAY_Y (LINES - 3)
#define RANDOM_READ_SPEED_DISPLAY_X 70

// The coordinates of the random write speed result
#define RANDOM_WRITE_SPEED_DISPLAY_Y (LINES - 2)
#define RANDOM_WRITE_SPEED_DISPLAY_X 70

// The coordinates of the Class 2 result
#define SPEED_CLASS_2_RESULT_Y 8
#define SPEED_CLASS_2_RESULT_X (COLS - 25)

// The coordinates of the Class 4 result
#define SPEED_CLASS_4_RESULT_Y 9
#define SPEED_CLASS_4_RESULT_X (COLS - 25)

// The coordinates of the Class 6 result
#define SPEED_CLASS_6_RESULT_Y 10
#define SPEED_CLASS_6_RESULT_X (COLS - 25)

// The coordinates of the Class 10 result
#define SPEED_CLASS_10_RESULT_Y 11
#define SPEED_CLASS_10_RESULT_X (COLS - 25)

// The coordinates of the U1 result
#define SPEED_U1_RESULT_Y 13
#define SPEED_U1_RESULT_X (COLS - 25)

// The coordinates of the U3 result
#define SPEED_U3_RESULT_Y 14
#define SPEED_U3_RESULT_X (COLS - 25)

// The coordinates of the V6 result
#define SPEED_V6_RESULT_Y 16
#define SPEED_V6_RESULT_X (COLS - 25)

// The coordinates of the V10 result
#define SPEED_V10_RESULT_Y 17
#define SPEED_V10_RESULT_X (COLS - 25)

// The coordinates of the V30 result
#define SPEED_V30_RESULT_Y 18
#define SPEED_V30_RESULT_X (COLS - 25)

// The coordinates of the V60 result
#define SPEED_V60_RESULT_Y 19
#define SPEED_V60_RESULT_X (COLS - 25)

// The coordinates of the V90 result
#define SPEED_V90_RESULT_Y 20
#define SPEED_V90_RESULT_X (COLS - 25)

// The coordinates of the A1 result
#define SPEED_A1_RESULT_Y 22
#define SPEED_A1_RESULT_X (COLS - 25)

// The coordinates of the A2 result
#define SPEED_A2_RESULT_Y 23
#define SPEED_A2_RESULT_X (COLS - 25)

// Return values for load_state()
#define LOAD_STATE_SUCCESS 0
#define LOAD_STATE_FILE_NOT_SPECIFIED 1
#define LOAD_STATE_FILE_DOES_NOT_EXIST 2
#define LOAD_STATE_LOAD_ERROR 3

// Bit flags for the sector map
#define SECTOR_MAP_FLAG_FAILED_THIS_ROUND  0x08
#define SECTOR_MAP_FLAG_READ_THIS_ROUND    0x04
#define SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND 0x02
#define SECTOR_MAP_FLAG_FAILED             0x01

/**
 * Log the given string to the log file, if the log file is open.  If curses
 * mode is turned off, also log the given string to stdout.  The time is
 * prepended to the message, and a newline is appended to the message.
 *
 * @param msg       The null-terminated string to write to the log file.
 */
void log_log(char *msg);

/**
 * Decodes the UUID embedded in the sector data (specified by data) and places
 * it into buffer specified by uuid_buffer.
 *
 * @param data         A pointer to a buffer containing sector data that the
 *                     UUID will be extracted from.  The buffer is expected to
 *                     be at least 290 bytes long.
 * @param uuid_buffer  A pointer to a buffer where the extracted UUID will be
 *                     placed.
 */
void get_embedded_device_uuid(char *data, char *uuid_buffer);

typedef struct _program_options_type {
    char *stats_file;
    char *log_file;
    char *device_name;
    uint64_t stats_interval;
    unsigned char probe_for_optimal_block_size;
    char no_curses;      // What's the current setting of no-curses?
    char orig_no_curses; // What was passed on the command line?
    char dont_show_warning_message;
    char *lock_file;
    char *state_file;
    uint64_t force_sectors;
} program_options_type;

extern program_options_type program_options;

typedef enum {
    FAKE_FLASH_UNKNOWN,
    FAKE_FLASH_YES,
    FAKE_FLASH_NO
} FakeFlashEnum;

// Global variables
typedef struct _device_stats_type {
    uint64_t num_sectors;
    uint64_t num_bad_sectors;
    uint64_t bytes_since_last_status_update;
    uint64_t reported_size_bytes;
    uint64_t detected_size_bytes;
    int sector_size;
    unsigned int physical_sector_size;
    int preferred_block_size;
    int block_size;
    int max_request_size;
    dev_t device_num;
    FakeFlashEnum is_fake_flash;
    uuid_t device_uuid;
} device_stats_type;

extern device_stats_type device_stats;

typedef struct _device_speeds_type {
    double sequential_write_speed;
    double sequential_read_speed;
    double random_write_iops;
    double random_read_iops;
} device_speeds_type;

extern device_speeds_type device_speeds;

typedef struct _sector_display_type {
    uint64_t sectors_per_block;
    char *sector_map;
    uint64_t sectors_in_last_block;
    uint64_t num_blocks;
    uint64_t num_lines;
    uint64_t blocks_per_line;
} sector_display_type;

extern sector_display_type sector_display;

// To handle device disconnects/reconnects, we're going to create a couple of
// buffers where we hold the most recent 1MB of data that we wrote to the
// beginning of the device (BOD) and middle of the device (MOD).  When a device
// is reconnected, we'll read back those two segments.  One segment needs to be
// a 100% match; we can be fuzzy about how much of the other segment matches.
// I'm taking this approach in case we were in the middle of writing to one of
// these two segments when the device was disconnected and we're not sure how
// much of the data was actually committed (e.g., actually made it out of the
// device's write cache and into permanent storage).
extern char bod_buffer[BOD_MOD_BUFFER_SIZE];
extern char mod_buffer[BOD_MOD_BUFFER_SIZE];

extern int64_t num_rounds;

typedef struct _state_data_type {
    uint64_t bytes_read;
    uint64_t bytes_written;
    int64_t first_failure_round;
    int64_t ten_percent_failure_round;
    int64_t twenty_five_percent_failure_round;
} state_data_type;

extern state_data_type state_data;

#endif // !defined(__MFST_H)

