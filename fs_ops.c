#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* Real file operations */
void fs_attach(Ixp9Req *r) {
    FidState *state = malloc(sizeof(FidState));
    if(!state) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    state->path = strdup("/");
    state->open_mode = 0;
    state->open_flags = 0;
    
    if(!state->path) {
        free(state);
        ixp_respond(r, "out of memory");
        return;
    }
    
    r->fid->qid.type = P9_QTDIR;
    r->fid->qid.path = 0;
    r->fid->aux = state;
    r->ofcall.rattach.qid = r->fid->qid;
    ixp_respond(r, nil);
}

void fs_walk(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    FidState *newstate;
    char newpath[PATH_MAX];
    char fullpath[PATH_MAX];
    struct stat st;
    int i;
    
    if(!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }

    /* Clone fid for walk to newfid */
    newstate = malloc(sizeof(FidState));
    if(!newstate) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    newstate->path = strdup(state->path);
    newstate->open_mode = 0;
    newstate->open_flags = 0;
    
    if(!newstate->path) {
        free(newstate);
        ixp_respond(r, "out of memory");
        return;
    }
    
    r->newfid->aux = newstate;

    if(r->ifcall.twalk.nwname == 0) {
        r->newfid->qid = r->fid->qid;
        ixp_respond(r, nil);
        return;
    }

    strncpy(newpath, newstate->path, PATH_MAX-1);
    newpath[PATH_MAX-1] = '\0';
    
    for(i = 0; i < r->ifcall.twalk.nwname; i++) {
        if(strcmp(newpath, "/") != 0) {
            if(safe_strcat(newpath, "/", PATH_MAX) < 0) {
                ixp_respond(r, "path too long");
                return;
            }
        }
        
        if(safe_strcat(newpath, r->ifcall.twalk.wname[i], PATH_MAX) < 0) {
            ixp_respond(r, "path too long");
            return;
        }
        
        if(!getfullpath(newpath, fullpath, sizeof(fullpath))) {
            ixp_respond(r, "invalid path");
            return;
        }
        
        if(lstat(fullpath, &st) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        
        r->ofcall.rwalk.wqid[i].type = P9_QTFILE;
        if(S_ISDIR(st.st_mode))
            r->ofcall.rwalk.wqid[i].type = P9_QTDIR;
        else if(S_ISLNK(st.st_mode))
            r->ofcall.rwalk.wqid[i].type = P9_QTSYMLINK;
        
        r->ofcall.rwalk.wqid[i].path = st.st_ino;
        r->ofcall.rwalk.wqid[i].version = st.st_mtime;
    }

    r->ofcall.rwalk.nwqid = i;
    r->newfid->qid = r->ofcall.rwalk.wqid[i-1];
    
    /* Update the newstate path */
    free(newstate->path);
    newstate->path = strdup(newpath);
    if(!newstate->path) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    ixp_respond(r, nil);
}

void fs_open(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;
    int flags = 0;
    
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
    
    /* Convert 9P open mode to Unix flags */
    switch(r->ifcall.topen.mode & 3) {
        case P9_OREAD:
            flags = O_RDONLY;
            break;
        case P9_OWRITE:
            flags = O_WRONLY;
            break;
        case P9_ORDWR:
            flags = O_RDWR;
            break;
    }
    
    if(r->ifcall.topen.mode & P9_OTRUNC)
        flags |= O_TRUNC;
    if(r->ifcall.topen.mode & P9_OAPPEND)
        flags |= O_APPEND;
    
    /* Store the mode and flags for later use */
    state->open_mode = r->ifcall.topen.mode;
    state->open_flags = flags;
    
    /* Test if we can actually open the file with these flags */
    if(!S_ISDIR(st.st_mode)) {
        int fd = open(fullpath, flags);
        if(fd < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        close(fd);
    }
    
    r->fid->qid.type = P9_QTFILE;
    if(S_ISDIR(st.st_mode))
        r->fid->qid.type = P9_QTDIR;
    else if(S_ISLNK(st.st_mode))
        r->fid->qid.type = P9_QTSYMLINK;
    
    r->fid->qid.path = st.st_ino;
    r->fid->qid.version = st.st_mtime;
    r->ofcall.ropen.qid = r->fid->qid;
    ixp_respond(r, nil);
}

void fs_clunk(Ixp9Req *r) {
    ixp_respond(r, nil);
}

void fs_flush(Ixp9Req *r) {
    ixp_respond(r, nil);
}

void fs_freefid(IxpFid *f) {
    if(f && f->aux) {
        FidState *state = f->aux;
        if(state->path)
            free(state->path);
        free(state);
        f->aux = NULL;
    }
}