#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <map>
#include <vector>

#include "hashtable.h"

#pragma comment(lib, "ws2_32.lib")

#define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))


void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

void msg_errno(const char *msg) {
    fprintf(stderr, "[WSA error %d] %s\n", WSAGetLastError(), msg);
}

void die(const char *msg) {
    fprintf(stderr, "[FATAL] %s (WSA error %d)\n", msg, WSAGetLastError());
    exit(1);
}

void fd_set_nb(SOCKET fd) {
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
}


const size_t k_max_msg = 32 << 20;

typedef std::vector<uint8_t> Buffer;


void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}


struct Conn {
    SOCKET fd = INVALID_SOCKET;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    Buffer incoming;  
    Buffer outgoing;
};


Conn* handle_accept(SOCKET listen_fd) {
    sockaddr_in client_addr = {};
    int addrlen = sizeof(client_addr);
    SOCKET connfd = accept(listen_fd, (sockaddr *)&client_addr, &addrlen);
    if (connfd == INVALID_SOCKET) {
        msg_errno("accept() error");
        return NULL;
    }

    uint32_t ip = client_addr.sin_addr.S_un.S_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
            ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
            ntohs(client_addr.sin_port));

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
   
    return conn;
}

const size_t k_max_args = 200 * 1000;

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool
read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    if (cur + n > end) {
        return false;
    }
    out.assign(cur, cur + n);
    cur += n;
    return true;
}


static int32_t  parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out) {
    const uint8_t *end = data + size;
    uint32_t nstr = 0;
    if (!read_u32(data, end, nstr)) {
        return -1;
    }
    if (nstr > k_max_args) {
        return -1;  // safety limit
    }

    while (out.size() < nstr) {
        uint32_t len = 0;
        if (!read_u32(data, end, len)) {
            return -1;
        }
        out.push_back(std::string());
        if (!read_str(data, end, len, out.back())) {
            return -1;
        }
    }
    if (data != end) {
        return -1;  // trailing garbage
    }
    return 0;
}

enum {
    ERR_UNKNOWN = 1,    // unknown command
    ERR_TOO_BIG = 2,    // response too big
};

// data types of serialized data
enum {
    TAG_NIL = 0,    // nil
    TAG_ERR = 1,    // error code + msg
    TAG_STR = 2,    // string
    TAG_INT = 3,    // int64
    TAG_DBL = 4,    // double
    TAG_ARR = 5,    // array
};

// help functions for the serialization
static void buf_append_u8(Buffer &buf, uint8_t data) {
    buf.push_back(data);
}
static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, (const uint8_t *)&data, 4);
}
static void buf_append_i64(Buffer &buf, int64_t data) {
    buf_append(buf, (const uint8_t *)&data, 8);
}
static void buf_append_dbl(Buffer &buf, double data) {
    buf_append(buf, (const uint8_t *)&data, 8);
}

// append serialized data types to the back
static void out_nil(Buffer &out) {
    buf_append_u8(out, TAG_NIL);
}
static void out_str(Buffer &out, const char *s, size_t size) {
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, (uint32_t)size);
    buf_append(out, (const uint8_t *)s, size);
}
static void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}
static void out_dbl(Buffer &out, double val) {
    buf_append_u8(out, TAG_DBL);
    buf_append_dbl(out, val);
}
static void out_err(Buffer &out, uint32_t code, const std::string &msg) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)msg.size());
    buf_append(out, (const uint8_t *)msg.data(), msg.size());
}
static void out_arr(Buffer &out, uint32_t n) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, n);
}


// global states
static struct {
    HMap db;    // top-level hashtable
} g_data;

// KV pair for the top-level hashtable
struct Entry {
    struct HNode node;  // hashtable node
    std::string key;
    std::string val;
};

// equality comparison for `struct Entry`
static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

// FNV hash
static uint64_t str_hash(const uint8_t *data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

static void do_get(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy `Entry` just for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable lookup
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }
    // copy the value
    const std::string &val = container_of(node, Entry, node)->val;
    return out_str(out, val.data(), val.size());
}

static void do_set(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy `Entry` just for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable lookup
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        // found, update the value
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        // not found, allocate & insert a new pair
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

static void do_del(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy `Entry` just for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable delete
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) { // deallocate the pair
        delete container_of(node, Entry, node);
    }
    return out_int(out, node ? 1 : 0);
}


static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *(Buffer *)arg;
    const std::string &key = container_of(node, Entry, node)->key;
    out_str(out, key.data(), key.size());
    return true;
}

static void do_keys(std::vector<std::string> &, Buffer &out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void *)&out);
}

static void do_request(std::vector<std::string> &cmd, Buffer &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else if (cmd.size() == 1 && cmd[0] == "keys") {
        return do_keys(cmd, out);
    } else {
        return out_err(out, ERR_UNKNOWN, "unknown command.");
    }
}

static void response_begin(Buffer &out, size_t *header) {
    *header = out.size();       // messege header position
    buf_append_u32(out, 0);     // reserve space
}
static size_t response_size(Buffer &out, size_t header) {
    return out.size() - header - 4;
}
static void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) {
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG, "response is too big.");
        msg_size = response_size(out, header);
    }
    // message header
    uint32_t len = (uint32_t)msg_size;
    memcpy(&out[header], &len, 4);
}


static bool try_one_request(Conn *conn) {
    // try to parse the protocol: message header
    if (conn->incoming.size() < 4) {
        return false;   // want read
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;   // want close
    }
    // message body
    if (4 + len > conn->incoming.size()) {
        return false;   // want read
    }
    const uint8_t *request = &conn->incoming[4];

    // got one request, do some application logic
    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;   // want close
    }
    size_t header_pos = 0;
    response_begin(conn->outgoing, &header_pos);
    do_request(cmd, conn->outgoing);
    response_end(conn->outgoing, header_pos);

    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
    // Q: Why not just empty the buffer? See the explanation of "pipelining".
    return true;        // success
}

static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = send(conn->fd, reinterpret_cast<const char*>(&conn->outgoing[0]), conn->outgoing.size(), 0);
    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;    // error handling
        return;
    }

    // remove written data from `outgoing`
    buf_consume(conn->outgoing, (size_t)rv);

    // update the readiness intention
    if (conn->outgoing.size() == 0) {   // all data written
        conn->want_read = true;
        conn->want_write = false;
    } // else: want write
}

static void handle_read(Conn *conn) {
    // read some data
    uint8_t buf[64 * 1024];
    ssize_t rv = recv(conn->fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready
    }
    // handle IO error
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return; // want close
    }
    // handle EOF
    if (rv == 0) {
        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return; // want close
    }
    // got some new data
    buf_append(conn->incoming, buf, (size_t)rv);

    // parse requests and generate responses
    while (try_one_request(conn)) {}
    // Q: Why calling this in a loop? See the explanation of "pipelining".

    // update the readiness intention
    if (conn->outgoing.size() > 0) {    // has a response
        conn->want_read = false;
        conn->want_write = true;
        // The socket is likely ready to write in a request-response protocol,
        // try to write it without waiting for the next iteration.
        return handle_write(conn);
    }   // else: want read
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCKET) {
        die("socket()");
    }

    int val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&val, sizeof(val));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("bind()");
    }

    fd_set_nb(listen_fd);

    if (listen(listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        die("listen()");
    }

    std::vector<Conn *> fd2conn;
    fd_set read_fds, write_fds;
    int max_fd;

    while (true) {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        FD_SET(listen_fd, &read_fds);
        max_fd = (int)listen_fd;

        for (Conn *conn : fd2conn) {
            if (!conn) continue;
            if (conn->want_read) FD_SET(conn->fd, &read_fds);
            if (conn->want_write) FD_SET(conn->fd, &write_fds);
            if ((int)conn->fd > max_fd) max_fd = (int)conn->fd;
        }

        int rv = select(max_fd + 1, &read_fds, &write_fds, NULL, NULL);
        if (rv == SOCKET_ERROR) {
            msg_errno("select() failed");
            break;
        }

        if (FD_ISSET(listen_fd, &read_fds)) {
            if (Conn *conn = handle_accept(listen_fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        for (Conn *conn : fd2conn) {
            if (!conn) continue;

            if (FD_ISSET(conn->fd, &read_fds) && conn->want_read) {
                handle_read(conn);
            }

            if (FD_ISSET(conn->fd, &write_fds) && conn->want_write) {
                handle_write(conn);
            }

            if (conn->want_close) {
                closesocket(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }
    }

    WSACleanup();
    return 0;
}
