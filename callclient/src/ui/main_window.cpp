#include "main_window.h"
#include "call/call_manager.h"
#include "ui/call_window.h"
#include "transport/rtp_session.h"
#include "media/video_capture.h"
#include "media/video_render.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVideoFrameFormat>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setupUI();
    setWindowTitle("Media Call");
    resize(400, 500);
}

void MainWindow::setupUI()
{
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* stack = new QVBoxLayout(central);

    loginWidget_ = new QWidget();
    auto* loginLayout = new QVBoxLayout(loginWidget_);
    loginLayout->addWidget(new QLabel("Username:"));
    usernameEdit_ = new QLineEdit();
    loginLayout->addWidget(usernameEdit_);
    loginBtn_ = new QPushButton("Login");
    loginLayout->addWidget(loginBtn_);
    statusLabel_ = new QLabel();
    loginLayout->addWidget(statusLabel_);
    loginLayout->addStretch();
    connect(loginBtn_, &QPushButton::clicked, this, &MainWindow::onLoginClicked);

    mainWidget_ = new QWidget();
    auto* mainLayout = new QVBoxLayout(mainWidget_);
    selfLabel_ = new QLabel("Not logged in");
    mainLayout->addWidget(selfLabel_);
    userList_ = new QListWidget();
    mainLayout->addWidget(new QLabel("Online users:"));
    mainLayout->addWidget(userList_);
    auto* btnLayout = new QHBoxLayout();
    callBtn_ = new QPushButton("Call");
    logoutBtn_ = new QPushButton("Logout");
    btnLayout->addWidget(callBtn_);
    btnLayout->addWidget(logoutBtn_);
    mainLayout->addLayout(btnLayout);
    connect(callBtn_, &QPushButton::clicked, this, &MainWindow::onCallClicked);
    connect(logoutBtn_, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);

    stack->addWidget(loginWidget_);
    stack->addWidget(mainWidget_);
    mainWidget_->hide();
}

void MainWindow::onLoginClicked()
{
    auto name = usernameEdit_->text().trimmed();
    if (name.isEmpty())
    {
        statusLabel_->setText("Enter a username");
        return;
    }
    emit loginRequested(name);
}

void MainWindow::onCallClicked()
{
    auto item = userList_->currentItem();
    if (!item) return;
    emit callRequested(item->text());
}

void MainWindow::onLogoutClicked()
{
    CallManager::instance().logout();
    loginWidget_->show();
    mainWidget_->hide();
    userList_->clear();
}

void MainWindow::onLoginSuccess()
{
    selfLabel_->setText("Logged in as: " + usernameEdit_->text());
    loginWidget_->hide();
    mainWidget_->show();
    statusLabel_->clear();
}

void MainWindow::onLoginFailed(const QString& reason)
{
    statusLabel_->setText("Login failed: " + reason);
}

void MainWindow::onUserListUpdated(const QStringList& users)
{
    userList_->clear();
    for (const auto& u : users)
        userList_->addItem(u);
}

void MainWindow::onIncomingCall(const QString& from)
{
    auto ret = QMessageBox::question(this, "Incoming Call",
        from + " is calling you. Answer?",
        QMessageBox::Yes | QMessageBox::No);

    if (ret == QMessageBox::Yes)
        CallManager::instance().acceptCall();
}

void MainWindow::onCallConnected()
{
    if (callWindow_) callWindow_->close();

    auto& mgr = CallManager::instance();
    QString peer = mgr.currentPeer();
    if (peer.isEmpty()) peer = "unknown";

    callWindow_ = new CallWindow(peer);
    callWindow_->setAttribute(Qt::WA_DeleteOnClose);

    // Wire up remote video display
    if (auto* sess = mgr.rtpSession())
    {
        if (auto* render = sess->videoRender())
            callWindow_->setRemoteVideoWidget(render->videoWidget());
        // Wire up local preview: forward captured frames to the preview label
        if (auto* cap = sess->videoCapture())
        {
            connect(cap, &VideoCapture::frameCaptured,
                    this, [this](const QVideoFrame& frame)
            {
                if (!callWindow_) return;
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
                }
                if (!img.isNull())
                    callWindow_->showLocalFrame(img);
            });
        }
    }

    connect(callWindow_, &CallWindow::hangUp,
            this, &MainWindow::onCallWindowHangUp);
    connect(callWindow_, &CallWindow::muteToggled,
            this, [](bool muted) { CallManager::instance().setMuted(muted); });
    callWindow_->show();
    hide();
}

void MainWindow::onCallDisconnected()
{
    if (callWindow_)
    {
        callWindow_->close();
        callWindow_ = nullptr;
    }
    show();
}

void MainWindow::onCallWindowHangUp()
{
    CallManager::instance().hangUp();
}
