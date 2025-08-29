#if !defined(__MFST_H)
#define __MFST_H

#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>
#include <uuid/uuid.h>

#include "device_testing_context.h"

#define VERSION "0.4"
#define PROGRAM_NAME " Mikaey's Flash Stress Test v" VERSION " "

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

// The coordinates of the "SQL status:" label
#define SQL_STATUS_LABEL_Y (LINES - 7)
#define SQL_STATUS_LABEL_X 2

// The coordinates of the SQL status
#define SQL_STATUS_Y (LINES - 7)
#define SQL_STATUS_X 14

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
#define SECTOR_MAP_FLAG_DO_NOT_USE         0x10
#define SECTOR_MAP_FLAG_FAILED_THIS_ROUND  0x08
#define SECTOR_MAP_FLAG_READ_THIS_ROUND    0x04
#define SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND 0x02
#define SECTOR_MAP_FLAG_FAILED             0x01

// Log levels
#define SEVERITY_LEVEL_INFO          0
#define SEVERITY_LEVEL_ERROR         1
#define SEVERITY_LEVEL_WARNING       2
#define SEVERITY_LEVEL_DEBUG         3
#define SEVERITY_LEVEL_DEBUG_VERBOSE 4

/**
 * Log the given string to the log file, if the log file is open.  If curses
 * mode is turned off, also log the given string to stdout.  The time is
 * prepended to the message, and a newline is appended to the message.
 *
 * This function is thread-safe.
 *
 * @param device_testing_context  The device to which the message applies.
 * @param funcname                The name of the calling function.  May be
 *                                NULL.  If not set to NULL, it will be included
 *                                in the message.
 * @param severity                The severity of the message to be logged.
 *                                Right now, the severity is just included with
 *                                the log message; in future versions, a command
 *                                line option will be introduced to allow
 *                                filtering of log messages based on severity.
 * @param msg                     The message number (e.g., the index of the
 *                                message in the log_file_messages array).
 * @param ...                     Parameters for any printf-style format
 *                                specifiers that appear in the message.
 */
void log_log(device_testing_context_type *device_testing_context, const char *funcname, int severity, int msg, ...);

/**
 * Redraws the entire screen.  Useful on initial setup or when the screen has
 * been resized.
 */
void redraw_screen();

/**
 * Returns the maximum number of contiguous writable sectors that can be written
 * starting from the given starting_sector, up to a max of max_sectors.
 * "Writable sectors" are those sectors that have not been previously marked bad
 * due to unrecoverable I/O errors.
 *
 * @param starting_sector  The sector number at which to start searching.
 * @param max_sectors      The maximum number of sectors to search.
 *
 * @returns The maximum number of contiguous sectors that can be written
 *          starting from starting_sector (which may be 0).
 */
uint64_t get_max_writable_sectors(device_testing_context_type *device_testing_context, uint64_t starting_sector, uint64_t max_sectors);

/**
 * Returns the maximum number of contiguous sectors that have been marked as
 * "unwritable", starting from the given starting_sector, up to a max of
 * max_sectors.  A sector is marked "unwritable" if a previous read attempt
 * resulted in I/O errors that were not resolved by the program's various retry
 * mechanisms.
 */
uint64_t get_max_unwritable_sectors(device_testing_context_type *device_testing_context, uint64_t starting_sector, uint64_t max_sectors);

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
    char *db_host;
    char *db_user;
    char *db_pass;
    char *db_name;
    int db_port;
    char *card_name;
    uint64_t card_id;
} program_options_type;

extern program_options_type program_options;

typedef struct _sector_display_type {
    uint64_t sectors_per_block;
    uint64_t sectors_in_last_block;
    uint64_t num_blocks;
    uint64_t num_lines;
    uint64_t blocks_per_line;
} sector_display_type;

extern sector_display_type sector_display;

typedef enum {
              MAIN_THREAD_STATUS_IDLE                = 0, // Status hasn't been set yet
              MAIN_THREAD_STATUS_PAUSED              = 1, // Main thread is paused waiting for the lockfile
              MAIN_THREAD_STATUS_WRITING             = 2, // Main thread is writing
              MAIN_THREAD_STATUS_READING             = 3, // Main thread is reading
              MAIN_THREAD_STATUS_DEVICE_DISCONNECTED = 4, // Device has disconnected and the main thread is waiting for it to be reconnected
              MAIN_THREAD_STATUS_ENDING              = 5  // Main thread is showing the failure dialog and will end once the user acknowledges
} main_thread_status_type;

extern volatile main_thread_status_type main_thread_status;

extern const char *WARNING_TITLE;
extern const char *ERROR_TITLE;

extern char speed_qualifications_shown;

#endif // !defined(__MFST_H)

