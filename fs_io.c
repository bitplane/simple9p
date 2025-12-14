#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <libgen.h> // For dirname

// fs_read handles Tread Fcall messages.
// It determines if the path is a directory, symlink, or regular file
// and calls the appropriate read function.
void fs_read(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;

    if (!state || !state->path) { // Ensure FidState and path are valid
        ixp_respond(r, "invalid fid state for read");
        return;
    }

    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, ixp_errbuf()); // getfullpath sets error via ixp_werrstr
        return;
    }

    // Use lstat to get information about the file/symlink itself
    if (lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    // Dispatch based on the type of file system object
    if (S_ISDIR(st.st_mode)) {
        read_directory(r, fullpath);
    } else if (S_ISLNK(st.st_mode)) {
        read_symlink(r, fullpath);
    } else if (S_ISREG(st.st_mode)) {
        read_file(r, fullpath);
    } else {
        // Not a directory, symlink, or regular file that we can read
        ixp_respond(r, strerror(EACCES)); // Or some other appropriate error
    }
}

// fs_write handles Twrite Fcall messages.
void fs_write(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    int fd;
    int write_os_flags;

    if (!state || !state->path) {
        ixp_respond(r, "invalid fid state for write");
        return;
    }

    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, ixp_errbuf());
        return;
    }

    // Check if the FID was opened with write permissions.
    if (!(state->open_flags & (O_WRONLY | O_RDWR))) {
        ixp_respond(r, strerror(EBADF)); // FID not opened for writing
        return;
    }
    
    // Determine the flags to use for opening the file
    // First, handle the base access mode (O_RDWR or O_WRONLY)
    if (state->open_flags & O_RDWR) {
        write_os_flags = O_RDWR;
    } else {
        write_os_flags = O_WRONLY;
    }
    
    // Handle append mode specifically
    int is_append = (state->open_flags & O_APPEND);
    if (is_append) {
        write_os_flags |= O_APPEND;
    }
    
    // Add O_CREAT for file creation if needed
    write_os_flags |= O_CREAT;
    
    // Handle truncation - if the offset is 0 and it's not append mode, 
    // and this is a fresh write, we might want to truncate
    if (r->ifcall.twrite.offset == 0 && !is_append && (state->open_flags & O_TRUNC)) {
        write_os_flags |= O_TRUNC;
    }

    // Debug print 
    if (debug) {
        fprintf(stderr, "fs_write: path=%s flags=%x append=%d offset=%lu count=%u\n", 
                fullpath, write_os_flags, is_append, (unsigned long)r->ifcall.twrite.offset, r->ifcall.twrite.count);
    }
    
    // Open the file with the determined flags - ensure it has appropriate permissions
    fd = open(fullpath, write_os_flags, 0666);
    if (fd < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    ssize_t n;
    
    if (is_append) {
        // For O_APPEND, we don't need to seek as the kernel will automatically
        // write at the end of the file. The offset from the 9P request is ignored.
        n = write(fd, r->ifcall.twrite.data, r->ifcall.twrite.count);
    } else {
        // If not in append mode, seek to the requested offset
        if (lseek(fd, r->ifcall.twrite.offset, SEEK_SET) < 0) {
            close(fd);
            ixp_respond(r, strerror(errno));
            return;
        }
        
        // Write the data at the specified offset
        n = write(fd, r->ifcall.twrite.data, r->ifcall.twrite.count);
    }
    
    close(fd);

    if (n < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    r->ofcall.rwrite.count = n;
    ixp_respond(r, nil);
}

// fs_open handles Topen Fcall messages.
void fs_open(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st;
    int flags = 0;
    
    if (!state) {
        ixp_respond(r, "invalid fid state");
        return;
    }
    
    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, "invalid path");
        return;
    }
    
    if (lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }
    
    /* Convert 9P open mode to Unix flags */
    switch (r->ifcall.topen.mode & 3) {
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
    
    if (r->ifcall.topen.mode & P9_OTRUNC)
        flags |= O_TRUNC;
    if (r->ifcall.topen.mode & P9_OAPPEND)
        flags |= O_APPEND;
    
    /* Store the mode and flags for later use */
    state->open_mode = r->ifcall.topen.mode;
    state->open_flags = flags;
    
    /* Test if we can actually open the file with these flags */
    if (!S_ISDIR(st.st_mode)) {
        int fd = open(fullpath, flags);
        if (fd < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        close(fd);
    }
    
    r->fid->qid.type = P9_QTFILE;
    if (S_ISDIR(st.st_mode))
        r->fid->qid.type = P9_QTDIR;
    else if (S_ISLNK(st.st_mode))
        r->fid->qid.type = P9_QTSYMLINK;
    
    r->fid->qid.path = st.st_ino;
    r->fid->qid.version = st.st_mtime;
    r->ofcall.ropen.qid = r->fid->qid;
    ixp_respond(r, nil);
}

// fs_create handles Tcreate Fcall messages.
void fs_create(Ixp9Req *r) {
    FidState *state = r->fid->aux; // FID for the parent directory
    char new_relative_path[PATH_MAX];
    char fullpath_os[PATH_MAX];    // Absolute OS path for the new file/dir
    struct stat st_new;            // To stat the newly created item
    int fd_create = -1;
    mode_t mode_os;
    FidState *new_fid_state;

    if (!state || !state->path) {
        ixp_respond(r, "invalid parent fid state for create");
        return;
    }

    if (strcmp(state->path, "/") == 0) {
        snprintf(new_relative_path, sizeof(new_relative_path), "/%s", r->ifcall.tcreate.name);
    } else {
        snprintf(new_relative_path, sizeof(new_relative_path), "%s/%s", state->path, r->ifcall.tcreate.name);
    }

    if (!getfullpath(new_relative_path, fullpath_os, sizeof(fullpath_os))) {
        ixp_respond(r, ixp_errbuf());
        return;
    }

    mode_os = r->ifcall.tcreate.perm & 0777;

    if (r->ifcall.tcreate.perm & P9_DMDIR) {
        if (mkdir(fullpath_os, mode_os) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    } else if (r->ifcall.tcreate.perm & P9_DMSYMLINK) {
        const char *target = r->ifcall.tcreate.extension;
        if (!target || !target[0]) {
            ixp_respond(r, "symlink target required");
            return;
        }
        if (symlink(target, fullpath_os) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    } else {
        // Create regular file
        int create_os_flags = O_CREAT | O_EXCL; 
        switch (r->ifcall.tcreate.mode & 3) { 
            case P9_OREAD:  create_os_flags |= O_RDONLY; break;
            case P9_OWRITE: create_os_flags |= O_WRONLY; break;
            case P9_ORDWR:  create_os_flags |= O_RDWR;   break;
        }
        if (r->ifcall.tcreate.mode & P9_OTRUNC) create_os_flags |= O_TRUNC;
        if (r->ifcall.tcreate.mode & P9_OAPPEND) create_os_flags |= O_APPEND;

        fd_create = open(fullpath_os, create_os_flags, mode_os);
        if (fd_create < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
        close(fd_create); 
    }

    if (lstat(fullpath_os, &st_new) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    if (r->fid->aux) {
        FidState* old_state_on_fid = r->fid->aux;
        if (old_state_on_fid->path) free(old_state_on_fid->path);
        free(old_state_on_fid);
        r->fid->aux = NULL;
    }
    
    new_fid_state = malloc(sizeof(FidState));
    if (!new_fid_state) {
        ixp_respond(r, "out of memory for new fid state");
        return;
    }

    new_fid_state->path = strdup(new_relative_path);
    if (!new_fid_state->path) {
        free(new_fid_state);
        ixp_respond(r, "out of memory for new fid path");
        return;
    }
    
    new_fid_state->open_mode = r->ifcall.tcreate.mode;
    new_fid_state->open_flags = 0; 
    switch (r->ifcall.tcreate.mode & 3) {
        case P9_OREAD:  new_fid_state->open_flags = O_RDONLY; break;
        case P9_OWRITE: new_fid_state->open_flags = O_WRONLY; break;
        case P9_ORDWR:  new_fid_state->open_flags = O_RDWR;   break;
    }
    if (r->ifcall.tcreate.mode & P9_OTRUNC) new_fid_state->open_flags |= O_TRUNC;
    if (r->ifcall.tcreate.mode & P9_OAPPEND) new_fid_state->open_flags |= O_APPEND;

    r->fid->aux = new_fid_state;

    r->fid->qid.path = st_new.st_ino;
    r->fid->qid.version = st_new.st_mtime;
    if (S_ISDIR(st_new.st_mode)) {
        r->fid->qid.type = P9_QTDIR;
    } else if (S_ISLNK(st_new.st_mode)) {
        r->fid->qid.type = P9_QTSYMLINK;
    } else {
        r->fid->qid.type = P9_QTFILE;
    }

    r->ofcall.rcreate.qid = r->fid->qid;
    r->ofcall.rcreate.iounit = 0; 
    
    ixp_respond(r, nil);
}

// fs_remove handles Tremove Fcall messages.
void fs_remove(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st; 

    if (!state || !state->path) {
        ixp_respond(r, "invalid fid state for remove");
        return;
    }

    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, ixp_errbuf());
        return;
    }

    if (lstat(fullpath, &st) < 0) {
        ixp_respond(r, strerror(errno)); 
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (rmdir(fullpath) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    } else { 
        if (unlink(fullpath) < 0) {
            ixp_respond(r, strerror(errno));
            return;
        }
    }
    ixp_respond(r, nil);
}