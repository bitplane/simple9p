#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h>

void fs_read(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;
    
    if(!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    if(!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    if(lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    if(S_ISDIR(st.st_mode)) {
        read_directory(r, fullpath);
    } else if(S_ISLNK(st.st_mode)) {
        read_symlink(r, fullpath);
    } else {
        read_file(r, fullpath);
    }
}

void fs_write(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    int fd;
    
    if(!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    if(!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    /* Check if file was opened for writing */
    if(!(state->open_flags & (O_WRONLY | O_RDWR))) {
        ixp_respond(r, "file not opened for writing");
        return;
    }
    
    /* Open the file with the stored flags */
    fd = open(fullpath, state->open_flags);
    if(fd < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* Handle offset for non-append mode */
    if(!(state->open_flags & O_APPEND)) {
        if(lseek(fd, r->ifcall.twrite.offset, SEEK_SET) < 0) {
            close(fd);
            ixp_respond(r, strerror(errno));
            return;
        }
    }
    
    ssize_t n = write(fd, r->ifcall.twrite.data, r->ifcall.twrite.count);
    close(fd);
    
    if(n < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    r->ofcall.rwrite.count = n;
    ixp_respond(r, nil);
}

void fs_create(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char newpath[PATH_MAX];
    char fullpath[PATH_MAX];
    struct stat st;
    int fd;
    mode_t mode;
    FidState *newstate;
    
    if(!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    /* Build the new path */
    strncpy(newpath, state->path, PATH_MAX-1);
    newpath[PATH_MAX-1] = '\0';
    
    if(strcmp(newpath, "/") != 0) {
        if(safe_strcat(newpath, "/", PATH_MAX) < 0) {
            ixp_respond(r, "path too long");
            return;
        }
    }
    
    if(safe_strcat(newpath, r->ifcall.tcreate.name, PATH_MAX) < 0) {
        ixp_respond(r, "path too long");
        return;
    }
    
    if(!getfullpath(newpath, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    /* Convert 9P permissions to Unix permissions */
    mode = r->ifcall.tcreate.perm & 0777;
    
    if(r->ifcall.tcreate.perm & P9_DMDIR) {
        /* Create directory */
        if(mkdir(fullpath, mode) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    } else {
        /* Create file with requested permissions */
        int create_flags = O_CREAT | O_EXCL;
        
        /* Add flags based on requested mode */
        switch(r->ifcall.tcreate.mode & 3) {
            case P9_OREAD:
                create_flags |= O_RDONLY;
                break;
            case P9_OWRITE:
                create_flags |= O_WRONLY;
                break;
            case P9_ORDWR:
                create_flags |= O_RDWR;
                break;
        }
        
        if(r->ifcall.tcreate.mode & P9_OTRUNC)
            create_flags |= O_TRUNC;
        if(r->ifcall.tcreate.mode & P9_OAPPEND)
            create_flags |= O_APPEND;
        
        fd = open(fullpath, create_flags, mode);
        if(fd < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        close(fd);
    }
    
    /* Stat the new file/directory */
    if(lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* Create new state for the fid */
    newstate = malloc(sizeof(FidState));
    if(!newstate) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    newstate->path = strdup(newpath);
    newstate->open_mode = r->ifcall.tcreate.mode;
    
    /* Set open flags based on create mode */
    newstate->open_flags = 0;
    switch(r->ifcall.tcreate.mode & 3) {
        case P9_OREAD:
            newstate->open_flags = O_RDONLY;
            break;
        case P9_OWRITE:
            newstate->open_flags = O_WRONLY;
            break;
        case P9_ORDWR:
            newstate->open_flags = O_RDWR;
            break;
    }
    
    if(r->ifcall.tcreate.mode & P9_OTRUNC)
        newstate->open_flags |= O_TRUNC;
    if(r->ifcall.tcreate.mode & P9_OAPPEND)
        newstate->open_flags |= O_APPEND;
    
    if(!newstate->path) {
        free(newstate);
        ixp_respond(r, "out of memory");
        return;
    }
    
    /* Replace old state with new state */
    if(state->path)
        free(state->path);
    free(state);
    r->fid->aux = newstate;
    
    /* Update QID */
    r->fid->qid.type = S_ISDIR(st.st_mode) ? P9_QTDIR : P9_QTFILE;
    r->fid->qid.path = st.st_ino;
    r->fid->qid.version = st.st_mtime;
    
    r->ofcall.rcreate.qid = r->fid->qid;
    r->ofcall.rcreate.iounit = 0;
    ixp_respond(r, nil);
}

void fs_remove(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;
    
    if(!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    if(!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    if(lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    if(S_ISDIR(st.st_mode)) {
        if(rmdir(fullpath) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    } else {
        if(unlink(fullpath) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    }
    
    ixp_respond(r, nil);
}