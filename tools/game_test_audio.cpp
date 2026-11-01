// Bounded developer capture of one explicitly selected process tree.
// Uses Windows process loopback, never the microphone or system-wide loopback.
// API reference: https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    ~Activation() { if (ready) CloseHandle(ready); }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activation = E_FAIL;
        ComPtr<IUnknown> unknown;
        result = operation->GetActivateResult(&activation, unknown.GetAddressOf());
        if (SUCCEEDED(result)) result = activation;
        if (SUCCEEDED(result)) result = unknown.As(&client);
        SetEvent(ready);
        return S_OK;
    }
};

class Handle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

static bool Write(HANDLE file, const void* data, DWORD bytes) {
    DWORD written = 0;
    return WriteFile(file, data, bytes, &written, nullptr) && written == bytes;
}

// Fixed PCM RIFF header: all fields have explicit widths and no ABI padding.
static bool Header(HANDLE file, uint32_t bytes) {
    const uint32_t riff = bytes + 36, fmt = 16, rate = 48000, byteRate = rate * 4;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    return SetFilePointer(file, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        Write(file, "RIFF", 4) && Write(file, &riff, 4) && Write(file, "WAVEfmt ", 8) &&
        Write(file, &fmt, 4) && Write(file, &pcm, 2) && Write(file, &channels, 2) &&
        Write(file, &rate, 4) && Write(file, &byteRate, 4) && Write(file, &align, 2) &&
        Write(file, &bits, 2) && Write(file, "data", 4) && Write(file, &bytes, 4);
}

static HRESULT Capture(DWORD pid, DWORD seconds, const wchar_t* path) {
    Handle target(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!target.value) return HRESULT_FROM_WIN32(GetLastError());
    auto completion = Microsoft::WRL::Make<Activation>();
    if (!completion || !completion->ready) return E_OUTOFMEMORY;
    AUDIOCLIENT_ACTIVATION_PARAMS activation{};
    activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation.ProcessLoopbackParams.TargetProcessId = pid;
    activation.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT parameters{};
    parameters.vt = VT_BLOB;
    parameters.blob.cbSize = sizeof(activation);
    parameters.blob.pBlobData = reinterpret_cast<BYTE*>(&activation);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient), &parameters, completion.Get(), operation.GetAddressOf());
    if (FAILED(hr)) return hr;
    if (WaitForSingleObject(completion->ready, 10000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (FAILED(completion->result)) return completion->result;
    auto client = completion->client;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        0, 0, &format, nullptr);
    if (FAILED(hr)) return hr;
    Handle signal(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!signal.value) return HRESULT_FROM_WIN32(GetLastError());
    hr = client->SetEventHandle(signal.value);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioCaptureClient> reader;
    hr = client->GetService(IID_PPV_ARGS(reader.GetAddressOf()));
    if (FAILED(hr)) return hr;
    Handle output(CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!Header(output.value, 0)) return HRESULT_FROM_WIN32(GetLastError());
    hr = client->Start();
    if (FAILED(hr)) return hr;
    LARGE_INTEGER frequency{}, started{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    printf("{\"event\":\"audio_ready\",\"pid\":%lu,\"qpc\":%lld,\"qpc_frequency\":%lld}\n", pid, started.QuadPart, frequency.QuadPart);
    fflush(stdout);
    uint32_t bytes = 0;
    const uint32_t maximum = seconds * format.nAvgBytesPerSec;
    std::vector<BYTE> silence;
    const ULONGLONG deadline = GetTickCount64() + seconds * 1000ull;
    while (bytes < maximum && GetTickCount64() < deadline) {
        HANDLE waits[]{signal.value, target.value};
        const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (waited == WAIT_OBJECT_0 + 1) { hr = HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED); break; }
        if (waited == WAIT_FAILED) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
        UINT32 pending = 0;
        while (SUCCEEDED(hr = reader->GetNextPacketSize(&pending)) && pending && bytes < maximum) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = reader->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const DWORD count = (std::min)(frames * format.nBlockAlign, maximum - bytes);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(count, 0);
                data = silence.data();
            }
            const bool written = Write(output.value, data, count);
            const HRESULT released = reader->ReleaseBuffer(frames);
            if (!written) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
            bytes += count;
            if (FAILED(released)) { hr = released; break; }
        }
        if (FAILED(hr)) break;
    }
    client->Stop();
    if (FAILED(hr)) return hr;
    // Preserve the requested time base if the selected process is silent.
    silence.assign(4096, 0);
    while (bytes < maximum) {
        const DWORD count = (std::min)(maximum - bytes, static_cast<uint32_t>(silence.size()));
        if (!Write(output.value, silence.data(), count)) return HRESULT_FROM_WIN32(GetLastError());
        bytes += count;
    }
    if (!Header(output.value, bytes) || !FlushFileBuffers(output.value)) return HRESULT_FROM_WIN32(GetLastError());
    printf("{\"event\":\"audio_complete\",\"pcm_bytes\":%u}\n", bytes);
    return S_OK;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { fwprintf(stderr, L"Usage: game_test_audio PID SECONDS NEW_OUTPUT.wav\n"); return 2; }
    wchar_t* end = nullptr;
    const unsigned long pid = wcstoul(argv[1], &end, 10);
    if (!pid || !end || *end) return 2;
    const unsigned long seconds = wcstoul(argv[2], &end, 10);
    if (!seconds || seconds > 60 || !end || *end) return 2;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 3;
    const HRESULT result = Capture(pid, seconds, argv[3]);
    CoUninitialize();
    if (FAILED(result)) fprintf(stderr, "process audio capture failed: 0x%08lX\n", static_cast<unsigned long>(result));
    return SUCCEEDED(result) ? 0 : 1;
}
