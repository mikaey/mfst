#include <assert.h>
#include <errno.h>
#include <json-c/json_object.h>
#include <json-c/json_pointer.h>
#include <json-c/json_util.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include "base64.h"
#include "mfst.h"

/**
 * Tests to see if the given JSON pointer exists in the specified object.
 *
 * @param obj   The object to examine.
 * @param path  The JSON pointer to test for in the object.
 *
 * @returns 0 if the specified property exists in obj, or -1 if it does not.
 */
int test_json_pointer(struct json_object *obj, const char *path) {
    struct json_object *child;
    int ret;

    ret = json_pointer_get(obj, path, &child);

    return ret;
}

int save_state() {
    struct json_object *root, *parent, *obj;
    char *filename;
    char *sector_map, *b64str;
    char device_uuid[37];
    size_t i, j;

    // If no state file was specified, do nothing
    if(!program_options.state_file) {
        return 0;
    }

    // The json_object_new_* series of functions don't specify what the return
    // value is if something goes wrong...so I'm just assuming that nothing
    // *can* go wrong.  Totally a good idea, right?
    root = json_object_new_object();

    // Write the device UUID out to the file
    uuid_unparse(device_stats.device_uuid, device_uuid);
    obj = json_object_new_string(device_uuid);
    if(json_object_object_add(root, "device_uuid", obj)) {
        json_object_put(obj);
        json_object_put(root);
        return -1;
    }

    // Put together the device_geomery object
    parent = json_object_new_object();

    obj = json_object_new_uint64(device_stats.reported_size_bytes);
    if(json_object_object_add(parent, "reported_size", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_uint64(device_stats.detected_size_bytes);
    if(json_object_object_add(parent, "detected_size", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_int(device_stats.sector_size);
    if(json_object_object_add(parent, "sector_size", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(json_object_object_add(root, "device_geometry", parent)) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    // Put together the device_info object
    parent = json_object_new_object();

    obj = json_object_new_int(device_stats.block_size);
    if(json_object_object_add(parent, "block_size", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_double(device_speeds.sequential_read_speed);
    if(json_object_object_add(parent, "sequential_read_speed", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_double(device_speeds.sequential_write_speed);
    if(json_object_object_add(parent, "sequential_write_speed", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_double(device_speeds.random_read_iops);
    if(json_object_object_add(parent, "random_read_iops", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_double(device_speeds.random_write_iops);
    if(json_object_object_add(parent, "random_write_iops", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(json_object_object_add(root, "device_info", parent)) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    // Put together the program_options object
    parent = json_object_new_object();

    obj = json_object_new_boolean(program_options.orig_no_curses);
    if(json_object_object_add(parent, "disable_curses", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(program_options.stats_file) {
        if(filename = realpath(program_options.stats_file, NULL)) {
            obj = json_object_new_string(filename);
            free(filename);

            if(json_object_object_add(parent, "stats_file", obj)) {
                json_object_put(obj);
                json_object_put(parent);
                json_object_put(root);
                return -1;
            }

            obj = json_object_new_uint64(program_options.stats_interval);
            if(json_object_object_add(parent, "stats_interval", obj)) {
                json_object_put(obj);
                json_object_put(parent);
                json_object_put(root);
                return -1;
            }
        } else {
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    }

    if(program_options.log_file) {
        if(filename = realpath(program_options.log_file, NULL)) {
            obj = json_object_new_string(filename);
            free(filename);

            if(json_object_object_add(parent, "log_file", obj)) {
                json_object_put(obj);
                json_object_put(parent);
                json_object_put(root);
                return -1;
            }
        } else {
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    }

    if(filename = realpath(program_options.lock_file, NULL)) {
        obj = json_object_new_string(filename);
        free(filename);

        if(json_object_object_add(parent, "lock_file", obj)) {
            json_object_put(obj);
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    } else {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(json_object_object_add(root, "program_options", parent)) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    // Put together the state data.

    parent = json_object_new_object();

    // The strategy for savestating is that we're going to savestate once every
    // round, at the end of the round.  In the event of a restart, we'll just
    // start over from the beginning of that round.
    //
    // The sector map has info on whether each sector has been read/written in
    // this round, as well as whether each sector has experienced an error.
    // Really all we're concerned about is the bad sector flag -- so to save
    // space, we'll compact that info down into a bitfield.  In the serialized
    // version of the bitmap, each byte will represent 8 sectors, with the
    // first sector being in the most significant bit and the last sector being
    // in the least significant bit.
    sector_map = (char *) malloc((device_stats.num_sectors / 8) + ((device_stats.num_sectors % 8) ? 1 : 0));
    for(i = 0; i < device_stats.num_sectors; i += 8) {
        sector_map[i / 8] = 0;
        for(j = 0; j < 8; j++) {
            if((i + j) < device_stats.num_sectors) {
                sector_map[i / 8] = (sector_map[i / 8] << 1) | (sector_display.sector_map[i + j] & 0x01);
            } else {
                sector_map[i / 8] <<= 1;
            }
        }
    }

    b64str = base64_encode(sector_map, (device_stats.num_sectors / 8) + ((device_stats.num_sectors % 8) ? 1 : 0), NULL);
    free(sector_map);

    if(!b64str) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_string(b64str);
    free(b64str);

    if(json_object_object_add(parent, "sector_map", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    // Save the beginning-of-device and middle-of-device data.
    if(!(b64str = base64_encode(bod_buffer, sizeof(bod_buffer), NULL))) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_string(b64str);
    free(b64str);

    if(json_object_object_add(parent, "beginning_of_device_data", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(!(b64str = base64_encode(mod_buffer, sizeof(mod_buffer), NULL))) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_string(b64str);
    free(b64str);

    if(json_object_object_add(parent, "middle_of_device_data", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_int64(num_rounds);
    if(json_object_object_add(parent, "rounds_completed", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_uint64(state_data.bytes_read);
    if(json_object_object_add(parent, "bytes_read", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    obj = json_object_new_uint64(state_data.bytes_written);
    if(json_object_object_add(parent, "bytes_written", obj)) {
        json_object_put(obj);
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    if(state_data.first_failure_round != -1) {
        obj = json_object_new_int64(state_data.first_failure_round);
        if(json_object_object_add(parent, "first_failure_round", obj)) {
            json_object_put(obj);
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    }

    if(state_data.ten_percent_failure_round != -1) {
        obj = json_object_new_int64(state_data.ten_percent_failure_round);
        if(json_object_object_add(parent, "ten_percent_failure_round", obj)) {
            json_object_put(obj);
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    }

    if(state_data.twenty_five_percent_failure_round != -1) {
        obj = json_object_new_int64(state_data.twenty_five_percent_failure_round);
        if(json_object_object_add(parent, "twenty_five_percent_failure_round", obj)) {
            json_object_put(obj);
            json_object_put(parent);
            json_object_put(root);
            return -1;
        }
    }

    if(json_object_object_add(root, "state", parent)) {
        json_object_put(parent);
        json_object_put(root);
        return -1;
    }

    // Write the state data to a temporary file in case we run into an error
    // while calling json_object_to_file.
    assert(filename = malloc(strlen(program_options.state_file) + 6));
    sprintf(filename, "%s.temp", program_options.state_file);

    if(json_object_to_file(filename, root)) {
        free(filename);
        json_object_put(root);
        return -1;
    }

    json_object_put(root);

    // Now move the temporary file overtop of the original.
    if(rename(filename, program_options.state_file)) {
        unlink(filename);
        free(filename);
        return -1;
    }

    free(filename);
    return 0;
}

int load_state() {
    struct stat statbuf;
    struct json_object *root, *obj;
    char str[256];
    int i;
    char *buffer;
    size_t detected_size, sector_size, k, l;
    char *uuid_str = NULL;

    // A bunch of constants for the JSON pointers to our JSON object
    const char *device_uuid_ptr = "/device_uuid";
    const char *reported_size_ptr = "/device_geometry/reported_size";
    const char *detected_size_ptr = "/device_geometry/detected_size";
    const char *sector_size_ptr = "/device_geometry/sector_size";
    const char *block_size_ptr = "/device_info/block_size";
    const char *sequential_read_speed_ptr = "/device_info/sequential_read_speed";
    const char *sequential_write_speed_ptr = "/device_info/sequential_write_speed";
    const char *random_read_iops_ptr = "/device_info/random_read_iops";
    const char *random_write_iops_ptr = "/device_info/random_write_iops";
    const char *disable_curses_ptr = "/program_options/disable_curses";
    const char *stats_file_ptr = "/program_options/stats_file";
    const char *log_file_ptr = "/program_options/log_file";
    const char *lock_file_ptr = "/program_options/lock_file";
    const char *stats_interval_ptr = "/program_options/stats_interval";
    const char *sector_map_ptr = "/state/sector_map";
    const char *bod_data_ptr = "/state/beginning_of_device_data";
    const char *mod_data_ptr = "/state/middle_of_device_data";
    const char *rounds_completed_ptr = "/state/rounds_completed";
    const char *bytes_read_ptr = "/state/bytes_read";
    const char *bytes_written_ptr = "/state/bytes_written";
    const char *ffr_ptr = "/state/first_failure_round";
    const char *tpfr_ptr = "/state/ten_percent_failure_round";
    const char *tfpfr_ptr = "/state/twenty_five_percent_failure_round";

    const char *all_props[] = {
        device_uuid_ptr,
        reported_size_ptr,
        detected_size_ptr,
        sector_size_ptr,
        block_size_ptr,
        sequential_read_speed_ptr,
        sequential_write_speed_ptr,
        random_read_iops_ptr,
        random_write_iops_ptr,
        disable_curses_ptr,
        stats_file_ptr,
        log_file_ptr,
        lock_file_ptr,
        stats_interval_ptr,
        sector_map_ptr,
        bod_data_ptr,
        mod_data_ptr,
        rounds_completed_ptr,
        bytes_read_ptr,
        bytes_written_ptr,
        ffr_ptr,
        tpfr_ptr,
        tfpfr_ptr,
        NULL
    };

    const json_type prop_types[] = {
        json_type_string,  // device_uuid_ptr
        json_type_int,     // reported_size_ptr
        json_type_int,     // detected_size_ptr
        json_type_int,     // sector_size_ptr
        json_type_int,     // block_size_ptr
        json_type_double,  // sequential_read_speed_ptr
        json_type_double,  // sequential_write_speed_ptr
        json_type_double,  // random_read_iops_ptr
        json_type_double,  // random_write_iops_ptr
        json_type_boolean, // disable_curses_ptr
        json_type_string,  // stats_file_ptr
        json_type_string,  // log_file_ptr
        json_type_string,  // lock_file_ptr
        json_type_int,     // stats_interval_ptr
        json_type_string,  // sector_map_ptr
        json_type_string,  // bod_data_ptr
        json_type_string,  // mod_data_ptr
        json_type_int,     // rounds_completed_ptr
        json_type_int,     // bytes_read_ptr
        json_type_int,     // bytes_written_ptr
        json_type_int,     // ffr_ptr
        json_type_int,     // tpfr_ptr
        json_type_int      // tfpfr_ptr
    };

    const int required_props[] = {
        0, // device_uuid_ptr
        1, // reported_size_ptr
        1, // detected_size_ptr
        1, // sector_size_ptr
        1, // block_size_ptr
        1, // sequential_read_speed_ptr
        1, // sequential_write_speed_ptr
        1, // random_read_iops_ptr
        1, // random_write_iops_ptr
        0, // disable_curses_ptr
        0, // stats_file_ptr
        0, // log_file_ptr
        0, // lock_file_ptr
        0, // stats_interval_ptr
        1, // sector_map_ptr
        1, // bod_data_ptr
        1, // mod_data_ptr
        1, // rounds_completed_ptr
        1, // bytes_read_ptr
        1, // bytes_written_ptr
        0, // ffr_ptr
        0, // tpfr_ptr
        0  // tfpfr_ptr
    };

    const int base64_props[] = {
        0, // device_uuid_ptr
        0, // reported_size_ptr
        0, // detected_size_ptr
        0, // sector_size_ptr
        0, // block_size_ptr
        0, // sequential_read_speed_ptr
        0, // sequential_write_speed_ptr
        0, // random_read_iops_ptr
        0, // random_write_iops_ptr
        0, // disable_curses_ptr
        0, // stats_file_ptr
        0, // log_file_ptr
        0, // lock_file_ptr
        0, // stats_interval_ptr
        1, // sector_map_ptr
        1, // bod_data_ptr
        1, // mod_data_ptr
        0, // rounds_completed_ptr
        0, // bytes_read_ptr
        0, // bytes_written_ptr
        0, // ffr_ptr
        0, // tpfr_ptr
        0  // tfpfr_ptr
    };

    char *buffers[] = {
        NULL, // device_uuid_ptr
        NULL, // reported_size_ptr
        NULL, // detected_size_ptr
        NULL, // sector_size_ptr
        NULL, // block_size_ptr
        NULL, // sequential_read_speed_ptr
        NULL, // sequential_write_speed_ptr
        NULL, // random_read_iops_ptr
        NULL, // random_write_iops_ptr
        NULL, // disable_curses_ptr
        NULL, // stats_file_ptr
        NULL, // log_file_ptr
        NULL, // lock_file_ptr
        NULL, // stats_interval_ptr
        NULL, // sector_map_ptr
        NULL, // bod_data_ptr
        NULL, // mod_data_ptr
        NULL, // rounds_completed_ptr
        NULL, // bytes_read_ptr
        NULL, // bytes_written_ptr
        NULL, // ffr_ptr
        NULL, // tpfr_ptr
        NULL, // tfpfr_ptr
    };

    size_t buffer_lens[] = {
        0, // device_uuid_ptr,
        0, // reported_size_ptr
        0, // detected_size_ptr
        0, // sector_size_ptr
        0, // block_size_ptr
        0, // sequential_read_speed_ptr
        0, // sequential_write_speed_ptr
        0, // random_read_iops_ptr
        0, // random_write_iops_ptr
        0, // disable_curses_ptr
        0, // stats_file_ptr
        0, // log_file_ptr
        0, // lock_file_ptr
        0, // stats_interval_ptr
        0, // sector_map_ptr
        0, // bod_data_ptr
        0, // mod_data_ptr
        0, // rounds_completed_ptr
        0, // bytes_read_ptr
        0, // bytes_written_ptr
        0, // ffr_ptr
        0, // tpfr_ptr
        0, // tfpfr_ptr
    };

    void *destinations[] = {
        &uuid_str,
        &device_stats.reported_size_bytes,
        &device_stats.detected_size_bytes,
        &device_stats.sector_size,
        &device_stats.block_size,
        &device_speeds.sequential_read_speed,
        &device_speeds.sequential_write_speed,
        &device_speeds.random_read_iops,
        &device_speeds.random_write_iops,
        &program_options.no_curses,
        &program_options.stats_file,
        &program_options.log_file,
        &program_options.lock_file,
        &program_options.stats_interval,
        &sector_display.sector_map,
        bod_buffer,
        mod_buffer,
        &num_rounds,
        &state_data.bytes_read,
        &state_data.bytes_written,
        &state_data.first_failure_round,
        &state_data.ten_percent_failure_round,
        &state_data.twenty_five_percent_failure_round
    };

    void free_buffers() {
        int j;
        json_object_put(root);

        for(j = 0; all_props[j]; j++) {
            if(buffers[j]) {
                free(buffers[j]);
            }
        }
    }

    if(!program_options.state_file) {
        return LOAD_STATE_FILE_NOT_SPECIFIED;
    }

    if(stat(program_options.state_file, &statbuf) == -1) {
        if(errno == ENOENT) {
            log_log("load_state(): state file not present");
            return LOAD_STATE_FILE_DOES_NOT_EXIST;
        } else {
            snprintf(str, sizeof(str), "load_state(): unable to stat() state file: %s", strerror(errno));
            log_log(str);
            return LOAD_STATE_LOAD_ERROR;
        }
    }

    if(!(root = json_object_from_file(program_options.state_file))) {
        snprintf(str, sizeof(str), "load_state(): Unable to load state file: %s", json_util_get_last_err());
        log_log(str);
        return LOAD_STATE_LOAD_ERROR;
    }

    // Validate the JSON before we try to load anything from it.
    for(i = 0; all_props[i]; i++) {
        // Make sure required properties are present in the file.
        if(required_props[i] && test_json_pointer(root, all_props[i])) {
            snprintf(str, sizeof(str), "load_state(): Rejecting state file: required property %s is missing from JSON", all_props[i]);
            log_log(str);
            json_object_put(root);
            return LOAD_STATE_LOAD_ERROR;
        }

        // Make sure data types match.
        if(!json_pointer_get(root, all_props[i], &obj)) {
            if(json_object_get_type(obj) == json_type_null) {
                json_object_put(obj);
                continue;
            }

            if((prop_types[i] == json_type_double && json_object_get_type(obj) != json_type_int && json_object_get_type(obj) != json_type_double) ||
                (prop_types[i] != json_type_double && json_object_get_type(obj) != prop_types[i])) {
                snprintf(str, sizeof(str), "load_state(): Rejecting state file: property %s has an incorrect data type", all_props[i]);
                log_log(str);
                free_buffers();

                return LOAD_STATE_LOAD_ERROR;
            }

            // Make sure numeric values are greater than 0 and strings are more
            // than 0 characters in length
            if(prop_types[i] == json_type_int) {
                if(json_object_get_uint64(obj) == 0) {
                    snprintf(str, sizeof(str), "load_state(): Rejecting state file: property %s is unparseable or zero", all_props[i]);
                    log_log(str);
                    free_buffers();

                    return LOAD_STATE_LOAD_ERROR;
                }
            } else if(prop_types[i] == json_type_double) {
                if(json_object_get_double(obj) <= 0) {
                    snprintf(str, sizeof(str), "load_state(): Rejecting state file: property %s is unparseable or zero", all_props[i]);
                    log_log(str);
                    free_buffers();

                    return LOAD_STATE_LOAD_ERROR;
                }
            } else if(prop_types[i] == json_type_string) {
                // Go ahead and copy it over to a buffer
                buffers[i] = malloc(json_object_get_string_len(obj) + 1);
                if(!buffers[i]) {
                    snprintf(str, sizeof(str), "load_state(): malloc() returned an error: %s", strerror(errno));
                    log_log(str);
                    free_buffers();

                    return LOAD_STATE_LOAD_ERROR;
                }

                strcpy(buffers[i], json_object_get_string(obj));

                if(base64_props[i]) {
                    // Base64-decode it and put that into buffers[i] instead
                    buffer = base64_decode(buffers[i], json_object_get_string_len(obj), &buffer_lens[i]);
                    if(!buffer) {
                        snprintf(str, sizeof(str), "load_state(): Rejecting state file: unable to Base64-decode %s", all_props[i]);
                        log_log(str);
                        free_buffers();

                        return LOAD_STATE_LOAD_ERROR;
                    }

                    free(buffers[i]);
                    buffers[i] = buffer;
                }
            }

            if(all_props[i] == detected_size_ptr) {
                detected_size = json_object_get_uint64(obj);
            } else if(all_props[i] == sector_size_ptr) {
                sector_size = json_object_get_uint64(obj);
            }

        }
    }

    // Make sure our Base64-encoded strings are the correct length.
    size_t expected_lens[] = {
        -1,                  // device_uuid_ptr
        -1,                  // reported_size_ptr
        -1,                  // detected_size_ptr
        -1,                  // sector_size_ptr
        -1,                  // block_size_ptr
        -1,                  // sequential_read_speed_ptr
        -1,                  // sequential_write_speed_ptr
        -1,                  // random_read_iops_ptr
        -1,                  // random_write_iops_ptr
        -1,                  // disable_curses_ptr
        -1,                  // stats_file_ptr
        -1,                  // log_file_ptr
        -1,                  // lock_file_ptr
        -1,                  // stats_interval_ptr
                             // sector_map_ptr
        (detected_size / sector_size) / 8 + (((detected_size / sector_size) % 8) ? 1 : 0),
        BOD_MOD_BUFFER_SIZE, // bod_data_ptr
        BOD_MOD_BUFFER_SIZE, // mod_data_ptr
        -1,                  // rounds_completed_ptr
        -1,                  // bytes_read_ptr
        -1,                  // bytes_written_ptr
        -1,                  // ffr_ptr
        -1,                  // tpfr_ptr
        -1                   // tfpfr_ptr
    };

    for(i = 0; all_props[i]; i++) {
        if(base64_props[i] && (buffer_lens[i] != expected_lens[i])) {
            snprintf(str, sizeof(str), "load_state(): Rejecting state file: %s contains the wrong amount of data", all_props[i]);
            log_log(str);
            free_buffers();

            return LOAD_STATE_LOAD_ERROR;
        }
    }

    // Allocate memory for the sector map, which we'll need to unpack later
    if(!(sector_display.sector_map = malloc(detected_size / sector_size))) {
        snprintf(str, sizeof(str), "load_state(): malloc() returned an error: %s", strerror(errno));
        log_log(str);
        free_buffers();

        return LOAD_STATE_LOAD_ERROR;
    }

    // Ok, let's go ahead and start populating everything.
    // I've tried to craft this so that if we make it to this point, we can't
    // fail -- we don't want to start overwriting global variables and then
    // have something happen that would leave those variables in an
    // inconsistent state.
    for(i = 0; all_props[i]; i++) {
        if(json_pointer_get(root, all_props[i], &obj)) {
            continue;
        }

        if(json_object_get_type(obj) == json_type_null) {
            continue;
        }

        if(prop_types[i] == json_type_int) {
            if(all_props[i] == sector_size_ptr || all_props[i] == block_size_ptr) {
                *((int *) destinations[i]) = json_object_get_int(obj);
            } else if(all_props[i] == ffr_ptr || all_props[i] == tpfr_ptr || all_props[i] == tfpfr_ptr) {
                *((ssize_t *) destinations[i]) = json_object_get_int64(obj);
            } else {
                *((size_t *) destinations[i]) = json_object_get_uint64(obj);
            }
        } else if(prop_types[i] == json_type_double) {
            *((double *) destinations[i]) = json_object_get_double(obj);
        } else if(prop_types[i] == json_type_boolean) {
            *((char *) destinations[i]) = json_object_get_boolean(obj);
        } else if(prop_types[i] == json_type_string) {
            // bod_buffer and mod_buffer have to handled specially because
            // we have statically allocated buffers for them
            if(all_props[i] == bod_data_ptr || all_props[i] == mod_data_ptr) {
                memcpy(destinations[i], buffers[i], BOD_MOD_BUFFER_SIZE);
                free(buffers[i]);
            } else if(all_props[i] == sector_map_ptr) {
                // We need to unpack the sector map
                for(k = 0; k < buffer_lens[i]; k++) {
                    for(l = 0; l < 8; l++) {
                        if((k + l) < buffer_lens[i]) {
                            sector_display.sector_map[(k * 8) + l] = (buffers[i][k] >> (7 - l)) & 0x01;
                        }
                    }
                }

                free(buffers[i]);
            } else {
                *((char **) destinations[i]) = buffers[i];
            }
        }
    }

    // Parse the device UUID
    if(uuid_str) {
        uuid_parse(uuid_str, device_stats.device_uuid);
        free(uuid_str);
    }

    json_object_put(root);
    return LOAD_STATE_SUCCESS;
}
