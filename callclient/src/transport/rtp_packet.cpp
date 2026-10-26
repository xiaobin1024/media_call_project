#include "rtp_packet.h"

RtpPacket::RtpPacket()
{
    memset(&header_, 0, sizeof(header_));
    header_.version_padding_cc = 0x80; // V=2, P=0, X=0, CC=0
}

void RtpPacket::setPayloadType(uint8_t pt)
{
    header_.marker_pt = (header_.marker_pt & 0x80) | (pt & 0x7F);
}

void RtpPacket::setSequenceNumber(uint16_t seq)
{
    header_.sequence_number = qToBigEndian(seq);
}

void RtpPacket::setTimestamp(uint32_t ts)
{
    header_.timestamp = qToBigEndian(ts);
}

void RtpPacket::setSsrc(uint32_t ssrc)
{
    header_.ssrc = qToBigEndian(ssrc);
}

void RtpPacket::setMarker(bool marker)
{
    if (marker)
        header_.marker_pt |= 0x80;
    else
        header_.marker_pt &= ~0x80;
}

void RtpPacket::setPayload(const QByteArray& payload)
{
    payload_ = payload;
}

uint8_t RtpPacket::payloadType() const
{
    return header_.marker_pt & 0x7F;
}

uint16_t RtpPacket::sequenceNumber() const
{
    return qFromBigEndian<uint16_t>(header_.sequence_number);
}

uint32_t RtpPacket::timestamp() const
{
    return qFromBigEndian<uint32_t>(header_.timestamp);
}

uint32_t RtpPacket::ssrc() const
{
    return qFromBigEndian<uint32_t>(header_.ssrc);
}

bool RtpPacket::marker() const
{
    return (header_.marker_pt & 0x80) != 0;
}

QByteArray RtpPacket::payload() const
{
    return payload_;
}

QByteArray RtpPacket::encode() const
{
    QByteArray data;
    data.append(reinterpret_cast<const char*>(&header_), sizeof(header_));
    data.append(payload_);
    return data;
}

RtpPacket RtpPacket::decode(const QByteArray& data)
{
    RtpPacket packet;
    if (data.size() < static_cast<int>(HEADER_SIZE))
        return packet;

    memcpy(&packet.header_, data.constData(), HEADER_SIZE);
    packet.payload_ = data.mid(HEADER_SIZE);
    return packet;
}
