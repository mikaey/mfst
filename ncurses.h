#if !defined(NCURSES_H)
#define NCURSES_H

#include <curses.h>

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
WINDOW *message_window(WINDOW *parent, const char *title, char **msg, char wait);

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
int handle_key_inputs(WINDOW *curwin);

/**
 * Erases the given window, refreshes it, then deletes it.
 *
 * @param window  The window to erase and delete.
 */
void erase_and_delete_window(WINDOW *window);

#endif // !defined(NCURSES_H)
