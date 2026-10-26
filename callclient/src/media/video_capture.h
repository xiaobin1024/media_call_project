#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include "media_device.h"
#include <QVideoFrame>
#include <atomic>

class QThread;
class VideoCaptureWorker;

class VideoCapture : public MediaDevice
{
    Q_OBJECT
public:
    explicit VideoCapture(QObject* parent = nullptr);
    ~VideoCapture() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

signals:
    void frameCaptured(const QVideoFrame& frame);
    void videoStarted(bool ok);
    void cameraError(const QString& errorString);

private:
    QThread* thread_ = nullptr;
    VideoCaptureWorker* worker_ = nullptr;
    std::atomic_bool running_{false};
};

#endif
