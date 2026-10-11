/*
 * Stand-ins for the few functions the Rust library asks for that the SDK's C library does
 * not have. An earlier build as an app needed many more; most of them duplicate what
 * this SDK already provides, so the background program carries only what was missing.
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* The console gives a background program an empty host name. The announcement that lets
 * the Spotify app find the receiver names the host it is on; with an empty name the
 * announcement was malformed and no phone listed the receiver (2026-10-11). */
int gethostname(char *name, size_t size)
{
    static const char host[] = "PS5-Connect";
    if (size < sizeof host)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t at = 0; at < sizeof host; at++)
        name[at] = host[at];
    return 0;
}

/* The network library asks for "as long a queue of waiting connections as the system
 * allows" by passing -1, as FreeBSD accepts. On the console a listener made that way
 * refused every connection (2026-10-11: the pairing port, on two different numbers, while
 * announcing itself correctly). A plain number is passed instead. */
int _listen(int socket_, int backlog);
int listen(int socket_, int backlog)
{
    if (backlog < 1 || backlog > 128)
        backlog = 128;
    return _listen(socket_, backlog);
}

/* The pairing server (librespot's) listens on the IPv6 "any" address and counts on that
 * taking IPv4 connections as well, as it does on Linux. Here, as on FreeBSD, an IPv6
 * socket takes IPv6 only unless told otherwise, so the phone, which is given the console's
 * IPv4 address, was refused (2026-10-11). A listening (stream) socket bound to an IPv6
 * address is told to take both. Datagram sockets are left alone: the announcement service
 * has separate ones for the two kinds of address on the same port. */
int _bind(int socket_, const struct sockaddr *address, socklen_t length);
int bind(int socket_, const struct sockaddr *address, socklen_t length)
{
    if (address != NULL && address->sa_family == AF_INET6)
    {
        int type = 0;
        socklen_t size = sizeof type;
        if (getsockopt(socket_, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_STREAM)
        {
            int only = 0;
            setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof only);
        }
    }
    return _bind(socket_, address, length);
}

int pthread_setname_np(void *thread, const char *name)
{
    (void)thread;
    (void)name;
    return 0;
}

static int apply_flags(int descriptor, int close_on_exec, int non_blocking)
{
    if (close_on_exec && fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0)
        return -1;
    if (non_blocking && fcntl(descriptor, F_SETFL, O_NONBLOCK) != 0)
        return -1;
    return 0;
}

int accept4(int socket_, struct sockaddr *address, socklen_t *length, int flags)
{
    int descriptor = accept(socket_, address, length);
    if (descriptor < 0)
        return -1;
    if (apply_flags(descriptor, flags & SOCK_CLOEXEC, flags & SOCK_NONBLOCK) != 0)
    {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

int dup3(int from, int to, int flags)
{
    if (from == to)
    {
        errno = EINVAL;
        return -1;
    }
    int descriptor = dup2(from, to);
    if (descriptor >= 0 && (flags & O_CLOEXEC))
        fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    return descriptor;
}

int killpg(pid_t group, int signal_)
{
    return kill(-group, signal_);
}

ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
    (void)flags;
    arc4random_buf(buffer, size);
    return (ssize_t)size;
}

/* Who is at the other end of a local socket: not available, and not needed here. */
int getpeereid(int socket_, uid_t *user, gid_t *group)
{
    (void)socket_;
    *user = 0;
    *group = 0;
    return 0;
}

/* Walks the loaded program's segments (used for backtraces): reports none. */
int dl_iterate_phdr(int (*callback)(void *, size_t, void *), void *data)
{
    (void)callback;
    (void)data;
    return 0;
}

/* Process and kernel inspection (the sysinfo crate, pulled in by librespot for its user
 * agent): nothing to report. */
void *kvm_openfiles(const char *a, const char *b, const char *c, int d, char *error)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    if (error != NULL)
        error[0] = 0;
    return NULL;
}
int kvm_close(void *kernel) { (void)kernel; return -1; }
void *kvm_getprocs(void *kernel, int a, int b, int *count)
{
    (void)kernel;
    (void)a;
    (void)b;
    if (count != NULL)
        *count = 0;
    return NULL;
}
char **kvm_getargv(void *kernel, const void *process, int limit)
{
    (void)kernel;
    (void)process;
    (void)limit;
    return NULL;
}
char **kvm_getenvv(void *kernel, const void *process, int limit)
{
    (void)kernel;
    (void)process;
    (void)limit;
    return NULL;
}
int kvm_getswapinfo(void *kernel, void *info, int max, int flags)
{
    (void)kernel;
    (void)info;
    (void)max;
    (void)flags;
    return -1;
}
void *procstat_open_sysctl(void) { return NULL; }
void procstat_close(void *handle) { (void)handle; }
void *procstat_getfiles(void *handle, void *process, int mmapped)
{
    (void)handle;
    (void)process;
    (void)mmapped;
    return NULL;
}
void procstat_freefiles(void *handle, void *files)
{
    (void)handle;
    (void)files;
}

/* Stack unwinding. The Rust library is built to abort on a panic, so nothing unwinds; the
 * standard library still refers to these names. */
uintptr_t _Unwind_GetIPInfo(void *context, int *before) { (void)context; *before = 0; return 0; }
uintptr_t _Unwind_GetCFA(void *context) { (void)context; return 0; }
uintptr_t _Unwind_GetRegionStart(void *context) { (void)context; return 0; }
uintptr_t _Unwind_GetTextRelBase(void *context) { (void)context; return 0; }
uintptr_t _Unwind_GetDataRelBase(void *context) { (void)context; return 0; }
void *_Unwind_GetLanguageSpecificData(void *context) { (void)context; return NULL; }
void *_Unwind_FindEnclosingFunction(void *address) { (void)address; return NULL; }
void _Unwind_SetIP(void *context, uintptr_t value) { (void)context; (void)value; }
void _Unwind_SetGR(void *context, int index, uintptr_t value)
{
    (void)context;
    (void)index;
    (void)value;
}
