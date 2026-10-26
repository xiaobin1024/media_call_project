#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include "media_device.h"
#include <QAudioSource>
#include <QMediaDevices>
#include <QAudioFormat>

class AudioCapture : public MediaDevice
{
    Q_OBJECT
public:
    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

signals:
    void audioDataReady(const QByteArray& data);

private:
    QAudioSource* audioSource_ = nullptr;
    QIODevice* ioDevice_ = nullptr;
    bool running_ = false;
};

#endif
