#include <assert.h>
#include <curses.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "device_testing_context.h"
#include "messages.h"
#include "mfst.h"
#include "ncurses.h"
#include "util.h"

int ncurses_active;

static struct timeval screen_dimensions_last_checked_at;

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
 * Splits a string into multiple strings that are each less than or equal to
 * the given number of characters.  If the string is longer than the
 * maximum_line_length, this function attempts to split the line on the closest
 * word boundary; otherwise, it splits the line mid-word.  Additionally, strings
 * with line feeds are split on the line feed characters (which are removed from
 * the resulting output).
 *
 * @param str              The input string to parse.
 * @param max_line_length  The maximum length of each string in the output.
 * @param string_count     A pointer to an int that will receive the number of
 *                         strings in the output array.
 *
 * @returns A pointer to an array of null-terminated strings.  Both the array
 *          and each of the strings in the array are allocated by this function
 *          using malloc() and must be free()'d once the caller is done with
 *          them.
 */
char **wordwrap(char *str, int max_line_length, int *string_count) {
    char *work_str, **output, **output_new, *curptr, *tmpptr;
    int work_str_len, cur_str_len, output_count, i;

    output = NULL;
    output_count = 0;

    // Don't modify the original string
    if(!(work_str = strdup(str))) {
        return NULL;
    }

    work_str_len = strlen(work_str);

    // Make a first pass through the string and replace any \n's with NULLs
    for(i = 0; i <= work_str_len; i++) {
        if(work_str[i] == '\n') {
            work_str[i] = 0;
        }
    }

    // Second pass: break strings that are longer than the maximum line length
    // using spaces
    for(curptr = work_str; curptr <= (work_str + work_str_len); curptr += strlen(curptr) + 1) {
        cur_str_len = strlen(curptr);
        if((cur_str_len = strlen(curptr)) > max_line_length) {
            // Set tmpptr to the last character in the string, then work
            // backwards until it finds something it can break on or it hits the
            // beginning of the string
            for(tmpptr = curptr + max_line_length; tmpptr >= curptr; tmpptr--) {
                if(*tmpptr == ' ' || *tmpptr == '\t') {
                    *tmpptr = 0;
                    break;
                }
            }
        }
    }

    // Third pass: duplicate all strings and place them in the output array.
    // While we're at it, split any strings that are still longer than the
    // maximum line length (e.g., those strings that contained a single word
    // that was longer than the maximum line length).
    for(curptr = work_str; curptr <= (work_str + work_str_len); curptr += strlen(curptr) + 1) {
        cur_str_len = strlen(curptr);
        if(!(output_new = realloc(output, sizeof(char *) * ++output_count))) {
            if(output) {
                for(i = 0; i < output_count - 1; i++) {
                    free(output[i]);
                }

                free(output);
                free(work_str);
                return NULL;
            }
        }

        output = output_new;

        if(cur_str_len >= max_line_length) {
            cur_str_len = max_line_length;
        }

        if(!(output[output_count - 1] = malloc(cur_str_len + 1))) {
            for(i = 0; i < output_count - 1; i++) {
                free(output[i]);
            }

            free(output);
            free(work_str);
            return NULL;
        }

        snprintf(output[output_count - 1], cur_str_len + 1, "%s", curptr);

        curptr[cur_str_len] = 0;
    }

    free(work_str);
    *string_count = output_count;
    return output;
}

void print_device_name(device_testing_context_type *device_testing_context) {
    if(ncurses_active && device_testing_context->device_info.device_name) {
        mvprintw(DEVICE_NAME_DISPLAY_Y, DEVICE_NAME_DISPLAY_X, "%.23s ", device_testing_context->device_info.device_name);
        refresh();
    }
}

WINDOW *message_window(device_testing_context_type *device_testing_context, WINDOW *parent, const char *title, char *msg, char wait) {
    WINDOW *window;
    int lines, len, longest, i;
    char **split;

    if(program_options.no_curses) {
        return NULL;
    }

    // Split the string so that it takes up a max of 75% of the screen width.
    if(!(split = wordwrap(msg, (COLS * 4) / 5, &lines))) {
        return NULL;
    }

    // Now figure out the actual length of the longest line
    longest = 0;

    for(i = 0; i < lines; i++) {
        len = strlen(split[i]);
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

    // If there are more rows than there are lines on the display, abort.
    if((lines + 2) > LINES) {
        for(i = 0; i < lines; i++) {
            free(split[i]);
        }

        free(split);
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

    for(i = 0; i < lines; i++) {
        mvwaddstr(window, i + 1, 2, split[i]);
        free(split[i]);
    }

    free(split);

    if(wait) {
        wattron(window, A_BOLD);
        mvwaddstr(window, lines + 2, (longest - 19) / 2, "Press Enter to continue");
        wattroff(window, A_BOLD);
    }

    wrefresh(window);

    if(wait) {
        while(handle_key_inputs(device_testing_context, window) != '\r') {
            napms(100);
        }
        erase_and_delete_window(window);
        return NULL;
    } else {
        return window;
    }
}

int handle_key_inputs(device_testing_context_type *device_testing_context, WINDOW *curwin) {
    int key, width, height;
    struct timeval now;
    time_t diff;

    if(!ncurses_active && !program_options.orig_no_curses) {
        // Check the size of the screen -- can we re-enable ncurses?
        // To prevent too much cursor flicker, we'll only check the size of the
        // screen if it's been at least one second since the last time we
        // checked it.

        assert(!gettimeofday(&now, NULL));
        diff = ((now.tv_sec - screen_dimensions_last_checked_at.tv_sec) * 1000000) + (now.tv_usec - screen_dimensions_last_checked_at.tv_usec);

        if(diff >= 1000000) {
            if(screen_setup()) {
                // screen_setup() says no -- bail out now
                return 0;
            } else {
                program_options.no_curses = 0;
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_NCURSES_REENABLING_NCURSES);
            }
        }
    }

    if(curwin) {
        key = wgetch(curwin);
    } else {
        key = getch();
    }

    if(key == KEY_RESIZE) {
        if(LINES < MIN_LINES || COLS < MIN_COLS) {
            // Bail out
            endwin();
            ncurses_active = 0;
            program_options.no_curses = 1;
            log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_NCURSES_TERMINAL_TOO_SMALL);
            assert(!gettimeofday(&screen_dimensions_last_checked_at, NULL));
        }

        if(curwin) {
            getmaxyx(curwin, height, width);
            mvwin(curwin, (LINES - height) / 2, (COLS - width) / 2);
        }

        clear();
        redraw_screen(device_testing_context);

        if(curwin) {
            touchwin(curwin);
        }

        refresh();

        return ERR;
    }

    return key;
}

void erase_and_delete_window(WINDOW *window) {
    if(!program_options.no_curses) {
        werase(window);
        touchwin(stdscr);
        wrefresh(window);
        delwin(window);
    }
}

void print_with_color(int y, int x, int color, const char *str) {
    if(!program_options.no_curses) {
        attron(COLOR_PAIR(color));
        mvaddstr(y, x, str);
        attroff(COLOR_PAIR(color));
    }
}

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

void draw_percentage(device_testing_context_type *device_testing_context) {
    float percent_bad;
    if(device_testing_context->device_info.num_physical_sectors) {
        percent_bad = (((float) device_testing_context->endurance_test_info.total_bad_sectors) / ((float) device_testing_context->device_info.num_physical_sectors)) * 100.0;
        mvprintw(PERCENT_SECTORS_FAILED_DISPLAY_Y, PERCENT_SECTORS_FAILED_DISPLAY_X, "%5.2f%%", percent_bad);
    } else {
        mvprintw(PERCENT_SECTORS_FAILED_DISPLAY_Y, PERCENT_SECTORS_FAILED_DISPLAY_X, "       ");
    }
}

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

void draw_colored_char(int y_loc, int x_loc, int color_pair, chtype ch) {
    attron(COLOR_PAIR(color_pair));
    mvaddch(y_loc, x_loc, ch);
    attroff(COLOR_PAIR(color_pair));
}

void draw_colored_str(int y_loc, int x_loc, int color_pair, char *str) {
    attron(COLOR_PAIR(color_pair));
    mvaddstr(y_loc, x_loc, str);
    attroff(COLOR_PAIR(color_pair));
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

void malloc_error(device_testing_context_type *device_testing_context, int errnum) {
    char msg_buffer[256];

    snprintf(msg_buffer, sizeof(msg_buffer),
             "Failed to allocate memory for one of the buffers we need to do "
             "the stress test.  Unfortunately this means that we have to abort "
             "the stress test.\n\nThe error we got was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
}

