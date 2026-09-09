#include "server.h"
#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static DirCheckpoint *find_checkpoint(FidState *state, uint64_t offset) {
    size_t index;

    for(index = 0; index < state->checkpoint_count; index++)
        if(state->checkpoints[index].offset == offset)
            return &state->checkpoints[index];
    return NULL;
}

static void remember_checkpoint(FidState *state, uint64_t offset,
                                long cookie) {
    DirCheckpoint *checkpoint;

    checkpoint = find_checkpoint(state, offset);
    if(!checkpoint) {
        checkpoint = &state->checkpoints[state->checkpoint_next];
        state->checkpoint_next = (state->checkpoint_next + 1) %
                                 S9_DIR_CHECKPOINTS;
        if(state->checkpoint_count < S9_DIR_CHECKPOINTS)
            state->checkpoint_count++;
    }
    checkpoint->offset = offset;
    checkpoint->cookie = cookie;
}

static int next_directory_stat(FidState *state, IxpStat *stat,
                               long *before, long *after) {
    for(;;) {
        struct dirent *entry;
        char *virtual_path;
        ResolvedPath resolved;
        struct stat native;

        *before = telldir(state->dir);
        errno = 0;
        entry = readdir(state->dir);
        if(!entry)
            return errno ? -1 : 0;
        if(strcmp(entry->d_name, ".") == 0 ||
           strcmp(entry->d_name, "..") == 0)
            continue;
        virtual_path = namespace_join_virtual_alloc(state->path,
                                                    entry->d_name);
        if(!virtual_path)
            return -1;
        if(namespace_resolve(virtual_path, &resolved) < 0 ||
           platform_lstat_child(state->dir, &resolved, entry->d_name,
                                &native) < 0) {
            s9_free(virtual_path);
            continue;
        }
        memset(stat, 0, sizeof(*stat));
        if(build_stat(stat, virtual_path, &resolved, &native, NULL) < 0) {
            s9_free(virtual_path);
            return -1;
        }
        s9_free(virtual_path);
        *after = telldir(state->dir);
        return 1;
    }
}

static int seek_directory(FidState *state, uint64_t offset, uint version) {
    DirCheckpoint *checkpoint;

    if(offset == state->dir_offset)
        return 0;
    if(offset == 0) {
        rewinddir(state->dir);
        state->dir_offset = 0;
        return 0;
    }
    checkpoint = find_checkpoint(state, offset);
    if(checkpoint) {
        seekdir(state->dir, checkpoint->cookie);
        state->dir_offset = offset;
        return 0;
    }

    rewinddir(state->dir);
    state->dir_offset = 0;
    while(state->dir_offset < offset) {
        IxpStat stat;
        uint16_t length;
        long before;
        long after;
        int result = next_directory_stat(state, &stat, &before, &after);

        (void)before;
        if(result <= 0) {
            int error = result == 0 ? EINVAL : errno;

            rewinddir(state->dir);
            state->dir_offset = 0;
            errno = error;
            return -1;
        }
        length = ixp_sizeof_stat(&stat, version);
        free_stat_strings(&stat);
        if(state->dir_offset > UINT64_MAX - length ||
           state->dir_offset + length > offset) {
            rewinddir(state->dir);
            state->dir_offset = 0;
            errno = EINVAL;
            return -1;
        }
        state->dir_offset += length;
        remember_checkpoint(state, state->dir_offset, after);
    }
    return 0;
}

void read_directory(Ixp9Req *r, FidState *state) {
    IxpMsg message;
    char *buffer;
    uint32_t count = fs_read_count(r);

    if(seek_directory(state, r->ifcall.tread.offset,
                      ixp_req_getversion(r)) < 0) {
        respond_errno(r, errno);
        return;
    }
    buffer = s9_malloc(count ? count : 1);
    if(!buffer) {
        respond_errno(r, ENOMEM);
        return;
    }
    message = ixp_message(buffer, count, MsgPack);
    message.version = ixp_req_getversion(r);

    for(;;) {
        IxpStat stat;
        uint16_t length;
        long before;
        long after;
        int result = next_directory_stat(state, &stat, &before, &after);

        if(result == 0)
            break;
        if(result < 0) {
            int error = errno;

            seekdir(state->dir, before);
            s9_free(buffer);
            respond_errno(r, error);
            return;
        }
        length = ixp_sizeof_stat(&stat, ixp_req_getversion(r));
        if((size_t)(message.end - message.pos) < length) {
            free_stat_strings(&stat);
            seekdir(state->dir, before);
            break;
        }
        remember_checkpoint(state, state->dir_offset + length, after);
        ixp_pstat(&message, &stat);
        state->dir_offset += length;
        free_stat_strings(&stat);
    }
    r->ofcall.rread.count = (uint32_t)(message.pos - buffer);
    r->ofcall.rread.data = buffer;
    ixp_respond(r, nil);
}

void read_synthetic_directory(Ixp9Req *r, FidState *state) {
    IxpMsg message;
    char *buffer;
    uint64_t position = 0;
    size_t index;
    int boundary = r->ifcall.tread.offset == 0;
    uint32_t count = fs_read_count(r);

    if(r->ifcall.tread.offset == 0) {
        NamespaceRoot *roots;
        size_t root_count;

        if(namespace_refresh() < 0 ||
           namespace_copy_roots(&roots, &root_count) < 0) {
            respond_errno(r, errno);
            return;
        }
        namespace_free_roots(state->namespace_roots,
                             state->namespace_root_count);
        state->namespace_roots = roots;
        state->namespace_root_count = root_count;
    } else if(!state->namespace_roots && state->namespace_root_count == 0) {
        respond_errno(r, EINVAL);
        return;
    }
    buffer = s9_malloc(count ? count : 1);
    if(!buffer) {
        respond_errno(r, ENOMEM);
        return;
    }
    message = ixp_message(buffer, count, MsgPack);
    message.version = ixp_req_getversion(r);

    for(index = 0; index < state->namespace_root_count; index++) {
        char *virtual_path;
        ResolvedPath resolved;
        struct stat st;
        IxpStat stat;
        uint16_t length;

        virtual_path = namespace_join_virtual_alloc("/",
                                                    state->namespace_roots[index].name);
        if(!virtual_path)
            continue;
        if(namespace_resolve_root(&state->namespace_roots[index],
                                  &resolved) < 0 ||
           platform_lstat(&resolved, &st) < 0) {
            s9_free(virtual_path);
            continue;
        }
        memset(&stat, 0, sizeof(stat));
        if(build_stat(&stat, virtual_path, &resolved, &st, NULL) < 0) {
            s9_free(virtual_path);
            s9_free(buffer);
            respond_errno(r, ENOMEM);
            return;
        }
        s9_free(virtual_path);
        length = ixp_sizeof_stat(&stat, ixp_req_getversion(r));
        if(position == r->ifcall.tread.offset)
            boundary = 1;
        if(position + length <= r->ifcall.tread.offset) {
            position += length;
            free_stat_strings(&stat);
            continue;
        }
        if(!boundary) {
            free_stat_strings(&stat);
            s9_free(buffer);
            respond_errno(r, EINVAL);
            return;
        }
        if((size_t)(message.end - message.pos) < length) {
            free_stat_strings(&stat);
            break;
        }
        ixp_pstat(&message, &stat);
        position += length;
        free_stat_strings(&stat);
    }
    if(r->ifcall.tread.offset > position) {
        s9_free(buffer);
        respond_errno(r, EINVAL);
        return;
    }
    r->ofcall.rread.count = (uint32_t)(message.pos - buffer);
    r->ofcall.rread.data = buffer;
    ixp_respond(r, nil);
}

void read_symlink(Ixp9Req *r, FidState *state) {
    uint64_t offset = r->ifcall.tread.offset;
    size_t count;
    uint32_t limit = fs_read_count(r);
    char *buffer;

    if(offset >= state->symlink_length)
        count = 0;
    else {
        count = state->symlink_length - (size_t)offset;
        if(count > limit)
            count = limit;
    }
    buffer = s9_malloc(count ? count : 1);
    if(!buffer) {
        respond_errno(r, ENOMEM);
        return;
    }
    if(count)
        memcpy(buffer, state->symlink + (size_t)offset, count);
    r->ofcall.rread.count = (uint32_t)count;
    r->ofcall.rread.data = buffer;
    ixp_respond(r, nil);
}

void read_file(Ixp9Req *r, FidState *state) {
    off_t offset = (off_t)r->ifcall.tread.offset;
    char *buffer;
    ssize_t count;
    uint32_t limit = fs_read_count(r);

    if(offset < 0 || (uint64_t)offset != r->ifcall.tread.offset) {
        respond_errno(r, EOVERFLOW);
        return;
    }
    buffer = s9_malloc(limit ? limit : 1);
    if(!buffer) {
        respond_errno(r, ENOMEM);
        return;
    }
    if(limit == 0)
        count = 0;
    else
        count = s9_pread(state->fd, buffer, limit, offset);
    if(count < 0) {
        int error = errno;
        s9_free(buffer);
        respond_errno(r, error);
        return;
    }
    r->ofcall.rread.count = (uint32_t)count;
    r->ofcall.rread.data = buffer;
    ixp_respond(r, nil);
}
