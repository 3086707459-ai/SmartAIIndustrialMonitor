#pragma once

// ---------------------------------------------------------------------------
// Result<T> - a minimal expected-like type.
//
// Rationale: the project must handle failure without exceptions crossing module
// boundaries (Qt's own API is exception-free, and a signal/slot body that throws
// is undefined behaviour in a queued connection).  Layered code therefore
// returns Result<T> from anything that can fail, and only main() / the UI edge
// converts a failure into a user-visible message.
// ---------------------------------------------------------------------------

#include <QString>

#include <utility>
#include <variant>

namespace saim {

/// Error payload carried by a failed Result.
struct Error
{
    int     code    = -1;
    QString message;

    [[nodiscard]] bool isEmpty() const noexcept { return code == 0 && message.isEmpty(); }
};

/// Lightweight discriminated union of `T` and `Error`.
template <typename T>
class Result
{
public:
    Result(T value) : m_data(std::move(value)) {}                       // NOLINT(google-explicit-constructor)
    Result(Error error) : m_data(std::move(error)) {}                   // NOLINT(google-explicit-constructor)

    [[nodiscard]] static Result ok(T value) { return Result{std::move(value)}; }
    [[nodiscard]] static Result fail(int code, QString message)
    {
        return Result{Error{code, std::move(message)}};
    }

    [[nodiscard]] bool isOk() const noexcept { return std::holds_alternative<T>(m_data); }
    [[nodiscard]] bool isError() const noexcept { return !isOk(); }

    /// Precondition: isOk().  Calling on an error is a programming mistake and
    /// throws std::bad_variant_access, which the test suite asserts on.
    [[nodiscard]] const T& value() const { return std::get<T>(m_data); }
    [[nodiscard]] T&       value() { return std::get<T>(m_data); }

    [[nodiscard]] const Error& error() const { return std::get<Error>(m_data); }

    /// Value if present, otherwise `fallback` - lets callers keep a hot path
    /// branch-free.
    [[nodiscard]] T valueOr(T fallback) const
    {
        return isOk() ? std::get<T>(m_data) : std::move(fallback);
    }

private:
    std::variant<T, Error> m_data;
};

/// Specialisation for operations that either succeed or fail with no payload.
template <>
class Result<void>
{
public:
    Result() = default;                                                 // success
    Result(Error error) : m_error(std::move(error)) {}                  // NOLINT(google-explicit-constructor)

    [[nodiscard]] static Result ok() { return Result{}; }
    [[nodiscard]] static Result fail(int code, QString message)
    {
        return Result{Error{code, std::move(message)}};
    }

    [[nodiscard]] bool isOk() const noexcept { return m_error.isEmpty(); }
    [[nodiscard]] bool isError() const noexcept { return !isOk(); }
    [[nodiscard]] const Error& error() const noexcept { return m_error; }

private:
    Error m_error{0, {}};
};

/// Convenience alias for the common "worked or not" case.
using Status = Result<void>;

}   // namespace saim
