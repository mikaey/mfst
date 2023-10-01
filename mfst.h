#if !defined(__MFST_H)
#define __MFST_H

#define VERSION "0.2"
#define PROGRAM_NAME " Mikaey's Flash Stress Test v" VERSION " "

// Periodicity of the random() function (cccording to the man page)
#define RANDOM_PERIOD 34359738352

// Number of seconds to profile the RNG
#define RNG_PROFILE_SECS 5

// So that I don't have to memorize the different color pairs
#define BLACK_ON_WHITE 1
#define BLACK_ON_BLUE  2
#define BLACK_ON_GREEN 3
#define BLACK_ON_RED   4
#define GREEN_ON_BLACK 5
#define RED_ON_BLACK   6

// Abort reasons
#define ABORT_REASON_READ_ERROR            1
#define ABORT_REASON_WRITE_ERROR           2
#define ABORT_REASON_SEEK_ERROR            3
#define ABORT_REASON_FIFTY_PERCENT_FAILURE 4
#define ABORT_REASON_DEVICE_REMOVED        5

// Minimum screen size we need to display the curses interface
#define MIN_LINES 31
#define MIN_COLS 103

// The starting coordinates of the sector map on the display
#define SECTOR_DISPLAY_Y 1
#define SECTOR_DISPLAY_X 2

// The coordinates of where to display the bytes per block on screen.
#define BYTES_PER_BLOCK_Y 2
#define BYTES_PER_BLOCK_X (COLS - 33)

// The coordinates of where to print the "Is fake flash" result
#define IS_FAKE_FLASH_Y (LINES - 3)
#define IS_FAKE_FLASH_X 19

#endif

