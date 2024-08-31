#include <errno.h>
#include <mariadb/errmsg.h>
#include <mariadb/mysql.h>
#include <mariadb/mysqld_error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mfst.h"
#include "sql.h"

#define CONSOLIDATED_SECTOR_MAP_SIZE 10000

volatile SqlThreadStatusType sqlThreadStatus;

static uint64_t previousTotalBytes;
static struct timespec previousTime;

int sqlThreadUpdateSectorMap(MYSQL *mysql, uint64_t cardId) {
    const char *updateQuery = "INSERT INTO consolidated_sector_maps (id, consolidated_sector_map, last_updated, cur_round_num, num_bad_sectors, status, rate) VALUES (?, ?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE consolidated_sector_map=VALUES(consolidated_sector_map), last_updated=VALUES(last_updated), cur_round_num=VALUES(cur_round_num), num_bad_sectors=VALUES(num_bad_sectors), status=VALUES(status), rate=VALUES(rate)";
    char indicator;
    char msg[256];
    time_t time_secs;
    double rate;
    double secs;
    struct timespec newTime;

    MYSQL_STMT *stmt;
    MYSQL_BIND bindParam[7];
    uint8_t *consolidatedSectorMap = NULL;
    int consolidatedSectorMapSize = (CONSOLIDATED_SECTOR_MAP_SIZE / 2) + (CONSOLIDATED_SECTOR_MAP_SIZE % 2);
    uint64_t sectorsPerBlock = device_stats.num_sectors / CONSOLIDATED_SECTOR_MAP_SIZE;
    uint64_t result, i, j;
    int64_t numRounds = num_rounds + 1;

    // So we don't get in trouble with gcc
    main_thread_status_type tmp_main_thread_status = main_thread_status;

    // Put the consolidated sector map together
    if(!(consolidatedSectorMap = malloc(sizeof(uint8_t) * consolidatedSectorMapSize))) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): malloc() failed: %m");
        log_log(msg);
        return -1;
    }
    
    memset(consolidatedSectorMap, 0, consolidatedSectorMapSize);

    for(i = 0; i < CONSOLIDATED_SECTOR_MAP_SIZE; i++) {
        if(!(i % 2)) {
            consolidatedSectorMap[i / 2] = 0x66;
        }
                
        for(j = sectorsPerBlock * i; j < (sectorsPerBlock * (i + 1)) && j < device_stats.num_sectors; j++) {
            if(!(i % 2)) {
                consolidatedSectorMap[i / 2] = (consolidatedSectorMap[i / 2] & 0x0f) | ((((consolidatedSectorMap[i / 2] >> 4) & sector_display.sector_map[j]) | (((consolidatedSectorMap[i / 2] >> 4) | sector_display.sector_map[j]) & 0x09)) << 4);
            } else {
                consolidatedSectorMap[i / 2] = (consolidatedSectorMap[i / 2] & 0xf0) | ((consolidatedSectorMap[i / 2] & sector_display.sector_map[j]) | (((consolidatedSectorMap[i / 2] & 0x0f) | sector_display.sector_map[j]) & 0x09));
            }
        }
    }

    if((time_secs = time(NULL)) == -1) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): time() failed: %m");
        log_log(msg);
        return -1;
    }

    if(clock_gettime(CLOCK_MONOTONIC, &newTime) == -1) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): clock_gettime() failed: %m");
        log_log(msg);
        return -1;
    }

    if(previousTime.tv_sec) {
        secs = (((double) newTime.tv_sec) + (((double) newTime.tv_nsec) / 1000000000.0)) - (((double) previousTime.tv_sec) + (((double) previousTime.tv_nsec) / 1000000000.0));
        rate = ((double)((state_data.bytes_read + state_data.bytes_written) - previousTotalBytes)) / secs;
    } else {
        rate = 0;
    }

    memcpy(&previousTime, &newTime, sizeof(struct timespec));
    previousTotalBytes = state_data.bytes_read + state_data.bytes_written;

    memset(bindParam, 0, sizeof(bindParam));

    bindParam[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[0].buffer = &cardId;
    bindParam[0].buffer_length = sizeof(cardId);
    indicator = STMT_INDICATOR_NONE;
    bindParam[0].u.indicator = &indicator;
    bindParam[0].is_unsigned = 1;

    bindParam[1].buffer_type = MYSQL_TYPE_BLOB;
    bindParam[1].buffer = consolidatedSectorMap;
    bindParam[1].buffer_length = consolidatedSectorMapSize;
    bindParam[1].u.indicator = &indicator;

    bindParam[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[2].buffer = &time_secs;
    bindParam[2].buffer_length = sizeof(time_secs);
    bindParam[2].is_unsigned = 1;

    bindParam[3].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[3].buffer = &numRounds;
    bindParam[3].buffer_length = sizeof(numRounds);

    bindParam[4].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[4].buffer = &device_stats.num_bad_sectors;
    bindParam[4].buffer_length = sizeof(device_stats.num_bad_sectors);
    bindParam[4].is_unsigned = 1;

    bindParam[5].buffer_type = MYSQL_TYPE_LONG;
    bindParam[5].buffer = &tmp_main_thread_status;
    bindParam[5].buffer_length = sizeof(main_thread_status);

    bindParam[6].buffer_type = MYSQL_TYPE_DOUBLE;
    bindParam[6].buffer = &rate;
    bindParam[6].buffer_length = sizeof(rate);

    if(!(stmt = mysql_stmt_init(mysql))) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): mysql_stmt_init() failed: %s", mysql_error(mysql));
        log_log(msg);
        free(consolidatedSectorMap);
        return -1;
    }

    if(result = mysql_stmt_prepare(stmt, updateQuery, strlen(updateQuery))) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadUpdateSectorMap(): lost connection to server");
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): mysql_stmt_prepare() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return -1;
        }
    }

    if(mysql_stmt_bind_param(stmt, bindParam)) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        mysql_stmt_close(stmt);
        free(consolidatedSectorMap);
        return -1;
    }

    sqlThreadStatus = SQL_THREAD_QUERY_EXECUTING;

    if(result = mysql_stmt_send_long_data(stmt, 1, consolidatedSectorMap, consolidatedSectorMapSize)) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadUpdateSectorMap(): lost connection to server");
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return 1;
        } else {
            sqlThreadStatus = SQL_THREAD_ERROR;
            snprintf(msg, sizeof(msg), "sqlThreadUpdateSectorMap(): mysql_stmt_send_long_data() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return -1;
        }
    }

    if(result = mysql_stmt_execute(stmt)) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadUpdateSectorMap(): lost connection to server");
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "qlThreadMain(): mysql_stmt_execute() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            free(consolidatedSectorMap);
            return -1;
        }
    }

    sqlThreadStatus = SQL_THREAD_CONNECTED;

    mysql_stmt_close(stmt);
    free(consolidatedSectorMap);
    return 0;
}

int sqlThreadInsertCard(MYSQL *mysql, char *name, uint64_t *id) {
    const char *insertQuery = "INSERT INTO cards (name, uuid, size, sector_size) VALUES (?, ?, ?, ?)";
    MYSQL_STMT *stmt;
    MYSQL_BIND bindParam[4];
    char indicator;
    char uuidStr[37];
    int result;
    char msg[256];

    uuid_unparse(device_stats.device_uuid, uuidStr);

    if(!(stmt = mysql_stmt_init(mysql))) {
        snprintf(msg, sizeof(msg), "sqlThreadInsertCard(): mysql_stmt_init() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        return -1;
    }

    if(result = mysql_stmt_prepare(stmt, insertQuery, strlen(insertQuery))) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadInsertCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadInsertCard(): mysql_stmt_prepare() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    memset(bindParam, 0, sizeof(bindParam));

    bindParam[0].buffer_type = MYSQL_TYPE_STRING;
    bindParam[0].buffer = name;
    bindParam[0].buffer_length = strlen(name);
    indicator = STMT_INDICATOR_NTS;
    bindParam[0].u.indicator = &indicator;

    bindParam[1].buffer_type = MYSQL_TYPE_STRING;
    bindParam[1].buffer = uuidStr;
    bindParam[1].buffer_length = strlen(uuidStr);
    bindParam[1].u.indicator = &indicator;

    bindParam[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[2].buffer = &device_stats.num_sectors;
    bindParam[2].buffer_length = sizeof(device_stats.num_sectors);
    bindParam[2].is_unsigned = 1;

    bindParam[3].buffer_type = MYSQL_TYPE_LONG;
    bindParam[3].buffer = &device_stats.sector_size;
    bindParam[3].buffer_length = sizeof(device_stats.sector_size);
    bindParam[3].is_unsigned = 0;

    if(mysql_stmt_bind_param(stmt, bindParam)) {
        snprintf(msg, sizeof(msg), "sqlThreadInsertCard(): mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        mysql_stmt_close(stmt);
        return -1;
    }

    sqlThreadStatus = SQL_THREAD_QUERY_EXECUTING;

    if(result = mysql_stmt_execute(stmt)) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadInsertCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadInsertCard(): mysql_stmt_execute() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    *id = mysql_stmt_insert_id(stmt);
    mysql_stmt_close(stmt);

    sqlThreadStatus = SQL_THREAD_CONNECTED;

    snprintf(msg, sizeof(msg), "sqlThreadInsertCard(): Successfully registered new card with ID %lu", *id);
    log_log(msg);

    return 0;
}

int sqlThreadFindCard(MYSQL *mysql, uuid_t uuid, uint64_t *id) {
    MYSQL_STMT *stmt;
    MYSQL_BIND bindParam[2];
    char uuidStr[37];
    char msg[256];
    char indicator;
    my_bool isError, isNull;
    int result;

    const char *findCardQuery = "SELECT id FROM cards WHERE uuid=?";
    
    // Does the card already exist in the database?
    uuid_unparse(device_stats.device_uuid, uuidStr);

    if(!(stmt = mysql_stmt_init(mysql))) {
        sqlThreadStatus = SQL_THREAD_ERROR;
        log_log("sqlThreadFindCard(): mysql_stmt_init() failed");
        return -1;
    }

    if(result = mysql_stmt_prepare(stmt, findCardQuery, strlen(findCardQuery))) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadFindCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            sqlThreadStatus = SQL_THREAD_ERROR;
            log_log("sqlThreadFindCard(): mysql_stmt_init() failed");
            return -1;
        }
    }

    memset(bindParam, 0, sizeof(bindParam));

    // Use bindParam[0] for the input bind and bindParam[1] for the output bind
    bindParam[0].buffer_type = MYSQL_TYPE_STRING;
    bindParam[0].buffer = uuidStr;
    bindParam[0].buffer_length = strlen(uuidStr);
    indicator = STMT_INDICATOR_NTS;
    bindParam[0].u.indicator = &indicator;

    bindParam[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[1].buffer = id;
    bindParam[1].buffer_length = sizeof(*id);
    bindParam[1].error = &isError;
    bindParam[1].is_null = &isNull;
    bindParam[1].is_unsigned = 1;

    if(mysql_stmt_bind_param(stmt, bindParam)) {
        snprintf(msg, sizeof(msg), "sqlThreadFindCard(): mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        mysql_stmt_close(stmt);
        return -1;
    }

    if(mysql_stmt_bind_result(stmt, bindParam + 1)) {
        snprintf(msg, sizeof(msg), "sqlThreadFindCard(): mysql_stmt_bind_result() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        mysql_stmt_close(stmt);
        return -1;
    }

    sqlThreadStatus = SQL_THREAD_QUERY_EXECUTING;
    if(result = mysql_stmt_execute(stmt)) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadFindCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadFindCard(): mysql_stmt_execute() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    result = mysql_stmt_fetch(stmt);
    mysql_stmt_free_result(stmt);
    mysql_stmt_close(stmt);

    sqlThreadStatus = SQL_THREAD_CONNECTED;

    if(result == MYSQL_NO_DATA) {
        *id = 0ULL;
    }

    return 0;
}

int sqlThreadUpdateCard(MYSQL *mysql, uint64_t id) {
    MYSQL_STMT *stmt;
    MYSQL_BIND bindParam[3];
    char msg[256];
    int result;

    const char *updateQuery = "UPDATE cards SET size=?, sector_size=? WHERE id=?";

    if(!(stmt = mysql_stmt_init(mysql))) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateCard(): mysql_stmt_init() failed: %s", mysql_error(mysql));
        log_log(msg);
        return -1;
    }

    if(result = mysql_stmt_prepare(stmt, updateQuery, strlen(updateQuery))) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadUpdateCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadUpdateCard(): mysql_stmt_prepare() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    memset(bindParam, 0, sizeof(bindParam));

    bindParam[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[0].buffer = &device_stats.num_sectors;
    bindParam[0].buffer_length = sizeof(device_stats.num_sectors);
    bindParam[0].is_unsigned = 1;

    bindParam[1].buffer_type = MYSQL_TYPE_LONG;
    bindParam[1].buffer = &device_stats.sector_size;
    bindParam[1].buffer_length = sizeof(device_stats.sector_size);
    bindParam[1].is_unsigned = 0;

    bindParam[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bindParam[2].buffer = &id;
    bindParam[2].buffer_length = sizeof(id);
    bindParam[2].is_unsigned = 1;

    if(mysql_stmt_bind_param(stmt, bindParam)) {
        snprintf(msg, sizeof(msg), "sqlThreadUpdateCard(): mysql_stmt_bind_param() failed: %s", mysql_stmt_error(stmt));
        log_log(msg);
        mysql_stmt_close(stmt);
        return -1;
    }

    sqlThreadStatus = SQL_THREAD_QUERY_EXECUTING;
    if(result = mysql_stmt_execute(stmt)) {
        if(result == CR_SERVER_GONE_ERROR || result == CR_SERVER_LOST || result == ER_CONNECTION_KILLED) {
            sqlThreadStatus = SQL_THREAD_DISCONNECTED;
            log_log("sqlThreadUpdateCard(): lost connection to server");
            mysql_stmt_close(stmt);
            return 1;
        } else {
            snprintf(msg, sizeof(msg), "sqlThreadUpdateCard(): mysql_stmt_execute() failed: %s", mysql_stmt_error(stmt));
            log_log(msg);
            mysql_stmt_close(stmt);
            return -1;
        }
    }

    sqlThreadStatus = SQL_THREAD_CONNECTED;

    return 0;
}

void *sqlThreadMain(void *arg) {
    /* Parameters we're getting from the main thread */
    SqlThreadParamsType *params = (SqlThreadParamsType *) arg;

    /* MySQL variables */
    MYSQL *mysql = NULL;
    char cardRegistered = 0;

    int result;
    char msg[256];
    char uuidStr[37];

    void *sqlThreadCleanup() {
        if(mysql) {
            mysql_close(mysql);
        }

        mysql_thread_end();

        return NULL;
    }

    if(!params->mysqlHost || !params->mysqlUsername || !params->mysqlPassword || !params->mysqlPort || !params->mysqlDbName) {
        sqlThreadStatus = SQL_THREAD_ERROR;
        log_log("sqlThreadMain(): A required parameter is missing");
        return NULL;
    }

    if(!mysql_thread_safe()) {
        sqlThreadStatus = SQL_THREAD_ERROR;
        log_log("sqlThreadMain(): mysql_thread_safe() returned 0");
        return NULL;
    }

    if(mysql_thread_init()) {
        sqlThreadStatus = SQL_THREAD_ERROR;
        log_log("sqlThreadMain(): mysql_thread_init() returned an error");
        return NULL;
    }

    uuid_unparse(device_stats.device_uuid, uuidStr);
    memset(&previousTime, 0, sizeof(previousTime));

    while(1) {    
        if(!(mysql = mysql_init(NULL))) {
            sqlThreadStatus = SQL_THREAD_ERROR;
            log_log("sqlThreadMain(): mysql_init() failed");
            return sqlThreadCleanup();
        }

        sqlThreadStatus = SQL_THREAD_CONNECTING;

        if(!mysql_real_connect(mysql, params->mysqlHost, params->mysqlUsername, params->mysqlPassword, params->mysqlDbName, params->mysqlPort, NULL, 0)) {
            sqlThreadStatus = SQL_THREAD_ERROR;
            log_log("sqlThreadMain(): mysql_real_connect() failed, waiting 30 seconds before trying again");
            mysql_close(mysql);
            sleep(30);
            continue;
        }

        sqlThreadStatus = SQL_THREAD_CONNECTED;

        // Were we given a card name?
        if(!cardRegistered) {
            if(params->cardId) {
                snprintf(msg, sizeof(msg), "sqlThreadMain(): Forcing use of card ID %lu", params->cardId);
                log_log(msg);
            } else {
                if(result = sqlThreadFindCard(mysql, device_stats.device_uuid, &params->cardId)) {
                    if(result == -1) {
                        sqlThreadStatus = SQL_THREAD_ERROR;
                        return sqlThreadCleanup();
                    } else {
                        mysql_close(mysql);
                        sleep(30);
                        continue;
                    }
                }

                if(!params->cardId) {
                    if(!params->cardName) {
                        sqlThreadStatus = SQL_THREAD_ERROR;
                        snprintf(msg, sizeof(msg), "sqlThreadMain(): No card with UUID %s exists in the database and no card name provided", uuidStr);
                        log_log(msg);
                        return sqlThreadCleanup();
                    }

                    // Register the new card
                    if(result = sqlThreadInsertCard(mysql, params->cardName, &params->cardId)) {
                        if(result == -1) {
                            sqlThreadStatus = SQL_THREAD_ERROR;
                            return sqlThreadCleanup();
                        } else {
                            mysql_close(mysql);
                            sleep(30);
                            continue;
                        }
                    }
                } else {
                    if(result = sqlThreadUpdateCard(mysql, params->cardId)) {
                        if(result == -1) {
                            sqlThreadStatus = SQL_THREAD_ERROR;
                            return sqlThreadCleanup();
                        } else {
                            mysql_close(mysql);
                            sleep(30);
                            continue;
                        }
                    }

                    snprintf(msg, sizeof(msg), "sqlThreadMain(): UUID %s already exists in database with ID %lu", uuidStr, params->cardId);
                    log_log(msg);
                }
            }

            cardRegistered = 1;
        }

        do {
            if(result = sqlThreadUpdateSectorMap(mysql, params->cardId)) {
                if(result == -1) {
                    sqlThreadStatus = SQL_THREAD_ERROR;
                    return sqlThreadCleanup();
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
