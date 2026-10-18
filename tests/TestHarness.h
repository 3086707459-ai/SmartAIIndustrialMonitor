#pragma once

// ---------------------------------------------------------------------------
// TestHarness.h - the smallest thing that can honestly be called a test runner.
//
// Declared scope
// --------------
// ~120 lines, no dependencies, no build configuration, no fixture DSL, no
// mocking framework.  It does four things: register tests, run them in a stable
// order, print pass/fail with file and line, and set a non-zero exit code when
// anything failed so that CTest notices.
//
// Why not Qt Test (a deliberate trade-off, not an oversight)
// ---------------------------------------------------------
// Qt Test is the right tool for widget testing, and it *is* used for the tests
// that need a real QApplication and a real event loop - see that target's CMake
// file.  For pure logic it brings QTEST_MAIN, moc-generated test class machinery
// and a second build pass, in exchange for features this suite does not use.  A
// reviewer reading these tests should be able to see, in one file, exactly how a
// failure is reported - which is not true of a framework they have to look up.
//
// The trade-off is stated plainly because it is one an interviewer may well
// probe: the cost is no data-driven tests, no automatic QObject-signal
// spying, and no built-in benchmark.  Where those are needed, Qt Test is used
// instead rather than this being stretched to cover it.
//
// Usage
// -----
//     SAIM_TEST(a_device_warns_above_its_limit)
//     {
//         Device device(QStringLiteral("M1"));
//         SAIM_CHECK(device.state() == DeviceState::Offline);
//     }
//
//     int main() { return saim::test::runAll(); }
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace saim::test {

/// One registered test.
struct Case
{
    const char*         name;
    const char*         file;
    int                 line;
    std::function<void()> body;
};

/// The single registry.  A function-local static rather than a namespace-scope
/// global, so that registration order within a translation unit is guaranteed
/// and there is no static-initialisation-order question to answer.
inline std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

/// Thrown by a failing check.  An exception rather than a longjmp or a return
/// code, so that a check inside a helper function aborts the whole test rather
/// than just the helper.
struct Failure
{
    std::string message;
    std::string file;
    int         line = 0;
};

/// Registers a test.  The name is a string literal with the C++ identifier
/// spelled out, because a test's name is read in CI output far more often than
/// it is typed.
struct Registrar
{
    Registrar(const char* name, const char* file, int line, std::function<void()> body)
    {
        registry().push_back(Case{name, file, line, std::move(body)});
    }
};

/// Runs everything.  Returns 0 when all passed, 1 otherwise.
inline int runAll()
{
    QTextStream out(stdout);

    int passed = 0;
    int failed = 0;
    QStringList failures;
    const QStringList active = qEnvironmentVariable("SAIM_TEST_FILTER").split(
        QLatin1Char(','), Qt::SkipEmptyParts);
    int skipped = 0;

    for (const Case& test : registry())
    {
        const QString name = QString::fromUtf8(test.name);

        // A filter exists so that a failing test can be isolated without
        // recompiling.  Matching is a substring test, which is what is wanted
        // when the name is the file-and-behaviour sentence these are.
        bool selected = active.isEmpty();
        for (const QString& pattern : active)
        {
            if (name.contains(pattern, Qt::CaseInsensitive))
            {
                selected = true;
                break;
            }
        }
        if (!selected)
        {
            ++skipped;
            continue;
        }

        try
        {
            test.body();
            out << QStringLiteral("  PASS  %1").arg(name) << Qt::endl;
            ++passed;
        }
        catch (const Failure& failure)
        {
            out << QStringLiteral("  FAIL  %1").arg(name) << Qt::endl;
            out << QStringLiteral("        %1").arg(QString::fromStdString(failure.message))
                << Qt::endl;
            out << QStringLiteral("        at %1:%2")
                       .arg(QString::fromStdString(failure.file))
                       .arg(failure.line)
                << Qt::endl;
            failures << name;
            ++failed;
        }
        catch (const std::exception& error)
        {
            // An unexpected exception is a test bug or a genuine crash in the
            // code under test; either way it must be reported as a failure
            // rather than allowed to abort the run and hide the tests after it.
            out << QStringLiteral("  FAIL  %1  (unexpected exception: %2)")
                       .arg(name, QString::fromUtf8(error.what()))
                << Qt::endl;
            failures << name;
            ++failed;
        }
    }

    out << Qt::endl;
    out << QStringLiteral("%1 passed, %2 failed%3")
               .arg(passed)
               .arg(failed)
               .arg(skipped > 0 ? QStringLiteral(", %1 filtered out").arg(skipped) : QString())
        << Qt::endl;

    if (failed > 0)
    {
        out << QStringLiteral("Failing tests:") << Qt::endl;
        for (const QString& name : failures)
        {
            out << QStringLiteral("  - %1").arg(name) << Qt::endl;
        }
    }

    return failed == 0 ? 0 : 1;
}

/// A stable (argc, argv) pair for the tests that need a QCoreApplication.
///
/// Qt keeps the pointers for the lifetime of the application object, so passing
/// a temporary built at the call site is a dangling-pointer bug that happens to
/// work until it doesn't.  These hand back references to function-local statics:
/// the storage lives as long as the process, and the call site stays a one-liner
/// that reads exactly like the real thing.
///
///     int main()
///     {
///         QCoreApplication app(argcRef(), argvRef());
///         return runAll();
///     }
[[nodiscard]] inline int& argcRef()
{
    static int argc = 1;
    return argc;
}

[[nodiscard]] inline char** argvRef()
{
    // argv[argc] must be null, and argv[0] must stay writable because Qt is
    // permitted to rewrite it.
    static char  program[] = "saim_test";
    static char* argv[]    = {program, nullptr};
    return argv;
}

/// Compares two doubles with a tolerance, which is the comparison almost every
/// numerical test wants and which no test should be writing by hand.
[[nodiscard]] inline bool nearlyEqual(double lhs, double rhs, double tolerance)
{
    const double scale = std::max(1.0, std::max(std::abs(lhs), std::abs(rhs)));
    return std::abs(lhs - rhs) <= tolerance * scale;
}

}   // namespace saim::test

#define SAIM_TEST(name)                                                                       \
    static void saim_test_##name();                                                           \
    static const ::saim::test::Registrar saim_test_registrar_##name(                          \
        #name, __FILE__, __LINE__, &saim_test_##name);                                        \
    static void saim_test_##name()

#define SAIM_CHECK(condition)                                                                 \
    do                                                                                        \
    {                                                                                         \
        if (!(condition))                                                                     \
        {                                                                                     \
            throw ::saim::test::Failure{std::string("check failed: ") + #condition,           \
                                        __FILE__, __LINE__};                                  \
        }                                                                                     \
    } while (false)

#define SAIM_CHECK_MSG(condition, message)                                                    \
    do                                                                                        \
    {                                                                                         \
        if (!(condition))                                                                     \
        {                                                                                     \
            throw ::saim::test::Failure{std::string(message) + " (" + #condition + ")",       \
                                        __FILE__, __LINE__};                                  \
        }                                                                                     \
    } while (false)

#define SAIM_CHECK_NEAR(lhs, rhs, tolerance)                                                  \
    do                                                                                        \
    {                                                                                         \
        if (!::saim::test::nearlyEqual((lhs), (rhs), (tolerance)))                            \
        {                                                                                     \
            throw ::saim::test::Failure{                                                      \
                std::string("expected ") + #lhs + " ~= " + #rhs + " (tolerance " +            \
                    std::to_string(tolerance) + ")", __FILE__, __LINE__};                     \
        }                                                                                     \
    } while (false)

#define SAIM_CHECK_THROWS(expression)                                                         \
    do                                                                                        \
    {                                                                                         \
        bool threw = false;                                                                   \
        try                                                                                   \
        {                                                                                     \
            (void)(expression);                                                               \
        }                                                                                     \
        catch (...)                                                                           \
        {                                                                                     \
            threw = true;                                                                     \
        }                                                                                     \
        if (!threw)                                                                           \
        {                                                                                     \
            throw ::saim::test::Failure{std::string("expected a throw: ") + #expression,      \
                                        __FILE__, __LINE__};                                  \
        }                                                                                     \
    } while (false)
