#include <curses.h>
#include <stdlib.h>
#include <string.h>

#include "mfst.h"
#include "ncurses.h"

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

WINDOW *message_window(WINDOW *parent, const char *title, char *msg, char wait) {
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
        while(handle_key_inputs(window) != '\r') {
            napms(100);
        }
        erase_and_delete_window(window);
        return NULL;
    } else {
        return window;
    }
}

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

void erase_and_delete_window(WINDOW *window) {
    if(!program_options.no_curses) {
        werase(window);
        touchwin(stdscr);
        wrefresh(window);
        delwin(window);
    }
}

