#if !defined(NCURSES_H)
#define NCURSES_H

#include <inttypes.h>

#include "config.h"
#include "device_testing_context.h"
#include "sql.h"

extern int ncurses_active;

#  if defined(HAVE_NCURSES)

#include <curses.h>

/**
 * Initializes curses and sets up the color pairs that we frequently use.
 *
 * @returns -1 if the screen is too small to hold the UI, 0 otherwise.
 */
int screen_setup();

/**
 * Prints the device name on the screen.
 */
void print_device_name(device_testing_context_type *device_testing_context);

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
void draw_sector(uint64_t sector_num, int color, int with_diamond, int with_x);

/**
 * Draw the "% sectors bad" display.
 *
 * @param device_testing_context  The device whose percentage of bad sectors
 *                                should be drawn.
 */
void draw_percentage(device_testing_context_type *device_testing_context);

/**
 * Redraw the blocks containing the given sectors.  The display is not
 * refreshed after the blocks are drawn.
 *
 * @param device_testing_context  The device whose sectors should be drawn.
 * @param start_sector            The sector number of the first sector to be
 *                                redrawn.
 * @param end_sector              The sector number at which to stop redrawing.
 *                                (Ergo, all sectors within the range
 *                                [start_sector, end_sector) are redrawn.)
 */
void draw_sectors(device_testing_context_type *device_testing_context, uint64_t start_sector, uint64_t end_sector);

/**
 * Recomputes the parameters for displaying the sector map on the display, then
 * redraws the entire sector map.  The display is not refreshed after the sector
 * map is redrawn.  If sector_map is NULL, the sector map is not redrawn, but
 * the display parameters are still recomputed.
 *
 * @param device_testing_context  The device that will be drawn on the display.
 */
void redraw_sector_map(device_testing_context_type *device_testing_context);

/**
 * Print the status of the SQL thread on the display.
 *
 * @param status  The status of the SQL thread.
 */
void print_sql_status(sql_thread_status_type status);

/**
 * Draws a single character on the screen in the requested color.  Does not
 * refresh the screen.
 *
 * @param y_loc       The Y location at which to draw the character.
 * @param x_loc       The X location at which to draw the character.
 * @param color_pair  The number of the color pair that should be used to draw
 *                    the requested character.
 * @param ch          The character to draw to the screen.
 */
void draw_colored_char(int y_loc, int x_loc, int color_pair, chtype ch);

/**
 * Draws a string on the screen in the requested color.  Does not refresh the
 * screen.
 *
 * @param y_loc       The Y location at which to draw the string.
 * @param x_loc       The X location at which to draw the string.
 * @param color_pair  The number of the color pair that should be used to draw
 *                    the requested string.
 * @param str         The string to draw to the screen.
 */
void draw_colored_str(int y_loc, int x_loc, int color_pair, char *str);

/**
 * Updates the read/write speed on the display.
 *
 * @param device_testing_context  The device whose info should be shown on the
 *                                display.
 */
void print_status_update(device_testing_context_type *device_testing_context);

/**
 * Displays a message to the user indicating that the device has been
 * disconnected.
 *
 * @returns A handle to the new window being displayed on the screen, or NULL if
 *          an error occurred or if ncurses is disabled.
 */
WINDOW *device_disconnected_message();

/**
 * Displays a message to the user indicating that a device reset is being
 * attempted.
 *
 * @returns A handle to the new window being displayed on the screen, or NULL if
 *          an error occurred or if ncurses is disabled.
 */
WINDOW *resetting_device_message();

/**
 * Displays a dialog to the user indicating that a memory allocation error
 * occurred.
 *
 * @param device_testing_context  The device currently being shown on the
 *                                screen.  (This is needed in case the screen
 *                                needs to be redrawn while the dialog is being
 *                                shown.)
 * @param errnum                  The error number of the error that occurred.
 */
void malloc_error(device_testing_context_type *device_testing_context, int errnum);

#  else

// If ncurses support isn't enabled, we'll just make all of these placeholder functions that do nothing.
#define ERR -1
typedef void WINDOW;
typedef unsigned chtype;

inline int screen_setup() { return 0; }
inline void print_device_name(device_testing_context_type *device_testing_context) {}
inline WINDOW *message_window(device_testing_context_type *device_testing_context, WINDOW *parent, const char *title, char *msg, char wait) { return NULL; }
inline int handle_key_inputs(device_testing_context_type *device_testing_context, WINDOW *curwin) { return ERR; }
inline void erase_and_delete_window(WINDOW *window) {}
inline void print_with_color(int y, int x, int color, const char *str) {}
inline void draw_sector(uint64_t sector_num, int color, int with_diamond, int with_x) {}
inline void draw_percentage(device_testing_context_type *device_testing_context) {}
inline void draw_sectors(device_testing_context_type *device_testing_context, uint64_t start_sector, uint64_t end_sector) {}
inline void redraw_sector_map(device_testing_context_type *device_testing_context) {}
inline void print_sql_status(sql_thread_status_type status) {}
inline void draw_colored_char(int y_loc, int x_loc, int color_pair, chtype ch) {}
inline void draw_colored_str(int y_loc, int x_loc, int color_pair, char *str) {}
inline void print_status_update(device_testing_context_type *device_testing_context) {}
inline WINDOW *device_disconnected_message() { return NULL; }
inline WINDOW *resetting_device_message() { return NULL; }
inline void malloc_error(device_testing_context_type *device_testing_context, int errnum) {}

// Placeholders for some ncurses functions
// This is probably a sign that I need to write wrappers for these...
inline void attroff(int num) {}
inline void attron(int num) {}
inline void box(WINDOW *window, chtype vertical_char, chtype horizontal_char) {}
inline int COLOR_PAIR(int num) { return 0; }
inline void delwin(WINDOW *window) {}
inline void endwin() {}
inline void erase() {}
inline void mvaddch(int y, int x, char ch) {}
inline void mvaddstr(int y, int x, const char *str) {}
inline void mvprintw(int y, int x, const char *str, ...) {}
inline void mvwprintw(WINDOW *window, int y, int x, const char *str, ...) {}
inline void refresh() {}
inline void touchwin(WINDOW *window) {}
inline void wattron(WINDOW *window, int attr) {}
inline void wattroff(WINDOW *window, int attr) {}
inline void wrefresh(WINDOW *window) {}

extern WINDOW *stdscr;
extern int LINES;
extern int COLS;
extern int A_BOLD;
extern chtype ACS_DIAMOND ;

#  endif // defined(HAVE_NCURSES)
#endif // !defined(NCURSES_H)
