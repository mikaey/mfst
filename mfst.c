#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE

#include <assert.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "block_size_test.h"
#include "crc32.h"
#include "device.h"
#include "lockfile.h"
#include "messages.h"
#include "mfst.h"
#include "ncurses.h"
#include "rng.h"
#include "state.h"
#include "sql.h"
#include "util.h"

// Number of slices per round of endurance testing
#define NUM_SLICES 16

// Since we use these strings so frequently, these are just here to save space
const char *WARNING_TITLE = "WARNING";
const char *ERROR_TITLE = "ERROR";

struct {
    FILE *log_file;
    FILE *stats_file;
} file_handles;

struct {
    struct timeval previous_update_time;
    uint64_t previous_bytes_written;
    uint64_t previous_bytes_read;
    uint64_t previous_bad_sectors;
} stress_test_stats;

unsigned long initial_seed;
char speed_qualifications_shown;
char ncurses_active;
char *forced_device;
int is_writing;

state_data_type state_data;
sector_display_type sector_display;
device_speeds_type device_speeds;
device_stats_type device_stats;
program_options_type program_options;
char bod_buffer[BOD_MOD_BUFFER_SIZE];
char mod_buffer[BOD_MOD_BUFFER_SIZE];
volatile int64_t num_rounds;

volatile main_thread_status_type main_thread_status;

volatile int log_log_lock = 0;

static struct timeval stats_cur_time;
static uint64_t num_bad_sectors;
static uint64_t num_bad_sectors_this_round;

// Scratch buffer for messages; we're allocating it statically so that we can
// still log messages in case of memory shortages
static char msg_buffer[512];

void log_log(const char *funcname, int severity, int msg, ...) {
    va_list ap;

    va_start(ap, msg);

    time_t now = time(NULL);
    char *t = ctime(&now);
    // Get rid of the newline on the end of the time
    t[strlen(t) - 1] = 0;

    while(log_log_lock) {
        usleep(1);
    }

    log_log_lock = 1;

    if(file_handles.log_file) {
        fprintf(file_handles.log_file, "[%s] [%s] ", t, severity == 0 ? "INFO" : (severity == 1 ? "ERROR" : (severity == 2 ? "WARNING" : "DEBUG")));

        if(funcname) {
            fprintf(file_handles.log_file, "%s(): ", funcname);
        }

        vfprintf(file_handles.log_file, log_file_messages[msg], ap);
        fprintf(file_handles.log_file, "\n");
        fflush(file_handles.log_file);
    }

    if(program_options.no_curses) {
        printf("[%s] [%s] ", t, severity == 0 ? "INFO" : (severity == 1 ? "ERROR" : (severity == 2 ? "WARNING" : "DEBUG")));

        if(funcname) {
            printf("%s(): ", funcname);
        }

        vprintf(log_file_messages[msg], ap);
        printf("\n");
        syncfs(1);
    }

    va_end(ap);
    log_log_lock = 0;
}

/**
 * Log the given stats to the stats file.  The stats file is a CSV file with
 * the following columns:
 *
 * * Date/time at which the stats were logged
 * * The number of read/write/verify rounds completed against the device so far
 * * Bytes written to the device since the timestamp indicated in the previous
 *   row (or since the start of the stress test, if this is the first row)
 * * The write rate (in bytes/sec) since the timestamp indicated in the
 *   previous row (or since the start of the stress test, if this is the first
 *   row)
 * * Bytes read from the device since the timestamp indicated in the previous
 *   row (or since the start of the stress test, if this is the first row)
 * * The read rate (in bytes/sec) since the timestamp indicated in the previous
 *   row (or since the start of the stress test, if this is the first row)
 * * The number of sectors that have newly failed verification since the
 *   timestamp indicated in the previous row (or since the start of the stress
 *   test, if this is the first row -- note that sectors which failed
 *   verification during a previous round of testing are not included in this
 *   number)
 * * The rate at which sectors are failing verification (in counts/minut) --
 *   note that sectors which failed verification during a previous round of
 *   testing are not accounted for in this number)
 * 
 * @param rounds         The number of read/write/verify rounds that have been
 *                       completed against the device so far.
 * @param bytes_written  The total number of bytes written to the device so
 *                       far.
 * @param bytes_read     The total number of bytes read from the device so far.
 * @param bad_sectors    The total number of sectors that have failed
 *                       verification so far.
 */
void stats_log(uint64_t rounds, uint64_t bad_sectors) {
    double write_rate, read_rate, bad_sector_rate;
    time_t now = time(NULL);
    char *ctime_str;
    struct timeval micronow;

    assert(!gettimeofday(&micronow, NULL));

    if(!file_handles.stats_file) {
        return;
    }

    ctime_str = ctime(&now);

    // Trim off the training newline from ctime_str
    ctime_str[strlen(ctime_str) - 1] = 0;
    write_rate = ((double)(state_data.bytes_written - stress_test_stats.previous_bytes_written)) /
        (((double)timediff(stress_test_stats.previous_update_time, micronow)) / 1000000);
    read_rate = ((double)(state_data.bytes_read - stress_test_stats.previous_bytes_read)) /
        (((double)timediff(stress_test_stats.previous_update_time, micronow)) / 1000000);
    bad_sector_rate = ((double)(bad_sectors - stress_test_stats.previous_bad_sectors)) /
        (((double)timediff(stress_test_stats.previous_update_time, micronow)) / 60000000);

    fprintf(file_handles.stats_file, "%s,%lu,%lu,%lu,%0.2f,%lu,%lu,%0.2f,%lu,%lu,%0.2f\n", ctime_str, rounds,
        state_data.bytes_written - stress_test_stats.previous_bytes_written, state_data.bytes_written, write_rate,
        state_data.bytes_read - stress_test_stats.previous_bytes_read, state_data.bytes_read,
        read_rate, bad_sectors, device_stats.num_bad_sectors, bad_sector_rate);
    fflush(file_handles.stats_file);

    assert(!gettimeofday(&stress_test_stats.previous_update_time, NULL));
    stress_test_stats.previous_bytes_written = state_data.bytes_written;
    stress_test_stats.previous_bytes_read = state_data.bytes_read;
    stress_test_stats.previous_bad_sectors = bad_sectors;
}

/**
 * Draw the block containing the given sector in the given color.  The display
 * is not refreshed after the block is drawn.
 * 
 * @param sector_num    The sector number of the sector to draw.
 * @param color         The ID of the color pair specifying the colors to draw
 *                      the block in.
 * @param with_diamond  Non-zero to indicate that a diamond should be drawn in
 *                      the block, or 0 to indicate that it should be an empty
 *                      block.
 * @param with_x        Non-zero to indicate that an X should be drawn in the
 *                      block, or 0 to indicate that it should be an empty
 *                      block.
 */
void draw_sector(uint64_t sector_num, int color, int with_diamond, int with_x) {
    int block_num, row, col;

    if(program_options.no_curses) {
        return;
    }

    block_num = sector_num / sector_display.sectors_per_block;
    if(block_num >= sector_display.num_blocks) {
        row = sector_display.num_lines - 1;
        col = sector_display.blocks_per_line - 1;
    } else {
        row = block_num / sector_display.blocks_per_line;
        col = block_num - (row * sector_display.blocks_per_line);
    }

    attron(COLOR_PAIR(color));

    if(with_diamond) {
        mvaddch(row + SECTOR_DISPLAY_Y, col + SECTOR_DISPLAY_X, ACS_DIAMOND);
    } else if(with_x) {
        mvaddch(row + SECTOR_DISPLAY_Y, col + SECTOR_DISPLAY_X, 'X');
    } else {
        mvaddch(row + SECTOR_DISPLAY_Y, col + SECTOR_DISPLAY_X, ' ');
    }

    attroff(COLOR_PAIR(color));
}

/**
 * Redraw the blocks containing the given sectors.  The display is not
 * refreshed after the blocks are drawn.
 * 
 * @param start_sector  The sector number of the first sector to be redrawn.
 * @param end_sector    The sector number at which to stop redrawing.  (Ergo,
 *                      all sectors within the range [start_sector, end_sector)
 *                      are redrawn.)
 */
void draw_sectors(uint64_t start_sector, uint64_t end_sector) {
    uint64_t i, j, num_sectors_in_cur_block, num_written_sectors, num_read_sectors;
    uint64_t min, max;
    char cur_block_has_bad_sectors;
    int color;
    int this_round;
    int unwritable;

    min = start_sector / sector_display.sectors_per_block;
    max = (end_sector / sector_display.sectors_per_block) + ((end_sector % sector_display.sectors_per_block) ? 1 : 0);

    if(min >= sector_display.num_blocks) {
        min = sector_display.num_blocks - 1;
    }

    if(max > sector_display.num_blocks) {
        max = sector_display.num_blocks;
    }

    for(i = min; i < max; i++) {
        cur_block_has_bad_sectors = 0;
        num_written_sectors = 0;
        num_read_sectors = 0;

        if(i == (sector_display.num_blocks - 1)) {
            num_sectors_in_cur_block = sector_display.sectors_in_last_block;
        } else {
            num_sectors_in_cur_block = sector_display.sectors_per_block;
        }

        this_round = 0;
        unwritable = 0;

        for(j = i * sector_display.sectors_per_block; j < ((i * sector_display.sectors_per_block) + num_sectors_in_cur_block); j++) {
            cur_block_has_bad_sectors |= sector_display.sector_map[j] & SECTOR_MAP_FLAG_FAILED;
            num_written_sectors += (sector_display.sector_map[j] & SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND) >> 1;
            num_read_sectors += (sector_display.sector_map[j] & SECTOR_MAP_FLAG_READ_THIS_ROUND) >> 2;
            this_round |= sector_display.sector_map[j] & SECTOR_MAP_FLAG_FAILED_THIS_ROUND;
            unwritable |= sector_display.sector_map[j] & SECTOR_MAP_FLAG_DO_NOT_USE;
        }

        if(cur_block_has_bad_sectors) {
            if(num_read_sectors == num_sectors_in_cur_block) {
                color = BLACK_ON_YELLOW;
            } else if(num_written_sectors == num_sectors_in_cur_block) {
                color = BLACK_ON_MAGENTA;
            } else {
                color = BLACK_ON_RED;
            }
        } else if(num_read_sectors == num_sectors_in_cur_block) {
            color = BLACK_ON_GREEN;
        } else if(num_written_sectors == num_sectors_in_cur_block) {
            color = BLACK_ON_BLUE;
        } else {
            color = BLACK_ON_WHITE;
        }

        draw_sector(i * sector_display.sectors_per_block, color, this_round, unwritable);
    }
}

/**
 * Mark the given sectors as "written" in the sector map.  The blocks
 * containing the given sectors are redrawn on the display.  The display is not
 * refreshed after the blocks are drawn.
 * 
 * @param start_sector  The sector number of the first sector to be marked as
 *                      written.
 * @param end_sector    The sector number at which to stop marking sectors as
 *                      written.  (Ergo, all sectors within the range
 *                      [start_sector, end_sector) are marked as written.)
 */
void mark_sectors_written(uint64_t start_sector, uint64_t end_sector) {
    uint64_t i;

    for(i = start_sector; i < end_sector && i < device_stats.num_sectors; i++) {
        sector_display.sector_map[i] |= SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND;
    }

    draw_sectors(start_sector, end_sector);
}

/**
 * Mark the given sectors as "read" in the sector map.  The blocks containing
 * the given sectors are redrawn on the display.  The display is not refreshed
 * after the blocks are drawn.
 * 
 * @param start_sector  The sector number of the first sector to be marked as
 *                      read.
 * @param end_sector    The sector number at which to stop marking sectors as
 *                      read.  (Ergo, all sectors within the range
 *                      [start_sector, end_sector) are marked as read.)
 */
void mark_sectors_read(uint64_t start_sector, uint64_t end_sector) {;
    uint64_t i;

    for(i = start_sector; i < end_sector && i < device_stats.num_sectors; i++) {
        sector_display.sector_map[i] |= SECTOR_MAP_FLAG_READ_THIS_ROUND;
    }

    draw_sectors(start_sector, end_sector);
}

/**
 * Draw the "% sectors bad" display.
 */
void draw_percentage() {
    float percent_bad;
    if(device_stats.num_sectors) {
        percent_bad = (((float) device_stats.num_bad_sectors) / ((float) device_stats.num_sectors)) * 100.0;
        mvprintw(PERCENT_SECTORS_FAILED_DISPLAY_Y, PERCENT_SECTORS_FAILED_DISPLAY_X, "%5.2f%%", percent_bad);
    } else {
        mvprintw(PERCENT_SECTORS_FAILED_DISPLAY_Y, PERCENT_SECTORS_FAILED_DISPLAY_X, "       ");
    }
}

/**
 * Mark the given sector as "bad" in the sector map.  The block containing the
 * given sector is redrawn on the display.  The display is not refreshed after
 * the blocks are drawn.
 * 
 * @param sector_num  The sector number of the sector to be marked as bad.
 */
void mark_sector_bad(uint64_t sector_num) {
    if(!(sector_display.sector_map[sector_num] & SECTOR_MAP_FLAG_FAILED)) {
        device_stats.num_bad_sectors++;
    }

    sector_display.sector_map[sector_num] |= SECTOR_MAP_FLAG_FAILED_THIS_ROUND | SECTOR_MAP_FLAG_FAILED;

    draw_sectors(sector_num, sector_num + 1);
    draw_percentage();
}

/**
 * Determines whether the given sector is marked as "bad" in the sector map.
 * 
 * @param sector_num  The sector number of the sector to query.
 * 
 * @returns Non-zero if the sector has been marked as "bad" in the sector map,
 *          zero otherwise.
 */
char is_sector_bad(uint64_t sector_num) {
    return sector_display.sector_map[sector_num] & SECTOR_MAP_FLAG_FAILED;
}

/**
 * Recomputes the parameters for displaying the sector map on the display, then
 * redraws the entire sector map.  The display is not refreshed after the sector
 * map is redrawn.  If sector_map is NULL, the sector map is not redrawn, but
 * the display parameters are still recomputed.
 */
void redraw_sector_map() {
    if(program_options.no_curses) {
        return;
    }

    sector_display.blocks_per_line = COLS - 41;
    sector_display.num_lines = LINES - 8;
    sector_display.num_blocks = sector_display.num_lines * sector_display.blocks_per_line;
    sector_display.sectors_per_block = device_stats.num_sectors / sector_display.num_blocks;
    sector_display.sectors_in_last_block = device_stats.num_sectors % sector_display.num_blocks + sector_display.sectors_per_block;

    mvprintw(BLOCK_SIZE_DISPLAY_Y, BLOCK_SIZE_DISPLAY_X, "%'lu bytes", sector_display.sectors_per_block * device_stats.sector_size);

    if(!sector_display.sector_map) {
        return;
    }

    draw_sectors(0, device_stats.num_sectors);
}

/**
 * Move to the given y/x position, enable the specified color, print out the
 * given string, the disable the specified color.  The cursor is not relocated
 * after the given string has been written.
 * 
 * @param y      The Y coordinate to move the cursor to.
 * @param x      The X coordinate to move the cursor to.
 * @param color  The index of the color pair to use.
 * @param str    The message to write to the screen.
 */
void print_with_color(int y, int x, int color, const char *str) {
    if(!program_options.no_curses) {
        attron(COLOR_PAIR(color));
        mvaddstr(y, x, str);
        attroff(COLOR_PAIR(color));
    }
}

/**
 * Using the results of the speed tests, print out the various SD speed class
 * markings and whether or not the speed results indicate that the card should
 * be displaying that mark.
 */
void print_class_marking_qualifications() {
    if(speed_qualifications_shown && !program_options.no_curses) {
        attron(A_BOLD);
        mvaddstr(SPEED_CLASS_QUALIFICATIONS_LABEL_Y, SPEED_CLASS_QUALIFICATIONS_LABEL_X, "Speed Class Qualifications:");
        mvaddstr(SPEED_CLASS_2_LABEL_Y , SPEED_CLASS_2_LABEL_X , "Class 2 :");
        mvaddstr(SPEED_CLASS_4_LABEL_Y , SPEED_CLASS_4_LABEL_X , "Class 4 :");
        mvaddstr(SPEED_CLASS_6_LABEL_Y , SPEED_CLASS_6_LABEL_X , "Class 6 :");
        mvaddstr(SPEED_CLASS_10_LABEL_Y, SPEED_CLASS_10_LABEL_X, "Class 10:");
        mvaddstr(SPEED_U1_LABEL_Y      , SPEED_U1_LABEL_X      , "U1      :");
        mvaddstr(SPEED_U3_LABEL_Y      , SPEED_U3_LABEL_X      , "U3      :");
        mvaddstr(SPEED_V6_LABEL_Y      , SPEED_V6_LABEL_X      , "V6      :");
        mvaddstr(SPEED_V10_LABEL_Y     , SPEED_V10_LABEL_X     , "V10     :");
        mvaddstr(SPEED_V30_LABEL_Y     , SPEED_V30_LABEL_X     , "V30     :");
        mvaddstr(SPEED_V60_LABEL_Y     , SPEED_V60_LABEL_X     , "V60     :");
        mvaddstr(SPEED_V90_LABEL_Y     , SPEED_V90_LABEL_X     , "V90     :");
        mvaddstr(SPEED_A1_LABEL_Y      , SPEED_A1_LABEL_X      , "A1      :");
        mvaddstr(SPEED_A2_LABEL_Y      , SPEED_A2_LABEL_X      , "A2      :");
        attroff(A_BOLD);
        
        if(device_speeds.sequential_write_speed) {
            if(device_speeds.sequential_write_speed >= 2000000) {
                print_with_color(SPEED_CLASS_2_RESULT_Y, SPEED_CLASS_2_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_2_RESULT_Y, SPEED_CLASS_2_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 4000000) {
                print_with_color(SPEED_CLASS_4_RESULT_Y, SPEED_CLASS_4_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_4_RESULT_Y, SPEED_CLASS_4_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 6000000) {
                print_with_color(SPEED_CLASS_6_RESULT_Y, SPEED_CLASS_6_RESULT_X, GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V6_RESULT_Y     , SPEED_V6_RESULT_X     , GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_6_RESULT_Y, SPEED_CLASS_6_RESULT_X, RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V6_RESULT_Y     , SPEED_V6_RESULT_X     , RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 10000000) {
                print_with_color(SPEED_CLASS_10_RESULT_Y, SPEED_CLASS_10_RESULT_X, GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_U1_RESULT_Y      , SPEED_U1_RESULT_X      , GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V10_RESULT_Y     , SPEED_V10_RESULT_X     , GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_10_RESULT_Y, SPEED_CLASS_10_RESULT_X, RED_ON_BLACK, "No     ");
                print_with_color(SPEED_U1_RESULT_Y      , SPEED_U1_RESULT_X      , RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V10_RESULT_Y     , SPEED_V10_RESULT_X     , RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 30000000) {
                print_with_color(SPEED_U3_RESULT_Y , SPEED_U3_RESULT_X , GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V30_RESULT_Y, SPEED_V30_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_U3_RESULT_Y , SPEED_U3_RESULT_X , RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V30_RESULT_Y, SPEED_V30_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 60000000) {
                print_with_color(SPEED_V60_RESULT_Y, SPEED_V60_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_V60_RESULT_Y, SPEED_V60_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_speeds.sequential_write_speed >= 90000000) {
                print_with_color(SPEED_V90_RESULT_Y, SPEED_V90_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_V90_RESULT_Y, SPEED_V90_RESULT_X, RED_ON_BLACK, "No     ");
            }
        } else {
            mvaddstr(SPEED_CLASS_2_RESULT_Y , SPEED_CLASS_2_RESULT_X , "Unknown");
            mvaddstr(SPEED_CLASS_4_RESULT_Y , SPEED_CLASS_4_RESULT_X , "Unknown");
            mvaddstr(SPEED_CLASS_6_RESULT_Y , SPEED_CLASS_6_RESULT_X , "Unknown");
            mvaddstr(SPEED_CLASS_10_RESULT_Y, SPEED_CLASS_10_RESULT_X, "Unknown");
            mvaddstr(SPEED_U1_RESULT_Y      , SPEED_U1_RESULT_X      , "Unknown");
            mvaddstr(SPEED_U3_RESULT_Y      , SPEED_U3_RESULT_X      , "Unknown");
            mvaddstr(SPEED_V6_RESULT_Y      , SPEED_V6_RESULT_X      , "Unknown");
            mvaddstr(SPEED_V10_RESULT_Y     , SPEED_V10_RESULT_X     , "Unknown");
            mvaddstr(SPEED_V30_RESULT_Y     , SPEED_V30_RESULT_X     , "Unknown");
            mvaddstr(SPEED_V60_RESULT_Y     , SPEED_V60_RESULT_X     , "Unknown");
            mvaddstr(SPEED_V90_RESULT_Y     , SPEED_V90_RESULT_X     , "Unknown");
        }

        if(device_speeds.random_read_iops && device_speeds.random_write_iops) {
            if(device_speeds.random_read_iops >= 2000 && device_speeds.random_write_iops >= 500) {
                print_with_color(SPEED_A1_RESULT_Y, SPEED_A1_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_A1_RESULT_Y, SPEED_A1_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_speeds.random_read_iops >= 4000 && device_speeds.random_write_iops >= 2000) {
                print_with_color(SPEED_A2_RESULT_Y, SPEED_A2_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_A2_RESULT_Y, SPEED_A2_RESULT_X, RED_ON_BLACK, "No     ");
            }
        } else {
            mvaddstr(SPEED_A1_RESULT_Y, SPEED_A1_RESULT_X, "Unknown");
            mvaddstr(SPEED_A2_RESULT_Y, SPEED_A2_RESULT_X, "Unknown");
        }
    }
}

void print_sql_status(sql_thread_status_type status) {
    mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "               ");

    switch(status) {
    case SQL_THREAD_NOT_CONNECTED:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Not connected"); break;
    case SQL_THREAD_CONNECTING:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Connecting"); break;
    case SQL_THREAD_CONNECTED:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Connected"); break;
    case SQL_THREAD_DISCONNECTED:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Disconnected"); break;
    case SQL_THREAD_QUERY_EXECUTING:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Executing query"); break;
    case SQL_THREAD_ERROR:
        mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "Error"); break;
    }
}

void redraw_screen() {
    char rate[13];
    int j;

    if(!program_options.no_curses) {
        box(stdscr, 0, 0);

        // Draw the labels for the bottom of the screen
        attron(A_BOLD);
        mvaddstr(PROGRAM_NAME_LABEL_Y          , PROGRAM_NAME_LABEL_X          , PROGRAM_NAME       );
        mvaddstr(DEVICE_SIZE_LABEL_Y           , DEVICE_SIZE_LABEL_X           , "Device size:"     );
        mvaddstr(REPORTED_DEVICE_SIZE_LABEL_Y  , REPORTED_DEVICE_SIZE_LABEL_X  , "Reported     :"   );
        mvaddstr(DETECTED_DEVICE_SIZE_LABEL_Y  , DETECTED_DEVICE_SIZE_LABEL_X  , "Detected     :"   );
        mvaddstr(IS_FAKE_FLASH_LABEL_Y         , IS_FAKE_FLASH_LABEL_X         , "Is fake flash:"   );
        mvaddstr(DEVICE_SPEEDS_LABEL_Y         , DEVICE_SPEEDS_LABEL_X         , "Device speeds:"   );
        mvaddstr(SEQUENTIAL_READ_SPEED_LABEL_Y , SEQUENTIAL_READ_SPEED_LABEL_X , "Sequential read :");
        mvaddstr(SEQUENTIAL_WRITE_SPEED_LABEL_Y, SEQUENTIAL_WRITE_SPEED_LABEL_X, "Sequential write:");
        mvaddstr(RANDOM_READ_SPEED_LABEL_Y     , RANDOM_READ_SPEED_LABEL_X     , "Random read     :");
        mvaddstr(RANDOM_WRITE_SPEED_LABEL_Y    , RANDOM_WRITE_SPEED_LABEL_X    , "Random write    :");
        mvaddstr(DEVICE_NAME_LABEL_Y           , DEVICE_NAME_LABEL_X           , " Device: "        );
        mvaddstr(PERCENT_SECTORS_FAILED_LABEL_Y, PERCENT_SECTORS_FAILED_LABEL_X, "% sectors failed:");
        if(program_options.db_host && program_options.db_user && program_options.db_pass && program_options.db_name) {
            mvaddstr(SQL_STATUS_LABEL_Y        , SQL_STATUS_LABEL_X            , "SQL status:"      );
        }

        attroff(A_BOLD);

        if(program_options.db_host && program_options.db_user && program_options.db_pass && program_options.db_name) {
            print_sql_status(sql_thread_status);
        }

        // Draw the device name
        if(program_options.device_name) {
            snprintf(msg_buffer, 23, "%s ", program_options.device_name);
            mvaddstr(DEVICE_NAME_DISPLAY_Y, DEVICE_NAME_DISPLAY_X, msg_buffer);
        }

        // Draw the color key for the right side of the screen
        attron(COLOR_PAIR(BLACK_ON_WHITE));
        mvaddstr(COLOR_KEY_BLOCK_SIZE_BLOCK_Y, COLOR_KEY_BLOCK_SIZE_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_WHITE));

        attron(COLOR_PAIR(BLACK_ON_BLUE));
        mvaddstr(COLOR_KEY_WRITTEN_BLOCK_Y, COLOR_KEY_WRITTEN_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_BLUE));

        mvaddstr(COLOR_KEY_WRITTEN_SLASH_Y, COLOR_KEY_WRITTEN_SLASH_X, "/");

        attron(COLOR_PAIR(BLACK_ON_MAGENTA));
        mvaddstr(COLOR_KEY_WRITTEN_BAD_BLOCK_Y, COLOR_KEY_WRITTEN_BAD_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_MAGENTA));

        attron(COLOR_PAIR(BLACK_ON_GREEN));
        mvaddstr(COLOR_KEY_VERIFIED_BLOCK_Y, COLOR_KEY_VERIFIED_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_GREEN));

        mvaddstr(COLOR_KEY_VERIFIED_SLASH_Y, COLOR_KEY_VERIFIED_SLASH_X, "/");

        attron(COLOR_PAIR(BLACK_ON_YELLOW));
        mvaddstr(COLOR_KEY_VERIFIED_BAD_BLOCK_Y, COLOR_KEY_VERIFIED_BAD_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_YELLOW));

        attron(COLOR_PAIR(BLACK_ON_RED));
        mvaddstr(COLOR_KEY_FAILED_BLOCK_Y, COLOR_KEY_FAILED_BLOCK_X, " ");
        attroff(COLOR_PAIR(BLACK_ON_RED));

        mvaddstr(COLOR_KEY_FAILED_SLASH_Y, COLOR_KEY_FAILED_SLASH_X, "/");

        attron(COLOR_PAIR(BLACK_ON_YELLOW));
        mvaddch(COLOR_KEY_FAILED_THIS_ROUND_BLOCK_Y, COLOR_KEY_FAILED_THIS_ROUND_BLOCK_X, ACS_DIAMOND);
        attroff(COLOR_PAIR(BLACK_ON_YELLOW));

        mvaddstr(BLOCK_SIZE_LABEL_Y    , BLOCK_SIZE_LABEL_X    , "="         );
        mvaddstr(WRITTEN_BLOCK_LABEL_Y , WRITTEN_BLOCK_LABEL_X , "= Written/failed previously" );
        mvaddstr(VERIFIED_BLOCK_LABEL_Y, VERIFIED_BLOCK_LABEL_X, "= Verified/failed previously");
        mvaddstr(FAILED_BLOCK_LABEL_Y  , FAILED_BLOCK_LABEL_X  , "= Failed/this round"  );

        if(num_rounds != -1) {
            j = snprintf(msg_buffer, sizeof(msg_buffer), " Round %'lu ", num_rounds + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), msg_buffer);
        }

        if(is_writing == 1) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        } else if(is_writing == 0) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        // Draw the reported size of the device if it's been determined
        if(device_stats.reported_size_bytes) {
            snprintf(msg_buffer, 26, "%'lu bytes", device_stats.reported_size_bytes);
            mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%-25s", msg_buffer);
        }

        // Draw the detected size of the device if it's been determined
        if(device_stats.detected_size_bytes) {
            snprintf(msg_buffer, 26, "%'lu bytes", device_stats.detected_size_bytes);
            mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%-25s", device_stats.detected_size_bytes ? msg_buffer : "");
        }

        if(device_stats.is_fake_flash == FAKE_FLASH_YES) {
            attron(COLOR_PAIR(RED_ON_BLACK));
            mvaddstr(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Yes");
            attroff(COLOR_PAIR(RED_ON_BLACK));
        } else if(device_stats.is_fake_flash == FAKE_FLASH_NO) {
            attron(COLOR_PAIR(GREEN_ON_BLACK));
            mvaddstr(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Probably not");
            attroff(COLOR_PAIR(GREEN_ON_BLACK));
        }

        if(sector_display.sectors_per_block) {
            mvprintw(BLOCK_SIZE_DISPLAY_Y, BLOCK_SIZE_DISPLAY_X, "%'lu bytes", sector_display.sectors_per_block * device_stats.sector_size);
        }

        if(device_speeds.sequential_read_speed) {
            mvaddstr(SEQUENTIAL_READ_SPEED_DISPLAY_Y, SEQUENTIAL_READ_SPEED_DISPLAY_X, format_rate(device_speeds.sequential_read_speed, msg_buffer, 31));
        }

        if(device_speeds.sequential_write_speed) {
            mvaddstr(SEQUENTIAL_WRITE_SPEED_DISPLAY_Y, SEQUENTIAL_WRITE_SPEED_DISPLAY_X, format_rate(device_speeds.sequential_write_speed, msg_buffer, 31));
        }

        if(device_speeds.random_read_iops) {
            mvprintw(RANDOM_READ_SPEED_DISPLAY_Y, RANDOM_READ_SPEED_DISPLAY_X, "%0.2f IOPS/s (%s)", device_speeds.random_read_iops,
                format_rate(device_speeds.random_read_iops * 4096, rate, sizeof(rate)));
        }

        if(device_speeds.random_write_iops) {
            mvprintw(RANDOM_WRITE_SPEED_DISPLAY_Y, RANDOM_WRITE_SPEED_DISPLAY_X, "%0.2f IOPS/s (%s)", device_speeds.random_write_iops,
                format_rate(device_speeds.random_write_iops * 4096, rate, sizeof(rate)));
        }

        if(device_speeds.sequential_read_speed != 0 || device_speeds.sequential_write_speed != 0 || device_speeds.random_read_iops != 0 ||
            device_speeds.random_write_iops != 0) {
            speed_qualifications_shown = 1;
        }

        print_class_marking_qualifications();
        redraw_sector_map();
        draw_percentage();
        refresh();
    }
}

/**
 * Waits for the lock on the lockfile to be released.
 * 
 * @param topwin  A pointer to the pointer to the window currently being shown
 *                on the display.  The contents of the window will be saved,
 *                and the window will be destroyed and recreated after the
 *                lockfile is overwritten.  The new pointer to the window will
 *                be stored in the location pointed to by topwin.
 */
void wait_for_file_lock(WINDOW **topwin) {
    WINDOW *window;
    FILE *memfile;
    main_thread_status_type previous_status;
    
    if(is_lockfile_locked()) {
        previous_status = main_thread_status;
        main_thread_status = MAIN_THREAD_STATUS_PAUSED;
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_WAITING_FOR_FILE_LOCK);
        if(!program_options.no_curses) {
            if(topwin) {
                assert(memfile = fmemopen(NULL, 131072, "r+"));
                putwin(*topwin, memfile);
                rewind(memfile);
            }

            window = message_window(stdscr, "Paused",
                                    "Another copy of this program is running "
                                    "its speed tests.  To increase the "
                                    "accuracy of those tests, we've paused "
                                    "what we're doing while the other program "
                                    "is running its speed tests.  Things will "
                                    "resume automatically here once the other "
                                    "program is finished.", 0);
        }

        while(is_lockfile_locked()) {
            if(!program_options.no_curses) {
                handle_key_inputs(window);

                // I'm not sure if napms() depends on curses being initialized,
                // so we'll play it safe and assume that it does.  If curses
                // isn't initialized, we'll just use sleep() instead and sleep
                // for a full second.  It shouldn't be a big deal.
                napms(100);
            } else {
                sleep(1);
            }
        }

        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_FILE_LOCK_RELEASED);
        main_thread_status = previous_status;

        // We're just going to redraw the whole thing, so we don't need to
        // worry about erasing it first
        if(!program_options.no_curses) {
            delwin(window);
            erase();

            redraw_screen();

            if(topwin) {
                *topwin = getwin(memfile);
                fclose(memfile);
                wrefresh(*topwin);
            }
        }
    }
}

/**
 * Profiles the system's random number generator.
 * 
 * The profile is run by generating random numbers for RNG_PROFILE_SECS
 * seconds and counting the number of random() calls that can be completed in
 * that time.
 * 
 * @returns The number of bytes per second the system is capable of generating.
 */
double profile_random_number_generator() {
    struct timeval start_time, end_time;
    int i;
    int64_t total_random_numbers_generated = 0;
    time_t diff;
    char rate_str[16];
    WINDOW *window;

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_PROFILING_RNG);
    window = message_window(stdscr, NULL, "Profiling random number generator...", 0);

    // Generate random numbers for 5 seconds.
    rng_init(0);
    assert(!gettimeofday(&start_time, NULL));
    do {
        for(i = 0; i < 100; i++) {
            rng_get_random_number();
            total_random_numbers_generated++;
        }
        assert(!gettimeofday(&end_time, NULL));
        handle_key_inputs(window);
        diff = timediff(start_time, end_time);
    } while(diff <= (RNG_PROFILE_SECS * 1000000));

    // Turn total number of random numbers into total number of bytes.
    total_random_numbers_generated *= sizeof(int);

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DONE_PROFILING_RNG);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_RNG_STATS, format_rate(((double) total_random_numbers_generated) / (((double) timediff(start_time, end_time)) / 1000000.0), rate_str, sizeof(rate_str)));

    if(window) {
        erase_and_delete_window(window);
    }

    if(total_random_numbers_generated < 471859200) {
        // Display a warning message to the user
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_RNG_TOO_SLOW);
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Your system is only able to generate %s of random data.  "
                 "The device may appear to be slower than it actually is, "
                 "and speed test results may be inaccurate.",
                 format_rate(((double) total_random_numbers_generated) / (((double) diff) / 1000000.0), rate_str, sizeof(rate_str)));
        message_window(stdscr, WARNING_TITLE, msg_buffer, 1);
    }

    return ((double) total_random_numbers_generated) / (((double) diff) / 1000000.0);
}

struct timeval last_update_time;
void print_status_update() {
    struct timeval cur_time;
    double rate;
    double secs_since_last_update;

    char str[18];

    assert(!gettimeofday(&cur_time, NULL));
    secs_since_last_update = cur_time.tv_sec - last_update_time.tv_sec;
    secs_since_last_update *= 1000000.0;
    secs_since_last_update += cur_time.tv_usec - last_update_time.tv_usec;
    secs_since_last_update /= 1000000.0;

    if(secs_since_last_update < 0.5) {
        return;
    }

    rate = device_stats.bytes_since_last_status_update / secs_since_last_update;
    device_stats.bytes_since_last_status_update = 0;

    if(!program_options.no_curses) {
        format_rate(rate, str, sizeof(str));
        mvprintw(STRESS_TEST_SPEED_DISPLAY_Y, STRESS_TEST_SPEED_DISPLAY_X, " %-15s", str);
    }

    assert(!gettimeofday(&last_update_time, NULL));
}

int write_data_to_device(int fd, void *buf, uint64_t len, uint64_t optimal_block_size) {
    uint64_t block_size, bytes_left, block_bytes_left;
    char *aligned_buf;
    int64_t ret;
    int iret;

    if(ret = posix_memalign((void **) &aligned_buf, sysconf(_SC_PAGESIZE), len)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        return -1;
    }

    block_size = len > optimal_block_size ? optimal_block_size : len;

    bytes_left = len;
    while(bytes_left) {
        block_bytes_left = block_size > bytes_left ? bytes_left : block_size;
        while(block_bytes_left) {
            // Make sure the data is in an aligned buffer
            memcpy(aligned_buf, ((char *) buf) + (len - bytes_left), block_bytes_left);
            if((ret = write(fd, aligned_buf, block_bytes_left)) == -1) {
                // In case free() modifies errno
                iret = errno;
                free(aligned_buf);
                errno = iret;
                return -1;
            }

            block_bytes_left -= ret;
            bytes_left -= ret;
        }
    }

    free(aligned_buf);
    return 0;
}

void io_error_during_size_probe() {
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_DEVICE_SIZE_TEST_DUE_TO_IO_ERROR);

    message_window(stdscr, WARNING_TITLE,
                   "We encountered an error while trying to determine the size "
                   "of the device.  It could be that the device was removed or "
                   "experienced an error and disconnected itself.  For now, "
                   "we'll assume that the device is the size it says it is -- "
                   "but if the device has actually been disconnected, the "
                   "remainder of the tests are going to fail pretty quickly.", 1);
}

void memory_error_during_size_probe(int errnum) {
    log_log("probe_device_size", SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(errnum));
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_DEVICE_SIZE_TEST_DUE_TO_MEMORY_ERROR);

    message_window(stdscr, WARNING_TITLE,
                   "We encountered an error while trying to allocate memory to "
                   "test the size of the device.  For now, we'll assume that "
                   "the device is the size it says it is -- but if the device "
                   "is fake flash, the remainder of the tests are going to "
                   "fail pretty quickly.", 1);
}

// This whole method assumes that no card is going to have a 16MB (or bigger)
// cache.  If it turns out that there are cards that do have bigger caches,
// then we might need to come back and revisit this.
uint64_t probe_device_size(int fd, uint64_t num_sectors, uint64_t optimal_block_size) {
    // Start out by writing to 9 different places on the card to minimize the
    // chances that the card is interspersed with good blocks.
    int errnum, iret;
    char *buf, *readbuf, keep_searching;
    unsigned int random_seed, i, bytes_left, ret;
    uint64_t initial_sectors[9];
    uint64_t low, high, cur, size, j;
    const uint64_t slice_size = 4194304;
    const uint64_t num_slices = 9;
    const uint64_t buf_size = slice_size * num_slices;
    WINDOW *window;

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_PROBING_FOR_DEVICE_SIZE);
    window = message_window(stdscr, NULL, "Probing for actual device size...", 0);

    if(iret = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), buf_size)) {
        erase_and_delete_window(window);

        memory_error_during_size_probe(iret);

        return 0;
    }

    if(iret = posix_memalign((void **) &readbuf, sysconf(_SC_PAGESIZE), buf_size)) {
        erase_and_delete_window(window);
        free(buf);

        memory_error_during_size_probe(iret);

        return 0;
    }

    random_seed = time(NULL);
    rng_init(random_seed);
    rng_fill_buffer(buf, buf_size);

    // Decide where we'll put the initial data.  The first and last writes will
    // go at the beginning and end of the card; the other writes will be at
    // random sectors in each 1/8th of the card.
    initial_sectors[0] = 0;
    initial_sectors[num_slices - 1] = num_sectors - (1 + (slice_size / device_stats.sector_size));

    // Make sure we don't overwrite the initial set of sectors
    low = slice_size / device_stats.sector_size;
    high = num_sectors / 8;

    for(i = 1; i < (num_slices - 1); i++) {
        initial_sectors[i] = low + ((rng_get_random_number() & RAND_MAX) % (high - low));
        low = (num_sectors / (num_slices - 1)) * i;
        
        if((initial_sectors[i] + (slice_size / device_stats.sector_size)) > low) {
            low = initial_sectors[i] + (slice_size / device_stats.sector_size);
        }

        if(i == 7) {
            // Make sure that the second-to-last slice doesnt overwrite the last slice
            high = num_sectors - (slice_size * 2);
        } else {
            high = (num_sectors / (num_slices - 1)) * (i + 1);
        }
    }

    // Write the blocks to the card.  We're going to write them in reverse order
    // so that if the card is caching some of the data when we go to read it
    // back, hopefully the stuff toward the end of the device will already be
    // flushed out of the cache.
    for(i = num_slices; i > 0; i--) {
        handle_key_inputs(window);
        if(lseek(fd, initial_sectors[i - 1] * device_stats.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;            
        }

        if(write_data_to_device(fd, buf + ((i - 1) * slice_size), slice_size, optimal_block_size)) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;
        }

        wait_for_file_lock(&window);
    }

    // We're going to repurpose high and low to hold the highest and lowest
    // possible sectors of the first "bad" sector
    low = 0;
    high = num_sectors;

    // Read the blocks back.
    for(i = 0; i < num_slices; i++) {
        handle_key_inputs(window);
        if(lseek(fd, initial_sectors[i] * device_stats.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;
        }

        bytes_left = slice_size;
        while(bytes_left) {
            wait_for_file_lock(&window);

            // For the read portion, we're just going to try to read the whole thing all at once
            if((ret = read(fd, readbuf + (slice_size - bytes_left), bytes_left)) == -1) {
                // Ignore a read failure and just zero out the remainder of the buffer instead
                memset(buf + (slice_size - bytes_left), 0, bytes_left);
                bytes_left = 0;
            } else {
                bytes_left -= ret;
            }
        }

        // Compare the two buffers, sector_size bytes at a time
        for(j = 0; j < slice_size; j += device_stats.sector_size) {
            if(memcmp(readbuf + j, buf + (i * slice_size) + j, device_stats.sector_size)) {
                // Are we at the beginning of the device?
                if(i == 0) {
                    // Are we at the very first block?
                    multifree(2, buf, readbuf);
                    if(j == 0) {
                        log_log(__func__, SEVERITY_LEVEL_WARNING, MSG_FIRST_SECTOR_ISNT_STABLE);
                        erase_and_delete_window(window);
                        message_window(stdscr, WARNING_TITLE,
                                       "The first sector of this device isn't "
                                       "stable.  This means we have no basis "
                                       "to figure out what the device's actual "
                                       "capacity is.  It could be that this is "
                                       "wraparound flash (which this program "
                                       "isn't designed to handle), that the "
                                       "first sector is bad, or that the "
                                       "device has no usable storage "
                                       "whatsoever.\n\nFor now, we'll assume "
                                       "that the device is the size it says it "
                                       "is -- but if it is actually fake "
                                       "flash, the endurance test is going to "
                                       "fail during the first round.", 1);

                        return 0;
                    } else {
                        erase_and_delete_window(window);
                        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, j);

                        return j;
                    }
                } else {
                    if(j > 0) {
                        erase_and_delete_window(window);
                        multifree(2, buf, readbuf);

                        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, (initial_sectors[i] * device_stats.sector_size) + j);

                        return (initial_sectors[i] * device_stats.sector_size) + j;
                    } else {
                        high = initial_sectors[i];
                        i = 9;
                        break;
                    }
                }
            } else {
                low = initial_sectors[i] + (j / device_stats.sector_size) + 1;
            }
        }
    }

    // If we didn't have any mismatches, then the card is probably good.
    if(high == num_sectors) {
        erase_and_delete_window(window);
        multifree(2, buf, readbuf);

        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, num_sectors * device_stats.sector_size);

        return num_sectors * device_stats.sector_size;
    }

    // Otherwise, start bisecting the area between low and high to figure out
    // where the first "bad" block is
    keep_searching = 1;

    while(keep_searching) {
        handle_key_inputs(window);
        // Select a 32MB area centered between low and high
        size = high - low;
        if(size > ((slice_size * num_slices) / device_stats.sector_size)) {
            cur = (size / 2) + low;
            size = (slice_size * num_slices) / device_stats.sector_size;
        } else {
            // The area between high and low isn't big enough to hold 32MB
            cur = low;

            // Give up after this round
            keep_searching = 0;
        }

        if(lseek(fd, cur * device_stats.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;
        }

        // Generate some more random data
        rng_fill_buffer(buf, slice_size * num_slices);
        if(write_data_to_device(fd, buf, slice_size * num_slices, optimal_block_size)) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;
        }

        if(lseek(fd, cur * device_stats.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe();

            return 0;
        }

        // Read the data back -- we're only going to read back half the data
        // to avoid the possibility that any part of the other half is cached
        for(i = 0; i < 4; i++) {
            handle_key_inputs(window);
            bytes_left = slice_size;
            while(bytes_left) {
                if((ret = read(fd, readbuf + (slice_size - bytes_left), bytes_left)) == -1) {
                    // Ignore a read failure and just zero out the remainder of the buffer instead
                    memset(buf + (slice_size - bytes_left), 0, bytes_left);
                    bytes_left = 0;
                } else {
                    bytes_left -= ret;
                }
            }

            // Compare the data, sector_size bytes at a time.
            for(j = 0; j < slice_size; j += device_stats.sector_size) {
                handle_key_inputs(window);
                if(memcmp(buf + (i * slice_size) + j, readbuf + j, device_stats.sector_size)) {
                    if(j > 0) {
                        erase_and_delete_window(window);
                        multifree(2, buf, readbuf);

                        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, (cur * device_stats.sector_size) + (i * slice_size) + j);

                        return (cur * device_stats.sector_size) + (i * slice_size) + j;
                    } else {
                        high = cur + (((i * slice_size) + j) / device_stats.sector_size);
                        i = 4;
                        break;
                    }
                }
            }

            if(i != 4) {
                // We verified all the data successfully, so the bad area has to be past where we are now
                low = cur + ((slice_size * 4) / device_stats.sector_size);
            }
        }
    }

    erase_and_delete_window(window);
    multifree(2, buf, readbuf);

    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, low * device_stats.sector_size);

    return low * device_stats.sector_size;
}

void lseek_error_during_speed_test(int errnum) {
    log_log("probe_device_speeds", SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_IO_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We got an error while trying to move around the device.  It "
             "could be that the device was removed or experienced an error and "
             "disconnected itself.  If that's the case, the remainder of the "
             "tests are going to fail pretty quickly.\n\nUnfortunately, this "
             "means that we won't be able to complete the speed tests.\n\nThe "
             "error we got was: %s", strerror(errnum));

    message_window(stdscr, WARNING_TITLE, msg_buffer, 1);
}

void io_error_during_speed_test(char write, int errnum) {
    log_log("probe_device_speeds", SEVERITY_LEVEL_DEBUG, write ? MSG_WRITE_ERROR : MSG_READ_ERROR, strerror(errnum));
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_IO_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We got an error while trying to %s the device.  It could be that "
             "the device was removed, experienced an error and disconnected "
             "itself, or set itself to read-only.\n\nUnfortunately, this means "
             "that we won't be able to complete the speed tests.\n\nThe error "
             "we got was: %s", write ? "write to" : "read from", strerror(errnum));

    message_window(stdscr, WARNING_TITLE, msg_buffer, 1);
}

int probe_device_speeds(int fd) {
    char *buf, wr, rd;
    uint64_t ctr, bytes_left, cur;
    int64_t ret;
    struct timeval start_time, cur_time;
    double secs, prev_secs;
    char rate[15];
    int local_errno;
    WINDOW *window;

    device_speeds.sequential_write_speed = 0;
    device_speeds.sequential_read_speed = 0;
    device_speeds.random_write_iops = 0;
    device_speeds.random_read_iops = 0;

    if(lock_lockfile()) {
        local_errno = errno;
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_LOCK_ERROR);

        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Unable to obtain a lock on the lockfile.  Unfortunately, "
                 "this means that we won't be able to run the speed tests.\n\n"
                 "The error we got was: %s", strerror(local_errno));

        message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
        return -1;
    }

    if(local_errno = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), device_stats.block_size < 4096 ? 4096 : device_stats.block_size)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(local_errno));
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_MEMORY_ERROR);

        unlock_lockfile();

        snprintf(msg_buffer, sizeof(msg_buffer),
                 "We couldn't allocate memory we need for the speed tests."
                 "Unfortunately, this means that we won't be able to run the "
                 "speed tests on this device.\n\nThe error we got was: %s",
                 strerror(local_errno));

        message_window(stdscr, WARNING_TITLE, msg_buffer, 1);
        return -1;
    }

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_SPEED_TEST_STARTING);
    window = message_window(stdscr, NULL, "Testing read/write speeds...", 0);

    for(rd = 0; rd < 2; rd++) {
        for(wr = 0; wr < 2; wr++) {
            ctr = 0;
            assert(!gettimeofday(&start_time, NULL));

            if(!rd) {
                if(lseek(fd, 0, SEEK_SET) == -1) {
                    local_errno = errno;
                    erase_and_delete_window(window);
                    free(buf);

                    lseek_error_during_speed_test(local_errno);

                    return -1;
                }
            }

            secs = 0;
            prev_secs = 0;
            while(secs < 30) {
                if(wr) {
                    rng_fill_buffer(buf, rd ? 4096 : device_stats.block_size);
                }

                bytes_left = rd ? 4096 : device_stats.block_size;
                while(bytes_left && secs < 30) {
                    handle_key_inputs(window);
                    if(rd) {
                        // Choose a random sector, aligned on a 4K boundary
                        cur = (((((uint64_t) rng_get_random_number()) << 32) | rng_get_random_number()) & 0x7FFFFFFFFFFFFFFF) %
                            (device_stats.num_sectors - (4096 / device_stats.sector_size)) & 0xFFFFFFFFFFFFFFF8;
                        if(lseek(fd, cur * device_stats.sector_size, SEEK_SET) == -1) {
                            erase_and_delete_window(window);
                            lseek_error_during_speed_test(local_errno);
                            free(buf);
                            unlock_lockfile();
                            return -1;
                        }
                    }

                    if(wr) {
                        ret = write(fd, buf, bytes_left);
                    } else {
                        ret = read(fd, buf, bytes_left);
                    }

                    if(ret == -1) {
                        local_errno = errno;
                        erase_and_delete_window(window);
                        free(buf);
                        unlock_lockfile();

                        io_error_during_speed_test(wr, local_errno);

                        return -1;
                    }

                    if(rd) {
                        ctr++;
                    } else {
                        ctr += ret;
                    }

                    bytes_left -= ret;

                    assert(!gettimeofday(&cur_time, NULL));
                    secs = ((double)timediff(start_time, cur_time)) / 1000000.0;

                    if(!program_options.no_curses) {
                        // Update the on-screen display every half second
                        if((secs - prev_secs) >= 0.5) {
                            if(rd) {
                                snprintf(msg_buffer, sizeof(msg_buffer), "%0.2f IOPS/s (%s)", ctr / secs, format_rate((ctr * 4096) / secs, rate, sizeof(rate)));
                                mvprintw(
                                    wr ? RANDOM_WRITE_SPEED_DISPLAY_Y : RANDOM_READ_SPEED_DISPLAY_Y,
                                    wr ? RANDOM_WRITE_SPEED_DISPLAY_X : RANDOM_READ_SPEED_DISPLAY_X, "%-28s", msg_buffer);
                            } else {
                                snprintf(msg_buffer, sizeof(msg_buffer), "%s", format_rate(ctr / secs, rate, sizeof(rate)));
                                mvprintw(
                                    wr ? SEQUENTIAL_WRITE_SPEED_DISPLAY_Y : SEQUENTIAL_READ_SPEED_DISPLAY_Y,
                                    wr ? SEQUENTIAL_WRITE_SPEED_DISPLAY_X : SEQUENTIAL_READ_SPEED_DISPLAY_X, "%-28s", msg_buffer);
                            }

                            refresh();
                            prev_secs = secs;
                        }
                    }
                }
            }

            if(rd) {
                log_log(NULL, SEVERITY_LEVEL_INFO, wr ? MSG_SPEED_TEST_RESULTS_RANDOM_WRITE_SPEED : MSG_SPEED_TEST_RESULTS_RANDOM_READ_SPEED, ctr / secs, format_rate((ctr * 4096) / secs, rate, sizeof(rate)));
                *(wr ? &device_speeds.random_write_iops : &device_speeds.random_read_iops) = ctr / secs;
            } else {
                log_log(NULL, SEVERITY_LEVEL_INFO, wr ? MSG_SPEED_TEST_RESULTS_SEQUENTIAL_WRITE_SPEED : MSG_SPEED_TEST_RESULTS_SEQUENTIAL_READ_SPEED, format_rate(ctr / secs, rate, sizeof(rate)));
                *(wr ? &device_speeds.sequential_write_speed : &device_speeds.sequential_read_speed) = ctr / secs;

                if(wr) {
                    speed_qualifications_shown = 1;
                    print_class_marking_qualifications();
                }
            }
        }
    }
    
    unlock_lockfile();

    erase_and_delete_window(window);

    // Show the speed class qualifications on the display
    print_class_marking_qualifications();

    // Print the speed class qualifications to the log.  We're doing it here
    // because we're going to use print_class_marking_qualifications() to
    // repaint them on the display, and we don't want to print them to the log
    // a second time if they've already been printed out.
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_SPEED_CLASS_QUALIFICATION_RESULTS);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_2, device_speeds.sequential_write_speed >= 2000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_4, device_speeds.sequential_write_speed >= 4000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_6, device_speeds.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_10, device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_U1, device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_U3, device_speeds.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V6, device_speeds.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V10, device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V30, device_speeds.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V60, device_speeds.sequential_write_speed >= 60000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V90, device_speeds.sequential_write_speed >= 90000000 ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_A1,
            (device_speeds.sequential_write_speed >= 10485760 && device_speeds.random_read_iops >= 1500 && device_speeds.random_write_iops >= 500) ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_A2,
            (device_speeds.sequential_write_speed >= 10485760 && device_speeds.random_read_iops >= 4000 && device_speeds.random_write_iops >= 2000) ? "Yes" : "No");
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);

    free(buf);
    return 0;
}

int *random_list() {
    int i, j, k, l, source[16], temp[16], *list;
    
    assert(list = malloc(sizeof(int) * 16));

    // Initialize the source list
    for(i = 0; i < 16; i++) {
        source[i] = i;
    }

    for(i = 0; i < 16; i++) {
        // Pick a new number and add it to the list
        j = (rng_get_random_number() & RAND_MAX) % (16 - i);
        list[i] = source[j];

        // Remove the item from the list
        for(k = 0, l = 0; k < (16 - i); k++) {
            if(k != j) {
                temp[l++] = source[k];
            }
        }

        // Transfer the temp list to the main list
        memcpy(source, temp, sizeof(int) * (15 - i));
    }

    return list;
}

/**
 * Get the starting sector for a slice.
 * 
 * @param slice_num  The slice for which to get the starting sector.
 * 
 * @returns The sector on which the slice starts.
*/
uint64_t get_slice_start(int slice_num) {
    return (device_stats.num_sectors / NUM_SLICES) * slice_num;
}

void print_device_summary(int64_t fifty_percent_failure_round, int64_t rounds_completed, int abort_reason) {
    char messages[7][384];
    char *out_messages[7];
    int i;

    const char *abort_reasons[] = {
                                  "(unknown)",
                                  "read_error",
                                  "write error",
                                  "seek error",
                                  "50% of sectors have failed",
                                  "device went away"
    };

    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_COMPLETE);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_REASON_FOR_ABORTING_TEST, abort_reason > 5 ? abort_reasons[0] : abort_reasons[abort_reason]);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_COMPLETED, rounds_completed);

    snprintf(messages[0], sizeof(messages[0]), "Reason for aborting test             : %s", abort_reason > 5 ? abort_reasons[0] : abort_reasons[abort_reason]);
    out_messages[0] = messages[0];
    snprintf(messages[1], sizeof(messages[1]), "Number of read/write cycles completed: %'lu", rounds_completed);
    out_messages[1] = messages[1];

    if(state_data.first_failure_round != -1) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_FIRST_FAILURE, state_data.first_failure_round);
        snprintf(messages[2], sizeof(messages[2]), "Read/write cycles to first failure   : %'lu", state_data.first_failure_round);
        out_messages[2] = messages[2];
    } else {
        out_messages[2] = NULL;
    }

    if(state_data.ten_percent_failure_round != -1) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_10_PERCENT_FAILURE, state_data.ten_percent_failure_round);
        snprintf(messages[3], sizeof(messages[3]), "Read/write cycles to 10%% failure     : %'lu", state_data.ten_percent_failure_round);
        out_messages[3] = messages[3];
    } else {
        out_messages[3] = NULL;
    }

    if(state_data.twenty_five_percent_failure_round != -1) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_25_PERCENT_FAILURE, state_data.twenty_five_percent_failure_round);
        snprintf(messages[4], sizeof(messages[4]), "Read/write cycles to 25%% failure     : %'lu", state_data.twenty_five_percent_failure_round);
        out_messages[4] = messages[4];
    } else {
        out_messages[4] = NULL;
    }

    if(fifty_percent_failure_round != -1) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_50_PERCENT_FAILURE, fifty_percent_failure_round);
        snprintf(messages[5], sizeof(messages[5]), "Read/write cycles to 50%% failure     : %'lu", fifty_percent_failure_round);
        out_messages[5] = messages[5];
    } else {
        out_messages[5] = NULL;
    }

    out_messages[6] = NULL;

    // Clear out msg_buffer
    msg_buffer[0] = 0;

    // Append the output messages together
    for(i = 0; out_messages[i]; i++) {
        if((sizeof(msg_buffer) - strlen(msg_buffer) - 1) > (i == 0 ? 0 : 1)) {
            if(i > 0) {
                strcat(msg_buffer, "\n");
            }

            if((sizeof(msg_buffer) - strlen(msg_buffer) - 1) > strlen(out_messages[i])) {
                strcat(msg_buffer, out_messages[i]);
            } else {
                strncat(msg_buffer, out_messages[i], sizeof(msg_buffer) - strlen(msg_buffer) - 1);
            }
        }
    }

    message_window(stdscr, "Test Complete", msg_buffer, 1);
}

/**
 * Initializes curses and sets up the color pairs that we frequently use.
 * 
 * @returns -1 if the screen is too small to hold the UI, 0 otherwise.
 */
int screen_setup() {
    setlocale(LC_ALL, "");
    initscr();

    if(LINES < MIN_LINES || COLS < MIN_COLS) {
        endwin();
        return -1;
    }

    start_color();
    cbreak();
    noecho();
    nonl();
    nodelay(stdscr, TRUE);
    curs_set(0);
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    init_pair(BLACK_ON_WHITE, COLOR_BLACK, COLOR_WHITE);
    init_pair(BLACK_ON_BLUE, COLOR_BLACK, COLOR_BLUE);
    init_pair(BLACK_ON_GREEN, COLOR_BLACK, COLOR_GREEN);
    init_pair(BLACK_ON_RED, COLOR_BLACK, COLOR_RED);
    init_pair(GREEN_ON_BLACK, COLOR_GREEN, COLOR_BLACK);
    init_pair(RED_ON_BLACK, COLOR_RED, COLOR_BLACK);
    init_pair(BLACK_ON_MAGENTA, COLOR_BLACK, COLOR_MAGENTA);
    init_pair(BLACK_ON_YELLOW, COLOR_BLACK, COLOR_YELLOW);

    ncurses_active = 1;
    return 0;
}

/**
 * Print the help.
 * 
 * @param program_name  The name of the program, as specified on the command
 *                      line.  The caller should set this to the value of
 *                      argv[0].
 */
void print_help(char *program_name) {
    printf("Usage: %s [ [-s | --stats-file filename] [-i | --stats-interval seconds]\n", program_name);
    printf("       [-l | --log-file filename] [-b | --probe-for-block-size]\n");
    printf("       [-n | --no-curses] [--this-will-destroy-my-device]\n");
    printf("       [-f | --lockfile filename] [-e | --sectors count]\n");
    printf("       [--dbhost hostname --dbuser username --dbpass password --dbname database\n");
    printf("       [--dbport port] [--cardname name|--cardid id]] device-name |\n");
    printf("       [-h | --help]]\n\n");
    printf("  device_name                    The device to test (for example, /dev/sdc).\n");
    printf("  -s|--stats-file filename       Write stats periodically to the given file.  If\n");
    printf("                                 the given file already exists, stats are\n");
    printf("                                 appended to the file instead of overwriting it.\n");
    printf("                                 Note that the program doesn't start writing\n");
    printf("                                 stats until the stress test starts.\n");
    printf("  -i|--stats-interval seconds    Change the interval at which stats are written\n");
    printf("                                 to the stats file.  Default: 60\n");
    printf("  -l|--log-file filename         Write log messages to the file filename.\n");
    printf("  -b|--probe-for-block-size      Probe the device to see what write block size\n");
    printf("                                 is fastest instead of relying on the maximum\n");
    printf("                                 number of sectors per request reported by the\n");
    printf("                                 kernel. Note that this process may take several\n");
    printf("                                 minutes to run, depending on the speed of the\n");
    printf("                                 device.\n");
    printf("  -n|--no-curses                 Don't use ncurses to display progress and\n");
    printf("                                 stats.  In this mode, log messages are printed\n");
    printf("                                 to stdout.  Note that this mode is\n");
    printf("                                 automatically enabled if stdout is not a\n");
    printf("                                 terminal or is too small to display the\n");
    printf("                                 interface.\n");
    printf("  --this-will-destroy-my-device  Bypass the 15-second delay at the start of the\n");
    printf("                                 program and start testing right away.  (Make\n");
    printf("                                 sure you understand what this program does\n");
    printf("                                 before using this option!)\n");
    printf("  -f|--lockfile filename         Use filename as the name for the lock file\n");
    printf("                                 instead of the default.  Default: mfst.lock\n");
    printf("  -e|--sectors count             Skip probing the size of the device and assume\n");
    printf("                                 that it is count sectors in size.\n");
    printf("  --force-device device_name     Force the program to use the specified device.\n");
    printf("                                 This option is only valid when resuming from a\n");
    printf("                                 state file.  Only use this option with\n");
    printf("                                 problematic devices and you are sure the device\n");
    printf("                                 you specify is the correct device.\n");
    printf("  --dbhost hostname              Name of the MySQL host to connect to.\n");
    printf("  --dbuser username              Username to use with the MySQL connection.\n");
    printf("  --dbpass password              Password to use with the MySQL connection.\n");
    printf("  --dbname database              Name of the database to use with the MySQL\n");
    printf("                                 connection.\n");
    printf("  --dbport port                  Port to use to connect to the MYSQL server.\n");
    printf("                                 Default: 3306\n");
    printf("  --cardname name                Name of the card to register in the database.\n");
    printf("  --cardid id                    Force data to be logged to the database using\n");
    printf("                                 the given card ID instead of auto-detecting or\n");
    printf("                                 registering the card.\n");
    printf("  -h|--help                      Display this help message.\n\n");
}

/**
 * Parse the command line arguments.  Parsed arguments are placed in the
 * program_options global struct.  If a particular option was not supplied on
 * the command line, it is set to its default value, which is:
 * 
 * * `NULL` for `-s`/`--stats-file` and `-l`/`--log-file`,
 * * `60` for `-i`/`--stats-interval`, and
 * * `0` for `-p`/`--probe` and `-n`/`--no-curses`.
 * 
 * @param argc  The number of arguments passed on the command line.  The caller
 *              should pass the unmodified value of `argc` that was supplied to
 *              `main()`.
 * @param argv  A pointer to an array of strings containing the command line
 *              arguments.  The caller should pass the unmodified value of
 *              `argv` that was supplied to `main()`.
 * 
 * @returns Zero if arguments were parsed successfully and the program should
 *          continue normally, or non-zero if the program should exit after
 *          this call completes (e.g., if -h was specified or if a required
 *          argument was missing).
 */
int parse_command_line_arguments(int argc, char **argv) {
    int optindex, c;
    struct option options[] = {
        { "stats-file"                 , required_argument, NULL, 's' },
        { "log-file"                   , required_argument, NULL, 'l' },
        { "probe-for-block-size"       , no_argument      , NULL, 'b' },
        { "stats-interval"             , required_argument, NULL, 'i' },
        { "no-curses"                  , no_argument      , NULL, 'n' },
        { "help"                       , no_argument      , NULL, 'h' },
        { "this-will-destroy-my-device", no_argument      , NULL, 2   },
        { "lockfile"                   , required_argument, NULL, 'f' },
        { "state-file"                 , required_argument, NULL, 't' },
        { "sectors"                    , required_argument, NULL, 'e' },
        { "force-device"               , required_argument, NULL, 3   },
        { "dbhost"                     , required_argument, NULL, 4   },
        { "dbuser"                     , required_argument, NULL, 5   },
        { "dbpass"                     , required_argument, NULL, 6   },
        { "dbname"                     , required_argument, NULL, 7   },
        { "dbport"                     , required_argument, NULL, 8   },
        { "cardid"                     , required_argument, NULL, 9   },
        { "cardname"                   , required_argument, NULL, 10  },
        { 0                            , 0                , 0   , 0   }
    };

    // Set the defaults for the command-line options
    memset(&program_options, 0, sizeof(program_options));
    program_options.stats_interval = 60;

    while(1) {
        c = getopt_long(argc, argv, "be:f:hi:l:ns:t:", options, &optindex);
        if(c == -1) {
            break;
        }

        switch(c) {
            case 2:
                program_options.dont_show_warning_message = 1; break;
            case 3:
                assert(forced_device = strdup(optarg)); break;
            case 4:
                assert(program_options.db_host = strdup(optarg)); break;
            case 5:
                assert(program_options.db_user = strdup(optarg)); break;
            case 6:
                assert(program_options.db_pass = strdup(optarg));
                // Mask the password so that it isn't visible to ps
                for(c = 0; c < strlen(argv[optind - 1]); c++) {
                    argv[optind - 1][c] = '*';
                }
                break;
            case 7:
                assert(program_options.db_name = strdup(optarg)); break;
            case 8:
                program_options.db_port = strtol(optarg, NULL, 10); break;
            case 9:
                program_options.card_id = strtoull(optarg, NULL, 10); break;
            case 10:
                assert(program_options.card_name = strdup(optarg)); break;
            case 'e':
                program_options.force_sectors = strtoull(optarg, NULL, 10); break;
            case 'f':
                assert(program_options.lock_file = strdup(optarg)); break;
            case 'h':
                print_help(argv[0]);
                return -1;
            case 'i':
                program_options.stats_interval = strtol(optarg, NULL, 10); break;
            case 'l':
                if(program_options.log_file) {
                    printf("Only one log file option may be specified on the command line.\n");
                    return -1;
                }

                assert(program_options.log_file = strdup(optarg)); break;
            case 'n':
                program_options.no_curses = 1;
                program_options.orig_no_curses = 1;
                break;
            case 'p':
                program_options.probe_for_optimal_block_size = 1; break;
            case 's':
                if(program_options.stats_file) {
                    printf("Only one stats file option may be specified on the command line.\n");
                    return -1;
                }

                assert(program_options.stats_file = strdup(optarg)); break;
            case 't':
                if(program_options.state_file) {
                    printf("Only one state file option may be specified on the command line.\n");
                    return -1;
                }

                assert(program_options.state_file = strdup(optarg)); break;
        }
    }

    if(optind < argc) {
        for(c = optind; c < argc; c++) {
            if(program_options.device_name) {
                printf("Only one device may be specified on the command line.\n");
                return -1;
            }

            assert(program_options.device_name = strdup(argv[c]));
        }
    }

    if(!program_options.device_name && !program_options.state_file) {
        print_help(argv[0]);
        return -1;
    }

    if(!program_options.lock_file) {
        program_options.lock_file = strdup("mfst.lock");
    }

    if(!program_options.db_port) {
        program_options.db_port = 3306;
    }

    return 0;
}

WINDOW *device_disconnected_message() {
    return message_window(stdscr, "Device Disconnected",
                          "The device has been disconnected.  It may have done "
                          "this on its own, or it may have been manually "
                          "removed (e.g., if someone pulled the device out of "
                          "its USB port).\n\nDon't worry -- just plug the "
                          "device back in.  We'll verify that it's the same "
                          "device, then resume the stress test automatically.", 0);
}

WINDOW *resetting_device_message() {
    return message_window(stdscr, "Attempting to reset device",
                          "The device has encountered an error.  We're "
                          "attempting to reset the device to see if that fixes "
                          "the issue.  You shouldn't need to do anything -- "
                          "but if this message stays up for a while, it might "
                          "indicate that the device has failed or isn't "
                          "handling the reset well.  In that case, you can try "
                          "unplugging the device and plugging it back in to "
                          "get the device working again.", 0);
}

int64_t lseek_or_reset_device(int *fd, off_t position, int *device_was_disconnected);

int handle_device_disconnect(int *fd, off_t position, int seek_after_reconnect) {
    WINDOW *window;
    char *new_device_name;
    dev_t new_device_num;
    int ret;
    main_thread_status_type previous_status = main_thread_status;

    main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;

    if(*fd != -1) {
        close(*fd);
        *fd = -1;
    }

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_DISCONNECTED);
    window = device_disconnected_message();
    ret = wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, device_stats.device_uuid, sector_display.sector_map, &new_device_name, &new_device_num, fd);
    handle_key_inputs(window);
    main_thread_status = previous_status;

    if(ret != -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RECONNECTED, new_device_name);

        if(program_options.device_name) {
            free(program_options.device_name);
        }

        program_options.device_name = new_device_name;
        device_stats.device_num = new_device_num;

        if(seek_after_reconnect) {
            if(lseek_or_reset_device(fd, position, NULL) == -1) {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);

                erase_and_delete_window(window);
                redraw_screen();

                return -1;
            }
        }

        erase_and_delete_window(window);
        redraw_screen();

        return 0;
    } else {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_FAILED_TO_REOPEN_DEVICE);

        erase_and_delete_window(window);
        redraw_screen();

        return 1;
    }
}

/**
 * Seeks to the given position relative to the start of the device.  Gracefully
 * handles device errors and disconnects by retrying the operation or, if the
 * device has been disconnected, waiting for the device to reconnect.
 *
 * @param fd                       A pointer to a variable holding the current
 *                                 device handle.  If the device
 *                                 disconnects/reconnects during the course of
 *                                 this function, the contents of the variable
 *                                 will be overwritten with a new handle to the
 *                                 device.  If the device disconnects and cannot
 *                                 be re-opened, this variable is set to -1.
 * @param position                 The position to which to seek, relative to
 *                                 the start of the device.
 * @param device_was_disconnected  A pointer to a variable which will be set to
 *                                 1 if the device was disconnected during the
 *                                 course of this function, or left unmodified
 *                                 otherwise.  This parameter may be set to
 *                                 NULL.
 *
 * @returns The current position of the file pointer, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
off_t lseek_or_retry(int *fd, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    if((ret = lseek(*fd, position, SEEK_SET)) == -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_TO_SECTOR_ERROR, position / device_stats.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_stats.device_num)) {
            if(device_was_disconnected) {
                *device_was_disconnected = 1;
            }

            if(handle_device_disconnect(fd, position, 0) == -1) {
                return -1;
            }
        } else {
            ret = lseek(*fd, position, SEEK_SET);
            retry_count++;
        }
    }

    return ret;
}

/**
 * Seeks to the given position relative to the start of the device.  Gracefully
 * handles device errors and disconnects by retrying the operation, attempting
 * to reset the device, and/or waiting for the device to reconnect.
 *
 * @param fd                       A pointer to a variable holding the current
 *                                 device handle.  If the device
 *                                 disconnects/reconnects during the course of
 *                                 this function, the contents of the variable
 *                                 will be overwritten with a new handle to the
 *                                 device.  If the device disconnects and cannot
 *                                 be re-opened, this variable is set to -1.
 * @param position                 The position to which to seek, relative to
 *                                 the start of the device.
 * @param device_was_disconnected  A pointer to a variable which will be set to
 *                                 1 if the device was disconnected during the
 *                                 course of this function, or left unmodified
 *                                 otherwise.  This parameter may be set to
 *                                 NULL.
 *
 * @returns The current position of the file pointer, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t lseek_or_reset_device(int *fd, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = lseek_or_retry(fd, position, device_was_disconnected);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
            if(device_was_disconnected) {
                *device_was_disconnected = 1;
            }

            if(handle_device_disconnect(fd, position, 0) == -1) {
                return -1;
            }
        } else {
            if(can_reset_device(device_stats.device_num)) {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);
                window = resetting_device_message();

                main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                *fd = reset_device(*fd);
                main_thread_status = previous_status;

                if(*fd == -1) {
                    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);

                    erase_and_delete_window(window);
                    redraw_screen();

                    return -1;
                }

                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                retry_count++;

                ret = lseek_or_retry(fd, position, device_was_disconnected);

                *device_was_disconnected = 1;

                erase_and_delete_window(window);
                redraw_screen();
            } else {
                // Insta-fail
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
                return -1;
            }
        }
    }

    return ret;
}

/**
 * Reads from the given device.  Gracefully handles device errors and
 * disconnects by retrying the operation or, if the device has been
 * disconnected, waiting for the device to reconnect.
 *
 * @param fd        A pointer to a variable holding the current device handle.
 *                  If the device disconnects/reconnects during the course of
 *                  this function, the contents of the variable will be
 *                  overwritten with a new handle to the device.  If the device
 *                  disconnects and cannot be re-opened, this variable is set to
 *                  -1.
 * @param buf       A pointer to a buffer which will receive the data read from
 *                  the device.
 * @param count     The number of bytes to read from the device.
 * @param position  The current position of the file pointer in the device.
 *                  This is used in case the device needs to be reset and must
 *                  be reopened.
 *
 * @returns The number of bytes read from the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t read_or_retry(int *fd, void *buf, uint64_t count, off_t position) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = read(*fd, buf, count);
    if(ret == -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_READ_ERROR_IN_SECTOR, position / device_stats.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_stats.device_num)) {
            if(handle_device_disconnect(fd, position, 1) == -1) {
                return -1;
            }
        } else {
            ret = read(*fd, buf, count);
            retry_count++;
        }
    }

    return ret;
}

/**
 * Reads from the given device.  Gracefully handles device errors and
 * disconnects by retrying the operation, attempting to reset the device, and/or
 * waiting for the device to reconnect.
 *
 * @param fd        A pointer to a variable holding the current device handle.
 *                  If the device disconnects/reconnects during the course of
 *                  this function, the contents of the variable will be
 *                  overwritten with a new handle to the device.  If the device
 *                  disconnects and cannot be re-opened, this variable is set to
 *                  -1.
 * @param buf       A pointer to a buffer which will receive the data read from
 *                  the device.
 * @param count     The number of bytes to read from the device.
 * @param position  The current position of the file pointer in the device.
 *                  This is used in case the device needs to be reset and must
 *                  be reopened.
 *
 * @returns The number of bytes read from the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t read_or_reset_device(int *fd, void *buf, uint64_t count, off_t position) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = read_or_retry(fd, buf, count, position);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
            if(handle_device_disconnect(fd, position, 1) == -1) {
                return -1;
            }
        } else {
            if(can_reset_device(device_stats.device_num)) {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);
                window = resetting_device_message();

                main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                *fd = reset_device(*fd);
                main_thread_status = previous_status;

                if(*fd == -1) {
                    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);
                    retry_count = MAX_RESET_RETRIES;
                } else {
                    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                    retry_count++;

                    if(lseek_or_retry(fd, position, NULL) == -1) {
                        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);
                        erase_and_delete_window(window);
                        redraw_screen();
                        return -1;
                    }

                    ret = read_or_retry(fd, buf, count, position);
                }

                erase_and_delete_window(window);
                redraw_screen();
            } else {
                // Insta-fail
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
                return -1;
            }
        }
    }

    return ret;
}

int64_t write_or_retry(int *fd, void *buf, uint64_t count, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = write(*fd, buf, count);
    if(ret == -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR_IN_SECTOR, position / device_stats.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(did_device_disconnect(device_stats.device_num)) {
            *device_was_disconnected = 1;
            if(num_rounds > 0) {
                if(handle_device_disconnect(fd, position, 1) == -1) {
                    return -1;
                }
            } else {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_DEVICE_DISCONNECTED_DURING_ROUND_1);
                return -1;
            }
        } else {
            ret = write(*fd, buf, count);
            retry_count++;
        }
    }

    return ret;
}

int64_t write_or_reset_device(int *fd, void *buf, uint64_t count, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = write_or_retry(fd, buf, count, position, device_was_disconnected);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
            *device_was_disconnected = 1;
            if(num_rounds > 0) {
                if(handle_device_disconnect(fd, position, 1) == -1) {
                    return -1;
                }
            } else {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_DEVICE_DISCONNECTED_DURING_ROUND_1);
                return -1;
            }
        } else {
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);

            if(num_rounds > 0) {
                if(can_reset_device(device_stats.device_num)) {
                    window = resetting_device_message();

                    main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                    *fd = reset_device(*fd);
                    main_thread_status = previous_status;

                    if(*fd == -1) {
                        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);
                        retry_count = MAX_RESET_RETRIES;
                    } else {
                        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                        retry_count++;

                        if(lseek_or_retry(fd, position, device_was_disconnected) == -1) {
                            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);
                            erase_and_delete_window(window);
                            redraw_screen();
                            return -1;
                        }

                        ret = write_or_retry(fd, buf, count, position, device_was_disconnected);
                    }

                    *device_was_disconnected = 1;

                    erase_and_delete_window(window);
                    redraw_screen();
                } else {
                    // Insta-fail
                    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
                    return -1;
                }
            } else {
                // Insta-fail
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_REFUSING_TO_RESET_DURING_ROUND_1);
                return -1;
            }
        }
    }

    return ret;
}

void malloc_error(int errnum) {
    snprintf(msg_buffer, sizeof(msg_buffer),
             "Failed to allocate memory for one of the buffers we need to do "
             "the stress test.  Unfortunately this means that we have to abort "
             "the stress test.\n\nThe error we got was: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void posix_memalign_error(int errnum) {
    snprintf(msg_buffer, sizeof(msg_buffer),
             "Failed to allocate memory for one of the buffers we need to do "
             "the stress test.  Unfortunately this means we have to abort the "
             "stress test.\n\nThe error we got was: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void reset_sector_map() {
    for(uint64_t j = 0; j < device_stats.num_sectors; j++) {
        sector_display.sector_map[j] &= SECTOR_MAP_FLAG_DO_NOT_USE | SECTOR_MAP_FLAG_FAILED;
    }
}

void reset_sector_map_partial(uint64_t start, uint64_t end) {
    for(uint64_t j = start; j < end; j++) {
        sector_display.sector_map[j] &= SECTOR_MAP_FLAG_DO_NOT_USE | SECTOR_MAP_FLAG_FAILED;
    }
}

uint64_t get_sector_number_xor_val(char *data) {
    unsigned char *udata = (unsigned char *) data;
    return
        (((uint64_t)udata[32]) << 56) |
        (((uint64_t)udata[48]) << 48) |
        (((uint64_t)udata[64]) << 40) |
        (((uint64_t)udata[80]) << 32) |
        (((uint64_t)udata[96]) << 24) |
        (((uint64_t)udata[112]) << 16) |
        (((uint64_t)udata[128]) << 8) |
        ((uint64_t)udata[144]);
}

int64_t get_round_num_xor_val(char *data) {
    unsigned char *udata = (unsigned char *) data;
    return
        (((uint64_t)udata[33]) << 56) |
        (((uint64_t)udata[49]) << 48) |
        (((uint64_t)udata[65]) << 40) |
        (((uint64_t)udata[81]) << 32) |
        (((uint64_t)udata[97]) << 24) |
        (((uint64_t)udata[113]) << 16) |
        (((uint64_t)udata[129]) << 8) |
        ((uint64_t)udata[145]);
}

void embed_device_uuid(char *data) {
    int i;
    for(i = 0; i < 16; i++) {
        data[i + 16] = device_stats.device_uuid[i] ^ data[(i * 16) + 34];
    }
}

void get_embedded_device_uuid(char *data, char *uuid_buffer) {
    int i;
    for(i = 0; i < 16; i++) {
        uuid_buffer[i] = data[i + 16] ^ data[(i * 16) + 34];
    }
}

void embed_sector_number(char *data, uint64_t sector_number) {
    *((uint64_t *) data) = sector_number ^ get_sector_number_xor_val(data);
}

void embed_round_number(char *data, int64_t round_num) {
    *((int64_t *) (data + 8)) = round_num ^ get_round_num_xor_val(data);
}

uint64_t decode_embedded_sector_number(char *data) {
    return (*((uint64_t *) data)) ^ get_sector_number_xor_val(data);
}

int64_t decode_embedded_round_number(char *data) {
    return (*((int64_t *) (data + 8))) ^ get_round_num_xor_val(data);
}

void embed_crc32c(char *data, int sector_size) {
    *((uint32_t *) &data[sector_size - sizeof(uint32_t)]) = calculate_crc32c(0, data, sector_size - sizeof(uint32_t));
}

uint32_t get_embedded_crc32c(char *data, int sector_size) {
    return *((uint32_t *) &data[sector_size - sizeof(uint32_t)]);
}

void log_sector_contents(uint64_t sector_num, int sector_size, char *expected_data, char *actual_data) {
    char tmp[16];
    int i;

    log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_EXPECTED_DATA_WAS);

    for(i = 0; i < sector_size; i += 16) {
        // If, for some reason, the sector size isn't a multiple of 16, then
        // make sure we don't overrun the expected_data buffer
        memset(tmp, 0, 16);
        memcpy(tmp, expected_data + i, (sector_size - i) >= 16 ? 16 : (sector_size - i));
        log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_MISMATCHED_DATA_LINE, (sector_num * sector_size) + i, tmp[0] & 0xff, tmp[1] & 0xff, tmp[2] & 0xff, tmp[3] & 0xff, tmp[4] & 0xff, tmp[5] & 0xff, tmp[6] & 0xff, tmp[7] & 0xff, tmp[8] & 0xff, tmp[9] & 0xff, tmp[10] & 0xff, tmp[11] & 0xff, tmp[12] & 0xff, tmp[13] & 0xff, tmp[14] & 0xff, tmp[15] & 0xff);
    }

    log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_ACTUAL_DATA_WAS);

    for(i = 0; i < sector_size; i += 16) {
        memset(tmp, 0, 16);
        memcpy(tmp, actual_data + i, (sector_size - i) >= 16 ? 16 : (sector_size - i));
        log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_MISMATCHED_DATA_LINE, (sector_num * sector_size) + i, tmp[0] & 0xff, tmp[1] & 0xff, tmp[2] & 0xff, tmp[3] & 0xff, tmp[4] & 0xff, tmp[5] & 0xff, tmp[6] & 0xff, tmp[7] & 0xff, tmp[8] & 0xff, tmp[9] & 0xff, tmp[10] & 0xff, tmp[11] & 0xff, tmp[12] & 0xff, tmp[13] & 0xff, tmp[14] & 0xff, tmp[15] & 0xff);
    }

    log_log(NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_BLANK_LINE);
}

void state_file_error() {
    WINDOW *window;
    int i;

    const char *warning_text =
        "There was a problem loading the state file.  If you want to continue "
        "and just ignore the existing state file, then you can ignore this "
        "message.  Otherwise, you have %d seconds to hit Ctrl+C.";

    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_STATE_FILE_LOAD_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer), warning_text, 15);

    window = message_window(stdscr, WARNING_TITLE, msg_buffer, 0);

    if(window) {
        for(i = 0; i < 150; i++) {
            handle_key_inputs(window);
            usleep(100000);
            if(i && !(i % 10)) {
                snprintf(msg_buffer, sizeof(msg_buffer), warning_text, 15 - (i / 10));
                delwin(window);
                window = message_window(stdscr, WARNING_TITLE, msg_buffer, 0);
                wrefresh(window);
            }
        }
    } else {
        sleep(15);
    }

    erase_and_delete_window(window);
}

void show_initial_warning_message() {
    WINDOW *window;
    int i;

    const char *warning_text =
        "This program is DESTRUCTIVE.  It is designed to stress test storage "
        "devices (particularly flash media) to the point of failure.  If you "
        "let this program run for long enough, it WILL completely destroy the "
        "device and render it completely unusable.  Do not use it on any "
        "storage devices that you care about.\n\nAny data on %s is going to be "
        "overwritten -- multiple times.  If you're not OK with this, you have "
        "%d seconds to hit Ctrl+C before we start doing anything.";

    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_1);
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_2);
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_BLANK_LINE);
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_3, program_options.device_name);

    snprintf(msg_buffer, sizeof(msg_buffer), warning_text, program_options.device_name, 15);
    window = message_window(stdscr, WARNING_TITLE, msg_buffer, 0);

    if(window) {
        for(i = 0; i < 150; i++) {
            handle_key_inputs(window);
            usleep(100000);
            if(i && !(i % 10)) {
                delwin(window);
                snprintf(msg_buffer, sizeof(msg_buffer), warning_text, program_options.device_name, 15 - (i / 10));
                window = message_window(stdscr, WARNING_TITLE, msg_buffer, 0);
                wrefresh(window);
            }
        }
    } else {
        sleep(15);
    }

    erase_and_delete_window(window);
}

void log_file_open_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_LOG_FILE_OPEN_ERROR, program_options.log_file, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open log file %s: %s", program_options.log_file, strerror(errnum));
    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void lockfile_open_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_LOCK_FILE_OPEN_ERROR, program_options.lock_file, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open lock file %s: %s", program_options.lock_file, strerror(errnum));
    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void stats_file_open_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_STATS_FILE_OPEN_ERROR, program_options.stats_file, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open stats file %s: %s", program_options.stats_file, strerror(errnum));
    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void no_working_gettimeofday(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_GETTIMEOFDAY_FAILED, strerror(errnum));
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_NO_WORKING_GETTIMEOFDAY);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because your system doesn't "
             "have a working gettimeofday() call.  So many things in this "
             "program depend on this that it would take a lot of work to make "
             "this program work without it, and I'm lazy.\n\nThe error we got "
             "was: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

uint64_t get_max_writable_sectors(uint64_t starting_sector, uint64_t max_sectors) {
    uint64_t out = 0;

    for(out = 0; out < max_sectors && !(sector_display.sector_map[starting_sector + out] & SECTOR_MAP_FLAG_DO_NOT_USE); out++);
    return out;
}

uint64_t get_max_unwritable_sectors(uint64_t starting_sector, uint64_t max_sectors) {
    uint64_t out = 0;

    for(out = 0; out < max_sectors && (sector_display.sector_map[starting_sector + out] & SECTOR_MAP_FLAG_DO_NOT_USE); out++);
    return out;
}

void mark_sector_unwritable(uint64_t sector_num) {
    sector_display.sector_map[sector_num] |= SECTOR_MAP_FLAG_DO_NOT_USE;
}

int endurance_test_read_block(int *fd, uint64_t starting_sector, int num_sectors, char *buffer, int *device_was_disconnected) {
    int ret, bytes_left_to_read, block_size;
    uint64_t num_sectors_to_read;
    handle_key_inputs(NULL);
    wait_for_file_lock(NULL);

    bytes_left_to_read = block_size = device_stats.sector_size * num_sectors;

    while(bytes_left_to_read) {
        // Alternate between reading from the device and filling the buffer with all zeroes
        num_sectors_to_read = get_max_writable_sectors(starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size), bytes_left_to_read / device_stats.sector_size);
        if(num_sectors_to_read) {
            if((ret = read_or_reset_device(fd, buffer + (block_size - bytes_left_to_read), num_sectors_to_read * device_stats.sector_size, lseek(*fd, 0, SEEK_CUR))) == -1) {
                if(*fd == -1) {
                    return -1;
                } else {
                    // Mark this sector as bad and skip over it
                    if(!is_sector_bad(starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size))) {
                        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_READ_ERROR_MARKING_SECTOR_UNUSABLE, starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size));
                    }

                    mark_sector_unwritable(starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size));
                    mark_sector_bad(starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size));
                    bytes_left_to_read -= device_stats.sector_size;

                    if((lseek_or_retry(fd, (starting_sector * device_stats.sector_size) + (block_size - bytes_left_to_read), device_was_disconnected)) == -1) {
                        // Give up if we can't seek
                        return -1;
                    }

                    continue;
                }
            }

            bytes_left_to_read -= ret;
            device_stats.bytes_since_last_status_update += ret;
        }

        if(bytes_left_to_read) {
            num_sectors_to_read = get_max_unwritable_sectors(starting_sector + ((block_size - bytes_left_to_read) / device_stats.sector_size), bytes_left_to_read / device_stats.sector_size);
            if(num_sectors_to_read) {
                memset(buffer + (block_size - bytes_left_to_read), 0, num_sectors_to_read * device_stats.sector_size);
                bytes_left_to_read -= num_sectors_to_read * device_stats.sector_size;

                // Seek past the unwritable sectors
                if(lseek_or_retry(fd, (starting_sector * device_stats.sector_size) + (block_size - bytes_left_to_read), device_was_disconnected) == -1) {
                    return -1;
                }
            }
        }

        print_status_update();
    }

    return 0;
}

void device_locate_error() {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_DEVICE_LOCATE_ERROR);
    message_window(stdscr, ERROR_TITLE,
                   "An error occurred while trying to locate the device "
                   "described in the state file. (Make sure you're running "
                   "this program as root.)", 1);
}

void multiple_matching_devices_error() {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_DEVICE_AMBIGUITY_ERROR);
    message_window(stdscr, ERROR_TITLE,
                   "There are multiple devices that match the data in the "
                   "state file.  Please specify which device you want to "
                   "test on the command line.", 1);
}

void wrong_device_specified_error() {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_WRONG_DEVICE_ERROR);
    message_window(stdscr, ERROR_TITLE,
                   "The device you specified on the command line does not "
                   "match the device described in the state file.  If you run "
                   "this program again without the device name, we'll figure "
                   "out which device to use automatically.  Otherwise, provide "
                   "a different device on the command line.", 1);
}

WINDOW *no_matching_device_warning() {
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_DEVICE_NOT_ATTACHED);
    return message_window(stdscr, "No devices found",
                          "No devices could be found that match the data in "
                          "the state file.  If you haven't plugged the device "
                          "in yet, go ahead and do so now.  Otherwise, you can "
                          "hit Ctrl+C now to abort the program.", 0);
}

void wait_for_device_connect_error(WINDOW *window) {
    if(window) {
        erase_and_delete_window(window);
    }

    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_WAIT_FOR_DEVICE_RECONNECT_ERROR);
    message_window(stdscr, ERROR_TITLE, "An error occurred while waiting for you to reconnect the device.", 1);
}

void fstat_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test %s because we weren't able to pull stats"
             "on it.  The device may have been removed, or you may not have "
             "permissions to open it.  (Make sure you're running this program "
             "as root.)\n\nThe error we got was: %s", program_options.device_name, strerror(errnum));
    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void stat_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we were unable to "
             "pull stats on it.  The device may have been removed, or you may "
             "not have permissions to open it.  (Make sure you're running this "
             "program as root.)\n\nThe error we got was: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void not_a_block_device_error() {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_NOT_A_BLOCK_DEVICE, program_options.device_name);
    message_window(stdscr, ERROR_TITLE, "We won't be able to test this device because it isn't a block device.  You must provide a block device to test with.", 1);
}

void device_open_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OPEN_DEVICE);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we couldn't open "
             "it.  The device might have gone away, or you might not have "
             "permission to open it.  (Make sure you run this program as "
             "root.)\n\nHere's the error was got: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void ioctl_error(int errnum) {
    log_log(NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we couldn't pull "
             "stats on it.\n\nHere's the error we got: %s", strerror(errnum));

    message_window(stdscr, ERROR_TITLE, msg_buffer, 1);
}

void save_state_error() {
    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_SAVE_STATE_ERROR);
    message_window(stdscr, WARNING_TITLE, "An error occurred while trying to save the program state.  Save stating has been disabled.", 1);

    free(program_options.state_file);
    program_options.state_file = NULL;
}

int was_bod_area_affected(uint64_t starting_byte) {
    return starting_byte < BOD_MOD_BUFFER_SIZE;
}

int was_mod_area_affected(uint64_t starting_byte, uint64_t ending_byte) {
    return (starting_byte >= device_stats.middle_of_device && starting_byte < (device_stats.middle_of_device + BOD_MOD_BUFFER_SIZE)) || (ending_byte >= device_stats.middle_of_device && ending_byte < (device_stats.middle_of_device + BOD_MOD_BUFFER_SIZE));
}

/**
 * Update the beginning-of-device buffer if a write operation at the given
 * starting_byte would have affected the data in the BOD area.
 *
 * @param starting_byte  The starting location at which the write to the device
 *                       occurred (relative to the start of the device).
 * @param buffer         A buffer containing the data that was written to the
 *                       device.
 * @param num_bytes      The number of bytes that were written to the device.
 */
void update_bod_buffer(uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    uint64_t bytes_to_copy;

    if(was_bod_area_affected(starting_byte)) {
        bytes_to_copy = num_bytes;

        if(num_bytes + starting_byte > BOD_MOD_BUFFER_SIZE) {
            bytes_to_copy = BOD_MOD_BUFFER_SIZE - starting_byte;
        }

        memcpy(bod_buffer + starting_byte, buffer, bytes_to_copy);

        if(save_state()) {
            save_state_error();
        }
    }
}

/**
 * Update the middle-of-device buffer if a write operation at the given
 * starting_byte would have affected the data in the MOD area.
 *
 * @param starting_byte  The starting location at which the write to the device
 *                       occurred (relative to the start of the device).
 * @param buffer         A buffer containing the data that was written to the
 *                       device.
 * @param num_bytes      The number of bytes that were written to the device.
 */
void update_mod_buffer(uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    uint64_t bytes_to_copy;
    uint64_t buffer_offset = 0;
    char *mod_position = mod_buffer;

    if(was_mod_area_affected(starting_byte, starting_byte + num_bytes - 1)) {
        bytes_to_copy = num_bytes;

        if(starting_byte < device_stats.middle_of_device) {
            buffer_offset = device_stats.middle_of_device - starting_byte;
            bytes_to_copy -= buffer_offset;

            if(bytes_to_copy > BOD_MOD_BUFFER_SIZE) {
                bytes_to_copy = BOD_MOD_BUFFER_SIZE;
            }
        } else {
            mod_position += starting_byte - device_stats.middle_of_device;
            if((starting_byte - device_stats.middle_of_device) + num_bytes > BOD_MOD_BUFFER_SIZE) {
                bytes_to_copy = BOD_MOD_BUFFER_SIZE - (starting_byte - device_stats.middle_of_device);
            }
        }

        memcpy(mod_position, buffer + buffer_offset, bytes_to_copy);

        if(save_state()) {
            save_state_error();
        }
    }
}

/**
 * Update the beginning-of-device or middle-of-device buffer if a write
 * operation at the given starting_byte would have affected the data in the BOD
 * or MOD areas.
 *
 * @param starting_byte  The starting location at which the write to the device
 *                       occurred (relative to the start of the device).
 * @param buffer         A buffer containing the data that was written.
 * @param num_bytes      The number of bytes that were written to the device.
 */
void update_bod_mod_buffers(uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    update_bod_buffer(starting_byte, buffer, num_bytes);
    update_mod_buffer(starting_byte, buffer, num_bytes);
}

/**
 * Writes a block of data to the device for the endurance test, skipping over
 * any sectors that are flagged as "unwritable".
 *
 * @param fd                          A pointer to a variable containing the
 *                                    file handle for the current device.  This
 *                                    variable may be updated during the course
 *                                    of the operation -- for example, if a
 *                                    device disconnect occurs.
 * @param starting_sector             The starting sector at which to start
 *                                    writing the data to the device.
 * @param num_sectors                 The number of sectors to write to the
 *                                    device.
 * @param buffer                      A pointer to a buffer containing the data
 *                                    to be written to the device.
 * @param device_was_disconnected     A pointer to a variable indicating whether
 *                                    a device disconnect was encountered during
 *                                    the operation.  If a device disconnect is
 *                                    encountered, this variable will be set to
 *                                    1; if the operation completed without a
 *                                    disconnect, this variable will be set to
 *                                    0.
 *
 * @returns 0 if the operation completed successfully or if the device
 * disconnected during the operation.  If an error occurred, one of the
 * ABORT_REASON_* codes is returned instead.  Note that if an error occurs that
 * caused the device to disconnect and the device could not be reconnected, *fd
 * is set to -1.
 */
int endurance_test_write_block(int *fd, uint64_t starting_sector, int num_sectors, char *buffer, int *device_was_disconnected) {
    // The logic around figuring out where we needed to write to various buffers
    // was getting pretty gnarly, so I decided to just err on the side of using
    // more variables to keep track of where everything is.  I'm going off the
    // assumption that on an optimized build, the compiler will optimize some of
    // these out and the memory/performance effect will be negligible.
    uint64_t num_sectors_to_write, num_bytes_to_write;
    uint64_t num_bytes_written, num_sectors_written;
    uint64_t num_bytes_remaining, num_sectors_remaining;
    uint64_t current_sector, current_byte;
    uint64_t num_sectors_affected_this_round, num_bytes_affected_this_round;
    uint64_t num_bytes_to_update_in_bod_mod_buffer;
    uint64_t starting_byte;
    int ret;

    num_bytes_remaining = num_sectors * device_stats.sector_size;
    *device_was_disconnected = 0;
    ret = 0;

    starting_byte = starting_sector * device_stats.sector_size;

    while(num_bytes_remaining && !*device_was_disconnected) {
        handle_key_inputs(NULL);
        wait_for_file_lock(NULL);

        num_sectors_remaining = num_bytes_remaining / device_stats.sector_size;
        num_sectors_written = num_sectors - num_sectors_remaining;
        num_bytes_written = num_sectors_written * device_stats.sector_size;
        current_sector = starting_sector + num_sectors_written;
        current_byte = current_sector * device_stats.sector_size;

        if(num_sectors_to_write = get_max_writable_sectors(current_sector, num_sectors_remaining)) {
            num_bytes_to_write = num_sectors_to_write * device_stats.sector_size;
            if((ret = write_or_reset_device(fd, buffer + num_bytes_written, num_bytes_to_write, current_byte, device_was_disconnected)) == -1) {
                if(*fd == -1) {
                    // The device has disconnected, and attempts to wait
                    // for it to reconnect have failed
                    return ABORT_REASON_WRITE_ERROR;
                } else {
                    // Mark this sector bad and skip over it
                    if(!is_sector_bad(current_sector)) {
                        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR_SECTOR_UNUSABLE, current_sector);
                        num_bad_sectors++;
                    }

                    mark_sector_unwritable(current_sector);
                    mark_sector_bad(current_sector);
                    num_bad_sectors_this_round++;

                    num_bytes_remaining -= device_stats.sector_size;

                    if((lseek_or_retry(fd, current_byte + device_stats.sector_size, device_was_disconnected)) == -1) {
                        // Give up if we can't seek
                        return ABORT_REASON_SEEK_ERROR;
                    }

                    continue;
                }
            } else {
                num_bytes_written += ret;
                num_bytes_remaining -= ret;
                num_sectors_written = num_bytes_written / device_stats.sector_size;
                num_sectors_remaining = num_sectors - num_sectors_written;
                num_sectors_affected_this_round = num_sectors_written;
                num_bytes_affected_this_round = num_sectors_affected_this_round * device_stats.sector_size;
                current_sector = starting_sector + num_sectors_written;
            }
        }

        // If the device disconnected during the write operation, we want to
        // give up right away so that we can restart the slice.
        if(*device_was_disconnected) {
            break;
        }

        if(num_sectors_to_write = get_max_unwritable_sectors(current_sector, num_sectors_remaining)) {
            // Seek past the bad sectors
            num_sectors_remaining -= num_sectors_to_write;
            num_bytes_remaining -= num_sectors_to_write * device_stats.sector_size;
            num_sectors_written += num_sectors_to_write;
            num_bytes_written += num_sectors_to_write * device_stats.sector_size;
            num_sectors_affected_this_round += num_sectors_to_write;
            num_bytes_affected_this_round = num_sectors_affected_this_round * device_stats.sector_size;

            if(lseek_or_retry(fd, starting_byte + num_bytes_written, device_was_disconnected) == -1) {
                return ABORT_REASON_SEEK_ERROR;
            }
        }

        // Update the BOD and MOD buffers if necessary
        update_bod_mod_buffers(current_byte, buffer + (current_byte - starting_byte), num_bytes_affected_this_round);
        device_stats.bytes_since_last_status_update += ret;
        state_data.bytes_written += ret;

        print_status_update();
    }

    return 0;
}

/**
 * Embed sector number, round number, UUID, and CRC32 data into the the data in
 * the given buffer.  The buffer is modified in place.
 *
 * @param buffer           A buffer containing the data to be written to the
 *                         sector (or verified against the sector contents).
 * @param num_sectors      The number of sectors worth of data available in the
 *                         buffer.
 * @param starting_sector  The sector number of the first sector represented by
 *                         the buffer.
 * @param round_num        The current round number of endurance testing.
 */
void prepare_endurance_test_block(char *buffer, int num_sectors, uint64_t starting_sector, uint64_t round_num) {
    int i;
    // We'll embed some information into the data to try to detect
    // various types of errors:
    //  - Sector number (to detect address decoding errors),
    //  - Round number (to detect failed writes),
    //  - Device UUID (to detect cross-device reads)
    //  - CRC32 (to detect bit flip errors)
    for(i = 0; i < num_sectors; i++) {
        embed_sector_number(buffer + (i * device_stats.sector_size), starting_sector + i);
        embed_round_number(buffer + (i * device_stats.sector_size), round_num);
        embed_device_uuid(buffer + (i * device_stats.sector_size));
        embed_crc32c(buffer + (i * device_stats.sector_size), device_stats.sector_size);
    }
}

/**
 * Writes random data to a slice of the device.  Sector number, round number,
 * and device UUID are embedded in the random data.
 *
 * @param fd           A pointer to a handle to the device to be written to.
 * @param round_num    The current round number (starting from 0).
 * @param rng_seed     The seed that should be used to initialize the RNG when
 *                     generating data to write to the device.
 * @param slice_num    The slice number of the current slice.
 * @param num_sectors  The number of sectors per slice.  If the number of
 *                     sectors would cause the write to go past the end of the
 *                     device, the write is automatically truncated to fit the
 *                     remaining space on the deivce.
 * @param bod_buffer   A pointer to the start of the current beginning-of-device
 *                     buffer.
 * @param mod_buffer   A pointer to the start of the current middle-of-device
 *                     buffer.
 *
 * @returns 0 if the write completed successfully, -1 if an error occurred (not
 *          related to the device), or one of the ABORT_REASON_* codes if an
 *          error related to the device occurs.
 */
int endurance_test_write_slice(int *fd, uint64_t round_num, unsigned int rng_seed, uint64_t slice_num, uint64_t num_sectors, char *bod_buffer, char *mod_buffer) {
    uint64_t cur_sector, last_sector, cur_block_size, sectors_in_cur_block, bytes_left_to_write, i, num_sectors_to_write, sectors_per_block;
    int device_was_disconnected, ret;
    sql_thread_status_type prev_sql_thread_status = sql_thread_status;
    char *write_buffer;

    if(ret = posix_memalign((void **) &write_buffer, sysconf(_SC_PAGESIZE), device_stats.block_size)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        posix_memalign_error(ret);
        return -1;
    }

    sectors_per_block = device_stats.block_size / device_stats.sector_size;

    // Set last_sector to one sector past the last sector we should touch this slice
    if(slice_num == (NUM_SLICES - 1)) {
        last_sector = device_stats.num_sectors;
    } else {
        last_sector = get_slice_start(slice_num + 1);
    }

    do {
        device_was_disconnected = 0;
        rng_reseed(rng_seed);

        if(lseek_or_retry(fd, get_slice_start(slice_num) * device_stats.sector_size, &device_was_disconnected) == -1) {
            free(write_buffer);
            return ABORT_REASON_SEEK_ERROR;
        }

        for(cur_sector = get_slice_start(slice_num); cur_sector < last_sector && !device_was_disconnected; cur_sector += sectors_in_cur_block) {
            if(sql_thread_status != prev_sql_thread_status) {
                prev_sql_thread_status = sql_thread_status;
                print_sql_status(sql_thread_status);
            }

            if((cur_sector + sectors_per_block) > last_sector) {
                sectors_in_cur_block = last_sector - cur_sector;
                cur_block_size = sectors_in_cur_block * device_stats.sector_size;
            } else {
                sectors_in_cur_block = sectors_per_block;
                cur_block_size = sectors_in_cur_block * device_stats.sector_size;
            }

            rng_fill_buffer(write_buffer, cur_block_size);
            bytes_left_to_write = cur_block_size;

            prepare_endurance_test_block(write_buffer, sectors_in_cur_block, cur_sector, round_num);

            handle_key_inputs(NULL);
            wait_for_file_lock(NULL);

            ret = endurance_test_write_block(fd, cur_sector, sectors_in_cur_block, write_buffer, &device_was_disconnected);
            if(ret == -1) {
                free(write_buffer);
                return ABORT_REASON_WRITE_ERROR;
            }

            if(device_was_disconnected) {
                // Unmark the sectors we've written in this slice so far
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_RESTARTING_SLICE);
                reset_sector_map_partial(get_slice_start(slice_num), last_sector);
                redraw_sector_map();
            } else {
                mark_sectors_written(cur_sector, cur_sector + sectors_in_cur_block);

                assert(!gettimeofday(&stats_cur_time, NULL));
                if(timediff(stress_test_stats.previous_update_time, stats_cur_time) >= (program_options.stats_interval * 1000000)) {
                    stats_log(num_rounds, device_stats.num_bad_sectors);
                }
            }

            refresh();
        }
    } while(device_was_disconnected);

    free(write_buffer);
    return 0;
}

int main(int argc, char **argv) {
    int fd, cur_block_size, local_errno, restart_slice, state_file_status;
    struct stat fs;
    uint64_t bytes_left_to_write, ret, cur_sector, num_sectors_to_write;
    unsigned int sectors_per_block;
    unsigned short max_sectors_per_request;
    char *buf, *compare_buf, *zero_buf, *ff_buf, *new_device_name;
    struct timeval speed_start_time;
    struct timeval rng_init_time;
    uint64_t cur_sectors_per_block, last_sector;
    uint64_t num_good_sectors_this_round;
    uint64_t cur_slice, i, j;
    int *read_order;
    int op_retry_count; // How many times have we tried the same operation without success?
    int reset_retry_count; // How many times have we tried to reset the device?
    int device_was_disconnected;
    int iret;
    WINDOW *window;
    dev_t new_device_num;
    pthread_t sql_thread;
    sql_thread_params_type sql_thread_params;
    sql_thread_status_type prev_sql_thread_status = 0;

    // Set things up so that cleanup() works properly
    sector_display.sector_map = NULL;
    fd = -1;
    file_handles.log_file = NULL;
    file_handles.stats_file = NULL;
    ncurses_active = 0;
    buf = NULL;
    compare_buf = NULL;
    zero_buf = NULL;
    ff_buf = NULL;
    read_order = NULL;
    program_options.lock_file = NULL;
    program_options.state_file = NULL;
    forced_device = NULL;
    is_writing = -1;

    void cleanup() {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_PROGRAM_ENDING);
        if(fd != -1) {
            close(fd);
        }

        close_lockfile();

        if(file_handles.log_file) {
            fclose(file_handles.log_file);
        }

        if(file_handles.stats_file) {
            fclose(file_handles.stats_file);
        }

        if(ncurses_active) {
            erase();
            refresh();
            endwin();
        }

        if(buf) {
            free(buf);
        }

        if(compare_buf) {
            free(compare_buf);
        }

        if(zero_buf) {
            free(zero_buf);
        }

        if(ff_buf) {
            free(ff_buf);
        }

        if(sector_display.sector_map) {
            free(sector_display.sector_map);
        }

        if(read_order) {
            free(read_order);
        }

        if(program_options.log_file) {
            free(program_options.log_file);
        }

        if(program_options.stats_file) {
            free(program_options.stats_file);
        }

        if(program_options.lock_file) {
            free(program_options.lock_file);
        }

        if(program_options.state_file) {
            free(program_options.state_file);
        }

        if(forced_device) {
            free(forced_device);
        }
    }

    speed_qualifications_shown = 0;
    device_speeds.random_read_iops = 0;
    device_speeds.random_write_iops = 0;
    device_speeds.sequential_read_speed = 0;
    device_speeds.sequential_write_speed = 0;
    sector_display.sectors_per_block = 0;
    state_data.first_failure_round = -1;
    state_data.ten_percent_failure_round = -1;
    state_data.twenty_five_percent_failure_round = -1;
    device_stats.is_fake_flash = FAKE_FLASH_UNKNOWN;
    num_rounds = -1;
    main_thread_status = MAIN_THREAD_STATUS_IDLE;

    if(parse_command_line_arguments(argc, argv)) {
        return -1;
    }

    // Zero out the stress test stats and the device stats
    memset(&stress_test_stats, 0, sizeof(stress_test_stats));
    memset(&device_stats, 0, sizeof(device_stats));

    state_file_status = load_state();

    // Recompute num_sectors now so that we don't crash when we call redraw_screen
    if(state_file_status == LOAD_STATE_SUCCESS) {
        device_stats.num_sectors = device_stats.detected_size_bytes / device_stats.sector_size;
    }

    // If the user didn't specify a curses option on the command line, then use
    // what's in the state file.
    if(!program_options.no_curses) {
        program_options.no_curses = program_options.orig_no_curses;
    }

    // If stdout isn't a tty (e.g., if output is being redirected to a file),
    // then we should turn off the ncurses routines.
    if(!program_options.no_curses && !isatty(1)) {
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_NCURSES_STDOUT_NOT_A_TTY);
        program_options.no_curses = 1;
    }

    // Initialize ncurses
    if(!program_options.no_curses) {
        if(screen_setup()) {
            log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_NCURSES_TERMINAL_TOO_SMALL);
            program_options.no_curses = 1;
        } else {
            redraw_screen();
        }
    }

    if(state_file_status == LOAD_STATE_LOAD_ERROR) {
        state_file_error();
        state_file_status = LOAD_STATE_FILE_DOES_NOT_EXIST;
    }

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        if(!program_options.dont_show_warning_message) {
            show_initial_warning_message();
        }
    }

    if(program_options.log_file) {
        if(!(file_handles.log_file = fopen(program_options.log_file, "a"))) {
            log_file_open_error(errno);
            cleanup();
            return -1;
        }
    }

    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_PROGRAM_STARTING, VERSION);

    if(state_file_status == LOAD_STATE_SUCCESS) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_RESUMING_FROM_STATE_FILE, program_options.state_file);
    }

    if(iret = open_lockfile(program_options.lock_file)) {
        lockfile_open_error(iret);
        cleanup();
        return -1;
    }

    if(program_options.stats_file) {
        if(!(file_handles.stats_file = fopen(program_options.stats_file, "a"))) {
            stats_file_open_error(errno);
            cleanup();
            return -1;
        }

        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_LOGGING_STATS_TO_FILE, program_options.stats_file);

        // Write the CSV headers out to the file
        fprintf(file_handles.stats_file,
            "Date/Time,Rounds Completed,Bytes Written,Total Bytes Written,Write Rate (bytes/sec),Bytes Read,Total Bytes Read,Read Rate (bytes/sec),Bad Sectors,Total Bad Sesctors,Bad Sector Rate (counts/min)\n");
        fflush(file_handles.stats_file);
    }

    // Does the system have a working gettimeofday?
    if(gettimeofday(&speed_start_time, NULL) == -1) {
        no_working_gettimeofday(errno);
        cleanup();
        return -1;
    }

    if(state_file_status == LOAD_STATE_SUCCESS && !forced_device) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_FINDING_DEVICE_FROM_STATE_FILE);
        window = message_window(stdscr, NULL, "Finding device described in state file...", 0);

        iret = find_device(program_options.device_name, 0, device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, device_stats.device_uuid,
                           sector_display.sector_map, &new_device_name, &new_device_num, &fd);

        erase_and_delete_window(window);

        if(iret == -1) {
            device_locate_error();
            cleanup();
            return -1;
        } else if(iret > 1) {
            multiple_matching_devices_error();
            cleanup();
            return -1;
        } else if(iret == 0) {
            if(program_options.device_name) {
                wrong_device_specified_error();
                cleanup();
                return -1;
            } else {
                window = no_matching_device_warning();
                if(wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, device_stats.device_uuid,
                                             sector_display.sector_map, &program_options.device_name, &device_stats.device_num, &fd) == -1) {
                    wait_for_device_connect_error(window);
                    cleanup();
                    return -1;
                } else {
                    erase_and_delete_window(window);
                }
            }
        } else {
            if(program_options.device_name) {
                free(program_options.device_name);
            }

            program_options.device_name = new_device_name;
            device_stats.device_num = new_device_num;
        }

        if(fstat(fd, &fs)) {
            log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_FSTAT_ERROR, strerror(errno));
            fstat_error(errno);
            cleanup();
            return -1;
        }
    } else {
        if(forced_device) {
            if(state_file_status != LOAD_STATE_SUCCESS) {
                log_log(NULL, SEVERITY_LEVEL_INFO, MSG_IGNORING_FORCED_DEVICE);
            } else {
                log_log(NULL, SEVERITY_LEVEL_INFO, MSG_USING_FORCED_DEVICE);

                if(program_options.device_name) {
                    free(program_options.device_name);
                }

                program_options.device_name = forced_device;
                forced_device = NULL;
            }
        }

        if((ret = is_block_device(program_options.device_name)) == -1) {
            local_errno = errno;
            log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_STAT_ERROR, strerror(local_errno));
            stat_error(local_errno);
            cleanup();
            return -1;
        } else if(!ret) {
            not_a_block_device_error();
            cleanup();
            return -1;
        }

        if((fd = open(program_options.device_name, O_DIRECT | O_SYNC | O_LARGEFILE | O_RDWR)) == -1) {
            local_errno = errno;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_OPEN_ERROR, strerror(local_errno));
            device_open_error(local_errno);
            cleanup();
            return -1;
        }
    }

    if(ioctl(fd, BLKGETSIZE64, &device_stats.reported_size_bytes) || ioctl(fd, BLKSSZGET, &device_stats.sector_size) ||
        ioctl(fd, BLKSECTGET, &max_sectors_per_request) || ioctl(fd, BLKPBSZGET, &device_stats.physical_sector_size)) {
        local_errno = errno;
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_IOCTL_ERROR, strerror(local_errno));
        ioctl_error(local_errno);
        cleanup();
        return -1;
    }

    device_stats.device_num = fs.st_rdev;

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        device_stats.num_sectors = device_stats.reported_size_bytes / device_stats.sector_size;
    }

    device_stats.preferred_block_size = fs.st_blksize;
    device_stats.max_request_size = device_stats.sector_size * max_sectors_per_request;

    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_HEADER);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_REPORTED_SIZE, device_stats.reported_size_bytes);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_LOGICAL_SECTOR_SIZE, device_stats.sector_size);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_PHYSICAL_SECTOR_SIZE, device_stats.physical_sector_size);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_TOTAL_SECTORS, device_stats.num_sectors);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_PREFERRED_BLOCK_SIZE, fs.st_blksize);
    log_log(NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_MAX_SECTORS_PER_REQUEST, max_sectors_per_request);

    mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_stats.reported_size_bytes);
    refresh();

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        profile_random_number_generator();

        if(program_options.probe_for_optimal_block_size) {
            wait_for_file_lock(NULL);

            if((device_stats.block_size = probe_for_optimal_block_size(fd)) <= 0) {
                device_stats.block_size = device_stats.sector_size * max_sectors_per_request;
                log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_UNABLE_TO_PROBE_FOR_OPTIMAL_BLOCK_SIZE, device_stats.block_size);
            }
        } else {
            device_stats.block_size = device_stats.sector_size * max_sectors_per_request;
        }

        sectors_per_block = device_stats.block_size / device_stats.sector_size;

        wait_for_file_lock(NULL);

        if(program_options.force_sectors) {
            device_stats.num_sectors = program_options.force_sectors;
            device_stats.detected_size_bytes = program_options.force_sectors * device_stats.sector_size;

            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_USING_FORCED_DEVICE_SIZE, device_stats.detected_size_bytes);

            if(device_stats.detected_size_bytes == device_stats.reported_size_bytes) {
                device_stats.is_fake_flash = FAKE_FLASH_NO;
            } else {
                device_stats.is_fake_flash = FAKE_FLASH_YES;
            }

            device_stats.middle_of_device = device_stats.detected_size_bytes / 2;

            if(!program_options.no_curses) {
                mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_stats.detected_size_bytes);
                if(device_stats.detected_size_bytes != device_stats.reported_size_bytes) {
                    attron(COLOR_PAIR(RED_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Yes");
                    attroff(COLOR_PAIR(RED_ON_BLACK));
                } else {
                    attron(COLOR_PAIR(GREEN_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Probably not");
                    attroff(COLOR_PAIR(GREEN_ON_BLACK));
                }
            }
        } else if(!(device_stats.detected_size_bytes = probe_device_size(fd, device_stats.num_sectors, device_stats.block_size))) {
            log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_USING_KERNEL_REPORTED_DEVICE_SIZE, device_stats.reported_size_bytes);

            device_stats.middle_of_device = device_stats.reported_size_bytes / 2;

            if(!program_options.no_curses) {
                mvaddstr(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "Unknown");
                mvaddstr(IS_FAKE_FLASH_DISPLAY_Y       , IS_FAKE_FLASH_DISPLAY_X       , "Unknown");
            }
        } else {
            device_stats.num_sectors = device_stats.detected_size_bytes / device_stats.sector_size;
            if(device_stats.detected_size_bytes == device_stats.reported_size_bytes) {
                device_stats.is_fake_flash = FAKE_FLASH_NO;
            } else {
                device_stats.is_fake_flash = FAKE_FLASH_YES;
            }

            device_stats.middle_of_device = device_stats.detected_size_bytes / 2;

            if(!program_options.no_curses) {
                mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_stats.detected_size_bytes);
                if(device_stats.detected_size_bytes != device_stats.reported_size_bytes) {
                    attron(COLOR_PAIR(RED_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Yes");
                    attroff(COLOR_PAIR(RED_ON_BLACK));
                } else {
                    attron(COLOR_PAIR(GREEN_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Probably not");
                    attroff(COLOR_PAIR(GREEN_ON_BLACK));
                }
            }
        }

        refresh();

        wait_for_file_lock(NULL);

        probe_device_speeds(fd);
    } else {
        device_stats.is_fake_flash = (device_stats.reported_size_bytes == device_stats.detected_size_bytes) ? FAKE_FLASH_NO : FAKE_FLASH_YES;
        sectors_per_block = device_stats.block_size / device_stats.sector_size;
        device_stats.middle_of_device = device_stats.detected_size_bytes / 2;
        redraw_screen();
    }

    // Start stress testing the device.
    //
    // The general strategy is:
    //  - Divide the known writable area into 16 slices.  For each slice (in order):
    //    - Set the seed for the random number generator to a unique value.
    //    - Write random data to every sector in the slice.
    //  - For each slice (in a random order):
    //    - Set the seed for the random number generator to the same value
    //      that we used when we originally wrote the data to the slice
    //    - Read back the data
    //    - Generate the same number of bytes of random data
    //    - Compare what was generated to what we read back, on a
    //      sector-by-sector basis.  If they match, then the sector is good.
    //  - Repeat until at least 50% of the sectors read result in mismatches.
    gettimeofday(&rng_init_time, NULL);
    initial_seed = rng_init_time.tv_sec + rng_init_time.tv_usec;
    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        state_data.first_failure_round = state_data.ten_percent_failure_round = state_data.twenty_five_percent_failure_round = -1;
    }

    rng_init(initial_seed);

    // Allocate buffers for reading from/writing to the device.  We're using
    // posix_memalign because the memory needs to be aligned on a page boundary
    // (since we're doing unbuffered reading/writing).
    if(ret = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), device_stats.block_size)) {
        log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        posix_memalign_error(ret);
        cleanup();
        return -1;
    }

    if(ret = posix_memalign((void **) &compare_buf, sysconf(_SC_PAGESIZE), device_stats.block_size)) {
        log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        posix_memalign_error(ret);
        cleanup();
        return -1;
    }

    // Flash media has a tendency to return either all 0x00's or all 0xff's when
    // it's not able to read a particular sector (for example, when the sector
    // doesn't exist).  These two buffers are going to just hold all 0x00's and
    // all 0xff's to make it easier to do memcmp's against them when a sector
    // doesn't match the expected values.
    zero_buf = (char *) malloc(device_stats.sector_size);
    if(!zero_buf) {
        log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
        malloc_error(errno);
        cleanup();
        return -1;
    }

    memset(zero_buf, 0, device_stats.sector_size);

    ff_buf = (char *) malloc(device_stats.sector_size);
    if(!ff_buf) {
        log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
        malloc_error(errno);
        cleanup();
        return -1;
    }

    memset(ff_buf, 0xff, device_stats.sector_size);

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        if(!(sector_display.sector_map = (char *) malloc(device_stats.num_sectors))) {
            log_log(__func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
            malloc_error(errno);
            cleanup();
            return -1;
        }

        // Initialize the sector map
        memset(sector_display.sector_map, 0, device_stats.num_sectors);
        device_stats.num_bad_sectors = 0;
    }

    // Generate a new UUID for the device if one isn't already assigned.
    if(!memcmp(zero_buf, device_stats.device_uuid, sizeof(uuid_t))) {
        uuid_generate(device_stats.device_uuid);
        if(state_file_status == LOAD_STATE_SUCCESS) {
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ASSIGNING_NEW_DEVICE_ID);
        }

        uuid_unparse(device_stats.device_uuid, device_uuid_str);
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ASSIGNING_DEVICE_ID_TO_DEVICE, device_uuid_str);
    }

    // Start filling up the device
    device_stats.bytes_since_last_status_update = 0;
    memset(&stress_test_stats, 0, sizeof(stress_test_stats));

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_STARTING);
        num_rounds = state_data.bytes_written = state_data.bytes_read = 0;
    } else {
        // Count up the number of bad sectors and update device_stats.num_bad_sectors
        for(j = 0; j < device_stats.num_sectors; j++) {
            if(is_sector_bad(j)) {
                device_stats.num_bad_sectors++;
            }
        }

        stress_test_stats.previous_bytes_written = state_data.bytes_written;
        stress_test_stats.previous_bytes_read = state_data.bytes_read;
        stress_test_stats.previous_bad_sectors = device_stats.num_bad_sectors;

        log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_RESUMING, num_rounds + 1);
    }

    assert(!gettimeofday(&stress_test_stats.previous_update_time, NULL));
    stats_cur_time = stress_test_stats.previous_update_time;

    // Fire up the SQL thread
    sql_thread_status = SQL_THREAD_NOT_CONNECTED;

    if(program_options.db_host && program_options.db_user && program_options.db_pass && program_options.db_name) {
        sql_thread_params.mysql_host = program_options.db_host;
        sql_thread_params.mysql_username = program_options.db_user;
        sql_thread_params.mysql_password = program_options.db_pass;
        sql_thread_params.mysql_port = program_options.db_port;
        sql_thread_params.mysql_db_name = program_options.db_name;
        sql_thread_params.card_name = program_options.card_name;
        sql_thread_params.card_id = program_options.card_id;

        if(iret = pthread_create(&sql_thread, NULL, &sql_thread_main, &sql_thread_params)) {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_ERROR_CREATING_SQL_THREAD, strerror(iret));
        }
    }

    print_sql_status(sql_thread_status);
    prev_sql_thread_status = sql_thread_status;

    for(num_bad_sectors = 0, num_bad_sectors_this_round = 0, num_good_sectors_this_round = 0; device_stats.num_bad_sectors < (device_stats.num_sectors / 2); num_rounds++, num_bad_sectors = 0, num_bad_sectors_this_round = 0, num_good_sectors_this_round = 0) {
        main_thread_status = MAIN_THREAD_STATUS_WRITING;
        draw_percentage(); // Just in case it hasn't been drawn recently

        if(prev_sql_thread_status != sql_thread_status) {
            prev_sql_thread_status = sql_thread_status;
            print_sql_status(sql_thread_status);
        }

        if(num_rounds > 0) {
            if(save_state()) {
                log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_SAVE_STATE_ERROR);
                message_window(stdscr, WARNING_TITLE, "An error occurred while trying to save the program state.  Save stating has been disabled.", 1);

                free(program_options.state_file);
                program_options.state_file = NULL;
            }
        }

        if(prev_sql_thread_status != sql_thread_status) {
            prev_sql_thread_status = sql_thread_status;
            print_sql_status(sql_thread_status);
        }

        is_writing = 1;
        if(!program_options.no_curses) {
            j = snprintf(msg_buffer, sizeof(msg_buffer), " Round %'lu ", num_rounds + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), msg_buffer);
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        }

        reset_sector_map();

        redraw_sector_map();
        refresh();

        read_order = random_list();

        for(cur_slice = 0, restart_slice = 0; cur_slice < NUM_SLICES; cur_slice++, restart_slice = 0) {
            if(ret = endurance_test_write_slice(&fd, num_rounds, initial_seed + read_order[cur_slice] + (num_rounds * NUM_SLICES), read_order[cur_slice], sectors_per_block, bod_buffer, mod_buffer)) {
                main_thread_status = MAIN_THREAD_STATUS_ENDING;

                if(ret > 0) {
                    print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds, ret);
                }

                cleanup();
                return 0;
            }
        }

        free(read_order);

        main_thread_status = MAIN_THREAD_STATUS_READING;
        read_order = random_list();
        device_stats.bytes_since_last_status_update = 0;
        is_writing = 0;

        if(!program_options.no_curses) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        for(cur_slice = 0; cur_slice < NUM_SLICES; cur_slice++) {
            rng_reseed(initial_seed + read_order[cur_slice] + (num_rounds * NUM_SLICES));

            if(lseek_or_retry(&fd, get_slice_start(read_order[cur_slice]) * device_stats.sector_size, &device_was_disconnected) == -1) {
                main_thread_status = MAIN_THREAD_STATUS_ENDING;
                print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds, ABORT_REASON_SEEK_ERROR);
                cleanup();
                return 0;
            }

            if(read_order[cur_slice] == 15) {
                last_sector = device_stats.num_sectors;
            } else {
                last_sector = get_slice_start(read_order[cur_slice] + 1);
            }

            for(cur_sector = get_slice_start(read_order[cur_slice]); cur_sector < last_sector; cur_sector += cur_sectors_per_block) {
                if(sql_thread_status != prev_sql_thread_status) {
                    prev_sql_thread_status = sql_thread_status;
                    print_sql_status(sql_thread_status);
                }

                // Use bytes_left_to_write to hold the bytes left to read
                if((cur_sector + sectors_per_block) > last_sector) {
                    cur_sectors_per_block = last_sector - cur_sector;
                    cur_block_size = cur_sectors_per_block * device_stats.sector_size;
                } else {
                    cur_block_size = device_stats.block_size;
                    cur_sectors_per_block = sectors_per_block;
                }

                // Regenerate the data we originally wrote to the device.
                rng_fill_buffer(buf, cur_block_size);
                bytes_left_to_write = cur_block_size;

                // Re-embed the sector number and CRC32 into the expected data
                for(i = 0; i < cur_sectors_per_block; i++) {
                    embed_sector_and_round_number(&(buf[i * device_stats.sector_size]), cur_sector + i, num_rounds);
                    embed_device_uuid(&(buf[i * device_stats.sector_size]));
                    embed_crc32c(&(buf[i * device_stats.sector_size]), device_stats.sector_size);
                }

                if(endurance_test_read_block(&fd, cur_sector, cur_sectors_per_block, compare_buf, &device_was_disconnected)) {
                    main_thread_status = MAIN_THREAD_STATUS_ENDING;
                    print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds,
                                         ABORT_REASON_READ_ERROR);

                    cleanup();
                    return 0;
                }

                mark_sectors_read(cur_sector, cur_sector + cur_sectors_per_block);
                state_data.bytes_read += cur_block_size;

                // Compare
                for(j = 0; j < cur_block_size; j += device_stats.sector_size) {
                    handle_key_inputs(NULL);
                    if(memcmp(buf + j, compare_buf + j, device_stats.sector_size)) {
                        if(!is_sector_bad(cur_sector + (j / device_stats.sector_size))) {
                            if(!memcmp(compare_buf + j, zero_buf, device_stats.sector_size)) {
                                // The data in the sector is all zeroes
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_SECTOR_ALL_00S, cur_sector + (j / device_stats.sector_size));
                            } else if(!memcmp(compare_buf + j, ff_buf, device_stats.sector_size)) {
                                // The data in the sector is all 0xff's
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_SECTOR_ALL_FFS, cur_sector + (j / device_stats.sector_size));
                            } else if(calculate_crc32c(0, compare_buf + j, device_stats.sector_size)) {
                                // The CRC-32 embedded in the sector data doesn't match the calculated CRC-32
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_CRC32_MISMATCH, cur_sector + (j / device_stats.sector_size), get_embedded_crc32c(compare_buf + j, device_stats.sector_size),
                                        calculate_crc32c(0, compare_buf + j, device_stats.sector_size - sizeof(uint32_t)));
                                log_sector_contents(cur_sector + (j / device_stats.sector_size), device_stats.sector_size, buf + j, compare_buf + j);
                            } else if(decode_embedded_round_number(compare_buf + j) != num_rounds) {
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_WRITE_FAILURE, cur_sector + (j / device_stats.sector_size), decode_embedded_round_number(compare_buf + j) + 1,
                                        decode_embedded_sector_number(compare_buf + j));
                            } else if(decode_embedded_sector_number(compare_buf + j) != (cur_sector + (j / device_stats.sector_size))) {
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_ADDRESS_DECODING_FAILURE, cur_sector + (j / device_stats.sector_size), decode_embedded_sector_number(compare_buf + j));
                            } else {
                                log_log(NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_GENERIC, cur_sector + (j / device_stats.sector_size));
                                log_sector_contents(cur_sector + (j / device_stats.sector_size), device_stats.sector_size, buf + j, compare_buf + j);
                            }

                            num_bad_sectors++;
                        }

                        mark_sector_bad(cur_sector + (j / device_stats.sector_size));
                    } else {
                        if(is_sector_bad(cur_sector + (j / device_stats.sector_size))) {
                            num_good_sectors_this_round++;
                        }
                    }
                }

                refresh();

                assert(!gettimeofday(&stats_cur_time, NULL));
                if(timediff(stress_test_stats.previous_update_time, stats_cur_time) >= (program_options.stats_interval * 1000000)) {
                    stats_log(num_rounds, device_stats.num_bad_sectors);
                }
            }
        }

        free(read_order);
        read_order = NULL;

        if(!num_bad_sectors && !device_stats.num_bad_sectors) {
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUND_COMPLETE_NO_BAD_SECTORS, num_rounds + 1);
        } else {
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUND_COMPLETE_WITH_BAD_SECTORS, num_rounds + 1);
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_BAD_SECTORS_THIS_ROUND, num_bad_sectors_this_round);
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_NEW_BAD_SECTORS_THIS_ROUND, num_bad_sectors);
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_PREVIOUSLY_BAD_SECTORS_NOW_GOOD, num_good_sectors_this_round);
            log_log(NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_TOTAL_BAD_SECTORS, device_stats.num_bad_sectors, (((double) device_stats.num_bad_sectors) / ((double) device_stats.num_sectors)) * 100);
        }
    }

    main_thread_status = MAIN_THREAD_STATUS_ENDING;
    print_device_summary(num_rounds - 1, num_rounds, ABORT_REASON_FIFTY_PERCENT_FAILURE);

    cleanup();
    return 0;
}

