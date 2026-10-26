#include "audio_playback.h"

#include <QDebug>

AudioPlayback::AudioPlayback(QObject* parent)
    : MediaDevice(parent)
{
}

AudioPlayback::~AudioPlayback()
{
    stop();
}

bool AudioPlayback::start()
{
    if (running_) return true;

    QAudioFormat format;
    format.setSampleRate(8000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    auto device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
    {
        qWarning() << "[AudioPlayback] 无可用音频输出设备（扬声器缺失或被禁用）";
        return false;
    }

    audioSink_ = new QAudioSink(device, format, this);
    ioDevice_ = audioSink_->start();
    running_ = (ioDevice_ != nullptr);

    qDebug() << "[AudioPlayback] start device=" << device.description()
             << "rate= 8000 ch= 1 ok=" << running_
             << "error=" << audioSink_->error();

    return running_;
}

void AudioPlayback::stop()
{
    if (audioSink_)
    {
        audioSink_->stop();
        delete audioSink_;
        audioSink_ = nullptr;
    }
    ioDevice_ = nullptr;
    running_ = false;
}

bool AudioPlayback::isRunning() const
{
    return running_;
}

void AudioPlayback::enqueueData(const QByteArray& data)
{
    if (ioDevice_ && running_)
        ioDevice_->write(data);
}
