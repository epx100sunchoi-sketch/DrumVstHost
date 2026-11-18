#pragma once

#include <JuceHeader.h>

//==============================================================================
// Windows에 들어 있는 MP3 인코더(Media Foundation)로 스테레오 MP3 파일을 쓴다.
// 따로 설치할 것이 없다. 샘플레이트는 MP3 규격대로 32·44.1·48kHz만 된다.
// 한 스레드에서 open → write … → finish 순서로 부른다. finish 전에 지우면 쓰던 파일을 지운다.
class Mp3FileWriter
{
public:
    Mp3FileWriter();
    ~Mp3FileWriter();

    static bool supportsSampleRate (double sampleRate) noexcept;

    // kbps를 인코더가 받지 않으면 그보다 낮은 표준 값(256·192·160·128)으로 내려 간다. 실제 값은 getKbps().
    bool open (const juce::File& file, double sampleRate, int kbps, juce::String& error);
    bool write (const float* left, const float* right, int numSamples);
    bool finish (juce::String& error);
    int getKbps() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (Mp3FileWriter)
};
