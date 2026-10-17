/*
 * Functions the SDK only finds in libScePosixForWebKit or libkernel_sys. Game processes do
 * not load those modules, so an import from them is a null pointer at run time; the
 * definitions below keep every one of them inside the app.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

void pthread_set_name_np(pthread_t thread, const char *name);

static int unsupported(void)
{
    errno = ENOSYS;
    return -1;
}

/*
 * Random bytes come from the kernel's generator (the kern.arandom sysctl, which FreeBSD 11's
 * own C library uses), with /dev/urandom as a second source. The console's random service
 * (libSceRandom) is not used: its import was a null pointer in a game process.
 * Returns 0 when every byte was filled.
 */
static int fill_random(void *buffer, size_t size)
{
    unsigned char *cursor = buffer;
    int name[2] = {CTL_KERN, KERN_ARND};
    while (size > 0)
    {
        size_t chunk = size < 256 ? size : 256;
        size_t filled = chunk;
        if (sysctl(name, 2, cursor, &filled, NULL, 0) != 0 || filled == 0)
            break;
        cursor += filled;
        size -= filled;
    }
    if (size > 0)
    {
        int descriptor = open("/dev/urandom", O_RDONLY);
        if (descriptor >= 0)
        {
            while (size > 0)
            {
                ssize_t count = read(descriptor, cursor, size);
                if (count <= 0)
                    break;
                cursor += count;
                size -= (size_t)count;
            }
            close(descriptor);
        }
    }
    return size == 0 ? 0 : -1;
}

void arc4random_buf(void *buffer, size_t size)
{
    /* This call cannot report failure, and handing back predictable bytes to code that
     * asked for random ones is worse than stopping. */
    if (fill_random(buffer, size) != 0)
        __builtin_trap();
}

/* The rest of the arc4random family, so that the C library archive's version (which
 * defines arc4random_buf as well) is never pulled in beside this one. */
uint32_t arc4random(void)
{
    uint32_t value;
    arc4random_buf(&value, sizeof value);
    return value;
}

uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2)
        return 0;
    /* Reject the values that would make some results more likely than others. */
    const uint32_t minimum = (uint32_t)(-bound) % bound;
    for (;;)
    {
        const uint32_t value = arc4random();
        if (value >= minimum)
            return value % bound;
    }
}

/* FreeBSD 12's entropy call, which the console's FreeBSD 11 kernel module lacks. */
ssize_t getrandom(void *buffer, size_t size, unsigned int flags)
{
    (void)flags;
    if (fill_random(buffer, size) != 0)
    {
        errno = EIO;
        return -1;
    }
    return (ssize_t)size;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    pthread_set_name_np(thread, name);
    return 0;
}

/* Name resolution through the console's resolver (IPv4, one address). */
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceNetResolverCreate(const char *name, int pool, int flags);
int sceNetResolverStartNtoa(int resolver, const char *host, struct in_addr *address,
                            int timeout, int retries, int flags);
int sceNetResolverDestroy(int resolver);

static int resolve_ipv4(const char *host, struct in_addr *address)
{
    if (inet_pton(AF_INET, host, address) == 1)
        return 0;
    int pool = sceNetPoolCreate("resolver", 16 * 1024, 0);
    if (pool < 0)
        return EAI_FAIL;
    int result = EAI_FAIL;
    int resolver = sceNetResolverCreate("resolver", pool, 0);
    if (resolver >= 0)
    {
        if (sceNetResolverStartNtoa(resolver, host, address, 0, 0, 0) >= 0)
            result = 0;
        else
            result = EAI_NONAME;
        sceNetResolverDestroy(resolver);
    }
    sceNetPoolDestroy(pool);
    return result;
}

int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    *result = NULL;
    if (hints != NULL && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;
    struct in_addr address;
    if (host == NULL)
        address.s_addr = htonl((hints != NULL && (hints->ai_flags & AI_PASSIVE))
                                   ? INADDR_ANY
                                   : INADDR_LOOPBACK);
    else
    {
        int status = resolve_ipv4(host, &address);
        if (status != 0)
            return status;
    }
    struct addrinfo *entry = calloc(1, sizeof *entry + sizeof(struct sockaddr_in));
    if (entry == NULL)
        return EAI_MEMORY;
    struct sockaddr_in *socket_address = (struct sockaddr_in *)(entry + 1);
    socket_address->sin_len = sizeof *socket_address;
    socket_address->sin_family = AF_INET;
    socket_address->sin_port = htons(service != NULL ? (unsigned short)atoi(service) : 0);
    socket_address->sin_addr = address;
    entry->ai_family = AF_INET;
    entry->ai_socktype =
        hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
    entry->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
    entry->ai_addrlen = sizeof *socket_address;
    entry->ai_addr = (struct sockaddr *)socket_address;
    *result = entry;
    return 0;
}

void freeaddrinfo(struct addrinfo *entry)
{
    while (entry != NULL)
    {
        struct addrinfo *next = entry->ai_next;
        free(entry);
        entry = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code)
    {
    case EAI_NONAME:
        return "host name not found";
    case EAI_FAMILY:
        return "address family not supported";
    case EAI_MEMORY:
        return "out of memory";
    default:
        return "name resolution failed";
    }
}

/*
 * Directory-relative calls. Only absolute paths and paths relative to the working
 * directory can be served by the path-based calls the kernel module exports.
 */
static int path_based(int directory, const char *path)
{
    return directory == AT_FDCWD || path[0] == '/';
}

int openat(int directory, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT)
    {
        __builtin_va_list arguments;
        __builtin_va_start(arguments, flags);
        mode = (mode_t)__builtin_va_arg(arguments, int);
        __builtin_va_end(arguments);
    }
    if (!path_based(directory, path))
        return unsupported();
    return open(path, flags, mode);
}

int fstatat(int directory, const char *path, struct stat *status, int flags)
{
    if (!path_based(directory, path))
        return unsupported();
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat(path, status) : stat(path, status);
}

int unlinkat(int directory, const char *path, int flags)
{
    if (!path_based(directory, path))
        return unsupported();
    return (flags & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int mkdirat(int directory, const char *path, mode_t mode)
{
    if (!path_based(directory, path))
        return unsupported();
    return mkdir(path, mode);
}

int renameat(int from_directory, const char *from, int to_directory, const char *to)
{
    if (!path_based(from_directory, from) || !path_based(to_directory, to))
        return unsupported();
    return rename(from, to);
}

int linkat(int from_directory, const char *from, int to_directory, const char *to, int flags)
{
    (void)from_directory;
    (void)from;
    (void)to_directory;
    (void)to;
    (void)flags;
    return unsupported();
}

int fchmodat(int directory, const char *path, mode_t mode, int flags)
{
    (void)directory;
    (void)path;
    (void)mode;
    (void)flags;
    return unsupported();
}

int symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;
    return unsupported();
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
    (void)path;
    (void)buffer;
    (void)size;
    errno = EINVAL; /* nothing on the app's paths is a symbolic link */
    return -1;
}

int chown(const char *path, uid_t user, gid_t group)
{
    (void)path;
    (void)user;
    (void)group;
    return unsupported();
}

int lchown(const char *path, uid_t user, gid_t group)
{
    (void)path;
    (void)user;
    (void)group;
    return unsupported();
}

int fchown(int descriptor, uid_t user, gid_t group)
{
    (void)descriptor;
    (void)user;
    (void)group;
    return unsupported();
}

int chroot(const char *path)
{
    (void)path;
    return unsupported();
}

pid_t fork(void)
{
    return unsupported();
}

int setpgid(pid_t process, pid_t group)
{
    (void)process;
    (void)group;
    return unsupported();
}

pid_t setsid(void)
{
    return unsupported();
}
