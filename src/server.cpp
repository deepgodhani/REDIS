#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <assert.h>

#include "buffer.h"
#include <string>
#include <map>


#pragma comment(lib, "ws2_32.lib")

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



struct Conn {
    SOCKET fd = INVALID_SOCKET;
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    std::vector<uint8_t> incoming;  
    std::vector<uint8_t> outgoing;
};

void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

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
    RES_OK = 0,
    RES_ERR = 1,    // error
    RES_NX = 2,     // key not found
};

// +--------+---------+
// | status | data... |
// +--------+---------+
struct Response {
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

static std::map<std::string, std::string> g_data;

static void do_request(std::vector<std::string> &cmd, Response &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        printf("Received GET command: key=%s\n", cmd[1].c_str());
        auto it = g_data.find(cmd[1]);
        if (it == g_data.end()) {
            printf("Key not found: %s\n", cmd[1].c_str());
            out.status = RES_NX;    // not found
            return;
        }
        const std::string &val = it->second;
        out.data.assign(val.begin(), val.end());
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        printf("Received SET command: key=%s, value=%s\n", cmd[1].c_str(), cmd[2].c_str());
        g_data[cmd[1]] = cmd[2]; 
        out.status = RES_OK; 
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        printf("Received DEL command: key=%s\n", cmd[1].c_str());
        g_data.erase(cmd[1]);
        out.status = RES_OK; 
    } else {
        printf("Unrecognized command: %s\n", cmd[0].c_str());
        out.status = RES_ERR;       // unrecognized command
    }
}

static void make_response(const Response &resp, std::vector<uint8_t> &out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(out, (const uint8_t *)&resp_len, 4);
    buf_append(out, (const uint8_t *)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
}

bool try_one_request(Conn *conn) {
    if (conn->incoming.size() < 4) {
        return false;   // want read
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    if (4 + len > conn->incoming.size()) {
        return false;   // want read
    }

    const uint8_t *request = &conn->incoming[4];
    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;   // want close
    }
    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

     // application logic done! remove the request message.

       buf_consume(conn->incoming, 4 + len);

    return true;
}

void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    int rv = send(conn->fd, (const char *)&conn->outgoing[0], conn->outgoing.size(), 0);
    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;
        msg_errno("send() error");
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, (size_t)rv);


    if (conn->outgoing.size() == 0) {
        conn->want_write = false;
        conn->want_read = true;
    }
}

void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024];
    int rv = recv(conn->fd, (char *)buf, sizeof(buf), 0);
    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;
        msg_errno("recv() error");
        conn->want_close = true;
        return;
    }
    printf("recv() returned %d bytes\n", rv);
    if (rv == 0) {
        msg("client closed");
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while (try_one_request(conn)) {}

    if (conn->outgoing.size() > 0) {
        conn->want_read = false;
        conn->want_write = true;
        handle_write(conn);
    }
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
