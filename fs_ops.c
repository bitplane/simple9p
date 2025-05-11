#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h> // For O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_TRUNC

// fs_attach handles the Tattach Fcall.
// It initializes a new FidState for the root of the filesystem.
void fs_attach(Ixp9Req *r) {
    FidState *state = malloc(sizeof(FidState));
    if (!state) {
        ixp_respond(r, "out of memory");
        return;
    }

    state->path = strdup("/"); // Represents the root of the served directory
    if (!state->path) {
        free(state);
        ixp_respond(r, "out of memory");
        return;
    }
    state->open_mode = 0;  // Not opened in a specific mode yet
    state->open_flags = 0; // No OS flags yet

    // Set the QID for the root directory
    // For simplicity, using inode 0 for root, but a real stat might be better
    // if root_path itself has a stable inode.
    // However, QID.path for attach is often special.
    // Let's stat the actual root_path to get its properties.
    char fullpath_root[PATH_MAX];
    struct stat st_root;
    if (!getfullpath("/", fullpath_root, sizeof(fullpath_root))) {
         free(state->path);
         free(state);
         ixp_respond(r, ixp_errbuf());
         return;
    }
    if (lstat(fullpath_root, &st_root) < 0) {
        free(state->path);
        free(state);
        ixp_respond(r, strerror(errno));
        return;
    }

    r->fid->qid.type = P9_QTDIR; // Root is always a directory
    r->fid->qid.path = st_root.st_ino; 
    r->fid->qid.version = st_root.st_mtime; 
    r->fid->aux = state;
    r->ofcall.rattach.qid = r->fid->qid;
    ixp_respond(r, nil);
}

// fs_walk handles the Twalk Fcall.
// It navigates the filesystem, creating a new FID (newfid) for the target path.
void fs_walk(Ixp9Req *r) {
    FidState *state = r->fid->aux; // Current FID's state
    FidState *newstate;            // State for the new FID (r->newfid)
    char current_relative_path[PATH_MAX];
    char fullpath_os[PATH_MAX];    // For lstat
    struct stat st;
    int i;

    if (!state || !state->path) {
        ixp_respond(r, "invalid fid state for walk");
        return;
    }

    // Clone current fid state for the new fid
    newstate = malloc(sizeof(FidState));
    if (!newstate) {
        ixp_respond(r, "out of memory");
        return;
    }
    newstate->path = strdup(state->path);
    if (!newstate->path) {
        free(newstate);
        ixp_respond(r, "out of memory");
        return;
    }
    newstate->open_mode = 0;  // New FID is not opened yet
    newstate->open_flags = 0;
    r->newfid->aux = newstate; // Attach new state to the new FID

    // If no names to walk (nwname == 0), newfid is a clone of fid
    if (r->ifcall.twalk.nwname == 0) {
        r->newfid->qid = r->fid->qid; // QID is the same
        // newstate->path is already a copy of state->path
        ixp_respond(r, nil);
        return;
    }

    // Make a mutable copy of the path for constructing the new path
    strncpy(current_relative_path, newstate->path, PATH_MAX -1);
    current_relative_path[PATH_MAX -1] = '\0';

    for (i = 0; i < r->ifcall.twalk.nwname; i++) {
        const char *name_component = r->ifcall.twalk.wname[i];

        // Append path component
        if (strcmp(current_relative_path, "/") != 0) { // Avoid "//" for root
            if (safe_strcat(current_relative_path, "/", PATH_MAX) < 0) {
                ixp_respond(r, "path too long during walk");
                // Note: newfid->aux (newstate) needs cleanup if we error out early.
                // libixp's ixp_requtf GONE might handle freeing newfid and its aux.
                // For now, assuming libixp handles it on error response.
                return;
            }
        }
        if (safe_strcat(current_relative_path, name_component, PATH_MAX) < 0) {
            ixp_respond(r, "path too long during walk");
            return;
        }
        
        // Clean the path (e.g. resolve "." and "..")
        // cleanname modifies in place.
        // It's important that current_relative_path remains a valid 9P-style path.
        // cleanname is more for OS paths. For 9P paths, ".." should be handled by server logic.
        // For now, we assume wname components are valid names, not ".." or ".".
        // If ".." is encountered, it should be resolved against current_relative_path.
        // This simple server doesn't fully implement ".." resolution in walk beyond getfullpath.

        if (!getfullpath(current_relative_path, fullpath_os, sizeof(fullpath_os))) {
            ixp_respond(r, ixp_errbuf()); // Path became invalid
            return;
        }

        if (lstat(fullpath_os, &st) < 0) {
            // If any component doesn't exist, walk fails.
            // Respond with error, and number of successful walks (i)
            r->ofcall.rwalk.nwqid = i; // Report how many names were successfully walked
            ixp_respond(r, strerror(errno));
            return;
        }

        // Store QID for this successfully walked component
        r->ofcall.rwalk.wqid[i].path = st.st_ino;
        r->ofcall.rwalk.wqid[i].version = st.st_mtime;
        if (S_ISDIR(st.st_mode)) {
            r->ofcall.rwalk.wqid[i].type = P9_QTDIR;
        } else if (S_ISLNK(st.st_mode)) {
            r->ofcall.rwalk.wqid[i].type = P9_QTSYMLINK;
        } else {
            r->ofcall.rwalk.wqid[i].type = P9_QTFILE;
        }
    }

    // All components walked successfully
    r->ofcall.rwalk.nwqid = i;
    r->newfid->qid = r->ofcall.rwalk.wqid[i - 1]; // QID of the final target

    // Update the path in newstate to the final walked path
    free(newstate->path);
    newstate->path = strdup(current_relative_path);
    if (!newstate->path) {
        // This is tricky: walk succeeded, but server ran out of memory for final path.
        // Should ideally not happen.
        ixp_respond(r, "out of memory storing final path for walk");
        // The newfid is now in an inconsistent state.
        return;
    }

    ixp_respond(r, nil);
}

// fs_clunk handles the Tclunk Fcall.
// It signifies that a FID is no longer needed by the client.
// The server should release any resources associated with the FID.
void fs_clunk(Ixp9Req *r) {
    // FidState is freed by fs_freefid, which is called by libixp
    // after fs_clunk responds or if the FID is implicitly clunked (e.g. Tremove).
    ixp_respond(r, nil);
}

// fs_flush handles the Tflush Fcall.
// It's used to abort a pending request. This simple server doesn't
// have complex pending requests that would need explicit flushing logic beyond what libixp handles.
void fs_flush(Ixp9Req *r) {
    // For a simple server, responding nil is usually sufficient.
    // libixp handles the actual flushing of messages for the old tag.
    ixp_respond(r, nil);
}

// fs_freefid is called by libixp when a FID is destroyed.
// It should free any auxiliary data (FidState) attached to the FID.
void fs_freefid(IxpFid *f) {
    if (f && f->aux) {
        FidState *state = f->aux;
        if (state->path) {
            free(state->path);
            state->path = NULL;
        }
        free(state);
        f->aux = NULL;
    }
}