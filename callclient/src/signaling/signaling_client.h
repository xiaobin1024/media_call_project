#ifndef SIGNALING_CLIENT_H
#define SIGNALING_CLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QJsonObject>
#include <QTimer>

class SignalingClient : public QObject
{
    Q_OBJECT
public:
    explicit SignalingClient(QObject* parent = nullptr);

    void connectToServer(const QString& ip, quint16 port);
    void disconnectFromServer();
    void login(const QString& username);
    void logout();
    void sendCallOffer(const QString& target, const QString& sdp, const QString& lanIp = "");
    void sendCallAnswer(const QString& target, const QString& sdp, const QString& lanIp = "");
    void sendCallEarly(const QString& target, const QString& sdp, const QString& lanIp = "");
    void sendHangUp(const QString& target);
    void sendProbe(quint16 localPort);
    void sendPathReport(const QString& target, bool directRx,
                        int rxProbes, double videoLoss,
                        const QString& reason);
    void processPendingData();

signals:
    void connected();
    void disconnected();
    void loginSuccess();
    void loginFailed(const QString& reason);
    void userListUpdated(const QStringList& users);
    void callOfferReceived(const QString& from, const QString& sdp);
    void callEarlyReceived(const QString& from, const QString& sdp);
    void callAnswerReceived(const QString& from, const QString& sdp);
    void relayInfoReceived(quint32 relayId);
    void pathDecisionReceived(const QString& mode, quint64 epoch,
                              const QString& reason);
    void pathReportReceived(const QString& from, bool directRx,
                            const QString& reason);
    void callEnded(const QString& from);
    void callError(const QString& msg);
    void portDiscovered(quint16 port);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onError(QAbstractSocket::SocketError error);
    void onReconnectTimer();

private:
    void handleMessage(const QJsonObject& msg);
    void sendJson(const QJsonObject& msg);

    QTcpSocket* socket_;
    QString serverIp_;
    quint16 serverPort_;
    QString username_;
    QByteArray readBuf_;
    QTimer* reconnectTimer_;
    int reconnectInterval_ = 2000;
    bool loggedIn_ = false;
};

#endif
