#ifndef RTP_SESSION_H
#define RTP_SESSION_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QVideoFrame>
#include <QImage>
#include <QBuffer>
#include <QMutex>
#include <QMap>
#include <QSet>
#include <QHash>
#include <cstdint>

class RtpPacket;
class JitterBuffer;
class AudioCapture;
class AudioPlayback;
class VideoCapture;
class VideoRender;

class RtpSession : public QObject
{
    Q_OBJECT
public:
    explicit RtpSession(QObject* parent = nullptr);
    ~RtpSession();

    bool start(quint16 localPort);
    void stop();
    bool isRunning() const;

    void setRemotePeer(const QHostAddress& addr, quint16 port);
    void setSsrc(uint32_t ssrc);
    uint32_t videoSsrc() const { return videoSsrc_; }
    // userTag 非空时发送"带用户名"的探测包（0x01 + 用户名），服务端按用户名精确匹配；
    // 为空则发老式 1 字节探针（服务端只能按 IP 匹配）。同 NAT 后多客户端必须用前者。
    void sendUdpProbe(const QHostAddress& host, quint16 port,
                      const QByteArray& userTag = QByteArray());

    void setRelayConfig(const QHostAddress& addr, quint16 port, quint32 relayId);
    void setRelayMode(bool enabled);
    void sendRelayHello();
    void setKeepaliveInterval(int ms);
    bool relayMode() const { return relayMode_; }
    bool hasReceivedPackets() const { return receivedAny_; }

    double videoLossPercent() const;
    int videoFrameTotal() const;
    void resetVideoStats();

    quint16 localPort() const;

    void setAudioCapture(AudioCapture* capture);
    void setAudioPlayback(AudioPlayback* capture);
    void setVideoCapture(VideoCapture* capture);
    void setVideoRender(VideoRender* render);
    void setMuted(bool muted);

    uint32_t ssrc() const { return ssrc_; }
    QString localSdp() const;
    AudioCapture* audioCapture() const { return audioCapture_; }
    AudioPlayback* audioPlayback() const { return audioPlayback_; }
    VideoCapture* videoCapture() const { return videoCapture_; }
    VideoRender* videoRender() const { return videoRender_; }

signals:
    void audioPacketReceived(const QByteArray& data);
    void videoPacketReceived(const QVideoFrame& frame);
    void remoteMediaReceived();
    void directProbeReceived(quint32 relayId);
    void directMediaReceived();
    void relayMediaReceived();

private slots:
    void onReadyRead();
    void onAudioDataReady(const QByteArray& data);
    void sendAudioPacket();
    void onVideoFrameCaptured(const QVideoFrame& frame);
    void sendVideoPacket();

private:
    void sendRtpPacket(const RtpPacket& packet);
    void sendRelayPacket(const QByteArray& payload);
    void handleVideoPacket(const RtpPacket& packet);
    void sendPunch();
    bool isDuplicate(uint32_t ssrc, quint16 seq);

    QUdpSocket* socket_;
    QHostAddress remoteAddr_;
    quint16 remotePort_ = 0;
    QTimer* punchTimer_ = nullptr;
    quint16 localPort_ = 0;

    QHostAddress relayAddr_;
    quint16 relayPort_ = 0;
    quint32 relayId_ = 0;
    QTimer* relayKeepaliveTimer_ = nullptr;
    bool relayMode_ = false;
    bool receivedAny_ = false;
    uint32_t ssrc_ = 0;
    uint32_t videoSsrc_ = 0;
    bool running_ = false;
    QHash<uint32_t, QSet<quint16>> seenSeqs_;

    JitterBuffer* jitterBuffer_;

    AudioCapture* audioCapture_ = nullptr;
    AudioPlayback* audioPlayback_ = nullptr;
    VideoCapture* videoCapture_ = nullptr;
    VideoRender* videoRender_ = nullptr;

    uint16_t audioSeq_ = 0;
    uint32_t audioTimestamp_ = 0;
    QTimer* audioSendTimer_;
    QByteArray audioBuffer_;
    bool muted_ = false;

    QTimer* videoSendTimer_;
    QImage latestImage_;
    QMutex imageMutex_;
    bool newFrameAvailable_ = false;
    bool cameraFrameErrorLogged_ = false;
    uint32_t videoTimestamp_ = 0;
    uint16_t videoSeq_ = 0;
    struct VideoFrameBuf
    {
        QMap<uint16_t, QByteArray> frags; // seq -> payload（按键即 seq 有序）
        uint16_t minSeq = 0;
        uint16_t maxSeq = 0;
        bool started = false;
    };
    QMap<uint32_t, VideoFrameBuf> videoFrames_;
    int videoQuality_ = 65;
    int videoMaxFps_ = 10;
    int videoFramesReceived_ = 0; // 成功重组渲染的完整帧
    int videoFramesDropped_ = 0;  // 因缺分片而丢弃的帧
};

#endif
