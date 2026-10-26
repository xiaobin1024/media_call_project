#include "audio_playback.h"

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
    if (device.isNull()) return false;

    audioSink_ = new QAudioSink(device, format, this);
    ioDevice_ = audioSink_->start();
    running_ = (ioDevice_ != nullptr);

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
