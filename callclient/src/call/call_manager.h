#ifndef CALL_MANAGER_H
#define CALL_MANAGER_H

#include <QObject>
#include <QHostAddress>
#include <QElapsedTimer>
#include <cstdint>

class QTimer;
class SignalingClient;
class RtpSession;
class AudioCapture;
class AudioPlayback;
class VideoCapture;
class VideoRender;

class CallManager : public QObject
{
    Q_OBJECT
public:
    static CallManager& instance();

    void init(const QString& serverIp, quint16 serverPort,
              const QString& localSdp = "");
    void shutdown();

    void login(const QString& username);
    void logout();
    void call(const QString& target);
    void hangUp();
    void acceptCall();
    void setMuted(bool muted);

    SignalingClient* signalingClient() const { return signaling_; }
    RtpSession* rtpSession() const { return rtpSession_; }
    QString currentPeer() const { return currentPeer_; }

signals:
    void loginSuccess();
    void loginFailed(const QString& reason);
    void userListUpdated(const QStringList& users);
    void incomingCall(const QString& from);
    void callConnected();
    void callDisconnected();
    void callError(const QString& msg);

private:
    enum class PathState { Probing, P2pProbation, P2p, Relay };

    explicit CallManager(QObject* parent = nullptr);
    ~CallManager() override;
    CallManager(const CallManager&) = delete;
    CallManager& operator=(const CallManager&) = delete;

    void setupMedia();
    void teardownMedia();
    void startMedia();
    void stopMedia();
    void enableRelay();
    void onPathDecision(const QString& mode, quint64 epoch,
                        const QString& reason);
    void onDirectPacketReceived();
    void sendPathReport(bool directRx, const QString& reason);
    void checkPathHealth();
    void reportPathFailure(const QString& reason);
    QString localLanIp();

    SignalingClient* signaling_ = nullptr;
    RtpSession* rtpSession_ = nullptr;
    QTimer* pathReportTimer_ = nullptr;
    QTimer* pathCheckTimer_ = nullptr;
    QTimer* pathDecisionTimer_ = nullptr;

    QHostAddress relayServerAddr_;
    quint16 relayServerPort_ = 0;
    quint32 relayId_ = 0;
    bool relayEnabled_ = false;
    bool earlyPunched_ = false;

    PathState pathState_ = PathState::Probing;
    bool directRx_ = false;
    bool peerDirectRx_ = false;
    bool pathFailureReported_ = false;
    quint64 pathEpoch_ = 0;
    int directRxPackets_ = 0;
    QElapsedTimer directRxElapsed_;

    AudioCapture* audioCapture_ = nullptr;
    AudioPlayback* audioPlayback_ = nullptr;
    VideoCapture* videoCapture_ = nullptr;
    VideoRender* videoRender_ = nullptr;

    QString serverIp_;
    quint16 serverPort_ = 0;
    QString localSdp_;
    QString currentPeer_;
    QString myUsername_;
    QString lanIpCache_;
    bool inCall_ = false;

    QHostAddress pendingRemoteAddr_;
    quint16 pendingRemotePort_ = 0;

    quint16 lastDiscoveredPort_ = 0;
};

#endif
