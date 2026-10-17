/*
 * FreeBSD libc functions that Rust's standard library references and the console's
 * modules do not export. Functions the app can reach get a real implementation; the rest
 * (process spawning, credentials, FIFOs) fail with ENOSYS.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining)
{
    struct timespec interval = *request;
    if (flags & TIMER_ABSTIME)
    {
        struct timespec now;
        if (clock_gettime(clock, &now) != 0)
            return errno;
        interval.tv_sec -= now.tv_sec;
        interval.tv_nsec -= now.tv_nsec;
        if (interval.tv_nsec < 0)
        {
            interval.tv_nsec += 1000000000L;
            interval.tv_sec -= 1;
        }
        if (interval.tv_sec < 0)
            return 0;
        remaining = NULL;
    }
    return nanosleep(&interval, remaining) == 0 ? 0 : errno;
}

#undef dirfd
int dirfd(DIR *directory)
{
    /* FreeBSD's DIR starts with its descriptor. */
    return *(int *)directory;
}

static int apply_descriptor_flags(int descriptor, int flags)
{
    if ((flags & O_CLOEXEC) && fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
        return -1;
    if ((flags & O_NONBLOCK) && fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0)
        return -1;
    return 0;
}

int accept4(int socket, struct sockaddr *address, socklen_t *length, int flags)
{
    int descriptor = accept(socket, address, length);
    if (descriptor < 0)
        return -1;
    int descriptor_flags = 0;
    if (flags & SOCK_CLOEXEC)
        descriptor_flags |= O_CLOEXEC;
    if (flags & SOCK_NONBLOCK)
        descriptor_flags |= O_NONBLOCK;
    if (apply_descriptor_flags(descriptor, descriptor_flags) != 0)
    {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

int pipe2(int descriptors[2], int flags)
{
    if (pipe(descriptors) != 0)
        return -1;
    if (apply_descriptor_flags(descriptors[0], flags) != 0 ||
        apply_descriptor_flags(descriptors[1], flags) != 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    return 0;
}

static int unsupported(void)
{
    errno = ENOSYS;
    return -1;
}

int futimens(int descriptor, const struct timespec times[2])
{
    (void)descriptor;
    (void)times;
    return unsupported();
}

int utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
    (void)directory;
    (void)path;
    (void)times;
    (void)flags;
    return unsupported();
}

int getpeereid(int socket, uid_t *user, gid_t *group)
{
    (void)socket;
    (void)user;
    (void)group;
    return unsupported();
}

int getpwuid_r(uid_t user, struct passwd *entry, char *buffer, size_t size,
               struct passwd **result)
{
    (void)user;
    (void)entry;
    (void)buffer;
    (void)size;
    *result = NULL;
    return ENOSYS;
}

int killpg(pid_t group, int signal)
{
    (void)group;
    (void)signal;
    return unsupported();
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return unsupported();
}

int setgid(gid_t group)
{
    (void)group;
    return unsupported();
}

int posix_spawn_file_actions_addchdir(void *actions, const char *path)
{
    (void)actions;
    (void)path;
    return ENOSYS;
}

int posix_spawn_file_actions_addchdir_np(void *actions, const char *path)
{
    (void)actions;
    (void)path;
    return ENOSYS;
}

int dl_iterate_phdr(int (*callback)(void *, size_t, void *), void *data)
{
    (void)callback;
    (void)data;
    return 0;
}

/*
 * Unwinder accessors. The app is built with panic=abort, so nothing unwinds; these only
 * satisfy the standard library's backtrace code, which then reports empty frames. A build
 * that links the real unwinder defines APP_HAS_UNWINDER and gets its versions instead.
 */
#ifndef APP_HAS_UNWINDER
struct _Unwind_Context;

void *_Unwind_FindEnclosingFunction(void *address)
{
    (void)address;
    return NULL;
}

unsigned long _Unwind_GetCFA(struct _Unwind_Context *context)
{
    (void)context;
    return 0;
}

unsigned long _Unwind_GetDataRelBase(struct _Unwind_Context *context)
{
    (void)context;
    return 0;
}

unsigned long _Unwind_GetIPInfo(struct _Unwind_Context *context, int *before_instruction)
{
    (void)context;
    *before_instruction = 0;
    return 0;
}

void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *context)
{
    (void)context;
    return NULL;
}

unsigned long _Unwind_GetRegionStart(struct _Unwind_Context *context)
{
    (void)context;
    return 0;
}

unsigned long _Unwind_GetTextRelBase(struct _Unwind_Context *context)
{
    (void)context;
    return 0;
}

void _Unwind_SetGR(struct _Unwind_Context *context, int index, unsigned long value)
{
    (void)context;
    (void)index;
    (void)value;
}

void _Unwind_SetIP(struct _Unwind_Context *context, unsigned long value)
{
    (void)context;
    (void)value;
}
#endif /* APP_HAS_UNWINDER */
