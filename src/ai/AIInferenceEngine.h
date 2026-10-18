#pragma once

// ---------------------------------------------------------------------------
// AIInferenceEngine.h - the ONNX Runtime boundary.
//
// The single rule this class exists to enforce
// -------------------------------------------
// *Nothing else in the code base includes onnxruntime_cxx_api.h.*  Everything
// above this line speaks in `std::vector<float>` in and `std::vector<float>`
// out.  Two things follow:
//
//   * Build portability.  ONNX Runtime is an optional dependency here.  If it is
//     not installed, this file still compiles and the engine reports itself
//     unavailable; FaultPredictor then uses its built-in model.  A reviewer
//     cloning the repository gets a running application either way, which is the
//     difference between a project that can be evaluated and one that cannot.
//
//   * Testability.  A fake engine can be substituted in a unit test, so the
//     predictor's logic can be tested without a model file.  The engine is
//     therefore an interface, not a concrete class.
//
// Threading
// ---------
// An Ort::Session is **not** thread safe and must not be shared across threads.
// This project runs inference on a dedicated worker thread per engine instance;
// the thread-affinity is documented here and enforced by the AIWorker, which
// owns one engine and never lets it escape.
//
// Sessions are also expensive to create (tens of milliseconds, plus file I/O),
// so the engine creates them once at start-up rather than per inference, and
// `warmUp()` runs a dummy inference to force the memory allocator to settle -
// otherwise the *first* real inference pays the cost and can take 50x longer
// than the steady state, which shows up as a visible stall in the UI.
// ---------------------------------------------------------------------------

#include "common/Result.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

namespace saim {

/// One input or output tensor description, surfaced for diagnostics.
struct TensorInfo
{
    std::string name;
    std::vector<int64_t> shape;
    std::string elementType;   ///< "float32", "int64", ...
};

/// Metadata about a loaded model, shown on the AI diagnostics page.  Being able
/// to state exactly which model file, which opset and which input shape is in
/// production is a deployment requirement, not a nicety.
struct ModelMetadata
{
    QString  filePath;
    QString  producer;
    QString  description;
    int      opsetVersion = 0;
    qint64   fileSizeBytes = 0;
    std::vector<TensorInfo> inputs;
    std::vector<TensorInfo> outputs;
    bool     supportsBatch = false;
};

/// Abstract inference backend.
class IInferenceEngine
{
public:
    virtual ~IInferenceEngine() = default;

    /// Loads a model from disk.  Safe to call again to hot-swap a model - the
    /// old session is released only after the new one loaded successfully, so a
    /// bad model file cannot leave the engine without one.
    [[nodiscard]] virtual Status load(const QString& modelPath) = 0;

    [[nodiscard]] virtual bool isReady() const = 0;
    [[nodiscard]] virtual const ModelMetadata& metadata() const = 0;

    /// Single-input, single-output float inference - the shape every model in
    /// this project uses.  Returns the flattened output tensor.
    [[nodiscard]] virtual Result<std::vector<float>> run(const std::vector<float>& input) = 0;

    /// Runs `iterations` dummy inferences so that the first real call is not the
    /// one that pays for allocation.
    virtual void warmUp(int iterations = 3) = 0;

    /// Median latency over the last N inferences, in microseconds.
    [[nodiscard]] virtual double medianLatencyMicros() const = 0;
    [[nodiscard]] virtual const char* backendName() const = 0;
};

// ---------------------------------------------------------------------------
// OnnxInferenceEngine - the real thing, when ONNX Runtime is available.
// ---------------------------------------------------------------------------

class OnnxInferenceEngine final : public IInferenceEngine
{
public:
    struct Options
    {
        /// 0 = let ONNX Runtime pick (it uses all physical cores).
        /// Deliberately left at a low value in this project: the engine shares a
        /// machine with the UI, and saturating every core makes the *interface*
        /// stutter even though inference itself gets faster.  On an industrial
        /// HMI, a responsive screen beats a marginally faster prediction.
        int intraOpThreads = 2;
        /// 1: this application runs one inference at a time per session, so
        /// parallel graph execution buys nothing and costs memory.
        int interOpThreads = 1;
        bool useCpuArena = true;
        /// ORT_LOG_LEVEL: 3 = warnings and above.  Verbose logging on a plant PC
        /// fills the disk and slows inference measurably.
        int logSeverityLevel = 3;
        /// "cpu" today; a deployment with a GPU would pass "cuda" or "dml".
        QString executionProvider = QStringLiteral("cpu");
    };

    /// The two overloads exist because GCC rejects a default argument of the
    /// form `Options options = {}` for a nested class whose members carry
    /// default member initialisers: those initialisers are not yet complete at
    /// the point the default argument is parsed.  Delegating from the no-arg
    /// overload in the .cpp sidesteps that without changing the call syntax.
    ///
    /// The alternative - hoisting Options out of the class - would also work,
    /// but it would put four backend-specific structs into namespace scope,
    /// where they no longer read as belonging to their engine.
    explicit OnnxInferenceEngine(Options options);
    OnnxInferenceEngine();
    ~OnnxInferenceEngine() override;

    OnnxInferenceEngine(const OnnxInferenceEngine&)            = delete;
    OnnxInferenceEngine& operator=(const OnnxInferenceEngine&) = delete;

    [[nodiscard]] Status load(const QString& modelPath) override;
    [[nodiscard]] bool isReady() const override;
    [[nodiscard]] const ModelMetadata& metadata() const override;
    [[nodiscard]] Result<std::vector<float>> run(const std::vector<float>& input) override;
    void warmUp(int iterations = 3) override;
    [[nodiscard]] double medianLatencyMicros() const override;
    [[nodiscard]] const char* backendName() const override;

    /// Whether the build actually linked ONNX Runtime.  Checked once at
    /// start-up so the UI can say "ONNX Runtime not compiled in" instead of
    /// silently falling back.
    [[nodiscard]] static bool isCompiledIn();

private:
    /// Pimpl: the ORT headers never appear in this header, so a translation unit
    /// that includes AIInferenceEngine.h does not need ONNX Runtime on its
    /// include path - which is what keeps the optional dependency optional.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    ModelMetadata m_metadata;
};

// ---------------------------------------------------------------------------
// EngineFactory
// ---------------------------------------------------------------------------

class EngineFactory
{
public:
    /// Returns an ONNX engine when the library is available and enabled,
    /// otherwise nullptr.  Callers must handle nullptr - that is the documented
    /// contract, not an error path.
    [[nodiscard]] static std::unique_ptr<IInferenceEngine> createOnnx(
        const OnnxInferenceEngine::Options& options = {},
        QString* unavailableReason = nullptr);

    /// The names of every execution provider this build can offer, for the
    /// diagnostics page.
    [[nodiscard]] static QStringList availableProviders();
};

}   // namespace saim
