#include "rtp_session.h"
#include "rtp_packet.h"
#include "jitter_buffer.h"
#include "media/audio_capture.h"
#include "media/audio_playback.h"
#include "media/video_capture.h"
#include "media/video_render.h"
#include <cmath>
#include <QImage>
#include <QVideoFrameFormat>
#include <QDebug>

static constexpr int AUDIO_FRAME_SIZE = 320;
static constexpr int VIDEO_CHUNK_SIZE = 1200;
static constexpr int PUNCH_INTERVAL_MS = 200;

// Relay datagram layout (must match callserver):
//   magic(2 bytes) | version(1) | reserved(1) | relayId(4, big-endian) | payload
static constexpr int RELAY_MAGIC = 0x5A4C;
static constexpr int RELAY_HEADER_SIZE = 8;
static constexpr int DIRECT_PROBE_MAGIC = 0x5A50;
static constexpr int DIRECT_PROBE_SIZE = 8;

RtpSession::RtpSession(QObject* parent)
    : QObject(parent)
    , socket_(new QUdpSocket(this))
    , jitterBuffer_(new JitterBuffer(200, this))
    , audioSendTimer_(new QTimer(this))
    , videoSendTimer_(new QTimer(this))
{
    connect(audioSendTimer_, &QTimer::timeout, this, &RtpSession::sendAudioPacket);
    connect(videoSendTimer_, &QTimer::timeout, this, &RtpSession::sendVideoPacket);
    connect(socket_, &QUdpSocket::readyRead, this, &RtpSession::onReadyRead);

    punchTimer_ = new QTimer(this);
    punchTimer_->setInterval(PUNCH_INTERVAL_MS);
    connect(punchTimer_, &QTimer::timeout, this, [this]() { sendPunch(); });

    // 中继端点保活独立于当前发送路径：P2P 期间也要让服务器保持
    // RelayBinding 的 A/B 端点有效，回切时才能立即转发。
    relayKeepaliveTimer_ = new QTimer(this);
    relayKeepaliveTimer_->setInterval(2000);
    connect(relayKeepaliveTimer_, &QTimer::timeout,
            this, [this]() { sendRelayHello(); });
}

RtpSession::~RtpSession()
{
    stop();
}

bool RtpSession::start(quint16 localPort)
{
    if (running_) return true;

    if (!socket_->bind(localPort))
        return false;

    localPort_ = socket_->localPort();
    running_ = true;
    cameraFrameErrorLogged_ = false;
    audioSendTimer_->start(20);
    videoSendTimer_->start(1000 / videoMaxFps_);

    if (relayPort_ != 0)
        relayKeepaliveTimer_->start();

    return true;
}

void RtpSession::stop()
{
    if (!running_) return;
    punchTimer_->stop();
    relayKeepaliveTimer_->stop();
    audioSendTimer_->stop();
    videoSendTimer_->stop();
    socket_->close();
    jitterBuffer_->reset();
    videoFrames_.clear();
    seenSeqs_.clear();
    running_ = false;
}

bool RtpSession::isRunning() const
{
    return running_;
}

void RtpSession::setRemotePeer(const QHostAddress& addr, quint16 port)
{
    remoteAddr_ = addr;
    remotePort_ = port;

    sendPunch();
    if (running_ && !punchTimer_->isActive())
        punchTimer_->start();
}

void RtpSession::setRelayConfig(const QHostAddress& addr, quint16 port,
                                quint32 relayId)
{
    relayAddr_ = addr;
    relayPort_ = port;
    relayId_ = relayId;
    if (running_ && relayPort_ != 0)
        relayKeepaliveTimer_->start();
    qDebug() << "[RTP] relay config" << addr.toString() << port << "id=" << relayId;
}

void RtpSession::setRelayMode(bool enabled)
{
    if (relayMode_ == enabled) return;
    relayMode_ = enabled;
    qDebug() << "[RTP] relay mode" << (enabled ? "ON" : "OFF");
}

void RtpSession::setKeepaliveInterval(int ms)
{
    if (punchTimer_ && ms > 0)
        punchTimer_->start(ms);
}

void RtpSession::setSsrc(uint32_t ssrc)
{
    ssrc_ = ssrc;
    videoSsrc_ = ssrc + 1; // 视频流独立 SSRC，seq 也按流独立计数（RFC 3550）
}

void RtpSession::sendUdpProbe(const QHostAddress& host, quint16 port,
                              const QByteArray& userTag)
{
    if (userTag.isEmpty())
    {
        // 老格式：1 字节探针，服务端只能按公网 IP 匹配会话
        QByteArray probe(1, 0);
        socket_->writeDatagram(probe, host, port);
        return;
    }

    // 新格式：0x01 + 用户名。同一 NAT 后有多台客户端时公网 IP 相同，
    // 只有带上用户名服务端才能把 port_discovered 发回正确的一端。
    QByteArray probe;
    probe.append(char(0x01));
    probe.append(userTag);
    socket_->writeDatagram(probe, host, port);
}

void RtpSession::sendPunch()
{
    if (!running_ || remotePort_ == 0) return;
    if (relayMode_)
    {
        // 中继模式下仍保留少量直连探测，用于 relay 后的回切评估；
        // 媒体本身继续走中继，不会因探测而切到直连。
        sendRelayHello();
    }

    // 直连探测包：既用于 NAT 打洞，也用于双向可达性确认。
    // 接收端只认这个 magic 或合法 RTP，不再把任意 UDP 包当成媒体。
    QByteArray probe(DIRECT_PROBE_SIZE, '\0');
    probe[0] = char((DIRECT_PROBE_MAGIC >> 8) & 0xff);
    probe[1] = char(DIRECT_PROBE_MAGIC & 0xff);
    probe[2] = 1; // version
    probe[3] = 0; // reserved
    probe[4] = char((relayId_ >> 24) & 0xff);
    probe[5] = char((relayId_ >> 16) & 0xff);
    probe[6] = char((relayId_ >> 8) & 0xff);
    probe[7] = char(relayId_ & 0xff);
    socket_->writeDatagram(probe, remoteAddr_, remotePort_);
}

void RtpSession::sendRelayHello()
{
    if (!running_ || relayPort_ == 0) return;
    sendRelayPacket(QByteArray());
}

bool RtpSession::isDuplicate(uint32_t ssrc, quint16 seq)
{
    QSet<quint16>& seen = seenSeqs_[ssrc];
    if (seen.contains(seq))
        return true;

    // 只需覆盖切换窗口。超过上限后清空，避免长期占用内存；
    // 清空后极少数老包可能重复，但会被 JitterBuffer/视频重组自然吸收。
    if (seen.size() >= 512)
        seen.clear();

    seen.insert(seq);
    return false;
}

double RtpSession::videoLossPercent() const
{
    const int total = videoFramesReceived_ + videoFramesDropped_;
    if (total == 0)
        return 0.0;
    return 100.0 * videoFramesDropped_ / total;
}

int RtpSession::videoFrameTotal() const
{
    return videoFramesReceived_ + videoFramesDropped_;
}

void RtpSession::resetVideoStats()
{
    videoFramesReceived_ = 0;
    videoFramesDropped_ = 0;
}

quint16 RtpSession::localPort() const
{
    return localPort_;
}

void RtpSession::setAudioCapture(AudioCapture* capture)
{
    audioCapture_ = capture;
    if (audioCapture_)
    {
        connect(audioCapture_, &AudioCapture::audioDataReady,
                this, &RtpSession::onAudioDataReady);
    }
}

void RtpSession::setAudioPlayback(AudioPlayback* playback)
{
    audioPlayback_ = playback;
}

void RtpSession::setVideoCapture(VideoCapture* capture)
{
    videoCapture_ = capture;
    if (videoCapture_)
    {
        connect(videoCapture_, &VideoCapture::frameCaptured,
                this, &RtpSession::onVideoFrameCaptured);
    }
}

void RtpSession::setVideoRender(VideoRender* render)
{
    videoRender_ = render;
}

void RtpSession::setMuted(bool muted)
{
    muted_ = muted;
}

QString RtpSession::localSdp() const
{
    return QString("%1:%2").arg(localPort_);
}

void RtpSession::onReadyRead()
{
    while (socket_->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(static_cast<int>(socket_->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        socket_->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        const int n = datagram.size();
        if (n <= 0) continue;

        const auto* raw = reinterpret_cast<const unsigned char*>(datagram.constData());

        // 学习对端真实源地址端口：NAT 分配给媒体流的源端口可能不同于 SDP 公告端口。
        // 中继包来自服务器地址，不会命中 sender == remoteAddr_。
        if (remotePort_ != 0 && senderPort != 0 &&
            sender == remoteAddr_ && senderPort != remotePort_)
        {
            qDebug() << "[RTP] peer source learned from" << sender.toString()
                     << senderPort << "was" << remoteAddr_.toString()
                     << remotePort_ << "-> redirect & reply";
            remoteAddr_ = sender;
            remotePort_ = senderPort;
            sendPunch();
        }

        // 1 字节打洞包：只用于开 NAT，不算直连可达
        if (n == 1)
        {
            if (!relayMode_)
                qDebug() << "[RTP] hole-punch from" << sender.toString() << senderPort;
            continue;
        }

        // 8 字节直连探测包：magic 0x5A50
        if (n >= DIRECT_PROBE_SIZE &&
            raw[0] == ((DIRECT_PROBE_MAGIC >> 8) & 0xff) &&
            raw[1] == (DIRECT_PROBE_MAGIC & 0xff))
        {
            quint32 id = (static_cast<quint32>(raw[4]) << 24)
                       | (static_cast<quint32>(raw[5]) << 16)
                       | (static_cast<quint32>(raw[6]) << 8)
                       | static_cast<quint32>(raw[7]);
            receivedAny_ = true;
            emit directProbeReceived(id);
            continue;
        }

        bool fromRelay = relayPort_ != 0 && sender == relayAddr_ &&
                         senderPort == relayPort_;

        RtpPacket packet = RtpPacket::decode(datagram);
        if (packet.payload().isEmpty())
        {
            if (!relayMode_)
                qDebug() << "[RTP] hole-punch from" << sender.toString() << senderPort;
            continue;
        }

        if (!receivedAny_)
        {
            receivedAny_ = true;
            emit remoteMediaReceived();
        }

        if (fromRelay)
            emit relayMediaReceived();
        else
            emit directMediaReceived();

        // P2P 与中继切换窗口内可能同时收到两条路径的同一包，按 SSRC+seq 去重
        if (isDuplicate(packet.ssrc(), packet.sequenceNumber()))
            continue;

        uint8_t pt = packet.payloadType();

        if (pt == RtpPacket::PT_H264)
        {
            qDebug() << "[RTP] recv video, payload=" << packet.payload().size()
                     << "marker=" << packet.marker()
                     << "seq=" << packet.sequenceNumber()
                     << "ts=" << packet.timestamp();
            handleVideoPacket(packet);
        }
        else
        {
            jitterBuffer_->addPacket(packet);
            while (jitterBuffer_->hasPacket())
            {
                RtpPacket pkt = jitterBuffer_->getPacket();
                if (pkt.payloadType() == RtpPacket::PT_PCMA && audioPlayback_)
                {
                    audioPlayback_->enqueueData(pkt.payload());
                    emit audioPacketReceived(pkt.payload());
                }
            }
        }
    }
}

void RtpSession::onAudioDataReady(const QByteArray& data)
{
    audioBuffer_.append(data);
}

void RtpSession::onVideoFrameCaptured(const QVideoFrame& frame)
{
    QImage img = frame.toImage().copy();
    if (img.isNull())
    {
        QVideoFrame f = frame;
        if (f.map(QVideoFrame::ReadOnly))
        {
            QVideoFrameFormat::PixelFormat pixFmt = f.pixelFormat();
            QImage::Format imgFmt =
                QVideoFrameFormat::imageFormatFromPixelFormat(pixFmt);
            if (imgFmt != QImage::Format_Invalid)
            {
                img = QImage(reinterpret_cast<const uchar*>(f.bits(0)),
                             f.width(), f.height(),
                             f.bytesPerLine(0), imgFmt).copy();
            }
            f.unmap();
        }
        if (img.isNull())
        {
            if (!cameraFrameErrorLogged_)
            {
                cameraFrameErrorLogged_ = true;
                qWarning() << "[RTP] camera frame conversion failed, format="
                           << frame.pixelFormat() << "size=" << frame.size();
            }
            return;
        }
    }
    QMutexLocker lock(&imageMutex_);
    latestImage_ = img;
    newFrameAvailable_ = true;
}

void RtpSession::sendAudioPacket()
{
    if (remotePort_ == 0) return;

    if (muted_)
    {
        audioTimestamp_ += 160;
        audioSeq_++;
        audioBuffer_.clear();
        return;
    }

    if (!audioCapture_ || !audioCapture_->isRunning()) return;

    if (audioBuffer_.size() < AUDIO_FRAME_SIZE) return;

    QByteArray frame = audioBuffer_.left(AUDIO_FRAME_SIZE);
    audioBuffer_.remove(0, AUDIO_FRAME_SIZE);

    RtpPacket packet;
    packet.setPayloadType(RtpPacket::PT_PCMA);
    packet.setSequenceNumber(audioSeq_++);
    packet.setTimestamp(audioTimestamp_);
    packet.setSsrc(ssrc_);

    audioTimestamp_ += 160;

    packet.setPayload(frame);
    sendRtpPacket(packet);
}

void RtpSession::sendVideoPacket()
{
    if (remotePort_ == 0 || !videoCapture_ || !videoCapture_->isRunning())
    {
        qDebug() << "[RTP] sendVideo: blocked, remotePort=" << remotePort_
                 << "cap=" << videoCapture_
                 << "running=" << (videoCapture_ ? videoCapture_->isRunning() : false);
        return;
    }

    QImage img;
    {
        QMutexLocker lock(&imageMutex_);
        if (!newFrameAvailable_)
        {
            qDebug() << "[RTP] sendVideo: no new frame";
            return;
        }
        img = latestImage_;
        newFrameAvailable_ = false;
    }

    if (img.isNull())
    {
        qDebug() << "[RTP] sendVideo: null image";
        return;
    }

    img = img.scaled(QSize(320, 240), Qt::KeepAspectRatio,
                     Qt::SmoothTransformation);

    QByteArray jpegData;
    QBuffer buf(&jpegData);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", videoQuality_);
    buf.close();

    if (jpegData.isEmpty())
    {
        qDebug() << "[RTP] sendVideo: empty JPEG";
        return;
    }

    int fragments = 0;
    int offset = 0;
    while (offset < jpegData.size())
    {
        int chunkSize = qMin(VIDEO_CHUNK_SIZE, jpegData.size() - offset);
        bool last = (offset + chunkSize >= jpegData.size());

        RtpPacket pkt;
        pkt.setPayloadType(RtpPacket::PT_H264);
        pkt.setSequenceNumber(videoSeq_++);
        pkt.setTimestamp(videoTimestamp_);
        pkt.setSsrc(videoSsrc_);
        pkt.setMarker(last);
        pkt.setPayload(jpegData.mid(offset, chunkSize));
        sendRtpPacket(pkt);
        fragments++;
        offset += chunkSize;
    }

    qDebug() << "[RTP] sendVideo: sent frame" << jpegData.size() << "bytes in"
             << fragments << "fragments, ts=" << videoTimestamp_;

    videoTimestamp_ += 9000;
}

void RtpSession::sendRtpPacket(const RtpPacket& packet)
{
    if (remotePort_ == 0 && !relayMode_) return;
    QByteArray data = packet.encode();
    if (relayMode_)
        sendRelayPacket(data);
    else
        socket_->writeDatagram(data, remoteAddr_, remotePort_);
}

void RtpSession::sendRelayPacket(const QByteArray& payload)
{
    if (relayPort_ == 0) return;
    QByteArray hdr(RELAY_HEADER_SIZE, '\0');
    hdr[0] = char((RELAY_MAGIC >> 8) & 0xff);
    hdr[1] = char(RELAY_MAGIC & 0xff);
    hdr[2] = 1; // version
    hdr[3] = 0; // reserved
    hdr[4] = char((relayId_ >> 24) & 0xff);
    hdr[5] = char((relayId_ >> 16) & 0xff);
    hdr[6] = char((relayId_ >> 8) & 0xff);
    hdr[7] = char(relayId_ & 0xff);
    socket_->writeDatagram(hdr + payload, relayAddr_, relayPort_);
}

void RtpSession::handleVideoPacket(const RtpPacket& packet)
{
    uint32_t ts = packet.timestamp();
    uint16_t seq = packet.sequenceNumber();

    // 防止 marker 丢失导致僵尸帧长期驻留：超过阈值则丢弃最旧的帧
    if (videoFrames_.size() > 16)
        videoFrames_.erase(videoFrames_.begin());

    VideoFrameBuf& buf = videoFrames_[ts];
    if (!buf.started)
    {
        buf.started = true;
        buf.minSeq = buf.maxSeq = seq;
    }
    // 用 16 位前向差判断"更新的 seq"，天然处理 seq 回绕
    uint16_t forward = static_cast<uint16_t>(seq - buf.maxSeq);
    if (forward && forward < 0x8000)
        buf.maxSeq = seq;
    forward = static_cast<uint16_t>(buf.minSeq - seq);
    if (forward && forward < 0x8000)
        buf.minSeq = seq;

    buf.frags.insert(seq, packet.payload());

    if (packet.marker())
    {
        VideoFrameBuf finished = videoFrames_.take(ts);

        // 期望分片数 = (maxSeq - minSeq) + 1（无符号差，兼容回绕）
        int expected = static_cast<int>(
                           static_cast<uint16_t>(finished.maxSeq - finished.minSeq)) + 1;

        if (finished.frags.size() != expected)
        {
            videoFramesDropped_++;
            qDebug() << "[RTP] handleVideo: frame incomplete (" << finished.frags.size()
                     << "/" << expected << "frags) ts=" << ts << ", drop frame";
            return; // 缺分片：整帧丢弃，避免解码出花屏
        }

        QByteArray jpegData;
        jpegData.reserve(expected * VIDEO_CHUNK_SIZE);
        for (auto it = finished.frags.constBegin(); it != finished.frags.constEnd(); ++it)
            jpegData += it.value(); // 按 seq 顺序拼接，天然抗乱序

        videoFramesReceived_++;
        if (videoFramesReceived_ % 30 == 0)
        {
            double loss =
                videoFramesReceived_ + videoFramesDropped_ == 0
                    ? 0.0
                    : 100.0 * videoFramesDropped_ /
                          (videoFramesReceived_ + videoFramesDropped_);
            qDebug() << "[RTP] frame stat: ok=" << videoFramesReceived_
                     << "dropped=" << videoFramesDropped_
                     << "loss=" << QString::number(loss, 'f', 2) << "%";
        }

        qDebug() << "[RTP] handleVideo: assembling" << jpegData.size() << "bytes from ts=" << ts;
        QImage img;
        if (img.loadFromData(jpegData, "JPEG"))
        {
            qDebug() << "[RTP] handleVideo: decoded OK, size=" << img.size();
            if (videoRender_)
                videoRender_->renderFrame(img);
            else
                qDebug() << "[RTP] handleVideo: no videoRender_!";
        }
        else
        {
            // 解码失败=该帧实际未显示，计入 dropped，避免 loss% 低估
            videoFramesReceived_--;
            videoFramesDropped_++;
            qDebug() << "[RTP] handleVideo: JPEG decode FAILED, count as dropped"
                     << "(total dropped=" << videoFramesDropped_ << ")";
        }
    }
}
