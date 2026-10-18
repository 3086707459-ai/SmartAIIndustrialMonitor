#pragma once

// ---------------------------------------------------------------------------
// ErrorCode.h - the small, fixed vocabulary of failure kinds.
//
// Why an enum rather than just a message string
// --------------------------------------------
// A message is for the operator; a code is for the code.  The UI needs to react
// differently to "the gateway is unreachable" (offer a retry, keep buffering)
// than to "the model file is corrupt" (offer to reload it) - and it must do that
// without pattern-matching on human-readable text, which changes the moment
// somebody improves the wording.
//
// The values are stable integers because failures travel through queued signals
// and are eventually persisted; renumbering them would silently reinterpret
// historical rows.
// ---------------------------------------------------------------------------

namespace saim {

enum class ErrorCode : int
{
    Ok = 0,

    // -- generic -------------------------------------------------------------
    Unknown = 1,
    InvalidArgument = 2,
    NotInitialised = 3,
    NotSupported = 4,
    Timeout = 5,

    // -- I/O and storage -----------------------------------------------------
    IoError = 100,
    FileNotFound = 101,
    PermissionDenied = 102,
    DatabaseOpenFailed = 110,
    DatabaseQueryFailed = 111,
    DatabaseMigrationFailed = 112,

    // -- network -------------------------------------------------------------
    NetworkUnreachable = 200,
    ConnectionRefused = 201,
    ConnectionLost = 202,
    ProtocolViolation = 203,

    // -- AI ------------------------------------------------------------------
    ModelNotFound = 300,
    ModelLoadFailed = 301,
    ModelInferenceFailed = 302,
    ModelShapeMismatch = 303,
    InsufficientData = 304,   ///< window too short to predict - not a fault
    BackendUnavailable = 305
};

/// Stable, human-readable name for a code.  Used in logs and in the diagnostics
/// panel; never parsed.
[[nodiscard]] const char* toString(ErrorCode code);

}   // namespace saim
