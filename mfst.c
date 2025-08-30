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
#include "device_speed_test.h"
#include "device_testing_context.h"
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

char speed_qualifications_shown;
char ncurses_active;
char *forced_device;

sector_display_type sector_display;
program_options_type program_options;

volatile main_thread_status_type main_thread_status;

volatile int log_log_lock = 0;

static struct timeval stats_cur_time;

// Scratch buffer for messages; we're allocating it statically so that we can
// still log messages in case of memory shortages
static char msg_buffer[512];

void log_log(device_testing_context_type *device_testing_context, const char *funcname, int severity, int msg, ...) {
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

    if(device_testing_context) {
        if(device_testing_context->log_file_handle) {
            fprintf(device_testing_context->log_file_handle, "[%s] [%s] ", t, severity == 0 ? "INFO" : (severity == 1 ? "ERROR" : (severity == 2 ? "WARNING" : "DEBUG")));

            if(funcname) {
                fprintf(device_testing_context->log_file_handle, "%s(): ", funcname);
            }

            vfprintf(device_testing_context->log_file_handle, log_file_messages[msg], ap);
            fprintf(device_testing_context->log_file_handle, "\n");
            fflush(device_testing_context->log_file_handle);
        }
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
 * * The total number of bad sectors discovered on the device
 * * The rate at which sectors are failing verification (in counts/minute) --
 *   note that sectors which failed verification during a previous round of
 *   testing are not accounted for in this number)
 *
 * @param device_testing_context  The device against which stats are to be
 *                                logged.
 *
 */
void stats_log(device_testing_context_type *device_testing_context) {
    double write_rate, read_rate, bad_sector_rate;
    time_t now = time(NULL);
    char *ctime_str;
    struct timeval micronow;

    assert(!gettimeofday(&micronow, NULL));

    if(!device_testing_context->endurance_test_info.stats_file_handle) {
        return;
    }

    ctime_str = ctime(&now);

    // Trim off the training newline from ctime_str
    ctime_str[strlen(ctime_str) - 1] = 0;
    write_rate = ((double)(device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written - device_testing_context->endurance_test_info.stats_file_counters.last_bytes_written)) /
        (((double)timediff(device_testing_context->endurance_test_info.stats_file_counters.last_update_time, micronow)) / 1000000);
    read_rate = ((double)(device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read - device_testing_context->endurance_test_info.stats_file_counters.last_bytes_read)) /
        (((double)timediff(device_testing_context->endurance_test_info.stats_file_counters.last_update_time, micronow)) / 1000000);
    bad_sector_rate = ((double)(device_testing_context->endurance_test_info.total_bad_sectors - device_testing_context->endurance_test_info.stats_file_counters.last_bad_sectors)) /
        (((double)timediff(device_testing_context->endurance_test_info.stats_file_counters.last_update_time, micronow)) / 60000000);

    fprintf(device_testing_context->endurance_test_info.stats_file_handle,
            "%s,%lu,%lu,%lu,%0.2f,%lu,%lu,%0.2f,%lu,%lu,%0.2f\n",
            ctime_str,
            device_testing_context->endurance_test_info.rounds_completed,
            device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written - device_testing_context->endurance_test_info.stats_file_counters.last_bytes_written,
            device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written,
            write_rate,
            device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read - device_testing_context->endurance_test_info.stats_file_counters.last_bytes_read,
            device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read,
            read_rate,
            device_testing_context->endurance_test_info.total_bad_sectors - device_testing_context->endurance_test_info.stats_file_counters.last_bad_sectors,
            device_testing_context->endurance_test_info.total_bad_sectors,
            bad_sector_rate);
    fflush(device_testing_context->endurance_test_info.stats_file_handle);

    memcpy(&device_testing_context->endurance_test_info.stats_file_counters.last_update_time, &micronow, sizeof(struct timeval));
    device_testing_context->endurance_test_info.stats_file_counters.last_bytes_written = device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written;
    device_testing_context->endurance_test_info.stats_file_counters.last_bytes_read = device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read;
    device_testing_context->endurance_test_info.stats_file_counters.last_bad_sectors = device_testing_context->endurance_test_info.total_bad_sectors;
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
void draw_sectors(device_testing_context_type *device_testing_context, uint64_t start_sector, uint64_t end_sector) {
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
            cur_block_has_bad_sectors |= device_testing_context->endurance_test_info.sector_map[j] & SECTOR_MAP_FLAG_FAILED;
            num_written_sectors += (device_testing_context->endurance_test_info.sector_map[j] & SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND) >> 1;
            num_read_sectors += (device_testing_context->endurance_test_info.sector_map[j] & SECTOR_MAP_FLAG_READ_THIS_ROUND) >> 2;
            this_round |= device_testing_context->endurance_test_info.sector_map[j] & SECTOR_MAP_FLAG_FAILED_THIS_ROUND;
            unwritable |= device_testing_context->endurance_test_info.sector_map[j] & SECTOR_MAP_FLAG_DO_NOT_USE;
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
 * @param device_testing_context  The device whose sectors should be marked as
 *                                "written".
 * @param start_sector            The sector number of the first sector to be
 *                                marked as written.
 * @param end_sector              The sector number at which to stop marking
 *                                sectors as written.  (Ergo, all sectors within
 *                                the range [start_sector, end_sector) are
 *                                marked as written.)
 */
void mark_sectors_written(device_testing_context_type *device_testing_context, uint64_t start_sector, uint64_t end_sector) {
    uint64_t i;

    for(i = start_sector; i < end_sector && i < device_testing_context->device_info.num_physical_sectors; i++) {
        device_testing_context->endurance_test_info.sector_map[i] |= SECTOR_MAP_FLAG_WRITTEN_THIS_ROUND;
    }

    draw_sectors(device_testing_context, start_sector, end_sector);
}

/**
 * Mark the given sectors as "read" in the sector map.  The blocks containing
 * the given sectors are redrawn on the display.  The display is not refreshed
 * after the blocks are drawn.
 *
 * @param device_testing_context  The device whose sectors should be marked as
 *                                "read".
 * @param start_sector            The sector number of the first sector to be
 *                                marked as read.
 * @param end_sector              The sector number at which to stop marking
 *                                sectors as read.  (Ergo, all sectors within
 *                                the range [start_sector, end_sector) are
 *                                marked as read.)
 */
void mark_sectors_read(device_testing_context_type *device_testing_context, uint64_t start_sector, uint64_t end_sector) {;
    uint64_t i;

    for(i = start_sector; i < end_sector && i < device_testing_context->device_info.num_physical_sectors; i++) {
        device_testing_context->endurance_test_info.sector_map[i] |= SECTOR_MAP_FLAG_READ_THIS_ROUND;
    }

    draw_sectors(device_testing_context, start_sector, end_sector);
}

/**
 * Draw the "% sectors bad" display.
 */
void draw_percentage(device_testing_context_type *device_testing_context) {
    float percent_bad;
    if(device_testing_context->device_info.num_physical_sectors) {
        percent_bad = (((float) device_testing_context->endurance_test_info.total_bad_sectors) / ((float) device_testing_context->device_info.num_physical_sectors)) * 100.0;
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
 * @param device_testing_context  The device whose sectors should be marked as
 *                                bad.
 * @param sector_num              The sector number of the sector to be marked
 *                                as bad.
 */
void mark_sector_bad(device_testing_context_type *device_testing_context, uint64_t sector_num) {
    if(!(device_testing_context->endurance_test_info.sector_map[sector_num] & SECTOR_MAP_FLAG_FAILED)) {
        device_testing_context->endurance_test_info.total_bad_sectors++;
    }

    device_testing_context->endurance_test_info.sector_map[sector_num] |= SECTOR_MAP_FLAG_FAILED_THIS_ROUND | SECTOR_MAP_FLAG_FAILED;

    draw_sectors(device_testing_context, sector_num, sector_num + 1);
    draw_percentage(device_testing_context);
}

/**
 * Determines whether the given sector is marked as "bad" in the sector map.
 *
 * @param device_testing_context  The device whose sector map should be checked.
 * @param sector_num              The sector number of the sector to query.
 *
 * @returns Non-zero if the sector has been marked as "bad" in the sector map,
 *          zero otherwise.
 */
char is_sector_bad(device_testing_context_type *device_testing_context, uint64_t sector_num) {
    return device_testing_context->endurance_test_info.sector_map[sector_num] & SECTOR_MAP_FLAG_FAILED;
}

/**
 * Recomputes the parameters for displaying the sector map on the display, then
 * redraws the entire sector map.  The display is not refreshed after the sector
 * map is redrawn.  If sector_map is NULL, the sector map is not redrawn, but
 * the display parameters are still recomputed.
 */
void redraw_sector_map(device_testing_context_type *device_testing_context) {
    if(program_options.no_curses) {
        return;
    }

    sector_display.blocks_per_line = COLS - 41;
    sector_display.num_lines = LINES - 8;
    sector_display.num_blocks = sector_display.num_lines * sector_display.blocks_per_line;
    sector_display.sectors_per_block = device_testing_context->device_info.num_physical_sectors / sector_display.num_blocks;
    sector_display.sectors_in_last_block = device_testing_context->device_info.num_physical_sectors % sector_display.num_blocks + sector_display.sectors_per_block;

    mvprintw(BLOCK_SIZE_DISPLAY_Y, BLOCK_SIZE_DISPLAY_X, "%'lu bytes", sector_display.sectors_per_block * device_testing_context->device_info.sector_size);

    if(!device_testing_context->endurance_test_info.sector_map) {
        return;
    }

    draw_sectors(device_testing_context, 0, device_testing_context->device_info.num_physical_sectors);
}

void print_sql_status(sql_thread_status_type status) {
    mvprintw(SQL_STATUS_Y, SQL_STATUS_X, "               ");

    if(!program_options.db_host || !program_options.db_user || !program_options.db_pass || !program_options.db_name) {
        return;
    }

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

void print_device_name(device_testing_context_type *device_testing_context) {
    if(ncurses_active && device_testing_context->device_info.device_name) {
        mvprintw(DEVICE_NAME_DISPLAY_Y, DEVICE_NAME_DISPLAY_X, "%.23s ", device_testing_context->device_info.device_name);
        refresh();
    }
}

/**
 * Redraws the entire display.
 *
 * @param device_testing_context  The device whose details will be shown on the
 *                                screen.
 */
void redraw_screen(device_testing_context_type *device_testing_context) {
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
        print_device_name(device_testing_context);

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

        if(device_testing_context->endurance_test_info.test_started) {
            j = snprintf(msg_buffer, sizeof(msg_buffer), " Round %'lu ", device_testing_context->endurance_test_info.rounds_completed + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), msg_buffer);
        }

        if(device_testing_context->endurance_test_info.current_phase == CURRENT_PHASE_WRITING) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        } else if(device_testing_context->endurance_test_info.current_phase == CURRENT_PHASE_READING) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        // Draw the reported size of the device if it's been determined
        if(device_testing_context->device_info.logical_size) {
            snprintf(msg_buffer, 26, "%'lu bytes", device_testing_context->device_info.logical_size);
            mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%-25s", msg_buffer);
        }

        // Draw the detected size of the device if it's been determined
        if(device_testing_context->device_info.physical_size) {
            snprintf(msg_buffer, 26, "%'lu bytes", device_testing_context->device_info.physical_size);
            mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%-25s", msg_buffer);
        }

        if(device_testing_context->device_info.is_fake_flash == FAKE_FLASH_YES) {
            attron(COLOR_PAIR(RED_ON_BLACK));
            mvaddstr(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Yes");
            attroff(COLOR_PAIR(RED_ON_BLACK));
        } else if(device_testing_context->device_info.is_fake_flash == FAKE_FLASH_NO) {
            attron(COLOR_PAIR(GREEN_ON_BLACK));
            mvaddstr(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Probably not");
            attroff(COLOR_PAIR(GREEN_ON_BLACK));
        }

        if(sector_display.sectors_per_block) {
            mvprintw(BLOCK_SIZE_DISPLAY_Y, BLOCK_SIZE_DISPLAY_X, "%'lu bytes", sector_display.sectors_per_block * device_testing_context->device_info.sector_size);
        }

        if(device_testing_context->performance_test_info.sequential_read_speed) {
            mvaddstr(SEQUENTIAL_READ_SPEED_DISPLAY_Y, SEQUENTIAL_READ_SPEED_DISPLAY_X, format_rate(device_testing_context->performance_test_info.sequential_read_speed, msg_buffer, 31));
        }

        if(device_testing_context->performance_test_info.sequential_write_speed) {
            mvaddstr(SEQUENTIAL_WRITE_SPEED_DISPLAY_Y, SEQUENTIAL_WRITE_SPEED_DISPLAY_X, format_rate(device_testing_context->performance_test_info.sequential_write_speed, msg_buffer, 31));
        }

        if(device_testing_context->performance_test_info.random_read_iops) {
            mvprintw(RANDOM_READ_SPEED_DISPLAY_Y, RANDOM_READ_SPEED_DISPLAY_X, "%0.2f IOPS/s (%s)", device_testing_context->performance_test_info.random_read_iops,
                format_rate(device_testing_context->performance_test_info.random_read_iops * 4096, rate, sizeof(rate)));
        }

        if(device_testing_context->performance_test_info.random_write_iops) {
            mvprintw(RANDOM_WRITE_SPEED_DISPLAY_Y, RANDOM_WRITE_SPEED_DISPLAY_X, "%0.2f IOPS/s (%s)", device_testing_context->performance_test_info.random_write_iops,
                format_rate(device_testing_context->performance_test_info.random_write_iops * 4096, rate, sizeof(rate)));
        }

        if(device_testing_context->performance_test_info.sequential_read_speed != 0 || device_testing_context->performance_test_info.sequential_write_speed != 0 ||
            device_testing_context->performance_test_info.random_read_iops != 0 || device_testing_context->performance_test_info.random_write_iops != 0) {
            speed_qualifications_shown = 1;
        }

        print_class_marking_qualifications(device_testing_context);
        redraw_sector_map(device_testing_context);
        draw_percentage(device_testing_context);
        refresh();
    }
}

/**
 * Waits for the lock on the lockfile to be released.
 *
 * @param device_testing_context  The current device being tested.  (This is
 *                                needed in case the screen needs to be redrawn
 *                                while waiting for the lock to be released.)
 * @param topwin                  A pointer to the pointer to the window
 *                                currently being shown on the display.  The
 *                                contents of the window will be saved, and the
 *                                window will be destroyed and recreated after
 *                                the lockfile is unlocked.  The new pointer to
 *                                the window will be stored in the location
 *                                pointed to by topwin.
 */
void wait_for_file_lock(device_testing_context_type *device_testing_context, WINDOW **topwin) {
    WINDOW *window;
    FILE *memfile;
    main_thread_status_type previous_status;

    if(is_lockfile_locked()) {
        previous_status = main_thread_status;
        main_thread_status = MAIN_THREAD_STATUS_PAUSED;
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_WAITING_FOR_FILE_LOCK);
        if(!program_options.no_curses) {
            if(topwin) {
                assert(memfile = fmemopen(NULL, 131072, "r+"));
                putwin(*topwin, memfile);
                rewind(memfile);
            }

            window = message_window(device_testing_context, stdscr, "Paused",
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
                handle_key_inputs(device_testing_context, window);

                // I'm not sure if napms() depends on curses being initialized,
                // so we'll play it safe and assume that it does.  If curses
                // isn't initialized, we'll just use sleep() instead and sleep
                // for a full second.  It shouldn't be a big deal.
                napms(100);
            } else {
                sleep(1);
            }
        }

        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_FILE_LOCK_RELEASED);
        main_thread_status = previous_status;

        // We're just going to redraw the whole thing, so we don't need to
        // worry about erasing it first
        if(!program_options.no_curses) {
            delwin(window);
            erase();

            redraw_screen(device_testing_context);

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
 * @param device_testing_context  The current device being tested.  (This is
 *                                needed in case the screen needs to be redrawn
 *                                while the RNG test is in progress.)
 *
 * @returns The number of bytes per second the system is capable of generating.
 */
double profile_random_number_generator(device_testing_context_type *device_testing_context) {
    struct timeval start_time, end_time;
    int i;
    int64_t total_random_numbers_generated = 0;
    time_t diff;
    char rate_str[16];
    WINDOW *window;

    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_PROFILING_RNG);
    window = message_window(device_testing_context, stdscr, NULL, "Profiling random number generator...", 0);

    // Generate random numbers for 5 seconds.
    rng_init(device_testing_context, 0);
    assert(!gettimeofday(&start_time, NULL));
    do {
        for(i = 0; i < 100; i++) {
            rng_get_random_number(device_testing_context);
            total_random_numbers_generated++;
        }
        assert(!gettimeofday(&end_time, NULL));
        handle_key_inputs(device_testing_context, window);
        diff = timediff(start_time, end_time);
    } while(diff <= (RNG_PROFILE_SECS * 1000000));

    // Turn total number of random numbers into total number of bytes.
    total_random_numbers_generated *= sizeof(int);

    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DONE_PROFILING_RNG);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_RNG_STATS, format_rate(((double) total_random_numbers_generated) / (((double) timediff(start_time, end_time)) / 1000000.0), rate_str, sizeof(rate_str)));

    if(window) {
        erase_and_delete_window(window);
    }

    if(total_random_numbers_generated < 471859200) {
        // Display a warning message to the user
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_RNG_TOO_SLOW);
        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Your system is only able to generate %s of random data.  "
                 "The device may appear to be slower than it actually is, "
                 "and speed test results may be inaccurate.",
                 format_rate(((double) total_random_numbers_generated) / (((double) diff) / 1000000.0), rate_str, sizeof(rate_str)));
        message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 1);
    }

    return ((double) total_random_numbers_generated) / (((double) diff) / 1000000.0);
}

void print_status_update(device_testing_context_type *device_testing_context) {
    struct timeval cur_time;
    double rate;
    double secs_since_last_update;

    char str[18];

    if(program_options.no_curses) {
        return;
    }

    assert(!gettimeofday(&cur_time, NULL));
    secs_since_last_update = cur_time.tv_sec - device_testing_context->endurance_test_info.screen_counters.last_update_time.tv_sec;
    secs_since_last_update *= 1000000.0;
    secs_since_last_update += cur_time.tv_usec - device_testing_context->endurance_test_info.screen_counters.last_update_time.tv_usec;
    secs_since_last_update /= 1000000.0;

    if(secs_since_last_update < 0.5) {
        return;
    }

    rate = device_testing_context->endurance_test_info.screen_counters.bytes_since_last_update / secs_since_last_update;
    device_testing_context->endurance_test_info.screen_counters.bytes_since_last_update = 0;

    format_rate(rate, str, sizeof(str));
    mvprintw(STRESS_TEST_SPEED_DISPLAY_Y, STRESS_TEST_SPEED_DISPLAY_X, " %-15s", str);

    assert(!gettimeofday(&device_testing_context->endurance_test_info.screen_counters.last_update_time, NULL));
}

/**
 * Writes the given data to the device.  Data is copied to a page-aligned
 * buffer, then written in chunks that correspond to the device's optimal block
 * size (as specified in device_testing_context->device_info.optimal_block_size)
 * or the number of bytes remaining, whichever is smaller.  This function is
 * used primarily for the device size test; it does not gracefully handle device
 * disconnects/reconnects.
 *
 * @param device_testing_context  The device to which to write the data.
 * @param buf                     A pointer to the buffer containing the data to
 *                                be written.
 * @param len                     The number of bytes to be written.
 *
 * @returns 0 if the operation completed successfully, or -1 if it did not.  On
 *          error, errno is set to the underlying error.
 */
int write_data_to_device(device_testing_context_type *device_testing_context, void *buf, uint64_t len) {
    uint64_t block_size, bytes_left, block_bytes_left;
    char *aligned_buf;
    int64_t ret;
    int iret;

    if(ret = posix_memalign((void **) &aligned_buf, sysconf(_SC_PAGESIZE), len)) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        return -1;
    }

    block_size = len > device_testing_context->device_info.optimal_block_size ? device_testing_context->device_info.optimal_block_size : len;

    bytes_left = len;
    while(bytes_left) {
        block_bytes_left = block_size > bytes_left ? bytes_left : block_size;
        while(block_bytes_left) {
            // Make sure the data is in an aligned buffer
            memcpy(aligned_buf, ((char *) buf) + (len - bytes_left), block_bytes_left);
            if((ret = write(device_testing_context->device_info.fd, aligned_buf, block_bytes_left)) == -1) {
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

/**
 * Displays a dialog to the user indicating that the device size test
 * encountered an I/O error.  This function blocks until the user dismisses the
 * dialog.
 *
 * @param device_testing_context  The current device being tested.  (This is
 *                                needed in case the display needs to be redrawn
 *                                while the dialog is being shown to the user.)
 */
void io_error_during_size_probe(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_DEVICE_SIZE_TEST_DUE_TO_IO_ERROR);

    message_window(device_testing_context, stdscr, WARNING_TITLE,
                   "We encountered an error while trying to determine the size "
                   "of the device.  It could be that the device was removed or "
                   "experienced an error and disconnected itself.  For now, "
                   "we'll assume that the device is the size it says it is -- "
                   "but if the device has actually been disconnected, the "
                   "remainder of the tests are going to fail pretty quickly.", 1);
}

/**
 * Displays a dialog to the user indicating that the device size test
 * encountered a memory allocation error.  This function blocks until the user
 * dismisses the dialog.
 *
 * @param device_testing_context  The current device being tested.  (This is
 *                                needed in case the display needs to be redrawn
 *                                while the dialog is being shown to the user.)
 * @param errnum                  The error number of the error that occurred.
 */
void memory_error_during_size_probe(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, "probe_device_size", SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(errnum));
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_DEVICE_SIZE_TEST_DUE_TO_MEMORY_ERROR);

    message_window(device_testing_context, stdscr, WARNING_TITLE,
                   "We encountered an error while trying to allocate memory to "
                   "test the size of the device.  For now, we'll assume that "
                   "the device is the size it says it is -- but if the device "
                   "is fake flash, the remainder of the tests are going to "
                   "fail pretty quickly.", 1);
}

/**
 * Executes the device capacity test.
 *
 * NOTE: This test works by writing 32MB of data to the card, then going back
 *       and verifying the first 16MB of that data.  The idea is that if a
 *       device is caching writes, and you make a write that is at least twice
 *       the size of the write cache, the first half of the write should be
 *       flushed out to the device's cold storage.  Of course, this assumes a
 *       couple of things: (1) that no device is going to have a cache size of
 *       more than 16MB; and (2) that the device flushes the cache in a
 *       first-in, first-out fashion.  If either of those ever turn out not to
 *       be true, we may have to come back and revisit our approach.
 *
 * On success, device_testing_context->capacity_test_info is populated with
 * information on the detected capacity of the device.
 *
 * @param device_testing_context  The device to be tested.
 *
 * @returns 0 if the test completed successfully, or -1 if an error occurred.
 */
uint64_t probe_device_size(device_testing_context_type *device_testing_context) {
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

    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_PROBING_FOR_DEVICE_SIZE);
    window = message_window(device_testing_context, stdscr, NULL, "Probing for actual device size...", 0);

    if(iret = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), buf_size)) {
        erase_and_delete_window(window);

        memory_error_during_size_probe(device_testing_context, iret);

        return -1;
    }

    if(iret = posix_memalign((void **) &readbuf, sysconf(_SC_PAGESIZE), buf_size)) {
        erase_and_delete_window(window);
        free(buf);

        memory_error_during_size_probe(device_testing_context, iret);

        return -1;
    }

    random_seed = time(NULL);
    rng_init(device_testing_context, random_seed);
    rng_fill_buffer(device_testing_context, buf, buf_size);

    // Decide where we'll put the initial data.  The first and last writes will
    // go at the beginning and end of the card; the other writes will be at
    // random sectors in each 1/8th of the card.
    initial_sectors[0] = 0;
    initial_sectors[num_slices - 1] = device_testing_context->device_info.num_logical_sectors - (1 + (slice_size / device_testing_context->device_info.sector_size));

    // Make sure we don't overwrite the initial set of sectors
    low = slice_size / device_testing_context->device_info.sector_size;
    high = device_testing_context->device_info.num_logical_sectors / 8;

    for(i = 1; i < (num_slices - 1); i++) {
        initial_sectors[i] = low + ((rng_get_random_number(device_testing_context) & RAND_MAX) % (high - low));
        low = (device_testing_context->device_info.num_logical_sectors / (num_slices - 1)) * i;

        if((initial_sectors[i] + (slice_size / device_testing_context->device_info.sector_size)) > low) {
            low = initial_sectors[i] + (slice_size / device_testing_context->device_info.sector_size);
        }

        if(i == 7) {
            // Make sure that the second-to-last slice doesnt overwrite the last slice
            high = device_testing_context->device_info.num_logical_sectors - (slice_size * 2);
        } else {
            high = (device_testing_context->device_info.num_logical_sectors / (num_slices - 1)) * (i + 1);
        }
    }

    // Write the blocks to the card.  We're going to write them in reverse order
    // so that if the card is caching some of the data when we go to read it
    // back, hopefully the stuff toward the end of the device will already be
    // flushed out of the cache.
    for(i = num_slices; i > 0; i--) {
        handle_key_inputs(device_testing_context, window);
        if(lseek(device_testing_context->device_info.fd, initial_sectors[i - 1] * device_testing_context->device_info.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        if(write_data_to_device(device_testing_context, buf + ((i - 1) * slice_size), slice_size)) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        wait_for_file_lock(device_testing_context, &window);
    }

    // We're going to repurpose high and low to hold the highest and lowest
    // possible sectors of the first "bad" sector
    low = 0;
    high = device_testing_context->device_info.num_logical_sectors;

    // Read the blocks back.
    for(i = 0; i < num_slices; i++) {
        handle_key_inputs(device_testing_context, window);
        if(lseek(device_testing_context->device_info.fd, initial_sectors[i] * device_testing_context->device_info.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        bytes_left = slice_size;
        while(bytes_left) {
            wait_for_file_lock(device_testing_context, &window);

            // For the read portion, we're just going to try to read the whole thing all at once
            if((ret = read(device_testing_context->device_info.fd, readbuf + (slice_size - bytes_left), bytes_left)) == -1) {
                // Ignore a read failure and just zero out the remainder of the buffer instead
                memset(buf + (slice_size - bytes_left), 0, bytes_left);
                bytes_left = 0;
            } else {
                bytes_left -= ret;
            }
        }

        // Compare the two buffers, sector_size bytes at a time
        for(j = 0; j < slice_size; j += device_testing_context->device_info.sector_size) {
            if(memcmp(readbuf + j, buf + (i * slice_size) + j, device_testing_context->device_info.sector_size)) {
                // Are we at the beginning of the device?
                if(i == 0) {
                    // Are we at the very first block?
                    multifree(2, buf, readbuf);
                    if(j == 0) {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_WARNING, MSG_FIRST_SECTOR_ISNT_STABLE);
                        erase_and_delete_window(window);
                        message_window(device_testing_context, stdscr, WARNING_TITLE,
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

                        return -1;
                    } else {
                        erase_and_delete_window(window);
                        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, j);

                        device_testing_context->capacity_test_info.test_performed = 1;
                        device_testing_context->capacity_test_info.device_size = j;
                        device_testing_context->capacity_test_info.num_sectors = j / device_testing_context->device_info.sector_size;
                        device_testing_context->capacity_test_info.is_fake_flash = (j == device_testing_context->device_info.logical_size) ? FAKE_FLASH_NO : FAKE_FLASH_YES;

                        return -1;
                    }
                } else {
                    if(j > 0) {
                        erase_and_delete_window(window);
                        multifree(2, buf, readbuf);

                        device_testing_context->capacity_test_info.test_performed = 1;
                        device_testing_context->capacity_test_info.device_size = (initial_sectors[i] * device_testing_context->device_info.sector_size) + j;
                        device_testing_context->capacity_test_info.num_sectors = device_testing_context->capacity_test_info.device_size / device_testing_context->device_info.sector_size;
                        device_testing_context->capacity_test_info.is_fake_flash = (device_testing_context->capacity_test_info.device_size == device_testing_context->device_info.logical_size) ? FAKE_FLASH_NO : FAKE_FLASH_YES;

                        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, device_testing_context->capacity_test_info.device_size);

                        return 0;
                    } else {
                        high = initial_sectors[i];
                        i = 9;
                        break;
                    }
                }
            } else {
                low = initial_sectors[i] + (j / device_testing_context->device_info.sector_size) + 1;
            }
        }
    }

    // If we didn't have any mismatches, then the card is probably good.
    if(high == device_testing_context->device_info.num_logical_sectors) {
        erase_and_delete_window(window);
        multifree(2, buf, readbuf);

        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, device_testing_context->device_info.logical_size);

        device_testing_context->capacity_test_info.test_performed = 1;
        device_testing_context->capacity_test_info.device_size = device_testing_context->device_info.logical_size;
        device_testing_context->capacity_test_info.num_sectors = device_testing_context->device_info.num_logical_sectors;
        device_testing_context->capacity_test_info.is_fake_flash = FAKE_FLASH_NO;

        return 0;
    }

    // Otherwise, start bisecting the area between low and high to figure out
    // where the first "bad" block is
    keep_searching = 1;

    while(keep_searching) {
        handle_key_inputs(device_testing_context, window);
        // Select a 32MB area centered between low and high
        size = high - low;
        if(size > ((slice_size * num_slices) / device_testing_context->device_info.sector_size)) {
            cur = (size / 2) + low;
            size = (slice_size * num_slices) / device_testing_context->device_info.sector_size;
        } else {
            // The area between high and low isn't big enough to hold 32MB
            cur = low;

            // Give up after this round
            keep_searching = 0;
        }

        if(lseek(device_testing_context->device_info.fd, cur * device_testing_context->device_info.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        // Generate some more random data
        rng_fill_buffer(device_testing_context, buf, slice_size * num_slices);
        if(write_data_to_device(device_testing_context, buf, slice_size * num_slices)) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        if(lseek(device_testing_context->device_info.fd, cur * device_testing_context->device_info.sector_size, SEEK_SET) == -1) {
            errnum = errno;
            erase_and_delete_window(window);
            multifree(2, buf, readbuf);

            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
            io_error_during_size_probe(device_testing_context);

            return -1;
        }

        // Read the data back -- we're only going to read back half the data
        // to avoid the possibility that any part of the other half is cached
        for(i = 0; i < 4; i++) {
            handle_key_inputs(device_testing_context, window);
            bytes_left = slice_size;
            while(bytes_left) {
                if((ret = read(device_testing_context->device_info.fd, readbuf + (slice_size - bytes_left), bytes_left)) == -1) {
                    // Ignore a read failure and just zero out the remainder of the buffer instead
                    memset(buf + (slice_size - bytes_left), 0, bytes_left);
                    bytes_left = 0;
                } else {
                    bytes_left -= ret;
                }
            }

            // Compare the data, sector_size bytes at a time.
            for(j = 0; j < slice_size; j += device_testing_context->device_info.sector_size) {
                handle_key_inputs(device_testing_context, window);
                if(memcmp(buf + (i * slice_size) + j, readbuf + j, device_testing_context->device_info.sector_size)) {
                    if(j > 0) {
                        erase_and_delete_window(window);
                        multifree(2, buf, readbuf);

                        device_testing_context->capacity_test_info.test_performed = 1;
                        device_testing_context->capacity_test_info.device_size = (cur * device_testing_context->device_info.sector_size) + (i * slice_size) + j;
                        device_testing_context->capacity_test_info.num_sectors = device_testing_context->capacity_test_info.device_size / device_testing_context->device_info.sector_size;
                        device_testing_context->capacity_test_info.is_fake_flash = (device_testing_context->capacity_test_info.device_size == device_testing_context->device_info.logical_size) ? FAKE_FLASH_NO : FAKE_FLASH_YES;

                        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, device_testing_context->capacity_test_info.device_size);

                        return device_testing_context->capacity_test_info.device_size;
                    } else {
                        high = cur + (((i * slice_size) + j) / device_testing_context->device_info.sector_size);
                        i = 4;
                        break;
                    }
                }
            }

            if(i != 4) {
                // We verified all the data successfully, so the bad area has to be past where we are now
                low = cur + ((slice_size * 4) / device_testing_context->device_info.sector_size);
            }
        }
    }

    erase_and_delete_window(window);
    multifree(2, buf, readbuf);

    device_testing_context->capacity_test_info.test_performed = 1;
    device_testing_context->capacity_test_info.device_size = low * device_testing_context->device_info.sector_size;
    device_testing_context->capacity_test_info.num_sectors = low;
    device_testing_context->capacity_test_info.is_fake_flash = (device_testing_context->capacity_test_info.device_size == device_testing_context->device_info.logical_size) ? FAKE_FLASH_NO : FAKE_FLASH_YES;

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_SIZE, device_testing_context->capacity_test_info.device_size);

    return 0;
}

int *random_list(device_testing_context_type *device_testing_context) {
    int i, j, k, l, source[16], temp[16], *list;

    assert(list = malloc(sizeof(int) * 16));

    // Initialize the source list
    for(i = 0; i < 16; i++) {
        source[i] = i;
    }

    for(i = 0; i < 16; i++) {
        // Pick a new number and add it to the list
        j = (rng_get_random_number(device_testing_context) & RAND_MAX) % (16 - i);
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
 * @param device_testing_context  The device for which to get the starting
 *                                sector of the requested slice.
 * @param slice_num               The slice for which to get the starting
 *                                sector.
 *
 * @returns The sector on which the slice starts.
*/
uint64_t get_slice_start(device_testing_context_type *device_testing_context, int slice_num) {
    return (device_testing_context->device_info.num_physical_sectors / NUM_SLICES) * slice_num;
}

/**
 * Displays the end-of-test summary and shows it on screen/writes it out to the
 * log file.
 *
 * @param device_testing_context  The device to be summarized.
 * @param abort_reason            The reason that the test was aborted.
 */

void print_device_summary(device_testing_context_type *device_testing_context, int abort_reason) {
    char messages[9][384];
    char *out_messages[9];
    int i;

    const char *abort_reasons[] = {
                                  "(unknown)",
                                  "read_error",
                                  "write error",
                                  "seek error",
                                  "50% of sectors have failed",
                                  "device went away"
    };

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_COMPLETE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_REASON_FOR_ABORTING_TEST, abort_reason > 5 ? abort_reasons[0] : abort_reasons[abort_reason]);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_COMPLETED, device_testing_context->endurance_test_info.rounds_completed);

    snprintf(messages[0], sizeof(messages[0]), "Reason for aborting test             : %s", abort_reason > 5 ? abort_reasons[0] : abort_reasons[abort_reason]);
    out_messages[0] = messages[0];
    snprintf(messages[1], sizeof(messages[1]), "Number of read/write cycles completed: %'lu", device_testing_context->endurance_test_info.rounds_completed);
    out_messages[1] = messages[1];

    if(device_testing_context->endurance_test_info.rounds_to_first_error != -1ULL) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_FIRST_FAILURE, device_testing_context->endurance_test_info.rounds_to_first_error);
        snprintf(messages[2], sizeof(messages[2]), "Read/write cycles to first failure   : %'lu", device_testing_context->endurance_test_info.rounds_to_first_error);
        out_messages[2] = messages[2];
    } else {
        out_messages[2] = NULL;
    }

    if(device_testing_context->endurance_test_info.rounds_to_10_threshold != -1ULL) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUNDS_TO_10_PERCENT_FAILURE, device_testing_context->endurance_test_info.rounds_to_10_threshold);
        snprintf(messages[5], sizeof(messages[3]), "Read/write cycles to 10%% failure     : %'lu", device_testing_context->endurance_test_info.rounds_to_10_threshold);
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

    message_window(device_testing_context, stdscr, "Test Complete", msg_buffer, 1);
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
 *                      line.  The caller should set this to argv[0].
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
    return message_window(NULL, stdscr, "Device Disconnected",
                          "The device has been disconnected.  It may have done "
                          "this on its own, or it may have been manually "
                          "removed (e.g., if someone pulled the device out of "
                          "its USB port).\n\nDon't worry -- just plug the "
                          "device back in.  We'll verify that it's the same "
                          "device, then resume the stress test automatically.", 0);
}

WINDOW *resetting_device_message() {
    return message_window(NULL, stdscr, "Attempting to reset device",
                          "The device has encountered an error.  We're "
                          "attempting to reset the device to see if that fixes "
                          "the issue.  You shouldn't need to do anything -- "
                          "but if this message stays up for a while, it might "
                          "indicate that the device has failed or isn't "
                          "handling the reset well.  In that case, you can try "
                          "unplugging the device and plugging it back in to "
                          "get the device working again.", 0);
}

int64_t lseek_or_reset_device(device_testing_context_type *device_testing_context, off_t position, int *device_was_disconnected);

/**
 * Displays a message to the user indicating that the device has been
 * disconnected and waits for the device to be reconnected.
 *
 * @param device_testing_context  The device currently being tested.
 * @param position                The position to seek to once the device has
 *                                been reconnected.  If `seek_after_reconnect`
 *                                is set to zero, this parameter is ignored.
 * @param seek_after_reconnect    Non-zero to indicate that a seek operation, to
 *                                the position indicated by `position`, should
 *                                be performed after the device has been
 *                                reconnected, or zero to indicate that no seek
 *                                operation should be performed.
 *
 * @returns 0 if the device was reconnected successfully, or -1 if an error
 *          occurred.
 */
int handle_device_disconnect(device_testing_context_type *device_testing_context, off_t position, int seek_after_reconnect) {
    WINDOW *window;
    char *new_device_name;
    dev_t new_device_num;
    int ret, local_errno;
    main_thread_status_type previous_status = main_thread_status;
    device_search_params_t device_search_params;
    device_search_result_t *device_search_result;
    main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;

    if(device_testing_context->device_info.fd != -1) {
        device_info_invalidate_file_handle(device_testing_context);
    }

    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_DISCONNECTED);
    window = device_disconnected_message();

    device_search_params.preferred_dev_name = NULL;
    device_search_params.must_match_preferred_dev_name = 0;

    device_search_result = wait_for_device_reconnect(device_testing_context, &device_search_params);

    handle_key_inputs(device_testing_context, window);
    main_thread_status = previous_status;

    if(device_search_result) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RECONNECTED, device_search_result->device_name);

        device_testing_context->device_info.fd = device_search_result->fd;

        if(program_options.device_name) {
            free(program_options.device_name);
        }

        program_options.device_name = strdup(device_search_result->device_name);

        if(device_info_set_device_name(device_testing_context, device_search_result->device_name)) {
            local_errno = errno;
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_MALLOC_ERROR, local_errno);

            erase_and_delete_window(window);
            redraw_screen(device_testing_context);

            return -1;
        }

        device_testing_context->device_info.device_num = device_search_result->device_num;

        free_device_search_result(device_search_result);

        if(seek_after_reconnect) {
            if(lseek_or_reset_device(device_testing_context, position, NULL) == -1) {
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);

                erase_and_delete_window(window);
                redraw_screen(device_testing_context);

                return -1;
            }
        }

        erase_and_delete_window(window);
        redraw_screen(device_testing_context);

        return 0;
    } else {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_FAILED_TO_REOPEN_DEVICE);

        erase_and_delete_window(window);
        redraw_screen(device_testing_context);

        return -1;
    }
}

/**
 * Seeks to the given position relative to the start of the device.  Gracefully
 * handles device errors and disconnects by retrying the operation or, if the
 * device has been disconnected, waiting for the device to reconnect.
 *
 * @param device_testing_context   The device on which to seek.
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
off_t lseek_or_retry(device_testing_context_type *device_testing_context, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    if((ret = lseek(device_testing_context->device_info.fd, position, SEEK_SET)) == -1) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_TO_SECTOR_ERROR, position / device_testing_context->device_info.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_testing_context->device_info.device_num)) {
            if(device_was_disconnected) {
                *device_was_disconnected = 1;
            }

            if(handle_device_disconnect(device_testing_context, position, 0) == -1) {
                return -1;
            }
        } else {
            ret = lseek(device_testing_context->device_info.fd, position, SEEK_SET);
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
 * @param device_testing_context   The device on which to seek.
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
int64_t lseek_or_reset_device(device_testing_context_type *device_testing_context, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = lseek_or_retry(device_testing_context, position, device_was_disconnected);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_testing_context->device_info.device_num) || device_testing_context->device_info.fd == -1) {
            if(device_was_disconnected) {
                *device_was_disconnected = 1;
            }

            if(handle_device_disconnect(device_testing_context, position, 0)) {
                return -1;
            }
        } else {
            if(can_reset_device(device_testing_context)) {
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);
                window = resetting_device_message();

                main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                iret = reset_device(device_testing_context);
                main_thread_status = previous_status;

                if(iret) {
                    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);

                    erase_and_delete_window(window);
                    redraw_screen(device_testing_context);

                    return -1;
                }

                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                retry_count++;

                ret = lseek_or_retry(device_testing_context, position, device_was_disconnected);

                *device_was_disconnected = 1;

                erase_and_delete_window(window);
                redraw_screen(device_testing_context);
            } else {
                // Insta-fail
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
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
 * @param device_testing_context  The device from which to read.
 * @param buf                     A pointer to a buffer which will receive the
 *                                data read from the device.
 * @param count                   The number of bytes to read from the device.
 * @param position                The current position of the file pointer in
 *                                the device.  If the device is disconnected or
 *                                needs to be reset, a seek operation to the
 *                                given position is performed after the device
 *                                reconnects.
 *
 * @returns The number of bytes read from the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t read_or_retry(device_testing_context_type *device_testing_context, void *buf, uint64_t count, off_t position) {
    int retry_count = 0;
    int64_t ret;

    ret = read(device_testing_context->device_info.fd, buf, count);
    if(ret == -1) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_READ_ERROR_IN_SECTOR, position / device_testing_context->device_info.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_testing_context->device_info.device_num)) {
            if(handle_device_disconnect(device_testing_context, position, 1)) {
                return -1;
            }
        } else {
            ret = read(device_testing_context->device_info.fd, buf, count);
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
 * @param device_testing_context  The device from which to read.
 * @param buf                     A pointer to a buffer which will receive the
 *                                data read from the device.
 * @param count                   The number of bytes to read from the device.
 * @param position                The current position of the file pointer in
 *                                the device.  If the device is disconnected or
 *                                needs to be reset, a seek operation to the
 *                                given position is performed after the device
 *                                reconnects.
 *
 * @returns The number of bytes read from the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t read_or_reset_device(device_testing_context_type *device_testing_context, void *buf, uint64_t count, off_t position) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = read_or_retry(device_testing_context, buf, count, position);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        if(did_device_disconnect(device_testing_context->device_info.device_num) || device_testing_context->device_info.fd == -1) {
            if(handle_device_disconnect(device_testing_context, position, 1)) {
                return -1;
            }
        } else {
            if(can_reset_device(device_testing_context)) {
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);
                window = resetting_device_message();

                main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                iret = reset_device(device_testing_context);
                main_thread_status = previous_status;

                if(iret) {
                    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);
                    retry_count = MAX_RESET_RETRIES;
                } else {
                    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                    retry_count++;

                    if(lseek_or_retry(device_testing_context, position, NULL) == -1) {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);
                        erase_and_delete_window(window);
                        redraw_screen(device_testing_context);
                        return -1;
                    }

                    ret = read_or_retry(device_testing_context, buf, count, position);
                }

                erase_and_delete_window(window);
                redraw_screen(device_testing_context);
            } else {
                // Insta-fail
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
                return -1;
            }
        }
    }

    return ret;
}

/**
 * Writes to the given device.  Gracefully handles device errors and disconnects
 * by retrying the operation or, if the device has been disconnected, waiting
 * for the device to reconnect.
 *
 * @param device_testing_context   The device to which to write.
 * @param buf                      A pointer to a buffer containing the data to
 *                                 be written to the device.
 * @param count                    The number of bytes to write to the device.
 * @param position                 The current position of the file pointer in
 *                                 the device.  If the device is disconnected
 *                                 or needs to be reset, a seek operation to the
 *                                 given position is performed after the device
 *                                 reconnects.
 * @param device_was_disconnected  A pointer to a variable which will be set to
 *                                 1 if the device was disconnected during the
 *                                 course of this function, or left unmodified
 *                                 otherwise.
 *
 * @returns The number of bytes written to the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t write_or_retry(device_testing_context_type *device_testing_context, void *buf, uint64_t count, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = write(device_testing_context->device_info.fd, buf, count);
    if(ret == -1) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR_IN_SECTOR, position / device_testing_context->device_info.sector_size);
    }

    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(did_device_disconnect(device_testing_context->device_info.device_num)) {
            *device_was_disconnected = 1;
            if(device_testing_context->endurance_test_info.rounds_completed) {
                if(handle_device_disconnect(device_testing_context, position, 1)) {
                    return -1;
                }
            } else {
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_DEVICE_DISCONNECTED_DURING_ROUND_1);
                return -1;
            }
        } else {
            ret = write(device_testing_context->device_info.fd, buf, count);
            retry_count++;
        }
    }

    return ret;
}

/**
 * Writes to the given device.  Gracefully handles device errors and disconnects
 * by retrying the operation, attempting to reset the device, and/or waiting for
 * the device to reconnect.
 *
 * @param device_testing_context  The device to which to write.
 * @param buf                     A pointer to a buffer containing the data to
 *                                be written to the device.
 * @param count                   The number of bytes to write to the device.
 * @param position                The current position of the file pointer in
 *                                the device.  If the device is disconnected or
 *                                needs to be reset, a seek operation to the
 *                                given position is performed after the device
 *                                reconnects.
 * @param device_was_disconnected  A pointer to a variable which will be set to
 *                                 1 if the device was disconnected during the
 *                                 course of this function, or left unmodified
 *                                 otherwise.
 *
 * @returns The number of bytes written to the device, or -1 if (a) an
 *          unrecoverable error occurred, or (b) an error occurred and retry
 *          attempts have been exhausted.
 */
int64_t write_or_reset_device(device_testing_context_type *device_testing_context, void *buf, uint64_t count, off_t position, int *device_was_disconnected) {
    int retry_count = 0;
    WINDOW *window;
    int64_t ret;
    int iret;
    char *new_device_name;
    dev_t new_device_num;
    main_thread_status_type previous_status = main_thread_status;

    ret = write_or_retry(device_testing_context, buf, count, position, device_was_disconnected);
    while(ret == -1 && retry_count < MAX_RESET_RETRIES) {
        // If we haven't completed at least one round, then we can't be sure that the
        // beginning-of-device and middle-of-device are accurate -- and if the device
        // is disconnected and reconnected (or reset), the device name might change --
        // so only try to recover if we've completed at least one round.
        if(did_device_disconnect(device_testing_context->device_info.device_num) || device_testing_context->device_info.fd == -1) {
            *device_was_disconnected = 1;
            if(device_testing_context->endurance_test_info.rounds_completed) {
                if(handle_device_disconnect(device_testing_context, position, 1)) {
                    return -1;
                }
            } else {
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_DEVICE_DISCONNECTED_DURING_ROUND_1);
                return -1;
            }
        } else {
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ATTEMPTING_DEVICE_RESET);

            if(device_testing_context->endurance_test_info.rounds_completed) {
                if(can_reset_device(device_testing_context)) {
                    window = resetting_device_message();

                    main_thread_status = MAIN_THREAD_STATUS_DEVICE_DISCONNECTED;
                    iret = reset_device(device_testing_context);
                    main_thread_status = previous_status;

                    if(iret) {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_FAILED);
                        retry_count = MAX_RESET_RETRIES;
                    } else {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_RESET_SUCCESS);
                        retry_count++;

                        if(lseek_or_retry(device_testing_context, position, device_was_disconnected) == -1) {
                            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LSEEK_AFTER_DEVICE_RESET_FAILED);
                            erase_and_delete_window(window);
                            redraw_screen(device_testing_context);
                            return -1;
                        }

                        ret = write_or_retry(device_testing_context, buf, count, position, device_was_disconnected);
                    }

                    *device_was_disconnected = 1;

                    erase_and_delete_window(window);
                    redraw_screen(device_testing_context);
                } else {
                    // Insta-fail
                    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_DONT_KNOW_HOW_TO_RESET_DEVICE);
                    return -1;
                }
            } else {
                // Insta-fail
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_ENDURANCE_TEST_REFUSING_TO_RESET_DURING_ROUND_1);
                return -1;
            }
        }
    }

    return ret;
}

void malloc_error(device_testing_context_type *device_testing_context, int errnum) {
    snprintf(msg_buffer, sizeof(msg_buffer),
             "Failed to allocate memory for one of the buffers we need to do "
             "the stress test.  Unfortunately this means that we have to abort "
             "the stress test.\n\nThe error we got was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

void posix_memalign_error(device_testing_context_type *device_testing_context, int errnum) {
    snprintf(msg_buffer, sizeof(msg_buffer),
             "Failed to allocate memory for one of the buffers we need to do "
             "the stress test.  Unfortunately this means we have to abort the "
             "stress test.\n\nThe error we got was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Resets the read/written flags in every sector of the given device's sector
 * map.
 *
 * @param device_testing_context  The device whose sector map should be reset.
 */
void reset_sector_map(device_testing_context_type *device_testing_context) {
    for(uint64_t j = 0; j < device_testing_context->device_info.num_physical_sectors; j++) {
        device_testing_context->endurance_test_info.sector_map[j] &= SECTOR_MAP_FLAG_DO_NOT_USE | SECTOR_MAP_FLAG_FAILED;
    }
}

/**
 * Resets the read/written flags in the selected portion of the given device's
 * sector map.
 *
 * @param device_testing_context  The device whose sector map should be reset.
 * @param start                   The beginning of the range of sectors to be
 *                                reset.
 * @param end                     The end of the range of sectors to be reset.
 *                                The actual range of sectors that will be reset
 *                                is [start, end).
 */
void reset_sector_map_partial(device_testing_context_type *device_testing_context, uint64_t start, uint64_t end) {
    for(uint64_t j = start; j < end; j++) {
        device_testing_context->endurance_test_info.sector_map[j] &= SECTOR_MAP_FLAG_DO_NOT_USE | SECTOR_MAP_FLAG_FAILED;
    }
}

/**
 * The following data is written to each 512-byte sector:
 *
 * 00000000: SS SS SS SS SS SS SS SS   RR RR RR RR RR RR RR RR
 * 00000010: UU UU UU UU UU UU UU UU   UU UU UU UU UU UU UU UU
 * 00000020: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000030: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000040: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000050: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000060: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000070: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000080: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000090: LL MM NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000a0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000b0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000c0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000d0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000e0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000000f0: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000100: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000110: XX XX NN XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000120: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000130: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000140: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000150: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000160: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000170: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000180: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 00000190: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001a0: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001b0: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001c0: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001d0: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001e0: XX XX XX XX XX XX XX XX   XX XX XX XX XX XX XX XX
 * 000001f0: XX XX XX XX XX XX XX XX   XX XX XX XX CC CC CC CC
 *
 * L, M, N, and X are all randomly generated data.
 *
 * S is the result of taking the sector number and XORing it with L.
 * R is the result of taking the round number and XORing it with M.
 * U is the result of taking the device's UUID and XORing it with N.
 * C is the CRC32 of the first 508 bytes of the sector's data.
 *
 * Why not just store the raw sector number/round number/UUID in the sector
 * instead?  Because then we wouldn't do a very good job of detecting stuck-on
 * or stuck-off bits in those locations.  This way, we're able to encode this
 * information into the sector while still making it look like random data.
 *
 * The following functions are to assist with embedding/decoding this data.
 */

/**
 * Given a sector of data read from a device (or about to be written to a
 * device), get the XOR value needed to extract the sector number from the data
 * (or the XOR value needed to embed the sector number into the data).
 *
 * @param data  A pointer to a buffer containing the data read from (or about to
 *              be written to) the sector.
 *
 * @returns The XOR value needed to embed the sector number into (or extract the
 *          sector number from) the sector.
 */
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

/**
 * Given a sector of data read from a device (or about to be written to a
 * device), get the XOR value needed to extract the round number from the data
 * (or the XOR value needed to embed the round number into the data).
 *
 * @param data  A pointer to a buffer containing the data read from (or about to
 *              be written to) the device.
 *
 * @returns The XOR value needed to embed the round number into (or extract the
 *          round number from) the sector.
 */
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

/**
 * Given a sector of data about to be written to a device, embed the device's
 * UUID into the data.
 *
 * @param uuid  The UUID to embed into the data.
 * @param data  A pointer to a buffer containing the data in which the UUID
 *              should be embedded.
 */
void embed_device_uuid(uuid_t uuid, char *data) {
    int i;
    for(i = 0; i < 16; i++) {
        data[i + 16] = uuid[i] ^ data[(i * 16) + 34];
    }
}

/**
 * Given a sector of data read from a device, extract the UUID from the data.
 *
 * @param data         A pointer to a buffer containing the data from which to
 *                     extract the UUID.
 * @param uuid_buffer  A pointer to a buffer where the extracted UUID should be
 *                     stored.
 */
void get_embedded_device_uuid(char *data, char *uuid_buffer) {
    int i;
    for(i = 0; i < 16; i++) {
        uuid_buffer[i] = data[i + 16] ^ data[(i * 16) + 34];
    }
}

/**
 * Given a sector of data about to be written to a device, embed the sector
 * number into the data.
 *
 * @param data           A pointer to a buffer containing the data in which the
 *                       sector number should be embedded.
 * @param sector_number  The sector number to be embedded.
 */
void embed_sector_number(char *data, uint64_t sector_number) {
    *((uint64_t *) data) = sector_number ^ get_sector_number_xor_val(data);
}

/**
 * Given a sector of data about to be written to a device, embed the round
 * number into the data.
 *
 * @param data       A pointer to a buffer containing the data in which the
 *                   round number should be embedded.
 * @param round_num  The round number to be embedded.
 */
void embed_round_number(char *data, int64_t round_num) {
    *((int64_t *) (data + 8)) = round_num ^ get_round_num_xor_val(data);
}

/**
 * Given a sector of data read from a device, extract the sector number from
 * the data.
 *
 * @param data  A pointer to a buffer containing the data read from the device.
 *
 * @returns The sector number extracted from the data.
 */
uint64_t decode_embedded_sector_number(char *data) {
    return (*((uint64_t *) data)) ^ get_sector_number_xor_val(data);
}

/**
 * Given a sector of data read from a device, extract the round number from the
 * data.
 *
 * @param data  A pointer to a buffer containing the data read from the device.
 *
 * @returns The round number extracted from the data.
 */
int64_t decode_embedded_round_number(char *data) {
    return (*((int64_t *) (data + 8))) ^ get_round_num_xor_val(data);
}

/**
 * Given a sector of data about to be written to a device, calculate the CRC32
 * of the data and embed it into the data.
 *
 * @param data         A pointer to a buffer containing the data in which the
 *                     CRC32 should be embedded.
 * @param sector_size  The size of the sector, in bytes.
 */
void embed_crc32c(char *data, int sector_size) {
    *((uint32_t *) &data[sector_size - sizeof(uint32_t)]) = calculate_crc32c(0, data, sector_size - sizeof(uint32_t));
}

/**
 * Given a sector of data read from a device, extract the CRC32 from the data.
 *
 * @param data         A pointer to a buffer containing the data read from the
 *                     device.
 * @param sector_size  The size of the sector, in bytes.
 *
 * @returns The CRC32 extracted from the data.
 */
uint32_t get_embedded_crc32c(char *data, int sector_size) {
    return *((uint32_t *) &data[sector_size - sizeof(uint32_t)]);
}

/**
 * Log the contents of a sector, and the data that was expected to be in the
 * sector, to the log file.
 *
 * @param device_testing_context  The device from which the data was read.
 * @param sector_num              The sector number of the given sector.
 * @param sector_size             The size of the sector, in bytes.
 * @param expected_data           A pointer to a buffer containing the data
 *                                expected to be in the sector.
 * @param actual_data             A pointer to a buffer containing the data
 *                                actually read from the device.
 */
void log_sector_contents(device_testing_context_type *device_testing_context, uint64_t sector_num, int sector_size, char *expected_data, char *actual_data) {
    char tmp[16];
    int i;

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_EXPECTED_DATA_WAS);

    for(i = 0; i < sector_size; i += 16) {
        // If, for some reason, the sector size isn't a multiple of 16, then
        // make sure we don't overrun the expected_data buffer
        memset(tmp, 0, 16);
        memcpy(tmp, expected_data + i, (sector_size - i) >= 16 ? 16 : (sector_size - i));
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_MISMATCHED_DATA_LINE, (sector_num * sector_size) + i, tmp[0] & 0xff, tmp[1] & 0xff, tmp[2] & 0xff, tmp[3] & 0xff, tmp[4] & 0xff, tmp[5] & 0xff, tmp[6] & 0xff, tmp[7] & 0xff, tmp[8] & 0xff, tmp[9] & 0xff, tmp[10] & 0xff, tmp[11] & 0xff, tmp[12] & 0xff, tmp[13] & 0xff, tmp[14] & 0xff, tmp[15] & 0xff);
    }

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_ACTUAL_DATA_WAS);

    for(i = 0; i < sector_size; i += 16) {
        memset(tmp, 0, 16);
        memcpy(tmp, actual_data + i, (sector_size - i) >= 16 ? 16 : (sector_size - i));
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_ENDURANCE_TEST_MISMATCHED_DATA_LINE, (sector_num * sector_size) + i, tmp[0] & 0xff, tmp[1] & 0xff, tmp[2] & 0xff, tmp[3] & 0xff, tmp[4] & 0xff, tmp[5] & 0xff, tmp[6] & 0xff, tmp[7] & 0xff, tmp[8] & 0xff, tmp[9] & 0xff, tmp[10] & 0xff, tmp[11] & 0xff, tmp[12] & 0xff, tmp[13] & 0xff, tmp[14] & 0xff, tmp[15] & 0xff);
    }

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG_VERBOSE, MSG_BLANK_LINE);
}

/**
 * Shows a warning to the user indicating that there was a problem loading the
 * requested state file.  The warning automatically dismisses itself after 15
 * seconds.
 *
 * @param device_testing_context  The current device being tested.  This is
 *                                needed in case the screen needs to be redrawn
 *                                while the dialog is being shown.
 */
void state_file_error(device_testing_context_type *device_testing_context) {
    WINDOW *window;
    int i;

    const char *warning_text =
        "There was a problem loading the state file.  If you want to continue "
        "and just ignore the existing state file, then you can ignore this "
        "message.  Otherwise, you have %d seconds to hit Ctrl+C.";

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_STATE_FILE_LOAD_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer), warning_text, 15);

    window = message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 0);

    if(window) {
        for(i = 0; i < 150; i++) {
            handle_key_inputs(device_testing_context, window);
            usleep(100000);
            if(i && !(i % 10)) {
                snprintf(msg_buffer, sizeof(msg_buffer), warning_text, 15 - (i / 10));
                delwin(window);
                window = message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 0);
                wrefresh(window);
            }
        }
    } else {
        sleep(15);
    }

    erase_and_delete_window(window);
}

/**
 * Shows the initial warning message displayed at the start of testing.
 *
 * @param device_testing_context  The device to be tested.
 */
void show_initial_warning_message(device_testing_context_type *device_testing_context) {
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

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_1);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_2);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_INITIAL_WARNING_PART_3, program_options.device_name);

    snprintf(msg_buffer, sizeof(msg_buffer), warning_text, program_options.device_name, 15);
    window = message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 0);

    if(window) {
        for(i = 0; i < 150; i++) {
            handle_key_inputs(device_testing_context, window);
            usleep(100000);
            if(i && !(i % 10)) {
                delwin(window);
                snprintf(msg_buffer, sizeof(msg_buffer), warning_text, program_options.device_name, 15 - (i / 10));
                window = message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 0);
                wrefresh(window);
            }
        }
    } else {
        sleep(15);
    }

    erase_and_delete_window(window);
}

/**
 * Displays a dialog to the user indicating that an error occurred while opening
 * the log file.  If ncurses is not active, a message is printed to the console
 * instead.
 *
 * @param device_testing_context  The device being tested.
 * @param filename                The path to the log file that was to be
 *                                opened.
 * @param errnum                  The error number of the error that occurred.
 */
void log_file_open_error(device_testing_context_type *device_testing_context, char *filename, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_LOG_FILE_OPEN_ERROR, filename, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open log file %s: %s", filename, strerror(errnum));
    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while opening
 * the lock file.  If ncurses is not active, a message is printed to stdout
 * instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is being shown.)
 * @param errnum                  The error number of the error that occurred.
 */
void lockfile_open_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_LOCK_FILE_OPEN_ERROR, program_options.lock_file, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open lock file %s: %s", program_options.lock_file, strerror(errnum));
    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while opening
 * the stats file.  If ncurses is not active, a message is printed to stdout
 * instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is being shown.)
 * @param errnum                  The error number of the error that occurred.
 */
void stats_file_open_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_STATS_FILE_OPEN_ERROR, program_options.stats_file, strerror(errnum));

    snprintf(msg_buffer, sizeof(msg_buffer), "Unable to open stats file %s: %s", program_options.stats_file, strerror(errnum));
    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while calling
 * gettimeofday().  If ncurses is not active, a message is printed to the
 * console instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is being shown.)
 * @paran errnum                  The error number of the error that occurred.
 */
void no_working_gettimeofday(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_GETTIMEOFDAY_FAILED, strerror(errnum));
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_NO_WORKING_GETTIMEOFDAY);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because your system doesn't "
             "have a working gettimeofday() call.  So many things in this "
             "program depend on this that it would take a lot of work to make "
             "this program work without it, and I'm lazy.\n\nThe error we got "
             "was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Get the number of contiguous sectors, starting with `starting_sector` and
 * going to a max of `max_sectors`, that have not been flagged as "unwritable"
 * in the device's sector map.
 *
 * @param device_testing_context  The device for which the number of writable
 *                                sectors is being requested.
 * @param starting_sector         The sector at which to start counting.
 * @param max_sectors             The maximum number of sectors to count.
 *
 * @returns The number of contiguous sectors, starting from `starting_sector`,
 *          which have not been flagged as "unwritable" in the sector map.  If
 *          starting_sector has been flagged as "unwritable", 0 is returned.
 */
uint64_t get_max_writable_sectors(device_testing_context_type *device_testing_context, uint64_t starting_sector, uint64_t max_sectors) {
    uint64_t out = 0;

    for(out = 0; out < max_sectors && !(device_testing_context->endurance_test_info.sector_map[starting_sector + out] & SECTOR_MAP_FLAG_DO_NOT_USE); out++);
    return out;
}

/**
 * Get the number of contiguous sectors, starting with `starting_sector` and
 * going to a max of `max_sectors`, that have been flagged as "unwritable" in
 * the device's sector map.
 *
 * @param device_testing_context  The device for which the number of unwritable
 *                                sectors is being requested.
 * @param starting_sector         The sector at which to start counting.
 * @param max_sectors             The maximum number of sectors to count.
 *
 * @returns The number of contiguous sectors, starting from `starting_sector`,
 *          which have been flagged as "unwritable" in the sector map.  If
 *          `starting_sector` has not been flagged as "unwritable", 0 is
 *          returned.
 */
uint64_t get_max_unwritable_sectors(device_testing_context_type *device_testing_context, uint64_t starting_sector, uint64_t max_sectors) {
    uint64_t out = 0;

    for(out = 0; out < max_sectors && (device_testing_context->endurance_test_info.sector_map[starting_sector + out] & SECTOR_MAP_FLAG_DO_NOT_USE); out++);
    return out;
}

/**
 * Marks the given sector as unwritable.  No further I/O operations will be
 * attempted on the given sector, and the sector will be flagged as "bad" on all
 * future rounds of endurance testing.
 *
 * @param device_testing_context  The device that contains the unwritable
 *                                sector.
 * @param sector_num              The sector number of the unwritable sector.
 */
void mark_sector_unwritable(device_testing_context_type *device_testing_context, uint64_t sector_num) {
    device_testing_context->endurance_test_info.sector_map[sector_num] |= SECTOR_MAP_FLAG_DO_NOT_USE;
}

/**
 * Reads a block of data from the device, automatically skipping over any
 * sectors that have been flagged as "unwritable".  This function gracefully
 * handles device errors by retrying the operation, resetting the device, and/or
 * marking unreadable sectors as "unwritable".  Unreadable sectors are filled
 * with all zeroes in the returned data.
 *
 * @param device_testing_context  The device from which to read.
 * @param starting_sector         The sector number of the first sector from
 *                                which to read.
 * @param num_sectors             The number of sectors to read from the device.
 * @param buffer                  A pointer to a buffer that will receive the
 *                                data read from the device.
 *
 * @returns 0 if the data was read successfully, or -1 if an error occurred.
 *          Generally speaking, a return value of -1 represents an unrecoverable
 *          error.
 */
int endurance_test_read_block(device_testing_context_type *device_testing_context, uint64_t starting_sector, int num_sectors, char *buffer) {
    int ret, bytes_left_to_read, block_size;
    uint64_t num_sectors_to_read;
    handle_key_inputs(device_testing_context, NULL);
    wait_for_file_lock(device_testing_context, NULL);

    bytes_left_to_read = block_size = device_testing_context->device_info.sector_size * num_sectors;

    while(bytes_left_to_read) {
        // Alternate between reading from the device and filling the buffer with all zeroes
        num_sectors_to_read = get_max_writable_sectors(device_testing_context, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size), bytes_left_to_read / device_testing_context->device_info.sector_size);
        if(num_sectors_to_read) {
            if((ret = read_or_reset_device(device_testing_context,
                                           buffer + (block_size - bytes_left_to_read),
                                           num_sectors_to_read * device_testing_context->device_info.sector_size,
                                           lseek(device_testing_context->device_info.fd, 0, SEEK_CUR))) == -1) {
                if(device_testing_context->device_info.fd == -1) {
                    return -1;
                } else {
                    // Mark this sector as bad and skip over it
                    if(!is_sector_bad(device_testing_context, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size))) {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_READ_ERROR_MARKING_SECTOR_UNUSABLE, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size));
                    }

                    mark_sector_unwritable(device_testing_context, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size));
                    mark_sector_bad(device_testing_context, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size));
                    bytes_left_to_read -= device_testing_context->device_info.sector_size;

                    if((lseek_or_retry(device_testing_context, (starting_sector * device_testing_context->device_info.sector_size) + (block_size - bytes_left_to_read), NULL)) == -1) {
                        // Give up if we can't seek
                        return -1;
                    }

                    continue;
                }
            }

            bytes_left_to_read -= ret;
            device_testing_context->endurance_test_info.screen_counters.bytes_since_last_update += ret;
        }

        if(bytes_left_to_read) {
            num_sectors_to_read = get_max_unwritable_sectors(device_testing_context, starting_sector + ((block_size - bytes_left_to_read) / device_testing_context->device_info.sector_size), bytes_left_to_read / device_testing_context->device_info.sector_size);
            if(num_sectors_to_read) {
                memset(buffer + (block_size - bytes_left_to_read), 0, num_sectors_to_read * device_testing_context->device_info.sector_size);
                bytes_left_to_read -= num_sectors_to_read * device_testing_context->device_info.sector_size;

                // Seek past the unwritable sectors
                if(lseek_or_retry(device_testing_context, (starting_sector * device_testing_context->device_info.sector_size) + (block_size - bytes_left_to_read), NULL) == -1) {
                    return -1;
                }
            }
        }

        print_status_update(device_testing_context);
    }

    return 0;
}

/**
 * Displays a dialog to the user indicating that an error occurred while trying
 * to locate the device described in the state file.  If ncurses is not active,
 * a message is printed to stdout instead.  A message is logged to the log file
 * as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 */
void device_locate_error(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_DEVICE_LOCATE_ERROR);
    message_window(device_testing_context, stdscr, ERROR_TITLE,
                   "An error occurred while trying to locate the device "
                   "described in the state file. (Make sure you're running "
                   "this program as root.)", 1);
}

/**
 * Displays a dialog to the user indicating that a device ambiguity error
 * occurred while locating the device described in the state file.  (A device
 * ambiguity error indicates that more than one device matched the parameters
 * described in the state file.)  If ncurses is not active, a message is printed
 * to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 */
void multiple_matching_devices_error(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_DEVICE_AMBIGUITY_ERROR);
    message_window(device_testing_context, stdscr, ERROR_TITLE,
                   "There are multiple devices that match the data in the "
                   "state file.  Please specify which device you want to "
                   "test on the command line.", 1);
}

/**
 * Displays a dialog to the user indicating that a device was located that
 * matches the parameters described in the state file, but that a device was
 * explicitly provided on the command line and the matched device is not the
 * same as the one specified on the command line.  If ncurses is not active, a
 * message is printed to stdout instead.  A message is logged to the log file as
 * well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 */
void wrong_device_specified_error(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_WRONG_DEVICE_ERROR);
    message_window(device_testing_context, stdscr, ERROR_TITLE,
                   "The device you specified on the command line does not "
                   "match the device described in the state file.  If you run "
                   "this program again without the device name, we'll figure "
                   "out which device to use automatically.  Otherwise, provide "
                   "a different device on the command line.", 1);
}

/**
 * Displays a message to the user indicating that no devices could be found that
 * match the parameters described in the state file.  If ncurses is not active,
 * a message is printed to stdout instead.  A message is logged to the log file
 * as well.
 *
 * @returns A pointer to the window being displayed, or NULL if ncurses is not
 *          active.
 */
WINDOW *no_matching_device_warning(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_DEVICE_NOT_ATTACHED);
    return message_window(NULL, stdscr, "No devices found",
                          "No devices could be found that match the data in "
                          "the state file.  If you haven't plugged the device "
                          "in yet, go ahead and do so now.  Otherwise, you can "
                          "hit Ctrl+C now to abort the program.", 0);
}

/**
 * Displays a dialog to the user indicating that an error occurred while waiting
 * for a device matching the parameters described in the state file to be
 * connected.  If ncurses is not active, a message is printed to stdout instead.
 * A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 * @param window                  A pointer to the window currently being
 *                                displayed on screen, or NULL if no window is
 *                                currently being displayed.  The provided
 *                                window is erased and deleted before this
 *                                function's message is displayed.
 */
void wait_for_device_connect_error(device_testing_context_type *device_testing_context, WINDOW *window) {
    if(window) {
        erase_and_delete_window(window);
    }

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_WAIT_FOR_DEVICE_RECONNECT_ERROR);
    message_window(device_testing_context, stdscr, ERROR_TITLE, "An error occurred while waiting for you to reconnect the device.", 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while calling
 * fstat() for the given device.  If ncurses is not active, a message is printed
 * to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 * @param errnum                  The error number of the error that occurred.
 */
void fstat_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test %s because we weren't able to pull stats"
             "on it.  The device may have been removed, or you may not have "
             "permissions to open it.  (Make sure you're running this program "
             "as root.)\n\nThe error we got was: %s", device_testing_context->device_info.device_name, strerror(errnum));
    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while calling
 * stat() for the given device.  If ncurses is not active, a message is printed
 * to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 * @param errnum                  The error number of the error that occurred.
 */
void stat_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we were unable to "
             "pull stats on it.  The device may have been removed, or you may "
             "not have permissions to open it.  (Make sure you're running this "
             "program as root.)\n\nThe error we got was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that the device they specified on
 * the command line is not a block device.  If ncurses is not active, a message
 * is printed to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 */
void not_a_block_device_error(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_NOT_A_BLOCK_DEVICE, device_testing_context->device_info.device_name);
    message_window(device_testing_context, stdscr, ERROR_TITLE, "We won't be able to test this device because it isn't a block device.  You must provide a block device to test with.", 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while
 * attempting to open the specified device.  If ncurses is not active, a message
 * is printed to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 * @param errnum                  The error number of the error that occurred.
 */
void device_open_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OPEN_DEVICE);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we couldn't open "
             "it.  The device might have gone away, or you might not have "
             "permission to open it.  (Make sure you run this program as "
             "root.)\n\nHere's the error was got: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while calling
 * ioctl() on the given device.  If ncurses is not active, a message is printed
 * to stdout instead.  A message is logged to the log file as well.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 * @param errnum                  The error number of the error that occurred.
 */
void ioctl_error(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_UNABLE_TO_OBTAIN_DEVICE_INFO);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We won't be able to test this device because we couldn't pull "
             "stats on it.\n\nHere's the error we got: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

/**
 * Displays a dialog to the user indicating that an error occurred while saving
 * the program state.  If ncurses is not active, a message is printed to stdout
 * instead.  A message is logged to the log file as well.  Disables save stating
 * by freeing program_options.state_file and then setting it to NULL.
 *
 * @param device_testing_context  The device being tested.  (This is needed in
 *                                case the screen needs to be redrawn while the
 *                                dialog is active.)
 */
void save_state_error(device_testing_context_type *device_testing_context) {
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_SAVE_STATE_ERROR);
    message_window(device_testing_context, stdscr, WARNING_TITLE, "An error occurred while trying to save the program state.  Save stating has been disabled.", 1);

    free(program_options.state_file);
    program_options.state_file = NULL;
}

/**
 * Determines whether the given byte position falls within the device's
 * beginning-of-device area.
 *
 * @param starting_byte  The byte position to query.
 *
 * @returns Non-zero if the given byte position falls within the device's
 *          beginning-of-device area, or zero if it does not.
 */
int was_bod_area_affected(device_testing_context_type *device_testing_context, uint64_t starting_byte) {
    return starting_byte < device_testing_context->device_info.bod_mod_buffer_size;
}

/**
 * Determines whether the given byte range intersects with the device's
 * middle-of-device area.
 *
 * @param device_testing_context  The device being queried.
 * @param starting_byte           The byte position of the start of the range
 *                                being queried.
 * @param ending_byte             The byte position of the end of the range
 *                                being queried.
 *
 * @returns Non-zero if the given byte range intersects the device's
 *          middle-of-device area, or zero if it does not.
 */
int was_mod_area_affected(device_testing_context_type *device_testing_context, uint64_t starting_byte, uint64_t ending_byte) {
    return (starting_byte >= device_testing_context->device_info.middle_of_device && starting_byte < (device_testing_context->device_info.middle_of_device + device_testing_context->device_info.bod_mod_buffer_size)) || (ending_byte >= device_testing_context->device_info.middle_of_device && ending_byte < (device_testing_context->device_info.middle_of_device + device_testing_context->device_info.bod_mod_buffer_size));
}

/**
 * Update the beginning-of-device buffer if a write operation at the given
 * starting_byte would have affected the data in the BOD area.
 *
 * @param device_testing_context  The device whose BOD buffer should be updated.
 * @param starting_byte           The starting location at which the write to
 *                                the device occurred (relative to the start of
 *                                the device).
 * @param buffer                  A buffer containing the data that was written
 *                                to the device.
 * @param num_bytes               The number of bytes that were written to the
 *                                device.
 */
void update_bod_buffer(device_testing_context_type *device_testing_context, uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    uint64_t bytes_to_copy;

    if(was_bod_area_affected(device_testing_context, starting_byte)) {
        bytes_to_copy = num_bytes;

        if(num_bytes + starting_byte > device_testing_context->device_info.bod_mod_buffer_size) {
            bytes_to_copy = device_testing_context->device_info.bod_mod_buffer_size - starting_byte;
        }

        memcpy(device_testing_context->device_info.bod_buffer + starting_byte, buffer, bytes_to_copy);

        if(save_state(device_testing_context)) {
            save_state_error(device_testing_context);
        }
    }
}

/**
 * Update the middle-of-device buffer if a write operation at the given
 * starting_byte would have affected the data in the MOD area.
 *
 * @param device_testing_context  The device whose MOD buffer needs to be
 *                                updated.
 * @param starting_byte           The starting location at which the write to
 *                                the device occurred (relative to the start of
 *                                the device).
 * @param buffer                  A buffer containing the data that was written
 *                                to the device.
 * @param num_bytes               The number of bytes that were written to the
 *                                device.
 */
void update_mod_buffer(device_testing_context_type *device_testing_context, uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    uint64_t bytes_to_copy;
    uint64_t buffer_offset = 0;
    char *mod_position = device_testing_context->device_info.mod_buffer;

    if(was_mod_area_affected(device_testing_context, starting_byte, starting_byte + num_bytes - 1)) {
        bytes_to_copy = num_bytes;

        if(starting_byte < device_testing_context->device_info.middle_of_device) {
            buffer_offset = device_testing_context->device_info.middle_of_device - starting_byte;
            bytes_to_copy -= buffer_offset;

            if(bytes_to_copy > device_testing_context->device_info.bod_mod_buffer_size) {
                bytes_to_copy = device_testing_context->device_info.bod_mod_buffer_size;
            }
        } else {
            mod_position += starting_byte - device_testing_context->device_info.middle_of_device;
            if((starting_byte - device_testing_context->device_info.middle_of_device) + num_bytes > device_testing_context->device_info.bod_mod_buffer_size) {
                bytes_to_copy = device_testing_context->device_info.bod_mod_buffer_size - (starting_byte - device_testing_context->device_info.middle_of_device);
            }
        }

        memcpy(mod_position, buffer + buffer_offset, bytes_to_copy);

        if(save_state(device_testing_context)) {
            save_state_error(device_testing_context);
        }
    }
}

/**
 * Update the beginning-of-device or middle-of-device buffer if a write
 * operation at the given starting_byte would have affected the data in the BOD
 * or MOD areas.
 *
 * @param device_testing_context  The device whose BOD or MOD buffers need to be
 *                                updated.
 * @param starting_byte           The starting location at which the write to
 *                                the device occurred (relative to the start of
 *                                the device).
 * @param buffer                  A buffer containing the data that was written.
 * @param num_bytes               The number of bytes that were written to the
 *                                device.
 */
void update_bod_mod_buffers(device_testing_context_type *device_testing_context, uint64_t starting_byte, void *buffer, uint64_t num_bytes) {
    update_bod_buffer(device_testing_context, starting_byte, buffer, num_bytes);
    update_mod_buffer(device_testing_context, starting_byte, buffer, num_bytes);
}

/**
 * Writes a block of data to the device for the endurance test, skipping over
 * any sectors that are flagged as "unwritable".  This function gracefully
 * handles device errors by retrying the operation, resetting the device, and/or
 * marking unwritable sectors as "unwritable".
 *
 * @param device_testing_context   The device to which to write.
 * @param starting_sector          The starting sector at which to start writing
 *                                 the data to the device.
 * @param num_sectors              The number of sectors to write to the device.
 * @param buffer                   A pointer to a buffer containing the data to
 *                                 be written to the device.
 * @param device_was_disconnected  A pointer to a variable indicating whether a
 *                                 device disconnect was encountered during the
 *                                 operation.  If a device disconnect is
 *                                 encountered, this variable will be set to 1;
 *                                 if the operation completed without a
 *                                 disconnect, this variable will be set to 0.
 *
 * @returns 0 if the operation completed successfully or if the device
 * disconnected during the operation.  If an error occurred, one of the
 * ABORT_REASON_* codes is returned instead.  Note that if an error occurs that
 * caused the device to disconnect and the device could not be reconnected,
 * device_testing_context->device_info.fd is set to -1.
 */
int endurance_test_write_block(device_testing_context_type *device_testing_context, uint64_t starting_sector, int num_sectors, char *buffer, int *device_was_disconnected) {
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

    num_bytes_remaining = num_sectors * device_testing_context->device_info.sector_size;
    *device_was_disconnected = 0;
    ret = 0;

    starting_byte = starting_sector * device_testing_context->device_info.sector_size;

    while(num_bytes_remaining && !*device_was_disconnected) {
        handle_key_inputs(device_testing_context, NULL);
        wait_for_file_lock(device_testing_context, NULL);

        num_sectors_remaining = num_bytes_remaining / device_testing_context->device_info.sector_size;
        num_sectors_written = num_sectors - num_sectors_remaining;
        num_bytes_written = num_sectors_written * device_testing_context->device_info.sector_size;
        current_sector = starting_sector + num_sectors_written;
        current_byte = current_sector * device_testing_context->device_info.sector_size;

        if(num_sectors_to_write = get_max_writable_sectors(device_testing_context, current_sector, num_sectors_remaining)) {
            num_bytes_to_write = num_sectors_to_write * device_testing_context->device_info.sector_size;
            if((ret = write_or_reset_device(device_testing_context, buffer + num_bytes_written, num_bytes_to_write, current_byte, device_was_disconnected)) == -1) {
                if(device_testing_context->device_info.fd == -1) {
                    // The device has disconnected, and attempts to wait
                    // for it to reconnect have failed
                    return ABORT_REASON_WRITE_ERROR;
                } else {
                    // Mark this sector bad and skip over it
                    if(!is_sector_bad(device_testing_context, current_sector)) {
                        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_WRITE_ERROR_SECTOR_UNUSABLE, current_sector);
                        device_testing_context->endurance_test_info.num_new_bad_sectors_this_round++;
                    }

                    mark_sector_unwritable(device_testing_context, current_sector);
                    mark_sector_bad(device_testing_context, current_sector);
                    device_testing_context->endurance_test_info.num_bad_sectors_this_round++;

                    num_bytes_remaining -= device_testing_context->device_info.sector_size;

                    if((lseek_or_retry(device_testing_context, current_byte + device_testing_context->device_info.sector_size, device_was_disconnected)) == -1) {
                        // Give up if we can't seek
                        return ABORT_REASON_SEEK_ERROR;
                    }

                    continue;
                }
            } else {
                num_bytes_written += ret;
                num_bytes_remaining -= ret;
                num_sectors_written = num_bytes_written / device_testing_context->device_info.sector_size;
                num_sectors_remaining = num_sectors - num_sectors_written;
                num_sectors_affected_this_round = num_sectors_written;
                num_bytes_affected_this_round = num_sectors_affected_this_round * device_testing_context->device_info.sector_size;
                current_sector = starting_sector + num_sectors_written;
            }
        }

        // If the device disconnected during the write operation, we want to
        // give up right away so that we can restart the slice.
        if(*device_was_disconnected) {
            break;
        }

        if(num_sectors_to_write = get_max_unwritable_sectors(device_testing_context, current_sector, num_sectors_remaining)) {
            // Seek past the bad sectors
            num_sectors_remaining -= num_sectors_to_write;
            num_bytes_remaining -= num_sectors_to_write * device_testing_context->device_info.sector_size;
            num_sectors_written += num_sectors_to_write;
            num_bytes_written += num_sectors_to_write * device_testing_context->device_info.sector_size;
            num_sectors_affected_this_round += num_sectors_to_write;
            num_bytes_affected_this_round = num_sectors_affected_this_round * device_testing_context->device_info.sector_size;

            if(lseek_or_retry(device_testing_context, starting_byte + num_bytes_written, device_was_disconnected) == -1) {
                return ABORT_REASON_SEEK_ERROR;
            }
        }

        // Update the BOD and MOD buffers if necessary
        update_bod_mod_buffers(device_testing_context, current_byte, buffer + (current_byte - starting_byte), num_bytes_affected_this_round);
        device_testing_context->endurance_test_info.screen_counters.bytes_since_last_update += ret;
        device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written += ret;

        print_status_update(device_testing_context);
    }

    return 0;
}

/**
 * Embed sector number, round number, UUID, and CRC32 data into the the data in
 * the given buffer.  The buffer is modified in place.
 *
 * @param device_testing_context  The device being tested.
 * @param buffer                  A buffer containing the data to be written to
 *                                the sector (or verified against the sector
 *                                contents).
 * @param num_sectors             The number of sectors worth of data available
 *                                in the buffer.
 * @param starting_sector         The sector number of the first sector
 *                                represented by the buffer.
 */
void prepare_endurance_test_block(device_testing_context_type *device_testing_context, char *buffer, int num_sectors, uint64_t starting_sector) {
    int i;
    // We'll embed some information into the data to try to detect
    // various types of errors:
    //  - Sector number (to detect address decoding errors),
    //  - Round number (to detect failed writes),
    //  - Device UUID (to detect cross-device reads)
    //  - CRC32 (to detect bit flip errors)
    for(i = 0; i < num_sectors; i++) {
        embed_sector_number(buffer + (i * device_testing_context->device_info.sector_size), starting_sector + i);
        embed_round_number(buffer + (i * device_testing_context->device_info.sector_size), device_testing_context->endurance_test_info.rounds_completed);
        embed_device_uuid(device_testing_context->device_info.device_uuid, buffer + (i * device_testing_context->device_info.sector_size));
        embed_crc32c(buffer + (i * device_testing_context->device_info.sector_size), device_testing_context->device_info.sector_size);
    }
}

/**
 * Writes random data to a slice of the device.  Sector number, round number,
 * and device UUID are embedded in the random data.
 *
 * @param device_testing_context  The device to which to write.
 * @param rng_seed                The seed that should be used to initialize the
 *                                RNG when generating data to write to the device.
 * @param slice_num               The slice number of the current slice.
 * @param num_sectors             The number of sectors per slice.  If the
 *                                number of sectors would cause the write to go
 *                                past the end of the device, the write is
 *                                automatically truncated to fit the remaining
 *                                space on the deivce.
 *
 * @returns 0 if the write completed successfully, -1 if an error occurred (not
 *          related to the device), or one of the ABORT_REASON_* codes if an
 *          unrecoverable error occurred.
 */
int endurance_test_write_slice(device_testing_context_type *device_testing_context, unsigned int rng_seed, uint64_t slice_num, uint64_t num_sectors) {
    uint64_t cur_sector, last_sector, cur_block_size, sectors_in_cur_block, bytes_left_to_write, i, num_sectors_to_write, sectors_per_block;
    int device_was_disconnected, ret;
    sql_thread_status_type prev_sql_thread_status = sql_thread_status;
    char *write_buffer;

    if(ret = posix_memalign((void **) &write_buffer, sysconf(_SC_PAGESIZE), device_testing_context->device_info.optimal_block_size)) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        malloc_error(device_testing_context, ret);
        return -1;
    }

    sectors_per_block = device_testing_context->device_info.optimal_block_size / device_testing_context->device_info.sector_size;

    // Set last_sector to one sector past the last sector we should touch this slice
    if(slice_num == (NUM_SLICES - 1)) {
        last_sector = device_testing_context->device_info.num_physical_sectors;
    } else {
        last_sector = get_slice_start(device_testing_context, slice_num + 1);
    }

    do {
        device_was_disconnected = 0;
        rng_reseed(device_testing_context, rng_seed);

        if(lseek_or_retry(device_testing_context, get_slice_start(device_testing_context, slice_num) * device_testing_context->device_info.sector_size, &device_was_disconnected) == -1) {
            free(write_buffer);
            return ABORT_REASON_SEEK_ERROR;
        }

        for(cur_sector = get_slice_start(device_testing_context, slice_num); cur_sector < last_sector && !device_was_disconnected; cur_sector += sectors_in_cur_block) {
            if(sql_thread_status != prev_sql_thread_status) {
                prev_sql_thread_status = sql_thread_status;
                print_sql_status(sql_thread_status);
            }

            if((cur_sector + sectors_per_block) > last_sector) {
                sectors_in_cur_block = last_sector - cur_sector;
                cur_block_size = sectors_in_cur_block * device_testing_context->device_info.sector_size;
            } else {
                sectors_in_cur_block = sectors_per_block;
                cur_block_size = sectors_in_cur_block * device_testing_context->device_info.sector_size;
            }

            rng_fill_buffer(device_testing_context, write_buffer, cur_block_size);
            bytes_left_to_write = cur_block_size;

            prepare_endurance_test_block(device_testing_context, write_buffer, sectors_in_cur_block, cur_sector);

            handle_key_inputs(device_testing_context, NULL);
            wait_for_file_lock(device_testing_context, NULL);

            ret = endurance_test_write_block(device_testing_context, cur_sector, sectors_in_cur_block, write_buffer, &device_was_disconnected);
            if(ret == -1) {
                free(write_buffer);
                return ABORT_REASON_WRITE_ERROR;
            }

            if(device_was_disconnected) {
                // Unmark the sectors we've written in this slice so far
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_RESTARTING_SLICE);
                reset_sector_map_partial(device_testing_context, get_slice_start(device_testing_context, slice_num), last_sector);
                redraw_sector_map(device_testing_context);
            } else {
                mark_sectors_written(device_testing_context, cur_sector, cur_sector + sectors_in_cur_block);

                assert(!gettimeofday(&stats_cur_time, NULL));
                if(timediff(device_testing_context->endurance_test_info.stats_file_counters.last_update_time, stats_cur_time) >= (program_options.stats_interval * 1000000)) {
                    stats_log(device_testing_context);
                }
            }

            refresh();
        }
    } while(device_was_disconnected);

    free(write_buffer);
    return 0;
}

/**
 * Probes the device for basic information about the device.  Device information
 * is output to the log file and placed into the provided device testing
 * context.  The logical device size is also drawn on the screen; the screen is
 * refreshed afterwards.
 *
 * @param device_testing_context  The device to be probed.
 *
 * @returns 0 if the device probe completed successfully, or -1 if an error
 *          occurred.
 */
int probe_device_info(device_testing_context_type *device_testing_context) {
    struct stat fs;
    unsigned int physical_sector_size;

    // We're doing this as separate if statements so that if one of them errors,
    // errno doesn't potentially get overwritten by the other calls.
    if(ioctl(device_testing_context->device_info.fd, BLKGETSIZE64, &device_testing_context->device_info.logical_size)) {
        return -1;
    }

    if(ioctl(device_testing_context->device_info.fd, BLKSSZGET, &device_testing_context->device_info.sector_size)) {
        return -1;
    }

    if(ioctl(device_testing_context->device_info.fd, BLKSECTGET, &device_testing_context->device_info.max_sectors_per_request)) {
        return -1;
    }

    if(ioctl(device_testing_context->device_info.fd, BLKPBSZGET, &physical_sector_size)) {
        return -1;
    }

    device_testing_context->device_info.device_num = fs.st_rdev;
    device_testing_context->device_info.num_logical_sectors = device_testing_context->device_info.logical_size / device_testing_context->device_info.sector_size;

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_HEADER);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_REPORTED_SIZE, device_testing_context->device_info.logical_size);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_LOGICAL_SECTOR_SIZE, device_testing_context->device_info.sector_size);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_PHYSICAL_SECTOR_SIZE, physical_sector_size);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_TOTAL_SECTORS, device_testing_context->device_info.num_logical_sectors);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_PREFERRED_BLOCK_SIZE, fs.st_blksize);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_DEVICE_INFO_MAX_SECTORS_PER_REQUEST, device_testing_context->device_info.max_sectors_per_request);

    mvprintw(REPORTED_DEVICE_SIZE_DISPLAY_Y, REPORTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_testing_context->device_info.logical_size);
    refresh();

    return 0;
}

/**
 * Prints the end of round summary to the log file, and checks to see if we've
 * passed any of the major thresholds.
 *
 * @param device_testing_context  The device whose summary should be logged.
 */
void perform_end_of_round_summary(device_testing_context_type *device_testing_context) {
    if(!device_testing_context->endurance_test_info.num_new_bad_sectors_this_round && !device_testing_context->endurance_test_info.total_bad_sectors) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUND_COMPLETE_NO_BAD_SECTORS, device_testing_context->endurance_test_info.rounds_completed + 1);
    } else {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_ROUND_COMPLETE_WITH_BAD_SECTORS, device_testing_context->endurance_test_info.rounds_completed + 1);
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_BAD_SECTORS_THIS_ROUND, device_testing_context->endurance_test_info.num_bad_sectors_this_round);
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_NEW_BAD_SECTORS_THIS_ROUND, device_testing_context->endurance_test_info.num_new_bad_sectors_this_round);
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_PREVIOUSLY_BAD_SECTORS_NOW_GOOD, device_testing_context->endurance_test_info.num_good_sectors_this_round);
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_TOTAL_BAD_SECTORS, device_testing_context->endurance_test_info.total_bad_sectors, (((double) device_testing_context->endurance_test_info.total_bad_sectors) / ((double) device_testing_context->device_info.num_physical_sectors)) * 100);

        // Check to see if we passed any failure thresholds
        if(device_testing_context->endurance_test_info.rounds_to_first_error == -1ULL &&
           device_testing_context->endurance_test_info.total_bad_sectors) {
            device_testing_context->endurance_test_info.rounds_to_first_error = device_testing_context->endurance_test_info.rounds_completed;
        }

        if(device_testing_context->endurance_test_info.rounds_to_10_threshold == -1ULL &&
           device_testing_context->endurance_test_info.total_bad_sectors >= device_testing_context->endurance_test_info.sectors_to_10_threshold) {
            device_testing_context->endurance_test_info.rounds_to_10_threshold = device_testing_context->endurance_test_info.rounds_completed;
        }

        if(device_testing_context->endurance_test_info.rounds_to_25_threshold == -1ULL &&
           device_testing_context->endurance_test_info.total_bad_sectors >= device_testing_context->endurance_test_info.sectors_to_25_threshold) {
            device_testing_context->endurance_test_info.rounds_to_25_threshold = device_testing_context->endurance_test_info.rounds_completed;
        }
    }
}

int main(int argc, char **argv) {
    int cur_block_size, local_errno, restart_slice, state_file_status;
    struct stat fs;
    uint64_t bytes_left_to_write, ret, cur_sector;
    unsigned int sectors_per_block;
    char *buf, *compare_buf, *zero_buf, *ff_buf;
    struct timeval speed_start_time;
    struct timeval rng_init_time;
    uint64_t cur_sectors_per_block, last_sector;
    uint64_t cur_slice, j;
    int *read_order;
    int device_was_disconnected;
    int iret;
    char device_uuid_str[37];
    uuid_t device_uuid_from_device;
    WINDOW *window;
    pthread_t sql_thread;
    sql_thread_params_type sql_thread_params;
    sql_thread_status_type prev_sql_thread_status = 0;
    int num_uuid_mismatches, device_mangling_detected;
    device_search_params_t device_search_params;
    device_search_result_t *device_search_result;
    device_testing_context_type *device_testing_context;

    // Set things up so that cleanup() works properly
    ncurses_active = 0;
    buf = NULL;
    compare_buf = NULL;
    zero_buf = NULL;
    ff_buf = NULL;
    read_order = NULL;
    program_options.lock_file = NULL;
    program_options.state_file = NULL;
    forced_device = NULL;
    device_testing_context = NULL;

    void cleanup() {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_PROGRAM_ENDING);

        close_lockfile();

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

        delete_device_testing_context(device_testing_context);
    }

    speed_qualifications_shown = 0;
    sector_display.sectors_per_block = 0;
    main_thread_status = MAIN_THREAD_STATUS_IDLE;

    if(parse_command_line_arguments(argc, argv)) {
        return -1;
    }

    // Create a new device testing context
    if(!(device_testing_context = new_device_testing_context(BOD_MOD_BUFFER_SIZE))) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR);
        return -1;
    }

    // Recompute num_sectors now so that we don't crash when we call redraw_screen
    if((state_file_status = load_state(device_testing_context)) == LOAD_STATE_SUCCESS) {
        device_testing_context->device_info.num_physical_sectors = device_testing_context->device_info.physical_size / device_testing_context->device_info.sector_size;
    }

    // If the user didn't specify a curses option on the command line, then use
    // what's in the state file.
    if(!program_options.no_curses) {
        program_options.no_curses = program_options.orig_no_curses;
    }

    // If stdout isn't a tty (e.g., if output is being redirected to a file),
    // then we should turn off the ncurses routines.
    if(!program_options.no_curses && !isatty(1)) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_NCURSES_STDOUT_NOT_A_TTY);
        program_options.no_curses = 1;
    }

    // Initialize ncurses
    if(!program_options.no_curses) {
        if(screen_setup()) {
            log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_NCURSES_TERMINAL_TOO_SMALL);
            program_options.no_curses = 1;
        } else {
            redraw_screen(device_testing_context);
        }
    }

    if(state_file_status == LOAD_STATE_LOAD_ERROR) {
        state_file_error(device_testing_context);
        state_file_status = LOAD_STATE_FILE_DOES_NOT_EXIST;
    }

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        device_info_set_device_name(device_testing_context, program_options.device_name);
        print_device_name(device_testing_context);

        if(!program_options.dont_show_warning_message) {
            show_initial_warning_message(device_testing_context);
        }
    }

    // If a log file was specified on the command line, copy it to the device
    // testing context.
    if(program_options.log_file) {
        // If a log file already exists in the device testing context, let the
        // one on the command line override it.
        if(device_testing_context->log_file_name) {
            free(device_testing_context->log_file_name);
        }

        if(!(device_testing_context->log_file_name = strdup(program_options.log_file))) {
            log_file_open_error(device_testing_context, program_options.log_file, errno);
            cleanup();
            return -1;
        }
    }

    if(device_testing_context->log_file_name) {
        if(!(device_testing_context->log_file_handle = fopen(device_testing_context->log_file_name, "a"))) {
            log_file_open_error(device_testing_context, device_testing_context->log_file_name, errno);
            cleanup();
            return -1;
        }
    }

    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_PROGRAM_STARTING, VERSION);

    if(state_file_status == LOAD_STATE_SUCCESS) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_RESUMING_FROM_STATE_FILE, program_options.state_file);
    }

    if(iret = open_lockfile(device_testing_context, program_options.lock_file)) {
        lockfile_open_error(device_testing_context, iret);
        cleanup();
        return -1;
    }

    if(program_options.stats_file) {
        if(!(device_testing_context->endurance_test_info.stats_file_handle = fopen(program_options.stats_file, "a"))) {
            stats_file_open_error(device_testing_context, errno);
            cleanup();
            return -1;
        }

        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_LOGGING_STATS_TO_FILE, program_options.stats_file);

        // Write the CSV headers out to the file, but only if we're not
        // resuming from a state file
        if(state_file_status != LOAD_STATE_SUCCESS) {
            fprintf(device_testing_context->endurance_test_info.stats_file_handle,
                    "Date/Time,Rounds Completed,Bytes Written,Total Bytes Written,Write Rate (bytes/sec),Bytes Read,Total Bytes Read,Read Rate (bytes/sec),Bad Sectors,Total Bad Sesctors,Bad Sector Rate (counts/min)\n");
            fflush(device_testing_context->endurance_test_info.stats_file_handle);
        }
    }

    // Does the system have a working gettimeofday?
    if(gettimeofday(&speed_start_time, NULL) == -1) {
        no_working_gettimeofday(device_testing_context, errno);
        cleanup();
        return -1;
    }

    if(state_file_status == LOAD_STATE_SUCCESS && !forced_device) {
        // State file was loaded successfully, try to find the device described in the state file
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_FINDING_DEVICE_FROM_STATE_FILE);
        window = message_window(device_testing_context, stdscr, NULL, "Finding device described in state file...", 0);

        device_search_params.preferred_dev_name = program_options.device_name;
        device_search_params.must_match_preferred_dev_name = 0;

        ret = find_device(device_testing_context, &device_search_params);

        erase_and_delete_window(window);

        if(ret) {
            if(errno == ENOTUNIQ) {
                // Multiple matching devices found
                multiple_matching_devices_error(device_testing_context);
                cleanup();
                return -1;
            } else if(errno == ENODEV) {
                // No matching device found
                if(program_options.device_name) {
                    // ...and a device was specified on the command line
                    wrong_device_specified_error(device_testing_context);
                    cleanup();
                    return -1;
                } else {
                    // ...and no device was specified on the command line -- wait for it to be connected
                    window = no_matching_device_warning(device_testing_context);

                    device_search_params.preferred_dev_name = NULL;
                    device_search_params.must_match_preferred_dev_name = 0;

                    if(!(device_search_result = wait_for_device_reconnect(device_testing_context, &device_search_params))) {
                        // An error occurred while waiting for the device to be connected
                        wait_for_device_connect_error(device_testing_context, window);
                        cleanup();
                        return -1;
                    } else {
                        // Device was reconnected -- copy the device name to the device testing context
                        if(device_info_set_device_name(device_testing_context, device_search_result->device_name)) {
                            local_errno = errno;
                            log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, local_errno);
                            malloc_error(device_testing_context, local_errno);
                            cleanup();
                            return -1;
                        }

                        if(!(program_options.device_name = strdup(device_search_result->device_name))) {
                            local_errno = errno;
                            log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, local_errno);
                            malloc_error(device_testing_context, errno);
                            cleanup();
                            return -1;
                        }

                        device_testing_context->device_info.device_num = device_search_result->device_num;
                        device_testing_context->device_info.fd = device_search_result->fd;

                        free_device_search_result(device_search_result);
                        erase_and_delete_window(window);
                    }
                }
            } else {
                device_locate_error(device_testing_context);
                cleanup();
                return -1;
            }
        } else {
            if(program_options.device_name) {
                free(program_options.device_name);
            }

            if(!(program_options.device_name = strdup(device_testing_context->device_info.device_name))) {
                local_errno = errno;
                log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, local_errno);
                malloc_error(device_testing_context, local_errno);
                cleanup();
                return -1;
            }

        }

        if(fstat(device_testing_context->device_info.fd, &fs)) {
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_FSTAT_ERROR, strerror(errno));
            fstat_error(device_testing_context, errno);
            cleanup();
            return -1;
        }
    } else {
        if(forced_device) {
            if(state_file_status != LOAD_STATE_SUCCESS) {
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_IGNORING_FORCED_DEVICE);
            } else {
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_USING_FORCED_DEVICE);

                if(program_options.device_name) {
                    free(program_options.device_name);
                }

                program_options.device_name = forced_device;

                if(device_info_set_device_name(device_testing_context, forced_device)) {
                    local_errno = errno;
                    log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, local_errno);
                    malloc_error(device_testing_context, local_errno);
                    cleanup();
                    return -1;
                }

                forced_device = NULL;
            }
        }

        if((ret = is_block_device(device_testing_context->device_info.device_name)) == -1) {
            local_errno = errno;
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_STAT_ERROR, strerror(local_errno));
            stat_error(device_testing_context, local_errno);
            cleanup();
            return -1;
        } else if(!ret) {
            not_a_block_device_error(device_testing_context);
            cleanup();
            return -1;
        }

        if((device_testing_context->device_info.fd = open(device_testing_context->device_info.device_name, O_DIRECT | O_SYNC | O_LARGEFILE | O_RDWR)) == -1) {
            local_errno = errno;
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_OPEN_ERROR, strerror(local_errno));
            device_open_error(device_testing_context, local_errno);
            cleanup();
            return -1;
        }
    }

    if(probe_device_info(device_testing_context)) {
        local_errno = errno;
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_IOCTL_ERROR, strerror(local_errno));
        ioctl_error(device_testing_context, local_errno);
        cleanup();
        return -1;
    }

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        profile_random_number_generator(device_testing_context);

        if(program_options.probe_for_optimal_block_size) {
            wait_for_file_lock(device_testing_context, NULL);

            if(probe_for_optimal_block_size(device_testing_context)) {
                device_testing_context->device_info.optimal_block_size = device_testing_context->device_info.sector_size * device_testing_context->device_info.max_sectors_per_request;
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_UNABLE_TO_PROBE_FOR_OPTIMAL_BLOCK_SIZE, device_testing_context->device_info.optimal_block_size);
            } else {
                device_testing_context->device_info.optimal_block_size = device_testing_context->optimal_block_size_test_info.optimal_block_size;
            }
        } else {
            device_testing_context->device_info.optimal_block_size = device_testing_context->device_info.sector_size * device_testing_context->device_info.max_sectors_per_request;
        }

        sectors_per_block = device_testing_context->device_info.optimal_block_size / device_testing_context->device_info.sector_size;

        wait_for_file_lock(device_testing_context, NULL);

        if(program_options.force_sectors) {
            device_testing_context->device_info.num_physical_sectors = program_options.force_sectors;
            device_testing_context->device_info.physical_size = program_options.force_sectors * device_testing_context->device_info.sector_size;

            log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_USING_FORCED_DEVICE_SIZE, device_testing_context->device_info.physical_size);

            if(device_testing_context->device_info.physical_size == device_testing_context->device_info.logical_size) {
                device_testing_context->device_info.is_fake_flash = FAKE_FLASH_NO;
            } else {
                device_testing_context->device_info.is_fake_flash = FAKE_FLASH_YES;
            }

            if(!program_options.no_curses) {
                mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_testing_context->device_info.physical_size);
                if(device_testing_context->device_info.physical_size != device_testing_context->device_info.logical_size) {
                    attron(COLOR_PAIR(RED_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Yes");
                    attroff(COLOR_PAIR(RED_ON_BLACK));
                } else {
                    attron(COLOR_PAIR(GREEN_ON_BLACK));
                    mvprintw(IS_FAKE_FLASH_DISPLAY_Y, IS_FAKE_FLASH_DISPLAY_X, "Probably not");
                    attroff(COLOR_PAIR(GREEN_ON_BLACK));
                }
            }
        } else if(probe_device_size(device_testing_context)) {
            log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_USING_KERNEL_REPORTED_DEVICE_SIZE, device_testing_context->device_info.logical_size);

            device_testing_context->device_info.num_physical_sectors = device_testing_context->device_info.logical_size / device_testing_context->device_info.sector_size;

            if(!program_options.no_curses) {
                mvaddstr(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "Unknown");
                mvaddstr(IS_FAKE_FLASH_DISPLAY_Y       , IS_FAKE_FLASH_DISPLAY_X       , "Unknown");
            }
        } else {
            device_testing_context->device_info.physical_size = device_testing_context->capacity_test_info.device_size;
            device_testing_context->device_info.num_physical_sectors = device_testing_context->capacity_test_info.num_sectors;
            device_testing_context->device_info.is_fake_flash = device_testing_context->capacity_test_info.is_fake_flash;

            if(!program_options.no_curses) {
                mvprintw(DETECTED_DEVICE_SIZE_DISPLAY_Y, DETECTED_DEVICE_SIZE_DISPLAY_X, "%'lu bytes", device_testing_context->device_info.physical_size);
                if(device_testing_context->device_info.physical_size != device_testing_context->device_info.logical_size) {
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

        device_testing_context->device_info.middle_of_device = device_testing_context->device_info.physical_size / 2;

        refresh();

        wait_for_file_lock(device_testing_context, NULL);

        probe_device_speeds(device_testing_context);
    } else {
        device_testing_context->device_info.is_fake_flash = (device_testing_context->device_info.logical_size == device_testing_context->device_info.physical_size) ? FAKE_FLASH_NO : FAKE_FLASH_YES;
        sectors_per_block = device_testing_context->device_info.optimal_block_size / device_testing_context->device_info.sector_size;
        device_testing_context->device_info.middle_of_device = device_testing_context->device_info.physical_size / 2;
        redraw_screen(device_testing_context);
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
    device_testing_context->endurance_test_info.rng_state.initial_seed = rng_init_time.tv_sec + rng_init_time.tv_usec;
    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        device_testing_context->endurance_test_info.rounds_to_first_error = device_testing_context->endurance_test_info.rounds_to_10_threshold =
            device_testing_context->endurance_test_info.rounds_to_25_threshold = -1ULL;
    }

    rng_init(device_testing_context, device_testing_context->endurance_test_info.rng_state.initial_seed);

    // Allocate buffers for reading from/writing to the device.  We're using
    // posix_memalign because the memory needs to be aligned on a page boundary
    // (since we're doing unbuffered reading/writing).
    if(ret = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), device_testing_context->device_info.optimal_block_size)) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        malloc_error(device_testing_context, ret);
        cleanup();
        return -1;
    }

    if(ret = posix_memalign((void **) &compare_buf, sysconf(_SC_PAGESIZE), device_testing_context->device_info.optimal_block_size)) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_POSIX_MEMALIGN_ERROR, strerror(ret));
        malloc_error(device_testing_context, ret);
        cleanup();
        return -1;
    }

    // Flash media has a tendency to return either all 0x00's or all 0xff's when
    // it's not able to read a particular sector (for example, when the sector
    // doesn't exist).  These two buffers are going to just hold all 0x00's and
    // all 0xff's to make it easier to do memcmp's against them when a sector
    // doesn't match the expected values.
    zero_buf = (char *) malloc(device_testing_context->device_info.sector_size);
    if(!zero_buf) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
        malloc_error(device_testing_context, errno);
        cleanup();
        return -1;
    }

    memset(zero_buf, 0, device_testing_context->device_info.sector_size);

    // Same thing as the zero buffer, but for all 0xff's
    ff_buf = (char *) malloc(device_testing_context->device_info.sector_size);
    if(!ff_buf) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
        malloc_error(device_testing_context, errno);
        cleanup();
        return -1;
    }

    memset(ff_buf, 0xff, device_testing_context->device_info.sector_size);

    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        if(!(device_testing_context->endurance_test_info.sector_map = (char *) malloc(device_testing_context->device_info.num_physical_sectors))) {
            log_log(device_testing_context, __func__, SEVERITY_LEVEL_ERROR, MSG_MALLOC_ERROR, strerror(errno));
            malloc_error(device_testing_context, errno);
            cleanup();
            return -1;
        }

        // Initialize the sector map
        memset(device_testing_context->endurance_test_info.sector_map, 0, device_testing_context->device_info.num_physical_sectors);
        device_testing_context->endurance_test_info.total_bad_sectors = 0;
    }

    // Generate a new UUID for the device if one isn't already assigned.
    if(!memcmp(zero_buf, device_testing_context->device_info.device_uuid, sizeof(uuid_t))) {
        uuid_generate(device_testing_context->device_info.device_uuid);
        if(state_file_status == LOAD_STATE_SUCCESS) {
            log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ASSIGNING_NEW_DEVICE_ID);
        }

        uuid_unparse(device_testing_context->device_info.device_uuid, device_uuid_str);
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ASSIGNING_DEVICE_ID_TO_DEVICE, device_uuid_str);
    }

    // Precompute the failure thresholds
    device_testing_context->endurance_test_info.sectors_to_10_threshold = device_testing_context->device_info.num_physical_sectors / 10;
    if(device_testing_context->device_info.num_physical_sectors % 10) {
        device_testing_context->endurance_test_info.sectors_to_10_threshold++;
    }

    device_testing_context->endurance_test_info.sectors_to_25_threshold = device_testing_context->device_info.num_physical_sectors / 4;
    if(device_testing_context->device_info.num_physical_sectors % 4) {
        device_testing_context->endurance_test_info.sectors_to_25_threshold++;
    }

    // Start filling up the device
    if(state_file_status == LOAD_STATE_FILE_NOT_SPECIFIED || state_file_status == LOAD_STATE_FILE_DOES_NOT_EXIST) {
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_STARTING);
    } else {
        // Count up the number of bad sectors and update device_testing_context->endurance_test_info.total_bad_sectors
        for(j = 0; j < device_testing_context->device_info.num_physical_sectors; j++) {
            if(is_sector_bad(device_testing_context, j)) {
                device_testing_context->endurance_test_info.total_bad_sectors++;
            }
        }

        device_testing_context->endurance_test_info.stats_file_counters.last_bytes_written = device_testing_context->endurance_test_info.stats_file_counters.total_bytes_written;
        device_testing_context->endurance_test_info.stats_file_counters.last_bytes_read = device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read;
        device_testing_context->endurance_test_info.stats_file_counters.last_bad_sectors = device_testing_context->endurance_test_info.total_bad_sectors;

        log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_ENDURANCE_TEST_RESUMING, device_testing_context->endurance_test_info.rounds_completed + 1);
    }

    assert(!gettimeofday(&device_testing_context->endurance_test_info.stats_file_counters.last_update_time, NULL));
    stats_cur_time = device_testing_context->endurance_test_info.stats_file_counters.last_update_time;

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
        sql_thread_params.device_testing_context = device_testing_context;

        if(iret = pthread_create(&sql_thread, NULL, &sql_thread_main, &sql_thread_params)) {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ERROR_CREATING_SQL_THREAD, strerror(iret));
        }
    }

    print_sql_status(sql_thread_status);
    prev_sql_thread_status = sql_thread_status;
    device_testing_context->endurance_test_info.test_started = 1;

    for(; device_testing_context->endurance_test_info.total_bad_sectors < (device_testing_context->device_info.num_physical_sectors / 2); device_testing_context->endurance_test_info.rounds_completed++) {
        main_thread_status = MAIN_THREAD_STATUS_WRITING;
        draw_percentage(device_testing_context); // Just in case it hasn't been drawn recently
        endurance_test_info_reset_per_round_counters(device_testing_context);

        if(prev_sql_thread_status != sql_thread_status) {
            prev_sql_thread_status = sql_thread_status;
            print_sql_status(sql_thread_status);
        }

        // If we're past the first round of testing, save the program state.
        if(device_testing_context->endurance_test_info.rounds_completed) {
            if(save_state(device_testing_context)) {
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_SAVE_STATE_ERROR);
                message_window(device_testing_context, stdscr, WARNING_TITLE, "An error occurred while trying to save the program state.  Save stating has been disabled.", 1);

                free(program_options.state_file);
                program_options.state_file = NULL;
            }
        }

        if(prev_sql_thread_status != sql_thread_status) {
            prev_sql_thread_status = sql_thread_status;
            print_sql_status(sql_thread_status);
        }

        device_testing_context->endurance_test_info.current_phase = CURRENT_PHASE_WRITING;
        if(!program_options.no_curses) {
            j = snprintf(msg_buffer, sizeof(msg_buffer), " Round %'lu ", device_testing_context->endurance_test_info.rounds_completed + 1);
            mvaddstr(ROUNDNUM_DISPLAY_Y, ROUNDNUM_DISPLAY_X(j), msg_buffer);
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Writing ");
        }

        reset_sector_map(device_testing_context);

        redraw_sector_map(device_testing_context);
        refresh();

        read_order = random_list(device_testing_context);

        for(cur_slice = 0, restart_slice = 0; cur_slice < NUM_SLICES; cur_slice++, restart_slice = 0) {
            if(ret = endurance_test_write_slice(device_testing_context,
                                                device_testing_context->endurance_test_info.rng_state.initial_seed + read_order[cur_slice] + (device_testing_context->endurance_test_info.rounds_completed * NUM_SLICES),
                                                read_order[cur_slice],
                                                sectors_per_block)) {
                main_thread_status = MAIN_THREAD_STATUS_ENDING;

                if(ret > 0) {
                    print_device_summary(device_testing_context, ret);
                }

                cleanup();
                return 0;
            }
        }

        free(read_order);

        main_thread_status = MAIN_THREAD_STATUS_READING;
        read_order = random_list(device_testing_context);
        device_testing_context->endurance_test_info.current_phase = CURRENT_PHASE_READING;

        if(!program_options.no_curses) {
            mvaddstr(READWRITE_DISPLAY_Y, READWRITE_DISPLAY_X, " Reading ");
        }

        for(cur_slice = 0; cur_slice < NUM_SLICES; cur_slice++) {
            rng_reseed(device_testing_context, device_testing_context->endurance_test_info.rng_state.initial_seed + read_order[cur_slice] + (device_testing_context->endurance_test_info.rounds_completed * NUM_SLICES));

            if(lseek_or_retry(device_testing_context, get_slice_start(device_testing_context, read_order[cur_slice]) * device_testing_context->device_info.sector_size, &device_was_disconnected) == -1) {
                main_thread_status = MAIN_THREAD_STATUS_ENDING;
                print_device_summary(device_testing_context, ABORT_REASON_SEEK_ERROR);
                cleanup();
                return 0;
            }

            if(read_order[cur_slice] == 15) {
                last_sector = device_testing_context->device_info.num_physical_sectors;
            } else {
                last_sector = get_slice_start(device_testing_context, read_order[cur_slice] + 1);
            }

            for(cur_sector = get_slice_start(device_testing_context, read_order[cur_slice]); cur_sector < last_sector; cur_sector += cur_sectors_per_block) {
                if(sql_thread_status != prev_sql_thread_status) {
                    prev_sql_thread_status = sql_thread_status;
                    print_sql_status(sql_thread_status);
                }

                // Use bytes_left_to_write to hold the bytes left to read
                if((cur_sector + sectors_per_block) > last_sector) {
                    cur_sectors_per_block = last_sector - cur_sector;
                    cur_block_size = cur_sectors_per_block * device_testing_context->device_info.sector_size;
                } else {
                    cur_block_size = device_testing_context->device_info.optimal_block_size;
                    cur_sectors_per_block = sectors_per_block;
                }

                // Regenerate the data we originally wrote to the device.
                rng_fill_buffer(device_testing_context, buf, cur_block_size);
                bytes_left_to_write = cur_block_size;

                // Re-embed the sector number and CRC32 into the expected data
                prepare_endurance_test_block(device_testing_context, buf, cur_sectors_per_block, cur_sector);

                num_uuid_mismatches = 0;
                do {
                    device_mangling_detected = 0;

                    if(endurance_test_read_block(device_testing_context, cur_sector, cur_sectors_per_block, compare_buf)) {
                        main_thread_status = MAIN_THREAD_STATUS_ENDING;
                        print_device_summary(device_testing_context, ABORT_REASON_READ_ERROR);

                        cleanup();
                        return 0;
                    }

                    // Do a first pass to see if there was any device mangling
                    for(j = 0; j < cur_block_size; j += device_testing_context->device_info.sector_size) {
                        if(!is_sector_bad(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size))) {
                            if(!calculate_crc32c(0, compare_buf + j, device_testing_context->device_info.sector_size)) {
                                get_embedded_device_uuid(compare_buf + j, device_uuid_from_device);
                                if(memcmp(device_testing_context->device_info.device_uuid, device_uuid_from_device, sizeof(uuid_t))) {
                                    device_mangling_detected = 1;
                                    num_uuid_mismatches++;

                                    uuid_unparse(device_uuid_from_device, device_uuid_str);

                                    if(num_uuid_mismatches < 5) {
                                        // Seek back to the beginning of the block
                                        if(lseek_or_retry(device_testing_context, cur_sector * device_testing_context->device_info.sector_size, &device_was_disconnected) == -1) {
                                            main_thread_status = MAIN_THREAD_STATUS_ENDING;
                                            print_device_summary(device_testing_context, ABORT_REASON_WRITE_ERROR);
                                            cleanup();
                                            return 0;
                                        }

                                        log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DEVICE_MANGLING_DETECTED, cur_sector + (j / device_testing_context->device_info.sector_size), device_uuid_str);
                                    }

                                    break;
                                }
                            }
                        }
                    }
                } while(device_mangling_detected && num_uuid_mismatches < 5);

                mark_sectors_read(device_testing_context, cur_sector, cur_sector + cur_sectors_per_block);
                device_testing_context->endurance_test_info.stats_file_counters.total_bytes_read += cur_block_size;

                // Compare
                num_uuid_mismatches = 0;
                for(j = 0; j < cur_block_size; j += device_testing_context->device_info.sector_size) {
                    handle_key_inputs(device_testing_context, NULL);
                    if(memcmp(buf + j, compare_buf + j, device_testing_context->device_info.sector_size)) {
                        if(!is_sector_bad(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size))) {
                            get_embedded_device_uuid(compare_buf + j, device_uuid_from_device);

                            if(!memcmp(compare_buf + j, zero_buf, device_testing_context->device_info.sector_size)) {
                                // The data in the sector is all zeroes
                                log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_SECTOR_ALL_00S, cur_sector + (j / device_testing_context->device_info.sector_size));
                            } else if(!memcmp(compare_buf + j, ff_buf, device_testing_context->device_info.sector_size)) {
                                // The data in the sector is all 0xff's
                                log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_SECTOR_ALL_FFS, cur_sector + (j / device_testing_context->device_info.sector_size));
                            } else if(calculate_crc32c(0, compare_buf + j, device_testing_context->device_info.sector_size)) {
                                // The CRC-32 embedded in the sector data doesn't match the calculated CRC-32
                                log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_CRC32_MISMATCH, cur_sector + (j / device_testing_context->device_info.sector_size), get_embedded_crc32c(compare_buf + j, device_testing_context->device_info.sector_size),
                                        calculate_crc32c(0, compare_buf + j, device_testing_context->device_info.sector_size - sizeof(uint32_t)));
                                log_sector_contents(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size), device_testing_context->device_info.sector_size, buf + j, compare_buf + j);
                            } else if(memcmp(device_testing_context->device_info.device_uuid, device_uuid_from_device, sizeof(uuid_t))) {
                                // The UUID embedded in the sector data doesn't match this device's UUID
                                // If we made it to this point, we've already tried to re-read the data and failed
                                log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_DEVICE_MANGLING, cur_sector + (j / device_testing_context->device_info.sector_size), device_uuid_str);
                            } else if(decode_embedded_round_number(compare_buf + j) != device_testing_context->endurance_test_info.rounds_completed) {
                                log_log(device_testing_context,
                                        NULL,
                                        SEVERITY_LEVEL_DEBUG,
                                        MSG_DATA_MISMATCH_WRITE_FAILURE,
                                        cur_sector + (j / device_testing_context->device_info.sector_size),
                                        decode_embedded_round_number(compare_buf + j) + 1,
                                        decode_embedded_sector_number(compare_buf + j));
                            } else if(decode_embedded_sector_number(compare_buf + j) != (cur_sector + (j / device_testing_context->device_info.sector_size))) {
                                log_log(device_testing_context,
                                        NULL,
                                        SEVERITY_LEVEL_DEBUG,
                                        MSG_DATA_MISMATCH_ADDRESS_DECODING_FAILURE,
                                        cur_sector + (j / device_testing_context->device_info.sector_size), decode_embedded_sector_number(compare_buf + j));
                            } else {
                                log_log(device_testing_context, NULL, SEVERITY_LEVEL_DEBUG, MSG_DATA_MISMATCH_GENERIC, cur_sector + (j / device_testing_context->device_info.sector_size));
                                log_sector_contents(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size), device_testing_context->device_info.sector_size, buf + j, compare_buf + j);
                            }

                            device_testing_context->endurance_test_info.num_new_bad_sectors_this_round++;
                        }

                        mark_sector_bad(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size));
                        device_testing_context->endurance_test_info.num_bad_sectors_this_round++;

                    } else {
                        if(is_sector_bad(device_testing_context, cur_sector + (j / device_testing_context->device_info.sector_size))) {
                            device_testing_context->endurance_test_info.num_good_sectors_this_round++;
                        }
                    }
                }

                refresh();

                assert(!gettimeofday(&stats_cur_time, NULL));
                if(timediff(device_testing_context->endurance_test_info.stats_file_counters.last_update_time, stats_cur_time) >= (program_options.stats_interval * 1000000)) {
                    stats_log(device_testing_context);
                }
            }
        }

        free(read_order);
        read_order = NULL;

        perform_end_of_round_summary(device_testing_context);
    }

    main_thread_status = MAIN_THREAD_STATUS_ENDING;
    print_device_summary(device_testing_context, ABORT_REASON_FIFTY_PERCENT_FAILURE);

    cleanup();
    return 0;
}

