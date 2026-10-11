/*
 * Non-blocking sockets. The console refuses both FreeBSD ways of switching a socket to
 * non-blocking mode (ioctl FIONBIO and fcntl F_SETFL O_NONBLOCK answer "Permission
 * denied"); its own way is the SO_NBIO socket option. These wrappers translate, so code
 * written for FreeBSD keeps working.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SOCKET_OPTION_NON_BLOCKING 0x1200 /* SO_NBIO */

int _ioctl(int descriptor, unsigned long request, void *argument);
int _fcntl(int descriptor, int command, long argument);
int _close(int descriptor);

static int set_socket_non_blocking(int descriptor, int enabled)
{
    return setsockopt(descriptor, SOL_SOCKET, SOCKET_OPTION_NON_BLOCKING, &enabled,
                      sizeof enabled);
}

/* 1 or 0 for a socket, -1 when the descriptor is not a socket. */
static int socket_non_blocking(int descriptor)
{
    int enabled = 0;
    socklen_t length = sizeof enabled;
    int saved = errno;
    if (getsockopt(descriptor, SOL_SOCKET, SOCKET_OPTION_NON_BLOCKING, &enabled, &length) != 0)
    {
        errno = saved;
        return -1;
    }
    return enabled != 0;
}

/*
 * Descriptors the console will not duplicate. An event queue (kqueue) answers "Operation
 * not permitted" to dup and F_DUPFD, and tokio's I/O driver duplicates its queue when it
 * starts. Such a descriptor is shared instead: the "duplicate" is the same number, and the
 * count of extra owners kept here makes close() release it only when the last owner does.
 */
#define SHARED_DESCRIPTOR_LIMIT 4096
static unsigned char extra_owners[SHARED_DESCRIPTOR_LIMIT];

static int share_descriptor(int descriptor)
{
    if (descriptor < 0 || descriptor >= SHARED_DESCRIPTOR_LIMIT)
    {
        errno = EMFILE;
        return -1;
    }
    unsigned char owners = extra_owners[descriptor];
    do
    {
        if (owners == 255)
        {
            errno = EMFILE;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(&extra_owners[descriptor], &owners,
                                          (unsigned char)(owners + 1), 1, __ATOMIC_SEQ_CST,
                                          __ATOMIC_SEQ_CST));
    return descriptor;
}

/* Last resort after fcntl's duplicate commands failed. Seen on the console for an event
 * queue: F_DUPFD_CLOEXEC and F_DUPFD answer "Invalid argument", dup answers "Operation not
 * permitted". Only that last answer means the descriptor exists but may not be duplicated. */
static int duplicate_or_share(int descriptor)
{
    int duplicate = dup(descriptor);
    if (duplicate >= 0)
        return duplicate;
    if (errno == EPERM)
        return share_descriptor(descriptor);
    return -1;
}

int close(int descriptor)
{
    if (descriptor >= 0 && descriptor < SHARED_DESCRIPTOR_LIMIT)
    {
        unsigned char owners = extra_owners[descriptor];
        while (owners > 0)
        {
            if (__atomic_compare_exchange_n(&extra_owners[descriptor], &owners,
                                            (unsigned char)(owners - 1), 1, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST))
                return 0;
        }
    }
    return _close(descriptor);
}

int ioctl(int descriptor, unsigned long request, ...)
{
    __builtin_va_list arguments;
    __builtin_va_start(arguments, request);
    void *argument = __builtin_va_arg(arguments, void *);
    __builtin_va_end(arguments);

    int result = _ioctl(descriptor, request, argument);
    if (result != 0 && request == FIONBIO && errno == EACCES && argument != 0)
        return set_socket_non_blocking(descriptor, *(int *)argument != 0);
    return result;
}

int fcntl(int descriptor, int command, ...)
{
    __builtin_va_list arguments;
    __builtin_va_start(arguments, command);
    long argument = __builtin_va_arg(arguments, long);
    __builtin_va_end(arguments);

    if (command == F_SETFL)
    {
        int result = _fcntl(descriptor, command, argument);
        if (result == 0 || errno != EACCES)
            return result;
        /* Sockets: apply the non-blocking bit through the socket option. The remaining
         * status flags a socket can carry (O_APPEND, O_ASYNC) have no use here. */
        return set_socket_non_blocking(descriptor, (argument & O_NONBLOCK) != 0);
    }
#ifdef F_DUPFD_CLOEXEC
    if (command == F_DUPFD_CLOEXEC)
    {
        /* Rust's OwnedFd::try_clone (tokio clones its event queue this way). If the console
         * does not know the command, a plain duplicate does the job: close-on-exec has no
         * effect in this app. */
        int duplicate = _fcntl(descriptor, command, argument);
        if (duplicate >= 0)
            return duplicate;
        if (errno == EINVAL)
            duplicate = _fcntl(descriptor, F_DUPFD, argument);
        if (duplicate >= 0)
            return duplicate;
        return duplicate_or_share(descriptor);
    }
#endif
    if (command == F_DUPFD)
    {
        int duplicate = _fcntl(descriptor, command, argument);
        if (duplicate >= 0)
            return duplicate;
        return duplicate_or_share(descriptor);
    }
    int result = _fcntl(descriptor, command, argument);
    /* The console refuses close-on-exec on some descriptors (an event queue answers
     * "Invalid argument", which stopped tokio's I/O driver from starting). This app never
     * starts another program, so the flag has no effect and is reported as set. */
    if (command == F_SETFD && result != 0 && errno == EINVAL)
        return 0;
    if (command == F_GETFL && result >= 0 && socket_non_blocking(descriptor) == 1)
        result |= O_NONBLOCK;
    return result;
}
