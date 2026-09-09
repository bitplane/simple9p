#include "namespace.h"
#include "platform.h"

#include <dos/dosextens.h>
#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>

typedef struct VolumeName {
    struct VolumeName *next;
    char name[256];
} VolumeName;

static BPTR native_root;

void platform_namespace_cleanup(Namespace *ns) {
    (void)ns;
    if(native_root) {
        UnLock(native_root);
        native_root = BNULL;
    }
}

int platform_namespace_ready(Namespace *ns) {
    platform_namespace_cleanup(ns);
    if(!ns->synthetic) {
        native_root = Lock((STRPTR)ns->native_root, ACCESS_READ);
        if(!native_root) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    return 0;
}

static void free_names(VolumeName *names) {
    while(names) {
        VolumeName *next = names->next;
        free(names);
        names = next;
    }
}

static VolumeName *snapshot_names(int *success) {
    struct DosList *list;
    struct DosList *entry;
    VolumeName *head = NULL;
    VolumeName **tail = &head;

    *success = 0;
    list = LockDosList(LDF_VOLUMES | LDF_READ);
    if(!list)
        return NULL;

    entry = list;
    while((entry = NextDosEntry(entry, LDF_VOLUMES)) != NULL) {
        const char *name = AROS_BSTR_ADDR(entry->dol_Name);
        size_t length = AROS_BSTR_strlen(entry->dol_Name);
        VolumeName *volume;

        if(length >= sizeof(volume->name))
            continue;
        volume = malloc(sizeof(*volume));
        if(!volume) {
            UnLockDosList(LDF_VOLUMES | LDF_READ);
            free_names(head);
            return NULL;
        }
        memcpy(volume->name, name, length);
        volume->name[length] = '\0';
        volume->next = NULL;
        *tail = volume;
        tail = &volume->next;
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    *success = 1;
    return head;
}

int platform_namespace_init(Namespace *ns) {
    return namespace_use_synthetic(ns);
}

int platform_namespace_discover(Namespace *ns) {
    VolumeName *names;
    VolumeName *volume;
    int success;

    names = snapshot_names(&success);
    if(!success)
        return -1;

    for(volume = names; volume; volume = volume->next) {
        char path[258];
        struct stat st;

        if(snprintf(path, sizeof(path), "%s:", volume->name) >= (int)sizeof(path))
            continue;
        if(stat(path, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        if(namespace_add_root(ns, volume->name, path) < 0) {
            fprintf(stderr, "Cannot export Amiga volume %s: %s\n",
                    volume->name, strerror(errno));
            free_names(names);
            return -1;
        }
    }
    free_names(names);
    return 0;
}

static int parent_is_exported(const ResolvedPath *path) {
    const char *root_name = namespace.synthetic ? path->root_path
                                                : namespace.native_root;
    char parent[S9_PATH_MAX];
    char *slash;
    BPTR root;
    BPTR current;
    int allowed = 0;

    if(!path->relative_path[0])
        return 1;
    if(strlen(path->native_path) >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(parent, path->native_path);
    slash = strrchr(parent, '/');
    if(slash)
        *slash = '\0';
    else
        strcpy(parent, root_name);
    root = namespace.synthetic ? Lock((STRPTR)root_name, ACCESS_READ)
                               : DupLock(native_root);
    current = Lock((STRPTR)parent, ACCESS_READ);
    if(!root || !current)
        goto done;
    for(;;) {
        BPTR next;
        if(SameLock(root, current) == LOCK_SAME) {
            allowed = 1;
            break;
        }
        next = ParentDir(current);
        UnLock(current);
        current = next;
        if(!current)
            break;
    }
done:
    if(current)
        UnLock(current);
    if(root)
        UnLock(root);
    if(!allowed)
        errno = EPERM;
    return allowed;
}

int platform_lstat(const ResolvedPath *path, struct stat *st) {
    if(!parent_is_exported(path))
        return -1;
    return lstat(path->native_path, st);
}

int platform_lstat_child(DIR *directory, const ResolvedPath *path,
                         const char *name, struct stat *st) {
    (void)directory;
    (void)name;
    return platform_lstat(path, st);
}

int platform_open(const ResolvedPath *path, int flags, mode_t mode) {
    if(!parent_is_exported(path))
        return -1;
    return open(path->native_path, flags, mode);
}

DIR *platform_opendir(const ResolvedPath *path) {
    if(!parent_is_exported(path))
        return NULL;
    return opendir(path->native_path);
}

ssize_t platform_readlink(const ResolvedPath *path, char *buffer, size_t size) {
    if(!parent_is_exported(path))
        return -1;
    return readlink(path->native_path, buffer, size);
}

int platform_access_execute(const ResolvedPath *path) {
    if(!parent_is_exported(path))
        return -1;
    return access(path->native_path, X_OK);
}

int platform_mkdir(const ResolvedPath *path, mode_t mode) {
    if(!parent_is_exported(path))
        return -1;
    return mkdir(path->native_path, mode);
}

int platform_symlink(const char *target, const ResolvedPath *path) {
    if(!parent_is_exported(path))
        return -1;
    return symlink(target, path->native_path);
}

int platform_remove(const ResolvedPath *path, int directory) {
    if(!parent_is_exported(path))
        return -1;
    return directory ? rmdir(path->native_path) : unlink(path->native_path);
}

int platform_rename(const ResolvedPath *old_path,
                    const ResolvedPath *new_path) {
    if(!parent_is_exported(old_path) || !parent_is_exported(new_path))
        return -1;
    return rename(old_path->native_path, new_path->native_path);
}

int platform_chmod(const ResolvedPath *path, mode_t mode) {
    if(!parent_is_exported(path))
        return -1;
    return chmod(path->native_path, mode);
}

int platform_chown(const ResolvedPath *path, uid_t uid, gid_t gid) {
    if(!parent_is_exported(path))
        return -1;
    return chown(path->native_path, uid, gid);
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
    if(!parent_is_exported(path))
        return -1;
    times.actime = atime;
    times.modtime = mtime;
    return utime(path->native_path, &times);
}

int platform_truncate(const ResolvedPath *path, off_t length) {
    if(!parent_is_exported(path))
        return -1;
    return truncate(path->native_path, length);
}
