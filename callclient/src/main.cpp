#include <QApplication>
#include <QDebug>
#include <QDateTime>
#include <QMessageLogContext>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
#include "ui/main_window.h"
#include "call/call_manager.h"

static void debugHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    fprintf(stdout, "[%s] %s\n",
            qPrintable(QDateTime::currentDateTime().toString("HH:mm:ss.zzz")),
            qPrintable(msg));
    fflush(stdout);
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
    {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    qInstallMessageHandler(debugHandler);
    QApplication app(argc, argv);
    app.setApplicationName("MediaCall");

    MainWindow window;
    window.show();

    // 默认服务器地址（可被同目录 config.json 与命令行覆盖）
    QString serverIp = "47.116.98.181";
    quint16 serverPort = 8889;

    // 1) 读取可执行文件同目录下的 config.json（换服务器只需改这个文件）
    QString configPath = QCoreApplication::applicationDirPath() + "/config.json";
    QFile configFile(configPath);
    if (configFile.exists() && configFile.open(QIODevice::ReadOnly))
    {
        QJsonDocument doc = QJsonDocument::fromJson(configFile.readAll());
        configFile.close();
        if (doc.isObject())
        {
            QJsonObject obj = doc.object();
            if (obj.contains("serverIp") && obj["serverIp"].isString())
                serverIp = obj["serverIp"].toString();
            if (obj.contains("serverPort"))
            {
                int p = obj["serverPort"].toInt(8889);
                if (p > 0 && p <= 65535)
                    serverPort = static_cast<quint16>(p);
            }
            qDebug() << "[Config] 已读取" << configPath
                     << "serverIp =" << serverIp << "serverPort =" << serverPort;
        }
    }
    else
    {
        qDebug() << "[Config] 未找到 config.json，使用内置默认"
                 << serverIp << serverPort;
    }

    // 2) 命令行参数覆盖：call_client <IP> <端口>
    if (argc >= 3) {
        serverIp = argv[1];
        serverPort = static_cast<quint16>(QString(argv[2]).toUInt());
        qDebug() << "[Config] 命令行覆盖为" << serverIp << serverPort;
    }

    CallManager& mgr = CallManager::instance();
    mgr.init(serverIp, serverPort);

    QObject::connect(&mgr, &CallManager::loginSuccess,
                     &window, &MainWindow::onLoginSuccess);
    QObject::connect(&mgr, &CallManager::loginFailed,
                     &window, &MainWindow::onLoginFailed);
    QObject::connect(&mgr, &CallManager::userListUpdated,
                     &window, &MainWindow::onUserListUpdated);
    QObject::connect(&mgr, &CallManager::incomingCall,
                     &window, &MainWindow::onIncomingCall);

    QObject::connect(&window, &MainWindow::loginRequested,
                     &mgr, &CallManager::login);
    QObject::connect(&window, &MainWindow::callRequested,
                     &mgr, [&mgr](const QString& target)
    {
        mgr.call(target);
    });

    QObject::connect(&mgr, &CallManager::callConnected,
                     &window, &MainWindow::onCallConnected);
    QObject::connect(&mgr, &CallManager::callDisconnected,
                     &window, &MainWindow::onCallDisconnected);

    return app.exec();
}
