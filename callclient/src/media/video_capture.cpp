#include "video_capture.h"
#include "video_capture_worker.h"
#include <QMetaType>
#include <QThread>
#include <QDebug>

VideoCapture::VideoCapture(QObject* parent)
    : MediaDevice(parent)
{
    qRegisterMetaType<QVideoFrame>("QVideoFrame");

    thread_ = new QThread(this);
    worker_ = new VideoCaptureWorker;
    worker_->moveToThread(thread_);

    connect(worker_, &VideoCaptureWorker::frameCaptured,
            this, &VideoCapture::frameCaptured);
    connect(worker_, &VideoCaptureWorker::started,
            this, [this](bool ok)
    {
        running_ = ok;
        emit videoStarted(ok);
    });
    connect(worker_, &VideoCaptureWorker::error,
            this, &VideoCapture::cameraError);
    connect(thread_, &QThread::finished,
            worker_, &QObject::deleteLater);

    thread_->start();
}

VideoCapture::~VideoCapture()
{
    running_ = false;
    if (thread_ && thread_->isRunning() && worker_)
        QMetaObject::invokeMethod(worker_, "stop", Qt::BlockingQueuedConnection);

    if (thread_)
    {
        thread_->quit();
        thread_->wait();
    }
    worker_ = nullptr;
}

bool VideoCapture::start()
{
    if (!thread_ || !worker_)
        return false;

    QMetaObject::invokeMethod(worker_, "start", Qt::QueuedConnection);
    return true;
}

void VideoCapture::stop()
{
    running_ = false;
    if (thread_ && thread_->isRunning() && worker_)
        QMetaObject::invokeMethod(worker_, "stop", Qt::QueuedConnection);
}

bool VideoCapture::isRunning() const
{
    return running_;
}
