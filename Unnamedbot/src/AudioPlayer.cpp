#include "../include/AudioPlayer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objbase.h>
#include <wrl/client.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>

#include "../include/Logger.h"
#include "../include/Utils.h"

using Microsoft::WRL::ComPtr;

namespace UnnamedBot {

    namespace {

        // WASAPI 共享模式必须按设备混音格式喂数据，所以这里把 TTS 的 24k/mono
        // 线性插值重采样到设备格式（通常是 48k/2ch float32）。
        struct MixFormat {
            WAVEFORMATEX* wfx = nullptr;
            int sampleRate = 48000;
            int channels = 2;
            int bytesPerSample = 4;
            bool isFloat = true;

            ~MixFormat() {
                if (wfx) CoTaskMemFree(wfx);
            }
        };

        // 把源 PCM16 转成混音格式的字节流（一次转换整块）
        std::vector<uint8_t> ConvertToMixFormat(
            const std::vector<int16_t>& src,
            int srcRate,
            int srcChannels,
            const MixFormat& mix
        ) {
            if (srcRate <= 0 || srcChannels <= 0 || src.empty()) {
                return {};
            }

            const double ratio = static_cast<double>(mix.sampleRate) / static_cast<double>(srcRate);
            const size_t srcFrames = src.size() / static_cast<size_t>(srcChannels);
            const size_t dstFrames = static_cast<size_t>(std::ceil(srcFrames * ratio));

            std::vector<uint8_t> out(dstFrames * static_cast<size_t>(mix.channels) * static_cast<size_t>(mix.bytesPerSample));

            for (size_t i = 0; i < dstFrames; ++i) {
                const double srcPos = static_cast<double>(i) / ratio;
                const size_t i0 = std::min(static_cast<size_t>(srcPos), srcFrames - 1);
                const size_t i1 = std::min(i0 + 1, srcFrames - 1);
                const float frac = static_cast<float>(srcPos - static_cast<double>(i0));

                for (int ch = 0; ch < mix.channels; ++ch) {
                    const int srcCh = (ch < srcChannels) ? ch : (srcChannels - 1);
                    const float s0 = static_cast<float>(src[i0 * static_cast<size_t>(srcChannels) + static_cast<size_t>(srcCh)]) / 32768.0f;
                    const float s1 = static_cast<float>(src[i1 * static_cast<size_t>(srcChannels) + static_cast<size_t>(srcCh)]) / 32768.0f;
                    const float value = std::clamp(s0 + (s1 - s0) * frac, -1.0f, 1.0f);

                    uint8_t* dst = out.data() + ((i * static_cast<size_t>(mix.channels) + static_cast<size_t>(ch)) * static_cast<size_t>(mix.bytesPerSample));
                    if (mix.isFloat) {
                        std::memcpy(dst, &value, sizeof(float));
                    } else if (mix.bytesPerSample == 2) {
                        const int16_t v = static_cast<int16_t>(value * 32767.0f);
                        std::memcpy(dst, &v, sizeof(int16_t));
                    } else if (mix.bytesPerSample == 4) {
                        const int32_t v = static_cast<int32_t>(value * 2147483647.0f);
                        std::memcpy(dst, &v, sizeof(int32_t));
                    }
                }
            }

            return out;
        }

    } // namespace

    AudioPlayer& AudioPlayer::Instance() {
        static AudioPlayer instance;
        return instance;
    }

    AudioPlayer::~AudioPlayer() {
        Shutdown();
    }

    void AudioPlayer::EnsureWorker() {
        if (running_.load()) {
            return;
        }
        running_.store(true);
        worker_ = std::thread(&AudioPlayer::WorkerLoop, this);
    }

    bool AudioPlayer::PlayPcm16(const std::vector<int16_t>& samples, int sampleRate, int channels) {
        if (samples.empty() || sampleRate <= 0 || channels <= 0) {
            return false;
        }

        EnsureWorker();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(Buffer{ samples, sampleRate, channels });
        }
        cv_.notify_one();
        return true;
    }

    void AudioPlayer::Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        stopping_.store(true);
        cv_.notify_all();
    }

    void AudioPlayer::Shutdown() {
        if (!running_.exchange(false)) {
            return;
        }
        stopping_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        level_.store(0.0f);
        playing_.store(false);
    }

    void AudioPlayer::Tick() {
        const bool busy = playing_.load(std::memory_order_relaxed);
        float level = level_.load(std::memory_order_relaxed);
        // 说话时由 PushLevel 抬升，这里只负责放手：约 25 帧内回落到 0
        level = busy ? level : level * 0.82f;
        if (level < 0.004f) {
            level = 0.0f;
        }
        level_.store(level, std::memory_order_relaxed);
    }

    void AudioPlayer::PushLevel(float rms) {
        const float value = std::clamp(rms * gain_, 0.0f, 1.0f);
        float current = level_.load(std::memory_order_relaxed);
        // attack 快、release 慢，嘴型才跟得上音节又不抖
        current = (value > current) ? (current + (value - current) * 0.55f)
                                    : (current + (value - current) * 0.18f);
        level_.store(std::clamp(current, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void AudioPlayer::WorkerLoop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comReady = SUCCEEDED(com);

        for (;;) {
            Buffer buffer;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return !running_.load() || stopping_.load() || !queue_.empty();
                });
                if (!running_.load()) {
                    break;
                }
                if (stopping_.load()) {
                    stopping_.store(false);
                    queue_.clear();
                    continue;
                }
                buffer = std::move(queue_.front());
                queue_.pop_front();
            }

            playing_.store(true);
            PlayOne(buffer);
            playing_.store(false);
        }

        if (comReady) {
            CoUninitialize();
        }
    }

    void AudioPlayer::PlayOne(const Buffer& buffer) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))) {
            LOG_WARN("Audio", "Create MMDeviceEnumerator failed.");
            return;
        }

        ComPtr<IMMDevice> device;
        // eMultimedia 是用户实际听声音用的端点；取不到再退回 eConsole
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) &&
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            LOG_WARN("Audio", "No default render device.");
            return;
        }
        {
            // 打日志用设备友好名：出问题时一眼能看出声音被送去了哪个端点
            std::string name = "(unknown)";
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal) {
                    name = Utils::WideToUtf8(value.pwszVal);
                }
                PropVariantClear(&value);
            }
            LOG_INFO("Audio", "Render endpoint: " + name);
        }

        ComPtr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())))) {
            LOG_WARN("Audio", "Activate IAudioClient failed.");
            return;
        }

        MixFormat mix;
        if (FAILED(client->GetMixFormat(&mix.wfx)) || !mix.wfx) {
            LOG_WARN("Audio", "GetMixFormat failed.");
            return;
        }
        mix.sampleRate = static_cast<int>(mix.wfx->nSamplesPerSec);
        mix.channels = static_cast<int>(mix.wfx->nChannels);
        mix.bytesPerSample = static_cast<int>(mix.wfx->wBitsPerSample / 8);
        mix.isFloat = (mix.wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
            (mix.wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                mix.wfx->cbSize >= 22 &&
                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix.wfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        const REFERENCE_TIME duration = 10000000 / 10; // 100ms 缓冲
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, mix.wfx, nullptr))) {
            LOG_WARN("Audio", "IAudioClient::Initialize failed.");
            return;
        }

        UINT32 bufferFrames = 0;
        if (FAILED(client->GetBufferSize(&bufferFrames))) {
            return;
        }

        ComPtr<IAudioRenderClient> render;
        if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
            LOG_WARN("Audio", "GetService(IAudioRenderClient) failed.");
            return;
        }

        const std::vector<uint8_t> pcm = ConvertToMixFormat(buffer.samples, buffer.sampleRate, buffer.channels, mix);
        {
            // 自检：转换后是不是静音（声音送出去了但没人听见时，先确认数据不为 0）
            double peak = 0.0;
            if (mix.isFloat) {
                const float* f = reinterpret_cast<const float*>(pcm.data());
                const size_t n = pcm.size() / sizeof(float);
                for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(static_cast<double>(f[i])));
            } else if (mix.bytesPerSample == 2) {
                const int16_t* s = reinterpret_cast<const int16_t*>(pcm.data());
                const size_t n = pcm.size() / sizeof(int16_t);
                for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(static_cast<double>(s[i])) / 32768.0);
            }
            LOG_INFO("Audio", "Converted peak amplitude: " + std::to_string(peak));
        }
        const size_t frameBytes = static_cast<size_t>(mix.channels) * static_cast<size_t>(mix.bytesPerSample);
        const UINT32 totalFrames = static_cast<UINT32>(pcm.size() / frameBytes);

        LOG_INFO("Audio", "Playback start: " + std::to_string(totalFrames) + " frames @ " +
            std::to_string(mix.sampleRate) + " Hz, " + std::to_string(mix.channels) + " ch, " +
            std::to_string(mix.bytesPerSample * 8) + "-bit " + (mix.isFloat ? "float" : "pcm") +
            ", buffer=" + std::to_string(bufferFrames) + " frames");

        if (FAILED(client->Start())) {
            LOG_WARN("Audio", "IAudioClient::Start failed.");
            return;
        }

        UINT32 written = 0;
        const auto startAt = GetTickCount64();
        // 最长时间保护：按数据长度算出的时长再放宽 2s
        const ULONGLONG limitMs = static_cast<ULONGLONG>(totalFrames * 1000ull / static_cast<UINT32>(mix.sampleRate)) + 2000;

        while (written < totalFrames) {
            if (!running_.load() || stopping_.load()) {
                break;
            }

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                break;
            }

            const UINT32 available = bufferFrames - padding;
            if (available == 0) {
                Sleep(5);
                continue;
            }

            const UINT32 chunk = std::min(available, totalFrames - written);
            BYTE* dst = nullptr;
            if (FAILED(render->GetBuffer(chunk, &dst)) || !dst) {
                break;
            }

            std::memcpy(dst, pcm.data() + static_cast<size_t>(written) * frameBytes,
                static_cast<size_t>(chunk) * frameBytes);

            // 顺手算这块的 RMS 给嘴型用
            double sum = 0.0;
            const size_t samples = static_cast<size_t>(chunk) * static_cast<size_t>(mix.channels);
            if (mix.isFloat) {
                const float* f = reinterpret_cast<const float*>(dst);
                for (size_t i = 0; i < samples; ++i) sum += static_cast<double>(f[i]) * f[i];
            } else if (mix.bytesPerSample == 2) {
                const int16_t* s = reinterpret_cast<const int16_t*>(dst);
                for (size_t i = 0; i < samples; ++i) {
                    const double v = static_cast<double>(s[i]) / 32768.0;
                    sum += v * v;
                }
            }
            if (samples > 0) {
                PushLevel(static_cast<float>(std::sqrt(sum / static_cast<double>(samples))));
            }

            if (FAILED(render->ReleaseBuffer(chunk, 0))) {
                break;
            }
            written += chunk;

            if (GetTickCount64() - startAt > limitMs) {
                break;
            }
            Sleep(5);
        }

        // 让缓冲里的尾巴播完（最多 300ms），再停
        for (int i = 0; i < 12; ++i) {
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)) || padding == 0) {
                break;
            }
            Sleep(25);
        }
        client->Stop();
        LOG_INFO("Audio", "Playback done: " + std::to_string(written) + "/" + std::to_string(totalFrames) + " frames");
    }

} // namespace UnnamedBot
