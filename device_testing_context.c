#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device_testing_context.h"

device_testing_context_type *new_device_testing_context(int bod_mod_buffer_size) {
    device_testing_context_type *ret;

    if(!(ret = malloc(sizeof(device_testing_context_type)))) {
        return NULL;
    }

    // Zero everything out
    memset(ret, 0, sizeof(device_testing_context_type));
    ret->device_info.fd = -1;

    ret->endurance_test_info.rounds_to_first_error = -1ULL;
    ret->endurance_test_info.rounds_to_10_threshold = -1ULL;
    ret->endurance_test_info.rounds_to_25_threshold = -1ULL;
    ret->endurance_test_info.rounds_completed = 0ULL;

    // Allocate space for the BOD/MOD buffers.
    if(!(ret->device_info.bod_buffer = malloc(sizeof(char) * bod_mod_buffer_size))) {
        delete_device_testing_context(ret);
        return NULL;
    }

    if(!(ret->device_info.mod_buffer = malloc(sizeof(char) * bod_mod_buffer_size))) {
        delete_device_testing_context(ret);
        return NULL;
    }

    ret->device_info.bod_mod_buffer_size = bod_mod_buffer_size;

    return ret;
}

void delete_device_testing_context(device_testing_context_type *dtc) {
    if(dtc) {
        device_info_invalidate_file_handle(dtc);

        if(dtc->device_info.device_name) {
            free(dtc->device_info.device_name);
        }

        if(dtc->device_info.bod_buffer) {
            free(dtc->device_info.bod_buffer);
        }

        if(dtc->device_info.mod_buffer) {
            free(dtc->device_info.mod_buffer);
        }

        if(dtc->endurance_test_info.sector_map) {
            free(dtc->endurance_test_info.sector_map);
        }

        if(dtc->endurance_test_info.stats_file_handle) {
            fclose(dtc->endurance_test_info.stats_file_handle);
        }

        if(dtc->state_file_name) {
            free(dtc->state_file_name);
        }

        if(dtc->log_file_name) {
            free(dtc->log_file_name);
        }

        if(dtc->log_file_handle) {
            fclose(dtc->log_file_handle);
        }

        free(dtc);
    }
}

int device_info_set_device_name(device_testing_context_type *dtc, char *device_name) {
    if(dtc) {
        if(dtc->device_info.device_name) {
            free(dtc->device_info.device_name);
            dtc->device_info.device_name = NULL;
        }

        if(!(dtc->device_info.device_name = strdup(device_name))) {
            return -1;
        }
    }

    return 0;
}

void device_info_invalidate_file_handle(device_testing_context_type *dtc) {
    if(dtc->device_info.fd != -1) {
        close(dtc->device_info.fd);
        dtc->device_info.fd = -1;
    }
}

void endurance_test_info_reset_per_round_counters(device_testing_context_type *dtc) {
    if(dtc) {
        dtc->endurance_test_info.num_new_bad_sectors_this_round = 0;
        dtc->endurance_test_info.num_bad_sectors_this_round = 0;
        dtc->endurance_test_info.num_good_sectors_this_round = 0;
    }
}
