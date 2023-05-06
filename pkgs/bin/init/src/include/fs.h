
#ifndef _FS_H
#define _FS_H 1

#include <limits.h>
#include <sys/mount.h>
#include <sys/inotify.h>

#define STATIC_FLAGS MS_NODEV | MS_RDONLY | MS_NOSUID | MS_NOEXEC

#define DEFAULT_DATAPART "/dev/sda"

#define INOTIFY_BUFFER_SIZE sizeof(struct inotify_event) + NAME_MAX + 1

void mount_fs(void);
void mount_datapart(void);
void wait_for_path(const char *path);

int fexists(const char *path);

#endif
