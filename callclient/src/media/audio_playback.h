#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

#include "media_device.h"
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioFormat>

class AudioPlayback : public MediaDevice
{
    Q_OBJECT
public:
    explicit AudioPlayback(QObject* parent = nullptr);
    ~AudioPlayback() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

    void enqueueData(const QByteArray& data);

private:
    QAudioSink* audioSink_ = nullptr;
    QIODevice* ioDevice_ = nullptr;
    bool running_ = false;
};

#endif
