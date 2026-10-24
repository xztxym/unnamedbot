#include "../include/KokoroTts.h"
#include "../include/Logger.h"
#include "../include/Utils.h"

#include "../ThirdParty/kokoro/onnxruntime_c_api.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>

namespace UnnamedBot {

    namespace {

        // ---- espeak-ng 动态入口 ----
        using EspeakInitializeFn = int (*)(int output, int buflength, const char* path, int options);
        using EspeakSetVoiceByNameFn = int (*)(const char* name);
        using EspeakTextToPhonemesFn = const char* (*)(const void** textptr, int textmode, int phonememode);
        using EspeakTerminateFn = int (*)(void);

        struct EspeakApi {
            EspeakInitializeFn initialize = nullptr;
            EspeakSetVoiceByNameFn setVoice = nullptr;
            EspeakTextToPhonemesFn toPhonemes = nullptr;
            EspeakTerminateFn terminate = nullptr;
            bool ok() const { return initialize && setVoice && toPhonemes; }
        };

        EspeakApi g_espeak;
        constexpr int kEspeakCharsUtf8 = 1;
        constexpr int kEspeakPhonemesIpa = 0x02;

        // ---- Kokoro 词表 ----
        struct KokoroVocabEntry {
            unsigned int codepoint;
            int id;
        };

#include "KokoroVocab.inc"

        int VocabId(char32_t cp) {
            for (const auto& e : kKokoroVocab) {
                if (e.codepoint == static_cast<unsigned int>(cp)) return e.id;
            }
            return -1;
        }

        // UTF-8 → 码点；非法字节返回 0 并前进一步
        char32_t NextUtf8(const std::string& s, size_t& i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) { ++i; return c; }
            int extra = 0;
            char32_t cp = 0;
            if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
            else { ++i; return 0; }
            if (i + extra >= s.size()) { i = s.size(); return 0; }
            for (int k = 1; k <= extra; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) { ++i; return 0; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            i += extra + 1;
            return cp;
        }

        std::string WsToUtf8(const std::wstring& w) { return Utils::WideToUtf8(w); }

    } // namespace

    KokoroTts& KokoroTts::Instance() {
        static KokoroTts instance;
        return instance;
    }

    KokoroTts::~KokoroTts() {
        Shutdown();
    }

    void KokoroTts::Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        const OrtApi* api = static_cast<const OrtApi*>(api_);
        if (api) {
            if (session_) { api->ReleaseSession(static_cast<OrtSession*>(session_)); session_ = nullptr; }
            if (memoryInfo_) { api->ReleaseMemoryInfo(static_cast<OrtMemoryInfo*>(memoryInfo_)); memoryInfo_ = nullptr; }
            if (env_) { api->ReleaseEnv(static_cast<OrtEnv*>(env_)); env_ = nullptr; }
        }
        available_ = false;
        if (g_espeak.terminate) g_espeak.terminate();
        if (espeakDll_) { FreeLibrary(static_cast<HMODULE>(espeakDll_)); espeakDll_ = nullptr; }
        if (ortDll_) { FreeLibrary(static_cast<HMODULE>(ortDll_)); ortDll_ = nullptr; }
        std::memset(&g_espeak, 0, sizeof(g_espeak));
        currentRows_.clear();
        currentRowCount_ = 0;
        currentVoice_.clear();
    }

    bool KokoroTts::Initialize(const std::wstring& engineDir) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (available_) return true;

        engineDir_ = engineDir;
        namespace fs = std::filesystem;
        std::error_code ec;
        modelPath_ = WsToUtf8((fs::path(engineDir) / L"model.onnx").wstring());
        voicesDir_ = WsToUtf8((fs::path(engineDir) / L"voices").wstring());
        const std::wstring ortPath = (fs::path(engineDir) / L"onnxruntime.dll").wstring();
        const std::wstring espeakPath = (fs::path(engineDir) / L"espeak-ng.dll").wstring();
        const std::wstring dataPath = (fs::path(engineDir) / L"espeak-ng-data").wstring();

        if (!fs::exists(modelPath_, ec)) {
            LOG_WARN("Kokoro", "model.onnx not found: " + modelPath_);
            return false;
        }

        // ---- onnxruntime ----
        ortDll_ = LoadLibraryW(ortPath.c_str());
        if (!ortDll_) {
            LOG_WARN("Kokoro", "cannot load onnxruntime.dll (err=" + std::to_string(GetLastError()) + ")");
            return false;
        }
        using OrtGetApiBaseFn = const OrtApiBase* (*)();
        auto getApiBase = reinterpret_cast<OrtGetApiBaseFn>(
            GetProcAddress(static_cast<HMODULE>(ortDll_), "OrtGetApiBase"));
        if (!getApiBase) {
            LOG_WARN("Kokoro", "OrtGetApiBase not exported by onnxruntime.dll");
            return false;
        }
        const OrtApi* api = getApiBase()->GetApi(ORT_API_VERSION);
        if (!api) {
            LOG_WARN("Kokoro", "onnxruntime API version mismatch");
            return false;
        }
        api_ = const_cast<OrtApi*>(api);

        OrtEnv* env = nullptr;
        if (api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "kokoro", &env) != nullptr) { api_ = nullptr; return false; }
        env_ = env;

        OrtSessionOptions* opts = nullptr;
        api->CreateSessionOptions(&opts);
        api->SetIntraOpNumThreads(opts, 4);
        api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);

        OrtSession* session = nullptr;
        std::wstring modelW = (fs::path(engineDir) / L"model.onnx").wstring();
        if (api->CreateSession(env, modelW.c_str(), opts, &session) != nullptr) {
            api->ReleaseSessionOptions(opts);
            LOG_WARN("Kokoro", "CreateSession failed for " + modelPath_);
            return false;
        }
        api->ReleaseSessionOptions(opts);
        session_ = session;

        OrtMemoryInfo* memInfo = nullptr;
        if (api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo) != nullptr) {
            LOG_WARN("Kokoro", "CreateCpuMemoryInfo failed");
            return false;
        }
        memoryInfo_ = memInfo;

        // ---- espeak-ng ----
        espeakDll_ = LoadLibraryW(espeakPath.c_str());
        if (!espeakDll_) {
            LOG_WARN("Kokoro", "cannot load espeak-ng.dll (err=" + std::to_string(GetLastError()) + ")");
            return false;
        }
        g_espeak.initialize = reinterpret_cast<EspeakInitializeFn>(
            GetProcAddress(static_cast<HMODULE>(espeakDll_), "espeak_Initialize"));
        g_espeak.setVoice = reinterpret_cast<EspeakSetVoiceByNameFn>(
            GetProcAddress(static_cast<HMODULE>(espeakDll_), "espeak_SetVoiceByName"));
        g_espeak.toPhonemes = reinterpret_cast<EspeakTextToPhonemesFn>(
            GetProcAddress(static_cast<HMODULE>(espeakDll_), "espeak_TextToPhonemes"));
        g_espeak.terminate = reinterpret_cast<EspeakTerminateFn>(
            GetProcAddress(static_cast<HMODULE>(espeakDll_), "espeak_Terminate"));
        if (!g_espeak.ok()) {
            LOG_WARN("Kokoro", "espeak-ng.dll missing required exports");
            return false;
        }
        const std::string dataUtf8 = WsToUtf8(dataPath);
        const int sr = g_espeak.initialize(2 /*AUDIO_OUTPUT_SYNCHRONOUS*/, 0, dataUtf8.c_str(), 0);
        if (sr <= 0) {
            LOG_WARN("Kokoro", "espeak_Initialize failed (" + std::to_string(sr) + "), data=" + dataUtf8);
            return false;
        }
        if (g_espeak.setVoice("en-us") != 0) {
            LOG_WARN("Kokoro", "espeak_SetVoiceByName(en-us) failed");
            return false;
        }

        available_ = true;
        LOG_INFO("Kokoro", "Local Kokoro TTS ready. engine=" + WsToUtf8(engineDir_) +
            ", voices=" + std::to_string(Voices().size()));
        return true;
    }

    std::vector<std::string> KokoroTts::Voices() const {
        std::vector<std::string> out;
        if (voicesDir_.empty()) return out;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(voicesDir_, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != L".bin") continue;
            out.push_back(entry.path().stem().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool KokoroTts::HasVoice(const std::string& voice) const {
        const auto list = Voices();
        return std::find(list.begin(), list.end(), voice) != list.end();
    }

    bool KokoroTts::LoadVoiceRows(const std::string& voice, std::vector<float>& rows, int& count) {
        if (voice.empty()) return false;
        if (voice == currentVoice_ && !currentRows_.empty()) {
            rows = currentRows_;
            count = currentRowCount_;
            return true;
        }
        const std::string path = voicesDir_ + "\\" + voice + ".bin";
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
        fseek(f, 0, SEEK_END);
        const long bytes = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (bytes <= 0 || (bytes % (256 * 4)) != 0) { fclose(f); return false; }
        std::vector<float> data(static_cast<size_t>(bytes) / sizeof(float));
        const size_t read = fread(data.data(), 1, static_cast<size_t>(bytes), f);
        fclose(f);
        if (read != static_cast<size_t>(bytes)) return false;
        rows = std::move(data);
        count = static_cast<int>(rows.size() / 256);
        currentRows_ = rows;
        currentRowCount_ = count;
        currentVoice_ = voice;
        return count > 0;
    }

    bool KokoroTts::Phonemize(const std::string& text, std::string& phonemes, std::string& error) {
        if (!available_ || !g_espeak.ok()) { error = "kokoro not available"; return false; }
        std::string source = text;
        const void* ptr = source.c_str();
        std::string out;
        for (int guard = 0; guard < 64; ++guard) {
            const char* part = g_espeak.toPhonemes(&ptr, kEspeakCharsUtf8, kEspeakPhonemesIpa);
            if (!part) break;
            out += part;
            if (!ptr || *static_cast<const char*>(ptr) == '\0') break;
        }
        if (out.empty()) { error = "espeak produced no phonemes"; return false; }
        phonemes = std::move(out);
        return true;
    }

    bool KokoroTts::Synthesize(const std::string& text, const std::string& voice, float speed,
        std::vector<int16_t>& pcm, int& sampleRate, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm.clear();
        sampleRate = 24000;
        if (!available_) { error = "kokoro not initialized"; return false; }

        std::string phonemes;
        if (!Phonemize(text, phonemes, error)) return false;

        std::vector<int64_t> ids;
        ids.reserve(phonemes.size());
        size_t i = 0;
        while (i < phonemes.size()) {
            const char32_t cp = NextUtf8(phonemes, i);
            if (cp == 0) continue;
            const int id = VocabId(cp);
            if (id >= 0) ids.push_back(id);
        }
        if (ids.empty()) { error = "no vocab matched phonemes"; return false; }

        std::vector<float> rows;
        int rowCount = 0;
        if (!LoadVoiceRows(voice, rows, rowCount)) {
            error = "voice not found: " + voice;
            return false;
        }
        int rowIndex = static_cast<int>(ids.size()) - 1;
        if (rowIndex >= rowCount) rowIndex = rowCount - 1;
        if (rowIndex < 0) rowIndex = 0;

        const OrtApi* api = static_cast<const OrtApi*>(api_);
        OrtSession* session = static_cast<OrtSession*>(session_);
        OrtMemoryInfo* memInfo = static_cast<OrtMemoryInfo*>(memoryInfo_);

        const int64_t shapeIds[2] = { 1, static_cast<int64_t>(ids.size()) };
        const int64_t shapeStyle[2] = { 1, 256 };
        const int64_t shapeSpeed[1] = { 1 };
        const float speedValue = std::clamp(speed, 0.5f, 2.0f);
        const float* stylePtr = rows.data() + static_cast<size_t>(rowIndex) * 256;

        OrtValue* idsTensor = nullptr;
        OrtValue* styleTensor = nullptr;
        OrtValue* speedTensor = nullptr;
        auto releaseInputs = [&]() {
            if (idsTensor) { api->ReleaseValue(idsTensor); idsTensor = nullptr; }
            if (styleTensor) { api->ReleaseValue(styleTensor); styleTensor = nullptr; }
            if (speedTensor) { api->ReleaseValue(speedTensor); speedTensor = nullptr; }
        };

        if (api->CreateTensorWithDataAsOrtValue(memInfo, ids.data(), ids.size() * sizeof(int64_t),
            shapeIds, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &idsTensor) != nullptr) {
            error = "create ids tensor failed";
            return false;
        }
        if (api->CreateTensorWithDataAsOrtValue(memInfo, const_cast<float*>(stylePtr), 256 * sizeof(float),
            shapeStyle, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &styleTensor) != nullptr) {
            releaseInputs();
            error = "create style tensor failed";
            return false;
        }
        if (api->CreateTensorWithDataAsOrtValue(memInfo, const_cast<float*>(&speedValue), sizeof(float),
            shapeSpeed, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &speedTensor) != nullptr) {
            releaseInputs();
            error = "create speed tensor failed";
            return false;
        }

        const char* inputNames[3] = { "input_ids", "style", "speed" };
        const OrtValue* inputs[3] = { idsTensor, styleTensor, speedTensor };
        const char* outputNames[1] = { "waveform" };
        OrtValue* outputs[1] = { nullptr };

        const OrtStatus* status = api->Run(session, nullptr, inputNames, inputs, 3, outputNames, 1, outputs);
        releaseInputs();
        if (status != nullptr) {
            const char* msg = api->GetErrorMessage(status);
            error = std::string("Run failed: ") + (msg ? msg : "?");
            api->ReleaseStatus(const_cast<OrtStatus*>(status));
            return false;
        }

        OrtValue* output = outputs[0];
        OrtTensorTypeAndShapeInfo* info = nullptr;
        api->GetTensorTypeAndShape(output, &info);
        size_t count = 0;
        api->GetTensorShapeElementCount(info, &count);
        api->ReleaseTensorTypeAndShapeInfo(info);
        float* data = nullptr;
        api->GetTensorMutableData(output, reinterpret_cast<void**>(&data));
        if (!data || count == 0) {
            api->ReleaseValue(output);
            error = "empty waveform";
            return false;
        }

        pcm.resize(count);
        for (size_t k = 0; k < count; ++k) {
            float v = data[k];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[k] = static_cast<int16_t>(v * 32767.0f);
        }
        const size_t frames = count;
        api->ReleaseValue(output);

        LOG_INFO("Kokoro", "Synthesis ok: " + std::to_string(frames) + " frames @ 24000 Hz, voice=" +
            voice + ", phonemes=" + std::to_string(ids.size()));
        return true;
    }

} // namespace UnnamedBot
