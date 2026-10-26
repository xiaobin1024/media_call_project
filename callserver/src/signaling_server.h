#ifndef SIGNALING_SERVER_H
#define SIGNALING_SERVER_H

#include "socket_util.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

class Session;

// UDP relay magic: 0x5A4C. Relay datagram layout:
//   magic(2 bytes) | version(1) | reserved(1) | relayId(4, big-endian) | RTP payload
static constexpr uint16_t RELAY_MAGIC = 0x5A4C;
static constexpr int RELAY_HEADER_SIZE = 8;

enum class PathMode
{
    Probing,
    P2pProbation,
    P2p,
    Relay
};

struct RelayBinding
{
    sockaddr_in addrA{};
    sockaddr_in addrB{};
    bool learnedA = false;
    bool learnedB = false;

    std::string userA;
    std::string userB;

    PathMode mode = PathMode::Probing;
    uint64_t epoch = 0;
    bool directRxA = false;
    bool directRxB = false;
    bool reportA = false;
    bool reportB = false;

    std::chrono::steady_clock::time_point createdAt{};
    std::chrono::steady_clock::time_point probationStart{};
    std::chrono::steady_clock::time_point lastReportA{};
    std::chrono::steady_clock::time_point lastReportB{};
    std::chrono::steady_clock::time_point relayStart{};
    std::chrono::steady_clock::time_point relayDirectSince{};
    bool relayDirectPending = false;
};

class SignalingServer
{
public:
    SignalingServer(const std::string& ip, int port);
    ~SignalingServer();

    bool start();
    void stop();

    void broadcastUserList();
    void removeSession(int fd);
    bool forwardTo(const std::string& target, const std::string& msg);
    std::string getSessionIp(const std::string& username);

    void sendRelayInfo(const std::string& user, uint32_t relayId);
    uint32_t registerBinding(const std::string& userA, const std::string& userB);
    void removeUserBindings(const std::string& username);
    void handlePathReport(const std::string& user, bool directRx,
                          const std::string& reason);

    std::atomic<bool> running_{false};

private:
    struct PathDecision
    {
        std::string userA;
        std::string userB;
        uint32_t relayId = 0;
        PathMode mode = PathMode::Relay;
        uint64_t epoch = 0;
        std::string reason;
    };

    void eventLoop();
    void handleAccept();
    void handleUdpRead();
    void handleRead(int fd, Session* session);
    void pumpSessionWrites();
    void handleRelayDatagram(const sockaddr_in& from, const char* data,
                             int len, uint32_t relayId);
    void tickPathTimers();
    void sendPathDecision(const PathDecision& decision);
    static const char* pathModeName(PathMode mode);

    socket_t listenFd_ = INVALID_SOCK;
    socket_t udpFd_ = INVALID_SOCK;
    int port_;
    std::string ip_;

    std::map<int, std::shared_ptr<Session>> sessions_;
    std::mutex sessionsMutex_;

    uint32_t nextRelayId_ = 1;
    std::map<uint32_t, RelayBinding> bindings_;
    std::map<std::string, uint32_t> userRelayId_;
    std::mutex bindingsMutex_;
};

#endif
