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

static void cache_drop(void);

void platform_namespace_cleanup(Namespace *ns) {
    (void)ns;
    cache_drop();
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

static int dup_cloexec(int descriptor) {
#ifdef F_DUPFD_CLOEXEC
    return fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
#else
    int copy = dup(descriptor);

    if(copy >= 0 && fcntl(copy, F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno;
        close(copy);
        errno = error;
        return -1;
    }
    return copy;
#endif
}

static int root_descriptor(const ResolvedPath *path) {
    if(namespace.synthetic)
        return open(path->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return dup_cloexec(native_root);
}

/*
 * One-entry cache of the last parent directory opened. Without it a walk
 * of depth N reopens every component from the root at each step, N squared
 * openat calls in all; with it each step extends the previous one by one
 * openat. Before the entry is used, the directory at its path is checked
 * against the cached inode, so a rename underneath the server, by 9d or by
 * anyone else, costs a cache miss rather than a wrong answer. Holding the
 * descriptor also stops the inode number being reused meanwhile.
 */
static struct {
    int fd;
    dev_t dev;
    ino_t ino;
    char root[S9_PATH_MAX];
    char relative[S9_PATH_MAX];
} parent_cache = { -1, 0, 0, "", "" };

static void cache_drop(void) {
    if(parent_cache.fd >= 0)
        close(parent_cache.fd);
    parent_cache.fd = -1;
}

static const char *root_key(const ResolvedPath *path) {
    return namespace.synthetic && path->root_path ? path->root_path : "";
}

static void cache_store(const ResolvedPath *path, const char *relative,
                        int directory) {
    struct stat st;
    int copy;

    cache_drop();
    if(strlen(root_key(path)) >= sizeof(parent_cache.root) ||
       strlen(relative) >= sizeof(parent_cache.relative))
        return;
    if(fstat(directory, &st) < 0)
        return;
    copy = dup_cloexec(directory);
    if(copy < 0)
        return;
    strcpy(parent_cache.root, root_key(path));
    strcpy(parent_cache.relative, relative);
    parent_cache.dev = st.st_dev;
    parent_cache.ino = st.st_ino;
    parent_cache.fd = copy;
}

/*
 * If the cache covers `relative` or a prefix of it, return a fresh
 * descriptor for the cached directory and point *remainder past the covered
 * part (NULL when it covers all of it). Returns -1 on a miss.
 */
static int cache_take(const ResolvedPath *path, int root, const char *relative,
                      const char **remainder) {
    struct stat st;
    size_t length;

    if(parent_cache.fd < 0 || strcmp(root_key(path), parent_cache.root) != 0)
        return -1;
    length = strlen(parent_cache.relative);
    if(strncmp(relative, parent_cache.relative, length) != 0 ||
       (relative[length] != '\0' && relative[length] != '/'))
        return -1;
    if(fstatat(root, parent_cache.relative, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
       st.st_dev != parent_cache.dev || st.st_ino != parent_cache.ino) {
        cache_drop();
        return -1;
    }
    *remainder = relative[length] ? relative + length + 1 : NULL;
    return dup_cloexec(parent_cache.fd);
}

static int open_parent(const ResolvedPath *path, char *leaf,
                       size_t leaf_size) {
    char relative[S9_PATH_MAX];
    const char *component;
    const char *slash;
    char *cursor;
    int directory;
    int cached;

    if(strlen(path->relative_path) >= sizeof(relative)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    slash = strrchr(path->relative_path, '/');
    component = slash ? slash + 1 : path->relative_path;
    if(!component[0])
        component = ".";
    if(strlen(component) >= leaf_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(leaf, component);
    directory = root_descriptor(path);
    if(directory < 0 || !slash)
        return directory;

    /* `relative` is the parent directory's path; walk it from the root or
     * from the cached prefix. */
    memcpy(relative, path->relative_path, (size_t)(slash - path->relative_path));
    relative[slash - path->relative_path] = '\0';
    component = relative;
    cached = cache_take(path, directory, relative, &component);
    if(cached >= 0) {
        close(directory);
        directory = cached;
    }
    cursor = component ? relative + (component - relative) : NULL;
    while(cursor && *cursor) {
        char *next = strchr(cursor, '/');
        int child;

        if(next)
            *next = '\0';
        child = openat(directory, cursor,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if(child < 0) {
            int error = errno;
            close(directory);
            errno = error;
            return -1;
        }
        close(directory);
        directory = child;
        if(next)
            *next = '/';
        cursor = next ? next + 1 : NULL;
    }
    cache_store(path, relative, directory);
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

int platform_mknod(const ResolvedPath *path, mode_t mode, unsigned major,
                   unsigned minor) {
    char leaf[S9_PATH_MAX];
    int parent = open_parent(path, leaf, sizeof(leaf));
    int result;
    int error;

    if(parent < 0)
        return -1;
    result = mknodat(parent, leaf, mode, makedev(major, minor));
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
