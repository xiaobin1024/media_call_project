#include "jitter_buffer.h"
#include "rtp_packet.h"

JitterBuffer::JitterBuffer(int maxSize, QObject* parent)
    : QObject(parent), maxSize_(maxSize)
{
}

void JitterBuffer::addPacket(const RtpPacket& packet)
{
    uint16_t seq = packet.sequenceNumber();

    if (!initialized_)
    {
        expectedSeq_ = seq;
        initialized_ = true;
    }

    if (static_cast<int>(buffer_.size()) >= maxSize_)
        return;

    buffer_.insert(seq, packet);

    // Detect lost packets
    if (seq != expectedSeq_ && seq > expectedSeq_)
    {
        int diff = seq - expectedSeq_;
        for (int i = 0; i < diff && i < 10; ++i)
            emit packetLost(expectedSeq_ + i);
    }
}

bool JitterBuffer::hasPacket() const
{
    return !buffer_.isEmpty();
}

RtpPacket JitterBuffer::getPacket()
{
    auto it = buffer_.find(expectedSeq_);
    if (it != buffer_.end())
    {
        RtpPacket pkt = it.value();
        buffer_.erase(it);
        expectedSeq_++;
        return pkt;
    }

    // Missing expected packet, take the earliest available
    if (!buffer_.isEmpty())
    {
        auto first = buffer_.begin();
        expectedSeq_ = first.key();
        RtpPacket pkt = first.value();
        buffer_.erase(first);
        expectedSeq_++;
        return pkt;
    }

    return RtpPacket();
}

void JitterBuffer::reset()
{
    buffer_.clear();
    expectedSeq_ = 0;
    initialized_ = false;
}

int JitterBuffer::size() const
{
    return buffer_.size();
}

bool JitterBuffer::hasLostPacket(uint16_t seq) const
{
    return initialized_ && seq != expectedSeq_;
}
