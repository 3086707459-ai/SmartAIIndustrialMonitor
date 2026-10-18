#include "ai/AIInferenceEngine.h"

#include "core/Logger.h"

#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <deque>
#include <numeric>

// The guard is the whole point of this file: the ONNX Runtime headers are
// included exactly here and nowhere else in the project, and only when the
// library was actually found at configure time.
#ifdef SAIM_HAVE_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace saim {

#ifdef SAIM_HAVE_ONNX

// ===========================================================================
// Implementation with ONNX Runtime available
// ===========================================================================

namespace {

/// Maps ORT's element type enum onto a readable name for the diagnostics page.
std::string elementTypeName(ONNXTensorElementDataType type)
{
    switch (type)
    {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:  return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE: return "float64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:  return "int32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:  return "int64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:  return "uint8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:   return "bool";
    default:                                   return "other";
    }
}

/// ORT requires the log severity to be set before any other API call; the
/// Environment is therefore created once, process-wide, and never destroyed
/// before the last session is gone.  A leaked singleton is the correct answer
/// here: destroying it at static-destruction time races with session teardown.
Ort::Env& sharedEnvironment()
{
    static Ort::Env s_environment(ORT_LOGGING_LEVEL_WARNING, "SmartAIIndustrialMonitor");
    return s_environment;
}

}   // namespace

struct OnnxInferenceEngine::Impl
{
    explicit Impl(const Options& engineOptions) : options(engineOptions) {}

    Options options;

    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    // Cached at load() so the hot path does no string allocation.  Ort::Session
    // requires the names as `const char*` that outlive the Run() call, so they
    // are stored here rather than built per inference.
    std::vector<std::string>   inputNames;
    std::vector<std::string>   outputNames;
    std::vector<const char*>   inputNamePointers;
    std::vector<const char*>   outputNamePointers;

    std::vector<int64_t> inputShape;
    std::size_t inputElementCount = 0;

    /// Ring of recent latencies, for the median reported in the UI.
    std::deque<double> latenciesMicros;
    static constexpr std::size_t kLatencyWindow = 64;
};

OnnxInferenceEngine::OnnxInferenceEngine(Options options)
    : m_impl(std::make_unique<Impl>(options))
{
}

OnnxInferenceEngine::OnnxInferenceEngine()
    : OnnxInferenceEngine(Options{})
{
}

OnnxInferenceEngine::~OnnxInferenceEngine() = default;   // Impl is complete here

bool OnnxInferenceEngine::isCompiledIn()
{
    return true;
}

Status OnnxInferenceEngine::load(const QString& modelPath)
{
    const QFileInfo info(modelPath);
    if (!info.exists() || !info.isFile())
    {
        return Status::fail(1, QStringLiteral("Model file not found: %1").arg(modelPath));
    }

    SAIM_INFO(logcat::kAi,
              QStringLiteral("Loading ONNX model: %1 (%2 KiB)")
                  .arg(modelPath)
                  .arg(info.size() / 1024));

    try
    {
        Ort::SessionOptions sessionOptions;

        // Thread counts: see the comment on Options.  Keeping intra-op low is a
        // deliberate UI-responsiveness trade, not an oversight.
        sessionOptions.SetIntraOpNumThreads(std::max(1, m_impl->options.intraOpThreads));
        sessionOptions.SetInterOpNumThreads(std::max(1, m_impl->options.interOpThreads));
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (m_impl->options.useCpuArena)
        {
            // The arena avoids per-inference allocations, which is worth having
            // for a model called twice a second for the life of the process.
            sessionOptions.EnableCpuMemArena();
        }

        // Build the session into a local first: a malformed model must not
        // destroy the working session we may already have.
        auto candidate = std::make_unique<Ort::Session>(
            sharedEnvironment(), modelPath.toStdString().c_str(), sessionOptions);

        // Query the model's metadata for the diagnostics page.  Every value here
        // is something a deployment engineer may need to confirm at 2 a.m.
        Ort::ModelMetadata modelMetadata = candidate->GetModelMetadata();
        ModelMetadata metadata;
        metadata.filePath = modelPath;
        metadata.fileSizeBytes = info.size();

        const auto readString = [&modelMetadata](const char* key) -> QString {
            auto value = modelMetadata.LookupCustomMetadataMapAllocated(
                key, Ort::AllocatorWithDefaultOptions());
            return value != nullptr ? QString::fromStdString(value.get()) : QString{};
        };
        metadata.producer    = readString("producer");
        metadata.description = readString("description");

        // The opset is written into the model's custom metadata map by the
        // export script (scripts/export_onnx.py).  Reading it here is what lets
        // the diagnostics page prove that the .onnx in production is the one the
        // training pipeline produced.
        const QString opsetText = readString("opset");
        metadata.opsetVersion = opsetText.isEmpty() ? 0 : opsetText.toInt();

        const std::size_t inputCount = candidate->GetInputCount();
        for (std::size_t i = 0; i < inputCount; ++i)
        {
            auto name = candidate->GetInputNameAllocated(i, m_impl->allocator);
            auto typeInfo = candidate->GetInputTypeInfo(i);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

            TensorInfo entry;
            entry.name = name.get();
            entry.shape = tensorInfo.GetShape();
            entry.elementType = elementTypeName(tensorInfo.GetElementType());
            metadata.inputs.push_back(entry);

            m_impl->inputNames.emplace_back(name.get());
        }

        const std::size_t outputCount = candidate->GetOutputCount();
        for (std::size_t i = 0; i < outputCount; ++i)
        {
            auto name = candidate->GetOutputNameAllocated(i, m_impl->allocator);
            auto typeInfo = candidate->GetOutputTypeInfo(i);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

            TensorInfo entry;
            entry.name = name.get();
            entry.shape = tensorInfo.GetShape();
            entry.elementType = elementTypeName(tensorInfo.GetElementType());
            metadata.outputs.push_back(entry);

            m_impl->outputNames.emplace_back(name.get());
        }

        if (m_impl->inputNames.empty() || m_impl->outputNames.empty())
        {
            return Status::fail(2, QStringLiteral("Model %1 has no inputs or no outputs")
                                        .arg(modelPath));
        }

        // Rebuild the const char* arrays.  ORT does not copy the names, so these
        // must remain valid for as long as the session lives.
        m_impl->inputNamePointers.clear();
        m_impl->outputNamePointers.clear();
        for (const std::string& name : m_impl->inputNames)
        {
            m_impl->inputNamePointers.push_back(name.c_str());
        }
        for (const std::string& name : m_impl->outputNames)
        {
            m_impl->outputNamePointers.push_back(name.c_str());
        }

        // Input shape.  A dynamic first dimension (-1) means the model accepts a
        // batch; we always run batch 1, so the element count comes from the
        // remaining dimensions.
        m_impl->inputShape = metadata.inputs.front().shape;
        if (m_impl->inputShape.empty())
        {
            return Status::fail(3, QStringLiteral("Model %1 declares an empty input shape")
                                        .arg(modelPath));
        }

        m_impl->inputElementCount = 1;
        for (std::size_t i = 0; i < m_impl->inputShape.size(); ++i)
        {
            const int64_t dimension = m_impl->inputShape[i];
            if (dimension <= 0)
            {
                if (i == 0)
                {
                    m_impl->inputShape[i] = 1;   // we always use batch size 1
                    metadata.supportsBatch = true;
                    m_impl->inputElementCount *= 1;
                    continue;
                }
                return Status::fail(4, QStringLiteral("Model %1 has a dynamic non-batch dimension")
                                            .arg(modelPath));
            }
            m_impl->inputElementCount *= static_cast<std::size_t>(dimension);
        }

        // Commit only now that everything succeeded.
        m_impl->session = std::move(candidate);
        m_metadata = metadata;
        m_impl->latenciesMicros.clear();

        SAIM_INFO(logcat::kAi,
                  QStringLiteral("ONNX model loaded: input '%1' shape [%2], %3 output(s), "
                                 "%4 parameters per inference")
                      .arg(QString::fromStdString(m_metadata.inputs.front().name),
                           [&metadata] {
                               QStringList dims;
                               for (const int64_t d : metadata.inputs.front().shape)
                               {
                                   dims << QString::number(d);
                               }
                               return dims.join(QLatin1Char(','));
                           }())
                      .arg(m_metadata.outputs.size())
                      .arg(m_impl->inputElementCount));

        return Status::ok();
    }
    catch (const Ort::Exception& exception)
    {
        // ORT reports everything through exceptions.  They must not escape this
        // function: the worker's slot is invoked from the event loop, and an
        // exception unwinding out of a Qt slot is undefined behaviour.
        return Status::fail(5, QStringLiteral("ONNX Runtime error loading %1: %2")
                                    .arg(modelPath, QString::fromUtf8(exception.what())));
    }
}

bool OnnxInferenceEngine::isReady() const
{
    return m_impl->session != nullptr;
}

const ModelMetadata& OnnxInferenceEngine::metadata() const
{
    return m_metadata;
}

Result<std::vector<float>> OnnxInferenceEngine::run(const std::vector<float>& input)
{
    if (m_impl->session == nullptr)
    {
        return Result<std::vector<float>>::fail(1, QStringLiteral("no model loaded"));
    }

    if (input.size() != m_impl->inputElementCount)
    {
        // A shape mismatch at run time is a programming error, and ORT's own
        // message for it is unhelpful.  Catch it here with the actual numbers.
        return Result<std::vector<float>>::fail(
            2, QStringLiteral("input size mismatch: model expects %1, got %2")
                   .arg(m_impl->inputElementCount)
                   .arg(input.size()));
    }

    try
    {
        const auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            const_cast<float*>(input.data()),   // ORT does not mutate the input
            input.size(),
            m_impl->inputShape.data(),
            m_impl->inputShape.size());

        const auto start = std::chrono::steady_clock::now();

        std::vector<Ort::Value> outputs = m_impl->session->Run(
            Ort::RunOptions{nullptr},
            m_impl->inputNamePointers.data(), &inputTensor, 1,
            m_impl->outputNamePointers.data(), m_impl->outputNamePointers.size());

        const auto end = std::chrono::steady_clock::now();
        const double micros = std::chrono::duration<double, std::micro>(end - start).count();
        m_impl->latenciesMicros.push_back(micros);
        if (m_impl->latenciesMicros.size() > Impl::kLatencyWindow)
        {
            m_impl->latenciesMicros.pop_front();
        }

        if (outputs.empty() || !outputs.front().IsTensor())
        {
            return Result<std::vector<float>>::fail(3, QStringLiteral("model returned no tensor"));
        }

        const float* raw = outputs.front().GetTensorData<float>();
        const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
        std::size_t elementCount = 1;
        for (const int64_t dimension : shape)
        {
            elementCount *= static_cast<std::size_t>(std::max<int64_t>(1, dimension));
        }

        return Result<std::vector<float>>::ok(std::vector<float>(raw, raw + elementCount));
    }
    catch (const Ort::Exception& exception)
    {
        return Result<std::vector<float>>::fail(
            4, QStringLiteral("ONNX Runtime error during inference: %1")
                   .arg(QString::fromUtf8(exception.what())));
    }
}

void OnnxInferenceEngine::warmUp(int iterations)
{
    if (m_impl->session == nullptr)
    {
        return;
    }

    // A zero input is fine for warming: it exercises the same allocation and
    // kernel-selection paths as a real one.  Doing this at start-up moves the
    // 50-100 ms first-call cost out of the operator's first interaction.
    std::vector<float> dummy(m_impl->inputElementCount, 0.0f);
    for (int i = 0; i < std::max(1, iterations); ++i)
    {
        const auto result = run(dummy);
        if (result.isError())
        {
            SAIM_WARN(logcat::kAi,
                      QStringLiteral("Warm-up inference failed: %1").arg(result.error().message));
            return;
        }
    }
    m_impl->latenciesMicros.clear();   // warm-up timings are not representative
    SAIM_INFO(logcat::kAi,
              QStringLiteral("ONNX engine warmed up (%1 iterations)").arg(iterations));
}

double OnnxInferenceEngine::medianLatencyMicros() const
{
    if (m_impl->latenciesMicros.empty())
    {
        return 0.0;
    }
    std::vector<double> sorted(m_impl->latenciesMicros.begin(), m_impl->latenciesMicros.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
}

const char* OnnxInferenceEngine::backendName() const
{
    return "onnxruntime";
}

#else   // !SAIM_HAVE_ONNX

// ===========================================================================
// Stub implementation - the project builds and runs without ONNX Runtime.
//
// Every method reports "not available" rather than pretending to work.  A silent
// fake here would be far worse than no engine at all: the dashboard would show
// model output that came from nowhere.
// ===========================================================================

struct OnnxInferenceEngine::Impl
{
    explicit Impl(const Options& engineOptions) : options(engineOptions) {}
    Options options;
};

OnnxInferenceEngine::OnnxInferenceEngine(Options options)
    : m_impl(std::make_unique<Impl>(options))
{
}

OnnxInferenceEngine::OnnxInferenceEngine()
    : OnnxInferenceEngine(Options{})
{
}

OnnxInferenceEngine::~OnnxInferenceEngine() = default;

bool OnnxInferenceEngine::isCompiledIn()
{
    return false;
}

Status OnnxInferenceEngine::load(const QString& modelPath)
{
    Q_UNUSED(modelPath)
    return Status::fail(1, QStringLiteral(
        "This build was compiled without ONNX Runtime. Install it and reconfigure with "
        "-DONNXRuntime_ROOT=<path> (or -DSAIM_WITH_ONNX=OFF to silence this message). "
        "FaultPredictor will use its built-in physics model instead."));
}

bool OnnxInferenceEngine::isReady() const { return false; }

const ModelMetadata& OnnxInferenceEngine::metadata() const { return m_metadata; }

Result<std::vector<float>> OnnxInferenceEngine::run(const std::vector<float>& input)
{
    Q_UNUSED(input)
    return Result<std::vector<float>>::fail(1, QStringLiteral("ONNX Runtime not compiled in"));
}

void OnnxInferenceEngine::warmUp(int iterations) { Q_UNUSED(iterations) }

double OnnxInferenceEngine::medianLatencyMicros() const { return 0.0; }

const char* OnnxInferenceEngine::backendName() const { return "unavailable"; }

#endif   // SAIM_HAVE_ONNX

// ---------------------------------------------------------------------------
// EngineFactory
// ---------------------------------------------------------------------------

std::unique_ptr<IInferenceEngine> EngineFactory::createOnnx(
    const OnnxInferenceEngine::Options& options, QString* unavailableReason)
{
#ifdef SAIM_HAVE_ONNX
    if (unavailableReason != nullptr)
    {
        unavailableReason->clear();
    }
    return std::make_unique<OnnxInferenceEngine>(options);
#else
    // The parameter is genuinely unused on this branch - there is no engine to
    // configure.  Named in a cast rather than commented out so the signature
    // stays identical to the ONNX build: a caller must not have to change when
    // the dependency appears.
    Q_UNUSED(options)

    if (unavailableReason != nullptr)
    {
        *unavailableReason = QStringLiteral(
            "ONNX Runtime was not found at configure time; using the built-in model.");
    }
    return nullptr;
#endif
}

QStringList EngineFactory::availableProviders()
{
    QStringList providers;
#ifdef SAIM_HAVE_ONNX
    providers << QStringLiteral("CPUExecutionProvider");
    // The available list would be queried from Ort::GetAvailableProviders() in a
    // build that shipped a GPU package; the CPU provider is the one this project
    // deploys against, and naming only what is actually used avoids a misleading
    // diagnostics page.
#else
    providers << QStringLiteral("none (ONNX Runtime not compiled in)");
#endif
    return providers;
}

}   // namespace saim
