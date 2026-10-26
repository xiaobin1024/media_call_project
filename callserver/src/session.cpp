#include "session.h"
#include "signaling_server.h"
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

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

static std::string findField(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

Session::Session(socket_t fd, SignalingServer& server,
                 const std::string& peerIp, int peerPort)
    : fd_(fd), server_(server), peerIp_(peerIp), peerPort_(peerPort)
{
}

Session::~Session()
{
}

void Session::feed(const char* data, int len)
{
    readBuf_.append(data, len);

    while (true)
    {
        auto pos = readBuf_.find('\n');
        if (pos == std::string::npos) break;

        std::string line = readBuf_.substr(0, pos);
        readBuf_.erase(0, pos + 1);

        if (!line.empty())
            handleMessage(line);
    }
}

void Session::send(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    writeQueue_.push(msg);
}

bool Session::flushWrite()
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    while (!writeQueue_.empty())
    {
        const auto& msg = writeQueue_.front();
        int n = ::send(fd_, msg.data(), static_cast<int>(msg.size()), 0);
        if (n < 0)
        {
            int err = GET_ERR();
            if (err == WOULD_BLOCK) return true;
            // connection broken, drain queue and signal removal
            while (!writeQueue_.empty())
                writeQueue_.pop();
            return false;
        }
        writeQueue_.pop();
    }
    return true;
}

void Session::handleMessage(const std::string& line)
{
    auto type = findField(line, "type");

    if (type == "login")
    {
        auto name = findField(line, "username");

        if (name.empty())
        {
            LOG() << "login rejected: empty username from " << peerIp_ << ":" << peerPort_ << std::endl;
            send("{\"type\":\"login_resp\",\"success\":false,\"msg\":\"empty username\"}\n");
            return;
        }

        // 同一用户名重复登录：先踢掉旧会话，再让本次登录生效。
        // 否则旧连接仍占据该用户名，后续按用户名转发会发到已失效的连接上。
        int kicked = server_.removeOtherSessionsByUsername(name, fd_);
        username_ = name;

        LOG() << "login: " << username_ << " from " << peerIp_ << ":" << peerPort_
              << (kicked > 0 ? (" (kicked " + std::to_string(kicked) + " duplicate)") : "")
              << std::endl;
        send("{\"type\":\"login_resp\",\"success\":true}\n");
        server_.broadcastUserList();
    }
    else if (type == "call_offer")
    {
        auto target = findField(line, "target");
        auto sdp = findField(line, "sdp");

        std::string targetIp = server_.getSessionIp(target);
        bool sameNetwork = (!targetIp.empty() && targetIp == peerIp_);

        LOG() << "call_offer " << username_ << "(" << peerIp_ << ")"
              << " -> " << target << "(" << targetIp << ")"
              << " rawSDP=[" << sdp << "]"
              << " sameNetwork=" << (sameNetwork ? "true" : "false")
              << std::endl;

        std::string oldSdp = sdp;

        // SDP format: "publicPort:localPort" (two ports, no IP)
        if (!sdp.empty() && std::isdigit(static_cast<unsigned char>(sdp[0])))
        {
            auto c = sdp.find(':');
            if (c != std::string::npos)
            {
                std::string pubPort = sdp.substr(0, c);
                std::string localPort = sdp.substr(c + 1);
                LOG() << "  parsed format: pubPort=" << pubPort
                      << " localPort=" << localPort << std::endl;
                if (sameNetwork)
                {
                    std::string lanIp = findField(line, "lanIp");
                    sdp = (lanIp.empty() ? "127.0.0.1" : lanIp) + ":" + localPort;
                    LOG() << "  same network -> use " << sdp << std::endl;
                }
                else if (pubPort != "0")
                {
                    sdp = peerIp_ + ":" + pubPort;
                    LOG() << "  cross network pubPort valid -> use " << peerIp_ << ":" << pubPort << std::endl;
                }
                else
                {
                    sdp = peerIp_ + ":" + localPort;
                    LOG() << "  cross network pubPort=0 fallback -> use " << peerIp_ << ":" << localPort << std::endl;
                }
            }
        }
        // Legacy format "127.0.0.1:port"
        else if (sdp.compare(0, 9, "127.0.0.1") == 0)
        {
            if (!sameNetwork)
            {
                sdp = peerIp_ + sdp.substr(9);
                LOG() << "  legacy 127.0.0.1 cross network -> use " << sdp << std::endl;
            }
            else
            {
                LOG() << "  legacy 127.0.0.1 same network -> keep as-is" << std::endl;
            }
        }
        else
        {
            LOG() << "  unknown SDP format, forward as-is" << std::endl;
        }

        if (oldSdp != sdp)
            LOG() << "  SDP transformed: [" << oldSdp << "] -> [" << sdp << "]" << std::endl;

        std::string fwd = "{\"type\":\"call_offer\",\"from\":\""
                        + username_ + "\",\"sdp\":\"" + sdp + "\"}\n";

        if (!server_.forwardTo(target, fwd))
        {
            LOG() << "call_offer forward failed: target " << target << " not found" << std::endl;
            send("{\"type\":\"call_error\",\"msg\":\"user offline\"}\n");
        }
    }
    else if (type == "call_answer")
    {
        auto target = findField(line, "target");
        auto sdp = findField(line, "sdp");

        std::string targetIp = server_.getSessionIp(target);
        bool sameNetwork = (!targetIp.empty() && targetIp == peerIp_);

        LOG() << "call_answer " << username_ << "(" << peerIp_ << ")"
              << " -> " << target << "(" << targetIp << ")"
              << " rawSDP=[" << sdp << "]"
              << " sameNetwork=" << (sameNetwork ? "true" : "false")
              << std::endl;

        std::string oldSdp = sdp;

        // SDP format: "publicPort:localPort" (two ports, no IP)
        if (!sdp.empty() && std::isdigit(static_cast<unsigned char>(sdp[0])))
        {
            auto c = sdp.find(':');
            if (c != std::string::npos)
            {
                std::string pubPort = sdp.substr(0, c);
                std::string localPort = sdp.substr(c + 1);
                LOG() << "  parsed format: pubPort=" << pubPort
                      << " localPort=" << localPort << std::endl;
                if (sameNetwork)
                {
                    std::string lanIp = findField(line, "lanIp");
                    sdp = (lanIp.empty() ? "127.0.0.1" : lanIp) + ":" + localPort;
                    LOG() << "  same network -> use " << sdp << std::endl;
                }
                else if (pubPort != "0")
                {
                    sdp = peerIp_ + ":" + pubPort;
                    LOG() << "  cross network pubPort valid -> use " << peerIp_ << ":" << pubPort << std::endl;
                }
                else
                {
                    sdp = peerIp_ + ":" + localPort;
                    LOG() << "  cross network pubPort=0 fallback -> use " << peerIp_ << ":" << localPort << std::endl;
                }
            }
        }
        // Legacy format "127.0.0.1:port"
        else if (sdp.compare(0, 9, "127.0.0.1") == 0)
        {
            if (!sameNetwork)
            {
                sdp = peerIp_ + sdp.substr(9);
                LOG() << "  legacy 127.0.0.1 cross network -> use " << sdp << std::endl;
            }
            else
            {
                LOG() << "  legacy 127.0.0.1 same network -> keep as-is" << std::endl;
            }
        }
        else
        {
            LOG() << "  unknown SDP format, forward as-is" << std::endl;
        }

        if (oldSdp != sdp)
            LOG() << "  SDP transformed: [" << oldSdp << "] -> [" << sdp << "]" << std::endl;

        std::string fwd = "{\"type\":\"call_answer\",\"from\":\""
                        + username_ + "\",\"sdp\":\"" + sdp + "\"}\n";

        if (!server_.forwardTo(target, fwd))
        {
            LOG() << "call_answer forward failed: target " << target << " not found" << std::endl;
            send("{\"type\":\"call_error\",\"msg\":\"caller gone\"}\n");
        }
        else
        {
            // Set up UDP relay fallback binding for this call
            uint32_t relayId = server_.registerBinding(username_, target);
            server_.sendRelayInfo(username_, relayId);
            server_.sendRelayInfo(target, relayId);
        }
    }
    else if (type == "call_early")
    {
        // Callee notifies caller of its public port immediately after receiving
        // the offer, so the caller can punch a hole back before the answer.
        auto target = findField(line, "target");
        auto sdp = findField(line, "sdp");

        std::string targetIp = server_.getSessionIp(target);
        bool sameNetwork = (!targetIp.empty() && targetIp == peerIp_);

        LOG() << "call_early " << username_ << "(" << peerIp_ << ")"
              << " -> " << target << "(" << targetIp << ")"
              << " rawSDP=[" << sdp << "]"
              << " sameNetwork=" << (sameNetwork ? "true" : "false")
              << std::endl;

        std::string oldSdp = sdp;

        if (!sdp.empty() && std::isdigit(static_cast<unsigned char>(sdp[0])))
        {
            auto c = sdp.find(':');
            if (c != std::string::npos)
            {
                std::string pubPort = sdp.substr(0, c);
                std::string localPort = sdp.substr(c + 1);
                if (sameNetwork)
                {
                    std::string lanIp = findField(line, "lanIp");
                    sdp = (lanIp.empty() ? "127.0.0.1" : lanIp) + ":" + localPort;
                }
                else if (pubPort != "0")
                    sdp = peerIp_ + ":" + pubPort;
                else
                    sdp = peerIp_ + ":" + localPort;
            }
        }
        else if (sdp.compare(0, 9, "127.0.0.1") == 0)
        {
            if (!sameNetwork)
                sdp = peerIp_ + sdp.substr(9);
        }

        if (oldSdp != sdp)
            LOG() << "  SDP transformed: [" << oldSdp << "] -> [" << sdp << "]" << std::endl;

        std::string fwd = "{\"type\":\"call_early\",\"from\":\""
                        + username_ + "\",\"sdp\":\"" + sdp + "\"}\n";
        if (!server_.forwardTo(target, fwd))
            LOG() << "call_early forward failed: target " << target << " not found" << std::endl;
    }
    else if (type == "path_report")
    {
        auto target = findField(line, "target");
        auto directRxStr = findField(line, "directRx");
        auto reason = findField(line, "reason");
        auto rxProbes = findField(line, "rxProbes");
        auto videoLoss = findField(line, "videoLoss");

        bool directRx = (directRxStr == "1" || directRxStr == "true");

        LOG() << "path_report " << username_ << " -> " << target
              << " directRx=" << (directRx ? 1 : 0)
              << " rxProbes=" << rxProbes
              << " videoLoss=" << videoLoss
              << " reason=" << reason << std::endl;

        if (!target.empty())
        {
            std::string fwd = "{\"type\":\"path_report\",\"from\":\""
                            + username_ + "\",\"directRx\":\""
                            + (directRx ? "1" : "0") + "\",\"rxProbes\":\""
                            + rxProbes + "\",\"videoLoss\":\""
                            + videoLoss + "\",\"reason\":\"" + reason + "\"}\n";
            server_.forwardTo(target, fwd);
        }

        server_.handlePathReport(username_, directRx, reason);
    }
    else if (type == "hang_up")
    {
        auto target = findField(line, "target");
        std::string fwd = "{\"type\":\"hang_up\",\"from\":\""
                        + username_ + "\"}\n";
        server_.forwardTo(target, fwd);

        // Release the media relay binding for this call
        server_.removeUserBindings(username_);
        if (target != username_)
            server_.removeUserBindings(target);
    }
    else if (type == "logout")
    {
        std::cout << "[Server] " << username_ << " logged out" << std::endl;
        server_.removeSession(fd_);
    }
}
