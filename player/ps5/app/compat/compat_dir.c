/*
 * Directory reading on top of the kernel module's open() and getdents(). The first console
 * runs showed the system C library's opendir()/readdir() failing with "Operation not
 * permitted" inside the app's own data folder, so the app carries its own.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <dirent.h>

int getdents(int descriptor, char *buffer, int size);

struct app_dir
{
    int descriptor; /* first member: dirfd() in compat.c reads it */
    int length;     /* bytes of valid records in `records` */
    int position;   /* offset of the next record */
    char records[8192];
};

DIR *fdopendir(int descriptor)
{
    struct app_dir *directory = malloc(sizeof *directory);
    if (directory == NULL)
    {
        errno = ENOMEM;
        return NULL;
    }
    directory->descriptor = descriptor;
    directory->length = 0;
    directory->position = 0;
    return (DIR *)directory;
}

DIR *opendir(const char *path)
{
    int descriptor = open(path, O_RDONLY | O_DIRECTORY);
    if (descriptor < 0)
        return NULL;
    DIR *directory = fdopendir(descriptor);
    if (directory == NULL)
        close(descriptor);
    return directory;
}

struct dirent *readdir(DIR *handle)
{
    struct app_dir *directory = (struct app_dir *)handle;
    for (;;)
    {
        if (directory->position >= directory->length)
        {
            int count = getdents(directory->descriptor, directory->records,
                                 (int)sizeof directory->records);
            if (count <= 0)
                return NULL; /* 0: end of directory, errno untouched; <0: errno set */
            directory->length = count;
            directory->position = 0;
        }
        struct dirent *entry = (struct dirent *)(directory->records + directory->position);
        if (entry->d_reclen == 0)
        {
            directory->position = directory->length;
            continue;
        }
        directory->position += entry->d_reclen;
        if (entry->d_fileno != 0) /* a zero file number marks a removed entry */
            return entry;
    }
}

int closedir(DIR *handle)
{
    struct app_dir *directory = (struct app_dir *)handle;
    int result = close(directory->descriptor);
    free(directory);
    return result;
}
