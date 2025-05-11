#ifndef SERVER_H
#define SERVER_H

#include <ixp.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#define nil NULL

/* Global variables */
extern IxpServer server;
extern char *root_path;
extern int debug;
extern Ixp9Srv p9srv;

/* Fid state structure to track open files */
typedef struct FidState {
    char *path;
    int open_mode;   /* 9P open mode */
    int open_flags;  /* Unix open flags */
} FidState;

/* Path functions */
void cleanname(char *name);
char *getfullpath(const char *path, char *buffer, size_t bufsize);
int safe_strcat(char *dst, const char *src, size_t dstsize);

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
void read_directory(Ixp9Req *r, const char *fullpath);
void read_symlink(Ixp9Req *r, const char *fullpath);
void read_file(Ixp9Req *r, const char *fullpath);

/* Stat helpers */
void build_stat(IxpStat *s, const char *path, const char *fullpath, struct stat *st);

#endif /* SERVER_H */