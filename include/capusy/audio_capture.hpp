#pragma once

#include "string_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && __has_include(<wil/com.h>)
#define CAPUSY_HAS_WASAPI 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <wil/com.h>
#include <wil/resource.h>
#endif

namespace capusy {

inline constexpr uint32_t AUDIO_OUT_SAMPLE_RATE = 48000u;
inline constexpr uint32_t AUDIO_OUT_CHANNELS = 2u;
inline constexpr uint32_t AUDIO_CHUNK_FRAMES = 960u; // 20 ms at 48 kHz

enum class AudioFlow : uint8_t {
    Render = 0,  // Desktop / system loopback output devices
    Capture = 1, // Microphone / input devices
};

enum class AudioSampleFormat : uint8_t {
    Unknown = 0,
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
    Pcm8,
};

struct AudioDeviceInfo {
    std::string id;
    std::wstring name;
    bool is_default = false;
};

struct StereoFrame {
    float l = 0.0f;
    float r = 0.0f;
};

/// Decodes a single sample at byte pointer `p` into normalized [-1.0, 1.0] float.
[[nodiscard]] inline float decode_audio_sample(const uint8_t* p, AudioSampleFormat fmt) noexcept {
    switch (fmt) {
        case AudioSampleFormat::Float32: {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(float));
            return std::isfinite(v) ? v : 0.0f;
        }
        case AudioSampleFormat::Pcm16: {
            int16_t v = 0;
            std::memcpy(&v, p, sizeof(int16_t));
            return static_cast<float>(v) * (1.0f / 32768.0f);
        }
        case AudioSampleFormat::Pcm24: {
            const uint32_t u = (static_cast<uint32_t>(p[0]) << 8u) |
                               (static_cast<uint32_t>(p[1]) << 16u) |
                               (static_cast<uint32_t>(p[2]) << 24u);
            const auto raw = static_cast<int32_t>(u);
            return static_cast<float>(raw) * (1.0f / 2147483648.0f);
        }
        case AudioSampleFormat::Pcm32: {
            int32_t v = 0;
            std::memcpy(&v, p, sizeof(int32_t));
            return static_cast<float>(v) * (1.0f / 2147483648.0f);
        }
        case AudioSampleFormat::Pcm8:
            return (static_cast<float>(p[0]) - 128.0f) * (1.0f / 128.0f);
        case AudioSampleFormat::Unknown:
            break;
    }
    return 0.0f;
}

[[nodiscard]] inline uint32_t audio_sample_bytes(AudioSampleFormat fmt) noexcept {
    switch (fmt) {
        case AudioSampleFormat::Float32: return 4u;
        case AudioSampleFormat::Pcm16: return 2u;
        case AudioSampleFormat::Pcm24: return 3u;
        case AudioSampleFormat::Pcm32: return 4u;
        case AudioSampleFormat::Pcm8: return 1u;
        case AudioSampleFormat::Unknown: return 0u;
    }
    return 0u;
}

/// Downmixes an interleaved multi-channel float frame into stereo (L, R).
[[nodiscard]] inline StereoFrame downmix_to_stereo(std::span<const float> ch) noexcept {
    const size_t n = ch.size();
    if (n == 0) return {};
    if (n == 1) return {.l = ch[0], .r = ch[0]};
    if (n == 2) return {.l = ch[0], .r = ch[1]};
    constexpr float kInvSqrt2 = 0.70710678f;
    if (n == 4) {
        // Quad: FL, FR, BL, BR
        return {.l = (ch[0] + (ch[2] * kInvSqrt2)) * 0.707f,
                .r = (ch[1] + (ch[3] * kInvSqrt2)) * 0.707f,};
    }
    if (n == 6) {
        // 5.1: FL, FR, FC, LFE, BL, BR
        const float center = ch[2] * kInvSqrt2;
        return {.l = (ch[0] + center + (ch[4] * kInvSqrt2)) * 0.65f,
                .r = (ch[1] + center + (ch[5] * kInvSqrt2)) * 0.65f,};
    }
    if (n >= 8) {
        // 7.1: FL, FR, FC, LFE, BL, BR, SL, SR
        const float center = ch[2] * kInvSqrt2;
        return {.l = (ch[0] + center + ((ch[4] + ch[6]) * kInvSqrt2 * 0.7f)) * 0.60f,
                .r = (ch[1] + center + ((ch[5] + ch[7]) * kInvSqrt2 * 0.7f)) * 0.60f,};
    }
    return {.l = ch[0], .r = ch[1]};
}

/// Decodes one interleaved input frame at `frame_ptr` into a stereo float frame.
[[nodiscard]] inline StereoFrame decode_frame_to_stereo(const uint8_t* frame_ptr, uint32_t channels, AudioSampleFormat fmt) noexcept {
    if (!frame_ptr || channels == 0) return {};
    const uint32_t sb = audio_sample_bytes(fmt);
    if (sb == 0) return {};
    if (channels == 1) {
        const float m = decode_audio_sample(frame_ptr, fmt);
        return {.l = m, .r = m};
    }
    if (channels == 2) {
        return {
            .l = decode_audio_sample(frame_ptr, fmt),
            .r = decode_audio_sample(frame_ptr + sb, fmt),
        };
    }
    std::array<float, 16> tmp{};
    const uint32_t clamped_ch = std::min<uint32_t>(channels, static_cast<uint32_t>(tmp.size()));
    for (uint32_t c = 0; c < clamped_ch; ++c) {
        tmp[c] = decode_audio_sample(frame_ptr + (static_cast<size_t>(c) * sb), fmt);
    }
    return downmix_to_stereo(std::span<const float>{tmp.data(), clamped_ch});
}

/// Low-allocation FIFO of interleaved stereo float frames (2 floats per frame).
class StereoFloatFifo {
public:
    explicit StereoFloatFifo(size_t reserve_frames = 48000u) {
        buf_.reserve(reserve_frames * 2u);
    }

    void clear() noexcept {
        buf_.clear();
        read_idx_ = 0;
    }

    [[nodiscard]] size_t frames_available() const noexcept {
        return (buf_.size() - read_idx_) / 2u;
    }

    void push_frame(float l, float r) {
        buf_.push_back(l);
        buf_.push_back(r);
    }

    void push_silence(size_t frames) {
        if (frames == 0) return;
        buf_.resize(buf_.size() + (frames * 2u), 0.0f);
    }

    /// Reads up to `max_frames` into `out_interleaved` (size >= max_frames * 2), returning frames read.
    size_t pop_frames(float* out_interleaved, size_t max_frames) noexcept {
        const size_t avail = frames_available();
        const size_t take = std::min(avail, max_frames);
        if (take > 0) {
            std::memcpy(out_interleaved, buf_.data() + read_idx_, take * 2u * sizeof(float));
            read_idx_ += take * 2u;
            maybe_compact();
        }
        return take;
    }

    /// Drops oldest frames if backlog exceeds `max_frames`, trimming down to `target_frames`.
    void trim_excess(size_t max_frames, size_t target_frames) noexcept {
        const size_t avail = frames_available();
        if (avail <= max_frames) return;
        const size_t drop = avail - std::min(target_frames, avail);
        read_idx_ += drop * 2u;
        maybe_compact();
    }

private:
    void maybe_compact() noexcept {
        if (read_idx_ == buf_.size()) {
            buf_.clear();
            read_idx_ = 0;
        } else if (read_idx_ >= 16384u && read_idx_ * 2u >= buf_.size()) {
            const size_t rem = buf_.size() - read_idx_;
            std::memmove(buf_.data(), buf_.data() + read_idx_, rem * sizeof(float));
            while (buf_.size() > rem) buf_.pop_back();
            read_idx_ = 0;
        }
    }

    std::vector<float> buf_;
    size_t read_idx_ = 0;
};

/// Stateful linear stereo resampler converting arbitrary `src_rate` to `dst_rate` (default 48 kHz).
/// Exact zero-interpolation pass-through when `src_rate == dst_rate`.
class StereoResampler {
public:
    void reset(uint32_t src_rate, uint32_t dst_rate = AUDIO_OUT_SAMPLE_RATE) noexcept {
        src_rate_ = src_rate > 0 ? src_rate : dst_rate;
        dst_rate_ = dst_rate > 0 ? dst_rate : AUDIO_OUT_SAMPLE_RATE;
        step_ = static_cast<double>(src_rate_) / static_cast<double>(dst_rate_);
        phase_ = 0.0;
        has_prev_ = false;
        prev_ = {};
    }

    void push_stereo(std::span<const StereoFrame> in, StereoFloatFifo& out) {
        if (in.empty()) return;
        if (src_rate_ == dst_rate_) {
            for (const StereoFrame& f : in) out.push_frame(f.l, f.r);
            return;
        }

        // Virtual timeline over [prev_ (index -1), in[0] (index 0), ..., in[n-1] (index n-1)]
        const size_t n = in.size();
        if (!has_prev_) {
            prev_ = in[0];
            has_prev_ = true;
            phase_ = 0.0;
        }

        const auto sample_at = [&](int64_t idx) noexcept -> StereoFrame {
            if (idx < 0) return prev_;
            if (std::cmp_greater_equal(idx, n)) return in[n - 1];
            return in[static_cast<size_t>(idx)];
        };

        const auto limit = static_cast<double>(n - 1);
        while (phase_ <= limit) {
            const auto i0 = static_cast<int64_t>(std::floor(phase_));
            const auto frac = static_cast<float>(phase_ - static_cast<double>(i0));
            const StereoFrame a = sample_at(i0);
            const StereoFrame b = sample_at(i0 + 1);
            out.push_frame(a.l + ((b.l - a.l) * frac), a.r + ((b.r - a.r) * frac));
            phase_ += step_;
        }
        phase_ -= static_cast<double>(n);
        prev_ = in[n - 1];
    }

    void push_silence(uint32_t src_frames, StereoFloatFifo& out) {
        if (src_frames == 0) return;
        if (src_rate_ == dst_rate_) {
            out.push_silence(src_frames);
            return;
        }
        const auto limit = static_cast<double>(src_frames - 1);
        size_t emitted = 0;
        while (phase_ <= limit) {
            ++emitted;
            phase_ += step_;
        }
        phase_ -= static_cast<double>(src_frames);
        prev_ = {};
        has_prev_ = true;
        out.push_silence(emitted);
    }

private:
    uint32_t src_rate_ = AUDIO_OUT_SAMPLE_RATE;
    uint32_t dst_rate_ = AUDIO_OUT_SAMPLE_RATE;
    double step_ = 1.0;
    double phase_ = 0.0;
    bool has_prev_ = false;
    StereoFrame prev_{};
};

/// Mixes two interleaved stereo float buffers (`a` and `b`, either may be empty) into interleaved 16-bit PCM (stereo).
inline void mix_stereo_to_pcm16(std::span<const float> a, std::span<const float> b, std::span<int16_t> out) noexcept {
    const size_t n = out.size();
    for (size_t i = 0; i < n; ++i) {
        float sum = 0.0f;
        if (i < a.size()) sum += a[i];
        if (i < b.size()) sum += b[i];
        // Soft-limit slightly above 1.0f before clamping so simultaneous desktop + mic never harsh-clips
        if (sum > 1.0f || sum < -1.0f) {
            sum = std::tanh(sum);
        }
        const float clamped = std::clamp(sum, -1.0f, 1.0f);
        out[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
}

/// Mixes two interleaved stereo float buffers (`a` and `b`, either may be empty) into mono 16-bit PCM (`out.size()` frames).
inline void mix_stereo_to_mono_pcm16(std::span<const float> a, std::span<const float> b, std::span<int16_t> out) noexcept {
    const size_t frames = out.size();
    for (size_t f = 0; f < frames; ++f) {
        const size_t i0 = f * 2u;
        const size_t i1 = i0 + 1u;
        float sum = 0.0f;
        if (i1 < a.size()) sum += (a[i0] + a[i1]) * 0.5f;
        if (i1 < b.size()) sum += (b[i0] + b[i1]) * 0.5f;
        if (sum > 1.0f || sum < -1.0f) {
            sum = std::tanh(sum);
        }
        const float clamped = std::clamp(sum, -1.0f, 1.0f);
        out[f] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
}

/// Resolves a user-friendly label for a selected device ID from an enumerated device list.
[[nodiscard]] inline std::wstring resolve_audio_device_label(
    std::string_view selected_id, std::span<const AudioDeviceInfo> devices, std::wstring_view default_prefix) {
    if (devices.empty()) return std::wstring{L"No device found"};
    if (!selected_id.empty()) {
        for (const auto& d : devices) {
            if (d.id == selected_id) return d.name;
        }
    }
    for (const auto& d : devices) {
        if (d.is_default) {
            return concat_wide(default_prefix, L" (", d.name, L")");
        }
    }
    return std::wstring{default_prefix};
}

#ifdef CAPUSY_HAS_WASAPI

namespace detail {

// PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
inline constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    .fmtid = {.Data1 = 0xa45c254e, .Data2 = 0xdf1c, .Data3 = 0x4efd, .Data4 = {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    .pid = 14,
};

// KSDATAFORMAT_SUBTYPE_PCM = {00000001-0000-0010-8000-00aa00389b71}
inline constexpr GUID kSubformatPcm = {
    .Data1 = 0x00000001, .Data2 = 0x0000, .Data3 = 0x0010, .Data4 = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}
inline constexpr GUID kSubformatIeeeFloat = {
    .Data1 = 0x00000003, .Data2 = 0x0000, .Data3 = 0x0010, .Data4 = {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};

[[nodiscard]] inline AudioSampleFormat detect_wave_format(const WAVEFORMATEX* wf) noexcept {
    if (!wf) return AudioSampleFormat::Unknown;
    const auto pcm_from_bits = [](WORD bits) noexcept -> AudioSampleFormat {
        switch (bits) {
            case 8: return AudioSampleFormat::Pcm8;
            case 16: return AudioSampleFormat::Pcm16;
            case 24: return AudioSampleFormat::Pcm24;
            case 32: return AudioSampleFormat::Pcm32;
            default: return AudioSampleFormat::Unknown;
        }
    };
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) {
        return AudioSampleFormat::Float32;
    }
    if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        return pcm_from_bits(wf->wBitsPerSample);
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, kSubformatIeeeFloat) && wf->wBitsPerSample == 32) {
            return AudioSampleFormat::Float32;
        }
        if (IsEqualGUID(ext->SubFormat, kSubformatPcm)) {
            return pcm_from_bits(wf->wBitsPerSample);
        }
    }
    return AudioSampleFormat::Unknown;
}

[[nodiscard]] inline std::wstring read_device_friendly_name(IMMDevice* dev) {
    if (!dev) return {};
    wil::com_ptr<IPropertyStore> store;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    const auto guard = wil::scope_exit([&] { PropVariantClear(&pv); });
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        return std::wstring{pv.pwszVal};
    }
    return {};
}

[[nodiscard]] inline std::string read_device_id_utf8(IMMDevice* dev) {
    if (!dev) return {};
    LPWSTR raw_id = nullptr;
    if (FAILED(dev->GetId(&raw_id)) || !raw_id) return {};
    const auto guard = wil::scope_exit([&] { CoTaskMemFree(raw_id); });
    return to_utf8(raw_id);
}

} // namespace detail

/// Enumerates active Windows WASAPI audio endpoints for the requested flow (Render = desktop loopback, Capture = mic).
/// Safe to call from STA (UI thread) or MTA (worker threads).
[[nodiscard]] inline std::vector<AudioDeviceInfo> enumerate_audio_devices(AudioFlow flow) {
    std::vector<AudioDeviceInfo> out;

    bool com_inited_here = false;
    wil::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (hr == CO_E_NOTINITIALIZED) {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            com_inited_here = true;
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        }
    }
    const auto com_guard = wil::scope_exit([&] {
        enumerator.reset();
        if (com_inited_here) CoUninitialize();
    });
    if (FAILED(hr) || !enumerator) return out;

    const EDataFlow edf = (flow == AudioFlow::Render) ? eRender : eCapture;
    std::string default_id;
    wil::com_ptr<IMMDevice> def_dev;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(edf, eConsole, &def_dev)) && def_dev) {
        default_id = detail::read_device_id_utf8(def_dev.get());
    }

    wil::com_ptr<IMMDeviceCollection> coll;
    if (FAILED(enumerator->EnumAudioEndpoints(edf, DEVICE_STATE_ACTIVE, &coll)) || !coll) return out;

    UINT count = 0;
    if (FAILED(coll->GetCount(&count))) return out;
    out.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        wil::com_ptr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev)) || !dev) continue;
        AudioDeviceInfo info;
        info.id = detail::read_device_id_utf8(dev.get());
        if (info.id.empty()) continue;
        info.name = detail::read_device_friendly_name(dev.get());
        if (info.name.empty()) info.name = (flow == AudioFlow::Render) ? L"Audio Output" : L"Microphone";
        info.is_default = (!default_id.empty() && info.id == default_id);
        out.push_back(std::move(info));
    }

    // Place the system default first, then sort remaining alphabetically for stable UI ordering.
    std::ranges::stable_sort(out, [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        if (a.is_default != b.is_default) return a.is_default;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

/// Shared-mode WASAPI capture stream supporting both desktop loopback (eRender) and microphone (eCapture),
/// converting any device mix format (mono/stereo/surround, PCM/float, any sample rate) into 48 kHz stereo float.
class WasapiCaptureStream {
public:
    WasapiCaptureStream() = default;
    WasapiCaptureStream(const WasapiCaptureStream&) = delete;
    WasapiCaptureStream& operator=(const WasapiCaptureStream&) = delete;
    WasapiCaptureStream(WasapiCaptureStream&&) = delete;
    WasapiCaptureStream& operator=(WasapiCaptureStream&&) = delete;
    ~WasapiCaptureStream() { close(); }

    bool open(AudioFlow flow, std::string_view device_id_utf8, uint32_t target_sample_rate = AUDIO_OUT_SAMPLE_RATE) {
        close();
        wil::com_ptr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || !enumerator) {
            return false;
        }

        const EDataFlow edf = (flow == AudioFlow::Render) ? eRender : eCapture;
        wil::com_ptr<IMMDevice> device;
        if (!device_id_utf8.empty()) {
            const std::wstring wid = to_wide(device_id_utf8);
            if (SUCCEEDED(enumerator->GetDevice(wid.c_str(), &device)) && device) {
                DWORD state = 0;
                if (FAILED(device->GetState(&state)) || state != DEVICE_STATE_ACTIVE) {
                    device.reset();
                }
            }
        }
        if (!device) {
            if (FAILED(enumerator->GetDefaultAudioEndpoint(edf, eConsole, &device)) || !device) {
                return false;
            }
        }

        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_))) || !client_) {
            return false;
        }

        WAVEFORMATEX* raw_fmt = nullptr;
        if (FAILED(client_->GetMixFormat(&raw_fmt)) || !raw_fmt) {
            close();
            return false;
        }
        const auto fmt_guard = wil::scope_exit([&] { CoTaskMemFree(raw_fmt); });

        fmt_ = detail::detect_wave_format(raw_fmt);
        channels_ = raw_fmt->nChannels;
        block_align_ = raw_fmt->nBlockAlign;
        sample_rate_ = raw_fmt->nSamplesPerSec;
        if (fmt_ == AudioSampleFormat::Unknown || channels_ == 0 || block_align_ == 0 || sample_rate_ == 0) {
            close();
            return false;
        }

        const DWORD flags = (flow == AudioFlow::Render) ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0u;
        constexpr REFERENCE_TIME kBufferDuration = 1'000'000; // 100 ms
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, raw_fmt, nullptr))) {
            close();
            return false;
        }

        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_))) || !capture_) {
            close();
            return false;
        }

        resampler_.reset(sample_rate_, target_sample_rate > 0 ? target_sample_rate : AUDIO_OUT_SAMPLE_RATE);
        scratch_.reserve(4096);
        return true;
    }

    [[nodiscard]] bool opened() const noexcept { return client_ != nullptr && capture_ != nullptr; }

    void retarget_sample_rate(uint32_t target_sample_rate) noexcept {
        if (sample_rate_ > 0 && target_sample_rate > 0) {
            resampler_.reset(sample_rate_, target_sample_rate);
        }
    }

    bool start() noexcept {
        if (!client_) return false;
        started_ = SUCCEEDED(client_->Start());
        return started_;
    }

    void stop() noexcept {
        if (client_ && started_) {
            (void)client_->Stop();
            started_ = false;
        }
    }

    void close() noexcept {
        stop();
        capture_.reset();
        client_.reset();
        fmt_ = AudioSampleFormat::Unknown;
        channels_ = 0;
        block_align_ = 0;
        sample_rate_ = 0;
    }

    /// Drains all currently queued WASAPI packets into `out_fifo` (48 kHz interleaved stereo float).
    /// Returns true if at least one packet was received from the audio engine.
    bool drain_into(StereoFloatFifo& out_fifo) {
        if (!capture_) return false;
        bool received = false;
        for (;;) {
            UINT32 packet_frames = 0;
            if (FAILED(capture_->GetNextPacketSize(&packet_frames)) || packet_frames == 0) break;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (frames > 0) {
                received = true;
                if ((flags & static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_SILENT)) != 0u || data == nullptr) {
                    resampler_.push_silence(frames, out_fifo);
                } else {
                    scratch_.resize(frames);
                    for (UINT32 i = 0; i < frames; ++i) {
                        scratch_[i] = decode_frame_to_stereo(data + (static_cast<size_t>(i) * block_align_), channels_, fmt_);
                    }
                    resampler_.push_stereo(scratch_, out_fifo);
                }
            }
            (void)capture_->ReleaseBuffer(frames);
        }
        return received;
    }

private:
    wil::com_ptr<IAudioClient> client_;
    wil::com_ptr<IAudioCaptureClient> capture_;
    AudioSampleFormat fmt_ = AudioSampleFormat::Unknown;
    uint32_t channels_ = 0;
    uint32_t block_align_ = 0;
    uint32_t sample_rate_ = 0;
    bool started_ = false;
    StereoResampler resampler_;
    std::vector<StereoFrame> scratch_;
};

#endif // CAPUSY_HAS_WASAPI

} // namespace capusy
