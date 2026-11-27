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
juce::File rootFolder();                                // <앱 폴더>\곡 분석
}

// 다듬기 창 '원곡 드럼 분리'가 Demucs를 다시 돌리지 않게, 이미 분리해 둔 곡(곡 분석 폴더)을 녹음 자리에 맞춰 쓴다.
// takeSong44(녹음의 원곡, 44.1kHz)와 cacheFolder의 original.wav를 소리로 맞춰 보고(자리 찾기 + 구간별 잔차 확인),
// 거의 전부 맞아떨어질 때만 no_drums.wav를 같은 자리로 잘라 restOut(44.1kHz, 녹음과 같은 길이)에 쓴다.
// 맞지 않는 구간(원곡이 아직 안 나온 앞부분 등)은 녹음 소리를 그대로 두어 '원곡 드럼 = 0'이 되게 한다.
// 녹음 때 원곡 EQ를 썼거나 다른 곡이면 false를 돌려준다(그냥 Demucs로 분리하면 된다). detail에 판단 근거를 적는다.
bool cutAlignedRestForTake (const juce::File& takeSong44, const juce::File& cacheFolder,
                            const juce::File& restOut, juce::String& detail);

// 분석·분리에 쓸 파이썬(Demucs·librosa가 든 가상환경)을 찾는다. 설정 값 → 앱 폴더 → 알려진 위치 순서.
juce::File findAnalysisPython (const juce::String& configuredPath);

// 분석용 스크립트 폴더(<앱 폴더>\tools).
juce::File toolsFolder();

// 원곡을 해독해 44.1kHz 32비트 WAV로 쓴다(이미 있으면 그대로 둔다).
bool writeAnalysisCopy (const juce::File& song, const juce::File& destination, juce::String& error);

// 소리 파일을 targetRate로 바꿔 32비트 실수 WAV로 쓴다(시각은 그대로). 같은 속도면 그대로 옮겨 쓴다.
bool writeResampledCopy (const juce::File& source, const juce::File& destination, double targetRate, juce::String& error);

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
