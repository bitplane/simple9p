#include "server.h"
#include <string.h>
#include <stdio.h>

/* Clean a path - remove . and .. components, duplicate slashes, etc. */
void cleanname(char *name) {
    char *p, *q, *dotdot;
    int rooted;

    if(name[0] == '\0')
        return;

    rooted = (name[0] == '/');
    
    /* invariants:
     *  p points at beginning of path element we're considering.
     *  q points just past the last path element we wrote (no slash).
     *  dotdot points just past the point where .. cannot backtrack
     *    any further (no slash).
     */
    p = q = dotdot = name + rooted;
    while(*p) {
        if(p[0] == '/') {
            p++;
        } else if(p[0] == '.' && (p[1] == '\0' || p[1] == '/')) {
            p++;
        } else if(p[0] == '.' && p[1] == '.' && (p[2] == '\0' || p[2] == '/')) {
            p += 2;
            if(q > dotdot) {
                /* can backtrack */
                while(--q > dotdot && q[-1] != '/')
                    ;
            } else if(!rooted) {
                /* /.. is / but ./../ is .. */
                if(q != name)
                    *q++ = '/';
                *q++ = '.';
                *q++ = '.';
                dotdot = q;
            }
        } else {
            /* real path element */
            if(q != name + rooted)
                *q++ = '/';
            while((*q = *p) != '\0' && *q != '/')
                p++, q++;
        }
    }
    
    if(q == name) {
        if(rooted) {
            *q++ = '/';
        } else {
            *q++ = '.';
        }
    }
    *q = '\0';
}

/* Helper to build full path - now thread-safe and secure */
char *getfullpath(const char *path, char *buffer, size_t bufsize) {
    char cleaned[PATH_MAX];
    
    if(!path || !buffer || bufsize < PATH_MAX) {
        return NULL;
    }
    
    /* Copy and clean the path */
    strncpy(cleaned, path, PATH_MAX-1);
    cleaned[PATH_MAX-1] = '\0';
    cleanname(cleaned);
    
    /* Check for directory traversal attempts */
    if(strstr(cleaned, "../") != NULL) {
        ixp_werrstr("Invalid path: directory traversal attempt");
        return NULL;
    }
    
    /* Build the full path */
    if(snprintf(buffer, bufsize, "%s%s", root_path, cleaned) >= bufsize) {
        ixp_werrstr("Path too long");
        return NULL;
    }
    
    return buffer;
}

/* Safe string operations */
int safe_strcat(char *dst, const char *src, size_t dstsize) {
    size_t dstlen = strlen(dst);
    size_t srclen = strlen(src);
    
    if(dstlen + srclen >= dstsize) {
        return -1;
    }
    
    strcat(dst, src);
    return 0;
}