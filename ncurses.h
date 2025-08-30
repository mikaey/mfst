#if !defined(NCURSES_H)
#define NCURSES_H

#include <curses.h>

#include "device_testing_context.h"
/**
 * Create a window and show a message in it.  Afterwards, the parent window
 * is touched and the window is refreshed.
 *
 * The string in `msg` is displayed in the new window, with line wrapping
 * automatically applied.  The width of the dialog is calculated to take up 75%
 * of the screen width, or the width of the longest line in the message,
 * whichever is less.
 *
 * If `wait` is set to a non-zero value, two lines are added to the bottom of
 * the window: a blank line, and a line that shows "Press Enter to continue",
 * centered in the window.  The length of this string is taken into account
 * when calculating the width of the window.  The function will then block and
 * wait for the user to press Enter.  Afterwards, the function will erase and
 * destroy the window, and the function will return NULL.
 *
 * @param device_testing_context  The current device being tested.  If `wait` is
 *                                non-zero, this parameter MUST be set to the
 *                                current device being tested.  (This is needed
 *                                in case the screen needs to be redrawn while
 *                                waiting for the user to dismiss the dialog.)
 *                                If `wait` is set to 0, this parameter is
 *                                ignored and may be set to NULL.
 * @param parent                  The window which will serve as the parent for
 *                                the new window.
 * @param title                   The title that will be displayed at the top of
 *                                the window.  If this is set to NULL, no title
 *                                is displayed.
 * @param msg                     A pointer to a null-terminated string that
 *                                will be shown in the new window.
 * @param wait                    Non-zero to indicate that the function should
 *                                block and wait for the user to press Enter, or
 *                                zero to indicate that the function should
 *                                return immediately after rendering the dialog.
 *
 * @returns A pointer to the new window, or NULL if (a) an error occurs while
 *          trying to wrap the given string, (b) wait is set to a non-zero value
 *          and the user dismissed the dialog, or (c) curses mode is turned off.
 *          If a pointer to a new window is returned, it is the caller's
 *          responsibility to delete it.
 */
WINDOW *message_window(device_testing_context_type *device_testing_context, WINDOW *parent, const char *title, char *msg, char wait);

/**
 * A wrapper for getch()/wgetch() that handles KEY_RESIZE events.
 *
 * @param device_testing_context  The device currently being tested.  (This is
 *                                needed in case a KEY_RESIZE keypress is
 *                                detected and the screen needs to be redrawn.)
 * @param curwin                  The active window, or NULL if no window/stdscr
 *                                is the active window.
 *
 * @returns If getch()/wgetch() returns KEY_RESIZE, this function intercepts
 *          the event and returns ERR.  Otherwise, this function returns
 *          whatever getch()/wgetch() returned.
 */
int handle_key_inputs(device_testing_context_type *device_testing_context, WINDOW *curwin);

/**
 * Erases the given window, refreshes it, then deletes it.
 *
 * @param window  The window to erase and delete.
 */
void erase_and_delete_window(WINDOW *window);

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
void print_with_color(int y, int x, int color, const char *str);

#endif // !defined(NCURSES_H)
