/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Installs only this app's signed folder. No system database or registry
 * writes. */
#include "app_bundle.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef APP_ROOT
#define APP_ROOT "/data/homebrew/PPSA99730"
#endif
/* The console refuses to exec a SELF without execute permission. Embedded
 * bytes do not carry the mode of the original build file. */
static mode_t app_mode(const char *name) {
  const size_t n = strlen(name);
  return !strcmp(name, "eboot.bin") ||
                 (!strncmp(name, "sce_module/", 11) && n >= 4 &&
                  !strcmp(name + n - 4, ".prx"))
             ? 0755
             : 0644;
}
static int parents(char *path) {
  for (char *p = path + 1; *p; ++p)
    if (*p == '/') {
      *p = 0;
      int rc = mkdir(path, 0777);
      *p = '/';
      if (rc && errno != EEXIST)
        return -1;
    }
  return 0;
}
/* Files are replaced individually while holding the same inode lock the app
 * holds for its whole lifetime. The manifest is published last; failures retry
 * on the next payload launch. Only compiled-in app paths can be written. */
int librespot_install_app(void) {
  char root[] = APP_ROOT "/.player-lock";
  if (parents(root))
    return -1;
  int lock = open(root, O_RDONLY | O_CREAT, 0666);
  if (lock < 0)
    return -1;
  /* An open app holds this lock. The update still goes ahead: every file is replaced by an
   * atomic rename, so the running app keeps the files it already has, and nothing can launch
   * a second copy while it runs. It is told to reopen to use them. Refusing here left the old
   * app on the console, unnoticed, through several builds. */
  const int held = flock(lock, LOCK_EX | LOCK_NB) == 0;
  char version[80] = {0};
  FILE *current = fopen(APP_ROOT "/.librespot-version", "rb");
  if (current) {
    fread(version, 1, sizeof version - 1, current);
    fclose(current);
  }
  int complete = !strcmp(version, librespot_app_version);
  for (unsigned i = 0; complete && i < librespot_app_file_count; ++i) {
    char path[320];
    struct stat info;
    snprintf(path, sizeof path, APP_ROOT "/%s", librespot_app_files[i].name);
    if (lstat(path, &info) || !S_ISREG(info.st_mode) ||
        (size_t)info.st_size != librespot_app_files[i].size ||
        (info.st_mode & 0777) != app_mode(librespot_app_files[i].name))
      complete = 0;
  }
  if (complete) {
    if (held)
      flock(lock, LOCK_UN);
    close(lock);
    return 0;
  }
  for (unsigned i = 0; i < librespot_app_file_count; ++i) {
    const struct app_file *file = &librespot_app_files[i];
    if (strstr(file->name, "..") || file->name[0] == '/')
      goto failed;
    char path[320], partial[336];
    snprintf(path, sizeof path, APP_ROOT "/%s", file->name);
    snprintf(partial, sizeof partial, "%s.partial", path);
    if (parents(path))
      goto failed;
    FILE *out = fopen(partial, "wb");
    if (!out)
      goto failed;
    const int written = fwrite(file->data, 1, file->size, out) == file->size;
    const int flushed = fflush(out) == 0;
    const int permissioned = fchmod(fileno(out), app_mode(file->name)) == 0;
    const int synced = fsync(fileno(out)) == 0;
    const int closed = fclose(out) == 0;
    if (!written || !flushed || !permissioned || !synced || !closed ||
        rename(partial, path))
      goto failed;
  }
  {
    FILE *out = fopen(APP_ROOT "/.librespot-version.partial", "wb");
    if (!out)
      goto failed;
    const int ok = fputs(librespot_app_version, out) >= 0;
    const int closed = fclose(out) == 0;
    if (!ok || !closed ||
        rename(APP_ROOT "/.librespot-version.partial",
               APP_ROOT "/.librespot-version"))
      goto failed;
  }
  if (held)
    flock(lock, LOCK_UN);
  close(lock);
  return held ? 0 : 1; /* 1: updated while the app was open; it must be reopened */
failed:
  if (held)
    flock(lock, LOCK_UN);
  close(lock);
  return -1;
}
