// What the PC plays, captured with WASAPI loopback in shared mode: other
// programs (OBS, the music player) are not disturbed. Output: 16 kHz mono
// 16-bit, in real time. While nothing plays, Windows delivers no audio at all,
// so the gaps are filled with silence to keep the stream's clock running.
// A built-in test tone can stand in for the device.
#pragma once
#include "common.hpp"
#include "resampler.hpp"

namespace ps {

struct OutputDevice {
    std::wstring id, name;
};

inline std::vector<OutputDevice> listOutputDevices() {
    std::vector<OutputDevice> list;
    Com<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)enumerator.out()))) return list;
    Com<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.out()))) return list;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        Com<IMMDevice> device;
        if (FAILED(devices->Item(i, device.out()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;
        OutputDevice d;
        d.id = id;
        CoTaskMemFree(id);
        Com<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.out()))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) d.name = name.pwszVal;
            PropVariantClear(&name);
        }
        if (d.name.empty()) d.name = L"Output device " + std::to_wstring(i + 1);
        list.push_back(d);
    }
    return list;
}

class AudioCapture {
public:
    using Sink = std::function<void(const int16_t*, size_t)>;
    static constexpr int Rate = 16000;
    static inline const std::wstring DefaultDevice = L"";
    static inline const std::wstring TestTone = L"tone";

    ~AudioCapture() { stop(); }

    // device: DefaultDevice (follows Windows' default speakers), TestTone, or a device id.
    void start(const std::wstring& device, Sink sink) {
        stop();
        source = device;
        output = std::move(sink);
        quit = false;
        worker = std::thread([this] { run(); });
    }

    void stop() {
        quit = true;
        if (worker.joinable()) worker.join();
    }

    float peakDb() const { return peak.load(); }
    float rmsDb() const { return rms.load(); }
    std::wstring description() { std::lock_guard<std::mutex> lock(m); return describe; }
    double secondsSinceSound() const { return nowSeconds() - lastSound.load(); }

private:
    std::thread worker;
    std::atomic<bool> quit{false};
    std::wstring source;
    Sink output;
    std::mutex m;
    std::wstring describe = L"Not started";
    std::atomic<float> peak{-120.f}, rms{-120.f};
    std::atomic<double> lastSound{0};
    // Level over 50 ms blocks of the 16 kHz output.
    float blockPeak = 0;
    double blockSum = 0;
    int blockCount = 0;
    std::vector<int16_t> pcm;

    void setDescription(const std::wstring& text) { std::lock_guard<std::mutex> lock(m); describe = text; }

    void deliver(const float* samples, size_t n) {
        pcm.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float v = std::clamp(samples[i], -1.f, 1.f);
            pcm[i] = int16_t(std::lround(v * 32767.f));
            float a = std::fabs(v);
            blockPeak = std::max(blockPeak, a);
            blockSum += double(v) * v;
            if (++blockCount == Rate / 20) {
                auto db = [](double x) { return x > 1e-6 ? float(20 * std::log10(x)) : -120.f; };
                peak = db(blockPeak);
                rms = db(std::sqrt(blockSum / blockCount));
                if (blockPeak > 0.001f) lastSound = nowSeconds();
                blockPeak = 0;
                blockSum = 0;
                blockCount = 0;
            }
        }
        if (output && n) output(pcm.data(), n);
    }

    void run() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        lastSound = nowSeconds();
        if (source == TestTone) runTone();
        else {
            while (!quit) {
                std::wstring error = runLoopback();
                if (quit) break;
                if (error == L"restarting") continue;   // the default speakers changed
                setDescription(L"Audio device problem: " + error + L" (retrying)");
                eventLog().add(L"Audio capture: " + error + L"; retrying in 2 s");
                for (int i = 0; i < 20 && !quit; ++i) Sleep(100);
            }
        }
        peak = -120.f;
        rms = -120.f;
        if (SUCCEEDED(co)) CoUninitialize();
    }

    // 440 Hz, half a second on and half off, at -12 dBFS: for tests.
    void runTone() {
        setDescription(L"Test tone (440 Hz beeps)");
        double start = nowSeconds();
        uint64_t made = 0;
        std::vector<float> block;
        while (!quit) {
            uint64_t due = uint64_t((nowSeconds() - start) * Rate);
            if (due > made) {
                block.resize(size_t(due - made));
                for (size_t i = 0; i < block.size(); ++i) {
                    uint64_t n = made + i;
                    bool on = (n / (Rate / 2)) % 2 == 0;
                    block[i] = on ? 0.25f * float(std::sin(2 * 3.14159265358979 * 440.0 * double(n) / Rate)) : 0.f;
                }
                deliver(block.data(), block.size());
                made = due;
            }
            Sleep(10);
        }
    }

    std::wstring runLoopback() {
        Com<IMMDeviceEnumerator> enumerator;
        Com<IMMDevice> device;
        Com<IAudioClient> client;
        Com<IAudioCaptureClient> capture;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)enumerator.out());
        if (FAILED(hr)) return L"no audio system (" + errorText(DWORD(hr)) + L")";
        bool followDefault = source.empty();
        hr = followDefault ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.out())
                           : enumerator->GetDevice(source.c_str(), device.out());
        if (FAILED(hr)) return followDefault ? L"no default speakers" : L"the chosen output device is not available";
        std::wstring deviceId;
        { LPWSTR id = nullptr; if (SUCCEEDED(device->GetId(&id))) { deviceId = id; CoTaskMemFree(id); } }
        std::wstring name = L"output device";
        {
            Com<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.out()))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
                PropVariantClear(&v);
            }
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.out());
        if (FAILED(hr)) return L"cannot open " + name;
        WAVEFORMATEX* format = nullptr;
        hr = client->GetMixFormat(&format);
        if (FAILED(hr)) return L"cannot read the format of " + name;
        struct FormatGuard { WAVEFORMATEX* f; ~FormatGuard() { CoTaskMemFree(f); } } guard{format};
        bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* x = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
            isFloat = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        }
        const int channels = format->nChannels, bits = format->wBitsPerSample, frameBytes = format->nBlockAlign;
        const double inRate = format->nSamplesPerSec;
        if (!(isFloat && bits == 32) && !(!isFloat && (bits == 16 || bits == 24 || bits == 32))) return L"unsupported audio format";
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, format, nullptr);
        if (FAILED(hr)) return L"cannot capture " + name + L" (" + errorText(DWORD(hr)) + L")";
        hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)capture.out());
        if (FAILED(hr)) return L"cannot capture " + name;
        hr = client->Start();
        if (FAILED(hr)) return L"cannot start capturing " + name;

        Resampler resampler;
        resampler.init(inRate, Rate);
        std::vector<float> mono, out;
        wchar_t desc[300];
        swprintf_s(desc, L"%s%s (%d Hz, %d ch)", followDefault ? L"Desktop audio: " : L"", name.c_str(), int(inRate), channels);
        setDescription(desc);
        eventLog().add(L"Capturing " + std::wstring(desc));

        double lastData = nowSeconds(), filledUntil = lastData, lastDefaultCheck = lastData;
        std::wstring result;
        while (!quit) {
            bool got = false;
            UINT32 packet = 0;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                mono.assign(frames, 0.f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    for (UINT32 f = 0; f < frames; ++f) {
                        const BYTE* frame = data + size_t(f) * frameBytes;
                        float sum = 0;
                        for (int c = 0; c < channels; ++c) {
                            if (isFloat) sum += reinterpret_cast<const float*>(frame)[c];
                            else if (bits == 16) sum += reinterpret_cast<const int16_t*>(frame)[c] / 32768.f;
                            else if (bits == 32) sum += float(reinterpret_cast<const int32_t*>(frame)[c] / 2147483648.0);
                            else {
                                const BYTE* s = frame + c * 3;
                                int32_t v = (int32_t(s[0]) << 8) | (int32_t(s[1]) << 16) | (int32_t(s[2]) << 24);
                                sum += float(v / 2147483648.0);
                            }
                        }
                        mono[f] = sum / float(channels);
                    }
                }
                capture->ReleaseBuffer(frames);
                out.clear();
                resampler.process(mono.data(), mono.size(), out);
                deliver(out.data(), out.size());
                got = true;
            }
            if (FAILED(hr)) { result = hr == AUDCLNT_E_DEVICE_INVALIDATED ? name + L" was removed or changed" : errorText(DWORD(hr)); break; }
            double now = nowSeconds();
            if (got) {
                lastData = now;
                filledUntil = now;
            } else if (now - lastData > 0.06) {
                // Nothing is playing: Windows sends nothing, so send silence.
                size_t n = size_t((now - filledUntil) * inRate);
                if (n) {
                    mono.assign(n, 0.f);
                    out.clear();
                    resampler.process(mono.data(), n, out);
                    deliver(out.data(), out.size());
                    filledUntil += double(n) / inRate;
                }
            }
            if (followDefault && now - lastDefaultCheck > 2) {
                lastDefaultCheck = now;
                Com<IMMDevice> current;
                LPWSTR id = nullptr;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, current.out())) && SUCCEEDED(current->GetId(&id))) {
                    bool changed = deviceId != id;
                    CoTaskMemFree(id);
                    if (changed) { eventLog().add(L"Windows' default speakers changed: switching"); break; }
                }
            }
            Sleep(10);
        }
        client->Stop();
        return result.empty() ? L"restarting" : result;
    }
};

} // namespace ps
