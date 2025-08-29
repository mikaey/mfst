#if !defined(SQL_H)
#define SQL_H

#include <inttypes.h>

#include "device_testing_context.h"

typedef enum {
              SQL_THREAD_NOT_CONNECTED = 0, // No attempt to connect has been made
              SQL_THREAD_CONNECTING,
              SQL_THREAD_CONNECTED,
              SQL_THREAD_DISCONNECTED, // We were previously connected to the SQL server, but we've been disconnected
              SQL_THREAD_QUERY_EXECUTING,
              SQL_THREAD_ERROR
} sql_thread_status_type;

typedef struct _sql_thread_params_type {
    char *mysql_host;
    char *mysql_username;
    char *mysql_password;
    int mysql_port;
    char *mysql_db_name;
    char *card_name;
    device_testing_context_type *device_testing_context;
    uint64_t card_id;
} sql_thread_params_type;

extern volatile sql_thread_status_type sql_thread_status;

void *sql_thread_main(void *arg);

#endif // !defined(SQL_H)
