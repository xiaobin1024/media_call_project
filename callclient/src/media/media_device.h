#ifndef MEDIA_DEVICE_H
#define MEDIA_DEVICE_H

#include <QObject>

class MediaDevice : public QObject
{
    Q_OBJECT
public:
    explicit MediaDevice(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~MediaDevice() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};

#endif
