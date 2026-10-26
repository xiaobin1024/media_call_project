#include "signaling_server.h"
#include "session.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>

static std::string timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    std::ostringstream os;
    os << buf << "." << std::setfill('0') << std::setw(3) << ms.count();
    return os.str();
}

#define LOG() std::cout << "[" << timestamp() << "][Server] "

static constexpr auto RELAY_REPROBE_COOLDOWN = std::chrono::seconds(10);
static constexpr auto RELAY_REPROBE_STABLE = std::chrono::seconds(3);

static void enterRelay(RelayBinding& b,
                       std::chrono::steady_clock::time_point now)
{
    b.mode = PathMode::Relay;
    b.epoch++;
    b.relayStart = now;
    b.relayDirectPending = false;
    b.directRxA = false;
    b.directRxB = false;
    b.reportA = false;
    b.reportB = false;
}

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <cstring>
#endif

SignalingServer::SignalingServer(const std::string& ip, int port)
    : ip_(ip), port_(port)
{
    SocketUtil::initialize();
}

SignalingServer::~SignalingServer()
{
    stop();
    SocketUtil::cleanup();
}

void SignalingServer::stop()
{
    running_ = false;
}

bool SignalingServer::start()
{
    listenFd_ = SocketUtil::createTcpSocket();
    if (listenFd_ == INVALID_SOCK)
    {
        std::cerr << "[Server] Failed to create socket" << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    if (!SocketUtil::bind(listenFd_, ip_, port_))
    {
        std::cerr << "[Server] Failed to bind " << ip_ << ":" << port_ << std::endl;
        SocketUtil::close(listenFd_);
        return false;
    }

    if (!SocketUtil::listen(listenFd_))
    {
        LOG() << "Failed to listen" << std::endl;
        SocketUtil::close(listenFd_);
        return false;
    }

    SocketUtil::setNonBlocking(listenFd_, true);

    // Create UDP discovery port (TCP port + 1)
    int udpPort = port_ + 1;
    udpFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udpFd_ != INVALID_SOCK)
    {
        sockaddr_in udpAddr{};
        udpAddr.sin_family = AF_INET;
        udpAddr.sin_port = htons(static_cast<u_short>(udpPort));
        inet_pton(AF_INET, ip_.c_str(), &udpAddr.sin_addr);
        if (::bind(udpFd_, reinterpret_cast<sockaddr*>(&udpAddr), sizeof(udpAddr)) == 0)
            LOG() << "UDP discovery on " << ip_ << ":" << udpPort << std::endl;
        else
            udpFd_ = INVALID_SOCK;
    }

    LOG() << "listening on " << ip_ << ":" << port_ << std::endl;

    running_ = true;
    eventLoop();

    SocketUtil::close(listenFd_);
    if (udpFd_ != INVALID_SOCK) CLOSE_SOCKET(udpFd_);

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& [fd, session] : sessions_)
            SocketUtil::close(fd);
        sessions_.clear();
    }

    std::cout << "[Server] stopped" << std::endl;
    return true;
}

void SignalingServer::eventLoop()
{
    while (running_)
    {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(listenFd_, &readFds);
        int maxFd = static_cast<int>(listenFd_);

        if (udpFd_ != INVALID_SOCK)
        {
            FD_SET(udpFd_, &readFds);
            if (udpFd_ > maxFd) maxFd = static_cast<int>(udpFd_);
        }

        std::vector<std::pair<int, std::shared_ptr<Session>>> currentSessions;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [fd, s] : sessions_)
                currentSessions.emplace_back(fd, s);
        }

        for (auto& [fd, session] : currentSessions)
        {
            FD_SET(fd, &readFds);
            if (fd > maxFd) maxFd = fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        int ret = select(maxFd + 1, &readFds, nullptr, nullptr, &tv);
        if (!running_) break;
        if (ret < 0)
        {
            std::cerr << "[Server] select error: errno=" << GET_ERR() << std::endl;
            continue;
        }

        if (ret > 0 && FD_ISSET(listenFd_, &readFds))
            handleAccept();

        if (ret > 0 && udpFd_ != INVALID_SOCK && FD_ISSET(udpFd_, &readFds))
            handleUdpRead();

        for (auto& [fd, session] : currentSessions)
        {
            if (FD_ISSET(fd, &readFds))
                handleRead(fd, session.get());
        }

        pumpSessionWrites();
        tickPathTimers();
    }
}

void SignalingServer::pumpSessionWrites()
{
    std::vector<std::pair<int, std::shared_ptr<Session>>> allSessions;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& [fd, s] : sessions_)
            allSessions.emplace_back(fd, s);
    }
    for (auto& [fd, s] : allSessions)
    {
        if (!s->flushWrite())
        {
            std::cout << "[Server] broken pipe, removing session " << fd << std::endl;
            removeSession(fd);
        }
    }
}

void SignalingServer::handleUdpRead()
{
    sockaddr_in clientAddr{};
    socklen_t addrLen = sizeof(clientAddr);
    // 中继媒体包 = 8B 中继头 + 12B RTP头 + 至多 1200B 分片 ≈ 1220B，
    // 缓冲必须足够大，否则大包会被截断（旧 64B 导致媒体包被截成 43B）。
    char buf[4096];
    int n = recvfrom(udpFd_, buf, sizeof(buf) - 1, 0,
                     reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
    if (n <= 0) return;
    buf[n] = '\0';

    // Relay media: 8-byte header starting with RELAY_MAGIC
    if (n >= RELAY_HEADER_SIZE &&
        (static_cast<unsigned char>(buf[0]) == ((RELAY_MAGIC >> 8) & 0xff)) &&
        (static_cast<unsigned char>(buf[1]) == (RELAY_MAGIC & 0xff)))
    {
        uint32_t relayId = (static_cast<unsigned char>(buf[4]) << 24)
                         | (static_cast<unsigned char>(buf[5]) << 16)
                         | (static_cast<unsigned char>(buf[6]) << 8)
                         | static_cast<unsigned char>(buf[7]);
        handleRelayDatagram(clientAddr, buf + RELAY_HEADER_SIZE,
                            n - RELAY_HEADER_SIZE, relayId);
        return;
    }

    char ipBuf[64] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
    int port = ntohs(clientAddr.sin_port);
    LOG() << "UDP probe from " << ipBuf << ":" << port
          << " len=" << n << std::endl;

    std::string clientIp(ipBuf);
    std::string portMsg = "{\"type\":\"port_discovered\",\"port\":"
                        + std::to_string(port) + "}\n";

    // 带用户名的探测包（新格式）：0x01 + 用户名。
    // 早期客户端只发 1 字节探针，服务端只能按公网 IP 匹配会话——同一 NAT 后有多台
    // 客户端时（公网 IP 相同）会命中别人的会话，自己永远收不到 port_discovered，
    // NAT 穿透必然失败。新格式按用户名精确匹配，同时保留对老探针的兼容。
    if (n >= 2 && static_cast<unsigned char>(buf[0]) == 0x01)
    {
        std::string probeUser(buf + 1, static_cast<size_t>(n - 1));
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (auto& [fd, s] : sessions_)
            {
                if (s->username() == probeUser)
                {
                    LOG() << "  port_discovered -> " << probeUser
                          << " (" << ipBuf << ":" << port << ") [matched by username]"
                          << std::endl;
                    s->send(portMsg);
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            LOG() << "  no session named '" << probeUser << "' for tagged probe" << std::endl;
        return;
    }

    // 老格式（1 字节探针）：退化为按 IP 匹配，仅保证旧客户端仍可用。
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& [fd, s] : sessions_)
        {
            if (s->peerIp() == clientIp)
            {
                LOG() << "  port_discovered -> " << (s->username().empty() ? "?" : s->username())
                      << " (" << clientIp << ":" << port << ")" << std::endl;
                s->send(portMsg);
                found = true;
                break;
            }
        }
    }
    if (!found)
        LOG() << "  no matching session for probe" << std::endl;
}

void SignalingServer::handleRelayDatagram(const sockaddr_in& from,
                                          const char* data, int len,
                                          uint32_t relayId)
{
    sockaddr_in* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(bindingsMutex_);
        auto it = bindings_.find(relayId);
        if (it == bindings_.end())
        {
            LOG() << "relay: unknown relayId " << relayId << " (len=" << len << ")" << std::endl;
            return;
        }
        RelayBinding& b = it->second;

        bool fromA = b.learnedA && b.addrA.sin_addr.s_addr == from.sin_addr.s_addr
                                 && b.addrA.sin_port == from.sin_port;
        bool fromB = b.learnedB && b.addrB.sin_addr.s_addr == from.sin_addr.s_addr
                                 && b.addrB.sin_port == from.sin_port;

        if (!fromA && !fromB)
        {
            // Learn a new endpoint into the first free slot
            if (!b.learnedA)
            {
                b.addrA = from;
                b.learnedA = true;
                LOG() << "relay " << relayId << ": learned endpoint A" << std::endl;
                fromA = true;
            }
            else if (!b.learnedB)
            {
                b.addrB = from;
                b.learnedB = true;
                LOG() << "relay " << relayId << ": learned endpoint B" << std::endl;
                fromB = true;
            }
            else
            {
                LOG() << "relay " << relayId << ": packet from unknown endpoint, dropped" << std::endl;
                return;
            }
        }

        if (fromA && b.learnedB)
            target = &b.addrB;
        else if (fromB && b.learnedA)
            target = &b.addrA;
    }

    if (target)
        ::sendto(udpFd_, data, len, 0,
                 reinterpret_cast<sockaddr*>(target), sizeof(*target));
}

const char* SignalingServer::pathModeName(PathMode mode)
{
    switch (mode)
    {
    case PathMode::Probing: return "probing";
    case PathMode::P2pProbation: return "p2p_probation";
    case PathMode::P2p: return "p2p";
    case PathMode::Relay: return "relay";
    }
    return "relay";
}

void SignalingServer::sendPathDecision(const PathDecision& decision)
{
    std::string msg = "{\"type\":\"path_decision\",\"relayId\":"
                    + std::to_string(decision.relayId)
                    + ",\"mode\":\"" + pathModeName(decision.mode)
                    + "\",\"epoch\":" + std::to_string(decision.epoch)
                    + ",\"reason\":\"" + decision.reason + "\"}\n";

    LOG() << "path " << decision.relayId << ": mode="
          << pathModeName(decision.mode) << " epoch=" << decision.epoch
          << " reason=" << decision.reason << std::endl;

    if (!forwardTo(decision.userA, msg))
        LOG() << "path: decision target " << decision.userA << " offline" << std::endl;
    if (decision.userB != decision.userA && !forwardTo(decision.userB, msg))
        LOG() << "path: decision target " << decision.userB << " offline" << std::endl;
}

void SignalingServer::handlePathReport(const std::string& user, bool directRx,
                                       const std::string& reason)
{
    std::vector<PathDecision> decisions;

    {
        std::lock_guard<std::mutex> lock(bindingsMutex_);
        auto uit = userRelayId_.find(user);
        if (uit == userRelayId_.end())
        {
            LOG() << "path_report from " << user << " ignored: no binding" << std::endl;
            return;
        }

        auto bit = bindings_.find(uit->second);
        if (bit == bindings_.end()) return;

        RelayBinding& b = bit->second;
        auto now = std::chrono::steady_clock::now();

        if (user == b.userA)
        {
            b.directRxA = directRx;
            b.reportA = true;
            b.lastReportA = now;
        }
        else if (user == b.userB)
        {
            b.directRxB = directRx;
            b.reportB = true;
            b.lastReportB = now;
        }
        else
        {
            return;
        }

        LOG() << "path " << uit->second << ": report from " << user
              << " directRx=" << (directRx ? 1 : 0)
              << " reason=" << reason
              << " A=" << (b.directRxA ? 1 : 0)
              << " B=" << (b.directRxB ? 1 : 0) << std::endl;

        if (!directRx)
        {
            b.relayDirectPending = false;
            if (b.mode != PathMode::Relay)
            {
                enterRelay(b, now);
                decisions.push_back({b.userA, b.userB, uit->second,
                                     b.mode, b.epoch,
                                     reason.empty() ? "peer_fail" : reason});
            }
        }
        else if (b.mode == PathMode::Probing && b.directRxA && b.directRxB)
        {
            b.mode = PathMode::P2pProbation;
            b.epoch++;
            b.probationStart = now;
            decisions.push_back({b.userA, b.userB, uit->second,
                                 b.mode, b.epoch, "both_direct"});
        }
        else if (b.mode == PathMode::Relay &&
                 b.reportA && b.reportB &&
                 now - b.lastReportA <= std::chrono::seconds(2) &&
                 now - b.lastReportB <= std::chrono::seconds(2) &&
                 b.directRxA && b.directRxB &&
                 now - b.relayStart >= RELAY_REPROBE_COOLDOWN)
        {
            if (!b.relayDirectPending)
            {
                b.relayDirectPending = true;
                b.relayDirectSince = now;
                LOG() << "path " << uit->second
                      << ": relay direct probe stable window started" << std::endl;
            }
            else if (now - b.relayDirectSince >= RELAY_REPROBE_STABLE)
            {
                b.mode = PathMode::P2pProbation;
                b.epoch++;
                b.probationStart = now;
                b.relayDirectPending = false;
                decisions.push_back({b.userA, b.userB, uit->second,
                                     b.mode, b.epoch, "reprobe_ok"});
            }
        }
        else if (b.mode == PathMode::Relay)
        {
            b.relayDirectPending = false;
        }
    }

    for (const auto& d : decisions)
        sendPathDecision(d);
}

void SignalingServer::tickPathTimers()
{
    std::vector<PathDecision> decisions;
    auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(bindingsMutex_);
        for (auto& [relayId, b] : bindings_)
        {
            if (b.mode == PathMode::Probing)
            {
                if (now - b.createdAt >= std::chrono::seconds(4))
                {
                    enterRelay(b, now);
                    decisions.push_back({b.userA, b.userB, relayId,
                                         b.mode, b.epoch, "direct_timeout"});
                }
            }
            else if (b.mode == PathMode::P2pProbation)
            {
                bool staleA = b.reportA &&
                              now - b.lastReportA > std::chrono::seconds(3);
                bool staleB = b.reportB &&
                              now - b.lastReportB > std::chrono::seconds(3);

                if (staleA || staleB)
                {
                    enterRelay(b, now);
                    decisions.push_back({b.userA, b.userB, relayId,
                                         b.mode, b.epoch, "report_timeout"});
                }
                else if (b.lastReportA > b.probationStart &&
                         b.lastReportB > b.probationStart &&
                         b.directRxA && b.directRxB &&
                         now - b.probationStart >= std::chrono::seconds(3))
                {
                    b.mode = PathMode::P2p;
                    b.epoch++;
                    decisions.push_back({b.userA, b.userB, relayId,
                                         b.mode, b.epoch, "probation_ok"});
                }
            }
            else if (b.mode == PathMode::P2p)
            {
                bool staleA = b.reportA &&
                              now - b.lastReportA > std::chrono::seconds(3);
                bool staleB = b.reportB &&
                              now - b.lastReportB > std::chrono::seconds(3);

                if (staleA || staleB)
                {
                    enterRelay(b, now);
                    decisions.push_back({b.userA, b.userB, relayId,
                                         b.mode, b.epoch, "report_timeout"});
                }
            }
        }
    }

    for (const auto& d : decisions)
        sendPathDecision(d);
}

void SignalingServer::sendRelayInfo(const std::string& user, uint32_t relayId)
{
    std::string msg = "{\"type\":\"relay_info\",\"relayId\":" + std::to_string(relayId) + "}\n";
    forwardTo(user, msg);
}

uint32_t SignalingServer::registerBinding(const std::string& userA,
                                          const std::string& userB)
{
    std::lock_guard<std::mutex> lock(bindingsMutex_);
    uint32_t relayId = nextRelayId_++;
    RelayBinding& b = bindings_[relayId];
    b = RelayBinding{};
    b.userA = userA;
    b.userB = userB;
    b.createdAt = std::chrono::steady_clock::now();
    b.mode = PathMode::Probing;
    userRelayId_[userA] = relayId;
    userRelayId_[userB] = relayId;
    LOG() << "relay: registered binding " << relayId << " for "
          << userA << " <-> " << userB << std::endl;
    return relayId;
}

void SignalingServer::removeUserBindings(const std::string& username)
{
    std::lock_guard<std::mutex> lock(bindingsMutex_);
    auto it = userRelayId_.find(username);
    if (it == userRelayId_.end()) return;
    uint32_t relayId = it->second;
    userRelayId_.erase(it);

    // erase every other user mapping pointing to the same binding
    for (auto m = userRelayId_.begin(); m != userRelayId_.end();)
    {
        if (m->second == relayId)
            m = userRelayId_.erase(m);
        else
            ++m;
    }
    bindings_.erase(relayId);
    LOG() << "relay: removed binding " << relayId << " (user " << username << ")" << std::endl;
}

void SignalingServer::handleAccept()
{
    std::string clientIp;
    int clientPort;
    socket_t clientFd = SocketUtil::accept(listenFd_, &clientIp, &clientPort);
    if (clientFd == INVALID_SOCK)
    {
        std::cerr << "[Server] accept failed: errno=" << GET_ERR() << std::endl;
        return;
    }

    SocketUtil::setNonBlocking(clientFd, true);

    auto session = std::make_shared<Session>(clientFd, *this, clientIp, clientPort);
    int total;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[clientFd] = session;
        total = static_cast<int>(sessions_.size());
    }

    LOG() << "new connection from " << clientIp << ":" << clientPort
          << ", total=" << total << std::endl;
}

void SignalingServer::handleRead(int fd, Session* session)
{
    char buf[4096];
    int n = recv(fd, buf, sizeof(buf) - 1, 0);

    if (n <= 0)
    {
        auto name = session->username();
        LOG() << (name.empty() ? "anonymous" : name) << " (" << session->peerIp()
              << ") disconnected" << std::endl;
        removeSession(fd);
        return;
    }

    buf[n] = '\0';
    session->feed(buf, n);
}

void SignalingServer::broadcastUserList()
{
    std::vector<std::shared_ptr<Session>> targets;
    std::string msg;

    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        msg = "{\"type\":\"user_list\",\"users\":[";
        bool first = true;
        for (auto& [fd, s] : sessions_)
        {
            auto name = s->username();
            if (name.empty()) continue;
            if (!first) msg += ",";
            msg += "\"" + name + "\"";
            first = false;
            targets.push_back(s);
        }
        msg += "]}\n";
    }

    for (auto& s : targets)
        s->send(msg);
}

bool SignalingServer::forwardTo(const std::string& target, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& [fd, s] : sessions_)
    {
        if (s->username() == target)
        {
            s->send(msg);
            return true;
        }
    }
    return false;
}

std::string SignalingServer::getSessionIp(const std::string& username)
{
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& [fd, s] : sessions_)
    {
        if (s->username() == username)
            return s->peerIp();
    }
    return {};
}

int SignalingServer::removeOtherSessionsByUsername(const std::string& username,
                                                   int selfFd)
{
    if (username.empty()) return 0;

    std::vector<int> victims;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        for (auto& [fd, s] : sessions_)
        {
            if (fd != selfFd && s->username() == username)
                victims.push_back(fd);
        }
    }

    for (int fd : victims)
    {
        LOG() << "duplicate login: kicking previous session of "
              << username << " (fd=" << fd << ")" << std::endl;
        removeSession(fd);
    }
    return static_cast<int>(victims.size());
}

void SignalingServer::removeSession(int fd)
{
    std::string name;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        name = it->second->username();
        sessions_.erase(it);
    }

    SocketUtil::close(fd);

    if (!name.empty())
    {
        removeUserBindings(name);
        broadcastUserList();
    }
}
