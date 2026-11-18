#pragma once

#include "Common.h"

//==============================================================================
// 원곡 분석(박자·드럼 분리) 결과를 곡마다 한 폴더에 둔다.
//   <앱 폴더>\곡 분석\<곡 이름> <해시>\
//     original.wav   앱이 직접 해독한 원곡(44.1kHz). 분석·분리를 모두 이 파일로 해서 시각이 정확히 맞는다.
//     beats.json     박자(초)·마디 첫 박·BPM
//     no_drums.wav   드럼을 뺀 원곡(Demucs)
//     drums.wav      original − no_drums. 드럼을 켜면 둘의 합이 원곡과 똑같다.
namespace songcache
{
juce::File folderFor (const juce::File& song);
inline juce::File original (const juce::File& folder)   { return folder.getChildFile ("original.wav"); }
inline juce::File beats (const juce::File& folder)      { return folder.getChildFile ("beats.json"); }
inline juce::File rest (const juce::File& folder)       { return folder.getChildFile ("no_drums.wav"); }
inline juce::File drums (const juce::File& folder)      { return folder.getChildFile ("drums.wav"); }
inline juce::File work (const juce::File& folder)       { return folder.getChildFile ("work"); }
bool hasStems (const juce::File& folder);
}

// 분석·분리에 쓸 파이썬(Demucs·librosa가 든 가상환경)을 찾는다. 설정 값 → 앱 폴더 → 알려진 위치 순서.
juce::File findAnalysisPython (const juce::String& configuredPath);

// 분석용 스크립트 폴더(<앱 폴더>\tools).
juce::File toolsFolder();

// 원곡을 해독해 44.1kHz 32비트 WAV로 쓴다(이미 있으면 그대로 둔다).
bool writeAnalysisCopy (const juce::File& song, const juce::File& destination, juce::String& error);

// destination = original − rest. 드럼을 켰을 때 rest + destination이 원곡과 정확히 같다.
bool makeExactDrumStem (const juce::File& original, const juce::File& rest, const juce::File& destination, juce::String& error);

//==============================================================================
struct BeatMap
{
    double bpm = 0.0;
    std::vector<double> beats;      // 초
    std::vector<char> downbeat;     // beats와 길이가 같다. 1이면 마디 첫 박
    juce::String method;

    bool isValid() const noexcept   { return beats.size() >= 4 && beats.size() == downbeat.size(); }
    bool loadJson (const juce::File&);
};

//==============================================================================
// 외부 프로그램(파이썬)을 뒤에서 돌리며 출력에 나오는 "NN%|"를 진행률로 알려 준다.
class ProcessJob final : private juce::Thread
{
public:
    ProcessJob() : juce::Thread ("process job") {}
    ~ProcessJob() override;

    std::function<void (float)> onProgress;                                 // 메시지 스레드, 0..1
    std::function<void (bool ok, const juce::String& outputTail)> onFinished;   // 메시지 스레드

    bool start (const juce::StringArray& command);
    void cancel();
    bool isRunning() const                                       { return isThreadRunning(); }

private:
    void run() override;

    juce::ChildProcess process;
    std::atomic<bool> cancelled { false };

    JUCE_DECLARE_WEAK_REFERENCEABLE (ProcessJob)
};
