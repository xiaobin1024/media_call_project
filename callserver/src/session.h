#ifndef SESSION_H
#define SESSION_H

#include "socket_util.h"
#include <string>
#include <queue>
#include <mutex>

class SignalingServer;

class Session
{
public:
    Session(socket_t fd, SignalingServer& server,
            const std::string& peerIp, int peerPort);
    ~Session();

    socket_t fd() const { return fd_; }
    std::string username() const { return username_; }
    std::string peerIp() const { return peerIp_; }

    void feed(const char* data, int len);
    void send(const std::string& msg);
    bool flushWrite();

private:
    void handleMessage(const std::string& line);

    socket_t fd_;
    SignalingServer& server_;
    std::string readBuf_;
    std::queue<std::string> writeQueue_;
    std::mutex writeMutex_;
    std::string username_;
    std::string peerIp_;
    int peerPort_;
};

#endif
