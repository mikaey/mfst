#if !defined(SQL_H)
#define SQL_H

#include <inttypes.h>

typedef enum {
              SQL_THREAD_NOT_CONNECTED = 0, // No attempt to connect has been made
              SQL_THREAD_CONNECTING,
              SQL_THREAD_CONNECTED,
              SQL_THREAD_DISCONNECTED, // We were previously connected to the SQL server, but we've been disconnected
              SQL_THREAD_QUERY_EXECUTING,
              SQL_THREAD_ERROR
} SqlThreadStatusType;

typedef struct _SqlThreadParamsType {
    char *mysqlHost;
    char *mysqlUsername;
    char *mysqlPassword;
    int mysqlPort;
    char *mysqlDbName;
    char *cardName;
    uint64_t cardId;
} SqlThreadParamsType;

extern volatile SqlThreadStatusType sqlThreadStatus;

void *sqlThreadMain(void *arg);

#endif // !defined(SQL_H)
