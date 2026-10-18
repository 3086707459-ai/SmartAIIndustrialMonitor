#pragma once

// ---------------------------------------------------------------------------
// DeviceProtocol.h - the wire protocol between the acquisition client and the
// plant's device gateway.
//
// Why frame at all
// ----------------
// TCP is a *byte stream*, not a message stream.  A single read() may deliver
// half a message, or three messages concatenated.  Any code that does
// `socket->readAll()` and then `QJsonDocument::fromJson()` on the result will
// appear to work in the lab and corrupt data on a loaded network.  The decoder
// below therefore maintains an internal assembly buffer and only ever emits
// complete frames - this is the single most common bug in homemade TCP clients;
//
// Wire format (all integers big-endian, i.e. network byte order):
//
//     +--------+--------+--------+--------+-------------------+
//     |  'S'   |  'A'   |  ver   |  type  |  payloadLength    |  (8 bytes)
//     +--------+--------+--------+--------+--------+----------+
//     |  payloadLength (uint32, big endian)  |  payload ...   |
//     +--------------------------------------+----------------+
//
//   payload = UTF-8 encoded JSON object
//
// A magic number catches a client pointed at the wrong port; the explicit
// version byte lets a future protocol revision be rejected cleanly instead of
// mis-parsed; the length prefix is bounded so a hostile or buggy peer cannot
// make the client allocate gigabytes.
// ---------------------------------------------------------------------------

#include "common/Result.h"
#include "common/Types.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace saim {

enum class MessageType : quint8
{
    Telemetry  = 0x01,   ///< device -> server: a SensorSample
    Heartbeat  = 0x02,   ///< both directions: liveness probe
    Register   = 0x03,   ///< device -> server: announce identity
    Command    = 0x04,   ///< server -> device: e.g. "set sampling rate"
    Acknowledge= 0x05,
    Alarm      = 0x06,
    Error      = 0x07,
    Snapshot   = 0x08    ///< server -> client: bulk replay after reconnect
};

[[nodiscard]] QString toString(MessageType type);

/// One decoded protocol frame.
struct Frame
{
    MessageType type = MessageType::Heartbeat;
    QJsonObject payload;
    QDateTime   receivedAt;
};

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

class ProtocolCodec
{
public:
    static constexpr char     kMagic0 = 'S';
    static constexpr char     kMagic1 = 'A';
    static constexpr quint8   kVersion = 1;
    static constexpr int      kHeaderSize = 8;
    /// 1 MiB.  A sensor frame is ~200 bytes; anything larger is a bug or an attack.
    static constexpr quint32  kMaxPayloadBytes = 1024 * 1024;

    // -- encoding -----------------------------------------------------------

    [[nodiscard]] static QByteArray encode(MessageType type, const QJsonObject& payload);

    /// Convenience: a Telemetry frame from a domain sample.
    [[nodiscard]] static QByteArray encodeSample(const SensorSample& sample);

    /// Convenience: decode a Telemetry payload into a domain sample.
    [[nodiscard]] static Result<SensorSample> decodeSample(const QJsonObject& payload);

    // -- decoding -----------------------------------------------------------

    /// Appends raw socket bytes to the assembly buffer and returns every frame
    /// that became complete.  Retains any trailing partial frame for the next
    /// call, so calling this once per readyRead() is correct.
    ///
    /// A frame that fails to parse is skipped (and counted) rather than
    /// aborting the stream: one corrupt frame must not kill a live connection.
    [[nodiscard]] std::vector<Frame> append(const QByteArray& chunk);

    /// Number of malformed frames discarded since construction.
    [[nodiscard]] quint64 discardedFrameCount() const noexcept { return m_discarded; }

    /// Bytes currently held awaiting completion - surfaced in diagnostics.
    [[nodiscard]] int pendingBytes() const noexcept { return static_cast<int>(m_buffer.size()); }

    void reset();

private:
    void decodeBuffered(std::vector<Frame>& frames);
    QByteArray m_buffer;
    quint64    m_discarded = 0;
};

}   // namespace saim
