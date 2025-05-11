#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

void read_directory(Ixp9Req *r, const char *fullpath) {
    DIR *dir = opendir(fullpath);
    struct dirent *de;
    IxpMsg m;
    char *buf = NULL;
    uint64_t offset = r->ifcall.tread.offset;
    uint64_t pos = 0;
    
    if(!dir) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    buf = malloc(r->ifcall.tread.count);
    if(!buf) {
        closedir(dir);
        ixp_respond(r, "out of memory");
        return;
    }
    
    m = ixp_message(buf, r->ifcall.tread.count, MsgPack);
    
    /* Read directory entries, skipping until we reach the requested offset */
    while((de = readdir(dir))) {
        IxpStat s;
        struct stat st2;
        char childpath[PATH_MAX];
        uint16_t slen;
        
        if(snprintf(childpath, sizeof(childpath), "%s/%s", fullpath, de->d_name) >= sizeof(childpath))
            continue;
            
        if(lstat(childpath, &st2) < 0)
            continue;
        
        /* Build stat structure */
        s.type = 0;
        s.dev = 0;
        s.qid.type = P9_QTFILE;
        if(S_ISDIR(st2.st_mode))
            s.qid.type = P9_QTDIR;
        else if(S_ISLNK(st2.st_mode))
            s.qid.type = P9_QTSYMLINK;
            
        s.qid.path = st2.st_ino;
        s.qid.version = st2.st_mtime;
        s.mode = st2.st_mode & 0777;
        if(S_ISDIR(st2.st_mode))
            s.mode |= P9_DMDIR;
        else if(S_ISLNK(st2.st_mode))
            s.mode |= P9_DMSYMLINK;
            
        s.atime = st2.st_atime;
        s.mtime = st2.st_mtime;
        s.length = st2.st_size;
        s.name = de->d_name;
        s.uid = getenv("USER");
        if(!s.uid) s.uid = "none";
        s.gid = s.uid;
        s.muid = s.uid;
        
        /* Calculate size of this stat entry */
        slen = ixp_sizeof_stat(&s);
        
        /* Skip entries until we reach the offset */
        if(pos + slen <= offset) {
            pos += slen;
            continue;
        }
        
        /* If this entry won't fit in the buffer, stop */
        if(m.pos - buf + slen > r->ifcall.tread.count)
            break;
        
        /* Add this entry to the result */
        ixp_pstat(&m, &s);
        pos += slen;
    }
    
    closedir(dir);
    r->ofcall.rread.count = m.pos - buf;
    r->ofcall.rread.data = buf;
    ixp_respond(r, nil);
    /* buf is now owned by libixp */
}

void read_symlink(Ixp9Req *r, const char *fullpath) {
    char *buf = malloc(r->ifcall.tread.count);
    int n;
    
    if(!buf) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    n = readlink(fullpath, buf, r->ifcall.tread.count - 1);
    if(n < 0) {
        free(buf);
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* readlink doesn't null-terminate */
    buf[n] = '\0';
    
    /* Respect the offset */
    if(r->ifcall.tread.offset >= n) {
        r->ofcall.rread.count = 0;
        r->ofcall.rread.data = buf;
    } else {
        int len = n - r->ifcall.tread.offset;
        if(len > r->ifcall.tread.count)
            len = r->ifcall.tread.count;
        memmove(buf, buf + r->ifcall.tread.offset, len);
        r->ofcall.rread.count = len;
        r->ofcall.rread.data = buf;
    }
    
    ixp_respond(r, nil);
    /* buf is now owned by libixp */
}

void read_file(Ixp9Req *r, const char *fullpath) {
    int fd = open(fullpath, O_RDONLY);
    char *buf = NULL;
    
    if(fd < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    buf = malloc(r->ifcall.tread.count);
    if(!buf) {
        close(fd);
        ixp_respond(r, "out of memory");
        return;
    }
    
    lseek(fd, r->ifcall.tread.offset, SEEK_SET);
    int n = read(fd, buf, r->ifcall.tread.count);
    close(fd);
    
    if(n < 0) {
        free(buf);
        ixp_respond(r, strerror(errno));
        return;
    }
    
    r->ofcall.rread.count = n;
    r->ofcall.rread.data = buf;
    ixp_respond(r, nil);
    /* buf is now owned by libixp */
}