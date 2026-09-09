#include "server.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

NineDServer nined;

uint32_t qid_version(const struct stat *st) {
    return (uint32_t)st->st_mtime + nined.qid_generation;
}

void qid_bump(void) {
    nined.qid_generation++;
}

void nined_state_cleanup(void) {
    nined.fid_count = 0;
    nined.qid_generation = 0;
}

void respond_errno(Ixp9Req *r, int error) {
    r->ofcall.error.uerrno = (uint32_t)error;
    ixp_respond(r, strerror(error));
}

char *read_symlink_target(const ResolvedPath *path, size_t hint,
                          size_t *length) {
    size_t size = hint < SIZE_MAX ? hint + 1 : 0;

    if(size < 2)
        size = 128;
    for(;;) {
        char *target = s9_malloc(size);
        ssize_t count;

        if(!target)
            return NULL;
        count = platform_readlink(path, target, size - 1);
        if(count < 0) {
            int error = errno;
            s9_free(target);
            errno = error;
            return NULL;
        }
        if((size_t)count < size - 1) {
            target[count] = '\0';
            if(length)
                *length = (size_t)count;
            return target;
        }
        s9_free(target);
        if(size > SIZE_MAX / 2) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        size *= 2;
    }
}

static void set_qid(IxpQid *qid, const ResolvedPath *resolved,
                    const struct stat *st) {
    if(resolved->synthetic) {
        qid->type = P9_QTDIR;
        qid->path = namespace_root_qid();
        qid->version = namespace.generation;
        return;
    }
    qid->type = P9_QTFILE;
    if(S_ISDIR(st->st_mode))
        qid->type = P9_QTDIR;
    else if(S_ISLNK(st->st_mode))
        qid->type = P9_QTSYMLINK;
    qid->path = namespace_qid(resolved, st);
    qid->version = qid_version(st);
}

void fid_state_register(FidState *state) {
    state->previous = NULL;
    state->next = nined.fids;
    if(state->next)
        state->next->previous = state;
    nined.fids = state;
    nined.fid_count++;
}

void fid_state_unregister(FidState *state) {
    int registered = state->previous || state->next || nined.fids == state;

    if(state->previous)
        state->previous->next = state->next;
    else if(nined.fids == state)
        nined.fids = state->next;
    if(state->next)
        state->next->previous = state->previous;
    state->previous = NULL;
    state->next = NULL;
    if(registered)
        nined.fid_count--;
}

void fid_state_close(FidState *state) {
    if(!state)
        return;
    if(state->fd >= 0) {
        close(state->fd);
        state->fd = -1;
    }
    if(state->dir) {
        closedir(state->dir);
        state->dir = NULL;
    }
    namespace_free_roots(state->namespace_roots,
                         state->namespace_root_count);
    state->namespace_roots = NULL;
    state->namespace_root_count = 0;
    s9_free(state->symlink);
    state->symlink = NULL;
    state->symlink_length = 0;
    state->stat_valid = 0;
    state->remove_on_close = 0;
    state->open_mode = 0;
    state->open_flags = 0;
    state->checkpoint_count = 0;
    state->checkpoint_next = 0;
    state->dir_offset = 0;
}

FidState *fid_state_create(const char *path) {
    FidState *state;

    if(nined.fid_count >= S9_MAX_FIDS) {
        errno = EMFILE;
        return NULL;
    }
    state = s9_malloc(sizeof(*state));
    if(!state)
        return NULL;
    memset(state, 0, sizeof(*state));
    state->fd = -1;
    state->path = s9_strdup(path);
    if(!state->path) {
        s9_free(state);
        return NULL;
    }
    fid_state_register(state);
    return state;
}

void fid_state_destroy(FidState *state) {
    if(!state)
        return;
    fid_state_unregister(state);
    fid_state_close(state);
    s9_free(state->path);
    s9_free(state);
}

void fs_attach(Ixp9Req *r) {
    FidState *state;
    ResolvedPath resolved;
    struct stat st;

    state = fid_state_create("/");
    if(!state) {
        respond_errno(r, errno);
        return;
    }
    if(namespace_resolve("/", &resolved) < 0 ||
       (!resolved.synthetic && platform_lstat(&resolved, &st) < 0)) {
        int error = errno;
        fid_state_destroy(state);
        respond_errno(r, error);
        return;
    }
    if(resolved.synthetic)
        memset(&st, 0, sizeof(st));
    set_qid(&r->fid->qid, &resolved, &st);
    r->fid->aux = state;
    r->ofcall.rattach.qid = r->fid->qid;
    ixp_respond(r, nil);
}

/* The parent of a virtual path. The root is its own parent. */
static char *parent_path_alloc(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t length = slash && slash != path ? (size_t)(slash - path) : 1;
    char *parent = s9_malloc(length + 1);

    if(!parent) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

void fs_walk(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char *candidate;
    uint16_t i;
    IxpQid final_qid;

    if(!state || !state->path) {
        respond_errno(r, EBADF);
        return;
    }
    candidate = s9_strdup(state->path);
    if(!candidate) {
        respond_errno(r, ENOMEM);
        return;
    }
    final_qid = r->fid->qid;

    for(i = 0; i < r->ifcall.twalk.nwname; i++) {
        char *next;
        ResolvedPath resolved;
        struct stat st;

        if(strcmp(r->ifcall.twalk.wname[i], "..") == 0)
            next = parent_path_alloc(candidate);
        else
            next = namespace_join_virtual_alloc(candidate,
                                                r->ifcall.twalk.wname[i]);
        if(!next) {
            if(i == 0) {
                int error = errno;
                s9_free(candidate);
                respond_errno(r, error);
                return;
            }
            break;
        }
        s9_free(candidate);
        candidate = next;
        if(namespace_resolve(candidate, &resolved) < 0 ||
           (!resolved.synthetic && platform_lstat(&resolved, &st) < 0)) {
            if(i == 0) {
                int error = errno;
                s9_free(candidate);
                respond_errno(r, error);
                return;
            }
            break;
        }
        if(resolved.synthetic)
            memset(&st, 0, sizeof(st));
        set_qid(&r->ofcall.rwalk.wqid[i], &resolved, &st);
        final_qid = r->ofcall.rwalk.wqid[i];
        if(i + 1 < r->ifcall.twalk.nwname &&
           !(final_qid.type & P9_QTDIR)) {
            i++;
            break;
        }
    }
    r->ofcall.rwalk.nwqid = i;

    if(i == r->ifcall.twalk.nwname) {
        if(r->newfid == r->fid) {
            char *old_path = state->path;
            state->path = candidate;
            s9_free(old_path);
        } else {
            FidState *newstate = fid_state_create(candidate);
            if(!newstate) {
                int error = errno;
                s9_free(candidate);
                respond_errno(r, error);
                return;
            }
            r->newfid->aux = newstate;
            s9_free(candidate);
        }
        r->newfid->qid = final_qid;
    } else {
        s9_free(candidate);
    }
    ixp_respond(r, nil);
}

void fs_clunk(Ixp9Req *r) {
    FidState *state = r->fid->aux;

    if(state && state->remove_on_close) {
        ResolvedPath resolved;
        struct stat st;

        if(namespace_resolve(state->path, &resolved) < 0 ||
           platform_lstat(&resolved, &st) < 0) {
            respond_errno(r, errno);
            return;
        }
        fid_state_close(state);
        if(platform_remove(&resolved, S_ISDIR(st.st_mode)) < 0) {
            respond_errno(r, errno);
            return;
        }
        qid_bump();
    }
    ixp_respond(r, nil);
}

void fs_flush(Ixp9Req *r) {
    ixp_respond(r, nil);
}

void fs_freefid(IxpFid *f) {
    if(f && f->aux) {
        fid_state_destroy(f->aux);
        f->aux = NULL;
    }
}
