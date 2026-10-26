#include "audio_capture.h"

#include <QDebug>

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
    if (device.isNull())
    {
        qWarning() << "[AudioCapture] 无可用音频输入设备（麦克风缺失或被占用/禁用）";
        return false;
    }

    audioSource_ = new QAudioSource(device, format, this);
    ioDevice_ = audioSource_->start();

    qDebug() << "[AudioCapture] start device=" << device.description()
             << "rate= 8000 ch= 1 fmt= Int16 ok=" << (ioDevice_ != nullptr)
             << "error=" << audioSource_->error();

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
