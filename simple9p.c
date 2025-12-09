#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/* Global variables */
IxpServer server;
char *root_path = NULL;
int debug = 0;

/* 9P server operations */
Ixp9Srv p9srv = {
    .attach = fs_attach,
    .walk = fs_walk,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .create = fs_create,
    .remove = fs_remove,
    .clunk = fs_clunk,
    .stat = fs_stat,
    .wstat = fs_wstat,
    .flush = fs_flush,
    .freefid = fs_freefid,
};

/* Handle device connection directly */
static void serve_device(int fd) {
    if(debug)
        fprintf(stderr, "serve_device: Starting with fd=%d\n", fd);
    
    /* Since this is a character device, we treat it as a direct connection */
    /* not a listening socket, so we handle it directly */
    
    /* Create a connection structure */
    IxpConn conn = {0};
    conn.fd = fd;
    conn.srv = &server;
    conn.aux = &p9srv;  /* Pass the 9P server structure */
    
    if(debug)
        fprintf(stderr, "serve_device: Serving 9P directly on device\n");
    
    /* Serve 9P protocol on this device */
    ixp_serve9conn(&conn);
    
    if(debug)
        fprintf(stderr, "serve_device: Connection closed\n");
}

int main(int argc, char *argv[]) {
    char *addr = nil;
    int c;
    
    while((c = getopt(argc, argv, "dp:")) != -1) {
        switch(c) {
        case 'd':
            debug = 1;
            break;
        case 'p':
            addr = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s [-d] [-p address] <directory>\n", argv[0]);
            exit(1);
        }
    }
    
    if(optind >= argc) {
        fprintf(stderr, "Usage: %s [-d] [-p address] <directory>\n", argv[0]);
        exit(1);
    }
    
    root_path = argv[optind];
    
    /* Check if root_path exists and is a directory */
    struct stat root_st;
    if(stat(root_path, &root_st) < 0) {
        fprintf(stderr, "Cannot stat root directory %s: %s\n", root_path, strerror(errno));
        exit(1);
    }
    
    if(!S_ISDIR(root_st.st_mode)) {
        fprintf(stderr, "Root path %s is not a directory\n", root_path);
        exit(1);
    }
    
    int fd;
    
    if(!addr) {
        addr = "tcp!*!564";
    }
    
    if(debug)
        fprintf(stderr, "Starting 9P server on %s for %s\n", addr, root_path);

    /* Check for stdio mode */
    if(strcmp(addr, "-") == 0) {
        /* Use stdin/stdout for 9P - requires bidirectional fd */
        /* Caller should use: simple9p -p - /path <> /dev/device */
        fd = STDIN_FILENO;
        if(debug)
            fprintf(stderr, "Using stdio (fd %d) for 9P\n", fd);

        /* Initialize server structure */
        memset(&server, 0, sizeof(server));
        server.aux = &p9srv;

        /* Serve on stdin/stdout */
        serve_device(fd);

        return 0;
    }

    /* Check if addr is a device file */
    struct stat st;
    if(stat(addr, &st) == 0 && S_ISCHR(st.st_mode)) {
        /* It's a character device - open it directly */
        fd = open(addr, O_RDWR);
        if(fd < 0) {
            fprintf(stderr, "Failed to open device %s: %s\n", addr, strerror(errno));
            exit(1);
        }
        if(debug)
            fprintf(stderr, "Opened device %s as fd %d\n", addr, fd);
        
        /* Initialize server structure */
        memset(&server, 0, sizeof(server));
        server.aux = &p9srv;
        
        /* Serve the device connection directly */
        serve_device(fd);
        
        close(fd);
    } else {
        /* Try as network address */
        fd = ixp_announce(addr);
        if(fd < 0) {
            fprintf(stderr, "Failed to announce on %s: %s\n", addr, strerror(errno));
            exit(1);
        }
        
        /* Initialize server */
        memset(&server, 0, sizeof(server));
        
        /* Start listening */
        ixp_listen(&server, fd, &p9srv, ixp_serve9conn, nil);
        
        /* Run server loop */
        ixp_serverloop(&server);
    }
    
    return 0;
}