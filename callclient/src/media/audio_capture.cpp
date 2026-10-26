#include "audio_capture.h"

AudioCapture::AudioCapture(QObject* parent)
    : MediaDevice(parent)
{
}

AudioCapture::~AudioCapture()
{
    stop();
}

bool AudioCapture::start()
{
    if (running_) return true;

    QAudioFormat format;
    format.setSampleRate(8000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    auto device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) return false;

    audioSource_ = new QAudioSource(device, format, this);
    ioDevice_ = audioSource_->start();

    if (ioDevice_)
    {
        connect(ioDevice_, &QIODevice::readyRead, this, [this]()
        {
            QByteArray data = ioDevice_->readAll();
            if (!data.isEmpty())
                emit audioDataReady(data);
        });
        running_ = true;
    }

    return running_;
}

void AudioCapture::stop()
{
    if (audioSource_)
    {
        audioSource_->stop();
        delete audioSource_;
        audioSource_ = nullptr;
    }
    ioDevice_ = nullptr;
    running_ = false;
}

bool AudioCapture::isRunning() const
{
    return running_;
}
