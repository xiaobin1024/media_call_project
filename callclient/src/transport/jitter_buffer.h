#ifndef JITTER_BUFFER_H
#define JITTER_BUFFER_H

#include <QObject>
#include <QMap>
#include <QByteArray>
#include <cstdint>

class RtpPacket;

class JitterBuffer : public QObject
{
    Q_OBJECT
public:
    explicit JitterBuffer(int maxSize = 200, QObject* parent = nullptr);

    void addPacket(const RtpPacket& packet);
    bool hasPacket() const;
    RtpPacket getPacket();

    void reset();
    int size() const;
    bool hasLostPacket(uint16_t seq) const;

signals:
    void packetLost(uint16_t seq);

private:
    QMap<uint16_t, RtpPacket> buffer_;
    uint16_t expectedSeq_ = 0;
    bool initialized_ = false;
    int maxSize_;
};

#endif
