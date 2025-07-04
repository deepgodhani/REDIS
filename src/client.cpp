#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <vector>
#include <assert.h>

#pragma comment(lib, "ws2_32.lib")

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg) {
    fprintf(stderr, "[WSA error %d] %s\n", WSAGetLastError(), msg);
    exit(1);
}

static int32_t read_full(SOCKET fd, uint8_t *buf, size_t n) {
    while (n > 0) {
        int rv = recv(fd, (char *)buf, (int)n, 0);
        if (rv <= 0) {
            return -1;  // error or EOF
        }
        assert((size_t)rv <= n);
        n -= rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(SOCKET fd, const char *buf, size_t n) {
    while (n > 0) {
        int rv = send(fd, buf, n, 0);
        
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= rv;
        buf += rv;
    }
    return 0;
}

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

const size_t k_max_msg = 4096;


static int32_t send_req(SOCKET fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + s.size();
    }
    if (len > k_max_msg) {
        return -1;
    }
    char wbuf[4 + k_max_msg];

    memcpy(&wbuf[0], &len, 4);  // assume little endian
    uint32_t n = cmd.size();
    memcpy(&wbuf[4], &n, 4);
    size_t cur = 8;

    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }

    return write_all(fd, wbuf, 4 + len);
}

static int32_t read_res(SOCKET fd) {
    std::vector<uint8_t> rbuf;
    rbuf.resize(4);
    int32_t err = read_full(fd, &rbuf[0], 4);
    if (err) {
        msg("read() error or EOF");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf.data(), 4);  // assume little-endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    rbuf.resize(4 + len);
    err = read_full(fd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    printf("len:%u data:%.*s\n", len, len < 100 ? len : 100, &rbuf[4]);
    return 0;
}

int main(int argc, char **argv) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        die("socket()");
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int rv = connect(fd, (sockaddr *)&addr, sizeof(addr));
    if (rv == SOCKET_ERROR) {
        die("connect()");
    }

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }
    int32_t err = send_req(fd, cmd);
    if (err) {
        goto L_DONE;
    }
    printf("Data sent: %zu bytes\n", 4 + cmd.size() * 4);

    err = read_res(fd);
    if (err) {
        printf("Failed to read response from server\n");
        goto L_DONE;
    }

    printf("Received response from server: success\n");

L_DONE:
    closesocket(fd);
    WSACleanup();
    return 0;
}
