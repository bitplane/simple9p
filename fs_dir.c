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
    int include_parent = 1;  // Include ".." entries but not "."
    
    if (!dir) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    buf = malloc(r->ifcall.tread.count);
    if (!buf) {
        closedir(dir);
        ixp_respond(r, "out of memory");
        return;
    }
    
    m = ixp_message(buf, r->ifcall.tread.count, MsgPack);
    
    /* Read directory entries, skipping until we reach the requested offset */
    while ((de = readdir(dir))) {
        IxpStat s;
        struct stat st2;
        char childpath[PATH_MAX];
        uint16_t slen;
        
        /* Skip "." entry as it's added by the client, but include ".." */
        if (strcmp(de->d_name, ".") == 0) {
            continue;
        }
        
        /* Skip ".." entry only if include_parent is 0 */
        if (!include_parent && strcmp(de->d_name, "..") == 0) {
            continue;
        }
        
        /* Safely construct the full path for the child entry */
        int path_len = snprintf(childpath, sizeof(childpath), "%s/%s", fullpath, de->d_name);
        if (path_len >= sizeof(childpath) || path_len < 0) {
            /* Path would be truncated or other snprintf error, skip this entry */
            continue;
        }
            
        if (lstat(childpath, &st2) < 0) {
            /* Failed to stat the entry, skip it */
            continue;
        }
        
        /* Build stat structure */
        memset(&s, 0, sizeof(IxpStat));
        s.type = 0;
        s.dev = 0;
        s.qid.type = P9_QTFILE;
        if (S_ISDIR(st2.st_mode))
            s.qid.type = P9_QTDIR;
        else if (S_ISLNK(st2.st_mode))
            s.qid.type = P9_QTSYMLINK;
            
        s.qid.path = st2.st_ino;
        s.qid.version = st2.st_mtime;
        s.mode = st2.st_mode & 0777;
        if (S_ISDIR(st2.st_mode))
            s.mode |= P9_DMDIR;
        else if (S_ISLNK(st2.st_mode))
            s.mode |= P9_DMSYMLINK;
            
        s.atime = st2.st_atime;
        s.mtime = st2.st_mtime;
        s.length = st2.st_size;
        
        /* Safely handle name assignment */
        s.name = strdup(de->d_name);
        if (!s.name) {
            /* Memory allocation failed, skip this entry */
            continue;
        }
        
        /* Use consistent UID/GID handling */
        const char *user = getenv("USER");
        /* Instead of direct assignment, use string constants which libixp will handle properly */
        s.uid = user ? (char*)user : "none";  /* Cast to remove const warning - libixp will copy this */
        s.gid = s.uid;
        s.muid = s.uid;
        
        /* Calculate size of this stat entry */
        slen = ixp_sizeof_stat(&s);
        
        /* Skip entries until we reach the offset */
        if (pos + slen <= offset) {
            pos += slen;
            free((char *)s.name);
            continue;
        }
        
        /* If this entry won't fit in the buffer, stop */
        if (m.pos - buf + slen > r->ifcall.tread.count) {
            free((char *)s.name);
            break;
        }
        
        /* Add this entry to the result */
        ixp_pstat(&m, &s);
        
        /* Free our allocated name - ixp_pstat makes its own copy */
        free((char *)s.name);
        
        pos += slen;
    }
    
    closedir(dir);
    r->ofcall.rread.count = m.pos - buf;
    r->ofcall.rread.data = buf;
    ixp_respond(r, nil);
    /* buf is now owned by libixp */
}

void read_symlink(Ixp9Req *r, const char *fullpath) {
    /* Add extra byte for null terminator */
    size_t buf_size = r->ifcall.tread.count + 1;
    char *buf = malloc(buf_size);
    int n;
    
    if (!buf) {
        ixp_respond(r, "out of memory");
        return;
    }
    
    /* We read one character less than the buffer size to ensure space for null terminator */
    n = readlink(fullpath, buf, buf_size - 1);
    if (n < 0) {
        free(buf);
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* Null-terminate the link target */
    buf[n] = '\0';
    
    /* Respect the offset */
    if (r->ifcall.tread.offset >= (uint64_t)n) {
        /* Offset is beyond the data, return empty result */
        r->ofcall.rread.count = 0;
        r->ofcall.rread.data = buf;
    } else {
        /* Calculate how much data to return */
        size_t len = n - r->ifcall.tread.offset;
        if (len > r->ifcall.tread.count)
            len = r->ifcall.tread.count;
        
        /* Move the data to the beginning of the buffer */
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
    
    if (fd < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    buf = malloc(r->ifcall.tread.count);
    if (!buf) {
        close(fd);
        ixp_respond(r, "out of memory");
        return;
    }
    
    /* Position file pointer at the requested offset */
    off_t seek_result = lseek(fd, r->ifcall.tread.offset, SEEK_SET);
    if (seek_result == (off_t)-1) {
        free(buf);
        close(fd);
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* Read the requested data */
    ssize_t n = read(fd, buf, r->ifcall.tread.count);
    close(fd);
    
    if (n < 0) {
        free(buf);
        ixp_respond(r, strerror(errno));
        return;
    }
    
    r->ofcall.rread.count = n;
    r->ofcall.rread.data = buf;
    ixp_respond(r, nil);
    /* buf is now owned by libixp */
}