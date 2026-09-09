#include "server.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ssize_t s9_pread(int fd, void *buffer, size_t size, off_t offset) {
#ifdef S9_NO_PREAD
    if(lseek(fd, offset, SEEK_SET) < 0)
        return -1;
    return read(fd, buffer, size);
#else
    return pread(fd, buffer, size, offset);
#endif
}

ssize_t s9_pwrite(int fd, const void *buffer, size_t size, off_t offset) {
#ifdef S9_NO_PREAD
    if(lseek(fd, offset, SEEK_SET) < 0)
        return -1;
    return write(fd, buffer, size);
#else
    return pwrite(fd, buffer, size, offset);
#endif
}

static int checked_offset(uint64_t value, off_t *offset) {
    off_t converted = (off_t)value;

    if(converted < 0 || (uint64_t)converted != value) {
        errno = EOVERFLOW;
        return -1;
    }
    *offset = converted;
    return 0;
}

static int open_flags(uint8_t mode, int *flags) {
    uint8_t access = mode & 3;

    if(mode & ~(3 | P9_OTRUNC | P9_ORCLOSE)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if((mode & P9_OTRUNC) &&
       (access == P9_OREAD || access == P9_OEXEC)) {
        errno = EACCES;
        return -1;
    }

    switch(access) {
    case P9_OREAD:
        *flags = O_RDONLY;
        break;
    case P9_OWRITE:
        *flags = O_WRONLY;
        break;
    case P9_ORDWR:
        *flags = O_RDWR;
        break;
    case P9_OEXEC:
        *flags = O_RDONLY;
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if(mode & P9_OTRUNC)
        *flags |= O_TRUNC;
    return 0;
}

uint32_t fs_read_count(const Ixp9Req *r) {
    uint32_t count = r->ifcall.tread.count;
    uint32_t limit = IXP_MAX_MSG - 24U;

    if(r->fid->iounit && limit > r->fid->iounit)
        limit = r->fid->iounit;
    if(count > limit)
        count = limit;
    return count;
}

void fs_read(Ixp9Req *r) {
    FidState *state = r->fid->aux;

    if(!state || !state->path) {
        respond_errno(r, EBADF);
        return;
    }
    /* libixp lets a read through on OWRITE|OTRUNC; don't rely on it. */
    if((state->open_flags & O_ACCMODE) == O_WRONLY) {
        respond_errno(r, EBADF);
        return;
    }
    if(state->dir)
        read_directory(r, state);
    else if(state->symlink)
        read_symlink(r, state);
    else if(state->fd >= 0)
        read_file(r, state);
    else if(namespace_is_protected(state->path))
        read_synthetic_directory(r, state);
    else
        respond_errno(r, EBADF);
}

void fs_write(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    off_t offset;
    ssize_t count;

    if(nined.read_only) {
        respond_errno(r, EROFS);
        return;
    }
    if(!state || state->fd < 0) {
        respond_errno(r, EBADF);
        return;
    }
    if(!(state->open_flags & (O_WRONLY | O_RDWR))) {
        respond_errno(r, EBADF);
        return;
    }
    if(r->ifcall.twrite.count == 0) {
        r->ofcall.rwrite.count = 0;
        ixp_respond(r, nil);
        return;
    }
    if(checked_offset(r->ifcall.twrite.offset, &offset) < 0) {
        respond_errno(r, errno);
        return;
    }
    count = s9_pwrite(state->fd, r->ifcall.twrite.data,
                      r->ifcall.twrite.count, offset);
    if(count < 0) {
        respond_errno(r, errno);
        return;
    }
    r->ofcall.rwrite.count = (uint32_t)count;
    if(count > 0)
        qid_bump();
    ixp_respond(r, nil);
}

void fs_open(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    ResolvedPath resolved;
    struct stat st;
    int flags;

    if(!state || !state->path) {
        respond_errno(r, EBADF);
        return;
    }
    if(open_flags(r->ifcall.topen.mode, &flags) < 0) {
        respond_errno(r, errno);
        return;
    }
    if(nined.read_only &&
       (((r->ifcall.topen.mode & 3) == P9_OWRITE) ||
        ((r->ifcall.topen.mode & 3) == P9_ORDWR) ||
        (r->ifcall.topen.mode & (P9_OTRUNC | P9_ORCLOSE)))) {
        respond_errno(r, EROFS);
        return;
    }
    if(namespace_resolve(state->path, &resolved) < 0 ||
       (!resolved.synthetic && platform_lstat(&resolved, &st) < 0)) {
        respond_errno(r, errno);
        return;
    }
    if(resolved.synthetic) {
        if((r->ifcall.topen.mode & 3) != P9_OREAD ||
           (r->ifcall.topen.mode & P9_OTRUNC)) {
            respond_errno(r, EPERM);
            return;
        }
        memset(&st, 0, sizeof(st));
        st.st_mode = S_IFDIR | 0555;
    } else if(namespace_is_protected(state->path) &&
              ((r->ifcall.topen.mode & 3) != P9_OREAD ||
               (r->ifcall.topen.mode & P9_OTRUNC))) {
        respond_errno(r, EPERM);
        return;
    } else if((r->ifcall.topen.mode & 3) == P9_OEXEC &&
              platform_access_execute(&resolved) < 0) {
        respond_errno(r, errno);
        return;
    }

    fid_state_close(state);
    if(!resolved.synthetic && S_ISDIR(st.st_mode)) {
        state->dir = platform_opendir(&resolved);
        if(!state->dir) {
            respond_errno(r, errno);
            return;
        }
    } else if(!resolved.synthetic && S_ISLNK(st.st_mode)) {
        if((r->ifcall.topen.mode & 3) != P9_OREAD) {
            respond_errno(r, EACCES);
            return;
        }
        state->symlink = read_symlink_target(&resolved,
                                             (size_t)st.st_size,
                                             &state->symlink_length);
        if(!state->symlink) {
            respond_errno(r, errno);
            return;
        }
    } else if(!resolved.synthetic && S_ISREG(st.st_mode)) {
        state->fd = platform_open(&resolved, flags, 0);
        if(state->fd < 0) {
            respond_errno(r, errno);
            return;
        }
        if(flags & O_TRUNC) {
            qid_bump();
            if(fstat(state->fd, &st) < 0) {
                int error = errno;
                fid_state_close(state);
                respond_errno(r, error);
                return;
            }
        }
    } else if(!resolved.synthetic) {
        respond_errno(r, EOPNOTSUPP);
        return;
    }

    state->open_mode = r->ifcall.topen.mode;
    state->open_flags = flags;
    state->remove_on_close = !!(r->ifcall.topen.mode & P9_ORCLOSE);
    state->opened_stat = st;
    state->stat_valid = 1;
    r->fid->qid.type = S_ISDIR(st.st_mode) ? P9_QTDIR :
                       S_ISLNK(st.st_mode) ? P9_QTSYMLINK : P9_QTFILE;
    r->fid->qid.path = resolved.synthetic ? namespace_root_qid()
                                           : namespace_qid(&resolved, &st);
    r->fid->qid.version = resolved.synthetic ? namespace.generation
                                             : qid_version(&st);
    r->ofcall.ropen.qid = r->fid->qid;
    ixp_respond(r, nil);
}

/* Parse a 9P2000.u device extension: "b major minor" or "c major minor". */
static int parse_device(const char *extension, mode_t *type,
                        unsigned *major, unsigned *minor) {
    const char *p = extension;
    char *end;
    unsigned long value[2];
    int i;

    if(!p || (p[0] != 'b' && p[0] != 'c') || p[1] != ' ')
        return -1;
    *type = p[0] == 'b' ? S_IFBLK : S_IFCHR;
    p += 2;
    for(i = 0; i < 2; i++) {
        if(*p < '0' || *p > '9')
            return -1;
        errno = 0;
        value[i] = strtoul(p, &end, 10);
        if(errno || value[i] > UINT_MAX)
            return -1;
        p = end;
        if(i == 0) {
            if(*p != ' ')
                return -1;
            p++;
        }
    }
    if(*p)
        return -1;
    *major = (unsigned)value[0];
    *minor = (unsigned)value[1];
    return 0;
}

void fs_create(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char *new_path;
    ResolvedPath resolved;
    struct stat st;
    mode_t permissions;
    uint32_t types;
    int flags;
    int created = 0;
    int is_directory;
    int is_symlink;
    int is_special;
    mode_t special_type = 0;
    unsigned major = 0;
    unsigned minor = 0;
    char *link_copy = NULL;

    if(!state || !state->path) {
        respond_errno(r, EBADF);
        return;
    }
    if(nined.read_only) {
        respond_errno(r, EROFS);
        return;
    }
    if(namespace_is_namespace_root(state->path)) {
        respond_errno(r, EPERM);
        return;
    }
    new_path = namespace_join_virtual_alloc(state->path,
                                            r->ifcall.tcreate.name);
    if(!new_path) {
        respond_errno(r, errno);
        return;
    }
    if(namespace_resolve(new_path, &resolved) < 0 ||
       open_flags(r->ifcall.tcreate.mode, &flags) < 0) {
        int error = errno;
        s9_free(new_path);
        respond_errno(r, error);
        return;
    }
    types = r->ifcall.tcreate.perm & ~(0777U | S9_DMSPECIAL);
    is_directory = !!(types & P9_DMDIR);
    is_symlink = !!(types & P9_DMSYMLINK);
    is_special = types == P9_DMDEVICE || types == P9_DMNAMEDPIPE ||
                 types == P9_DMSOCKET;
    if(types != 0 && types != P9_DMDIR && types != P9_DMSYMLINK &&
       !is_special) {
        s9_free(new_path);
        respond_errno(r, EOPNOTSUPP);
        return;
    }
    if(is_special) {
        if((r->ifcall.tcreate.mode & 3) != P9_OREAD ||
           (r->ifcall.tcreate.mode & P9_OTRUNC)) {
            s9_free(new_path);
            respond_errno(r, EACCES);
            return;
        }
        if(types == P9_DMNAMEDPIPE)
            special_type = S_IFIFO;
        else if(types == P9_DMSOCKET)
            special_type = S_IFSOCK;
        else if(parse_device(r->ifcall.tcreate.extension, &special_type,
                             &major, &minor) < 0) {
            s9_free(new_path);
            respond_errno(r, EINVAL);
            return;
        }
    }
    if(is_symlink) {
        if((r->ifcall.tcreate.mode & 3) != P9_OREAD ||
           (r->ifcall.tcreate.mode & P9_OTRUNC)) {
            s9_free(new_path);
            respond_errno(r, EACCES);
            return;
        }
        if(!r->ifcall.tcreate.extension ||
           !r->ifcall.tcreate.extension[0]) {
            s9_free(new_path);
            respond_errno(r, EINVAL);
            return;
        }
        link_copy = s9_strdup(r->ifcall.tcreate.extension);
        if(!link_copy) {
            s9_free(new_path);
            respond_errno(r, ENOMEM);
            return;
        }
    }
    if(is_directory && ((r->ifcall.tcreate.mode & 3) != P9_OREAD ||
                        (r->ifcall.tcreate.mode & P9_OTRUNC))) {
        s9_free(link_copy);
        s9_free(new_path);
        respond_errno(r, EACCES);
        return;
    }
    permissions = r->ifcall.tcreate.perm & 0777;
    if(r->ifcall.tcreate.perm & P9_DMSETUID)
        permissions |= S_ISUID;
    if(r->ifcall.tcreate.perm & P9_DMSETGID)
        permissions |= S_ISGID;
    if(r->ifcall.tcreate.perm & S9_DMSETVTX)
        permissions |= S_ISVTX;
    if(is_special) {
        /* Linux clunks the fid straight after a mknod, so the node is not
         * opened. Reads on it answer EBADF, as for any unopened fid. */
        if(platform_mknod(&resolved, special_type | permissions,
                          major, minor) < 0)
            goto fail;
        created = 1;
    } else if(is_directory) {
        if(platform_mkdir(&resolved, permissions) < 0)
            goto fail;
        created = 1;
        state->dir = platform_opendir(&resolved);
        if(!state->dir)
            goto fail;
    } else if(is_symlink) {
        if(platform_symlink(link_copy, &resolved) < 0)
            goto fail;
        created = 1;
        state->symlink = link_copy;
        state->symlink_length = strlen(link_copy);
        link_copy = NULL;
    } else {
        state->fd = platform_open(&resolved, flags | O_CREAT | O_EXCL,
                                  permissions);
        if(state->fd < 0)
            goto fail;
        created = 1;
    }
    if(platform_lstat(&resolved, &st) < 0)
        goto fail;

    s9_free(state->path);
    state->path = new_path;
    state->open_mode = r->ifcall.tcreate.mode;
    state->open_flags = flags;
    state->remove_on_close = !!(r->ifcall.tcreate.mode & P9_ORCLOSE);
    state->opened_stat = st;
    state->stat_valid = 1;
    r->fid->qid.type = is_directory ? P9_QTDIR :
                       is_symlink ? P9_QTSYMLINK : P9_QTFILE;
    r->fid->qid.path = namespace_qid(&resolved, &st);
    qid_bump();
    r->fid->qid.version = qid_version(&st);
    r->ofcall.rcreate.qid = r->fid->qid;
    r->ofcall.rcreate.iounit = 0;
    s9_free(link_copy);
    ixp_respond(r, nil);
    return;

fail:
    {
        int error = errno;
        fid_state_close(state);
        if(created) {
            if(is_directory)
                platform_remove(&resolved, 1);
            else
                platform_remove(&resolved, 0);
        }
        s9_free(link_copy);
        s9_free(new_path);
        respond_errno(r, error);
    }
}

void fs_remove(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    ResolvedPath resolved;
    struct stat st;

    if(!state || !state->path) {
        respond_errno(r, EBADF);
        return;
    }
    if(nined.read_only) {
        respond_errno(r, EROFS);
        return;
    }
    if(namespace_is_protected(state->path)) {
        respond_errno(r, EPERM);
        return;
    }
    if(namespace_resolve(state->path, &resolved) < 0 ||
       platform_lstat(&resolved, &st) < 0) {
        respond_errno(r, errno);
        return;
    }
    /* Some platforms refuse to remove an open file. The fid goes away
     * whether or not the remove succeeds, so nothing is lost by closing. */
    fid_state_close(state);
    if(platform_remove(&resolved, S_ISDIR(st.st_mode)) < 0) {
        respond_errno(r, errno);
        return;
    }
    qid_bump();
    ixp_respond(r, nil);
}
