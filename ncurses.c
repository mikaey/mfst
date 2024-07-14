#include <curses.h>
#include <string.h>

#include "mfst.h"
#include "ncurses.h"

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

