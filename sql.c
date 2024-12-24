#include <errno.h>
#include <mariadb/errmsg.h>
#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "messages.h"
#include "mfst.h"
#include "sql.h"

#define CONSOLIDATED_SECTOR_MAP_SIZE 10000

volatile sql_thread_status_type sql_thread_status;

static uint64_t previous_total_bytes;
static struct timespec previous_time;

int sql_thread_update_sector_map(MYSQL *mysql, uint64_t card_id) {
    const char *update_query = "INSERT INTO consolidated_sector_maps (id, consolidated_sector_map, last_updated, cur_round_num, num_bad_sectors, status, rate) VALUES (?, ?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE consolidated_sector_map=VALUES(consolidated_sector_map), last_updated=VALUES(last_updated), cur_round_num=VALUES(cur_round_num), num_bad_sectors=VALUES(num_bad_sectors), status=VALUES(status), rate=VALUES(rate)";
    char indicator;
    time_t time_secs;
    double rate;
    double secs;
    struct timespec new_time;

    MYSQL_STMT *stmt;
    MYSQL_BIND bind_params[7];
    uint8_t *consolidated_sector_map = NULL;
    int consolidated_sector_map_size = (CONSOLIDATED_SECTOR_MAP_SIZE / 2) + (CONSOLIDATED_SECTOR_MAP_SIZE % 2);
    uint64_t sectors_per_block = device_stats.num_sectors / CONSOLIDATED_SECTOR_MAP_SIZE;
    uint64_t result, i, j;
    int64_t current_round = num_rounds + 1;
    int ret;

    // So we don't get in trouble with gcc
    main_thread_status_type tmp_main_thread_status = main_thread_status;

    // Put the consolidated sector map together
    if(!(consolidated_sector_map = malloc(sizeof(uint8_t) * consolidated_sector_map_size))) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MALLOC_ERROR, strerror(errno));
        return -1;
    }
    
    memset(consolidated_sector_map, 0, consolidated_sector_map_size);

    for(i = 0; i < CONSOLIDATED_SECTOR_MAP_SIZE; i++) {
        if(!(i % 2)) {
            consolidated_sector_map[i / 2] = 0x66;
        }
                
        for(j = sectors_per_block * i; j < (sectors_per_block * (i + 1)) && j < device_stats.num_sectors; j++) {
            if(!(i % 2)) {
                consolidated_sector_map[i / 2] = (consolidated_sector_map[i / 2] & 0x0f) | ((((consolidated_sector_map[i / 2] >> 4) & sector_display.sector_map[j]) | (((consolidated_sector_map[i / 2] >> 4) | sector_display.sector_map[j]) & 0x09)) << 4);
            } else {
                consolidated_sector_map[i / 2] = (consolidated_sector_map[i / 2] & 0xf0) | ((consolidated_sector_map[i / 2] & sector_display.sector_map[j]) | (((consolidated_sector_map[i / 2] & 0x0f) | sector_display.sector_map[j]) & 0x09));
            }
        }
    }

    if((time_secs = time(NULL)) == -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_TIME_ERROR, strerror(errno));
        return -1;
    }

    if(clock_gettime(CLOCK_MONOTONIC, &new_time) == -1) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_CLOCK_GETTIME_ERROR, strerror(errno));
        return -1;
    }

    if(previous_time.tv_sec) {
        secs = (((double) new_time.tv_sec) + (((double) new_time.tv_nsec) / 1000000000.0)) - (((double) previous_time.tv_sec) + (((double) previous_time.tv_nsec) / 1000000000.0));
        rate = ((double)((state_data.bytes_read + state_data.bytes_written) - previous_total_bytes)) / secs;
    } else {
        rate = 0;
    }

    memcpy(&previous_time, &new_time, sizeof(struct timespec));
    previous_total_bytes = state_data.bytes_read + state_data.bytes_written;

    memset(bind_params, 0, sizeof(bind_params));

    bind_params[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[0].buffer = &card_id;
    bind_params[0].buffer_length = sizeof(card_id);
    indicator = STMT_INDICATOR_NONE;
    bind_params[0].u.indicator = &indicator;
    bind_params[0].is_unsigned = 1;

    bind_params[1].buffer_type = MYSQL_TYPE_BLOB;
    bind_params[1].buffer = consolidated_sector_map;
    bind_params[1].buffer_length = consolidated_sector_map_size;
    bind_params[1].u.indicator = &indicator;

    bind_params[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[2].buffer = &time_secs;
    bind_params[2].buffer_length = sizeof(time_secs);
    bind_params[2].is_unsigned = 1;

    bind_params[3].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[3].buffer = &current_round;
    bind_params[3].buffer_length = sizeof(current_round);

    bind_params[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[4].buffer = &device_stats.num_bad_sectors;
    bind_params[4].buffer_length = sizeof(device_stats.num_bad_sectors);
    bind_params[4].is_unsigned = 1;

    bind_params[5].buffer_type = MYSQL_TYPE_LONG;
    bind_params[5].buffer = &tmp_main_thread_status;
    bind_params[5].buffer_length = sizeof(tmp_main_thread_status);

    bind_params[6].buffer_type = MYSQL_TYPE_DOUBLE;
    bind_params[6].buffer = &rate;
    bind_params[6].buffer_length = sizeof(rate);

    if(!(stmt = mysql_stmt_init(mysql))) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_INIT_ERROR, mysql_error(mysql));
        free(consolidated_sector_map);
        return -1;
    }

    if(mysql_stmt_prepare(stmt, update_query, strlen(update_query))) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            ret = 1;
        } else {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_PREPARE_ERROR, mysql_stmt_error(stmt));
            ret = -1;
        }

        mysql_stmt_close(stmt);
        free(consolidated_sector_map);
        return ret;
    }

    if(mysql_stmt_bind_param(stmt, bind_params)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_BIND_PARAM_ERROR, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        free(consolidated_sector_map);
        return -1;
    }

    sql_thread_status = SQL_THREAD_QUERY_EXECUTING;

    if(mysql_stmt_send_long_data(stmt, 1, consolidated_sector_map, consolidated_sector_map_size)) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            ret = 1;
        } else {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_SEND_LONG_DATA_ERROR, mysql_stmt_error(stmt));
            ret = -1;
        }

        mysql_stmt_close(stmt);
        free(consolidated_sector_map);
        return ret;
    }

    if(mysql_stmt_execute(stmt)) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            ret = 1;
        } else {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_EXECUTE_ERROR, mysql_stmt_error(stmt));
            ret = -1;
        }
    } else {
        sql_thread_status = SQL_THREAD_CONNECTED;
        ret = 0;
    }

    mysql_stmt_close(stmt);
    free(consolidated_sector_map);
    return 0;
}

int sql_thread_insert_card(MYSQL *mysql, char *name, uint64_t *id) {
    const char *insert_query = "INSERT INTO cards (name, uuid, size, sector_size) VALUES (?, ?, ?, ?)";
    MYSQL_STMT *stmt;
    MYSQL_BIND bind_params[4];
    char indicator;
    char uuid_str[37];
    int result;

    uuid_unparse(device_stats.device_uuid, uuid_str);

    if(!(stmt = mysql_stmt_init(mysql))) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_INIT_ERROR, mysql_error(mysql));
        return -1;
    }

    if(mysql_stmt_prepare(stmt, insert_query, strlen(insert_query))) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_PREPARE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    memset(bind_params, 0, sizeof(bind_params));

    bind_params[0].buffer_type = MYSQL_TYPE_STRING;
    bind_params[0].buffer = name;
    bind_params[0].buffer_length = strlen(name);
    indicator = STMT_INDICATOR_NTS;
    bind_params[0].u.indicator = &indicator;

    bind_params[1].buffer_type = MYSQL_TYPE_STRING;
    bind_params[1].buffer = uuid_str;
    bind_params[1].buffer_length = strlen(uuid_str);
    bind_params[1].u.indicator = &indicator;

    bind_params[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[2].buffer = &device_stats.num_sectors;
    bind_params[2].buffer_length = sizeof(device_stats.num_sectors);
    bind_params[2].is_unsigned = 1;

    bind_params[3].buffer_type = MYSQL_TYPE_LONG;
    bind_params[3].buffer = &device_stats.sector_size;
    bind_params[3].buffer_length = sizeof(device_stats.sector_size);
    bind_params[3].is_unsigned = 0;

    if(mysql_stmt_bind_param(stmt, bind_params)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_BIND_PARAM_ERROR, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    sql_thread_status = SQL_THREAD_QUERY_EXECUTING;

    if(mysql_stmt_execute(stmt)) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_EXECUTE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    *id = mysql_stmt_insert_id(stmt);
    mysql_stmt_close(stmt);

    sql_thread_status = SQL_THREAD_CONNECTED;

    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_CARD_REGISTERED, *id);

    return 0;
}

int sql_thread_find_card(MYSQL *mysql, uuid_t uuid, uint64_t *id) {
    MYSQL_STMT *stmt;
    MYSQL_BIND bind_params[2];
    char uuid_str[37];
    char indicator;
    my_bool is_error, is_null;
    int result;

    const char *find_card_query = "SELECT id FROM cards WHERE uuid=?";
    
    // Does the card already exist in the database?
    uuid_unparse(device_stats.device_uuid, uuid_str);

    if(!(stmt = mysql_stmt_init(mysql))) {
        sql_thread_status = SQL_THREAD_ERROR;
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_INIT_ERROR, mysql_error(mysql));
        return -1;
    }

    if(mysql_stmt_prepare(stmt, find_card_query, strlen(find_card_query))) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_PREPARE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    memset(bind_params, 0, sizeof(bind_params));

    // Use bind_params[0] for the input bind and bind_params[1] for the output bind
    bind_params[0].buffer_type = MYSQL_TYPE_STRING;
    bind_params[0].buffer = uuid_str;
    bind_params[0].buffer_length = strlen(uuid_str);
    indicator = STMT_INDICATOR_NTS;
    bind_params[0].u.indicator = &indicator;

    bind_params[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[1].buffer = id;
    bind_params[1].buffer_length = sizeof(*id);
    bind_params[1].error = &is_error;
    bind_params[1].is_null = &is_null;
    bind_params[1].is_unsigned = 1;

    if(mysql_stmt_bind_param(stmt, bind_params)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_BIND_PARAM_ERROR, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    if(mysql_stmt_bind_result(stmt, bind_params + 1)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_BIND_RESULT_ERROR, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    sql_thread_status = SQL_THREAD_QUERY_EXECUTING;
    if(mysql_stmt_execute(stmt)) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_EXECUTE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    result = mysql_stmt_fetch(stmt);
    mysql_stmt_free_result(stmt);
    mysql_stmt_close(stmt);

    sql_thread_status = SQL_THREAD_CONNECTED;

    if(result == MYSQL_NO_DATA) {
        *id = 0ULL;
    }

    return 0;
}

int sql_thread_update_card(MYSQL *mysql, uint64_t id) {
    MYSQL_STMT *stmt;
    MYSQL_BIND bind_params[3];
    int result;

    const char *update_query = "UPDATE cards SET size=?, sector_size=? WHERE id=?";

    if(!(stmt = mysql_stmt_init(mysql))) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_INIT_ERROR, mysql_error(mysql));
        return -1;
    }

    if(mysql_stmt_prepare(stmt, update_query, strlen(update_query))) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_PREPARE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    memset(bind_params, 0, sizeof(bind_params));

    bind_params[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[0].buffer = &device_stats.num_sectors;
    bind_params[0].buffer_length = sizeof(device_stats.num_sectors);
    bind_params[0].is_unsigned = 1;

    bind_params[1].buffer_type = MYSQL_TYPE_LONG;
    bind_params[1].buffer = &device_stats.sector_size;
    bind_params[1].buffer_length = sizeof(device_stats.sector_size);
    bind_params[1].is_unsigned = 0;

    bind_params[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bind_params[2].buffer = &id;
    bind_params[2].buffer_length = sizeof(id);
    bind_params[2].is_unsigned = 1;

    if(mysql_stmt_bind_param(stmt, bind_params)) {
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_BIND_PARAM_ERROR, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    sql_thread_status = SQL_THREAD_QUERY_EXECUTING;
    if(mysql_stmt_execute(stmt)) {
        result = mysql_stmt_errno(stmt);
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sql_thread_status = SQL_THREAD_DISCONNECTED;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_LOST_CONNECTION);
            result = 1;
        } else {
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_STMT_EXECUTE_ERROR, mysql_stmt_error(stmt));
            result = -1;
        }

        mysql_stmt_close(stmt);
        return result;
    }

    sql_thread_status = SQL_THREAD_CONNECTED;

    return 0;
}

void *sql_thread_main(void *arg) {
    /* Parameters we're getting from the main thread */
    sql_thread_params_type *params = (sql_thread_params_type *) arg;

    /* MySQL variables */
    MYSQL *mysql = NULL;
    char card_registered = 0;

    int result;
    char msg[256];
    char uuid_str[37];

    void *sql_thread_cleanup() {
        if(mysql) {
            mysql_close(mysql);
        }

        mysql_thread_end();

        return NULL;
    }

    if(!params->mysql_host || !params->mysql_username || !params->mysql_password || !params->mysql_port || !params->mysql_db_name) {
        sql_thread_status = SQL_THREAD_ERROR;
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_SQL_THREAD_REQUIRED_PARAM_MISSING);
        return NULL;
    }

    if(!mysql_thread_safe()) {
        sql_thread_status = SQL_THREAD_ERROR;
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_THREAD_SAFE_RETURNED_0);
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_MARIADB_LIBRARIES_NOT_THREAD_SAFE);
        return NULL;
    }

    if(mysql_thread_init()) {
        sql_thread_status = SQL_THREAD_ERROR;
        log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_THREAD_INIT_ERROR);
        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_MARIADB_LIBRARY_ERROR);
        return NULL;
    }

    uuid_unparse(device_stats.device_uuid, uuid_str);
    memset(&previous_time, 0, sizeof(previous_time));

    while(1) {    
        if(!(mysql = mysql_init(NULL))) {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_INIT_ERROR);
            return sql_thread_cleanup();
        }

        sql_thread_status = SQL_THREAD_CONNECTING;

        if(!mysql_real_connect(mysql, params->mysql_host, params->mysql_username, params->mysql_password, params->mysql_db_name, params->mysql_port, NULL, 0)) {
            sql_thread_status = SQL_THREAD_ERROR;
            log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_MYSQL_REAL_CONNECT_ERROR);
            mysql_close(mysql);
            sleep(30);
            continue;
        }

        sql_thread_status = SQL_THREAD_CONNECTED;

        // Were we given a card name?
        if(!card_registered) {
            if(params->card_id) {
                log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_FORCING_CARD_ID, params->card_id);
            } else {
                if(result = sql_thread_find_card(mysql, device_stats.device_uuid, &params->card_id)) {
                    if(result == -1) {
                        sql_thread_status = SQL_THREAD_ERROR;
                        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_FIND_CARD_ERROR);
                        return sql_thread_cleanup();
                    } else {
                        mysql_close(mysql);
                        sleep(30);
                        continue;
                    }
                }

                if(!params->card_id) {
                    if(!params->card_name) {
                        sql_thread_status = SQL_THREAD_ERROR;
                        log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_CARD_NOT_REGISTERED_AND_NO_CARD_NAME_PROVIDED);
                        return sql_thread_cleanup();
                    }

                    // Register the new card
                    if(result = sql_thread_insert_card(mysql, params->card_name, &params->card_id)) {
                        if(result == -1) {
                            sql_thread_status = SQL_THREAD_ERROR;
                            log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_CARD_INSERT_ERROR);
                            return sql_thread_cleanup();
                        } else {
                            mysql_close(mysql);
                            sleep(30);
                            continue;
                        }
                    }
                } else {
                    if(result = sql_thread_update_card(mysql, params->card_id)) {
                        if(result == -1) {
                            sql_thread_status = SQL_THREAD_ERROR;
                            log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_CARD_UPDATE_ERROR);
                            return sql_thread_cleanup();
                        } else {
                            mysql_close(mysql);
                            sleep(30);
                            continue;
                        }
                    }

                    log_log(__func__, SEVERITY_LEVEL_DEBUG, MSG_CARD_ALREADY_REGISTERED, uuid_str, params->card_id);
                }
            }

            card_registered = 1;
        }

        do {
            if(result = sql_thread_update_sector_map(mysql, params->card_id)) {
                if(result == -1) {
                    sql_thread_status = SQL_THREAD_ERROR;
                    log_log(NULL, SEVERITY_LEVEL_WARNING, MSG_MAP_UPDATE_ERROR);
                    return sql_thread_cleanup();
                } else {
                    mysql_close(mysql);
                    break;
                }
            }

            sleep(30);
        } while(!result);

        sleep(30);
    }
}
