#include "video_capture_worker.h"
#include <QCamera>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoSink>
#include <QDebug>

VideoCaptureWorker::VideoCaptureWorker(QObject* parent)
    : QObject(parent)
{
}

VideoCaptureWorker::~VideoCaptureWorker()
{
    stop();
}

void VideoCaptureWorker::start()
{
    if (camera_ && running_)
    {
        emit started(true);
        return;
    }

    releaseCamera();
    startFailed_ = false;

    const auto inputs = QMediaDevices::videoInputs();
    qDebug() << "[VideoWorker] camera inputs:" << inputs.size();
    for (const auto& input : inputs)
        qDebug() << "[VideoWorker] camera:" << input.description() << input.id();

    if (inputs.isEmpty())
    {
        qWarning() << "[VideoWorker] no camera found";
        emit error(QStringLiteral("no camera found"));
        emit started(false);
        return;
    }

    const QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (device.isNull())
    {
        qWarning() << "[VideoWorker] no default camera";
        emit error(QStringLiteral("no default camera"));
        emit started(false);
        return;
    }

    qDebug() << "[VideoWorker] starting camera:" << device.description();

    camera_ = new QCamera(device, this);
    captureSession_ = new QMediaCaptureSession(this);
    videoSink_ = new QVideoSink(this);

    captureSession_->setCamera(camera_);
    captureSession_->setVideoSink(videoSink_);

    connect(videoSink_, &QVideoSink::videoFrameChanged,
            this, &VideoCaptureWorker::frameCaptured);
    connect(camera_, &QCamera::errorOccurred, this,
            [this](QCamera::Error error, const QString& errorString)
    {
        qWarning() << "[VideoWorker] camera error:" << error << errorString;
        if (running_)
        {
            running_ = false;
            emit started(false);
        }
        else
        {
            startFailed_ = true;
        }
        emit this->error(errorString);
    });
    connect(camera_, &QCamera::activeChanged, this, [](bool active)
    {
        qDebug() << "[VideoWorker] camera active:" << active;
    });

    camera_->start();

    if (camera_->error() != QCamera::NoError || startFailed_)
    {
        qWarning() << "[VideoWorker] camera failed to start:"
                   << camera_->error() << camera_->errorString();
        emit started(false);
        return;
    }

    running_ = true;
    qDebug() << "[VideoWorker] camera started";
    emit started(true);
}

void VideoCaptureWorker::stop()
{
    running_ = false;
    releaseCamera();
}

void VideoCaptureWorker::releaseCamera()
{
    if (camera_)
    {
        camera_->stop();
        delete camera_;
        camera_ = nullptr;
    }
    delete captureSession_;
    captureSession_ = nullptr;
    delete videoSink_;
    videoSink_ = nullptr;
}
