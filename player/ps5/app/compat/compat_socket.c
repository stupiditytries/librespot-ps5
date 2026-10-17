/*
 * Non-blocking sockets. The console refuses both FreeBSD ways of switching a socket to
 * non-blocking mode (ioctl FIONBIO and fcntl F_SETFL O_NONBLOCK answer "Permission
 * denied"); its own way is the SO_NBIO socket option. These wrappers translate, so code
 * written for FreeBSD keeps working.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SOCKET_OPTION_NON_BLOCKING 0x1200 /* SO_NBIO */

int _ioctl(int descriptor, unsigned long request, void *argument);
int _fcntl(int descriptor, int command, long argument);

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
    int result = _fcntl(descriptor, command, argument);
    if (command == F_GETFL && result >= 0 && socket_non_blocking(descriptor) == 1)
        result |= O_NONBLOCK;
    return result;
}
