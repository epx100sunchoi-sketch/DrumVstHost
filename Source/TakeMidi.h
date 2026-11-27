#pragma once

#include "Common.h"
#include "DrumModules.h"
#include "SongTools.h"

struct TakeInfo;

//==============================================================================
// 녹음 한 번의 연주 MIDI. 녹음을 멈출 때 녹음 폴더에 두 파일을 쓴다.
//   연주 원본.mid   드럼 모듈이 보낸 그대로(음 번호·하이햇 페달 CC4·초크). '고친 MIDI 불러오기'의 기준이다.
//   연주.mid        이지드러머 등 드럼 VST의 표준 음 번호(General MIDI)로 바꾼 것. 이지드러머 Song Track·DAW로 끌어다 쓴다.
// 두 파일 모두 원곡에서 찾은 박을 템포로 적었고(박마다 템포가 바뀐다), 녹음 안의 첫 마디 첫 박이 1마디 1박이 되게 틱 0을 잡았다.
// 그래서 이지드러머에서 박자를 맞추면(Quantize) 원곡 박에 맞는다. 원곡 박자를 모르면 120BPM 4/4로 적는다.
// 틱 0은 녹음 시각 TakeInfo::midiStart(초)이다(음수일 수 있다: 녹음 시작보다 앞).
//
// 다시 만들기: 고친 MIDI의 음을 박 단위 위치로 읽어 같은 템포로 녹음 시각을 되찾고, 음 높이(GM)마다 원래 친 음과 가까운 것끼리 짝짓는다.
// 짝이 있는 음은 원래 모듈 음 번호(예: 하이햇 엣지, 탐 림)와 그때의 하이햇 페달 값을 그대로 쓰고, 새로 그린 음(고스트 노트 등)은
// 그 GM 음에 맞는 모듈 음 번호로 넣는다. 지운 음은 빠진다. 페달(CC4)·초크는 원래 녹음 그대로 둔다.
namespace takemidi
{
constexpr int ppq = 960;

struct RawEvent
{
    juce::int64 sample = 0;     // 녹음 첫 샘플부터
    juce::uint8 data[3] {};
    juce::uint8 size = 0;
};

// 녹음 중 원곡 위치: takeSample부터 원곡 시간이 songTime에서 시작해 rolling이면 흐르고, 아니면 멈춰 있다.
// 예비박(카운트인)도 흐르는 것으로 센다(원곡 시간이 시작 위치 앞에서 다가온다).
struct SongSpan
{
    juce::int64 takeSample = 0;
    double songTime = 0.0;
    bool rolling = false;
};

struct Recording
{
    double sampleRate = 0.0;
    juce::int64 lengthSamples = 0;
    DrumModule::Brand brand = DrumModule::Brand::none;
    std::vector<RawEvent> events;      // 패드 배치 전(모듈 음 번호), 약해서 버린 신호는 빠져 있다
    std::vector<SongSpan> spans;
    BeatMap beats;                     // 원곡의 박(곡 시간, 초). 없으면 비어 있다
    double clickOffsetSeconds = 0.0;   // 메트로놈 시점(+ 이면 늦게): 연주자가 맞춘 박
    double gridShiftSeconds = 0.0;     // 들리는 박에 맞춰 친 음이 녹음 안에서 박보다 늦게 놓이는 시간(출력 지연 + 신호 대기)
};

// 박 시각표(녹음 시간, 초). times[k]가 k번째 박(틱 k*ppq)이다. 앞뒤는 같은 간격으로 늘려 녹음 전체를 덮는다.
struct Grid
{
    std::vector<double> times;
    int beatsPerBar = 4;
    bool fromSong = false;
    double bpm = 120.0;                // 박 간격의 가운데 값

    double secondsAt (double beat) const noexcept;
    double beatAt (double seconds) const noexcept;
};

Grid makeGrid (const Recording&);

struct Written
{
    double startSeconds = 0.0;
    double bpm = 0.0;
    int beatsPerBar = 4;
    bool fromSong = false;
    int notes = 0;
};

// 녹음 폴더에 두 파일을 쓴다. 친 음이 없으면 쓰지 않고 false(error는 비어 있다).
bool writeTakeMidi (const Recording&, const juce::File& folder, Written&, juce::String& error);

struct Rebuilt
{
    std::vector<RawEvent> events;      // 모듈 음 번호, 녹음 샘플 위치 순서
    int kept = 0, moved = 0, added = 0, removed = 0, ignored = 0;
    int barOffset = 0;                 // 고친 MIDI가 몇 마디 밀려 있었는지(0이면 그대로)
};

// 고친 MIDI(연주.mid를 이지드러머 등에서 고친 것)를 녹음과 맞춰 드럼 모듈이 보낸 것처럼 되돌린다.
bool rebuild (const juce::File& takeFolder, const TakeInfo&, const juce::File& editedMidi, Rebuilt&, juce::String& error);

// sourceMidi(연주 원본.mid나 연주 (고친 뒤).mid: 모듈 음 그대로의 파일)를 원곡 박 격자에 끌어당긴다
// (이지드러머 없이 앱 안에서 박자 맞추기). strength 0이면 아무 음도 옮기지 않는다(킷만 바꿔 다시 만들기).
// stepBeats: 격자 간격(박 단위, 8분음표 0.5·16분음표 0.25·8분 셋잇단 1/3), strength: 0~1(1 = 격자에 딱 맞춤).
// 음 켬·끔만 옮기고(표준 번호로 못 바꾸는 음은 그대로), 하이햇 음에는 원래 치던 때의 페달 값을 바로 앞에 넣는다.
bool quantizeRaw (const juce::File& sourceMidi, const TakeInfo&, double stepBeats, double strength, Rebuilt&, juce::String& error);

// events(모듈 음, 샘플 위치)를 rawMidi(연주 원본.mid)와 같은 틀(같은 템포 트랙·틱 기준)로 destination에 쓴다.
// 다시 만들기가 끝날 때마다 '연주 (고친 뒤).mid'를 이렇게 남겨, 킷만 바꿔 다시 만들어도 고친 박자가 유지되게 한다.
bool writeRawFormat (const std::vector<RawEvent>& events, const TakeInfo&, const juce::File& rawMidi,
                     const juce::File& destination, juce::String& error);

// 모듈 음 번호 → General MIDI 드럼 음(-1 = 이 음은 표준 번호로 바꾸지 않는다: 고칠 수 없고 늘 원래대로 남는다)
int toGeneralMidi (DrumModule::Brand, int moduleNote) noexcept;
// General MIDI 음 → 새로 그린 음에 쓸 모듈 음 번호(-1 = 모듈에 없는 악기)
int fromGeneralMidi (DrumModule::Brand, int gmNote) noexcept;
bool isHiHatNote (DrumModule::Brand, int moduleNote) noexcept;
}
