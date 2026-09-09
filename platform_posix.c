#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#if defined(__NetBSD__) && !defined(_NETBSD_SOURCE)
#define _NETBSD_SOURCE
#endif

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#ifdef S9_NO_DUPFD_CLOEXEC
#undef F_DUPFD_CLOEXEC
#endif

static int native_root = -1;

void platform_namespace_cleanup(Namespace *ns) {
    (void)ns;
    if(native_root >= 0) {
        close(native_root);
        native_root = -1;
    }
}

int platform_namespace_ready(Namespace *ns) {
    platform_namespace_cleanup(ns);
    if(!ns->synthetic) {
        native_root = open(ns->native_root,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        return native_root < 0 ? -1 : 0;
    }
    return 0;
}

static int root_descriptor(const ResolvedPath *path) {
    int root;

    if(namespace.synthetic)
        return open(path->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    root = native_root;

#ifdef F_DUPFD_CLOEXEC
    return fcntl(root, F_DUPFD_CLOEXEC, 0);
#else
    {
        int descriptor = dup(root);

        if(descriptor >= 0 && fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
            int error = errno;
            close(descriptor);
            errno = error;
            return -1;
        }
        return descriptor;
    }
#endif
}

static int open_parent(const ResolvedPath *path, char *leaf,
                       size_t leaf_size) {
    char relative[S9_PATH_MAX];
    char *component;
    char *next;
    int directory;

    directory = root_descriptor(path);
    if(directory < 0)
        return -1;
    if(!path->relative_path[0]) {
        if(leaf_size < 2) {
            close(directory);
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(leaf, ".");
        return directory;
    }
    if(strlen(path->relative_path) >= sizeof(relative)) {
        close(directory);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(relative, path->relative_path);
    component = relative;
    for(;;) {
        int child;

        next = strchr(component, '/');
        if(!next)
            break;
        *next = '\0';
        child = openat(directory, component,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if(child < 0) {
            int error = errno;
            close(directory);
            errno = error;
            return -1;
        }
        close(directory);
        directory = child;
        component = next + 1;
    }
    if(strlen(component) >= leaf_size) {
        close(directory);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(leaf, component);
    return directory;
}

int platform_lstat(const ResolvedPath *path, struct stat *st) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = fstatat(parent, leaf, st, AT_SYMLINK_NOFOLLOW);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_lstat_child(DIR *directory, const ResolvedPath *path,
                         const char *name, struct stat *st) {
    (void)path;
    return fstatat(dirfd(directory), name, st, AT_SYMLINK_NOFOLLOW);
}

int platform_open(const ResolvedPath *path, int flags, mode_t mode) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int descriptor;
    int error;

    if(parent < 0)
        return -1;
    descriptor = openat(parent, leaf, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    error = errno;
    close(parent);
    errno = error;
    return descriptor;
}

DIR *platform_opendir(const ResolvedPath *path) {
    int descriptor = platform_open(path, O_RDONLY | O_DIRECTORY, 0);
    DIR *directory;

    if(descriptor < 0)
        return NULL;
    directory = fdopendir(descriptor);
    if(!directory) {
        int error = errno;
        close(descriptor);
        errno = error;
    }
    return directory;
}

ssize_t platform_readlink(const ResolvedPath *path, char *buffer, size_t size) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    ssize_t result;
    int error;

    if(parent < 0)
        return -1;
    result = readlinkat(parent, leaf, buffer, size);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_access_execute(const ResolvedPath *path) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = faccessat(parent, leaf, X_OK, 0);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_mkdir(const ResolvedPath *path, mode_t mode) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = mkdirat(parent, leaf, mode);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_symlink(const char *target, const ResolvedPath *path) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = symlinkat(target, parent, leaf);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_remove(const ResolvedPath *path, int directory) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = unlinkat(parent, leaf, directory ? AT_REMOVEDIR : 0);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_rename(const ResolvedPath *old_path,
                    const ResolvedPath *new_path) {
    char old_leaf[S9_PATH_MAX];
    char new_leaf[S9_PATH_MAX];
    int old_parent = open_parent(old_path, old_leaf, sizeof(old_leaf));
    int new_parent;
    int result;
    int error;

    if(old_parent < 0)
        return -1;
    new_parent = open_parent(new_path, new_leaf, sizeof(new_leaf));
    if(new_parent < 0) {
        error = errno;
        close(old_parent);
        errno = error;
        return -1;
    }
    result = renameat(old_parent, old_leaf, new_parent, new_leaf);
    error = errno;
    close(old_parent);
    close(new_parent);
    errno = error;
    return result;
}

int platform_chmod(const ResolvedPath *path, mode_t mode) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = fchmodat(parent, leaf, mode, AT_SYMLINK_NOFOLLOW);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_chown(const ResolvedPath *path, uid_t uid, gid_t gid) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = fchownat(parent, leaf, uid, gid, AT_SYMLINK_NOFOLLOW);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_device_spec(const struct stat *st, char *buffer, size_t size) {
    char type;

    if(S_ISCHR(st->st_mode))
        type = 'c';
    else if(S_ISBLK(st->st_mode))
        type = 'b';
    else {
        errno = EINVAL;
        return -1;
    }
    if(snprintf(buffer, size, "%c %u %u", type,
                (unsigned)major(st->st_rdev),
                (unsigned)minor(st->st_rdev)) >= (int)size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int platform_set_times(const ResolvedPath *path, time_t atime, time_t mtime) {
    char leaf[S9_PATH_MAX];
    struct timespec times[2];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    times[0].tv_sec = atime;
    times[0].tv_nsec = 0;
    times[1].tv_sec = mtime;
    times[1].tv_nsec = 0;
    result = utimensat(parent, leaf, times, AT_SYMLINK_NOFOLLOW);
    error = errno;
    close(parent);
    errno = error;
    return result;
}

int platform_truncate(const ResolvedPath *path, off_t length) {
    int descriptor = platform_open(path, O_WRONLY, 0);
    int result;
    int error;

    if(descriptor < 0)
        return -1;
    result = ftruncate(descriptor, length);
    error = errno;
    close(descriptor);
    errno = error;
    return result;
}

int platform_namespace_init(Namespace *ns) {
    return namespace_use_native(ns, "/");
}

int platform_namespace_discover(Namespace *ns) {
    (void)ns;
    return 0;
}
