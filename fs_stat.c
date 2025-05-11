#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>

void build_stat(IxpStat *s, const char *path, const char *fullpath, struct stat *st) {
    const char *name_ptr;
    
    s->type = 0;
    s->dev = 0;
    
    /* Set QID type properly */
    s->qid.type = P9_QTFILE;
    if(S_ISDIR(st->st_mode))
        s->qid.type = P9_QTDIR;
    else if(S_ISLNK(st->st_mode))
        s->qid.type = P9_QTSYMLINK;
    
    s->qid.path = st->st_ino;
    s->qid.version = st->st_mtime;
    
    s->mode = st->st_mode & 0777;
    if(S_ISDIR(st->st_mode))
        s->mode |= P9_DMDIR;
    else if(S_ISLNK(st->st_mode))
        s->mode |= P9_DMSYMLINK;
        
    s->atime = st->st_atime;
    s->mtime = st->st_mtime;
    s->length = st->st_size;
    
    /* For symlinks, set length to the length of the target path */
    if(S_ISLNK(st->st_mode)) {
        char target[PATH_MAX];
        int n = readlink(fullpath, target, sizeof(target)-1);
        if(n > 0) {
            target[n] = '\0';
            s->length = n;
        }
    }
    
    /* Handle name assignment properly for const */
    name_ptr = strrchr(path, '/');
    if(name_ptr && name_ptr[1])
        name_ptr++;
    else
        name_ptr = path;
    if(strcmp(path, "/") == 0)
        name_ptr = "/";
    
    /* Cast away const - libixp expects a non-const string */
    s->name = (char *)name_ptr;
    
    s->uid = getenv("USER");
    if(!s->uid) s->uid = "none";
    s->gid = s->uid;
    s->muid = s->uid;
}

void fs_stat(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;
    IxpStat s;
    IxpMsg m;
    uint16_t size;
    
    if(!state || !state->path) {
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
    
    build_stat(&s, state->path, fullpath, &st);
    
    size = ixp_sizeof_stat(&s);
    r->ofcall.rstat.nstat = size;
    r->ofcall.rstat.stat = malloc(size);
    if(!r->ofcall.rstat.stat) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    m = ixp_message(r->ofcall.rstat.stat, size, MsgPack);
    ixp_pstat(&m, &s);
    
    ixp_respond(r, nil);
    /* stat buffer is now owned by libixp */
}

void fs_wstat(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    IxpStat *s = &r->ifcall.twstat.stat;
    struct stat st;
    
    if(!state || !state->path) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    if(!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    /* Only handle mode changes for now */
    if(s->mode != (uint32_t)~0) {
        mode_t mode = s->mode & 0777;
        if(chmod(fullpath, mode) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    }
    
    /* Handle name changes (rename) */
    if(s->name != NULL && strlen(s->name) > 0) {
        char newpath[PATH_MAX];
        char newfullpath[PATH_MAX];
        char *dir, *pathcopy;
        
        pathcopy = strdup(state->path);
        if(!pathcopy) {
            ixp_respond(r, "out of memory");
            return;
        }
        
        dir = dirname(pathcopy);
        snprintf(newpath, sizeof(newpath), "%s/%s", dir, s->name);
        free(pathcopy);
        
        if(!getfullpath(newpath, newfullpath, sizeof(newfullpath))) {
            ixp_respond(r, "invalid path");
            return;
        }
        
        if(rename(fullpath, newfullpath) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        
        /* Update the fid's path */
        free(state->path);
        state->path = strdup(newpath);
        if(!state->path) {
            ixp_respond(r, "out of memory");
            return;
        }
    }
    
    ixp_respond(r, nil);
}