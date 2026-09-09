#include "namespace.h"
#include "server.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Namespace namespace;

static uint64_t hash_u64(uint64_t hash, uint64_t value) {
    unsigned int i;

    for(i = 0; i < 8; i++) {
        hash ^= value & 0xff;
        hash *= UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

static uint64_t hash_string(const char *string) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)string;

    while(*p) {
        hash ^= *p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : UINT64_MAX;
}

void namespace_free_roots(NamespaceRoot *roots, size_t count) {
    size_t i;

    for(i = 0; i < count; i++) {
        free(roots[i].name);
        free(roots[i].path);
    }
    free(roots);
}

static int encode_name(const char *name, char *encoded, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)name;

    if(!name || !name[0] || !encoded || size == 0)
        return -1;

    while(*p) {
        if(*p == '%' || *p == '/') {
            if(used + 3 >= size)
                return -1;
            encoded[used++] = '%';
            encoded[used++] = hex[*p >> 4];
            encoded[used++] = hex[*p & 0x0f];
        } else {
            if(used + 1 >= size)
                return -1;
            encoded[used++] = (char)*p;
        }
        p++;
    }
    encoded[used] = '\0';
    return 0;
}

void namespace_cleanup(void) {
    platform_namespace_cleanup(&namespace);
    free(namespace.native_root);
    namespace_free_roots(namespace.roots, namespace.nroots);
    memset(&namespace, 0, sizeof(namespace));
}

int namespace_use_native(Namespace *ns, const char *path) {
    char *copy;

    if(!ns || !path)
        return -1;
    copy = strdup(path);
    if(!copy)
        return -1;
    ns->native_root = copy;
    ns->synthetic = 0;
    return 0;
}

int namespace_use_synthetic(Namespace *ns) {
    if(!ns)
        return -1;
    ns->synthetic = 1;
    return 0;
}

int namespace_add_root(Namespace *ns, const char *name, const char *path) {
    NamespaceRoot *roots;
    char encoded[S9_PATH_MAX];
    char *name_copy;
    char *path_copy;
    uint64_t id;
    size_t i;

    if(!ns || !ns->synthetic || encode_name(name, encoded, sizeof(encoded)) < 0)
        return -1;
    id = hash_string(encoded);
    for(i = 0; i < ns->nroots; i++) {
        if(strcmp(ns->roots[i].name, encoded) == 0) {
            errno = EEXIST;
            return -1;
        }
        if(ns->roots[i].id == id) {
            errno = EEXIST;
            return -1;
        }
    }

    name_copy = strdup(encoded);
    path_copy = strdup(path);
    if(!name_copy || !path_copy) {
        free(name_copy);
        free(path_copy);
        return -1;
    }

    roots = realloc(ns->roots, (ns->nroots + 1) * sizeof(*roots));
    if(!roots) {
        free(name_copy);
        free(path_copy);
        return -1;
    }
    ns->roots = roots;
    ns->roots[ns->nroots].name = name_copy;
    ns->roots[ns->nroots].path = path_copy;
    ns->roots[ns->nroots].id = id;
    ns->nroots++;
    return 0;
}

static int compare_roots(const void *left, const void *right) {
    const NamespaceRoot *a = left;
    const NamespaceRoot *b = right;
    int result = strcmp(a->name, b->name);

    return result ? result : strcmp(a->path, b->path);
}

static int roots_equal(const Namespace *left, const Namespace *right) {
    size_t i;

    if(left->nroots != right->nroots)
        return 0;
    for(i = 0; i < left->nroots; i++) {
        if(strcmp(left->roots[i].name, right->roots[i].name) != 0 ||
           strcmp(left->roots[i].path, right->roots[i].path) != 0)
            return 0;
    }
    return 1;
}

int namespace_refresh(void) {
    Namespace candidate;

    if(!namespace.synthetic)
        return 0;
    memset(&candidate, 0, sizeof(candidate));
    candidate.synthetic = 1;
    if(platform_namespace_discover(&candidate) < 0) {
        namespace_free_roots(candidate.roots, candidate.nroots);
        return -1;
    }
    if(candidate.nroots > 1)
        qsort(candidate.roots, candidate.nroots, sizeof(*candidate.roots),
              compare_roots);
    if(roots_equal(&namespace, &candidate)) {
        namespace_free_roots(candidate.roots, candidate.nroots);
        return 0;
    }
    namespace_free_roots(namespace.roots, namespace.nroots);
    namespace.roots = candidate.roots;
    namespace.nroots = candidate.nroots;
    namespace.generation++;
    return 0;
}

int namespace_copy_roots(NamespaceRoot **roots, size_t *count) {
    NamespaceRoot *copy;
    size_t i;

    if(!roots || !count) {
        errno = EINVAL;
        return -1;
    }
    *roots = NULL;
    *count = 0;
    if(!namespace.nroots)
        return 0;
    copy = calloc(namespace.nroots, sizeof(*copy));
    if(!copy) {
        errno = ENOMEM;
        return -1;
    }
    for(i = 0; i < namespace.nroots; i++) {
        copy[i].name = strdup(namespace.roots[i].name);
        copy[i].path = strdup(namespace.roots[i].path);
        copy[i].id = namespace.roots[i].id;
        if(!copy[i].name || !copy[i].path) {
            namespace_free_roots(copy, namespace.nroots);
            errno = ENOMEM;
            return -1;
        }
    }
    *roots = copy;
    *count = namespace.nroots;
    return 0;
}

int namespace_init(const char *root) {
    int result;
    struct stat st;

    memset(&namespace, 0, sizeof(namespace));
    if(root) {
        if(stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
            if(errno == 0)
                errno = ENOTDIR;
            return -1;
        }
        result = namespace_use_native(&namespace, root);
    } else {
        result = platform_namespace_init(&namespace);
    }
    if(result < 0) {
        namespace_cleanup();
        return -1;
    }
    if(namespace.synthetic && namespace_refresh() < 0) {
        namespace_cleanup();
        return -1;
    }
    if(platform_namespace_ready(&namespace) < 0) {
        namespace_cleanup();
        return -1;
    }
    return 0;
}

static int find_root(const char *path, size_t length, size_t *index) {
    size_t i;

    for(i = 0; i < namespace.nroots; i++) {
        if(strlen(namespace.roots[i].name) == length &&
           strncmp(namespace.roots[i].name, path, length) == 0) {
            *index = i;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

/*
 * Reset a ResolvedPath without zeroing its two S9_PATH_MAX buffers. Only the
 * string contents matter to readers, and resolving runs once per directory
 * entry during a listing.
 */
static void clear_resolved(ResolvedPath *resolved) {
    resolved->synthetic = 0;
    resolved->export_root = 0;
    resolved->root_id = 0;
    resolved->root_path = NULL;
    resolved->native_path[0] = '\0';
    resolved->relative_path[0] = '\0';
}

int namespace_resolve(const char *path, ResolvedPath *resolved) {
    char cleaned[S9_PATH_MAX];
    const char *name;
    const char *remainder;
    size_t length;
    size_t index;

    if(!path || !resolved)
        return -1;
    if(strlen(path) >= sizeof(cleaned)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(cleaned, path);
    cleanname(cleaned);
    clear_resolved(resolved);

    if(!namespace.synthetic) {
        name = cleaned;
        while(*name == '/')
            name++;
        if(strlen(name) >= sizeof(resolved->relative_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(resolved->relative_path, name);
        if(joinpath(resolved->native_path, sizeof(resolved->native_path),
                    namespace.native_root, cleaned) < 0) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if(strcmp(cleaned, "/") == 0) {
        resolved->synthetic = 1;
        return 0;
    }

    name = cleaned;
    while(*name == '/')
        name++;
    remainder = strchr(name, '/');
    length = remainder ? (size_t)(remainder - name) : strlen(name);
    if(find_root(name, length, &index) < 0)
        return -1;

    resolved->root_id = namespace.roots[index].id;
    resolved->root_path = namespace.roots[index].path;
    resolved->export_root = remainder == NULL || remainder[1] == '\0';
    if(resolved->export_root) {
        if(strlen(namespace.roots[index].path) >= sizeof(resolved->native_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(resolved->native_path, namespace.roots[index].path);
        resolved->relative_path[0] = '\0';
        return 0;
    }
    if(strlen(remainder + 1) >= sizeof(resolved->relative_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(resolved->relative_path, remainder + 1);
    if(joinpath(resolved->native_path, sizeof(resolved->native_path),
                namespace.roots[index].path,
                remainder + 1) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int namespace_resolve_root(const NamespaceRoot *root,
                           ResolvedPath *resolved) {
    if(!root || !resolved) {
        errno = EINVAL;
        return -1;
    }
    if(strlen(root->path) >= sizeof(resolved->native_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    clear_resolved(resolved);
    resolved->export_root = 1;
    resolved->root_id = root->id;
    resolved->root_path = root->path;
    strcpy(resolved->native_path, root->path);
    return 0;
}

int namespace_valid_component(const char *name) {
    return name && name[0] && !strchr(name, '/') &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

char *namespace_join_virtual_alloc(const char *dir, const char *name) {
    size_t dir_length;
    size_t name_length;
    size_t length;
    char *path;

    if(!dir || !namespace_valid_component(name)) {
        errno = EINVAL;
        return NULL;
    }
    dir_length = strlen(dir);
    name_length = strlen(name);
    if(dir_length > SIZE_MAX - name_length - 2) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    length = dir_length + name_length + 2;
    path = s9_malloc(length);
    if(!path) {
        errno = ENOMEM;
        return NULL;
    }
    if(strcmp(dir, "/") == 0)
        snprintf(path, length, "/%s", name);
    else
        snprintf(path, length, "%s/%s", dir, name);
    return path;
}

int namespace_is_protected(const char *path) {
    const char *name;
    size_t index;

    if(!namespace.synthetic || !path)
        return 0;
    if(strcmp(path, "/") == 0)
        return 1;
    name = path;
    while(*name == '/')
        name++;
    if(!name[0] || strchr(name, '/'))
        return 0;
    return find_root(name, strlen(name), &index) == 0;
}

int namespace_is_namespace_root(const char *path) {
    return namespace.synthetic && path && strcmp(path, "/") == 0;
}

uint64_t namespace_root_qid(void) {
    return UINT64_C(1);
}

uint64_t namespace_qid(const ResolvedPath *resolved, const struct stat *st) {
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t root_id = 0;

    if(resolved->synthetic)
        return namespace_root_qid();
    if(namespace.synthetic)
        root_id = resolved->root_id;
    hash = hash_u64(hash, root_id);
    hash = hash_u64(hash, (uint64_t)st->st_dev);
    hash = hash_u64(hash, (uint64_t)st->st_ino);
    if(hash == namespace_root_qid())
        hash++;
    return hash;
}
