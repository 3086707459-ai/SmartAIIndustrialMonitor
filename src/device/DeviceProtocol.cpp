#include "device/DeviceProtocol.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>

#include <cstring>
#include <algorithm>
#include <cmath>

namespace saim {

namespace {

/// Big-endian helpers.  Written out explicitly rather than using qToBigEndian so
/// that the wire format is auditable by reading this file alone.
void appendUint32(QByteArray& out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xFF));
    out.append(static_cast<char>((value >> 16) & 0xFF));
    out.append(static_cast<char>((value >> 8) & 0xFF));
    out.append(static_cast<char>(value & 0xFF));
}

quint32 readUint32(const char* data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return (static_cast<quint32>(bytes[0]) << 24) | (static_cast<quint32>(bytes[1]) << 16)
           | (static_cast<quint32>(bytes[2]) << 8) | static_cast<quint32>(bytes[3]);
}

/// JSON keys are centralised here: the encoder and the decoder must not be able
/// to drift apart, and a typo would otherwise present as "all values are zero".
namespace key {
constexpr const char* kId          = "id";
constexpr const char* kTimestamp   = "ts";
constexpr const char* kTemperature = "temperature";
constexpr const char* kVoltage     = "voltage";
constexpr const char* kCurrent     = "current";
constexpr const char* kVibration   = "vibration";
constexpr const char* kRpm         = "rpm";
constexpr const char* kValid       = "valid";
constexpr const char* kType        = "kind";
constexpr const char* kMessage     = "message";
}   // namespace key

}   // namespace

QString toString(MessageType type)
{
    switch (type)
    {
    case MessageType::Telemetry:   return QStringLiteral("TELEMETRY");
    case MessageType::Heartbeat:   return QStringLiteral("HEARTBEAT");
    case MessageType::Register:    return QStringLiteral("REGISTER");
    case MessageType::Command:     return QStringLiteral("COMMAND");
    case MessageType::Acknowledge: return QStringLiteral("ACK");
    case MessageType::Alarm:       return QStringLiteral("ALARM");
    case MessageType::Error:       return QStringLiteral("ERROR");
    case MessageType::Snapshot:    return QStringLiteral("SNAPSHOT");
    }
    return QStringLiteral("UNKNOWN");
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

QByteArray ProtocolCodec::encode(MessageType type, const QJsonObject& payload)
{
    // Compact, not indented: every byte on this link is transmitted 2 Hz x N
    // devices and the peer is a machine, not a human reading a log.
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    QByteArray frame;
    frame.reserve(kHeaderSize + body.size());
    frame.append(kMagic0);
    frame.append(kMagic1);
    frame.append(static_cast<char>(kVersion));
    frame.append(static_cast<char>(type));
    appendUint32(frame, static_cast<quint32>(body.size()));
    frame.append(body);
    return frame;
}

QByteArray ProtocolCodec::encodeSample(const SensorSample& sample)
{
    QJsonObject payload;
    payload.insert(QLatin1String(key::kId), sample.deviceId);
    payload.insert(QLatin1String(key::kTimestamp),
                   sample.timestamp.isValid() ? sample.timestamp.toString(Qt::ISODateWithMs)
                                              : QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    payload.insert(QLatin1String(key::kTemperature), sample.temperature);
    payload.insert(QLatin1String(key::kVoltage), sample.voltage);
    payload.insert(QLatin1String(key::kCurrent), sample.current);
    payload.insert(QLatin1String(key::kVibration), sample.vibration);
    payload.insert(QLatin1String(key::kRpm), sample.rpm);
    payload.insert(QLatin1String(key::kValid), sample.valid);
    return encode(MessageType::Telemetry, payload);
}

Result<SensorSample> ProtocolCodec::decodeSample(const QJsonObject& payload)
{
    const QJsonValue id = payload.value(QLatin1String(key::kId));
    if (!id.isString() || id.toString().isEmpty())
    {
        return Result<SensorSample>::fail(1, QStringLiteral("Telemetry frame has no device id"));
    }

    SensorSample sample;
    sample.deviceId = id.toString();

    const QJsonValue ts = payload.value(QLatin1String(key::kTimestamp));
    sample.timestamp = ts.isString() ? QDateTime::fromString(ts.toString(), Qt::ISODateWithMs)
                                     : QDateTime::currentDateTime();
    if (!sample.timestamp.isValid())
    {
        // A device with a wrong clock must not corrupt the time series.
        sample.timestamp = QDateTime::currentDateTime();
    }

    for (const char* field : {key::kTemperature, key::kVoltage, key::kCurrent,
                              key::kVibration, key::kRpm})
    {
        const QJsonValue value = payload.value(QLatin1String(field));
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
        {
            return Result<SensorSample>::fail(
                1, QStringLiteral("Telemetry field '%1' must be a finite number")
                       .arg(QLatin1String(field)));
        }
    }
    const auto number = [&payload](const char* field) -> double {
        const QJsonValue v = payload.value(QLatin1String(field));
        return v.isDouble() ? v.toDouble() : 0.0;
    };

    sample.temperature = number(key::kTemperature);
    sample.voltage     = number(key::kVoltage);
    sample.current     = number(key::kCurrent);
    sample.vibration   = number(key::kVibration);
    sample.rpm         = number(key::kRpm);

    const QJsonValue valid = payload.value(QLatin1String(key::kValid));
    sample.valid = valid.isBool() ? valid.toBool() : true;

    return Result<SensorSample>::ok(sample);
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

std::vector<Frame> ProtocolCodec::append(const QByteArray& chunk)
{
    std::vector<Frame> frames;
    // Bound the assembly buffer, not the socket read: one read may contain
    // arbitrarily many legal frames. Drain between bounded copies.
    constexpr qsizetype kMaxBufferBytes = kMaxPayloadBytes + kHeaderSize;
    qsizetype consumed = 0;
    while (consumed < chunk.size())
    {
        const qsizetype count = std::min(kMaxBufferBytes - m_buffer.size(),
                                         chunk.size() - consumed);
        m_buffer.append(chunk.constData() + consumed, count);
        consumed += count;
        decodeBuffered(frames);
    }
    return frames;
}

void ProtocolCodec::decodeBuffered(std::vector<Frame>& frames)
{
    int offset = 0;
    while (true)
    {
        const int available = static_cast<int>(m_buffer.size()) - offset;
        if (available < kHeaderSize)
        {
            break;   // header not complete yet
        }

        const char* header = m_buffer.constData() + offset;
        if (header[0] != kMagic0 || header[1] != kMagic1)
        {
            // Resynchronise: scan forward for the next plausible magic pair.
            const int next =
                static_cast<int>(m_buffer.indexOf(QByteArrayLiteral("SA"), offset + 1));
            ++m_discarded;
            SAIM_WARN(logcat::kNetwork,
                      QStringLiteral("Bad frame magic - dropping %1 byte(s) and resyncing")
                          .arg(next < 0 ? available : next - offset));
            if (next < 0)
            {
                // The next read may begin with 'A': preserve a trailing 'S'.
                const bool partialMagic = m_buffer.endsWith(kMagic0);
                m_buffer.remove(0, m_buffer.size() - (partialMagic ? 1 : 0));
                return;
            }
            offset = next;
            continue;
        }

        const quint8 version = static_cast<quint8>(header[2]);
        if (version != kVersion)
        {
            SAIM_ERROR(logcat::kNetwork,
                       QStringLiteral("Unsupported protocol version %1 (expected %2) - "
                                      "check that client and gateway builds match")
                           .arg(version)
                           .arg(kVersion));
            ++m_discarded;
            offset += kHeaderSize;   // skip this header; the length field is untrusted
            continue;
        }

        const quint32 payloadLength = readUint32(header + 4);
        if (payloadLength > kMaxPayloadBytes)
        {
            SAIM_ERROR(logcat::kNetwork,
                       QStringLiteral("Frame declares %1 payload bytes (max %2) - dropped")
                           .arg(payloadLength)
                           .arg(kMaxPayloadBytes));
            ++m_discarded;
            offset += kHeaderSize;
            continue;
        }

        const int total = kHeaderSize + static_cast<int>(payloadLength);
        if (available < total)
        {
            break;   // payload still arriving
        }

        const QByteArray body = m_buffer.mid(offset + kHeaderSize, static_cast<int>(payloadLength));
        offset += total;

        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            ++m_discarded;
            SAIM_WARN(logcat::kNetwork,
                      QStringLiteral("Malformed JSON payload at offset %1 (%2) - frame skipped")
                          .arg(parseError.offset)
                          .arg(parseError.errorString()));
            continue;
        }

        Frame frame;
        frame.type = static_cast<MessageType>(static_cast<quint8>(header[3]));
        frame.payload = document.object();
        frame.receivedAt = QDateTime::currentDateTime();
        frames.push_back(std::move(frame));
    }

    // Retain only the unconsumed tail.
    if (offset > 0)
    {
        m_buffer.remove(0, offset);
    }
}

void ProtocolCodec::reset()
{
    m_buffer.clear();
}

}   // namespace saim
