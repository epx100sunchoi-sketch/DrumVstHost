#pragma once

#include "Common.h"

// 녹음 한 번(테이크)은 폴더 하나다.
//   믹스.wav  들리던 그대로(24비트)
//   드럼.wav  볼륨을 적용하기 전의 드럼(32비트 실수) — 다듬기 창에서 다시 섞는다
//   원곡.wav  볼륨을 적용하기 전의 원곡(32비트 실수)
//   정보.xml  녹음할 때의 볼륨·지연, 다듬기 창에서 마지막으로 쓴 값
namespace TakeFiles
{
// 새로 만들 때의 이름(화면 언어에 따라 한국어·영어)
inline juce::String mix()  { return lang::pick ("믹스.wav", "mix.wav"); }
inline juce::String drum() { return lang::pick ("드럼.wav", "drums.wav"); }
inline juce::String song() { return lang::pick ("원곡.wav", "song.wav"); }
inline juce::String info() { return lang::pick ("정보.xml", "info.xml"); }

// 읽을 때: 다른 언어로 녹음한 폴더도 그대로 읽는다.
inline juce::File drumIn (const juce::File& folder) { return dataChild (folder, "드럼.wav", "drums.wav"); }
inline juce::File songIn (const juce::File& folder) { return dataChild (folder, "원곡.wav", "song.wav"); }
inline juce::File infoIn (const juce::File& folder) { return dataChild (folder, "정보.xml", "info.xml"); }
}

struct TakeInfo
{
    double sampleRate = 0.0;
    juce::int64 lengthSamples = 0;
    float drumVolume = 1.0f, songVolume = 0.7f;   // 녹음할 때의 볼륨(0~1, 화면의 0~100)
    float latencyMs = -1.0f;                      // 녹음할 때의 추정 지연
    juce::String songName;
    juce::Time recordedAt;

    // 다듬기 창에서 마지막으로 쓴 값
    bool edited = false;
    float editDrum = 1.0f, editSong = 0.7f;
    int offsetMs = 0;
    juce::int64 trimStart = 0, trimEnd = 0;

    bool load (const juce::File& folder);
    bool save (const juce::File& folder) const;
    double getLengthSeconds() const noexcept   { return sampleRate > 0.0 ? (double) lengthSamples / sampleRate : 0.0; }
};

bool isTakeFolder (const juce::File&);
juce::Array<juce::File> findRecentTakes (const juce::File& recordFolder, int maxCount);

//==============================================================================
// 녹음 다듬기: 드럼·원곡을 메모리에 올려 두고 볼륨·타이밍·구간을 바꿔 가며 미리 듣고 저장한다.
class TakePlayer
{
public:
    static std::shared_ptr<TakePlayer> load (const juce::File& folder, juce::String& error);

    TakeInfo info;

    const juce::File& getFolder() const noexcept               { return folder; }
    double getSampleRate() const noexcept                      { return rate; }
    juce::int64 getLength() const noexcept                     { return length; }
    double getLengthSeconds() const noexcept                   { return (double) length / rate; }
    const std::vector<float>& getDrumPeaks() const noexcept    { return drumPeaks; }
    const std::vector<float>& getSongPeaks() const noexcept    { return songPeaks; }

    // 화면이 바꾸고 오디오 스레드가 읽는 값
    std::atomic<float> drumGain { 1.0f }, songGain { 0.49f };   // 실제 배율
    std::atomic<int> offsetSamples { 0 };                        // + 이면 드럼을 뒤로(늦게) 민다
    std::atomic<juce::int64> trimStart { 0 }, trimEnd { 0 };
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> playing { false };

    void seek (juce::int64 samplePosition) noexcept;

    // 오디오 스레드: 미리 듣기 소리를 더한다. outputRate는 지금 소리 장치의 샘플레이트다.
    // 녹음 때와 다르면(예: 44.1kHz 녹음을 48kHz 장치로 들을 때) 속도·음높이가 바뀌지 않게 변환해서 읽는다.
    void renderAdding (float* left, float* right, int numSamples, double outputRate) noexcept;

    // 완성본 저장. 구간 앞뒤는 짧게 페이드하고, 넘치지 않게 리미터를 건다.
    enum class ExportFormat { wav24, mp3 };
    static constexpr int mp3Kbps = 320;
    bool exportMix (const juce::File& destination, ExportFormat, juce::String& error) const;

private:
    TakePlayer() = default;

    juce::File folder;
    juce::AudioBuffer<float> drum, song;
    double rate = 44100.0;
    juce::int64 length = 0;
    std::vector<float> drumPeaks, songPeaks;

    std::atomic<juce::int64> seekRequest { -1 };
    float currentDrumGain = -1.0f, currentSongGain = -1.0f;   // 오디오 스레드 전용(값을 부드럽게 바꾼다)
    double readFraction = 0.0;                                // 오디오 스레드 전용: 변환해서 읽을 때 position 뒤의 소수 부분
};
