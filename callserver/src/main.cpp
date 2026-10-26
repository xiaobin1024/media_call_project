#include "signaling_server.h"
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#endif

static SignalingServer* g_server = nullptr;

#ifdef _WIN32
#include <windows.h>
BOOL WINAPI consoleHandler(DWORD dwType)
{
    if (dwType == CTRL_C_EVENT || dwType == CTRL_BREAK_EVENT)
    {
        std::cout << "[Server] Ctrl+C, shutting down..." << std::endl;
        if (g_server)
            g_server->running_ = false;
        return TRUE;
    }
    return FALSE;
}
// 出错时停住控制台，避免双击一闪而过
static void waitBeforeExit() { system("pause"); }
#else
extern "C" void handleSignal(int sig)
{
    std::cout << "[Server] signal " << sig << ", shutting down..." << std::endl;
    if (g_server)
        g_server->running_ = false;
}
static void waitBeforeExit()
{
    std::cout << "Press Enter to exit...";
    std::cin.get();
}
#endif

// 打印本机网卡地址，方便更换服务器/公网 IP 后告知客户端连接地址
static void printLocalAddresses(int port)
{
#ifdef _WIN32
    char hostname[256] = {0};
#else
    char hostname[256];
    hostname[0] = '\0';
#endif
    if (gethostname(hostname, sizeof(hostname)) != 0)
        return;

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0)
        return;

    std::cout << "[Server] 本机内网/网卡地址（仅供同局域网客户端参考）:" << std::endl;
    for (auto* p = res; p; p = p->ai_next)
    {
        auto* sa = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        std::cout << "  - " << inet_ntoa(sa->sin_addr) << ":" << port << std::endl;
    }
    freeaddrinfo(res);
}

int main(int argc, const char* argv[])
{
    // 默认监听所有网卡、端口 8889，双击即可启动（无需参数）
    std::string ip = "0.0.0.0";
    int port = 8889;

    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (a.rfind("--port=", 0) == 0)
            port = atoi(a.c_str() + 7);
        else if (a.rfind("--", 0) != 0)
            pos.push_back(a);
    }
    if (pos.size() >= 1)
    {
        if (pos[0].find('.') != std::string::npos) ip = pos[0];
        else port = atoi(pos[0].c_str());
    }
    if (pos.size() >= 2)
    {
        if (pos[1].find('.') != std::string::npos) ip = pos[1];
        else port = atoi(pos[1].c_str());
    }

    if (port <= 0 || port > 65535)
    {
        std::cerr << "[Server] 端口非法: " << port << std::endl;
        waitBeforeExit();
        return -1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "      Media Call Server" << std::endl;
    std::cout << "========================================" << std::endl;

    try
    {
        SignalingServer server(ip, port);
        g_server = &server;
        std::cout << "[Server] 启动中，监听 " << ip << ":" << port
                  << "（TCP 信令 + UDP 中继端口 " << (port + 1) << "）" << std::endl;
        printLocalAddresses(port);
        std::cout << "[Server] 外网客户端请连接本机的【公网 IP】（云控制台/路由器查看），不要使用上面的内网地址。" << std::endl;
        std::cout << "[Server] 按 Ctrl+C 停止。" << std::endl;
        if (!server.start())
        {
            std::cerr << "[Server] 启动失败，请检查端口是否被占用或权限是否足够。" << std::endl;
            waitBeforeExit();
            return -1;
        }
        g_server = nullptr;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Server] 致命错误: " << e.what() << std::endl;
        waitBeforeExit();
        return -1;
    }

    std::cout << "[Server] 已退出。" << std::endl;
    return 0;
}
