// Stand-ins for two functions that only the C library archive has.
//
// The console runs no other programs for an app. whisper.cpp's maths library would start
// a debugger to print a backtrace when it aborts; with this stand-in that simply fails.
// (The C library archive's own execlp cannot be used: it drags in system-call wrappers
// that clash with the app's stand-ins.)
#include <errno.h>

int execlp(const char *file, const char *argument, ...)
{
    (void)file;
    (void)argument;
    errno = ENOSYS;
    return -1;
}

// Hard links: the C++ library's file-system support refers to this, and nothing the app
// does needs it. (The same archive member as above would otherwise come in for it.)
int link(const char *existing, const char *added)
{
    (void)existing;
    (void)added;
    errno = ENOSYS;
    return -1;
}

// A path's limits: also only asked by the C++ library's file-system support.
long pathconf(const char *path, int name)
{
    (void)path;
    (void)name;
    errno = ENOSYS;
    return -1;
}
