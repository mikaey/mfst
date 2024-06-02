#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include <assert.h>
#include <locale.h>
#include <curses.h>
#include <unistd.h>
#include <getopt.h>
#include "mfst.h"
#include "state.h"
#include "device.h"
#include "util.h"

// Since we use these strings so frequently, these are just here to save space
const char *WARNING_TITLE = "WARNING";
const char *ERROR_TITLE = "ERROR";


/**
 * Test to see if the lockfile is locked.
 * 
 * @returns Zero if the lockfile is not locked, or non-zero if it is.
 */
int is_lockfile_locked() {
    int retval = lockf(file_handles.lockfile_fd, F_TEST, 0);
    return (retval == -1 && (errno == EACCES || errno == EAGAIN));
}

/**
 * Locks the lockfile.
 * 
 * @returns Zero if the lockfile was locked successfully, or non-zero if it was
 *          not.
 */
int lock_lockfile() {
    return lockf(file_handles.lockfile_fd, F_TLOCK, 0);
}

/**
 * Unlocks the lockfile.
 * 
 * @returns Zero if the lockfile was unlocked successfully, or non-zero if it
 *          was not.
 */
int unlock_lockfile() {
    return lockf(file_handles.lockfile_fd, F_ULOCK, 0);
}

/**
 * Log the given string to the log file, if the log file is open.  If curses
 * mode is turned off, also log the given string to stdout.  The time is
 * prepended to the message, and a newline is appended to the message.
 *
 * @param msg       The null-terminated string to write to the log file.
 */
void log_log(char *msg) {
    time_t now = time(NULL);
    char *t = ctime(&now);
    // Get rid of the newline on the end of the time
    t[strlen(t) - 1] = 0;
    if(file_handles.log_file) {
        fprintf(file_handles.log_file, "[%s] %s\n", t, msg);
        fflush(file_handles.log_file);
    }

    if(program_options.no_curses) {
        printf("[%s] %s\n", t, msg);
        syncfs(1);
    }
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
void stats_log(size_t rounds, size_t bad_sectors) {
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
 * Erases the given window, refreshes it, then deletes it.
 *
 * @param window  The window to erase and delete.
 */
void erase_and_delete_window(WINDOW *window) {
    if(!program_options.no_curses) {
        werase(window);
        touchwin(stdscr);
        wrefresh(window);
        delwin(window);
    }
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
 */
void draw_sector(size_t sector_num, int color, int with_diamond) {
    int block_num, row, col;

    if(program_options.no_curses) {
        return;
    }

    block_num = sector_num / sector_display.sectors_per_block;
    row = block_num / sector_display.blocks_per_line;
    col = block_num - (row * sector_display.blocks_per_line);
    attron(COLOR_PAIR(color));

    if(with_diamond) {
        mvaddch(row + SECTOR_DISPLAY_Y, col + SECTOR_DISPLAY_X, ACS_DIAMOND);
    } else {
        mvaddstr(row + SECTOR_DISPLAY_Y, col + SECTOR_DISPLAY_X, " ");
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
void draw_sectors(size_t start_sector, size_t end_sector) {
    size_t i, j, num_sectors_in_cur_block, num_written_sectors, num_read_sectors;
    size_t min, max;
    char cur_block_has_bad_sectors;
    int color;
    int this_round;

    min = start_sector / sector_display.sectors_per_block;
    max = (end_sector / sector_display.sectors_per_block) + ((end_sector % sector_display.sectors_per_block) ? 1 : 0);

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

        for(j = i * sector_display.sectors_per_block; j < ((i * sector_display.sectors_per_block) + num_sectors_in_cur_block); j++) {
            cur_block_has_bad_sectors |= sector_display.sector_map[j] & 0x01;
            num_written_sectors += (sector_display.sector_map[j] & 0x02) >> 1;
            num_read_sectors += (sector_display.sector_map[j] & 0x04) >> 2;
            this_round |= (sector_display.sector_map[j] & 0x08);
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

        draw_sector(i * sector_display.sectors_per_block, color, this_round);
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
void mark_sectors_written(size_t start_sector, size_t end_sector) {
    size_t i;

    for(i = start_sector; i < end_sector && i < device_stats.num_sectors; i++) {
        sector_display.sector_map[i] |= 0x02;
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
void mark_sectors_read(size_t start_sector, size_t end_sector) {;
    size_t i;

    for(i = start_sector; i < end_sector && i < device_stats.num_sectors; i++) {
        sector_display.sector_map[i] |= 0x04;
    }

    draw_sectors(start_sector, end_sector);
}

/**
 * Mark the given sector as "bad" in the sector map.  The block containing the
 * given sector is redrawn on the display.  The display is not refreshed after
 * the blocks are drawn.
 * 
 * @param sector_num  The sector number of the sector to be marked as bad.
 */
void mark_sector_bad(size_t sector_num) {
    if(!(sector_display.sector_map[sector_num] & 0x01)) {
        device_stats.num_bad_sectors++;
    }

    sector_display.sector_map[sector_num] |= 0x09;

    draw_sector(sector_num, BLACK_ON_YELLOW, 1);
}

/**
 * Determines whether the given sector is marked as "bad" in the sector map.
 * 
 * @param sector_num  The sector number of the sector to query.
 * 
 * @returns Non-zero if the sector has been marked as "bad" in the sector map,
 *          zero otherwise.
 */
char is_sector_bad(size_t sector_num) {
    return sector_display.sector_map[sector_num] & 0x01;
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

    if(device_stats.num_sectors % sector_display.num_blocks) {
        sector_display.sectors_per_block = device_stats.num_sectors / (sector_display.num_blocks - 1);
        sector_display.sectors_in_last_block = device_stats.num_sectors % (sector_display.num_blocks - 1);
    } else {
        sector_display.sectors_per_block = device_stats.num_sectors / sector_display.num_blocks;
        sector_display.sectors_in_last_block = sector_display.sectors_per_block;
    }

    if(sector_display.sectors_per_block) {
        mvprintw(BLOCK_SIZE_DISPLAY_Y, BLOCK_SIZE_DISPLAY_X, "%'lu bytes", sector_display.sectors_per_block * device_stats.sector_size);
    }

    if(!sector_display.sector_map) {
        return;
    }

    draw_sectors(0, device_stats.num_sectors);
}

/**
 * Resets the random number generator and gives it the given seed.
 * 
 * @param seed  The seed to provide to the random number generator.
*/
void init_random_number_generator(unsigned int seed) {
    memset(random_number_state_buf, 0, sizeof(random_number_state_buf));
    initstate_r(seed, random_number_state_buf, sizeof(random_number_state_buf), &random_state);
}

/**
 * Obtains a random number from the random number generator.  Since random()
 * only generates 31 bits of random data, this function randomizes the uppermost
 * bit of the result.
 * 
 * @returns The generated random number.
*/
int32_t get_random_number() {
    int32_t result;
    random_r(&random_state, &result);

    // random() and random_r() generate random numbers between 0 and
    // 0x7FFFFFFF -- which means we're not testing 1 out of every 32 bits on
    // the device if we just accept this value.  To spice things up a little,
    // we'll throw some extra randomness into the top bit.
    result |= ((current_seed & result & 0x00000001) | (~(current_seed & (result >> 1)) & 0x00000001)) << 31;
    return result;
}

/**
 * Fills `buffer` with `size` random bytes.
 * 
 * @param buffer  A pointer to the buffer to be populated with random bytes.
 * 
 * @param size    The number of bytes to write to the buffer.  Must be a
 *                multiple of 4 -- if it isn't, it will be rounded up to the
 *                next multiple of 4.
*/
void fill_buffer(char *buffer, size_t size) {
    int32_t *int_buffer = (int32_t *) buffer;
    size_t i;
    for(i = 0; i < (size / 4); i++) {
        int_buffer[i] = get_random_number();
    }
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

/**
 * Redraws the entire screen.  Useful on initial setup or when the screen has
 * been resized.
 */
void redraw_screen() {
    char str[128];
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
        attroff(A_BOLD);

        // Draw the device name
        snprintf(str, 23, "%s ", program_options.device_name);
        mvaddstr(DEVICE_NAME_DISPLAY_Y, DEVICE_NAME_DISPLAY_X, str);

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
            j = snprintf(str, sizeof(str), " Round %'lu ", num_rounds + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), str);
        }

        if(is_writing == 1) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        } else if(is_writing == 0) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        // Draw the reported size of the device if it's been determined
        if(device_stats.reported_size_bytes) {
            snprintf(str, 26, "%'lu bytes", device_stats.reported_size_bytes);
            mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%-25s", str);
        }

        // Draw the detected size of the device if it's been determined
        if(device_stats.detected_size_bytes) {
            snprintf(str, 26, "%'lu bytes", device_stats.detected_size_bytes);
            mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%-25s", device_stats.detected_size_bytes ? str : "");
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
            mvaddstr(SEQUENTIAL_READ_SPEED_DISPLAY_Y, SEQUENTIAL_READ_SPEED_DISPLAY_X, format_rate(device_speeds.sequential_read_speed, str, 31));
        }

        if(device_speeds.sequential_write_speed) {
            mvaddstr(SEQUENTIAL_WRITE_SPEED_DISPLAY_Y, SEQUENTIAL_WRITE_SPEED_DISPLAY_X, format_rate(device_speeds.sequential_write_speed, str, 31));
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
        refresh();
    }
}

/**
 * A wrapper for getch()/wgetch() that handles KEY_RESIZE events.
 * 
 * @param curwin  The active window, or NULL if no window/stdscr is the active
 *                window.
 * 
 * @returns If getch()/wgetch() returns KEY_RESIZE, this function intercepts
 *          the event and returns ERR.  Otherwise, this function returns
 *          whatever getch()/wgetch() returned.
 */
int handle_key_inputs(WINDOW *curwin) {
    int key, width, height;

    if(curwin) {
        key = wgetch(curwin);
    } else {
        key = getch();
    }

    if(key == KEY_RESIZE) {
        if(curwin) {
            getmaxyx(curwin, height, width);
            mvwin(curwin, (LINES - height) / 2, (COLS - width) / 2);
        }

        erase();
        redraw_screen();

        if(curwin) {
            touchwin(curwin);
        }

        refresh();

        return ERR;
    }

    return key;
}

/**
 * Create a window and show a message in it.  Afterwards, the parent window
 * is touched and the window is refreshed.
 *
 * The strings in `msg` are displayed in the new window, one per line.  No
 * wrapping is applied.  If any part of the window would overflow off the edge
 * of the screen, the window is not created and NULL is returned.
 *
 * If `wait` is set to a non-zero value, two lines are added to the bottom of
 * the window: a blank line, and a line that shows "Press Enter to continue",
 * centered in the window.  The length of this string is taken into account
 * when calculating the width of the window.  The function will then block and
 * wait for the user to press Enter.  Afterwards, the function will erase and
 * destroy the window, and the function will return NULL.
 *
 * @param parent  The window which will serve as the parent for the new window.
 * @param title   The title that will be displayed at the top of the window.
 *                If this is set to NULL, no title is displayed.
 * @param msg     A pointer to an array of strings that will be shown in the
 *                new window.  The last element of the array must be set to
 *                NULL to denote the end of the array.
 * @param wait    Non-zero to indicate that the function should block and wait
 *                for the user to press Enter, or zero to indicate that the
 *                function should return immediately.
 *
 * @returns A pointer to the new window, or NULL if (a) a member of `msg` is
 *          too long and would cause the window to overflow off the edge of the
 *          display, (b) wait is set to a non-zero value, or (c) curses mode is
 *          turned off.  If a pointer to a new window is returned, it is the
 *          callers responsibility to delete it when done.
 */
WINDOW *message_window(WINDOW *parent, const char *title, char **msg, char wait) {
    WINDOW *window;
    int lines, len, longest;

    if(program_options.no_curses) {
        return NULL;
    }

    longest = 0;

    for(lines = 0; msg[lines]; lines++) {
        len = strlen(msg[lines]);
        if(len > longest) {
            longest = len;
        }
    }

    if(title) {
        len = strlen(title);
        if(len > longest) {
            longest = len;
        }
    }

    if(wait) {
        // If the "Press Enter to continue" line is longer than the longest line,
        // increase the length of the longest line to 23.
        if(longest < 23) {
            longest = 23;
        }
    }

    // If any line is longer than the width of the display, abort.
    if((longest + 4) > COLS) {
        return NULL;
    }

    // If there are more rows than there are lines on the display, abort.
    if((lines + 2) > LINES) {
        return NULL;
    }

    window = newwin(lines + 2 + (wait ? 2 : 0), longest + 4, (LINES - (lines + 2 + (wait ? 2 : 0))) / 2, (COLS - (longest + 4)) / 2);
    nodelay(window, TRUE);
    werase(window);
    box(window, 0, 0);

    if(title) {
        wattron(window, A_BOLD);
        mvwprintw(window, 0, ((longest + 4) - (len + 2)) / 2, " %s ", title);
        wattroff(window, A_BOLD);
    }

    for(len = 0; len < lines; len++) {
        mvwaddstr(window, len + 1, 2, msg[len]);
    }

    if(wait) {
        wattron(window, A_BOLD);
        mvwaddstr(window, lines + 2, (longest - 19) / 2, "Press Enter to continue");
        wattroff(window, A_BOLD);
    }

    wrefresh(window);

    if(wait) {
        while(handle_key_inputs(window) != '\r') {
            napms(100);
        }
        erase_and_delete_window(window);
        return NULL;
    } else {
        return window;
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
    
    if(is_lockfile_locked()) {
        log_log("Detected another copy of this program is running speed tests.  Suspending execution.");
        if(!program_options.no_curses) {
            if(topwin) {
                assert(memfile = fmemopen(NULL, 131072, "r+"));
                putwin(*topwin, memfile);
                rewind(memfile);
            }

            window = message_window(stdscr, "Paused", (char *[]) {
                "Another copy of this program is running its speed tests.  To increase the",
                "accuracy of those tests, we've paused what we're doing while the other program",
                "is running its speed tests.  Things will resume automatically here once the",
                "other program is finished.",
                NULL
            }, 0);
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

        log_log("Other program is finished.  Resuming execution.");

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
    char message[128];
    WINDOW *window;

    log_log("profile_random_number_generator(): Profiling random number generator");
    window = message_window(stdscr, NULL, (char *[]) { "Profiling random number generator...", NULL }, 0);

    // Generate random numbers for 5 seconds.
    init_random_number_generator(0);
    assert(!gettimeofday(&start_time, NULL));
    do {
        for(i = 0; i < 100; i++) {
            get_random_number();
            total_random_numbers_generated++;
        }
        assert(!gettimeofday(&end_time, NULL));
        handle_key_inputs(window);
        diff = timediff(start_time, end_time);
    } while(diff <= (RNG_PROFILE_SECS * 1000000));

    // Turn total number of random numbers into total number of bytes.
    total_random_numbers_generated *= sizeof(int);

    log_log("profile_random_number_generator(): Finished profiling random number generator.");
    snprintf(message, sizeof(message), "profile_random_number_generator(): System can generate %s of random data.",
        format_rate(((double) total_random_numbers_generated) / (((double) timediff(start_time, end_time)) / 1000000.0), rate_str, sizeof(rate_str)));
    log_log(message);

    if(window) {
        erase_and_delete_window(window);
    }

    if(total_random_numbers_generated < 471859200) {
        // Display a warning message to the user
        log_log("profile_random_number_generator(): WARNING: System is too slow to test all possible speed qualifications.  Speed test results may be inaccurate.");
        i = snprintf(message, sizeof(message), "Your system is only able to generate %s of random data.",
            format_rate(((double) total_random_numbers_generated) / (((double) diff) / 1000000.0), rate_str, sizeof(rate_str)));
        message_window(stdscr, WARNING_TITLE, (char *[]) {
            message,
            "The device may appear to be slower than it actually is, and speed test results",
            "may be inaccurate.",
            NULL
        }, 1);
    }

    return ((double) total_random_numbers_generated) / (((double) diff) / 1000000.0);
}

/**
 * Probe the device to determine the optimal size of write requests.
 * 
 * The test involves writing 256MB of data to the device in a sequential
 * fashion using various block sizes from 512 bytes to 64MB.  Block sizes that
 * are smaller than the device's optimal block size (as returned by the fstat()
 * call) and bigger than the maximum number of sectors per request (as returned
 * by ioctl(BLKSECTGET) call) are excluded.  The amount of time it takes to
 * write the data using each block size is measured.
 * 
 * @param fd  The file descriptor of the device being measured.
 * 
 * @returns The block size which the device was able to write the quickest, in
 *          bytes.
 */
int probe_for_optimal_block_size(int fd) {
    struct timeval start_time, end_time, cur_time, prev_time;
    int cur_pow; // Block size, expressed as 2^(cur_pow+9)
    size_t cur_block_size, total_bytes_written, cur_block_bytes_left, ret;
    char rate_buffer[13], *buf, msg[128];
    double rates[18], highest_rate;
    int highest_rate_pow;
    int min, max;
    int prev_percent, cur_percent, local_errno;
    WINDOW *window;

    const int buf_size = 268435456; // 256MB
    // We'll probe each power of 2 between 512 bytes and 64MB.  There are 18 of
    // them in total.
    const char *labels[] = {
        "512B",
        "1KB",
        "2KB",
        "4KB",
        "8KB",
        "16KB",
        "32KB",
        "64KB",
        "128KB",
        "256KB",
        "512KB",
        "1MB",
        "2MB",
        "4MB",
        "8MB",
        "16MB",
        "32MB",
        "64MB"
    };

    // Get a lock on the lockfile.
    if(lock_lockfile()) {
        local_errno = errno;
        snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): lockf() returned an error: %s", strerror(local_errno));
        log_log(msg);
        log_log("probe_for_optimal_block_size(): Skipping optimal write block size test.");

        message_window(stdscr, ERROR_TITLE, (char *[]) {
            "Unable to obtain a lock on the lockfile.  For now, we'll skip the optimal write",
            "block size test and use other means to determine the optimal write block size.",
            "However, if this happens again, other tests may fail or lock up.",
            "",
            "The error we got while trying to get a lock on the lockfile was:",
            strerror(local_errno),
            NULL
        }, 1);

        return -1;
    }

    // Don't bother testing below the device-specified optimal block size or
    // above the maximum number of sectors per request.
    for(min = 0; (1 << (min + 9)) < device_stats.preferred_block_size && min <= 17; min++) {}
    for(max = 17; (1 << (max + 9)) > device_stats.max_request_size && max > (min + 1); max--) {}

    log_log("probe_for_optimal_block_size(): Probing for optimal write block size...");
    window = message_window(stdscr, "Probing for optimal write block size", (char *[]) {
        "",
        "                                        ", // Make room for the progress bar
        NULL
    }, 0);

    buf = valloc(buf_size);
    if(!buf) {
        unlock_lockfile();

        snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): valloc() returned an error: %s", strerror(errno));
        log_log(msg);

        erase_and_delete_window(window);
        message_window(stdscr, WARNING_TITLE, (char *[]) {
            "We ran into an error while trying to allocate memory for the",
            "optimal write block size test.  This could mean your system"
            "is low on memory.  For now, we'll use other data to",
            "determine the optimal write block size.",
            NULL
        }, 1);

        return -1;
    }

    highest_rate = 0;
    highest_rate_pow = 0;

    for(cur_pow = min; cur_pow <= max; cur_pow++) {
        cur_block_size = 1 << (cur_pow + 9);
        prev_percent = 0;
        cur_percent = 0;

        assert(!gettimeofday(&end_time, NULL));

        if(!program_options.no_curses) {
            snprintf(msg, 41, "Trying %s per request", labels[cur_pow]);
            mvwprintw(window, 1, 2, "%-40s", msg);
            wattron(window, COLOR_PAIR(BLACK_ON_WHITE));
            mvwprintw(window, 2, 2, "%-40s", "");
            wattroff(window, COLOR_PAIR(BLACK_ON_WHITE));
            touchwin(stdscr);
            wrefresh(window);
        }

        // Generate random data (so that we know the device isn't caching it each time)
        init_random_number_generator(end_time.tv_sec);
        fill_buffer(buf, buf_size);

        assert(!gettimeofday(&start_time, NULL));
        prev_time = start_time;

        // Write the bytes out to the device.
        for(total_bytes_written = 0; total_bytes_written < buf_size; total_bytes_written += cur_block_size) {
            cur_block_bytes_left = cur_block_size;
            while(cur_block_bytes_left) {
                ret = write(fd, buf + total_bytes_written + (cur_block_size - cur_block_bytes_left), cur_block_bytes_left);
                if(ret == -1) {
                    free(buf);
                    unlock_lockfile();

                    snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): write() returned an error: %s", strerror(errno));
                    log_log(msg);

                    erase_and_delete_window(window);
                    message_window(stdscr, WARNING_TITLE, (char *[]) {
                        "We ran into an error while trying to probe for the optimal write block size.  It",
                        "could be that the device was removed, experienced an error and disconnected",
                        "itself, or set itself to read-only.  For now, we'll use other means to determine",
                        "the optimal write block size -- but if the device really has been removed or set",
                        "to read-only, the remainder of the tests are going to fail pretty quickly.",
                        NULL
                    }, 1);

                    return -1;
                } else {
                    cur_block_bytes_left -= ret;
                }
            }

            handle_key_inputs(NULL);

            cur_percent = ((total_bytes_written + cur_block_size) * 40) / buf_size;
            if(cur_percent != prev_percent) {
                // Advance the graph
                if(!program_options.no_curses) {
                    wattron(window, COLOR_PAIR(BLACK_ON_GREEN));
                    mvwprintw(window, 2, 2, "%*s", cur_percent, "");
                    wattroff(window, COLOR_PAIR(BLACK_ON_GREEN));
                    touchwin(stdscr);
                    wrefresh(window);
                }

                prev_percent = cur_percent;
            }

            assert(!gettimeofday(&cur_time, NULL));
            if(timediff(prev_time, cur_time) >= 500000) {
                if(!program_options.no_curses) {
                    snprintf(buf, 41, "Trying %s per request (%s)", labels[cur_pow],
                        format_rate(((double)(total_bytes_written + cur_block_size)) / (((double)timediff(start_time, cur_time)) / 1000000), rate_buffer,
                        sizeof(rate_buffer)));
                    mvwprintw(window, 1, 2, "%-40s", buf);
                    touchwin(stdscr);
                    wrefresh(window);
                }

                prev_time = cur_time;
            }
        }

        assert(!gettimeofday(&end_time, NULL));

        if(lseek(fd, 0, SEEK_SET) == -1) {
            local_errno = errno;
            unlock_lockfile();
            free(buf);

            snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): lseek() returned an error: %s", strerror(local_errno));
            log_log(msg);
            log_log("probe_for_optimal_block_size(): Aborting test");

            erase_and_delete_window(window);

            message_window(stdscr, WARNING_TITLE, (char *[]) {
                "We encountered an error while trying to probe for the optimal write block size.",
                "It could be that the device was disconnected, or experienced an error and",
                "disconnected itself.  For now, we'll use other means to determine the optimal",
                "write block size -- but if the device really has been removed, the remainder of",
                "the tests are going to fail pretty quickly.",
                "",
                "The error we encountered was:",
                strerror(local_errno),
                NULL
            }, 1);

            return -1;
        }

        rates[cur_pow] = buf_size / (((double) timediff(start_time, end_time)) / 1000000);
        snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): %5s: %s", labels[cur_pow], format_rate(rates[cur_pow], rate_buffer, sizeof(rate_buffer)));
        log_log(msg);

        // After a certain point, the increase in speeds is trivial.
        // Therefore, we'll only accept higher speeds for bigger block sizes
        // if it's more than a 5% increase in speed.
        if(rates[cur_pow] > (highest_rate * 1.05)) {
            highest_rate = rates[cur_pow];
            highest_rate_pow = cur_pow;
        }
    }

    unlock_lockfile();

    free(buf);
    erase_and_delete_window(window);

    snprintf(msg, sizeof(msg), "probe_for_optimal_block_size(): Optimal write block size test complete; optimal write block size is %'d bytes",
        1 << (highest_rate_pow + 9));
    log_log(msg);

    return 1 << (highest_rate_pow + 9);
}

struct timeval last_update_time;
void print_status_update(size_t cur_sector, size_t num_rounds) {
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

int write_data_to_device(int fd, void *buf, size_t len, size_t optimal_block_size) {
    size_t block_size, bytes_left, block_bytes_left;
    char *aligned_buf;
    ssize_t ret;

    if(!(aligned_buf = (char *) valloc(len))) {
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
                free(aligned_buf);
                return -1;
            }

            block_bytes_left -= ret;
            bytes_left -= ret;
        }
    }

    free(aligned_buf);
    return 0;
}

void lseek_error_during_size_probe() {
    char msg[128];
    snprintf(msg, sizeof(msg), "probe_device_size(): lseek() returned an error: %s", strerror(errno));
    log_log(msg);
    log_log("probe_device_size(): Aborting device size test");

    message_window(stdscr, WARNING_TITLE, (char *[]) {
        "We encountered an error while trying to move around the device.  It could be",
        "that the device was removed or experienced an error and disconnected itself.",
        "For now, we'll assume that the device is the size it says it is -- but if the",
        "device has actually been disconnected, the remainder of the tests are going to",
        "fail pretty quickly.",
        NULL
    }, 1);
}

void write_error_during_size_probe() {
    char msg[128];
    snprintf(msg, sizeof(msg), "probe_device_size(): write() returned an error: %s", strerror(errno));
    log_log(msg);
    log_log("probe_device_size(): Aborting device size test");

    message_window(stdscr, WARNING_TITLE, (char *[]) {
        "We encountered an error while trying to write to the device.  It could be that",
        "the device was removed, experienced an error and disconnected itself, or set",
        "set itself to read-only.  For now, we'll assume that the device is the size it",
        "it says it is -- but if the device has actually been disconnected or set to",
        "read-only, the remainder of the tests are going to fail pretty quickly.",
        NULL
    }, 1);
}

void memory_error_during_size_probe() {
    char msg[128];

    snprintf(msg, sizeof(msg), "probe_device_size(): valloc() returned an error: %s", strerror(errno));
    log_log(msg);
    log_log("probe_device_size(): Aborting device size test");

    message_window(stdscr, WARNING_TITLE, (char *[]) {
        "We encountered an error while trying to allocate memory to test the size of the",
        "device.  For now, we'll assume that the device is the size it says it is -- but",
        "if the device is fake flash, the remainder of the tests are going to fail pretty",
        "quickly.",
        NULL
    }, 1);
}

// This whole method assumes that no card is going to have a 16MB (or bigger)
// cache.  If it turns out that there are cards that do have bigger caches,
// then we might need to come back and revisit this.
size_t probe_device_size(int fd, size_t num_sectors, size_t optimal_block_size) {
    // Start out by writing to 9 different places on the card to minimize the
    // chances that the card is interspersed with good blocks.
    char *buf, *readbuf, keep_searching, str[256];
    unsigned int random_seed, i, bytes_left, ret;
    size_t initial_sectors[9];
    size_t low, high, cur, size, j;
    const size_t slice_size = 4194304;
    const size_t num_slices = 9;
    const size_t buf_size = slice_size * num_slices;
    WINDOW *window;

    log_log("probe_device_size(): Probing for actual device size");
    window = message_window(stdscr, NULL, (char *[]) { "Probing for actual device size...", NULL }, 0);

    buf = valloc(buf_size);
    if(!buf) {
        erase_and_delete_window(window);
        memory_error_during_size_probe();
        return 0;
    }

    readbuf = valloc(buf_size);
    if(!readbuf) {
        erase_and_delete_window(window);
        free(buf);
        memory_error_during_size_probe();
        return 0;
    }

    random_seed = time(NULL);
    init_random_number_generator(random_seed);
    fill_buffer(buf, buf_size);

    // Decide where we'll put the initial data.  The first and last writes will
    // go at the beginning and end of the card; the other writes will be at
    // random sectors in each 1/8th of the card.
    initial_sectors[0] = 0;
    initial_sectors[num_slices - 1] = num_sectors - (1 + (slice_size / device_stats.sector_size));

    // Make sure we don't overwrite the initial set of sectors
    low = slice_size / device_stats.sector_size;
    high = num_sectors / 8;

    for(i = 1; i < (num_slices - 1); i++) {
        initial_sectors[i] = low + ((get_random_number() & RAND_MAX) % (high - low));
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
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            lseek_error_during_size_probe();

            return 0;            
        }

        if(write_data_to_device(fd, buf + ((i - 1) * slice_size), slice_size, optimal_block_size)) {
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            write_error_during_size_probe();

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
            multifree(2, buf, readbuf);

            erase_and_delete_window(window);
            lseek_error_during_size_probe();

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
                    if(j == 0) {
                        multifree(2, buf, readbuf);

                        log_log("probe_device_size(): Unable to determine device size: first sector isn't stable");
                        erase_and_delete_window(window);
                        message_window(stdscr, WARNING_TITLE, (char *[]) {
                            "The first sector of this device isn't stable.  This means we",
                            "have no basis to figure out what the device's actual",
                            "capacity is.  It could be that this is wraparound flash",
                            "(which this program isn't designed to handle), that the",
                            "first sector is bad, or that the device has no usable",
                            "storage whatsoever.",
                            "",
                            "For now, we'll assume that the device is the size it says it",
                            "is -- but if it is actually fake flash, the endurance test",
                            "is going to fail during the first round.",
                            NULL
                        }, 1);

                        return 0;
                    } else {
                        erase_and_delete_window(window);

                        snprintf(str, sizeof(str), "probe_device_size(): Device is %'lu bytes in size", j);
                        log_log(str);

                        return j;
                    }
                } else {
                    if(j > 0) {
                        erase_and_delete_window(window);
                        multifree(2, buf, readbuf);

                        snprintf(str, sizeof(str), "probe_device_size(): Device is %'lu bytes in size", (initial_sectors[i] * device_stats.sector_size) + j);
                        log_log(str);

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

        snprintf(str, sizeof(str), "probe_device_size(): Device is %'lu bytes in size", num_sectors * device_stats.sector_size);
        log_log(str);

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
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            lseek_error_during_size_probe();

            return 0;
        }

        // Generate some more random data
        fill_buffer(buf, slice_size * num_slices);
        if(write_data_to_device(fd, buf, slice_size * num_slices, optimal_block_size)) {
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            write_error_during_size_probe();

            return 0;
        }

        if(lseek(fd, cur * device_stats.sector_size, SEEK_SET) == -1) {
            erase_and_delete_window(window);
            lseek_error_during_size_probe();

            multifree(2, buf, readbuf);

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

                        snprintf(str, sizeof(str), "probe_device_size(): Device is %'lu bytes in size",
                            (cur * device_stats.sector_size) + (i * slice_size) + j);
                        log_log(str);

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

    snprintf(str, sizeof(str), "probe_device_size(): Device is %'lu bytes in size", low * device_stats.sector_size);
    log_log(str);

    return low * device_stats.sector_size;
}

void lseek_error_during_speed_test() {
    int local_errno;
    char msg[128];

    local_errno = errno;
    snprintf(msg, sizeof(msg), "probe_device_speeds(): lseek() returned an error: %s", strerror(local_errno));
    log_log(msg);
    log_log("probe_device_size(): Aborting speed tests");

    message_window(stdscr, WARNING_TITLE, (char *[]) {
        "We got an error while trying to move around the device.  It could be that the",
        "device was removed or experienced an error and disconnected itself.  If that's",
        "the case, the remainder of the tests are going to fail pretty quickly.",
        "",
        "Unfortunately, this means that we won't be able to complete the speed tests.",
        "",
        "Here's the error we got while trying to move around the device:",
        strerror(local_errno),
        NULL
    }, 1);
}

void io_error_during_speed_test(char write) {
    int local_errno;
    char msg[128];

    local_errno = errno;
    snprintf(msg, sizeof(msg), "probe_device_speeds(): %s() returned an error: %s", write ? "write" : "read", strerror(local_errno));
    log_log(msg);
    log_log("probe_device_size(): Aborting speed tests");

    snprintf(msg, sizeof(msg), "We got an error while trying to %s the device.  It could be that the device", write ? "write to" : "read from");
    message_window(stdscr, WARNING_TITLE, (char *[]) {
        msg,
        "was removed, experienced an error and disconnected itself, or set itself to",
        "read-only.",
        "",
        "Unfortunately, this means that we won't be able to complete the speed tests.",
        "",
        "Here's the error we got:",
        strerror(local_errno),
        NULL
    }, 1);
}

int probe_device_speeds(int fd) {
    char *buf, wr, rd;
    size_t ctr, bytes_left, cur;
    ssize_t ret;
    struct timeval start_time, cur_time;
    double secs, prev_secs;
    char rate[15], str[192];
    int local_errno;
    WINDOW *window;

    device_speeds.sequential_write_speed = 0;
    device_speeds.sequential_read_speed = 0;
    device_speeds.random_write_iops = 0;
    device_speeds.random_read_iops = 0;

    if(lock_lockfile()) {
        local_errno = errno;
        snprintf(str, sizeof(str), "probe_device_size(): lockf() returned an error: %s", strerror(local_errno));
        log_log(str);
        log_log("probe_device_size(): Skipping optimal write block size test.");

        message_window(stdscr, ERROR_TITLE, (char *[]) {
            "Unable to obtain a lock on the lockfile.  For now, we'll assume that the device",
            "is the size it says it is.  However, if the device really is fake flash, other",
            "tests are going to fail pretty quickly.",
            "",
            "The error we got while trying to get a lock on the lockfile was:",
            strerror(local_errno),
            NULL
        }, 1);

        return -1;
    }

    if(!(buf = valloc(device_stats.block_size < 4096 ? 4096 : device_stats.block_size))) {
        local_errno = errno;
        snprintf(str, sizeof(str), "probe_device_speeds(): valloc() returned an error: %s", strerror(local_errno));
        log_log(str);
        log_log("probe_device_speeds(): Aborting speed tests");

        message_window(stdscr, WARNING_TITLE, (char *[]) {
            "We couldn't allocate memory we need for the speed tests.  We won't be able to",
            "to run speed tests on this device as a result.",
            "",
            "The error we got was:",
            strerror(local_errno),
            NULL
        }, 1);

        unlock_lockfile();
        return -1;
    }

    log_log("probe_device_speeds(): Beginning speed tests");
    window = message_window(stdscr, NULL, (char *[]) { "Testing read/write speeds...", NULL }, 0);

    for(rd = 0; rd < 2; rd++) {
        for(wr = 0; wr < 2; wr++) {
            ctr = 0;
            assert(!gettimeofday(&start_time, NULL));

            if(!rd) {
                if(lseek(fd, 0, SEEK_SET) == -1) {
                    erase_and_delete_window(window);
                    lseek_error_during_speed_test();
                    free(buf);
                    return -1;
                }
            }

            secs = 0;
            prev_secs = 0;
            while(secs < 30) {
                if(wr) {
                    fill_buffer(buf, rd ? 4096 : device_stats.block_size);
                }

                bytes_left = rd ? 4096 : device_stats.block_size;
                while(bytes_left && secs < 30) {
                    handle_key_inputs(window);
                    if(rd) {
                        // Choose a random sector, aligned on a 4K boundary
                        cur = (((((size_t) get_random_number()) << 32) | get_random_number()) & 0x7FFFFFFFFFFFFFFF) %
                            (device_stats.num_sectors - (4096 / device_stats.sector_size)) & 0xFFFFFFFFFFFFFFF8;
                        if(lseek(fd, cur * device_stats.sector_size, SEEK_SET) == -1) {
                            erase_and_delete_window(window);
                            lseek_error_during_speed_test();
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
                        erase_and_delete_window(window);
                        io_error_during_speed_test(wr);
                        free(buf);
                        unlock_lockfile();
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
                                snprintf(str, sizeof(str), "%0.2f IOPS/s (%s)", ctr / secs, format_rate((ctr * 4096) / secs, rate, sizeof(rate)));
                                mvprintw(
                                    wr ? RANDOM_WRITE_SPEED_DISPLAY_Y : RANDOM_READ_SPEED_DISPLAY_Y,
                                    wr ? RANDOM_WRITE_SPEED_DISPLAY_X : RANDOM_READ_SPEED_DISPLAY_X, "%-28s", str);
                            } else {
                                snprintf(str, sizeof(str), "%s", format_rate(ctr / secs, rate, sizeof(rate)));
                                mvprintw(
                                    wr ? SEQUENTIAL_WRITE_SPEED_DISPLAY_Y : SEQUENTIAL_READ_SPEED_DISPLAY_Y,
                                    wr ? SEQUENTIAL_WRITE_SPEED_DISPLAY_X : SEQUENTIAL_READ_SPEED_DISPLAY_X, "%-28s", str);
                            }

                            refresh();
                            prev_secs = secs;
                        }
                    }
                }
            }

            if(rd && wr) {
                snprintf(str, sizeof(str), "probe_device_speeds(): Random write speed    : %0.2f IOPS/s (%s)", ctr / secs, format_rate((ctr * 4096) / secs,
                    rate, sizeof(rate)));
                log_log(str);

                device_speeds.random_write_iops = ctr / secs;
            } else if(rd && !wr) {
                snprintf(str, sizeof(str), "probe_device_speeds(): Random read speed     : %0.2f IOPS/s (%s)", ctr / secs, format_rate((ctr * 4096) / secs,
                    rate, sizeof(rate)));
                log_log(str);

                device_speeds.random_read_iops = ctr / secs;
            } else if(!rd && wr) {
                snprintf(str, sizeof(str), "probe_device_speeds(): Sequential write speed: %s", format_rate(ctr / secs, rate, sizeof(rate)));
                log_log(str);

                device_speeds.sequential_write_speed = ctr / secs;
                speed_qualifications_shown = 1;
                print_class_marking_qualifications();
            } else {
                snprintf(str, sizeof(str), "probe_device_speeds(): Sequential read speed : %s", format_rate(ctr / secs, rate, sizeof(rate)));
                log_log(str);

                device_speeds.sequential_read_speed = ctr / secs;
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
    log_log("probe_device_speeds(): Speed test results:");
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for Class 2 marking : %s", device_speeds.sequential_write_speed >= 2000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for Class 4 marking : %s", device_speeds.sequential_write_speed >= 4000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for Class 6 marking : %s", device_speeds.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for Class 10 marking: %s", device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(str);
    log_log("probe_device_speeds():");
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for U1 marking      : %s", device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for U3 marking      : %s", device_speeds.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(str);
    log_log("probe_device_speeds():");
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for V6 marking      : %s", device_speeds.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for V10 marking     : %s", device_speeds.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for V30 marking     : %s", device_speeds.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for V60 marking     : %s", device_speeds.sequential_write_speed >= 60000000 ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for V90 marking     : %s", device_speeds.sequential_write_speed >= 90000000 ? "Yes" : "No");
    log_log(str);
    log_log("probe_device_speeds():");
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for A1 marking      : %s",
        (device_speeds.sequential_write_speed >= 10485760 && device_speeds.random_read_iops >= 1500 && device_speeds.random_write_iops >= 500) ? "Yes" : "No");
    log_log(str);
    snprintf(str, sizeof(str), "probe_device_speeds():   Qualifies for A2 marking      : %s",
        (device_speeds.sequential_write_speed >= 10485760 && device_speeds.random_read_iops >= 4000 && device_speeds.random_write_iops >= 2000) ? "Yes" : "No");
    log_log(str);
    log_log("probe_device_speeds():");

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
        j = (get_random_number() & RAND_MAX) % (16 - i);
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
size_t get_slice_start(int slice_num) {
    return (device_stats.num_sectors / 16) * slice_num;
}

void print_device_summary(ssize_t fifty_percent_failure_round, ssize_t rounds_completed, int abort_reason) {
    char buf[256];
    char messages[7][384];
    char *out_messages[7];
    int i;

    switch(abort_reason) {
    case 1:
        strncpy(buf, "read error", sizeof(buf)); break;
    case 2:
        strncpy(buf, "write error", sizeof(buf)); break;
    case 3:
        strncpy(buf, "seek error", sizeof(buf)); break;
    case 4:
        strncpy(buf, "50% of sectors have failed", sizeof(buf)); break;
    case 5:
        strncpy(buf, "device went away", sizeof(buf)); break;
    default:
        strncpy(buf, "unknown", sizeof(buf)); break;
    }

    snprintf(messages[0], sizeof(messages[0]), "Reason for aborting test             : %s", buf);
    out_messages[0] = messages[0];
    snprintf(messages[1], sizeof(messages[1]), "Number of read/write cycles completed: %'lu", rounds_completed);
    out_messages[1] = messages[1];

    if(state_data.first_failure_round != -1) {
        snprintf(messages[2], sizeof(messages[2]), "Read/write cycles to first failure   : %'lu", state_data.first_failure_round);
        out_messages[2] = messages[2];
    } else {
        out_messages[2] = NULL;
    }

    if(state_data.ten_percent_failure_round != -1) {
        snprintf(messages[3], sizeof(messages[3]), "Read/write cycles to 10%% failure     : %'lu", state_data.ten_percent_failure_round);
        out_messages[3] = messages[3];
    } else {
        out_messages[3] = NULL;
    }

    if(state_data.twenty_five_percent_failure_round != -1) {
        snprintf(messages[4], sizeof(messages[4]), "Read/write cycles to 25%% failure     : %'lu", state_data.twenty_five_percent_failure_round);
        out_messages[4] = messages[4];
    } else {
        out_messages[4] = NULL;
    }

    if(fifty_percent_failure_round != -1) {
        snprintf(messages[5], sizeof(messages[5]), "Read/write cycles to 50%% failure     : %'lu", fifty_percent_failure_round);
        out_messages[5] = messages[5];
    } else {
        out_messages[5] = NULL;
    }

    out_messages[6] = NULL;

    log_log("Stress test complete");
    for(i = 0; out_messages[i]; i++) {
        snprintf(buf, sizeof(buf), "  %s", out_messages[i]);
        log_log(buf);
    }

    message_window(stdscr, "Test Complete", out_messages, 1);
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
    printf("       [-f | --lockfile filename] device-name | [-h | --help]]\n\n");
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
                forced_device = malloc(strlen(optarg) + 1);
                strcpy(forced_device, optarg);
                break;
            case 'e':
                program_options.force_sectors = strtoull(optarg, NULL, 10); break;
            case 'f':
                program_options.lock_file = malloc(strlen(optarg) + 1);
                strcpy(program_options.lock_file, optarg);
                break;
            case 'h':
                print_help(argv[0]);
                return -1;
            case 'i':
                program_options.stats_interval = atoi(optarg); break;
            case 'l':
                if(program_options.log_file) {
                    printf("Only one log file option may be specified on the command line.\n");
                    return -1;
                }

                assert(program_options.log_file = malloc(strlen(optarg) + 1));
                strcpy(program_options.log_file, optarg);
                break;
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

                assert(program_options.stats_file = malloc(strlen(optarg) + 1));
                strcpy(program_options.stats_file, optarg);
                break;
            case 't':
                if(program_options.state_file) {
                    printf("Only one state file option may be specified on the command line.\n");
                    return -1;
                }

                assert(program_options.state_file = malloc(strlen(optarg) + 1));
                strcpy(program_options.state_file, optarg);
        }
    }

    if(optind < argc) {
        for(c = optind; c < argc; c++) {
            if(program_options.device_name) {
                printf("Only one device may be specified on the command line.\n");
                return -1;
            }

            assert(program_options.device_name = malloc(strlen(argv[c]) + 1));
            strcpy(program_options.device_name, argv[c]);
        }
    }

    if(!program_options.device_name && !program_options.state_file) {
        print_help(argv[0]);
        return -1;
    }

    if(!program_options.lock_file) {
        program_options.lock_file = malloc(10);
        strcpy(program_options.lock_file, "mfst.lock");
    }

    return 0;
}

off_t retriable_lseek(int *fd, off_t position, size_t num_rounds_completed, int *device_was_disconnected) {
    int op_retry_count;
    int reset_retry_count;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    off_t ret;
    WINDOW *window;

    op_retry_count = 0;
    reset_retry_count = 0;
    *device_was_disconnected = 0;
    ret = lseek(*fd, position, SEEK_SET);

    while(((reset_retry_count < MAX_RESET_RETRIES) || (reset_retry_count == MAX_RESET_RETRIES && op_retry_count < MAX_OP_RETRIES)) && ret == -1) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(num_rounds_completed > 0) {
            if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
                *device_was_disconnected = 1;
                if(*fd != -1) {
                    close(*fd);
                }

                log_log("retriable_lseek(): Device disconnected.  Waiting for device to be reconnected...");
                window = message_window(stdscr, "Device Disconnected", (char *[]) {
                    "The device has been disconnected.  It may have done this on its own, or it may",
                    "have been manually removed (e.g., if someone pulled the device out of its USB",
                    "port).",
                    "",
                    "Don't worry -- just plug the device back in.  We'll verify that it's the same",
                    "device, then resume the stress test automatically.",
                    NULL
                }, 0);

                iret = wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name, &new_device_num, fd);

                if(iret == -1) {
                    log_log("retriable_lseek(): Failed to reconnect device!");
                } else {
                    log_log("retriable_lseek(): Device reconnected.");
                    if(program_options.device_name) {
                        free(program_options.device_name);
                    }

                    program_options.device_name = new_device_name;
                    device_stats.device_num = new_device_num;
                }

                erase_and_delete_window(window);
                redraw_screen();
            } else {
                if(op_retry_count < MAX_OP_RETRIES) {
                    log_log("retriable_lseek(): Re-attempting seek operation");
                    ret = lseek(*fd, position, SEEK_SET);
                    op_retry_count++;
                } else {
                    if(can_reset_device(device_stats.device_num)) {
                        log_log("retriable_lseek(): Device error.  Attempting to reset the device...");
                        window = message_window(stdscr, "Attempting to reset device", (char *[]) {
                            "The device has encountered an error.  We're attempting to reset the device to",
                            "see if that fixes the issue.  You shouldn't need to do anything -- but if this",
                            "message stays up for a while, it might indicate that the device has failed or",
                            "isn't handling the reset well.  In that case, you can try unplugging the device",
                            "and plugging it back in to get the device working again.",
                            NULL
                        }, 0);

                        *fd = reset_device(*fd);
                        *device_was_disconnected = 1;

                        if(*fd == -1) {
                            log_log("retriable_lseek(): Device reset failed!");
                            reset_retry_count = MAX_RESET_RETRIES;
                        } else {
                            log_log("retriable_lseek(): Device reset successfully.");
                            op_retry_count = 0;
                            reset_retry_count++;
                        }

                        erase_and_delete_window(window);
                        redraw_screen();
                    } else {
                        // Insta-fail
                        reset_retry_count = MAX_RESET_RETRIES;
                    }
                }
            }
        } else {
          return -1;
        }
    }

    return ret;
}

ssize_t retriable_read(int *fd, void *buf, size_t count, off_t position, size_t num_rounds_completed) {
    int op_retry_count;
    int reset_retry_count;
    int device_was_disconnected;
    int iret;
    ssize_t ret;
    WINDOW *window;
    char str[256];
    char *new_device_name;
    dev_t new_device_num;

    op_retry_count = 0;
    reset_retry_count = 0;

    ret = read(*fd, buf, count);

    if(ret == -1) {
        snprintf(str, sizeof(str), "retriable_read(): write error during sector %lu", position / device_stats.sector_size);
    }

    while(((reset_retry_count < MAX_RESET_RETRIES) || (reset_retry_count == MAX_RESET_RETRIES && op_retry_count < MAX_OP_RETRIES)) && ret == -1) {
        if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
            if(*fd != -1) {
                close(*fd);
            }

            log_log("retriable_read(): Device disconnected.  Waiting for device to be reconnected...");
            window = message_window(stdscr, "Device Disconnected", (char *[]) {
                "The device has been disconnected.  It may have done this on its own, or it may",
                "have been manually removed (e.g., if someone pulled the device out of its USB",
                "port).",
                "",
                "Don't worry -- just plug the device back in.  We'll verify that it's the same",
                "device, then resume the stress test automatically.",
                NULL
            }, 0);

            iret = wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name, &new_device_num, fd);

            if(iret != -1) {
                log_log("retriable_read(): Device reconnected.");

                if(program_options.device_name) {
                    free(program_options.device_name);
                }

                program_options.device_name = new_device_name;
                device_stats.device_num = new_device_num;

                if(retriable_lseek(fd, position, num_rounds_completed, &device_was_disconnected) == -1) {
                    log_log("retriable_read(): Failed to seek to previous position after re-opening device");
                    erase_and_delete_window(window);
                    redraw_screen();
                    return -1;
                }
            } else {
                log_log("retriable_read(): Failed to reconnect device!");
            }

            erase_and_delete_window(window);
            redraw_screen();
        } else {
            if(op_retry_count < MAX_OP_RETRIES) {
                log_log("retriable_read(): Re-attempting read operation");
                ret = read(*fd, buf, count);
                op_retry_count++;
            } else {
                if(can_reset_device(device_stats.device_num)) {
                    log_log("retriable_read(): Device error.  Attempting to reset the device...");
                    window = message_window(stdscr, "Attempting to reset device", (char *[]) {
                        "The device has encountered an error.  We're attempting to reset the device to",
                        "see if that fixes the issue.  You shouldn't need to do anything -- but if this",
                        "message stays up for a while, it might indicate that the device has failed or",
                        "isn't handling the reset well.  In that case, you can try unplugging the device",
                        "and plugging it back in to get the device working again.",
                        NULL
                    }, 0);

                    *fd = reset_device(*fd);

                    if(*fd == -1) {
                        log_log("retriable_read(): Device reset failed!");
                        reset_retry_count = MAX_RESET_RETRIES;
                    } else {
                        log_log("retriable_read(): Device reset successfully.");
                        op_retry_count = 0;
                        reset_retry_count++;

                        if(retriable_lseek(fd, position, num_rounds_completed, &device_was_disconnected) == -1) {
                            log_log("retriable_read(): Failed to seek to previous position after re-opening device");
                            erase_and_delete_window(window);
                            redraw_screen();
                            return -1;
                        }

                        ret = read(*fd, buf, count);
                    }

                    usleep(250000); // Give the device time to either reconnect and settle or for the device to fully disconnect

                    erase_and_delete_window(window);
                    redraw_screen();
                } else {
                    // Insta-fail
                    return -1;
                }
            }
        }
    }

    return ret;
}

ssize_t retriable_write(int *fd, void *buf, size_t count, off_t position, size_t num_rounds_completed, int *device_was_disconnected) {
    int op_retry_count;
    int reset_retry_count;
    int iret;
    ssize_t ret;
    WINDOW *window;
    char str[256];
    char *new_device_name;
    dev_t new_device_num;

    op_retry_count = 0;
    reset_retry_count = 0;
    *device_was_disconnected = 0;

    ret = write(*fd, buf, count);

    if(ret == -1) {
        snprintf(str, sizeof(str), "retriable_write(): write error during sector %lu", position / device_stats.sector_size);
        log_log(str);
    }

    while(((reset_retry_count < MAX_RESET_RETRIES) || (reset_retry_count == MAX_RESET_RETRIES && op_retry_count < MAX_OP_RETRIES)) && ret == -1) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(num_rounds_completed > 0) {
            if(did_device_disconnect(device_stats.device_num) || *fd == -1) {
                if(*fd != -1) {
                    close(*fd);
                }

                log_log("retriable_write(): Device disconnected.  Waiting for device to be reconnected...");
                window = message_window(stdscr, "Device Disconnected", (char *[]) {
                    "The device has been disconnected.  It may have done this on its own, or it may",
                    "have been manually removed (e.g., if someone pulled the device out of its USB",
                    "port).",
                    "",
                    "Don't worry -- just plug the device back in.  We'll verify that it's the same",
                    "device, then resume the stress test automatically.",
                    NULL
                }, 0);

                iret = wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name, &new_device_num, fd);

                if(iret != -1) {
                    log_log("retriable_write(): Device reconnected.");

                    if(program_options.device_name) {
                        free(program_options.device_name);
                    }

                    program_options.device_name = new_device_name;
                    device_stats.device_num = new_device_num;

                    if(retriable_lseek(fd, position, num_rounds_completed, device_was_disconnected) == -1) {
                        log_log("retriable_write(): Failed to seek to previous position after re-opening device");
                        erase_and_delete_window(window);
                        redraw_screen();
                        return -1;
                    }
                } else {
                    log_log("retriable_write(): Failed to re-open device!");
                }

                *device_was_disconnected = 1;

                erase_and_delete_window(window);
                redraw_screen();
            } else {
                if(op_retry_count < MAX_OP_RETRIES) {
                    log_log("retriable_write(): Re-attempting write operation");
                    ret = write(*fd, buf, count);
                    op_retry_count++;
                } else {
                    if(can_reset_device(device_stats.device_num)) {
                        log_log("retriable_write(): Device error.  Attempting to reset the device...");
                        window = message_window(stdscr, "Attempting to reset device", (char *[]) {
                            "The device has encountered an error.  We're attempting to reset the device to",
                            "see if that fixes the issue.  You shouldn't need to do anything -- but if this",
                            "message stays up for a while, it might indicate that the device has failed or",
                            "isn't handling the reset well.  In that case, you can try unplugging the device",
                            "and plugging it back in to get the device working again.",
                            NULL
                        }, 0);

                        *fd = reset_device(*fd);

                        if(*fd == -1) {
                            log_log("retriable_write(): Device reset failed!");
                            reset_retry_count = MAX_RESET_RETRIES;
                        } else {
                            log_log("retriable_write(): Device reset successfully.");
                            op_retry_count = 0;
                            reset_retry_count++;

                            if(retriable_lseek(fd, position, num_rounds_completed, device_was_disconnected) == -1) {
                                log_log("retriable_write(): Failed to seek to previous position after re-opening device");
                                erase_and_delete_window(window);
                                redraw_screen();
                                return -1;
                            }

                            ret = write(*fd, buf, count);
                        }

                        *device_was_disconnected = 1;

                        erase_and_delete_window(window);
                        redraw_screen();
                    } else {
                        // Insta-fail
                        return -1;
                    }
                }
            }
        } else {
            return -1;
        }
    }

    return ret;
}

void malloc_error(int errnum) {
    char str[256];

    snprintf(str, sizeof(str), "malloc() failed: %s", strerror(errnum));
    log_log(str);

    message_window(stdscr, ERROR_TITLE, (char *[]) {
        "Failed to allocate memory for one of the buffers we need to do the stress test.",
        "Unfortunately this means we have to abort the stress test.",
        "",
        "The error we got while trying to allocate memory was:",
        strerror(errnum),
        NULL
    }, 1);
}

void valloc_error(int errnum) {
    char str[256];

    snprintf(str, sizeof(str), "valloc() failed: %s", strerror(errnum));
    log_log(str);

    message_window(stdscr, ERROR_TITLE, (char *[]) {
        "Failed to allocate memory for one of the buffers we need to do the stress test.",
        "Unfortunately this means we have to abort the stress test.",
        "",
        "The error we got while trying to allocate memory was:",
        strerror(errnum),
        NULL
    }, 1);
}

void reset_sector_map() {
    for(int j = 0; j < device_stats.num_sectors; j++) {
        sector_display.sector_map[j] &= 0x01;
    }

    // dispatch_sector_map_info_param_event(EVENT_INFO_SECTOR_MAP_RESET, device_stats.num_sectors, device_stats.sector_size, 1, sector_display.sector_map);
}

int main(int argc, char **argv) {
    int fd, cur_block_size, local_errno, restart_slice, state_file_status;
    struct stat fs;
    size_t bytes_left_to_write, ret, cur_sector, middle_of_device;
    unsigned int sectors_per_block;
    unsigned short max_sectors_per_request;
    char *buf, *compare_buf, *zero_buf, *ff_buf, *new_device_name;
    struct timeval speed_start_time;
    struct timeval rng_init_time;
    size_t num_bad_sectors, sectors_read, cur_sectors_per_block, last_sector;
    size_t num_bad_sectors_this_round, num_good_sectors_this_round;
    size_t cur_slice, i, j;
    int *read_order;
    int op_retry_count; // How many times have we tried the same operation without success?
    int reset_retry_count; // How many times have we tried to reset the device?
    int device_was_disconnected;
    int iret;
    char str[192], tmp[16];
    struct timeval stats_cur_time;
    WINDOW *window;
    dev_t new_device_num;

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
    file_handles.lockfile_fd = -1;
    program_options.lock_file = NULL;
    program_options.state_file = NULL;
    forced_device = NULL;
    is_writing = -1;

    void cleanup() {
        log_log("Program ending.");
        if(fd != -1) {
            close(fd);
        }

        if(file_handles.lockfile_fd != -1) {
            close(file_handles.lockfile_fd);
        }

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
        log_log("stdout isn't a tty -- turning off curses mode");
        program_options.no_curses = 1;
    }

    // Initialize ncurses
    if(!program_options.no_curses) {
        if(screen_setup()) {
            log_log("Terminal is too small -- turning off curses mode");
            program_options.no_curses = 1;
        } else {
            redraw_screen();
        }
    }

    if(state_file_status == LOAD_STATE_LOAD_ERROR) {
        log_log("WARNING: There was a problem loading the state file.  The existing state file");
        log_log("will be ignored (and eventually overwritten).  If you don't want this to happen,");
        log_log("you have 15 seconds to hit Ctrl+C to abort the program.");
        
        window = message_window(stdscr, WARNING_TITLE, (char *[]) {
            "There was a problem loading the state file.  If you want to continue and just",
            "ignore the existing state file, then you can ignore this message.  Otherwise,",
            "you have 15 seconds to hit Ctrl+C.",
            NULL
        }, 0);

        if(window) {
            for(j = 0; j < 15; j++) {
                handle_key_inputs(window);
                sleep(1);
                mvwprintw(window, 3, 11, "%-2lu", 14 - j);
                wrefresh(window);
            }
        } else {
            sleep(15);
        }

        erase_and_delete_window(window);

        state_file_status = LOAD_STATE_FILE_DOES_NOT_EXIST;
    }

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        if(!program_options.dont_show_warning_message) {
            log_log("WARNING: This program is DESTRUCTIVE.  It is designed to stress test storage");
            log_log("devices (particularly flash media) to the point of failure.  If you let this");
            log_log("program run for long enough, it WILL completely destroy the device and render it"),
            log_log("completely unusable.  Do not use it on any storage devices that you care about.");
            log_log("");
            snprintf(str, sizeof(str), "Any data on %s is going to be overwritten -- multiple times.  If you're", program_options.device_name);
            log_log(str);
            log_log("not OK with this, you have 15 seconds to hit Ctrl+C before we start doing anything.");
            
            window = message_window(stdscr, WARNING_TITLE, (char *[]) {
                "This program is DESTRUCTIVE.  It is designed to stress test storage devices",
                "(particularly flash media) to the point of failure.  If you let this program run",
                "for long enough, it WILL completely destroy the device and render it completely",
                "unusable.  Do not use it on any storage devices that you care about.",
                "",
                str,
                "not OK with this, you have 15 seconds to hit Ctrl+C.",
                NULL
            }, 0);

            if(window) {
                for(j = 0; j < 15; j++) {
                    handle_key_inputs(window);
                    sleep(1);
                    mvwprintw(window, 7, 29, "%-2lu", 14 - j);
                    wrefresh(window);
                }
            } else {
                sleep(15);
            }

            erase_and_delete_window(window);
        }
    }

    if(program_options.log_file) {
        file_handles.log_file = fopen(program_options.log_file, "a");
        if(!file_handles.log_file) {
            if(!program_options.no_curses) {
                local_errno = errno;
                snprintf(str, sizeof(str), "Unable to open log file %s:", program_options.log_file);
                message_window(stdscr, ERROR_TITLE, (char *[]) {
                    str,
                    strerror(local_errno),
                    NULL
                }, 1);
            } else {
                printf("Got the following error while trying to open log file %s: %s\n", program_options.log_file, strerror(errno));
            }

            cleanup();
            return -1;
        }
    }

    // dispatch_null_param_event(EVENT_PROGRAM_STARTED, 0);
    log_log("Program version " VERSION " started.");

    if(state_file_status == LOAD_STATE_SUCCESS) {
        snprintf(str, sizeof(str), "Resuming from state file %s", program_options.state_file);
        log_log(str);
    }

    if((file_handles.lockfile_fd = open(program_options.lock_file, O_WRONLY | O_CREAT)) == -1) {
        local_errno = errno;
        snprintf(str, sizeof(str), "Unable to open lock file %s: %s", program_options.lock_file, strerror(local_errno));
        log_log(str);

        snprintf(str, sizeof(str), "Unable to open lock file %s:", program_options.lock_file);
        message_window(stdscr, ERROR_TITLE, (char *[]) {
            str,
            strerror(local_errno),
            NULL
        }, 1);

        cleanup();
        return -1;
    }

    if(program_options.stats_file) {
        file_handles.stats_file = fopen(program_options.stats_file, "a");
        if(!file_handles.stats_file) {
            local_errno = errno;
            snprintf(str, sizeof(str), "Unable to open stats file %s: %s", program_options.stats_file, strerror(local_errno));
            log_log(str);

            snprintf(str, sizeof(str), "Unable to open stats file %s:", program_options.stats_file);
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                str,
                strerror(local_errno),
                NULL
            }, 1);

            cleanup();
            return -1;
        }

        snprintf(str, sizeof(str), "Logging stats to %s", program_options.stats_file);
        log_log(str);

        // Write the CSV headers out to the file
        fprintf(file_handles.stats_file,
            "Date/Time,Rounds Completed,Bytes Written,Total Bytes Written,Write Rate (bytes/sec),Bytes Read,Total Bytes Read,Read Rate (bytes/sec),Bad Sectors,Total Bad Sesctors,Bad Sector Rate (counts/min)\n");
        fflush(file_handles.stats_file);
    }

    // Does the system have a working gettimeofday?
    if(gettimeofday(&speed_start_time, NULL) == -1) {
        local_errno = errno;
        snprintf(str, sizeof(str), "Got the following error while calling gettimeofday(): %s", strerror(local_errno));
        log_log(str);
        log_log("Unable to test -- your system doesn't have a working gettimeofday() call.");
        message_window(stdscr, ERROR_TITLE, (char *[]) {
            "We won't be able to test this device because your system doesn't have a working",
            "gettimeofday() call.  So many things in this program depend on this that it",
            "would take a lot of work to make this program work without it, and I'm lazy.",
            "",
            "This is the error we got while trying to call gettimeofday():",
            strerror(local_errno),
            NULL
        }, 1);

        cleanup();
        return -1;
    }

    if(state_file_status == LOAD_STATE_SUCCESS && !forced_device) {
        log_log("Attempting to locate device described in state file");
        window = message_window(stdscr, NULL, (char *[]) {
            "Finding device described in state file...",
            NULL
        }, 0);

        iret = find_device(program_options.device_name, 0, device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &new_device_name,
                           &new_device_num, &fd);

        erase_and_delete_window(window);

        if(iret == -1) {
            log_log("An error occurred while trying to locate the device described in the state file.  Make sure you're running this program with sudo.");
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                "An error occurred while trying to locate the device described in the state file.",
                "(Make sure you're running this program with sudo.)",
                NULL
            }, 1);

            cleanup();
            return -1;
        } else if(iret > 1) {
            log_log("Multiple devices found that match the data in the state file.  Please specify which device you want to test on the command line.");
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                "There are multiple devices that match the data in the state file.  Please",
                "specify which device you want to test on the command line.",
                NULL
            }, 1);
            
            cleanup();
            return -1;
        } else if(iret == 0) {
            if(program_options.device_name) {
                log_log("The device specified on the command line does not match the device described in the state file.");
                log_log("Please remove the device from the command line or specify a different device.");
                message_window(stdscr, ERROR_TITLE, (char *[]) {
                    "The device you specified on the command line does not match the device described",
                    "in the state file.  If you run this program again without the device name, we'll",
                    "figure out which device to use automatically.  Otherwise, provide a different",
                    "device on the command line.",
                    NULL
                }, 1);

                cleanup();
                return -1;
            } else {
                log_log("No devices could be found that match the data in the state file.  Attach one now...");
                window = message_window(stdscr, "No devices found", (char *[]) {
                    "No devices could be found that match the data in the state file.  If you haven't",
                    "plugged the device in yet, go ahead and do so now.  Otherwisse, you can hit",
                    "Ctrl+C now to abort the program.",
                    NULL
                }, 0);

                iret = wait_for_device_reconnect(device_stats.reported_size_bytes, device_stats.detected_size_bytes, bod_buffer, mod_buffer, BOD_MOD_BUFFER_SIZE, &program_options.device_name,
                                                 &device_stats.device_num, &fd);

                if(iret == -1) {
                    erase_and_delete_window(window);
                    log_log("An error occurred while waiting for the device to be reconnected.");
                    message_window(stdscr, ERROR_TITLE, (char *[]) {
                        "An error occurred while waiting for you to reconnect the device.",
                        NULL
                    }, 1);

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
            local_errno = errno;
            snprintf(str, sizeof(str), "Got the following error while calling fstat() on %s: %s\n", program_options.device_name, strerror(local_errno));
            log_log(str);
            log_log("Unable to test -- unable to pull stats on the device.");

            snprintf(str, sizeof(str), "We won't be able to test %s because we weren't able to pull stats", program_options.device_name);
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                str,
                "it.  The device may have been removed, or you may not have permissions to open",
                "it.  (Make sure you're running this program with sudo.)",
                "",
                "This is the error we got while trying to call fstat():",
                strerror(local_errno),
                NULL
            }, 1);

            cleanup();
            return -1;
        }
    } else {
        if(forced_device) {
            if(state_file_status != LOAD_STATE_SUCCESS) {
                log_log("Not resuming from a state file -- ignoring --force-device parameter");
            } else {
                log_log("--force_device specified on command line, resuming from specified device");

                if(program_options.device_name) {
                    free(program_options.device_name);
                }

                program_options.device_name = forced_device;
                forced_device = NULL;
            }
        }

        if(stat(program_options.device_name, &fs)) {
            local_errno = errno;
            snprintf(str, sizeof(str), "Got the following error while calling stat() on %s: %s\n", program_options.device_name, strerror(local_errno));
            log_log(str);
            log_log("Unable to test -- unable to pull stats on the device.");
            snprintf(str, sizeof(str), "%s.  The device may have been removed, or you may not have permissions to", program_options.device_name);
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                "We won't be able to test this device because we were unable to pull stats on",
                str,
                "open it.  (Make sure you're running this program with sudo.)",
                "",
                "This is the error we got while trying to call stat():",
                strerror(local_errno),
                NULL
            }, 1);

            cleanup();
            return -1;
        }

        if(!S_ISBLK(fs.st_mode)) {
            snprintf(str, sizeof(str), "Unable to test -- %s is not a block device", program_options.device_name);
            log_log(str);
            snprintf(str, sizeof(str), "We won't be able to test with %s because it isn't a block device.  You must", program_options.device_name);
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                str,
                "provide a block device to test with.",
                NULL
            }, 1);

            cleanup();
            return -1;
        }

        if((fd = open(program_options.device_name, O_DIRECT | O_SYNC | O_LARGEFILE | O_RDWR)) == -1) {
            local_errno = errno;
            snprintf(str, sizeof(str), "Failed to open %s: %s", program_options.device_name, strerror(local_errno));
            log_log(str);
            log_log("Unable to test -- couldn't open device.  Make sure you run this program as sudo.");

            snprintf(str, sizeof(str), "We won't be able to test %s because we couldn't open the device.", program_options.device_name);
            message_window(stdscr, ERROR_TITLE, (char *[]) {
                str,
                "The device might have gone away, or you might not have permissions to open it.",
                "(Make sure you run this program as sudo.)",
                "",
                "Here's the error we got while trying to open the device:",
                strerror(local_errno),
                NULL
            }, 1);

            cleanup();
            return -1;
        }
    }

    if(ioctl(fd, BLKGETSIZE64, &device_stats.reported_size_bytes) || ioctl(fd, BLKSSZGET, &device_stats.sector_size) ||
        ioctl(fd, BLKSECTGET, &max_sectors_per_request) || ioctl(fd, BLKPBSZGET, &device_stats.physical_sector_size)) {
        local_errno = errno;
        snprintf(str, sizeof(str), "Got the following error while trying to call ioctl() on %s: %s", program_options.device_name, strerror(local_errno));
        log_log(str);

        snprintf(str, sizeof(str), "We won't be able to test %s because we couldn't pull stats on the device.", program_options.device_name);
        message_window(stdscr, ERROR_TITLE, (char *[]) {
            str,
            "",
            "Here's the error we got while trying to open the device:",
            strerror(local_errno),
            NULL
        }, 1);

        cleanup();
        return -1;
    }

    device_stats.device_num = fs.st_rdev;

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        device_stats.num_sectors = device_stats.reported_size_bytes / device_stats.sector_size;
    }

    device_stats.preferred_block_size = fs.st_blksize;
    device_stats.max_request_size = device_stats.sector_size * max_sectors_per_request;

    log_log("Device info reported by kernel:");
    snprintf(str, sizeof(str), "  Reported size            : %'lu bytes", device_stats.reported_size_bytes);
    log_log(str);
    snprintf(str, sizeof(str), "  Sector size (logical)    : %'u bytes", device_stats.sector_size);
    log_log(str);
    snprintf(str, sizeof(str), "  Sector size (physical)   : %'u bytes", device_stats.physical_sector_size);
    log_log(str);
    snprintf(str, sizeof(str), "  Total sectors (derived)  : %'lu", device_stats.num_sectors);
    log_log(str);
    snprintf(str, sizeof(str), "  Preferred block size     : %'lu bytes", fs.st_blksize);
    log_log(str);
    snprintf(str, sizeof(str), "  Max sectors per request  : %'hu", max_sectors_per_request);
    log_log(str);
    mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_stats.reported_size_bytes);
    refresh();

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        profile_random_number_generator();

        if(program_options.probe_for_optimal_block_size) {
            wait_for_file_lock(NULL);

            if((device_stats.block_size = probe_for_optimal_block_size(fd)) <= 0) {
                device_stats.block_size = device_stats.sector_size * max_sectors_per_request;
                snprintf(str, sizeof(str), "Unable to probe for optimal block size.  Falling back to derived block size (%'d bytes).", device_stats.block_size);
                log_log(str);
            }
        } else {
            device_stats.block_size = device_stats.sector_size * max_sectors_per_request;
        }

        sectors_per_block = device_stats.block_size / device_stats.sector_size;

        wait_for_file_lock(NULL);

        if(program_options.force_sectors) {
            device_stats.num_sectors = program_options.force_sectors;
            device_stats.detected_size_bytes = program_options.force_sectors * device_stats.sector_size;

            snprintf(str, sizeof(str), "Assuming that the device is %'lu bytes long (as specified on the command line).", device_stats.detected_size_bytes);
            log_log(str);

            if(device_stats.detected_size_bytes == device_stats.reported_size_bytes) {
                device_stats.is_fake_flash = FAKE_FLASH_NO;
            } else {
                device_stats.is_fake_flash = FAKE_FLASH_YES;
            }

            middle_of_device = device_stats.detected_size_bytes / 2;

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
            snprintf(str, sizeof(str), "Assuming that the kernel-reported device size (%'lu bytes) is correct.", device_stats.reported_size_bytes);
            log_log(str);

            middle_of_device = device_stats.reported_size_bytes / 2;

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

            middle_of_device = device_stats.detected_size_bytes / 2;

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
        middle_of_device = device_stats.detected_size_bytes / 2;
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
    current_seed = initial_seed = rng_init_time.tv_sec + rng_init_time.tv_usec;
    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        state_data.first_failure_round = state_data.ten_percent_failure_round = state_data.twenty_five_percent_failure_round = -1;
    }

    init_random_number_generator(initial_seed);

    buf = (char *) valloc(device_stats.block_size);
    if(!buf) {
        valloc_error(errno);
        cleanup();
        return -1;
    }

    compare_buf = (char *) valloc(device_stats.block_size);
    if(!compare_buf) {
        valloc_error(errno);
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
        malloc_error(errno);
        cleanup();
        return -1;
    }

    memset(zero_buf, 0, device_stats.sector_size);

    ff_buf = (char *) malloc(device_stats.sector_size);
    if(!ff_buf) {
        malloc_error(errno);
        cleanup();
        return -1;
    }

    memset(ff_buf, 0xff, device_stats.sector_size);

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        if(!(sector_display.sector_map = (char *) malloc(device_stats.num_sectors))) {
            malloc_error(errno);
            cleanup();
            return -1;
        }

        // Initialize the sector map
        memset(sector_display.sector_map, 0, device_stats.num_sectors);
        device_stats.num_bad_sectors = 0;
    }

    // Start filling up the device
    device_stats.bytes_since_last_status_update = 0;
    memset(&stress_test_stats, 0, sizeof(stress_test_stats));

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        log_log("Beginning stress test");
        num_rounds = state_data.bytes_written = state_data.bytes_read = 0;
    } else {
        // Count up the number of bad sectors and update device_stats.num_bad_sectors
        for(j = 0; j < device_stats.num_sectors; j++) {
            if(sector_display.sector_map[j] & 0x01) {
                device_stats.num_bad_sectors++;
            }
        }

        stress_test_stats.previous_bytes_written = state_data.bytes_written;
        stress_test_stats.previous_bytes_read = state_data.bytes_read;
        stress_test_stats.previous_bad_sectors = device_stats.num_bad_sectors;

        snprintf(str, sizeof(str), "Resuming stress test from round %'lu", num_rounds + 1);
        log_log(str);
    }

    assert(!gettimeofday(&stress_test_stats.previous_update_time, NULL));
    stats_cur_time = stress_test_stats.previous_update_time;

    for(num_bad_sectors = 0, num_bad_sectors_this_round = 0, num_good_sectors_this_round = 0; device_stats.num_bad_sectors < (device_stats.num_sectors / 2); num_rounds++, num_bad_sectors = 0, num_bad_sectors_this_round = 0, num_good_sectors_this_round = 0) {
        if(num_rounds > 0) {
            if(save_state()) {
                log_log("Error creating save state, disabling save stating");
                message_window(stdscr, WARNING_TITLE, (char *[]) {
                    "An error occurred while trying to save the program state.  Save stating has been",
                    "disabled.",
                    NULL
                }, 1);

                free(program_options.state_file);
                program_options.state_file = NULL;
            }
        }

        is_writing = 1;
        if(!program_options.no_curses) {
            j = snprintf(str, sizeof(str), " Round %'lu ", num_rounds + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), str);
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        }

        // Reset the sector map.
        for(j = 0; j < device_stats.num_sectors; j++) {
            sector_display.sector_map[j] &= 0x01;
        }

        redraw_sector_map();
        refresh();

        read_order = random_list();

        for(cur_slice = 0, restart_slice = 0; cur_slice < 16; cur_slice++, restart_slice = 0) {
            srandom_r(initial_seed + read_order[cur_slice] + (num_rounds * 16), &random_state);

            if(retriable_lseek(&fd, get_slice_start(read_order[cur_slice]) * device_stats.sector_size, num_rounds, &device_was_disconnected) == -1) {
                print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds,
                    ABORT_REASON_SEEK_ERROR);

                cleanup();
                return 0;
            }

            if(read_order[cur_slice] == 15) {
                last_sector = device_stats.num_sectors;
            } else {
                last_sector = get_slice_start(read_order[cur_slice] + 1);
            }

            for(cur_sector = get_slice_start(read_order[cur_slice]); cur_sector < last_sector; cur_sector += cur_sectors_per_block) {

                // Use bytes_left_to_write to hold the bytes left to read
                if((cur_sector + sectors_per_block) > last_sector) {
                    cur_sectors_per_block = last_sector - cur_sector;
                    cur_block_size = cur_sectors_per_block * device_stats.sector_size;
                } else {
                    cur_block_size = device_stats.block_size;
                    cur_sectors_per_block = sectors_per_block;
                }

                fill_buffer(buf, cur_block_size);
                bytes_left_to_write = cur_block_size;

                while(bytes_left_to_write) {
                    handle_key_inputs(NULL);
                    wait_for_file_lock(NULL);

                    if((ret = retriable_write(&fd, buf + (cur_block_size - bytes_left_to_write), bytes_left_to_write, lseek(fd, 0, SEEK_CUR), num_rounds, &device_was_disconnected)) == -1) {
                        if(fd == -1) {
                            print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds,
                                ABORT_REASON_WRITE_ERROR);

                            cleanup();
                            return 0;
                        } else {
                            // Mark this sector bad and skip over it
                            if(!is_sector_bad(cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size))) {
                                snprintf(str, sizeof(str), "Write error on sector %lu; marking sector bad", cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size));
                                log_log(str);
                            }

                            mark_sector_bad(cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size));

                            if(device_was_disconnected) {
                              restart_slice = 1;
                              break;
                            }

                            bytes_left_to_write -= device_stats.sector_size;

                            if((retriable_lseek(&fd, (cur_sector * device_stats.sector_size) + (cur_block_size - bytes_left_to_write), num_rounds, &device_was_disconnected)) == -1) {
                                // Give up if we can't seek
                                print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds, ABORT_REASON_WRITE_ERROR);
                                cleanup();
                                return 0;
                            }

                            continue;
                        }
                    }

                    if(device_was_disconnected) {
                        restart_slice = 1;
                        break;
                    }

                    // Do we need to update the BOD buffer?
                    if(((cur_sector * device_stats.sector_size) + (cur_block_size - bytes_left_to_write)) < BOD_MOD_BUFFER_SIZE) {
                        memcpy(
                            bod_buffer + (cur_sector * device_stats.sector_size) + (cur_block_size - bytes_left_to_write),
                            buf + (cur_block_size - bytes_left_to_write),
                            ((cur_sector * device_stats.sector_size) + ret) > BOD_MOD_BUFFER_SIZE ?
                            BOD_MOD_BUFFER_SIZE - (cur_sector * device_stats.sector_size) : ret
                        );

                        // If we updated the BOD buffer, we also need to
                        // savestate.
                        if(save_state()) {
                            log_log("Error creating save state, disabling save stating");
                            message_window(stdscr, WARNING_TITLE, (char *[]) {
                                "An error occurred while trying to save the program state.  Save stating has been"
                                "disabled.",
                                NULL
                            }, 1);

                            free(program_options.state_file);
                            program_options.state_file = NULL;
                        }

                    }

                    // Do we need to update the MOD buffer?
                    if(((cur_sector * device_stats.sector_size) >= middle_of_device) && (((cur_sector * device_stats.sector_size) + 
                        (cur_block_size - bytes_left_to_write)) < (middle_of_device + BOD_MOD_BUFFER_SIZE))) {
                        memcpy(
                            mod_buffer + ((cur_sector * device_stats.sector_size) + (cur_block_size - bytes_left_to_write) - middle_of_device),
                            buf + (cur_block_size - bytes_left_to_write),
                            ((cur_sector * device_stats.sector_size) + ret) > (middle_of_device + BOD_MOD_BUFFER_SIZE) ?
                            BOD_MOD_BUFFER_SIZE - ((cur_sector * device_stats.sector_size) - middle_of_device) : ret
                        );

                        // If we updated the MOD buffer, we also need to
                        // savestate.
                        if(save_state()) {
                            log_log("Error creating save state, disabling save stating");
                            message_window(stdscr, WARNING_TITLE, (char *[]) {
                                "An error occurred while trying to save the program state.  Save stating has been"
                                "disabled.",
                                NULL
                            }, 1);

                            free(program_options.state_file);
                            program_options.state_file = NULL;
                        }
                    }

                    bytes_left_to_write -= ret;
                    device_stats.bytes_since_last_status_update += ret;
                    state_data.bytes_written += ret;

                    print_status_update(cur_sector, num_rounds);

                }

                if(restart_slice) {
                    break;
                }

                mark_sectors_written(cur_sector, cur_sector + cur_sectors_per_block);
                refresh();

                assert(!gettimeofday(&stats_cur_time, NULL));
                if(timediff(stress_test_stats.previous_update_time, stats_cur_time) >= (program_options.stats_interval * 1000000)) {
                    stats_log(num_rounds, device_stats.num_bad_sectors);
                }

            }

            if(restart_slice) {
                // Unmark the sectors we've written in this slice so far
                log_log("Device disconnect was detected during this slice -- restarting slice");
                for(j = get_slice_start(read_order[cur_slice]); j < last_sector; j++) {
                    sector_display.sector_map[j] &= 0x01;
                }

                cur_slice--;
                redraw_sector_map();
                refresh();
            }
        }

        free(read_order);

        read_order = random_list();
        device_stats.bytes_since_last_status_update = 0;
        sectors_read = 0;
        is_writing = 0;
        if(!program_options.no_curses) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        for(cur_slice = 0; cur_slice < 16; cur_slice++) {
            srandom_r(initial_seed + read_order[cur_slice] + (num_rounds * 16), &random_state);

            if(retriable_lseek(&fd, get_slice_start(read_order[cur_slice]) * device_stats.sector_size, num_rounds, &device_was_disconnected) == -1) {
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

                // Use bytes_left_to_write to hold the bytes left to read
                if((cur_sector + sectors_per_block) > last_sector) {
                    cur_sectors_per_block = last_sector - cur_sector;
                    cur_block_size = cur_sectors_per_block * device_stats.sector_size;
                } else {
                    cur_block_size = device_stats.block_size;
                    cur_sectors_per_block = sectors_per_block;
                }

                // Regenerate the data we originally wrote to the device.
                fill_buffer(buf, cur_block_size);
                bytes_left_to_write = cur_block_size;

                while(bytes_left_to_write) {
                    handle_key_inputs(NULL);
                    wait_for_file_lock(NULL);

                    if((ret = retriable_read(&fd, compare_buf + (cur_block_size - bytes_left_to_write), bytes_left_to_write, lseek(fd, 0, SEEK_CUR), num_rounds)) == -1) {
                        if(fd == -1) {
                            print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds,
                                ABORT_REASON_READ_ERROR);

                            cleanup();
                            return 0;
                        } else {
                            // Mark this sector as bad and skip over it
                            if(!is_sector_bad(cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size))) {
                                snprintf(str, sizeof(str), "Read error on sector %lu; marking sector bad", cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size));
                                log_log(str);
                            }

                            mark_sector_bad(cur_sector + ((cur_block_size - bytes_left_to_write) / device_stats.sector_size));
                            bytes_left_to_write -= device_stats.sector_size;

                            if((retriable_lseek(&fd, (cur_sector * device_stats.sector_size) + (cur_block_size - bytes_left_to_write), num_rounds, &device_was_disconnected)) == -1) {
                                // Give up if we can't seek
                                print_device_summary(device_stats.num_bad_sectors < (device_stats.num_sectors / 2) ? -1 : num_rounds, num_rounds, ABORT_REASON_WRITE_ERROR);
                                cleanup();
                                return 0;
                            }

                            continue;
                        }
                    }

                    bytes_left_to_write -= ret;
                    device_stats.bytes_since_last_status_update += ret;
                    sectors_read += ret / device_stats.sector_size;

                    print_status_update(sectors_read, num_rounds);
                }

                mark_sectors_read(cur_sector, cur_sector + cur_sectors_per_block);
                state_data.bytes_read += cur_block_size;

                // Compare
                for(j = 0; j < cur_block_size; j += device_stats.sector_size) {
                    handle_key_inputs(NULL);
                    if(memcmp(buf + j, compare_buf + j, device_stats.sector_size)) {
                        num_bad_sectors_this_round++;
                        if(!is_sector_bad(cur_sector + (j / device_stats.sector_size))) {
                            if(!memcmp(compare_buf + j, zero_buf, device_stats.sector_size)) {
                                snprintf(str, sizeof(str), "Data verification failure in sector %lu (sector read as all 0x00's); marking sector bad", cur_sector + (j / device_stats.sector_size));
                                log_log(str);
                            } else if(!memcmp(compare_buf + j, ff_buf, device_stats.sector_size)) {
                                snprintf(str, sizeof(str), "Data verification failure in sector %lu (sector read as all 0xff's); marking sector bad", cur_sector + (j / device_stats.sector_size));
                                log_log(str);
                            } else {
                                snprintf(str, sizeof(str), "Data verification failure in sector %lu; marking sector bad", cur_sector + (j / device_stats.sector_size));
                                log_log(str);
                                log_log("Expected data was:");

                                for(i = 0; i < device_stats.sector_size; i++) {
                                    if(!(i % 16)) {
                                        snprintf(str, sizeof(str), "    %016lx:", (cur_sector * device_stats.sector_size) + j + i);
                                    }

                                    snprintf(tmp, sizeof(tmp), " %02x%s", buf[j + i] & 0xff, (i % 16) == 7 ? "    " : "");
                                    strcat(str, tmp);

                                    if((i % 16) == 15 || i == (device_stats.sector_size - 1)) {
                                        log_log(str);
                                    }
                                }

                                log_log("");
                                log_log("Actual data was:");

                                for(i = 0; i < device_stats.sector_size; i++) {
                                    if(!(i % 16)) {
                                        snprintf(str, sizeof(str), "    %016lx:", (cur_sector * device_stats.sector_size) + j + i);
                                    }

                                    snprintf(tmp, sizeof(tmp), " %02x%s", compare_buf[j + i] & 0xff, (i % 16) == 7 ? "    " : "");
                                    strcat(str, tmp);

                                    if((i % 16) == 15 || i == (device_stats.sector_size - 1)) {
                                        log_log(str);
                                    }
                                }

                                log_log("");

                                num_bad_sectors++;
                            }
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
            snprintf(str, sizeof(str), "Round %'lu complete, no bad sectors detected", num_rounds + 1);
            log_log(str);
        } else /* if(!num_bad_sectors && device_stats.num_bad_sectors) */ {
            snprintf(str, sizeof(str), "Round %'lu complete; round stats:", num_rounds + 1);
            log_log(str);

            snprintf(str, sizeof(str), "  Sectors that failed verification this round: %'lu", num_bad_sectors_this_round);
            log_log(str);

            snprintf(str, sizeof(str), "  New bad sectors this round: %'lu", num_bad_sectors);
            log_log(str);

            snprintf(str, sizeof(str), "  Sectors marked bad in a previous round that passed verification this round: %'lu", num_good_sectors_this_round);
            log_log(str);

            snprintf(str, sizeof(str), "  Total bad sectors: %'lu (%0.2f%% of total)", device_stats.num_bad_sectors, (((double) device_stats.num_bad_sectors) / ((double) device_stats.num_sectors)) * 100);
            log_log(str);
        } /* else {
            snprintf(str, sizeof(str), "Round %'lu complete, %'lu new bad sectors found this round; %'lu bad sectors discovered total (%0.2f%% of total)",
                num_rounds + 1, num_bad_sectors, device_stats.num_bad_sectors, (((double) device_stats.num_bad_sectors) / ((double) device_stats.num_sectors)) * 100);
            log_log(str);

            if(state_data.first_failure_round == -1) {
                state_data.first_failure_round = num_rounds;
            }

            if(((((double)device_stats.num_bad_sectors) / ((double)device_stats.num_sectors)) > 0.1) && (state_data.ten_percent_failure_round == -1)) {
                state_data.ten_percent_failure_round = num_rounds;
            }

            if(((((double)device_stats.num_bad_sectors) / ((double)device_stats.num_sectors)) > 0.25) &&
                (state_data.twenty_five_percent_failure_round == -1)) {
                state_data.twenty_five_percent_failure_round = num_rounds;
            }
            } */
    }

    print_device_summary(num_rounds - 1, num_rounds, ABORT_REASON_FIFTY_PERCENT_FAILURE);

    cleanup();
    return 0;
}

