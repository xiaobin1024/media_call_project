#ifndef RTP_PACKET_H
#define RTP_PACKET_H

#include <cstdint>
#include <QtEndian>
#include <QByteArray>
#include <cstring>

// Standard RTP header: 12 bytes
#pragma pack(push, 1)
struct RtpHeader
{
    uint8_t version_padding_cc;   // byte 0: V(2) | P(1) | X(1) | CC(4)
    uint8_t marker_pt;            // byte 1: M(1) | PT(7)
    uint16_t sequence_number;     // bytes 2-3
    uint32_t timestamp;           // bytes 4-7
    uint32_t ssrc;                // bytes 8-11
};
#pragma pack(pop)

class RtpPacket
{
public:
    RtpPacket();

    void setPayloadType(uint8_t pt);
    void setSequenceNumber(uint16_t seq);
    void setTimestamp(uint32_t ts);
    void setSsrc(uint32_t ssrc);
    void setMarker(bool marker);
    void setPayload(const QByteArray& payload);

    uint8_t payloadType() const;
    uint16_t sequenceNumber() const;
    uint32_t timestamp() const;
    uint32_t ssrc() const;
    bool marker() const;
    QByteArray payload() const;

    QByteArray encode() const;
    static RtpPacket decode(const QByteArray& data);

    static constexpr size_t HEADER_SIZE = sizeof(RtpHeader);
    static constexpr uint8_t PT_PCMA = 8;
    static constexpr uint8_t PT_H264 = 96;

private:
    RtpHeader header_;
    QByteArray payload_;
};

#endif
