#include "call_manager.h"
#include "signaling/signaling_client.h"
#include "transport/rtp_session.h"
#include "media/audio_capture.h"
#include "media/audio_playback.h"
#include "media/video_capture.h"
#include "media/video_render.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QUdpSocket>
#include <QTimer>
#include <QUrl>

CallManager& CallManager::instance()
{
    static CallManager inst;
    return inst;
}

CallManager::CallManager(QObject* parent)
    : QObject(parent)
{
}

CallManager::~CallManager()
{
    shutdown();
}

void CallManager::init(const QString& serverIp, quint16 serverPort,
                       const QString& localSdp)
{
    serverIp_ = serverIp;
    serverPort_ = serverPort;
    localSdp_ = localSdp;

    signaling_ = new SignalingClient(this);

    connect(signaling_, &SignalingClient::loginSuccess,
            this, &CallManager::loginSuccess);
    connect(signaling_, &SignalingClient::loginFailed,
            this, &CallManager::loginFailed);
    connect(signaling_, &SignalingClient::userListUpdated,
            this, &CallManager::userListUpdated);

    connect(signaling_, &SignalingClient::callOfferReceived,
            this, [this](const QString& from, const QString& sdp)
    {
        if (inCall_) return;
        currentPeer_ = from;
        int colon = sdp.lastIndexOf(':');
        QHostAddress addr;
        quint16 port = 0;
        if (colon >= 0)
        {
            addr = QHostAddress(sdp.left(colon));
            port = sdp.mid(colon + 1).toUShort();
        }
        pendingRemoteAddr_ = addr;
        pendingRemotePort_ = port;

        // 在弹出来电弹窗前就先创建媒体 + 发 UDP 探针
        if (!rtpSession_)
            setupMedia();
        if (rtpSession_)
        {
            lastDiscoveredPort_ = 0;
            rtpSession_->sendUdpProbe(QHostAddress(serverIp_), serverPort_ + 1, myUsername_.toUtf8());
            qDebug() << "[CM] early probe sent for" << from
                     << "localPort=" << rtpSession_->localPort();

            // 方案1：尽快拿到自己的公网端口并发"早回答"给主叫方，
            // 让主叫方无需等待 answer 就能反向打洞（双端同时打洞）。
            QElapsedTimer t;
            t.start();
            while (lastDiscoveredPort_ == 0 && t.elapsed() < 2000)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                signaling_->processPendingData();
            }
            if (lastDiscoveredPort_ != 0)
            {
                signaling_->sendCallEarly(from, QString("%1:%2")
                    .arg(lastDiscoveredPort_).arg(rtpSession_->localPort()),
                    localLanIp());
                qDebug() << "[CM] early sdp sent to" << from;
            }
        }

        // 弹出来电提醒（阻塞，等待用户点"接受"）
        emit incomingCall(from);
    });

    connect(signaling_, &SignalingClient::callEarlyReceived,
            this, [this](const QString& from, const QString& sdp)
    {
        if (from != currentPeer_) return;
        if (!rtpSession_) return;
        int colon = sdp.lastIndexOf(':');
        if (colon < 0) return;
        QHostAddress addr(sdp.left(colon));
        quint16 port = sdp.mid(colon + 1).toUShort();
        if (addr.isNull() || port == 0) return;

        // 方案1：主叫方拿到被叫方地址后立即反向打洞，不等 answer
        rtpSession_->setRemotePeer(addr, port);
        earlyPunched_ = true;
        qDebug() << "[CM] early punch to" << addr.toString() << port;
    });

    connect(signaling_, &SignalingClient::relayInfoReceived,
            this, [this](quint32 relayId)
    {
        relayId_ = relayId;
        relayServerAddr_ = QHostAddress(serverIp_);
        relayServerPort_ = serverPort_ + 1;
        if (rtpSession_)
            rtpSession_->setRelayConfig(relayServerAddr_, relayServerPort_, relayId_);
        qDebug() << "[CM] relay id" << relayId << "via" << relayServerAddr_.toString()
                 << relayServerPort_;

        // 中继信息只配置端点，不立即启用中继。
        // 发送中继保活包，让服务器提前学到本端 UDP 端点，便于快速回切。
        if (inCall_ && rtpSession_)
        {
            rtpSession_->sendRelayHello();
            if (pathState_ == PathState::Relay)
                enableRelay();
        }
    });

    connect(signaling_, &SignalingClient::pathDecisionReceived,
            this, &CallManager::onPathDecision);

    connect(signaling_, &SignalingClient::pathReportReceived,
            this, [this](const QString& from, bool directRx, const QString& reason)
    {
        if (from != currentPeer_) return;
        peerDirectRx_ = directRx;
        qDebug() << "[CM] peer path report directRx=" << directRx
                 << "reason=" << reason;
    });

    connect(signaling_, &SignalingClient::callAnswerReceived,
            this, [this](const QString& from, const QString& sdp)
    {
        Q_UNUSED(from)
        int colon = sdp.lastIndexOf(':');
        if (colon < 0) return;
        QHostAddress addr(sdp.left(colon));
        quint16 port = sdp.mid(colon + 1).toUShort();
        if (addr.isNull() || port == 0) return;
        if (rtpSession_)
        {
            rtpSession_->setRemotePeer(addr, port);
            startMedia();
            emit callConnected();
        }
    });

    connect(signaling_, &SignalingClient::callEnded,
            this, [this](const QString& from)
    {
        Q_UNUSED(from)
        if (inCall_)
        {
            stopMedia();
            teardownMedia();
            currentPeer_.clear();
            inCall_ = false;
            emit callDisconnected();
        }
    });

    connect(signaling_, &SignalingClient::callError,
            this, &CallManager::callError);

    connect(signaling_, &SignalingClient::portDiscovered,
            this, [this](quint16 port)
    {
        qDebug() << "[CM] portDiscovered signal: LDP set to" << port;
        lastDiscoveredPort_ = port;
    });

    signaling_->connectToServer(serverIp, serverPort);
}

void CallManager::shutdown()
{
    hangUp();
    if (signaling_)
    {
        signaling_->disconnectFromServer();
        signaling_->deleteLater();
        signaling_ = nullptr;
    }
    teardownMedia();
}

void CallManager::login(const QString& username)
{
    myUsername_ = username;
    signaling_->login(username);
}

void CallManager::logout()
{
    hangUp();
    signaling_->logout();
    myUsername_.clear();
}

void CallManager::call(const QString& target)
{
    if (inCall_)
    {
        emit callError("Already in a call");
        return;
    }
    currentPeer_ = target;
    setupMedia();

    quint16 localPort = rtpSession_->localPort();
    lastDiscoveredPort_ = 0;

    rtpSession_->sendUdpProbe(QHostAddress(serverIp_), serverPort_ + 1, myUsername_.toUtf8());

    qDebug() << "[CM] call: probe sent, waiting 2s, LDP=" << lastDiscoveredPort_;

    QElapsedTimer timer;
    timer.start();
    while (lastDiscoveredPort_ == 0 && timer.elapsed() < 2000)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        signaling_->processPendingData();
    }

    quint16 pubPort = lastDiscoveredPort_;
    QString lanIp = localLanIp();
    qDebug() << "[CM] call: loop done, pubPort=" << pubPort
             << "localPort=" << localPort << "lanIp=" << lanIp;
    QString sdp = QString("%1:%2").arg(pubPort).arg(localPort);
    signaling_->sendCallOffer(target, sdp, lanIp);
}

void CallManager::acceptCall()
{
    if (inCall_) return;
    if (!rtpSession_)
        setupMedia();

    quint16 localPort = rtpSession_ ? rtpSession_->localPort() : 0;

    qDebug() << "[CM] acceptCall: start, LDP=" << lastDiscoveredPort_
             << "localPort=" << localPort;

    // 如果端口还未发现，等待（有早发探针通常不会发生）
    if (lastDiscoveredPort_ == 0 && rtpSession_)
    {
        rtpSession_->sendUdpProbe(QHostAddress(serverIp_), serverPort_ + 1, myUsername_.toUtf8());
        QElapsedTimer timer;
        timer.start();
        while (lastDiscoveredPort_ == 0 && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            signaling_->processPendingData();
        }
        qDebug() << "[CM] acceptCall: fallback wait done, LDP=" << lastDiscoveredPort_;
    }

    startMedia();

    quint16 pubPort = lastDiscoveredPort_;
    QString lanIp = localLanIp();
    qDebug() << "[CM] acceptCall: final, pubPort=" << pubPort
             << "localPort=" << localPort << "lanIp=" << lanIp;
    QString sdp = QString("%1:%2").arg(pubPort).arg(localPort);
    signaling_->sendCallAnswer(currentPeer_, sdp, lanIp);
    emit callConnected();
}

void CallManager::setMuted(bool muted)
{
    if (rtpSession_)
        rtpSession_->setMuted(muted);
}

void CallManager::hangUp()
{
    if (!inCall_) return;
    if (!currentPeer_.isEmpty())
        signaling_->sendHangUp(currentPeer_);
    stopMedia();
    teardownMedia();
    currentPeer_.clear();
    inCall_ = false;
    emit callDisconnected();
}

void CallManager::setupMedia()
{
    if (rtpSession_)
    {
        if (pendingRemotePort_ != 0)
        {
            rtpSession_->setRemotePeer(pendingRemoteAddr_, pendingRemotePort_);
            pendingRemoteAddr_ = QHostAddress();
            pendingRemotePort_ = 0;
        }
        return;
    }

    uint32_t ssrc = QRandomGenerator::global()->bounded(UINT32_MAX);

    rtpSession_ = new RtpSession(this);
    rtpSession_->setSsrc(ssrc);
    rtpSession_->start(0);

    connect(rtpSession_, &RtpSession::directProbeReceived,
            this, [this](quint32) { onDirectPacketReceived(); });
    connect(rtpSession_, &RtpSession::directMediaReceived,
            this, &CallManager::onDirectPacketReceived);
    if (relayId_ != 0)
        rtpSession_->setRelayConfig(relayServerAddr_, relayServerPort_, relayId_);

    audioCapture_ = new AudioCapture(this);
    audioPlayback_ = new AudioPlayback(this);
    videoCapture_ = new VideoCapture(this);
    videoRender_ = new VideoRender(this);

    connect(videoCapture_, &VideoCapture::videoStarted, this, [](bool ok)
    {
        qDebug() << "[CM] video capture started=" << ok;
        if (!ok)
            qWarning() << "[CM] local video unavailable, check camera device/permission";
    });
    connect(videoCapture_, &VideoCapture::cameraError, this,
            [](const QString& errorString)
    {
        qWarning() << "[CM] video capture error:" << errorString;
    });

    rtpSession_->setAudioCapture(audioCapture_);
    rtpSession_->setAudioPlayback(audioPlayback_);
    rtpSession_->setVideoCapture(videoCapture_);
    rtpSession_->setVideoRender(videoRender_);

    if (pendingRemotePort_ != 0)
    {
        rtpSession_->setRemotePeer(pendingRemoteAddr_, pendingRemotePort_);
        pendingRemoteAddr_ = QHostAddress();
        pendingRemotePort_ = 0;
    }
}

void CallManager::teardownMedia()
{
    if (pathReportTimer_) pathReportTimer_->stop();
    if (pathCheckTimer_) pathCheckTimer_->stop();
    if (pathDecisionTimer_) pathDecisionTimer_->stop();

    pathState_ = PathState::Probing;
    directRx_ = false;
    peerDirectRx_ = false;
    pathFailureReported_ = false;
    pathEpoch_ = 0;
    directRxPackets_ = 0;
    directRxElapsed_.invalidate();

    relayEnabled_ = false;
    relayId_ = 0;
    earlyPunched_ = false;
    delete rtpSession_;
    rtpSession_ = nullptr;
    delete audioCapture_;
    audioCapture_ = nullptr;
    delete audioPlayback_;
    audioPlayback_ = nullptr;
    delete videoCapture_;
    videoCapture_ = nullptr;
    delete videoRender_;
    videoRender_ = nullptr;
}

void CallManager::startMedia()
{
    if (audioCapture_) audioCapture_->start();
    if (audioPlayback_) audioPlayback_->start();

    if (videoCapture_ && videoCapture_->start())
        qDebug() << "[CM] startMedia: video capture start requested";
    else
        qWarning() << "[CM] startMedia: failed to request video capture start";

    if (videoRender_) videoRender_->start();
    inCall_ = true;

    // 新路径策略：先直连探测，服务端双向确认后再进入 P2P；
    // 中继只做兜底，并且全程保活。
    pathState_ = PathState::Probing;
    directRx_ = false;
    peerDirectRx_ = false;
    pathFailureReported_ = false;
    pathEpoch_ = 0;
    directRxPackets_ = 0;
    directRxElapsed_.invalidate();

    if (rtpSession_)
    {
        rtpSession_->resetVideoStats();
        rtpSession_->setRelayMode(false);
        rtpSession_->setKeepaliveInterval(200);
        if (relayId_ != 0)
            rtpSession_->sendRelayHello();
    }

    if (!pathReportTimer_)
    {
        pathReportTimer_ = new QTimer(this);
        pathReportTimer_->setInterval(1000);
        connect(pathReportTimer_, &QTimer::timeout, this, [this]()
        {
            // 尚未直连时不上报 false，否则会被服务端视为失败；
            // 探测超时由服务端 4 秒计时处理。
            if (inCall_ && directRx_)
                sendPathReport(true, "periodic");
        });
    }

    if (!pathCheckTimer_)
    {
        pathCheckTimer_ = new QTimer(this);
        pathCheckTimer_->setInterval(200);
        connect(pathCheckTimer_, &QTimer::timeout,
                this, &CallManager::checkPathHealth);
    }

    if (!pathDecisionTimer_)
    {
        pathDecisionTimer_ = new QTimer(this);
        pathDecisionTimer_->setSingleShot(true);
        pathDecisionTimer_->setInterval(5000);
        connect(pathDecisionTimer_, &QTimer::timeout, this, [this]()
        {
            // 兼容旧服务端：旧服务端不会下发 path_decision。
            // 5 秒内没有收到任何决策时，如果中继可用则保守回退中继。
            if (inCall_ && pathEpoch_ == 0 && relayId_ != 0)
                reportPathFailure("decision_timeout");
        });
    }

    pathReportTimer_->start();
    pathCheckTimer_->start();
    pathDecisionTimer_->start();

    qDebug() << "[CM] path probing started, relayId=" << relayId_;
}

void CallManager::stopMedia()
{
    if (pathReportTimer_) pathReportTimer_->stop();
    if (pathCheckTimer_) pathCheckTimer_->stop();
    if (pathDecisionTimer_) pathDecisionTimer_->stop();

    if (audioCapture_) audioCapture_->stop();
    if (audioPlayback_) audioPlayback_->stop();
    if (videoCapture_) videoCapture_->stop();
    if (videoRender_) videoRender_->stop();
    inCall_ = false;
}

void CallManager::onDirectPacketReceived()
{
    // relay 期间也记录直连探测，供服务端判断是否值得回切；
    // 媒体仍走中继，不会因为探测包而提前切到直连。
    if (pathFailureReported_ && pathState_ != PathState::Relay)
        return;

    directRxPackets_++;
    directRxElapsed_.start();

    if (!directRx_)
    {
        directRx_ = true;
        qDebug() << "[CM] direct path confirmed locally, report to server";
        sendPathReport(true, pathState_ == PathState::Relay
                                  ? "relay_reprobe" : "first_direct");
    }
}

void CallManager::sendPathReport(bool directRx, const QString& reason)
{
    if (!signaling_ || currentPeer_.isEmpty())
        return;

    double loss = rtpSession_ ? rtpSession_->videoLossPercent() : 0.0;
    signaling_->sendPathReport(currentPeer_, directRx, directRxPackets_,
                               loss, reason);

    qDebug() << "[CM] path_report directRx=" << directRx
             << "rx=" << directRxPackets_
             << "loss=" << QString::number(loss, 'f', 2)
             << "reason=" << reason;
}

void CallManager::checkPathHealth()
{
    if (!inCall_ || !rtpSession_)
        return;

    if (pathState_ == PathState::Relay)
    {
        // relay 期间直连探测只用于回切评估；连续 3 秒收不到就停止上报，
        // 避免用陈旧的成功状态触发无效回切。
        if (directRx_ && directRxElapsed_.isValid() &&
            directRxElapsed_.elapsed() > 3000)
        {
            directRx_ = false;
            qDebug() << "[CM] relay direct probe stale, stop reporting";
        }
        return;
    }

    if (pathFailureReported_ || !directRx_)
        return;

    // Probing/观察期探测包 200ms 一次，600ms 未收到即可判失败；
    // P2P 稳定期探测包降到 1s 一次，允许连续丢 2 个包再回切。
    const int gapLimit = (pathState_ == PathState::P2p) ? 3000 : 600;
    if (directRxElapsed_.isValid() && directRxElapsed_.elapsed() > gapLimit)
    {
        reportPathFailure("probe_timeout");
        return;
    }

    if (pathState_ == PathState::P2pProbation &&
        rtpSession_->videoFrameTotal() >= 30 &&
        rtpSession_->videoLossPercent() > 5.0)
    {
        reportPathFailure("loss_high");
    }
}

void CallManager::reportPathFailure(const QString& reason)
{
    if (pathFailureReported_)
        return;

    // 中继端点还没就绪时不能切中继，否则 sendRelayPacket 无目标会把媒体丢掉。
    // 保持直连，等 relay_info 到达后再由后续健康检查决定。
    if (relayId_ == 0 || relayServerPort_ == 0)
    {
        qDebug() << "[CM] path failure detected but relay not ready:" << reason;
        directRx_ = false; // 避免 200ms 健康检查反复刷日志；直连包恢复后会重新置位
        return;
    }

    pathFailureReported_ = true;
    pathState_ = PathState::Relay;
    directRx_ = false;
    peerDirectRx_ = false;

    // 先本地切回中继，避免等待 TCP 决策期间继续丢媒体；
    // 服务端随后下发的 relay 决策是幂等的。
    enableRelay();

    qDebug() << "[CM] path failure:" << reason << ", fallback to relay";
    sendPathReport(false, reason);
}

void CallManager::onPathDecision(const QString& mode, quint64 epoch,
                                 const QString& reason)
{
    if (epoch < pathEpoch_)
    {
        qDebug() << "[CM] ignore stale path decision epoch=" << epoch
                 << "current=" << pathEpoch_;
        return;
    }

    if (pathFailureReported_ && mode != "relay" && mode != "p2p_probation")
    {
        qDebug() << "[CM] ignore path decision" << mode
                 << "after local failure";
        return;
    }

    pathEpoch_ = epoch;

    if (mode == "relay")
    {
        pathState_ = PathState::Relay;
        enableRelay();
    }
    else if (mode == "p2p_probation")
    {
        // 服务端确认直连探测稳定后发起的回切尝试，清理本地失败锁存。
        pathFailureReported_ = false;
        pathState_ = PathState::P2pProbation;
        relayEnabled_ = false;
        directRx_ = false;
        directRxPackets_ = 0;
        directRxElapsed_.invalidate();
        if (rtpSession_)
        {
            rtpSession_->setRelayMode(false);
            rtpSession_->setKeepaliveInterval(200);
        }
    }
    else if (mode == "p2p")
    {
        pathFailureReported_ = false;
        pathState_ = PathState::P2p;
        relayEnabled_ = false;
        if (rtpSession_)
        {
            rtpSession_->setRelayMode(false);
            rtpSession_->setKeepaliveInterval(1000);
        }
    }
    else if (mode == "probing")
    {
        pathFailureReported_ = false;
        pathState_ = PathState::Probing;
        relayEnabled_ = false;
        directRx_ = false;
        directRxPackets_ = 0;
        directRxElapsed_.invalidate();
        if (rtpSession_)
        {
            rtpSession_->setRelayMode(false);
            rtpSession_->setKeepaliveInterval(200);
        }
    }

    qDebug() << "[CM] path decision applied mode=" << mode
             << "epoch=" << epoch << "reason=" << reason;
}

void CallManager::enableRelay()
{
    pathState_ = PathState::Relay;

    if (relayId_ == 0 || !rtpSession_)
    {
        qDebug() << "[CM] relay decision deferred: no relay id/session yet";
        return;
    }

    relayEnabled_ = true;
    rtpSession_->setRelayMode(true);
    // 1s 一次：同时发中继保活和直连探测，便于 relay 后回切。
    rtpSession_->setKeepaliveInterval(1000);
    directRx_ = false;
    directRxPackets_ = 0;
    directRxElapsed_.invalidate();
    // 立即发一个空负载中继包，让服务器学到本端端点，双向即可对发
    rtpSession_->sendRelayHello();
    qDebug() << "[CM] relay mode active (fallback transport)";
}

QString CallManager::localLanIp()
{
    if (!lanIpCache_.isEmpty())
        return lanIpCache_;

    // 方案1：向服务器"连接"一个 UDP socket，让内核选出站网卡 IP（真实内网 IP）
    QUdpSocket s;
    s.connectToHost(QHostAddress(serverIp_), serverPort_);
    if (s.waitForConnected(100))
    {
        const QHostAddress addr = s.localAddress();
        if (!addr.isNull() && addr != QHostAddress(QHostAddress::AnyIPv4))
            lanIpCache_ = addr.toString();
    }

    // 方案2：兜底，枚举网卡挑第一个非回环 IPv4
    if (lanIpCache_.isEmpty())
    {
        const auto addrs = QNetworkInterface::allAddresses();
        for (const QHostAddress& a : addrs)
        {
            if (!a.isLoopback() && a.protocol() == QAbstractSocket::IPv4Protocol)
            {
                lanIpCache_ = a.toString();
                break;
            }
        }
    }

    qDebug() << "[CM] local lan ip =" << lanIpCache_;
    return lanIpCache_;
}
