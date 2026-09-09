#ifndef SERVER_H
#define SERVER_H

#include <ixp.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "namespace.h"

#define nil NULL
#define S9_DIR_CHECKPOINTS 16
/* Plan 9's DMSETVTX. libixp's header stops at DMSETGID. */
#define S9_DMSETVTX 0x00010000U
#define S9_DMSPECIAL (P9_DMSETUID | P9_DMSETGID | S9_DMSETVTX)
#ifndef S9_MAX_FIDS
#define S9_MAX_FIDS 1024
#endif

typedef struct FidState FidState;

typedef struct DirCheckpoint {
    uint64_t offset;
    long cookie;
} DirCheckpoint;

typedef struct NineDServer {
    FidState *fids;
    size_t fid_count;
    uint32_t qid_generation;
    int read_only;
} NineDServer;

/* Global variables */
extern IxpServer server;
extern int debug;
extern Ixp9Srv p9srv;
extern NineDServer nined;

/* Fid state structure to track open files */
struct FidState {
    char *path;
    int open_mode;   /* 9P open mode */
    int open_flags;  /* Unix open flags */
    int fd;
    DIR *dir;
    char *symlink;
    size_t symlink_length;
    uint64_t dir_offset;
    NamespaceRoot *namespace_roots;
    size_t namespace_root_count;
    DirCheckpoint checkpoints[S9_DIR_CHECKPOINTS];
    size_t checkpoint_count;
    size_t checkpoint_next;
    int remove_on_close;
    int stat_valid;
    struct stat opened_stat;
    FidState *previous;
    FidState *next;
};

/* Positioned I/O. Build with -DS9_NO_PREAD on a libc without pread(2). */
ssize_t s9_pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t s9_pwrite(int fd, const void *buffer, size_t size, off_t offset);

void *s9_malloc(size_t size);
char *s9_strdup(const char *string);
void s9_free(void *pointer);
#ifdef NINED_TESTING
void s9_fail_allocation_after(long count);
#endif

FidState *fid_state_create(const char *path);
void fid_state_destroy(FidState *state);
void fid_state_register(FidState *state);
void fid_state_unregister(FidState *state);
void fid_state_close(FidState *state);
uint32_t qid_version(const struct stat *st);
void qid_bump(void);
void nined_state_cleanup(void);
void respond_errno(Ixp9Req *r, int error);
uint32_t fs_read_count(const Ixp9Req *r);
char *read_symlink_target(const ResolvedPath *path, size_t hint,
                          size_t *length);

/* Path functions */
void cleanname(char *name);
const char *path_basename(const char *path);
int joinpath(char *dst, size_t dstsize, const char *dir, const char *name);

/* Filesystem operations */
void fs_attach(Ixp9Req *r);
void fs_walk(Ixp9Req *r);
void fs_open(Ixp9Req *r);
void fs_read(Ixp9Req *r);
void fs_write(Ixp9Req *r);
void fs_create(Ixp9Req *r);
void fs_remove(Ixp9Req *r);
void fs_clunk(Ixp9Req *r);
void fs_stat(Ixp9Req *r);
void fs_wstat(Ixp9Req *r);
void fs_flush(Ixp9Req *r);
void fs_freefid(IxpFid *f);

/* Directory operations */
void read_directory(Ixp9Req *r, FidState *state);
void read_synthetic_directory(Ixp9Req *r, FidState *state);
void read_symlink(Ixp9Req *r, FidState *state);
void read_file(Ixp9Req *r, FidState *state);

/* Stat helpers */
int build_stat(IxpStat *s, const char *path, const ResolvedPath *resolved,
               const struct stat *st,
               const char *symlink_target);
int build_synthetic_stat(IxpStat *s, const char *name);
void free_stat_strings(IxpStat *s);
#ifdef NINED_TESTING
int test_prepare_rename_updates(const char *old_path, const char *new_path);
#endif

#endif /* SERVER_H */
