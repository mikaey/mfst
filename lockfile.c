#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lockfile.h"
#include "mfst.h"

static int lockfile_fd = -1;

int open_lockfile(char *filename) {
    if((lockfile_fd = open(program_options.lock_file, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR)) == -1) {
        return errno;
    }

    return 0;
}

int is_lockfile_locked() {
    int retval = lockf(lockfile_fd, F_TEST, 0);
    return (retval == -1 && (errno == EACCES || errno == EAGAIN));
}

int lock_lockfile() {
    return lockf(lockfile_fd, F_TLOCK, 0);
}

int unlock_lockfile() {
    return lockf(lockfile_fd, F_ULOCK, 0);
}

void close_lockfile() {
    if(lockfile_fd != -1) {
        close(lockfile_fd);
        lockfile_fd = -1;
    }
}
