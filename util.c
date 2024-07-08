#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

char *format_rate(double rate, char *buf, size_t buf_size) {
    if(rate < 1024) {
        snprintf(buf, buf_size, "%d b/s", (int) rate);
        return buf;
    } else if(rate < 1048576) {
        snprintf(buf, buf_size, "%0.2f KB/s", rate / 1024);
        return buf;
    } else if(rate < 1073741824) {
        snprintf(buf, buf_size, "%0.2f MB/s", rate / 1048576);
        return buf;
    } else if(rate < 1099511627776) {
        snprintf(buf, buf_size, "%0.2f GB/s", rate / 1073741824);
        return buf;
    } else if(rate < 1125899906842624) {
        snprintf(buf, buf_size, "%0.2f TB/s", rate / 1099511627776);
        return buf;
    } else {
        snprintf(buf, buf_size, "%0.2f PB/s", rate / 1125899906842624);
        return buf;
    }
}

time_t timediff(struct timeval start_time, struct timeval end_time) {
    return ((end_time.tv_sec - start_time.tv_sec) * 1000000) + end_time.tv_usec - start_time.tv_usec;
}

/**
 * Frees multiple pointers.
 * 
 * @param num_args  The number of pointers to be freed.
 * @param ...       Pointers to be freed.
*/
void multifree(int num_args, ...) {
    int n;
    va_list list;

    va_start(list, num_args);
    for(n = 0; n < num_args; n++) {
        free(va_arg(list, void *));
    }
}

