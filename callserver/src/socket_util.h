#ifndef SOCKET_UTIL_H
#define SOCKET_UTIL_H

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <windows.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    using ssize_t = int;
    static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
    static constexpr int SOCKET_ERR = SOCKET_ERROR;
    #define CLOSE_SOCKET(s) closesocket(s)
    #define GET_ERR() WSAGetLastError()
    #define WOULD_BLOCK WSAEWOULDBLOCK
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
    static constexpr int SOCKET_ERR = -1;
    #define CLOSE_SOCKET(s) ::close(s)
    #define GET_ERR() errno
    #define WOULD_BLOCK EWOULDBLOCK
#endif

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <map>
#include <memory>

class SocketUtil
{
public:
    static bool initialize()
    {
#ifdef _WIN32
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }

    static void cleanup()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    static bool setNonBlocking(socket_t fd, bool nonBlocking)
    {
#ifdef _WIN32
        u_long mode = nonBlocking ? 1 : 0;
        return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) return false;
        return fcntl(fd, F_SETFL, nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
    }

    static socket_t createTcpSocket()
    {
        return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    static bool bind(socket_t fd, const std::string& ip, int port)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        return ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    static bool listen(socket_t fd, int backlog = 128)
    {
        return ::listen(fd, backlog) == 0;
    }

    static socket_t accept(socket_t fd, std::string* clientIp = nullptr, int* clientPort = nullptr)
    {
        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        socket_t clientFd = ::accept(fd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientFd != INVALID_SOCK && clientIp)
        {
            char buf[64] = {};
            inet_ntop(AF_INET, &clientAddr.sin_addr, buf, sizeof(buf));
            *clientIp = buf;
            if (clientPort) *clientPort = ntohs(clientAddr.sin_port);
        }
        return clientFd;
    }

    static bool close(socket_t fd)
    {
        if (fd != INVALID_SOCK)
            CLOSE_SOCKET(fd);
        return true;
    }
};

#endif
