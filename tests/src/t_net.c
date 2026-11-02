/* t_net: Winsock 2 (ws2_32.dll) on the loopback interface.
 *
 * WSAStartup, a TCP echo between a listener on 127.0.0.1:0 (port from
 * getsockname) and a client thread, getaddrinfo("localhost"),
 * inet_pton/inet_ntop, select with a timeout, non-blocking connect errors
 * (WSAGetLastError), UDP sendto/recvfrom, closesocket and WSACleanup.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define __USE_MINGW_ANSI_STDIO 0
#include <winsock2.h>
#include <ws2tcpip.h>
#include "lsiw_test.h"

static unsigned short g_port;

static DWORD WINAPI client_thread(LPVOID arg)
{
    (void)arg;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(g_port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) { closesocket(s); return 1; }
    const char msg[] = "ping over loopback";
    if (send(s, msg, sizeof msg, 0) != (int)sizeof msg) { closesocket(s); return 2; }
    char buf[64];
    int got = 0, n;
    while (got < (int)sizeof msg && (n = recv(s, buf + got, sizeof buf - got, 0)) > 0) got += n;
    shutdown(s, SD_BOTH);
    closesocket(s);
    return got == (int)sizeof msg && memcmp(buf, msg, sizeof msg) == 0 ? 0 : 3;
}

static void test_tcp(void)
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(ls != INVALID_SOCKET, "socket TCP");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    CHECK(bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0, "bind 127.0.0.1:0");
    CHECK(listen(ls, 4) == 0, "listen");
    int len = sizeof sa;
    CHECK(getsockname(ls, (struct sockaddr *)&sa, &len) == 0 && sa.sin_port != 0 && len == sizeof sa,
          "getsockname gives the port");
    g_port = ntohs(sa.sin_port);

    /* Nothing pending yet: select times out. */
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    struct timeval tv = { 0, 100000 };
    DWORD t0 = GetTickCount();
    int r = select(0, &rf, NULL, NULL, &tv);
    CHECKF(r == 0 && GetTickCount() - t0 >= 80, "select times out", "r=%d", r);

    HANDLE th = CreateThread(NULL, 0, client_thread, NULL, 0, NULL);
    FD_ZERO(&rf);
    FD_SET(ls, &rf);
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    r = select(0, &rf, NULL, NULL, &tv);
    CHECK(r == 1 && FD_ISSET(ls, &rf), "select reports the pending connection");
    struct sockaddr_in peer;
    int plen = sizeof peer;
    SOCKET c = accept(ls, (struct sockaddr *)&peer, &plen);
    CHECK(c != INVALID_SOCKET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), "accept");
    char buf[128];
    int got = 0, n;
    while (got < 19 && (n = recv(c, buf + got, sizeof buf - got, 0)) > 0) got += n;
    CHECKF(got == 19 && strcmp(buf, "ping over loopback") == 0, "recv the client's message", "got=%d", got);
    CHECK(send(c, buf, got, 0) == got, "send echo");
    n = recv(c, buf, sizeof buf, 0);
    CHECKF(n == 0, "recv returns 0 after the peer shut down", "n=%d", n);
    WaitForSingleObject(th, 10000);
    DWORD code = 99;
    GetExitCodeThread(th, &code);
    CHECKF(code == 0, "client thread got the echo", "code=%lu", (unsigned long)code);
    CloseHandle(th);
    CHECK(closesocket(c) == 0 && closesocket(ls) == 0, "closesocket");
    CHECK(closesocket(ls) == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK, "closesocket twice -> WSAENOTSOCK");

    /* Connecting to a closed port fails with WSAECONNREFUSED. */
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sa.sin_port = htons(g_port);
    r = connect(s, (struct sockaddr *)&sa, sizeof sa);
    CHECKF(r == SOCKET_ERROR && WSAGetLastError() == WSAECONNREFUSED, "connect to a closed port -> WSAECONNREFUSED",
           "r=%d err=%d", r, WSAGetLastError());
    closesocket(s);

    /* Non-blocking connect reports WSAEWOULDBLOCK, then completes. */
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sa.sin_port = 0;
    bind(ls, (struct sockaddr *)&sa, sizeof sa);
    listen(ls, 1);
    len = sizeof sa;
    getsockname(ls, (struct sockaddr *)&sa, &len);
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    CHECK(ioctlsocket(s, FIONBIO, &nb) == 0, "ioctlsocket FIONBIO");
    r = connect(s, (struct sockaddr *)&sa, sizeof sa);
    CHECKF(r == 0 || WSAGetLastError() == WSAEWOULDBLOCK, "non-blocking connect -> WSAEWOULDBLOCK",
           "err=%d", WSAGetLastError());
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    tv.tv_sec = 5;
    CHECK(select(0, NULL, &wf, NULL, &tv) == 1, "select reports the connection writable");
    closesocket(s);
    closesocket(ls);
}

static void test_names(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo("localhost", "80", &hints, &res);
    CHECKF(r == 0 && res, "getaddrinfo(\"localhost\")", "r=%d", r);
    if (r == 0 && res) {
        struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
        CHECK(res->ai_family == AF_INET && sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(sin->sin_port) == 80,
              "localhost is 127.0.0.1, port 80");
        freeaddrinfo(res);
    }
    res = NULL;
    hints.ai_flags = AI_NUMERICHOST;
    r = getaddrinfo("10.1.2.3", NULL, &hints, &res);
    CHECK(r == 0 && res && ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr == htonl(0x0a010203),
          "getaddrinfo numeric host");
    if (res) freeaddrinfo(res);
    r = getaddrinfo("lsiw-no-such-host.invalid", NULL, &hints, &res);
    CHECKF(r != 0, "getaddrinfo of an invalid name fails", "r=%d", r);

    struct in_addr a4;
    char s[64];
    CHECK(inet_pton(AF_INET, "192.168.1.200", &a4) == 1 && a4.s_addr == htonl(0xC0A801C8), "inet_pton IPv4");
    CHECK(inet_pton(AF_INET, "300.1.1.1", &a4) == 0, "inet_pton rejects a bad address");
    CHECK(inet_ntop(AF_INET, &a4, s, sizeof s) && strcmp(s, "192.168.1.200") == 0, "inet_ntop IPv4");
    struct in6_addr a6;
    CHECK(inet_pton(AF_INET6, "::1", &a6) == 1 && a6.s6_addr[15] == 1 && a6.s6_addr[0] == 0, "inet_pton IPv6");
    CHECK(inet_ntop(AF_INET6, &a6, s, sizeof s) && strcmp(s, "::1") == 0, "inet_ntop IPv6");
    CHECK(htons(0x1234) == 0x3412 && ntohl(htonl(0xdeadbeef)) == 0xdeadbeef, "byte order helpers");
    CHECK(inet_addr("127.0.0.1") == htonl(INADDR_LOOPBACK), "inet_addr");
    char host[256];
    CHECK(gethostname(host, sizeof host) == 0 && host[0], "gethostname");
}

static void test_udp(void)
{
    SOCKET a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET, "socket UDP");
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(b, (struct sockaddr *)&sa, sizeof sa) == 0, "bind UDP");
    int len = sizeof sa;
    getsockname(b, (struct sockaddr *)&sa, &len);
    const char msg[] = "datagram";
    CHECK(sendto(a, msg, sizeof msg, 0, (struct sockaddr *)&sa, sizeof sa) == (int)sizeof msg, "sendto");
    char buf[64];
    struct sockaddr_in from;
    int flen = sizeof from;
    DWORD tmo = 5000;
    setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    int n = recvfrom(b, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
    CHECKF(n == (int)sizeof msg && strcmp(buf, msg) == 0 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
           "recvfrom gets the datagram and the sender", "n=%d", n);
    struct sockaddr_in alocal;
    len = sizeof alocal;
    getsockname(a, (struct sockaddr *)&alocal, &len);
    CHECK(from.sin_port == alocal.sin_port && alocal.sin_port != 0, "sender port is the implicit bind");

    /* Receive timeout: WSAETIMEDOUT. */
    tmo = 100;
    setsockopt(b, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
    n = recvfrom(b, buf, sizeof buf, 0, NULL, NULL);
    CHECKF(n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT, "SO_RCVTIMEO -> WSAETIMEDOUT",
           "n=%d err=%d", n, WSAGetLastError());
    closesocket(a);
    closesocket(b);
}

int main(void)
{
    t_init("t_net");
    WSADATA wd;
    SOCKET early = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(early == INVALID_SOCKET && WSAGetLastError() == WSANOTINITIALISED, "socket before WSAStartup -> WSANOTINITIALISED");
    int r = WSAStartup(MAKEWORD(2, 2), &wd);
    CHECKF(r == 0 && wd.wVersion == MAKEWORD(2, 2), "WSAStartup 2.2", "r=%d", r);
    test_names();
    test_tcp();
    test_udp();
    CHECK(WSACleanup() == 0, "WSACleanup");
    return t_done();
}
