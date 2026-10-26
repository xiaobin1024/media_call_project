#ifndef VIDEO_RENDER_H
#define VIDEO_RENDER_H

#include "media_device.h"
#include <QImage>
#include <QWidget>
#include <QMutex>

class VideoRender : public MediaDevice
{
    Q_OBJECT
public:
    explicit VideoRender(QObject* parent = nullptr);
    ~VideoRender() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

    QWidget* videoWidget() const;

public slots:
    void renderFrame(const QImage& frame);

private:
    QWidget* displayWidget_ = nullptr;
    QImage currentFrame_;
    QMutex frameMutex_;
    bool running_ = false;
};

#endif
