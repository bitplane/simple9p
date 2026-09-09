#include "namespace.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

void platform_namespace_cleanup(Namespace *ns) {
    (void)ns;
}

int platform_namespace_ready(Namespace *ns) {
    struct stat st;

    if(ns->synthetic) {
        errno = EINVAL;
        return -1;
    }
    return stat(ns->native_root, &st) < 0 || !S_ISDIR(st.st_mode) ? -1 : 0;
}

int platform_namespace_init(Namespace *ns) {
    return namespace_use_native(ns, "/");
}

int platform_namespace_discover(Namespace *ns) {
    (void)ns;
    return 0;
}

static int path_allowed(const ResolvedPath *path) {
    if(!path || path->synthetic || !path->native_path[0]) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

int platform_lstat(const ResolvedPath *path, struct stat *st) {
    return path_allowed(path) ? lstat(path->native_path, st) : -1;
}

int platform_lstat_child(DIR *directory, const ResolvedPath *path,
                         const char *name, struct stat *st) {
    (void)directory;
    (void)name;
    return platform_lstat(path, st);
}

int platform_open(const ResolvedPath *path, int flags, mode_t mode) {
    return path_allowed(path) ? open(path->native_path, flags, mode) : -1;
}

DIR *platform_opendir(const ResolvedPath *path) {
    return path_allowed(path) ? opendir(path->native_path) : NULL;
}

ssize_t platform_readlink(const ResolvedPath *path, char *buffer, size_t size) {
    return path_allowed(path) ? readlink(path->native_path, buffer, size) : -1;
}

int platform_access_execute(const ResolvedPath *path) {
    return path_allowed(path) ? access(path->native_path, X_OK) : -1;
}

int platform_mkdir(const ResolvedPath *path, mode_t mode) {
    return path_allowed(path) ? mkdir(path->native_path, mode) : -1;
}

int platform_symlink(const char *target, const ResolvedPath *path) {
    return path_allowed(path) ? symlink(target, path->native_path) : -1;
}

int platform_mknod(const ResolvedPath *path, mode_t mode, unsigned major,
                   unsigned minor) {
    (void)major;
    (void)minor;
    if(!path_allowed(path))
        return -1;
    if(S_ISFIFO(mode))
        return mkfifo(path->native_path, mode & 07777);
    errno = EOPNOTSUPP;
    return -1;
}

int platform_remove(const ResolvedPath *path, int directory) {
    if(!path_allowed(path))
        return -1;
    return directory ? rmdir(path->native_path) : unlink(path->native_path);
}

int platform_rename(const ResolvedPath *old_path,
                    const ResolvedPath *new_path) {
    if(!path_allowed(old_path) || !path_allowed(new_path))
        return -1;
    return rename(old_path->native_path, new_path->native_path);
}

int platform_chmod(const ResolvedPath *path, mode_t mode) {
    return path_allowed(path) ? chmod(path->native_path, mode) : -1;
}

int platform_chown(const ResolvedPath *path, uid_t uid, gid_t gid) {
    return path_allowed(path) ? chown(path->native_path, uid, gid) : -1;
}

int platform_device_spec(const struct stat *st, char *buffer, size_t size) {
    (void)st;
    (void)buffer;
    (void)size;
    errno = EINVAL;
    return -1;
}

int platform_set_times(const ResolvedPath *path, time_t atime, time_t mtime) {
    struct utimbuf times;

    if(!path_allowed(path))
        return -1;
    times.actime = atime;
    times.modtime = mtime;
    return utime(path->native_path, &times);
}

int platform_truncate(const ResolvedPath *path, off_t length) {
    return path_allowed(path) ? truncate(path->native_path, length) : -1;
}
