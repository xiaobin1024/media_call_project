#ifndef VIDEO_CAPTURE_WORKER_H
#define VIDEO_CAPTURE_WORKER_H

#include <QObject>
#include <QVideoFrame>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;

class VideoCaptureWorker : public QObject
{
    Q_OBJECT
public:
    explicit VideoCaptureWorker(QObject* parent = nullptr);
    ~VideoCaptureWorker() override;

public slots:
    void start();
    void stop();

signals:
    void frameCaptured(const QVideoFrame& frame);
    void started(bool ok);
    void error(const QString& errorString);

private:
    void releaseCamera();

    QCamera* camera_ = nullptr;
    QMediaCaptureSession* captureSession_ = nullptr;
    QVideoSink* videoSink_ = nullptr;
    bool running_ = false;
    bool startFailed_ = false;
};

#endif
