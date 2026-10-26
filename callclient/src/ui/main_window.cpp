#include "main_window.h"
#include "call/call_manager.h"
#include "ui/call_window.h"
#include "transport/rtp_session.h"
#include "media/video_capture.h"
#include "media/video_render.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStyle>
#include <QVideoFrameFormat>
#include <QListWidgetItem>

namespace {
// 通过动态属性切换 QLabel 的配色（app.qss 里的 QLabel[state="..."]）。
// 改属性后必须手动 polish，否则样式不会立即刷新。
void setLabelState(QLabel* label, const char* state)
{
    if (!label) return;
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
}

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
    loginLayout->setContentsMargins(28, 28, 28, 28);
    loginLayout->setSpacing(10);

    auto* loginHeading = new QLabel("Username");
    loginHeading->setProperty("heading", true);
    loginLayout->addWidget(loginHeading);

    usernameEdit_ = new QLineEdit();
    usernameEdit_->setPlaceholderText("输入用户名，回车登录");
    usernameEdit_->setMaxLength(32);
    loginLayout->addWidget(usernameEdit_);

    loginBtn_ = new QPushButton("Login");
    loginBtn_->setProperty("primary", true);
    loginLayout->addWidget(loginBtn_);

    statusLabel_ = new QLabel();
    statusLabel_->setWordWrap(true);
    loginLayout->addWidget(statusLabel_);
    loginLayout->addStretch();

    connect(loginBtn_, &QPushButton::clicked, this, &MainWindow::onLoginClicked);
    // 回车即登录，省去一次鼠标点击
    connect(usernameEdit_, &QLineEdit::returnPressed, this, &MainWindow::onLoginClicked);

    mainWidget_ = new QWidget();
    auto* mainLayout = new QVBoxLayout(mainWidget_);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(10);

    selfLabel_ = new QLabel("Not logged in");
    selfLabel_->setProperty("state", "ok");
    mainLayout->addWidget(selfLabel_);

    auto* listHeading = new QLabel("Online users");
    listHeading->setProperty("heading", true);
    mainLayout->addWidget(listHeading);

    userList_ = new QListWidget();
    mainLayout->addWidget(userList_, 1);

    auto* btnLayout = new QHBoxLayout();
    callBtn_ = new QPushButton("Call");
    callBtn_->setProperty("primary", true);
    callBtn_->setEnabled(false);          // 未选中用户时不可点，避免空操作
    logoutBtn_ = new QPushButton("Logout");
    logoutBtn_->setProperty("danger", true);
    btnLayout->addWidget(callBtn_);
    btnLayout->addWidget(logoutBtn_);
    mainLayout->addLayout(btnLayout);

    connect(callBtn_, &QPushButton::clicked, this, &MainWindow::onCallClicked);
    connect(logoutBtn_, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(userList_, &QListWidget::itemSelectionChanged, this, [this]()
    {
        callBtn_->setEnabled(userList_->currentItem() != nullptr);
    });
    connect(userList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item)
    {
        if (item) emit callRequested(item->text());
    });

    stack->addWidget(loginWidget_);
    stack->addWidget(mainWidget_);
    mainWidget_->hide();
}

void MainWindow::onLoginClicked()
{
    auto name = usernameEdit_->text().trimmed();
    if (name.isEmpty())
    {
        statusLabel_->setText("请输入用户名");
        setLabelState(statusLabel_, "error");
        return;
    }
    statusLabel_->setText("正在登录…");
    setLabelState(statusLabel_, "");
    loginBtn_->setEnabled(false);
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
    callBtn_->setEnabled(false);
    selfLabel_->setText("Not logged in");
    setLabelState(selfLabel_, "");
}

void MainWindow::onLoginSuccess()
{
    selfLabel_->setText("已登录：" + usernameEdit_->text());
    setLabelState(selfLabel_, "ok");
    loginWidget_->hide();
    mainWidget_->show();
    statusLabel_->clear();
    setLabelState(statusLabel_, "");
    loginBtn_->setEnabled(true);
    usernameEdit_->clear();
}

void MainWindow::onLoginFailed(const QString& reason)
{
    statusLabel_->setText("登录失败：" + reason);
    setLabelState(statusLabel_, "error");
    loginBtn_->setEnabled(true);
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
