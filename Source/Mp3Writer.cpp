// Windows Media Foundation의 MP3 인코더("MP3 Encoder ACM Wrapper MFT", Windows 8 이후 기본 포함)로 MP3를 쓴다.

#include "Mp3Writer.h"
#include "Common.h"

#ifdef _WIN32

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#pragma comment (lib, "mfplat.lib")
#pragma comment (lib, "mfreadwrite.lib")
#pragma comment (lib, "mfuuid.lib")
#pragma comment (lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
juce::String hresultText (HRESULT hr)
{
    return "0x" + juce::String::toHexString ((juce::uint32) hr).paddedLeft ('0', 8).toUpperCase();
}
} // namespace

struct Mp3FileWriter::Impl
{
    ~Impl()
    {
        // finish 없이 끝나면(오류·취소) 쓰다 만 파일을 남기지 않는다.
        const bool unfinished = writer != nullptr;
        writer.Reset();

        if (unfinished)
            file.deleteFile();

        if (mfStarted)
            MFShutdown();

        if (comInitialised)
            CoUninitialize();
    }

    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    double rate = 44100.0;
    int kbps = 0;
    LONGLONG framesWritten = 0;
    bool comInitialised = false, mfStarted = false;
    juce::File file;
    juce::Random dither { 12345 };
};

Mp3FileWriter::Mp3FileWriter() = default;
Mp3FileWriter::~Mp3FileWriter() = default;

bool Mp3FileWriter::supportsSampleRate (double sampleRate) noexcept
{
    return sampleRate == 32000.0 || sampleRate == 44100.0 || sampleRate == 48000.0;
}

int Mp3FileWriter::getKbps() const noexcept
{
    return impl != nullptr ? impl->kbps : 0;
}

bool Mp3FileWriter::open (const juce::File& file, double sampleRate, int kbps, juce::String& error)
{
    impl.reset();

    if (! supportsSampleRate (sampleRate))
    {
        error = K ("MP3는 32·44.1·48kHz 녹음만 저장할 수 있습니다(이 녹음은 ") + juce::String (sampleRate / 1000.0, 1)
              + K ("kHz). WAV로 저장해 주세요.");
        return false;
    }

    auto state = std::make_unique<Impl>();
    state->rate = sampleRate;
    state->file = file;

    // 메시지 스레드는 JUCE가 이미 COM을 켜 두었을 수 있다(그때는 RPC_E_CHANGED_MODE: 그대로 쓴다).
    const HRESULT com = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    state->comInitialised = SUCCEEDED (com);

    HRESULT hr = MFStartup (MF_VERSION);

    if (FAILED (hr))
    {
        error = K ("Windows 미디어 기능을 시작하지 못해 MP3로 저장할 수 없습니다(") + hresultText (hr)
              + K ("). WAV로 저장해 주세요.");
        return false;
    }

    state->mfStarted = true;
    file.deleteFile();

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes (&attributes, 1);

    if (SUCCEEDED (hr))
        hr = attributes->SetGUID (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MP3);

    if (SUCCEEDED (hr))
        hr = MFCreateSinkWriterFromURL (file.getFullPathName().toWideCharPointer(), nullptr, attributes.Get(), &state->writer);

    if (FAILED (hr))
    {
        error = K ("MP3 파일을 만들 수 없습니다(") + hresultText (hr) + "): " + file.getFullPathName();
        return false;
    }

    // 원하는 비트레이트를 인코더가 받지 않으면 표준 값으로 내려 간다.
    HRESULT lastError = E_FAIL;

    for (const int candidate : { kbps, 256, 192, 160, 128 })
    {
        if (candidate > kbps && candidate != kbps)
            continue;

        ComPtr<IMFMediaType> output;
        hr = MFCreateMediaType (&output);

        if (SUCCEEDED (hr)) hr = output->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED (hr)) hr = output->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_MP3);
        if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
        if (SUCCEEDED (hr)) hr = output->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (candidate * 1000 / 8));
        if (SUCCEEDED (hr)) hr = state->writer->AddStream (output.Get(), &state->stream);

        if (SUCCEEDED (hr))
        {
            state->kbps = candidate;
            break;
        }

        lastError = hr;
    }

    if (state->kbps == 0)
    {
        error = K ("이 PC의 MP3 인코더가 이 설정을 받지 않습니다(") + hresultText (lastError) + K ("). WAV로 저장해 주세요.");
        return false;
    }

    ComPtr<IMFMediaType> input;
    hr = MFCreateMediaType (&input);

    if (SUCCEEDED (hr)) hr = input->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED (hr)) hr = input->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) sampleRate * 4);
    if (SUCCEEDED (hr)) hr = input->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED (hr)) hr = state->writer->SetInputMediaType (state->stream, input.Get(), nullptr);
    if (SUCCEEDED (hr)) hr = state->writer->BeginWriting();

    if (FAILED (hr))
    {
        error = K ("MP3 인코더를 준비하지 못했습니다(") + hresultText (hr) + K ("). WAV로 저장해 주세요.");
        return false;
    }

    impl = std::move (state);
    return true;
}

bool Mp3FileWriter::write (const float* left, const float* right, int numSamples)
{
    if (impl == nullptr || impl->writer == nullptr)
        return false;

    if (numSamples <= 0)
        return true;

    const DWORD bytes = (DWORD) numSamples * 4;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer (bytes, &buffer);

    if (FAILED (hr))
        return false;

    BYTE* data = nullptr;
    hr = buffer->Lock (&data, nullptr, nullptr);

    if (FAILED (hr))
        return false;

    // 16비트로 줄일 때 TPDF 디더(±1 LSB)를 더해 작은 소리가 거칠어지지 않게 한다.
    auto* pcm = reinterpret_cast<juce::int16*> (data);
    auto& random = impl->dither;

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = (ch == 0 ? left[i] : right[i]) * 32767.0f
                          + (random.nextFloat() - random.nextFloat());
            pcm[i * 2 + ch] = (juce::int16) juce::jlimit (-32768, 32767, juce::roundToInt (x));
        }
    }

    buffer->Unlock();
    hr = buffer->SetCurrentLength (bytes);

    ComPtr<IMFSample> sample;

    if (SUCCEEDED (hr)) hr = MFCreateSample (&sample);
    if (SUCCEEDED (hr)) hr = sample->AddBuffer (buffer.Get());

    // 시각은 100ns 단위. 반올림 오차가 쌓이지 않게 지금까지 쓴 프레임 수에서 매번 계산한다.
    const auto toTime = [this] (LONGLONG frames) { return (LONGLONG) std::llround ((double) frames * 1.0e7 / impl->rate); };
    const LONGLONG start = toTime (impl->framesWritten), end = toTime (impl->framesWritten + numSamples);

    if (SUCCEEDED (hr)) hr = sample->SetSampleTime (start);
    if (SUCCEEDED (hr)) hr = sample->SetSampleDuration (end - start);
    if (SUCCEEDED (hr)) hr = impl->writer->WriteSample (impl->stream, sample.Get());

    if (FAILED (hr))
        return false;

    impl->framesWritten += numSamples;
    return true;
}

bool Mp3FileWriter::finish (juce::String& error)
{
    if (impl == nullptr || impl->writer == nullptr)
    {
        error = K ("MP3 파일이 열려 있지 않습니다.");
        return false;
    }

    const HRESULT hr = impl->writer->Finalize();

    if (FAILED (hr))
    {
        error = K ("MP3 파일을 마무리하지 못했습니다(") + hresultText (hr) + K ("). 디스크 공간을 확인해 주세요.");
        impl.reset();   // 쓰다 만 파일은 지운다
        return false;
    }

    impl->writer.Reset();   // 끝까지 썼으므로 파일을 남긴다
    impl.reset();
    return true;
}

#else

struct Mp3FileWriter::Impl {};
Mp3FileWriter::Mp3FileWriter() = default;
Mp3FileWriter::~Mp3FileWriter() = default;
bool Mp3FileWriter::supportsSampleRate (double) noexcept { return false; }
int Mp3FileWriter::getKbps() const noexcept { return 0; }
bool Mp3FileWriter::open (const juce::File&, double, int, juce::String& error) { error = "MP3 is only available on Windows"; return false; }
bool Mp3FileWriter::write (const float*, const float*, int) { return false; }
bool Mp3FileWriter::finish (juce::String& error) { error = "MP3 is only available on Windows"; return false; }

#endif
