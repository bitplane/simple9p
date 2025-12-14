#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For truncate, chmod, readlink
#include <errno.h>
#include <libgen.h> // For basename
#include <limits.h> // For LONG_MAX

// build_stat populates an IxpStat structure from a file's stat data.
// s: The IxpStat structure to populate.
// path: The relative 9P path of the file.
// fullpath: The absolute OS path of the file.
// st: The struct stat obtained from lstat() on fullpath.
void build_stat(IxpStat *s, const char *path, const char *fullpath, struct stat *st) {
    char *name_component = NULL;
    char *path_copy_for_basename = NULL;

    // Initialize basic fields
    s->type = 0; // Typically 0 for 9P2000
    s->dev = 0;  // Typically 0 for 9P2000

    // Determine QID type
    s->qid.type = P9_QTFILE; // Default to file
    if (S_ISDIR(st->st_mode)) {
        s->qid.type = P9_QTDIR;
    } else if (S_ISLNK(st->st_mode)) {
        s->qid.type = P9_QTSYMLINK;
    }

    // QID path and version
    s->qid.path = st->st_ino;       // Use inode number for path component of QID
    s->qid.version = st->st_mtime;  // Use modification time for version

    // Mode: 9P permissions and directory/symlink flags
    s->mode = st->st_mode & 0777; // Basic Unix permissions
    if (S_ISDIR(st->st_mode)) {
        s->mode |= P9_DMDIR;
    } else if (S_ISLNK(st->st_mode)) {
        s->mode |= P9_DMSYMLINK;
    }
    // Other types like P9_DMAPPEND, P9_DMEXCL, P9_DMAUTH could be set if applicable

    // Timestamps
    s->atime = st->st_atime;
    s->mtime = st->st_mtime;

    // Length and blocks - use exactly what the OS reports
    s->length = st->st_size;
    
    // For symlinks, store target in extension field and set length
    if (S_ISLNK(st->st_mode)) {
        char target_buf[PATH_MAX];
        ssize_t len = readlink(fullpath, target_buf, sizeof(target_buf) - 1);
        if (len != -1) {
            target_buf[len] = '\0';
            s->length = len;
            s->extension = strdup(target_buf);
        }
    }

    // 9P2000.u numeric IDs
    s->n_uid = st->st_uid;
    s->n_gid = st->st_gid;
    s->n_muid = st->st_uid;

    // Name: The last component of the path
    // Handle root path specifically
    if (strcmp(path, "/") == 0) {
        s->name = strdup("/"); // Allocate a new copy to be consistent with other cases
    } else {
        path_copy_for_basename = strdup(path);
        if (path_copy_for_basename) {
            name_component = basename(path_copy_for_basename);
            s->name = strdup(name_component); // Duplicate basename result for safety
                                              // libixp will manage freeing this if it takes ownership
                                              // or if it copies the content during packing.
            free(path_copy_for_basename);
        } else {
            // Fallback if strdup fails - this is not ideal
            s->name = strdup("error_name"); // Still allocate to maintain consistency
        }
    }

    // User and group names
    // For simplicity, using environment USER or "none". 9P allows string UIDs.
    const char *user = getenv("USER");
    s->uid = user ? strdup(user) : strdup("none");
    s->gid = strdup(s->uid);  // Same as uid
    s->muid = strdup(s->uid); // Last modifier same as uid
}

// Helper to free allocated strings in IxpStat
void free_stat_strings(IxpStat *s) {
    if (s->name) free((char*)s->name);
    if (s->uid) free((char*)s->uid);
    if (s->gid) free((char*)s->gid);
    if (s->muid) free((char*)s->muid);
    if (s->extension) free((char*)s->extension);
}

// fs_stat handles Tstat messages.
void fs_stat(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    struct stat st_os; // OS stat structure
    IxpStat s_ixp;     // 9P stat structure
    IxpMsg m;
    uint16_t size_of_ixp_stat;

    if (!state || !state->path) {
        ixp_respond(r, "invalid fid state");
        return;
    }

    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        // getfullpath calls ixp_werrstr, so just return
        ixp_respond(r, ixp_errbuf());
        return;
    }

    if (lstat(fullpath, &st_os) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    memset(&s_ixp, 0, sizeof(IxpStat)); // Zero out the structure
    build_stat(&s_ixp, state->path, fullpath, &st_os);

    size_of_ixp_stat = ixp_sizeof_stat(&s_ixp, ixp_req_getversion(r));
    r->ofcall.rstat.nstat = size_of_ixp_stat;
    r->ofcall.rstat.stat = malloc(size_of_ixp_stat);

    if (!r->ofcall.rstat.stat) {
        free_stat_strings(&s_ixp);
        ixp_respond(r, "out of memory");
        return;
    }

    m = ixp_message(r->ofcall.rstat.stat, size_of_ixp_stat, MsgPack);
    m.version = ixp_req_getversion(r);
    ixp_pstat(&m, &s_ixp);

    // Free allocated strings after packing
    free_stat_strings(&s_ixp);

    ixp_respond(r, nil);
    // r->ofcall.rstat.stat is now owned by libixp and will be freed by it.
}

// fs_wstat handles Twstat messages.
void fs_wstat(Ixp9Req *r) {
    FidState *state = r->fid->aux;
    char fullpath[PATH_MAX];
    IxpStat *s_new = &r->ifcall.twstat.stat; // The new stat data from client
    struct stat current_st_os;               // Current OS attributes of the file
    int respond_early = 0;
    char *original_fid_path_on_success_rename = NULL;

    if (debug) {
        fprintf(stderr, "fs_wstat: path=%s, length=%llu (mask=%llu)\n", 
                state ? state->path : "NULL", 
                (unsigned long long)s_new->length, 
                (unsigned long long)~0ULL);
    }

    if (!state || !state->path) {
        ixp_respond(r, "invalid fid state");
        return;
    }

    if (!getfullpath(state->path, fullpath, sizeof(fullpath))) {
        ixp_respond(r, ixp_errbuf());
        return;
    }

    if (lstat(fullpath, &current_st_os) < 0) {
        ixp_respond(r, strerror(errno));
        return;
    }

    // Handle length change (truncate)
    // This is a special case that's particularly important to handle correctly
    // The FUSE protocol uses ~0ULL as a "don't change" marker for the length field
    if (s_new->length != (uint64_t)~0ULL) {
        if (debug) {
            fprintf(stderr, "fs_wstat: truncating file to %llu (current %llu)\n", 
                    (unsigned long long)s_new->length, 
                    (unsigned long long)current_st_os.st_size);
        }
        
        if (S_ISDIR(current_st_os.st_mode)) {
            // Can't truncate a directory
            ixp_respond(r, strerror(EISDIR));
            respond_early = 1;
        } else {
            // For regular files, perform the truncate
            // First check if we actually need to truncate (optimization)
            if (s_new->length != (uint64_t)current_st_os.st_size) {
                // Validate the truncate length is reasonable
                if (s_new->length > (uint64_t)LONG_MAX) {
                    // Most filesystem APIs can't handle sizes larger than LONG_MAX
                    ixp_respond(r, strerror(EFBIG));
                    respond_early = 1;
                } else {
                    if (truncate(fullpath, (off_t)s_new->length) < 0) {
                        ixp_respond(r, strerror(errno));
                        respond_early = 1;
                    }
                }
            }
        }
    }

    if (respond_early)
        return;

    // Handle mode changes (chmod)
    // P9_BIT32_MASK (~0U) is the "don't change" marker for uint32_t fields.
    if (s_new->mode != (uint32_t)~0) {
        mode_t requested_perms = s_new->mode & 0777; // Apply only permission bits
        if (requested_perms != (current_st_os.st_mode & 0777)) {
            if (chmod(fullpath, requested_perms) < 0) {
                ixp_respond(r, strerror(errno));
                respond_early = 1;
            }
        }
    }

    if (respond_early)
        return;

    // Handle name changes (rename)
    // s_new->name being NULL or empty means "don't change name".
    if (s_new->name != NULL && s_new->name[0] != '\0') {
        char current_basename_buf[PATH_MAX];
        char *current_basename;
        char *path_copy_for_basename;

        path_copy_for_basename = strdup(state->path);
        if (!path_copy_for_basename) {
             ixp_respond(r, "out of memory for wstat rename check");
             respond_early = 1;
        } else {
            current_basename = basename(path_copy_for_basename);
            // Check if the new name is actually different from the current one.
            if (strcmp(current_basename, s_new->name) != 0) {
                char new_relative_path[PATH_MAX];
                char new_absolute_fullpath[PATH_MAX];
                char *dir_part_copy; // dirname can modify its input
                char *original_path_copy_for_dirname;

                original_path_copy_for_dirname = strdup(state->path);
                if (!original_path_copy_for_dirname) {
                    ixp_respond(r, "out of memory for wstat rename");
                    respond_early = 1;
                } else {
                    dir_part_copy = dirname(original_path_copy_for_dirname);

                    if (strcmp(dir_part_copy, ".") == 0 && strchr(state->path, '/') == NULL) {
                        snprintf(new_relative_path, sizeof(new_relative_path), "%s", s_new->name);
                    } else if (strcmp(dir_part_copy, "/") == 0) {
                        snprintf(new_relative_path, sizeof(new_relative_path), "/%s", s_new->name);
                    } else {
                        snprintf(new_relative_path, sizeof(new_relative_path), "%s/%s", dir_part_copy, s_new->name);
                    }
                    free(original_path_copy_for_dirname);

                    if (!getfullpath(new_relative_path, new_absolute_fullpath, sizeof(new_absolute_fullpath))) {
                        ixp_respond(r, ixp_errbuf());
                        respond_early = 1;
                    } else {
                        if (rename(fullpath, new_absolute_fullpath) < 0) {
                            ixp_respond(r, strerror(errno));
                            respond_early = 1;
                        } else {
                            original_fid_path_on_success_rename = state->path; // Keep old path pointer
                            state->path = strdup(new_relative_path);
                            if (!state->path) {
                                // Critical: OS rename succeeded, but server state update failed.
                                // Try to restore old path to prevent FID from being totally broken.
                                state->path = original_fid_path_on_success_rename;
                                original_fid_path_on_success_rename = NULL; // Don't free it below
                                ixp_respond(r, "out of memory after rename, server state inconsistent");
                                respond_early = 1;
                                // Consider logging this critical failure.
                            } else {
                                free(original_fid_path_on_success_rename); // Free the old path string
                            }
                        }
                    }
                }
            }
            free(path_copy_for_basename);
        }
    }

    if (respond_early)
        return;

    // Other wstat operations (e.g., mtime, uid, gid) are not implemented.
    // Client would set s_new->mtime, s_new->uid, etc. to non-"don't change" values.

    ixp_respond(r, nil);
}