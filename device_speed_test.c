#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "device_speed_test.h"
#include "lockfile.h"
#include "messages.h"
#include "mfst.h"
#include "ncurses.h"
#include "rng.h"
#include "util.h"

// Scratch buffer for messages; we're allocating it statically so that we can
// still log messages in case of memory shortages
static char msg_buffer[512];

void lseek_error_during_speed_test(device_testing_context_type *device_testing_context, int errnum) {
    log_log(device_testing_context, "probe_device_speeds", SEVERITY_LEVEL_DEBUG, MSG_LSEEK_ERROR, strerror(errnum));
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_IO_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We got an error while trying to move around the device.  It "
             "could be that the device was removed or experienced an error and "
             "disconnected itself.  If that's the case, the remainder of the "
             "tests are going to fail pretty quickly.\n\nUnfortunately, this "
             "means that we won't be able to complete the speed tests.\n\nThe "
             "error we got was: %s", strerror(errnum));

    message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 1);
}

void io_error_during_speed_test(device_testing_context_type *device_testing_context, char write, int errnum) {
    log_log(device_testing_context, "probe_device_speeds", SEVERITY_LEVEL_DEBUG, write ? MSG_WRITE_ERROR : MSG_READ_ERROR, strerror(errnum));
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_IO_ERROR);

    snprintf(msg_buffer, sizeof(msg_buffer),
             "We got an error while trying to %s the device.  It could be that "
             "the device was removed, experienced an error and disconnected "
             "itself, or set itself to read-only.\n\nUnfortunately, this means "
             "that we won't be able to complete the speed tests.\n\nThe error "
             "we got was: %s", write ? "write to" : "read from", strerror(errnum));

    message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 1);
}

/**
 * Using the results of the speed tests, print out the various SD speed class
 * markings and whether or not the speed results indicate that the card should
 * be displaying that mark.
 */
void print_class_marking_qualifications(device_testing_context_type *device_testing_context) {
    if(!program_options.no_curses && (device_testing_context->performance_test_info.sequential_write_speed || (device_testing_context->performance_test_info.random_write_iops && device_testing_context->performance_test_info.random_read_iops))) {
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

        if(device_testing_context->performance_test_info.sequential_write_speed) {
            if(device_testing_context->performance_test_info.sequential_write_speed >= 2000000) {
                print_with_color(SPEED_CLASS_2_RESULT_Y, SPEED_CLASS_2_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_2_RESULT_Y, SPEED_CLASS_2_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 4000000) {
                print_with_color(SPEED_CLASS_4_RESULT_Y, SPEED_CLASS_4_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_4_RESULT_Y, SPEED_CLASS_4_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 6000000) {
                print_with_color(SPEED_CLASS_6_RESULT_Y, SPEED_CLASS_6_RESULT_X, GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V6_RESULT_Y     , SPEED_V6_RESULT_X     , GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_6_RESULT_Y, SPEED_CLASS_6_RESULT_X, RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V6_RESULT_Y     , SPEED_V6_RESULT_X     , RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 10000000) {
                print_with_color(SPEED_CLASS_10_RESULT_Y, SPEED_CLASS_10_RESULT_X, GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_U1_RESULT_Y      , SPEED_U1_RESULT_X      , GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V10_RESULT_Y     , SPEED_V10_RESULT_X     , GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_CLASS_10_RESULT_Y, SPEED_CLASS_10_RESULT_X, RED_ON_BLACK, "No     ");
                print_with_color(SPEED_U1_RESULT_Y      , SPEED_U1_RESULT_X      , RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V10_RESULT_Y     , SPEED_V10_RESULT_X     , RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 30000000) {
                print_with_color(SPEED_U3_RESULT_Y , SPEED_U3_RESULT_X , GREEN_ON_BLACK, "Yes    ");
                print_with_color(SPEED_V30_RESULT_Y, SPEED_V30_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_U3_RESULT_Y , SPEED_U3_RESULT_X , RED_ON_BLACK, "No     ");
                print_with_color(SPEED_V30_RESULT_Y, SPEED_V30_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 60000000) {
                print_with_color(SPEED_V60_RESULT_Y, SPEED_V60_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_V60_RESULT_Y, SPEED_V60_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.sequential_write_speed >= 90000000) {
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

        if(device_testing_context->performance_test_info.random_read_iops && device_testing_context->performance_test_info.random_write_iops) {
            if(device_testing_context->performance_test_info.random_read_iops >= 2000 && device_testing_context->performance_test_info.random_write_iops >= 500) {
                print_with_color(SPEED_A1_RESULT_Y, SPEED_A1_RESULT_X, GREEN_ON_BLACK, "Yes    ");
            } else {
                print_with_color(SPEED_A1_RESULT_Y, SPEED_A1_RESULT_X, RED_ON_BLACK, "No     ");
            }

            if(device_testing_context->performance_test_info.random_read_iops >= 4000 && device_testing_context->performance_test_info.random_write_iops >= 2000) {
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

int probe_device_speeds(device_testing_context_type *device_testing_context) {
    char *buf, wr, rd;
    uint64_t ctr, bytes_left, cur;
    int64_t ret;
    struct timeval start_time, cur_time;
    double secs, prev_secs;
    char rate[15];
    int local_errno;
    WINDOW *window;

    device_testing_context->performance_test_info.sequential_write_speed = 0;
    device_testing_context->performance_test_info.sequential_read_speed = 0;
    device_testing_context->performance_test_info.random_write_iops = 0;
    device_testing_context->performance_test_info.random_read_iops = 0;

    if(lock_lockfile(device_testing_context)) {
        local_errno = errno;
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_LOCK_ERROR);

        snprintf(msg_buffer, sizeof(msg_buffer),
                 "Unable to obtain a lock on the lockfile.  Unfortunately, "
                 "this means that we won't be able to run the speed tests.\n\n"
                 "The error we got was: %s", strerror(local_errno));

        message_window(device_testing_context, stdscr, ERROR_TITLE, msg_buffer, 1);
        return -1;
    }

    if(local_errno = posix_memalign((void **) &buf, sysconf(_SC_PAGESIZE), device_testing_context->device_info.optimal_block_size < 4096 ? 4096 : device_testing_context->device_info.optimal_block_size)) {
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_POSIX_MEMALIGN_ERROR, strerror(local_errno));
        log_log(device_testing_context, NULL, SEVERITY_LEVEL_WARNING, MSG_ABORTING_SPEED_TEST_DUE_TO_MEMORY_ERROR);

        unlock_lockfile(device_testing_context);

        snprintf(msg_buffer, sizeof(msg_buffer),
                 "We couldn't allocate memory we need for the speed tests."
                 "Unfortunately, this means that we won't be able to run the "
                 "speed tests on this device.\n\nThe error we got was: %s",
                 strerror(local_errno));

        message_window(device_testing_context, stdscr, WARNING_TITLE, msg_buffer, 1);
        return -1;
    }

    log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_SPEED_TEST_STARTING);
    window = message_window(device_testing_context, stdscr, NULL, "Testing read/write speeds...", 0);

    for(rd = 0; rd < 2; rd++) {
        for(wr = 0; wr < 2; wr++) {
            ctr = 0;
            assert(!gettimeofday(&start_time, NULL));

            if(!rd) {
                if(lseek(device_testing_context->device_info.fd, 0, SEEK_SET) == -1) {
                    local_errno = errno;
                    erase_and_delete_window(window);
                    free(buf);

                    lseek_error_during_speed_test(device_testing_context, local_errno);

                    return -1;
                }
            }

            secs = 0;
            prev_secs = 0;
            while(secs < 30) {
                if(wr) {
                    rng_fill_buffer(device_testing_context, buf, rd ? 4096 : device_testing_context->device_info.optimal_block_size);
                }

                bytes_left = rd ? 4096 : device_testing_context->device_info.optimal_block_size;
                while(bytes_left && secs < 30) {
                    handle_key_inputs(device_testing_context, window);
                    if(rd) {
                        // Choose a random sector, aligned on a 4K boundary
                        cur = (((((uint64_t) rng_get_random_number(device_testing_context)) << 32) | rng_get_random_number(device_testing_context)) & 0x7FFFFFFFFFFFFFFF) %
                            (device_testing_context->device_info.num_physical_sectors - (4096 / device_testing_context->device_info.sector_size)) & 0xFFFFFFFFFFFFFFF8;
                        if(lseek(device_testing_context->device_info.fd, cur * device_testing_context->device_info.sector_size, SEEK_SET) == -1) {
                            erase_and_delete_window(window);
                            lseek_error_during_speed_test(device_testing_context, local_errno);
                            free(buf);
                            unlock_lockfile(device_testing_context);
                            return -1;
                        }
                    }

                    if(wr) {
                        ret = write(device_testing_context->device_info.fd, buf, bytes_left);
                    } else {
                        ret = read(device_testing_context->device_info.fd, buf, bytes_left);
                    }

                    if(ret == -1) {
                        local_errno = errno;
                        erase_and_delete_window(window);
                        free(buf);
                        unlock_lockfile(device_testing_context);

                        io_error_during_speed_test(device_testing_context, wr, local_errno);

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
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, wr ? MSG_SPEED_TEST_RESULTS_RANDOM_WRITE_SPEED : MSG_SPEED_TEST_RESULTS_RANDOM_READ_SPEED, ctr / secs, format_rate((ctr * 4096) / secs, rate, sizeof(rate)));
                *(wr ? &device_testing_context->performance_test_info.random_write_iops : &device_testing_context->performance_test_info.random_read_iops) = ctr / secs;
            } else {
                log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, wr ? MSG_SPEED_TEST_RESULTS_SEQUENTIAL_WRITE_SPEED : MSG_SPEED_TEST_RESULTS_SEQUENTIAL_READ_SPEED, format_rate(ctr / secs, rate, sizeof(rate)));
                *(wr ? &device_testing_context->performance_test_info.sequential_write_speed : &device_testing_context->performance_test_info.sequential_read_speed) = ctr / secs;

                if(wr) {
                    print_class_marking_qualifications(device_testing_context);
                }
            }
        }
    }
    
    unlock_lockfile(device_testing_context);

    erase_and_delete_window(window);

    // Show the speed class qualifications on the display
    print_class_marking_qualifications(device_testing_context);

    // Print the speed class qualifications to the log.  We're doing it here
    // because we're going to use print_class_marking_qualifications() to
    // repaint them on the display, and we don't want to print them to the log
    // a second time if they've already been printed out.
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_SPEED_CLASS_QUALIFICATION_RESULTS);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_2, device_testing_context->performance_test_info.sequential_write_speed >= 2000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_4, device_testing_context->performance_test_info.sequential_write_speed >= 4000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_6, device_testing_context->performance_test_info.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_CLASS_10, device_testing_context->performance_test_info.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_U1, device_testing_context->performance_test_info.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_U3, device_testing_context->performance_test_info.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V6, device_testing_context->performance_test_info.sequential_write_speed >= 6000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V10, device_testing_context->performance_test_info.sequential_write_speed >= 10000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V30, device_testing_context->performance_test_info.sequential_write_speed >= 30000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V60, device_testing_context->performance_test_info.sequential_write_speed >= 60000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_V90, device_testing_context->performance_test_info.sequential_write_speed >= 90000000 ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_A1,
            (device_testing_context->performance_test_info.sequential_write_speed >= 10485760 && device_testing_context->performance_test_info.random_read_iops >= 1500 &&
             device_testing_context->performance_test_info.random_write_iops >= 500) ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_SPEED_TEST_QUALIFIES_FOR_A2,
            (device_testing_context->performance_test_info.sequential_write_speed >= 10485760 && device_testing_context->performance_test_info.random_read_iops >= 4000 &&
             device_testing_context->performance_test_info.random_write_iops >= 2000) ? "Yes" : "No");
    log_log(device_testing_context, NULL, SEVERITY_LEVEL_INFO, MSG_BLANK_LINE);

    free(buf);
    return 0;
}
