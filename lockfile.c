#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "device_testing_context.h"
#include "lockfile.h"
#include "messages.h"
#include "mfst.h"

static int lockfile_fd = -1;

int open_lockfile(device_testing_context_type *device_testing_context, char *filename) {
    int local_errno;

    if((lockfile_fd = open(program_options.lock_file, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) == -1) {
        local_errno = errno;
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_OPEN_ERROR, strerror(errno));
        return local_errno;
    }

    return 0;
}

int is_lockfile_locked() {
    int retval = lockf(lockfile_fd, F_TEST, 0);
    return (retval == -1 && (errno == EACCES || errno == EAGAIN));
}

int lock_lockfile(device_testing_context_type *device_testing_context) {
    int local_errno;

    if(lockf(lockfile_fd, F_TLOCK, 0) == -1) {
        local_errno = errno;
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LOCKF_ERROR, strerror(errno));
        errno = local_errno;
        return -1;
    }

    return 0;
}

int unlock_lockfile(device_testing_context_type *device_testing_context) {
    int local_errno;

    if(lockf(lockfile_fd, F_ULOCK, 0) == -1) {
        local_errno = errno;
        log_log(device_testing_context, __func__, SEVERITY_LEVEL_DEBUG, MSG_LOCKF_ERROR, strerror(errno));
        errno = local_errno;
        return -1;
    }

    return 0;
}

void close_lockfile() {
    if(lockfile_fd != -1) {
        close(lockfile_fd);
        lockfile_fd = -1;
    }
}
