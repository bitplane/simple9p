#define _GNU_SOURCE
#include <ixp.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Client {
    int fd;
    uint16_t tag;
    uint version;
    char send_buffer[IXP_MAX_MSG];
    char receive_buffer[IXP_MAX_MSG];
} Client;

static void fail_response(const char *operation, const IxpFcall *response) {
    fprintf(stderr, "%s: unexpected response %u", operation,
            response->hdr.type);
    if(response->hdr.type == P9_RError)
        fprintf(stderr, ": %s", response->error.ename);
    fputc('\n', stderr);
    abort();
}

static IxpFcall rpc(Client *client, IxpFcall *request) {
    IxpMsg message;
    IxpFcall response;

    memset(&response, 0, sizeof(response));
    request->hdr.tag = request->hdr.type == P9_TVersion ? IXP_NOTAG
                                                        : client->tag++;
    message = ixp_message(client->send_buffer, sizeof(client->send_buffer),
                          MsgPack);
    message.version = client->version;
    assert(ixp_fcall2msg(&message, request) != 0);
    assert(ixp_sendmsg(client->fd, &message) != 0);
    message = ixp_message(client->receive_buffer,
                          sizeof(client->receive_buffer), MsgUnpack);
    message.version = client->version;
    assert(ixp_recvmsg(client->fd, &message) != 0);
    assert(ixp_msg2fcall(&message, &response) != 0);
    return response;
}

static void expect_type(const char *operation, IxpFcall *response,
                        uint8_t type) {
    if(response->hdr.type != type)
        fail_response(operation, response);
}

static void expect_error(const char *operation, IxpFcall *response,
                         int error) {
    expect_type(operation, response, P9_RError);
    if(strcmp(response->error.ename, strerror(error)) != 0 ||
       response->error.uerrno != (uint32_t)error) {
        fprintf(stderr, "%s: expected %s (%d), got %s (%u)\n",
                operation, strerror(error), error, response->error.ename,
                response->error.uerrno);
        abort();
    }
}

static void version(Client *client) {
    IxpFcall request = {0};
    IxpFcall response;

    request.hdr.type = P9_TVersion;
    request.version.msize = IXP_MAX_MSG;
    request.version.version = "9P2000.u";
    response = rpc(client, &request);
    expect_type("version", &response, P9_RVersion);
    assert(strcmp(response.version.version, "9P2000.u") == 0);
    client->version = IXP_V9P2000U;
    ixp_freefcall(&response);
}

static IxpFcall send_attach(Client *client, uint32_t fid) {
    IxpFcall request = {0};

    request.hdr.type = P9_TAttach;
    request.hdr.fid = fid;
    request.tattach.afid = IXP_NOFID;
    request.tattach.uname = "test";
    request.tattach.aname = "";
    request.tattach.n_uname = (uint32_t)getuid();
    return rpc(client, &request);
}

static void attach(Client *client, uint32_t fid) {
    IxpFcall response = send_attach(client, fid);

    expect_type("attach", &response, P9_RAttach);
    ixp_freefcall(&response);
}

static IxpFcall walk(Client *client, uint32_t fid, uint32_t newfid,
                     const char **names, uint16_t count) {
    IxpFcall request = {0};
    uint16_t index;

    request.hdr.type = P9_TWalk;
    request.hdr.fid = fid;
    request.twalk.newfid = newfid;
    request.twalk.nwname = count;
    for(index = 0; index < count; index++)
        request.twalk.wname[index] = (char *)names[index];
    return rpc(client, &request);
}

static IxpFcall open_fid(Client *client, uint32_t fid, uint8_t mode) {
    IxpFcall request = {0};

    request.hdr.type = P9_TOpen;
    request.hdr.fid = fid;
    request.topen.mode = mode;
    return rpc(client, &request);
}

static IxpFcall create_fid(Client *client, uint32_t fid, const char *name,
                           uint32_t perm, uint8_t mode,
                           const char *extension) {
    IxpFcall request = {0};

    request.hdr.type = P9_TCreate;
    request.hdr.fid = fid;
    request.tcreate.name = (char *)name;
    request.tcreate.perm = perm;
    request.tcreate.mode = mode;
    request.tcreate.extension = (char *)(extension ? extension : "");
    return rpc(client, &request);
}

static IxpFcall read_fid(Client *client, uint32_t fid, uint64_t offset,
                         uint32_t count) {
    IxpFcall request = {0};

    request.hdr.type = P9_TRead;
    request.hdr.fid = fid;
    request.tread.offset = offset;
    request.tread.count = count;
    return rpc(client, &request);
}

static IxpFcall write_fid(Client *client, uint32_t fid, uint64_t offset,
                          const char *data, uint32_t count) {
    IxpFcall request = {0};

    request.hdr.type = P9_TWrite;
    request.hdr.fid = fid;
    request.twrite.offset = offset;
    request.twrite.count = count;
    request.twrite.data = (char *)data;
    return rpc(client, &request);
}

static IxpFcall stat_fid(Client *client, uint32_t fid) {
    IxpFcall request = {0};

    request.hdr.type = P9_TStat;
    request.hdr.fid = fid;
    return rpc(client, &request);
}

static IxpFcall clunk(Client *client, uint32_t fid) {
    IxpFcall request = {0};

    request.hdr.type = P9_TClunk;
    request.hdr.fid = fid;
    return rpc(client, &request);
}

static IxpFcall remove_fid(Client *client, uint32_t fid) {
    IxpFcall request = {0};

    request.hdr.type = P9_TRemove;
    request.hdr.fid = fid;
    return rpc(client, &request);
}

static IxpStat unchanged_stat(void) {
    IxpStat stat;

    memset(&stat, 0, sizeof(stat));
    stat.type = UINT16_MAX;
    stat.dev = UINT32_MAX;
    stat.qid.type = UINT8_MAX;
    stat.qid.version = UINT32_MAX;
    stat.qid.path = UINT64_MAX;
    stat.mode = UINT32_MAX;
    stat.atime = UINT32_MAX;
    stat.mtime = UINT32_MAX;
    stat.length = UINT64_MAX;
    stat.name = "";
    stat.uid = "";
    stat.gid = "";
    stat.muid = "";
    stat.extension = "";
    stat.n_uid = UINT32_MAX;
    stat.n_gid = UINT32_MAX;
    stat.n_muid = UINT32_MAX;
    return stat;
}

static IxpFcall wstat_fid(Client *client, uint32_t fid, IxpStat *stat) {
    IxpFcall request = {0};

    request.hdr.type = P9_TWStat;
    request.hdr.fid = fid;
    request.twstat.stat = *stat;
    return rpc(client, &request);
}

static IxpStat unpack_stat(Client *client, uint32_t fid) {
    IxpFcall response;
    IxpStat stat = {0};
    IxpMsg message;

    response = stat_fid(client, fid);
    expect_type("stat", &response, P9_RStat);
    message = ixp_message((char *)response.rstat.stat,
                          response.rstat.nstat, MsgUnpack);
    message.version = client->version;
    ixp_pstat(&message, &stat);
    ixp_freefcall(&response);
    return stat;
}

static IxpQid stat_qid(Client *client, uint32_t fid) {
    IxpStat stat = unpack_stat(client, fid);
    IxpQid qid = stat.qid;

    ixp_freestat(&stat);
    return qid;
}

static void write_file(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    size_t length = strlen(data);

    assert(fd >= 0);
    assert(write(fd, data, length) == (ssize_t)length);
    assert(close(fd) == 0);
}

static void write_roots(const char *path, const char **names,
                        const char **roots, size_t count) {
    FILE *file = fopen(path, "w");
    size_t index;

    assert(file);
    for(index = 0; index < count; index++)
        assert(fprintf(file, "%s\t%s\n", names[index], roots[index]) > 0);
    assert(fclose(file) == 0);
}

static size_t directory_names(Client *client, IxpFcall *response,
                              const char **expected, size_t expected_count) {
    IxpMsg message;
    size_t count = 0;
    size_t first_length = 0;

    expect_type("directory listing", response, P9_RRead);
    message = ixp_message(response->rread.data, response->rread.count,
                          MsgUnpack);
    message.version = client->version;
    while(message.pos < message.end) {
        IxpStat stat = {0};
        char *before = message.pos;

        ixp_pstat(&message, &stat);
        assert(message.pos > before);
        if(count == 0)
            first_length = (size_t)(message.pos - before);
        assert(count < expected_count);
        assert(strcmp(stat.name, expected[count]) == 0);
        count++;
        ixp_freestat(&stat);
    }
    assert(count == expected_count);
    return first_length;
}

static void write_pattern_file(const char *path, size_t length) {
    char buffer[1024];
    size_t written = 0;
    size_t index;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    while(written < length) {
        size_t count = length - written;

        if(count > sizeof(buffer))
            count = sizeof(buffer);
        for(index = 0; index < count; index++)
            buffer[index] = (char)('a' + (written + index) % 26);
        assert(write(fd, buffer, count) == (ssize_t)count);
        written += count;
    }
    assert(close(fd) == 0);
}

static void make_path(char *buffer, size_t size, const char *root,
                      const char *name) {
    assert(snprintf(buffer, size, "%s/%s", root, name) < (int)size);
}

static pid_t start_server_mode(const char *binary, const char *root,
                               Client *client, int read_only) {
    int sockets[2];
    pid_t child;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(sockets[0]);
        assert(dup2(sockets[1], STDIN_FILENO) == STDIN_FILENO);
        close(sockets[1]);
        if(read_only && root)
            execl(binary, binary, "-r", "-p", "-", root, (char *)NULL);
        else if(read_only)
            execl(binary, binary, "-r", "-p", "-", (char *)NULL);
        else if(root)
            execl(binary, binary, "-p", "-", root, (char *)NULL);
        else
            execl(binary, binary, "-p", "-", (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    memset(client, 0, sizeof(*client));
    client->fd = sockets[0];
    client->version = IXP_V9P2000;
    version(client);
    attach(client, 1);
    return child;
}

static pid_t start_raw_server(const char *binary, const char *root,
                              Client *client) {
    int sockets[2];
    pid_t child;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(sockets[0]);
        assert(dup2(sockets[1], STDIN_FILENO) == STDIN_FILENO);
        close(sockets[1]);
        execl(binary, binary, "-p", "-", root, (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    memset(client, 0, sizeof(*client));
    client->fd = sockets[0];
    client->version = IXP_V9P2000;
    return child;
}

static pid_t start_server(const char *binary, const char *root,
                          Client *client) {
    return start_server_mode(binary, root, client, 0);
}

static void stop_server(Client *client, pid_t child) {
    int status;

    close(client->fd);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void send_fcall(Client *client, IxpFcall *request) {
    IxpMsg message = ixp_message(client->send_buffer,
                                 sizeof(client->send_buffer), MsgPack);

    message.version = client->version;
    assert(ixp_fcall2msg(&message, request) != 0);
    assert(ixp_sendmsg(client->fd, &message) != 0);
}

static size_t drain_connection(Client *client) {
    char buffer[256];
    size_t total = 0;
    ssize_t count;

    shutdown(client->fd, SHUT_WR);
    while((count = read(client->fd, buffer, sizeof(buffer))) > 0)
        total += (size_t)count;
    assert(count == 0);
    return total;
}

static void expect_clean_server_exit(Client *client, pid_t child) {
    int status;

    close(client->fd);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
}

static void test_reject_small_msize(const char *binary, const char *root,
                                    uint32_t msize) {
    IxpFcall request = {0};
    Client client;
    pid_t child = start_raw_server(binary, root, &client);

    request.hdr.type = P9_TVersion;
    request.hdr.tag = IXP_NOTAG;
    request.version.msize = msize;
    request.version.version = "9P2000";
    send_fcall(&client, &request);
    assert(drain_connection(&client) == 0);
    expect_clean_server_exit(&client, child);
}

static void test_invalid_negotiation(const char *binary, const char *root) {
    static const uint32_t invalid[] = { 0, 1, 23, 24 };
    size_t index;

    for(index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++)
        test_reject_small_msize(binary, root, invalid[index]);
}

static void test_response_pack_failure(const char *binary, const char *root) {
    IxpFcall request = {0};
    IxpFcall response = {0};
    IxpMsg message;
    Client client;
    pid_t child = start_raw_server(binary, root, &client);

    request.hdr.type = P9_TVersion;
    request.hdr.tag = IXP_NOTAG;
    request.version.msize = 25;
    request.version.version = "9P2000";
    send_fcall(&client, &request);
    message = ixp_message(client.receive_buffer, sizeof(client.receive_buffer),
                          MsgUnpack);
    assert(ixp_recvmsg(client.fd, &message) != 0);
    assert(ixp_msg2fcall(&message, &response) != 0);
    expect_type("small version", &response, P9_RVersion);
    assert(response.version.msize == 25);
    ixp_freefcall(&response);

    memset(&request, 0, sizeof(request));
    request.hdr.type = P9_TError;
    request.hdr.tag = 1;
    send_fcall(&client, &request);
    assert(drain_connection(&client) == 0);
    expect_clean_server_exit(&client, child);
}

static void test_walks(Client *client) {
    const char *partial[] = { "dir", "missing", "later" };
    const char *directory[] = { "dir" };
    const char *invalid[] = { ".." };
    IxpFcall response;

    response = walk(client, 1, 2, partial, 3);
    expect_type("partial walk", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 1);
    ixp_freefcall(&response);
    response = open_fid(client, 2, P9_OREAD);
    expect_type("partial newfid discarded", &response, P9_RError);
    ixp_freefcall(&response);

    response = walk(client, 1, 1, partial, 3);
    expect_type("partial same-fid walk", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 1);
    ixp_freefcall(&response);
    response = walk(client, 1, 3, directory, 1);
    expect_type("same fid unchanged", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 1);
    ixp_freefcall(&response);
    response = clunk(client, 3);
    expect_type("clunk", &response, P9_RClunk);
    ixp_freefcall(&response);

    response = walk(client, 1, 4, invalid, 1);
    expect_error("invalid component", &response, EINVAL);
    ixp_freefcall(&response);
}

static void test_libixp_fid_cleanup(Client *client) {
    const char *directory[] = { "dir" };
    IxpFcall response;

    response = send_attach(client, 1);
    expect_type("duplicate attach", &response, P9_RError);
    ixp_freefcall(&response);

    response = walk(client, 1, 100, NULL, 0);
    expect_type("zero-element fid clone", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 0);
    ixp_freefcall(&response);
    response = clunk(client, 100);
    expect_type("clunk cloned fid", &response, P9_RClunk);
    ixp_freefcall(&response);

    response = walk(client, 1, 101, directory, 1);
    expect_type("create walked fid", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = walk(client, 1, 101, directory, 1);
    expect_type("duplicate walk fid", &response, P9_RError);
    ixp_freefcall(&response);
    response = clunk(client, 101);
    expect_type("clunk original walked fid", &response, P9_RClunk);
    ixp_freefcall(&response);
}

static void test_fid_limit(Client *client) {
    IxpFcall response;
    uint32_t index;

    for(index = 0; index < S9_MAX_FIDS - 1; index++) {
        response = walk(client, 1, 1000 + index, NULL, 0);
        expect_type("clone fid below limit", &response, P9_RWalk);
        ixp_freefcall(&response);
    }
    response = walk(client, 1, 1000 + index, NULL, 0);
    expect_error("reject fid above limit", &response, EMFILE);
    ixp_freefcall(&response);
    while(index > 0) {
        index--;
        response = clunk(client, 1000 + index);
        expect_type("clunk bounded fid", &response, P9_RClunk);
        ixp_freefcall(&response);
    }
}

static void test_rename_and_open_identity(Client *client, const char *root) {
    const char *file[] = { "dir", "file" };
    IxpFcall response;
    IxpStat stat;
    char path[1024];

    response = walk(client, 1, 10, file, 2);
    expect_type("walk file", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = walk(client, 1, 11, file, 2);
    expect_type("walk second file fid", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 10, P9_ORDWR);
    expect_type("open file", &response, P9_ROpen);
    ixp_freefcall(&response);

    stat = unchanged_stat();
    stat.name = "renamed";
    response = wstat_fid(client, 11, &stat);
    expect_type("rename", &response, P9_RWStat);
    ixp_freefcall(&response);
    response = open_fid(client, 11, P9_OREAD);
    expect_type("second fid follows rename", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(client, 10, 0, 32);
    expect_type("open fid survives rename", &response, P9_RRead);
    assert(response.rread.count == 8);
    assert(memcmp(response.rread.data, "original", 8) == 0);
    ixp_freefcall(&response);

    make_path(path, sizeof(path), root, "dir/renamed");
    assert(unlink(path) == 0);
    response = write_fid(client, 10, 0, "changed!", 8);
    expect_type("write unlinked open file", &response, P9_RWrite);
    ixp_freefcall(&response);
    assert(access(path, F_OK) < 0 && errno == ENOENT);
}

static void test_truncate_and_symlink(Client *client, const char *root) {
    const char *truncate_name[] = { "truncate" };
    const char *link_name[] = { "link" };
    IxpFcall response;
    IxpStat link_stat;
    struct stat st;
    char path[1024];

    response = walk(client, 1, 20, truncate_name, 1);
    expect_type("walk truncate", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 20, P9_ORDWR | P9_OTRUNC);
    expect_type("open truncate", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = write_fid(client, 20, 0, "", 0);
    expect_type("zero write", &response, P9_RWrite);
    assert(response.rwrite.count == 0);
    ixp_freefcall(&response);
    response = write_fid(client, 20, UINT64_MAX, "x", 1);
    expect_type("reject unrepresentable write offset", &response, P9_RError);
    ixp_freefcall(&response);
    response = read_fid(client, 20, 0, 0);
    expect_type("zero read", &response, P9_RRead);
    assert(response.rread.count == 0);
    ixp_freefcall(&response);
    response = read_fid(client, 20, UINT64_MAX, 1);
    expect_type("reject unrepresentable read offset", &response, P9_RError);
    ixp_freefcall(&response);
    response = write_fid(client, 20, 0, "12345678", 8);
    expect_type("first truncate write", &response, P9_RWrite);
    ixp_freefcall(&response);
    response = write_fid(client, 20, 0, "xy", 2);
    expect_type("second truncate write", &response, P9_RWrite);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "truncate");
    assert(stat(path, &st) == 0 && st.st_size == 8);

    response = walk(client, 1, 21, link_name, 1);
    expect_type("walk symlink", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 21, P9_OREAD);
    expect_type("open symlink", &response, P9_ROpen);
    ixp_freefcall(&response);
    link_stat = unpack_stat(client, 21);
    assert(link_stat.length == strlen("target/path"));
    assert(strcmp(link_stat.extension, "target/path") == 0);
    ixp_freestat(&link_stat);
    response = read_fid(client, 21, 3, 4);
    expect_type("ranged symlink read", &response, P9_RRead);
    assert(response.rread.count == 4);
    assert(memcmp(response.rread.data, "get/", 4) == 0);
    ixp_freefcall(&response);
    response = read_fid(client, 21, strlen("target/path"), 4);
    expect_type("symlink eof", &response, P9_RRead);
    assert(response.rread.count == 0);
    ixp_freefcall(&response);
}

static void test_directory_offsets(Client *client) {
    const char *directory[] = { "entries" };
    IxpFcall response;
    uint32_t first_count;
    uint32_t full_count;
    uint64_t offset;
    unsigned int index;

    response = walk(client, 1, 30, directory, 1);
    expect_type("walk directory", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 30, P9_OREAD);
    expect_type("open directory", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(client, 30, 0, 100);
    expect_type("first directory read", &response, P9_RRead);
    assert(response.rread.count > 0);
    first_count = response.rread.count;
    ixp_freefcall(&response);
    response = read_fid(client, 30, first_count, 100);
    expect_type("checkpoint directory read", &response, P9_RRead);
    ixp_freefcall(&response);
    offset = first_count;
    for(index = 0; index < 20; index++) {
        response = read_fid(client, 30, offset, 100);
        expect_type("advance directory checkpoints", &response, P9_RRead);
        assert(response.rread.count > 0);
        offset += response.rread.count;
        ixp_freefcall(&response);
    }
    response = read_fid(client, 30, first_count, 100);
    expect_type("rescan evicted directory checkpoint", &response, P9_RRead);
    assert(response.rread.count > 0);
    ixp_freefcall(&response);
    response = read_fid(client, 30, 1, 100);
    expect_type("mid-entry directory offset", &response, P9_RError);
    ixp_freefcall(&response);
    response = read_fid(client, 30, 0, 8192);
    expect_type("complete directory read", &response, P9_RRead);
    full_count = response.rread.count;
    assert(full_count > 0);
    ixp_freefcall(&response);
    response = read_fid(client, 30, (uint64_t)full_count + 1, 100);
    expect_type("directory offset beyond end", &response, P9_RError);
    ixp_freefcall(&response);
}

static void test_bounded_reads(Client *client) {
    const char *large[] = { "large" };
    const char *directory[] = { "entries" };
    const char *link[] = { "link" };
    IxpFcall response;
    uint32_t iounit;
    uint32_t index;

    response = walk(client, 1, 31, large, 1);
    expect_type("walk large file", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 31, P9_OREAD);
    expect_type("open large file", &response, P9_ROpen);
    iounit = response.ropen.iounit;
    assert(iounit > 0);
    ixp_freefcall(&response);
    response = read_fid(client, 31, 0, UINT32_MAX);
    expect_type("bounded regular-file read", &response, P9_RRead);
    assert(response.rread.count == iounit);
    for(index = 0; index < response.rread.count; index++)
        assert(response.rread.data[index] == (char)('a' + index % 26));
    ixp_freefcall(&response);

    response = walk(client, 1, 32, directory, 1);
    expect_type("walk bounded directory", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 32, P9_OREAD);
    expect_type("open bounded directory", &response, P9_ROpen);
    iounit = response.ropen.iounit;
    ixp_freefcall(&response);
    response = read_fid(client, 32, 0, UINT32_MAX);
    expect_type("bounded directory read", &response, P9_RRead);
    assert(response.rread.count > 0);
    assert(response.rread.count <= iounit);
    ixp_freefcall(&response);

    response = walk(client, 1, 33, link, 1);
    expect_type("walk bounded symlink", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 33, P9_OREAD);
    expect_type("open bounded symlink", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(client, 33, 0, UINT32_MAX);
    expect_type("bounded symlink read", &response, P9_RRead);
    assert(response.rread.count == strlen("target/path"));
    assert(memcmp(response.rread.data, "target/path",
                  response.rread.count) == 0);
    ixp_freefcall(&response);
}

static void test_hardlinks_and_versions(Client *client) {
    const char *first[] = { "hard-a" };
    const char *second[] = { "hard-b" };
    IxpFcall response;
    IxpStat a;
    IxpStat b;
    IxpStat stat;
    IxpMsg message;
    uint32_t version_before;

    response = walk(client, 1, 40, first, 1);
    expect_type("walk hardlink a", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 1);
    a.qid = response.rwalk.wqid[0];
    ixp_freefcall(&response);
    response = walk(client, 1, 41, second, 1);
    expect_type("walk hardlink b", &response, P9_RWalk);
    b.qid = response.rwalk.wqid[0];
    assert(a.qid.path == b.qid.path);
    ixp_freefcall(&response);
    response = open_fid(client, 40, P9_ORDWR);
    expect_type("open hardlink", &response, P9_ROpen);
    version_before = response.ropen.qid.version;
    ixp_freefcall(&response);
    response = write_fid(client, 40, 0, "z", 1);
    expect_type("write hardlink", &response, P9_RWrite);
    ixp_freefcall(&response);
    response = stat_fid(client, 40);
    expect_type("stat changed file", &response, P9_RStat);
    message = ixp_message((char *)response.rstat.stat,
                          response.rstat.nstat, MsgUnpack);
    message.version = client->version;
    memset(&a, 0, sizeof(a));
    ixp_pstat(&message, &a);
    assert(a.qid.version != version_before);
    ixp_freestat(&a);
    ixp_freefcall(&response);

    stat = unchanged_stat();
    stat.name = "hard-renamed";
    response = wstat_fid(client, 40, &stat);
    expect_type("rename one hardlink", &response, P9_RWStat);
    ixp_freefcall(&response);
    response = open_fid(client, 41, P9_OREAD);
    expect_type("other hardlink path unchanged", &response, P9_ROpen);
    ixp_freefcall(&response);
}

static void test_descendant_rename(Client *client) {
    const char *descendant[] = { "tree", "child", "file" };
    const char *tree[] = { "tree" };
    IxpFcall response;
    IxpStat stat;

    response = walk(client, 1, 60, descendant, 3);
    expect_type("walk descendant", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = walk(client, 1, 61, tree, 1);
    expect_type("walk parent", &response, P9_RWalk);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    stat.name = "moved";
    response = wstat_fid(client, 61, &stat);
    expect_type("rename parent", &response, P9_RWStat);
    ixp_freefcall(&response);
    response = open_fid(client, 60, P9_OREAD);
    expect_type("descendant fid follows parent rename", &response, P9_ROpen);
    ixp_freefcall(&response);
}

static void test_wstat_validation(Client *client, const char *root) {
    const char *name[] = { "metadata" };
    IxpFcall response;
    IxpStat stat;
    struct stat before;
    struct stat native;
    char path[1024];
    char data[8] = {0};
    int fd;

    response = walk(client, 1, 70, name, 1);
    expect_type("walk metadata", &response, P9_RWalk);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    stat.mode = 0640;
    stat.atime = 1000000000;
    stat.mtime = 1000000001;
    response = wstat_fid(client, 70, &stat);
    expect_type("combined metadata wstat", &response, P9_RWStat);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "metadata");
    assert(lstat(path, &native) == 0);
    assert((native.st_mode & 0777) == 0640);
    assert(native.st_size == 8);
    assert(native.st_atime == 1000000000);
    assert(native.st_mtime == 1000000001);

    before = native;
    stat = unchanged_stat();
    stat.length = 4;
    stat.mode = 0600;
    stat.mtime = 1000000002;
    response = wstat_fid(client, 70, &stat);
    expect_type("truncate combined with metadata", &response, P9_RWStat);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0);
    assert(native.st_size == 4);
    assert((native.st_mode & 0777) == 0600);
    assert(native.st_atime == before.st_atime);
    assert(native.st_mtime == 1000000002);

    stat = unchanged_stat();
    stat.length = 4;
    response = wstat_fid(client, 70, &stat);
    expect_type("standalone truncate wstat", &response, P9_RWStat);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0 && native.st_size == 4);

    stat = unchanged_stat();
    stat.uid = "someone";
    stat.length = 1;
    response = wstat_fid(client, 70, &stat);
    expect_error("unsupported ownership wstat", &response, EOPNOTSUPP);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0 && native.st_size == 4);

    stat = unchanged_stat();
    stat.n_uid = (uint32_t)getuid();
    stat.n_gid = (uint32_t)getgid();
    response = wstat_fid(client, 70, &stat);
    expect_type("numeric ownership wstat", &response, P9_RWStat);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0);
    assert(native.st_uid == getuid() && native.st_gid == getgid());

    stat = unchanged_stat();
    stat.uid = "0";
    if(getuid() == 0)
        stat.uid = "1";
    stat.gid = "";
    stat.mode = 0604;
    response = wstat_fid(client, 70, &stat);
    if(getuid() == 0) {
        expect_type("string ownership wstat as root", &response, P9_RWStat);
        ixp_freefcall(&response);
        assert(lstat(path, &native) == 0 && native.st_uid == 1);
        assert((native.st_mode & 0777) == 0604);
        assert(chown(path, 0, (gid_t)-1) == 0);
        assert(chmod(path, 0600) == 0);
    } else {
        expect_error("foreign ownership wstat", &response, EPERM);
        ixp_freefcall(&response);
        assert(lstat(path, &native) == 0 && native.st_uid == getuid());
        assert((native.st_mode & 0777) == 0600);
    }

    assert(lstat(path, &before) == 0);
    stat = unchanged_stat();
    stat.name = "metadata-renamed";
    stat.length = 1;
    stat.mode = 0600;
    stat.mtime = 1000000002;
    response = wstat_fid(client, 70, &stat);
    expect_error("rename combined with mutation", &response, EINVAL);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0);
    assert(native.st_size == before.st_size);
    assert((native.st_mode & 0777) == (before.st_mode & 0777));
    assert(native.st_atime == before.st_atime);
    assert(native.st_mtime == before.st_mtime);
    make_path(path, sizeof(path), root, "metadata-renamed");
    assert(access(path, F_OK) < 0 && errno == ENOENT);
    make_path(path, sizeof(path), root, "metadata");
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, data, sizeof(data)) == 4);
    assert(memcmp(data, "meta", 4) == 0);
    assert(close(fd) == 0);
}

static void test_create_validation(Client *client, const char *root) {
    IxpFcall response;
    struct stat st;
    char path[1024];

    attach(client, 80);
    response = create_fid(client, 80, "pipe", P9_DMNAMEDPIPE | 0600,
                          P9_ORDWR, NULL);
    expect_type("reject unsupported create type", &response, P9_RError);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "pipe");
    assert(lstat(path, &st) < 0 && errno == ENOENT);
}

static void test_containment(Client *client, const char *outside) {
    const char *escape[] = { "escape", "secret" };
    const char *link_only[] = { "escape" };
    IxpFcall response;
    IxpStat stat;
    struct stat before;
    struct stat after;
    char path[1024];
    char data[16] = {0};
    int fd;

    response = walk(client, 1, 90, escape, 2);
    expect_type("reject intermediate symlink", &response, P9_RWalk);
    assert(response.rwalk.nwqid == 1);
    ixp_freefcall(&response);
    response = walk(client, 1, 91, link_only, 1);
    expect_type("walk terminal symlink", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 91, P9_OREAD);
    expect_type("open terminal symlink", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(client, 91, 0, 64);
    expect_type("read terminal symlink", &response, P9_RRead);
    assert(response.rread.count == strlen(outside));
    assert(memcmp(response.rread.data, outside, strlen(outside)) == 0);
    ixp_freefcall(&response);

    make_path(path, sizeof(path), outside, "secret");
    assert(lstat(path, &before) == 0);
    stat = unchanged_stat();
    stat.length = 0;
    response = wstat_fid(client, 91, &stat);
    expect_type("do not truncate through symlink", &response, P9_RError);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    stat.mode = P9_DMSYMLINK | 0777;
    response = wstat_fid(client, 91, &stat);
    expect_type("do not chmod through symlink", &response, P9_RError);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    stat.mtime = 1000000001;
    response = wstat_fid(client, 91, &stat);
    expect_type("do not set times through symlink", &response, P9_RError);
    ixp_freefcall(&response);
    assert(lstat(path, &after) == 0);
    assert(after.st_size == before.st_size);
    assert((after.st_mode & 0777) == (before.st_mode & 0777));
    assert(after.st_mtime == before.st_mtime);
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(read(fd, data, sizeof(data)) == 7);
    assert(memcmp(data, "outside", 7) == 0);
    close(fd);
}

static void test_orclose_and_exec(Client *client, const char *root) {
    const char *temporary[] = { "temporary" };
    const char *executable[] = { "executable" };
    const char *plain[] = { "plain" };
    const char *empty_directory[] = { "orclose-empty" };
    const char *full_directory[] = { "orclose-full" };
    IxpFcall response;
    char path[1024];

    response = walk(client, 1, 50, temporary, 1);
    expect_type("walk temporary", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 50, P9_OREAD | P9_ORCLOSE);
    expect_type("open orclose", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = clunk(client, 50);
    expect_type("clunk orclose", &response, P9_RClunk);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "temporary");
    assert(access(path, F_OK) < 0 && errno == ENOENT);

    response = walk(client, 1, 53, empty_directory, 1);
    expect_type("walk empty orclose directory", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 53, P9_OREAD | P9_ORCLOSE);
    expect_type("open empty orclose directory", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = clunk(client, 53);
    expect_type("remove empty orclose directory", &response, P9_RClunk);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "orclose-empty");
    assert(access(path, F_OK) < 0 && errno == ENOENT);

    response = walk(client, 1, 54, full_directory, 1);
    expect_type("walk nonempty orclose directory", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 54, P9_OREAD | P9_ORCLOSE);
    expect_type("open nonempty orclose directory", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = clunk(client, 54);
    expect_type("reject nonempty orclose directory", &response, P9_RError);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "orclose-full");
    assert(access(path, F_OK) == 0);

    response = walk(client, 1, 51, executable, 1);
    expect_type("walk executable", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 51, P9_OEXEC);
    expect_type("open executable", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = walk(client, 1, 52, plain, 1);
    expect_type("walk plain", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 52, P9_OEXEC);
    expect_type("deny execute", &response, P9_RError);
    ixp_freefcall(&response);
}

static void test_qid_mutations(Client *client) {
    const char *created[] = { "qid-file" };
    IxpFcall response;
    IxpStat stat;
    IxpQid root_before;
    IxpQid root_after;
    IxpQid file_before;
    IxpQid file_after;

    root_before = stat_qid(client, 1);
    response = walk(client, 1, 120, NULL, 0);
    expect_type("clone root for qid create", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = create_fid(client, 120, "qid-file", 0600, P9_ORDWR, NULL);
    expect_type("create qid file", &response, P9_RCreate);
    file_before = response.rcreate.qid;
    ixp_freefcall(&response);
    root_after = stat_qid(client, 1);
    assert(root_after.version != root_before.version);

    response = write_fid(client, 120, 0, "qid", 3);
    expect_type("write qid file", &response, P9_RWrite);
    ixp_freefcall(&response);
    file_after = stat_qid(client, 120);
    assert(file_after.version != file_before.version);
    response = clunk(client, 120);
    expect_type("clunk created qid file", &response, P9_RClunk);
    ixp_freefcall(&response);

    response = walk(client, 1, 121, created, 1);
    expect_type("walk qid file", &response, P9_RWalk);
    file_before = response.rwalk.wqid[0];
    ixp_freefcall(&response);
    response = open_fid(client, 121, P9_OWRITE | P9_OTRUNC);
    expect_type("truncate qid file", &response, P9_ROpen);
    assert(response.ropen.qid.version != file_before.version);
    file_before = response.ropen.qid;
    ixp_freefcall(&response);

    stat = unchanged_stat();
    stat.mode = 0640;
    response = wstat_fid(client, 121, &stat);
    expect_type("chmod qid file", &response, P9_RWStat);
    ixp_freefcall(&response);
    file_after = stat_qid(client, 121);
    assert(file_after.version != file_before.version);

    root_before = stat_qid(client, 1);
    stat = unchanged_stat();
    stat.name = "qid-renamed";
    response = wstat_fid(client, 121, &stat);
    expect_type("rename qid file", &response, P9_RWStat);
    ixp_freefcall(&response);
    root_after = stat_qid(client, 1);
    assert(root_after.version != root_before.version);

    root_before = root_after;
    response = remove_fid(client, 121);
    expect_type("remove qid file", &response, P9_RRemove);
    ixp_freefcall(&response);
    root_after = stat_qid(client, 1);
    assert(root_after.version != root_before.version);
}

static void test_partial_wstat_qid(Client *client, const char *root) {
    const char *name[] = { "partial-wstat" };
    IxpFcall response;
    IxpStat stat;
    IxpQid before;
    IxpQid after;
    char path[1024];
    char data[32] = {0};

    response = walk(client, 1, 122, name, 1);
    expect_type("walk partial wstat file", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(client, 122, P9_ORDWR);
    expect_type("open partial wstat file", &response, P9_ROpen);
    before = response.ropen.qid;
    ixp_freefcall(&response);

    make_path(path, sizeof(path), root, "partial-wstat");
    assert(unlink(path) == 0);
    if(getuid() != 0) {
        stat = unchanged_stat();
        stat.length = 2;
        stat.mode = 0604;
        stat.n_uid = (uint32_t)getuid() + 1;
        response = wstat_fid(client, 122, &stat);
        expect_error("failed chown rolls back mode before truncating",
                     &response, EPERM);
        ixp_freefcall(&response);
        after = stat_qid(client, 122);
        assert(after.version == before.version);
        stat = unpack_stat(client, 122);
        assert((stat.mode & 0777) == 0600);
        assert(stat.length == strlen("partial-wstat"));
        ixp_freestat(&stat);
        response = read_fid(client, 122, 0, sizeof(data));
        expect_type("failed wstat leaves data unchanged", &response,
                    P9_RRead);
        assert(response.rread.count == strlen("partial-wstat"));
        assert(memcmp(response.rread.data, "partial-wstat",
                      response.rread.count) == 0);
        ixp_freefcall(&response);
    }

    /* What ftruncate(2) sends on a 9P2000.u mount: length and mtime. */
    stat = unchanged_stat();
    stat.length = 2;
    stat.mtime = 1000000002;
    response = wstat_fid(client, 122, &stat);
    expect_type("ftruncate-style wstat on an open fid", &response, P9_RWStat);
    ixp_freefcall(&response);
    stat = unpack_stat(client, 122);
    assert(stat.length == 2);
    assert(stat.mtime == 1000000002);
    ixp_freestat(&stat);
    response = read_fid(client, 122, 0, sizeof(data));
    expect_type("read truncated file", &response, P9_RRead);
    assert(response.rread.count == 2);
    assert(memcmp(response.rread.data, "pa", 2) == 0);
    ixp_freefcall(&response);
}

static void expect_mode(Client *client, uint32_t fid, const char *name,
                        uint32_t type, uint32_t permissions,
                        const char *extension) {
    const char *walk_name[] = { name };
    IxpFcall response;
    IxpStat stat;

    response = walk(client, 1, fid, walk_name, 1);
    expect_type(name, &response, P9_RWalk);
    ixp_freefcall(&response);
    stat = unpack_stat(client, fid);
    if((stat.mode & ~0777U) != type || (stat.mode & 0777) != permissions ||
       strcmp(stat.extension, extension) != 0) {
        fprintf(stderr, "%s: mode %#x extension '%s', expected %#x '%s'\n",
                name, stat.mode, stat.extension, type | permissions,
                extension);
        abort();
    }
    ixp_freestat(&stat);
}

static void test_special_files(Client *client, const char *root) {
    IxpFcall response;
    IxpStat stat;
    struct stat native;
    char path[1024];

    expect_mode(client, 130, "fifo", P9_DMNAMEDPIPE, 0600, "");
    expect_mode(client, 131, "sock", P9_DMSOCKET, 0700, "");
    expect_mode(client, 132, "setuid", P9_DMSETUID | P9_DMSETGID, 0755, "");
    make_path(path, sizeof(path), root, "chardev");
    if(lstat(path, &native) == 0)
        expect_mode(client, 133, "chardev", P9_DMDEVICE, 0600, "c 1 3");

    response = open_fid(client, 130, P9_OREAD);
    expect_error("open fifo", &response, EOPNOTSUPP);
    ixp_freefcall(&response);

    stat = unchanged_stat();
    stat.mode = P9_DMSETUID | 0700;
    response = wstat_fid(client, 132, &stat);
    expect_type("chmod setuid", &response, P9_RWStat);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "setuid");
    assert(lstat(path, &native) == 0);
    assert((native.st_mode & 07777) == (S_ISUID | 0700));
    stat = unchanged_stat();
    stat.mode = P9_DMDEVICE | 0700;
    response = wstat_fid(client, 132, &stat);
    expect_error("chmod cannot change type", &response, EOPNOTSUPP);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    stat.mode = 0700;
    response = wstat_fid(client, 132, &stat);
    expect_type("chmod clears setuid", &response, P9_RWStat);
    ixp_freefcall(&response);
    assert(lstat(path, &native) == 0 && (native.st_mode & 07777) == 0700);

    response = walk(client, 1, 134, NULL, 0);
    expect_type("clone root for setuid create", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = create_fid(client, 134, "suid-created", P9_DMSETUID | 0700,
                          P9_ORDWR, NULL);
    expect_type("create setuid file", &response, P9_RCreate);
    ixp_freefcall(&response);
    make_path(path, sizeof(path), root, "suid-created");
    assert(lstat(path, &native) == 0);
    assert((native.st_mode & 07777) == (S_ISUID | 0700));
    response = clunk(client, 134);
    expect_type("clunk setuid file", &response, P9_RClunk);
    ixp_freefcall(&response);
}

static void test_read_only(const char *binary, const char *root) {
    const char *name[] = { "read-only" };
    IxpFcall response;
    IxpStat requested;
    Client client;
    pid_t child;
    struct stat native;
    char path[1024];

    child = start_server_mode(binary, root, &client, 1);
    response = walk(&client, 1, 200, name, 1);
    expect_type("walk read-only file", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(&client, 200, P9_OREAD);
    expect_type("read-only open for read", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(&client, 200, 0, UINT32_MAX);
    expect_type("read-only read", &response, P9_RRead);
    assert(response.rread.count == strlen("unchanged"));
    assert(memcmp(response.rread.data, "unchanged",
                  response.rread.count) == 0);
    ixp_freefcall(&response);
    response = walk(&client, 1, 201, name, 1);
    expect_type("walk read-only write fid", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(&client, 201, P9_OWRITE);
    expect_error("read-only write open", &response, EROFS);
    ixp_freefcall(&response);
    response = open_fid(&client, 201, P9_OWRITE | P9_OTRUNC);
    expect_error("read-only truncate open", &response, EROFS);
    ixp_freefcall(&response);
    response = open_fid(&client, 201, P9_OREAD | P9_ORCLOSE);
    expect_error("read-only orclose open", &response, EROFS);
    ixp_freefcall(&response);

    response = walk(&client, 1, 202, NULL, 0);
    expect_type("clone read-only root", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = create_fid(&client, 202, "forbidden", 0600,
                          P9_OWRITE, NULL);
    expect_error("read-only create", &response, EROFS);
    ixp_freefcall(&response);

    response = walk(&client, 1, 203, name, 1);
    expect_type("walk read-only remove fid", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = remove_fid(&client, 203);
    expect_error("read-only remove", &response, EROFS);
    ixp_freefcall(&response);

    requested = unchanged_stat();
    response = wstat_fid(&client, 200, &requested);
    expect_type("read-only no-op wstat", &response, P9_RWStat);
    ixp_freefcall(&response);
    requested = unchanged_stat();
    requested.mode = 0600;
    response = wstat_fid(&client, 200, &requested);
    expect_error("read-only metadata change", &response, EROFS);
    ixp_freefcall(&response);
    stop_server(&client, child);

    make_path(path, sizeof(path), root, "read-only");
    assert(stat(path, &native) == 0);
    assert(native.st_size == (off_t)strlen("unchanged"));
    assert((native.st_mode & 0777) == 0644);
    make_path(path, sizeof(path), root, "forbidden");
    assert(access(path, F_OK) < 0 && errno == ENOENT);
}

static void test_synthetic_namespace(const char *binary,
                                     const char *first_root,
                                     const char *second_root,
                                     const char *roots_file) {
    const char *first[] = { "First" };
    const char *second_file[] = { "Second", "persistent" };
    const char *created[] = { "created" };
    const char *initial_names[] = { "Second", "First" };
    const char *initial_roots[] = { second_root, first_root };
    const char *changed_names[] = { "Third", "First" };
    const char *changed_roots[] = { second_root, first_root };
    const char *reordered_roots[] = { first_root, second_root };
    const char *first_page[] = { "First" };
    const char *initial_listing[] = { "First", "Second" };
    const char *changed_listing[] = { "First", "Third" };
    IxpFcall response;
    IxpStat stat;
    IxpQid before;
    IxpQid after;
    Client client;
    pid_t child;
    size_t first_length;
    char path[1024];

    write_roots(roots_file, NULL, NULL, 0);
    assert(setenv("NINED_TEST_ROOTS", roots_file, 1) == 0);
    child = start_server(binary, NULL, &client);

    attach(&client, 299);
    response = open_fid(&client, 299, P9_OREAD);
    expect_type("open synthetic root", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = read_fid(&client, 299, 0, 8192);
    directory_names(&client, &response, NULL, 0);
    ixp_freefcall(&response);

    before = stat_qid(&client, 299);
    write_roots(roots_file, initial_names, initial_roots, 2);
    response = read_fid(&client, 299, 0, 8192);
    first_length = directory_names(&client, &response, initial_listing, 2);
    ixp_freefcall(&response);
    after = stat_qid(&client, 299);
    assert(after.version != before.version);

    response = read_fid(&client, 299, 0, (uint32_t)first_length);
    directory_names(&client, &response, first_page, 1);
    ixp_freefcall(&response);

    response = walk(&client, 1, 305, second_file, 2);
    expect_type("walk file before volume removal", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(&client, 305, P9_OREAD);
    expect_type("open file before volume removal", &response, P9_ROpen);
    ixp_freefcall(&response);

    response = walk(&client, 1, 306, NULL, 0);
    expect_type("clone root for refresh", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(&client, 306, P9_OREAD);
    expect_type("open second synthetic root fid", &response, P9_ROpen);
    ixp_freefcall(&response);
    write_roots(roots_file, changed_names, changed_roots, 2);
    response = read_fid(&client, 306, 0, 8192);
    directory_names(&client, &response, changed_listing, 2);
    ixp_freefcall(&response);

    response = read_fid(&client, 299, first_length, 8192);
    directory_names(&client, &response, &initial_listing[1], 1);
    ixp_freefcall(&response);
    response = read_fid(&client, 305, 0, 32);
    expect_type("open file survives volume removal", &response, P9_RRead);
    assert(response.rread.count == strlen("persistent"));
    assert(memcmp(response.rread.data, "persistent",
                  response.rread.count) == 0);
    ixp_freefcall(&response);
    response = walk(&client, 1, 307, second_file, 1);
    expect_error("removed volume rejects new walk", &response, ENOENT);
    ixp_freefcall(&response);

    before = stat_qid(&client, 299);
    write_roots(roots_file, changed_listing, reordered_roots, 2);
    response = read_fid(&client, 306, 0, 8192);
    directory_names(&client, &response, changed_listing, 2);
    ixp_freefcall(&response);
    after = stat_qid(&client, 299);
    assert(after.version == before.version);

    assert(unlink(roots_file) == 0);
    response = read_fid(&client, 306, 0, 8192);
    expect_error("failed discovery preserves namespace", &response, ENOENT);
    ixp_freefcall(&response);
    response = walk(&client, 1, 308, first, 1);
    expect_type("walk preserved root after discovery failure", &response,
                P9_RWalk);
    ixp_freefcall(&response);

    stat = unchanged_stat();
    response = wstat_fid(&client, 1, &stat);
    expect_type("synthetic root no-op wstat", &response, P9_RWStat);
    ixp_freefcall(&response);

    response = walk(&client, 1, 300, first, 1);
    expect_type("walk synthetic export root", &response, P9_RWalk);
    ixp_freefcall(&response);
    stat = unchanged_stat();
    response = wstat_fid(&client, 300, &stat);
    expect_type("export root no-op wstat", &response, P9_RWStat);
    ixp_freefcall(&response);

    response = walk(&client, 300, 301, NULL, 0);
    expect_type("clone export root", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = create_fid(&client, 301, "created", 0600,
                          P9_ORDWR, NULL);
    expect_type("create inside export root", &response, P9_RCreate);
    ixp_freefcall(&response);
    response = write_fid(&client, 301, 0, "synthetic", 9);
    expect_type("write inside export root", &response, P9_RWrite);
    ixp_freefcall(&response);

    response = clunk(&client, 301);
    expect_type("clunk created file", &response, P9_RClunk);
    ixp_freefcall(&response);
    response = walk(&client, 300, 304, created, 1);
    expect_type("walk synthetic orclose file", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = open_fid(&client, 304, P9_OREAD | P9_ORCLOSE);
    expect_type("open synthetic orclose file", &response, P9_ROpen);
    ixp_freefcall(&response);
    response = clunk(&client, 304);
    expect_type("close before synthetic orclose remove", &response,
                P9_RClunk);
    ixp_freefcall(&response);

    stat = unchanged_stat();
    stat.name = "Renamed";
    response = wstat_fid(&client, 300, &stat);
    expect_error("protect export root rename", &response, EPERM);
    ixp_freefcall(&response);

    response = walk(&client, 1, 302, first, 1);
    expect_type("walk protected export root", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = remove_fid(&client, 302);
    expect_error("protect export root removal", &response, EPERM);
    ixp_freefcall(&response);

    response = walk(&client, 1, 303, NULL, 0);
    expect_type("clone synthetic root", &response, P9_RWalk);
    ixp_freefcall(&response);
    response = create_fid(&client, 303, "Third", 0600, P9_ORDWR, NULL);
    expect_error("protect synthetic root listing", &response, EPERM);
    ixp_freefcall(&response);
    stop_server(&client, child);
    unsetenv("NINED_TEST_ROOTS");

    make_path(path, sizeof(path), first_root, "created");
    assert(access(path, F_OK) < 0 && errno == ENOENT);
}

int main(int argc, char **argv) {
    char template[] = "/tmp/9d-protocol-XXXXXX";
    char outside_template[] = "/tmp/9d-outside-XXXXXX";
    char first_template[] = "/tmp/9d-first-XXXXXX";
    char second_template[] = "/tmp/9d-second-XXXXXX";
    char roots_template[] = "/tmp/9d-roots-XXXXXX";
    char path[1024];
    char second[1024];
    char *root;
    char *outside;
    char *first_root;
    char *second_root;
    int roots_fd;
    Client client;
    pid_t child;
    unsigned int index;

    assert(argc == 2 || argc == 3);
    root = mkdtemp(template);
    assert(root);
    outside = mkdtemp(outside_template);
    assert(outside);
    first_root = mkdtemp(first_template);
    assert(first_root);
    second_root = mkdtemp(second_template);
    assert(second_root);
    roots_fd = mkstemp(roots_template);
    assert(roots_fd >= 0);
    assert(close(roots_fd) == 0);
    make_path(path, sizeof(path), root, "dir");
    assert(mkdir(path, 0700) == 0);
    make_path(path, sizeof(path), root, "dir/file");
    write_file(path, "original");
    make_path(path, sizeof(path), root, "truncate");
    write_file(path, "abcdefgh");
    make_path(path, sizeof(path), root, "link");
    assert(symlink("target/path", path) == 0);
    make_path(path, sizeof(path), root, "large");
    write_pattern_file(path, IXP_MAX_MSG * 2U);
    make_path(path, sizeof(path), root, "entries");
    assert(mkdir(path, 0700) == 0);
    for(index = 0; index < 32; index++) {
        assert(snprintf(second, sizeof(second), "%s/item-%u", path, index) <
               (int)sizeof(second));
        write_file(second, "x");
    }
    make_path(path, sizeof(path), root, "hard-a");
    write_file(path, "hard");
    make_path(second, sizeof(second), root, "hard-b");
    assert(link(path, second) == 0);
    make_path(path, sizeof(path), root, "temporary");
    write_file(path, "temporary");
    make_path(path, sizeof(path), root, "executable");
    write_file(path, "executable");
    assert(chmod(path, 0700) == 0);
    make_path(path, sizeof(path), root, "plain");
    write_file(path, "plain");
    make_path(path, sizeof(path), root, "orclose-empty");
    assert(mkdir(path, 0700) == 0);
    make_path(path, sizeof(path), root, "orclose-full");
    assert(mkdir(path, 0700) == 0);
    make_path(path, sizeof(path), root, "orclose-full/child");
    write_file(path, "child");
    make_path(path, sizeof(path), root, "tree");
    assert(mkdir(path, 0700) == 0);
    make_path(path, sizeof(path), root, "tree/child");
    assert(mkdir(path, 0700) == 0);
    make_path(path, sizeof(path), root, "tree/child/file");
    write_file(path, "tree");
    make_path(path, sizeof(path), root, "metadata");
    write_file(path, "metadata");
    make_path(path, sizeof(path), root, "partial-wstat");
    write_file(path, "partial-wstat");
    make_path(path, sizeof(path), root, "read-only");
    write_file(path, "unchanged");
    assert(chmod(path, 0644) == 0);
    make_path(path, sizeof(path), outside, "secret");
    write_file(path, "outside");
    make_path(path, sizeof(path), second_root, "persistent");
    write_file(path, "persistent");
    make_path(path, sizeof(path), root, "escape");
    assert(symlink(outside, path) == 0);
    make_path(path, sizeof(path), root, "fifo");
    assert(mkfifo(path, 0600) == 0);
    make_path(path, sizeof(path), root, "sock");
    {
        struct sockaddr_un address;
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        assert(sock >= 0);
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        assert(strlen(path) < sizeof(address.sun_path));
        strcpy(address.sun_path, path);
        assert(bind(sock, (struct sockaddr *)&address, sizeof(address)) == 0);
        assert(close(sock) == 0);
        assert(chmod(path, 0700) == 0);
    }
    make_path(path, sizeof(path), root, "setuid");
    write_file(path, "setuid");
    assert(chmod(path, S_ISUID | S_ISGID | 0755) == 0);
    make_path(path, sizeof(path), root, "chardev");
    /* Only root can make device nodes; the test skips the check otherwise. */
    (void)mknod(path, S_IFCHR | 0600, makedev(1, 3));

    test_invalid_negotiation(argv[1], root);
    test_response_pack_failure(argv[1], root);
    child = start_server(argv[1], root, &client);
    test_libixp_fid_cleanup(&client);
    test_fid_limit(&client);
    test_walks(&client);
    test_rename_and_open_identity(&client, root);
    test_truncate_and_symlink(&client, root);
    test_directory_offsets(&client);
    test_bounded_reads(&client);
    test_hardlinks_and_versions(&client);
    test_descendant_rename(&client);
    test_wstat_validation(&client, root);
    test_create_validation(&client, root);
    test_containment(&client, outside);
    test_orclose_and_exec(&client, root);
    test_qid_mutations(&client);
    test_partial_wstat_qid(&client, root);
    test_special_files(&client, root);
    stop_server(&client, child);
    test_read_only(argv[1], root);
    if(argc == 3)
        test_synthetic_namespace(argv[2], first_root, second_root,
                                 roots_template);

    make_path(path, sizeof(path), root, "entries");
    for(index = 0; index < 32; index++) {
        assert(snprintf(second, sizeof(second), "%s/item-%u", path, index) <
               (int)sizeof(second));
        assert(unlink(second) == 0);
    }
    assert(rmdir(path) == 0);
    make_path(path, sizeof(path), root, "truncate"); unlink(path);
    make_path(path, sizeof(path), root, "link"); unlink(path);
    make_path(path, sizeof(path), root, "large"); unlink(path);
    make_path(path, sizeof(path), root, "hard-renamed"); unlink(path);
    make_path(path, sizeof(path), root, "hard-b"); unlink(path);
    make_path(path, sizeof(path), root, "executable"); unlink(path);
    make_path(path, sizeof(path), root, "plain"); unlink(path);
    make_path(path, sizeof(path), root, "orclose-full/child"); unlink(path);
    make_path(path, sizeof(path), root, "orclose-full"); rmdir(path);
    make_path(path, sizeof(path), root, "moved/child/file"); unlink(path);
    make_path(path, sizeof(path), root, "moved/child"); rmdir(path);
    make_path(path, sizeof(path), root, "moved"); rmdir(path);
    make_path(path, sizeof(path), root, "metadata"); unlink(path);
    make_path(path, sizeof(path), root, "read-only"); unlink(path);
    make_path(path, sizeof(path), root, "escape"); unlink(path);
    make_path(path, sizeof(path), root, "fifo"); unlink(path);
    make_path(path, sizeof(path), root, "sock"); unlink(path);
    make_path(path, sizeof(path), root, "setuid"); unlink(path);
    make_path(path, sizeof(path), root, "chardev"); unlink(path);
    make_path(path, sizeof(path), root, "suid-created"); unlink(path);
    make_path(path, sizeof(path), root, "dir"); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
    make_path(path, sizeof(path), outside, "secret"); unlink(path);
    assert(rmdir(outside) == 0);
    assert(rmdir(first_root) == 0);
    make_path(path, sizeof(path), second_root, "persistent"); unlink(path);
    assert(rmdir(second_root) == 0);
    puts("protocol tests passed");
    return 0;
}
