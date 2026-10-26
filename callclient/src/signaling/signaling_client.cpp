#include "signaling_client.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QHostAddress>

SignalingClient::SignalingClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
    , reconnectTimer_(new QTimer(this))
{
    connect(socket_, &QTcpSocket::connected, this, &SignalingClient::onConnected);
    connect(socket_, &QTcpSocket::disconnected, this, &SignalingClient::onDisconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &SignalingClient::onReadyRead);
    connect(socket_, &QTcpSocket::errorOccurred, this, &SignalingClient::onError);
    connect(reconnectTimer_, &QTimer::timeout, this, &SignalingClient::onReconnectTimer);
}

void SignalingClient::connectToServer(const QString& ip, quint16 port)
{
    serverIp_ = ip;
    serverPort_ = port;
    loggedIn_ = false;
    socket_->connectToHost(ip, port);
}

void SignalingClient::disconnectFromServer()
{
    reconnectTimer_->stop();
    loggedIn_ = false;
    username_.clear();
    socket_->disconnectFromHost();
}

void SignalingClient::login(const QString& username)
{
    username_ = username;
    loggedIn_ = true;
    QJsonObject msg;
    msg["type"] = "login";
    msg["username"] = username;
    sendJson(msg);
}

void SignalingClient::logout()
{
    QJsonObject msg;
    msg["type"] = "logout";
    sendJson(msg);
    loggedIn_ = false;
    reconnectTimer_->stop();
}

void SignalingClient::sendCallOffer(const QString& target, const QString& sdp, const QString& lanIp)
{
    QJsonObject msg;
    msg["type"] = "call_offer";
    msg["target"] = target;
    msg["sdp"] = sdp;
    if (!lanIp.isEmpty())
        msg["lanIp"] = lanIp;
    sendJson(msg);
}

void SignalingClient::sendCallAnswer(const QString& target, const QString& sdp, const QString& lanIp)
{
    QJsonObject msg;
    msg["type"] = "call_answer";
    msg["target"] = target;
    msg["sdp"] = sdp;
    if (!lanIp.isEmpty())
        msg["lanIp"] = lanIp;
    sendJson(msg);
}

void SignalingClient::sendCallEarly(const QString& target, const QString& sdp, const QString& lanIp)
{
    QJsonObject msg;
    msg["type"] = "call_early";
    msg["target"] = target;
    msg["sdp"] = sdp;
    if (!lanIp.isEmpty())
        msg["lanIp"] = lanIp;
    sendJson(msg);
}

void SignalingClient::sendHangUp(const QString& target)
{
    QJsonObject msg;
    msg["type"] = "hang_up";
    msg["target"] = target;
    sendJson(msg);
}

void SignalingClient::sendPathReport(const QString& target, bool directRx,
                                     int rxProbes, double videoLoss,
                                     const QString& reason)
{
    QJsonObject msg;
    msg["type"] = "path_report";
    msg["target"] = target;
    msg["directRx"] = directRx ? "1" : "0";
    msg["rxProbes"] = QString::number(rxProbes);
    msg["videoLoss"] = QString::number(videoLoss, 'f', 2);
    msg["reason"] = reason;
    sendJson(msg);
}

void SignalingClient::onConnected()
{
    reconnectTimer_->stop();
    emit connected();

    if (loggedIn_ && !username_.isEmpty())
    {
        QJsonObject msg;
        msg["type"] = "login";
        msg["username"] = username_;
        sendJson(msg);
    }
}

void SignalingClient::onDisconnected()
{
    emit disconnected();

    if (loggedIn_ && !serverIp_.isEmpty())
        reconnectTimer_->start(reconnectInterval_);
}

void SignalingClient::onReconnectTimer()
{
    if (socket_->state() == QAbstractSocket::UnconnectedState)
        socket_->connectToHost(serverIp_, serverPort_);
}

void SignalingClient::onReadyRead()
{
    readBuf_.append(socket_->readAll());

    while (true)
    {
        int idx = readBuf_.indexOf('\n');
        if (idx < 0) break;

        QByteArray line = readBuf_.left(idx).trimmed();
        readBuf_.remove(0, idx + 1);

        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError) continue;

        handleMessage(doc.object());
    }
}

void SignalingClient::processPendingData()
{
    if (socket_->bytesAvailable() > 0)
    {
        readBuf_.append(socket_->readAll());
        while (true)
        {
            int idx = readBuf_.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = readBuf_.left(idx).trimmed();
            readBuf_.remove(0, idx + 1);
            if (line.isEmpty()) continue;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError) continue;
            handleMessage(doc.object());
        }
    }
}

void SignalingClient::onError(QAbstractSocket::SocketError)
{
    emit loginFailed(socket_->errorString());

    if (!serverIp_.isEmpty())
        reconnectTimer_->start(reconnectInterval_);
}

void SignalingClient::handleMessage(const QJsonObject& msg)
{
    QString type = msg["type"].toString();
    qDebug().noquote() << "[SC] recv type=" << type;

    if (type == "login_resp")
    {
        if (msg["success"].toBool())
            emit loginSuccess();
        else
            emit loginFailed(msg["reason"].toString());
    }
    else if (type == "user_list")
    {
        QStringList users;
        for (const auto& u : msg["users"].toArray())
        {
            QString name = u.toString();
            if (name != username_)
                users.append(name);
        }
        emit userListUpdated(users);
    }
    else if (type == "call_offer")
    {
        emit callOfferReceived(msg["from"].toString(), msg["sdp"].toString());
    }
    else if (type == "call_early")
    {
        emit callEarlyReceived(msg["from"].toString(), msg["sdp"].toString());
    }
    else if (type == "call_answer")
    {
        emit callAnswerReceived(msg["from"].toString(), msg["sdp"].toString());
    }
    else if (type == "relay_info")
    {
        quint32 relayId = static_cast<quint32>(msg["relayId"].toInt());
        qDebug().noquote() << "[SC] relay_info relayId=" << relayId;
        emit relayInfoReceived(relayId);
    }
    else if (type == "path_decision")
    {
        QString mode = msg["mode"].toString();
        quint64 epoch = static_cast<quint64>(msg["epoch"].toDouble());
        QString reason = msg["reason"].toString();
        qDebug().noquote() << "[SC] path_decision mode=" << mode
                           << "epoch=" << epoch
                           << "reason=" << reason;
        emit pathDecisionReceived(mode, epoch, reason);
    }
    else if (type == "path_report")
    {
        emit pathReportReceived(msg["from"].toString(),
                                msg["directRx"].toString() == "1",
                                msg["reason"].toString());
    }
    else if (type == "hang_up")
    {
        emit callEnded(msg["from"].toString());
    }
    else if (type == "call_error")
    {
        emit callError(msg["msg"].toString());
    }
    else if (type == "port_discovered")
    {
        quint16 port = static_cast<quint16>(msg["port"].toInt());
        qDebug().noquote() << "[SC] port_discovered port=" << port;
        emit portDiscovered(port);
    }
}

void SignalingClient::sendProbe(quint16 localPort)
{
    QJsonObject msg;
    msg["type"] = "port_probe";
    msg["localPort"] = static_cast<int>(localPort);
    sendJson(msg);
}

void SignalingClient::sendJson(const QJsonObject& msg)
{
    QByteArray data = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    data.append('\n');
    socket_->write(data);
}