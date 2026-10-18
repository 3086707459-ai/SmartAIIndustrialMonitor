# ---------------------------------------------------------------------------
# tests/unit/CMakeLists.txt
#
# One executable per *layer*, not one per class.  The rule that makes this
# possible is the layered build itself: a test only has to link the layer under
# test and whatever sits below it.  The moment a "unit" test needs a target
# above its subject, the layering has been broken and the link list below is
# where the violation shows up - which is exactly when we want to find out.
#
# Note what is *absent*: no Qt6::Widgets, no Qt6::Sql, no Qt6::Network in most
# of these.  A test that pulls in the widget layer to exercise a ring buffer is
# a test that no longer tells you whether the ring buffer is right.
# ---------------------------------------------------------------------------

# -- common: value types, containers, the rule-based envelope -----------------
saim_add_unit_test(test_common
    SOURCES test_common.cpp
    LIBS    saim::common
)

# -- device: the state machine, alarms, liveness, history ---------------------
saim_add_unit_test(test_device
    SOURCES test_device.cpp
    LIBS    saim::device saim::core Qt6::Test
)

# -- ai: feature extraction, anomaly detection, the predictor fallback --------
saim_add_unit_test(test_ai
    SOURCES test_ai.cpp
    LIBS    saim::ai saim::common saim::core
)

# -- protocol: framing, escaping, round-trip, malformed input -----------------
#
# This one genuinely needs Qt6::Network because DeviceProtocol encodes and
# decodes over QByteArray streams, and because the framing is the contract the
# simulator and the real gateway both honour.
saim_add_unit_test(test_protocol
    SOURCES test_protocol.cpp
    LIBS    saim::network saim::device saim::core Qt6::Network
)

# -- database: schema creation, migrations, retention, io errors --------------
#
# QSQLITE, not the real file: a unit test for the repository must not depend on
# a writable path, and the SQLite driver is in-process.
saim_add_unit_test(test_database
    SOURCES test_database.cpp
    LIBS    saim::database saim::core Qt6::Sql
)

saim_add_unit_test(test_thread
    SOURCES test_thread.cpp
    LIBS saim::thread
)

saim_add_unit_test(test_backpressure
    SOURCES test_backpressure.cpp
    LIBS saim::thread Qt6::Sql
)
