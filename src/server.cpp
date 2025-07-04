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
    Buffer incoming;
    Buffer outgoing;
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
    buf_init(&conn->incoming, 1024);
    buf_init(&conn->outgoing, 1024);
    return conn;
}

bool try_one_request(Conn *conn) {
    if (buf_size(&conn->incoming) < 4) return false;

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data_begin, 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    if (4 + len > buf_size(&conn->incoming)) return false;

    const uint8_t *request = conn->incoming .data_begin + 4;
    printf("client says: len:%d data:%.*s\n", len, len < 100 ? len : 100, request);

    buf_append(&conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(&conn->outgoing, request, len);
    buf_consume(&conn->incoming, 4 + len);
    return true;
}

void handle_write(Conn *conn) {
    assert(buf_size(&conn->outgoing) !=0 );
    int rv = send(conn->fd, (const char *)conn->outgoing.data_begin, (int)buf_size(&conn->outgoing), 0);
    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return;
        msg_errno("send() error");
        conn->want_close = true;
        return;
    }

   buf_consume(&conn->outgoing, rv);

    if (buf_size(&conn->outgoing)==0) {
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

    if (rv == 0) {
        msg("client closed");
        conn->want_close = true;
        return;
    }

    buf_append(&conn->incoming, buf, rv);

    while (try_one_request(conn)) {}

    if (buf_size(&conn->outgoing)==0) {
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
