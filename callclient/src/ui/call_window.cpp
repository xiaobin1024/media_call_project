#include "call_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>

CallWindow::CallWindow(const QString& peer, QWidget* parent)
    : QWidget(parent), peer_(peer)
{
    setWindowTitle("Call with " + peer);
    resize(640, 480);
    setupUI();
}

void CallWindow::setupUI()
{
    auto* layout = new QVBoxLayout(this);

    peerLabel_ = new QLabel("In call with: " + peer_);
    peerLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(peerLabel_);

    remoteVideo_ = new QWidget();
    remoteVideo_->setMinimumSize(320, 240);
    remoteVideo_->setStyleSheet("background-color: #1a1a2e; border: 1px solid #444;");
    layout->addWidget(remoteVideo_, 1);

    localPreview_ = new QLabel();
    localPreview_->setFixedSize(160, 120);
    localPreview_->setStyleSheet("background-color: #16213e; border: 1px solid #444;");
    localPreview_->setAlignment(Qt::AlignCenter);
    localPreview_->setScaledContents(true);
    layout->addWidget(localPreview_, 0, Qt::AlignRight);

    auto* btnLayout = new QHBoxLayout();
    muteBtn_ = new QPushButton("Mute");
    hangUpBtn_ = new QPushButton("Hang Up");
    hangUpBtn_->setStyleSheet("background-color: #e74c3c; color: white;");
    btnLayout->addStretch();
    btnLayout->addWidget(muteBtn_);
    btnLayout->addWidget(hangUpBtn_);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);

    connect(hangUpBtn_, &QPushButton::clicked, this, &CallWindow::hangUp);
    connect(muteBtn_, &QPushButton::clicked, this, [this]()
    {
        muted_ = !muted_;
        muteBtn_->setText(muted_ ? "Unmute" : "Mute");
        emit muteToggled(muted_);
    });
}

void CallWindow::setRemoteVideoWidget(QWidget* widget)
{
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (!layout) return;
    layout->replaceWidget(remoteVideo_, widget);
    remoteVideo_->deleteLater();
    remoteVideo_ = widget;
}

void CallWindow::showLocalFrame(const QImage& frame)
{
    if (!frame.isNull())
        localPreview_->setPixmap(QPixmap::fromImage(frame));
}
