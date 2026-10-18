#include "TestHarness.h"
#include "device/DeviceProtocol.h"
#include <QCoreApplication>
#include <QJsonDocument>

using namespace saim;

namespace {
SensorSample sample()
{
    SensorSample s;
    s.deviceId = QStringLiteral("motor-测试");
    s.timestamp = QDateTime::fromString(QStringLiteral("2026-10-11T01:02:03.456Z"), Qt::ISODateWithMs);
    s.temperature = 50; s.voltage = 220; s.current = 8; s.vibration = 0.2; s.rpm = 1500;
    s.valid = true;
    return s;
}
QJsonObject payload()
{
    ProtocolCodec codec;
    return codec.append(ProtocolCodec::encodeSample(sample())).front().payload;
}
}

SAIM_TEST(every_possible_split_preserves_one_frame)
{
    const auto bytes = ProtocolCodec::encodeSample(sample());
    for (qsizetype split = 1; split < bytes.size(); ++split)
    {
        ProtocolCodec codec;
        SAIM_CHECK(codec.append(bytes.left(split)).empty());
        const auto frames = codec.append(bytes.mid(split));
        SAIM_CHECK(frames.size() == 1);
        const auto decoded = ProtocolCodec::decodeSample(frames.front().payload);
        SAIM_CHECK(decoded.isOk());
        SAIM_CHECK(decoded.value().deviceId == sample().deviceId);
        SAIM_CHECK(decoded.value().timestamp == sample().timestamp);
        SAIM_CHECK_NEAR(decoded.value().temperature, 50.0, 1e-12);
        SAIM_CHECK(codec.pendingBytes() == 0);
    }
}

SAIM_TEST(coalesced_valid_frames_can_exceed_single_frame_limit)
{
    const auto frame = ProtocolCodec::encode(MessageType::Heartbeat,
        QJsonObject{{QStringLiteral("padding"), QString(600000, QLatin1Char('x'))}});
    ProtocolCodec codec;
    SAIM_CHECK(codec.append(frame + frame).size() == 2);
    SAIM_CHECK(codec.pendingBytes() == 0);
    SAIM_CHECK(codec.discardedFrameCount() == 0);
}

SAIM_TEST(resynchronisation_preserves_split_magic_byte)
{
    const auto frame = ProtocolCodec::encodeSample(sample());
    ProtocolCodec codec;
    SAIM_CHECK(codec.append(QByteArray("garbage bytes") + frame.left(1)).empty());
    SAIM_CHECK(codec.pendingBytes() == 1);
    SAIM_CHECK(codec.append(frame.mid(1)).size() == 1);
}

SAIM_TEST(malformed_frames_do_not_poison_following_frames)
{
    const auto good = ProtocolCodec::encode(MessageType::Heartbeat, {});
    auto invalidJson = good;
    invalidJson[8] = '!';
    auto oversized = good.left(8);
    oversized[4] = char(0x7f);
    auto wrongVersion = good;
    wrongVersion[2] = char(2);
    ProtocolCodec codec;
    SAIM_CHECK(codec.append(invalidJson + oversized + wrongVersion + good).size() == 1);
    SAIM_CHECK(codec.discardedFrameCount() >= 3);
}

SAIM_TEST(telemetry_rejects_missing_and_non_numeric_measurements)
{
    for (const auto* key : {"temperature", "voltage", "current", "vibration", "rpm"})
    {
        auto p = payload();
        p.remove(QLatin1String(key));
        SAIM_CHECK(ProtocolCodec::decodeSample(p).isError());
        p.insert(QLatin1String(key), QStringLiteral("broken"));
        SAIM_CHECK(ProtocolCodec::decodeSample(p).isError());
        p.insert(QLatin1String(key), QJsonValue(QJsonValue::Null));
        SAIM_CHECK(ProtocolCodec::decodeSample(p).isError());
    }
}

SAIM_TEST(reset_discards_a_partial_connection_frame)
{
    const auto bytes = ProtocolCodec::encodeSample(sample());
    ProtocolCodec codec;
    SAIM_CHECK(codec.append(bytes.left(12)).empty());
    codec.reset();
    SAIM_CHECK(codec.pendingBytes() == 0);
    SAIM_CHECK(codec.append(bytes).size() == 1);
}

int main()
{
    QCoreApplication app(saim::test::argcRef(), saim::test::argvRef());
    return saim::test::runAll();
}
