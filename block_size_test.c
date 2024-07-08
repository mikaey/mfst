#include <assert.h>
#include <curses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "block_size_test.h"
#include "lockfile.h"
#include "mfst.h"
#include "ncurses.h"
#include "rng.h"
#include "util.h"

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
    uint64_t cur_block_size, total_bytes_written, cur_block_bytes_left, ret;
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
        rng_init(end_time.tv_sec);
        rng_fill_buffer(buf, buf_size);

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

