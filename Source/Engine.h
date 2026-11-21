#pragma once

#include <JuceHeader.h>
#include "Common.h"
#include "Dsp.h"
#include "SongTools.h"
#include "Take.h"

class StemPairSource;   // Engine.cpp

//==============================================================================
// 화면이 바꾸고 오디오 스레드가 읽는 채널 설정. 미터 값은 반대 방향으로 흐른다.
struct ChannelSettings
{
    std::atomic<float> volume { 1.0f };                              // 0..1 (화면의 0~100)
    std::atomic<bool>  mute { false };
    std::atomic<float> low { 0.0f }, mid { 0.0f }, high { 0.0f };   // dB
    std::atomic<float> comp { 0.0f };                                // 0..1
    std::atomic<float> peakL { 0.0f }, peakR { 0.0f };               // 마지막으로 읽은 뒤의 최댓값

    bool toneIsNeutral() const noexcept
    {
        return low.load() == 0.0f && mid.load() == 0.0f && high.load() == 0.0f && comp.load() == 0.0f;
    }
};

struct PluginEntry
{
    juce::String name, path;   // name: 드럼 VST면 알아보기 쉬운 이름("EZdrummer 3 (Toontrack)"), 아니면 파일 이름
    bool isDrum = false;       // 드럼 악기로 알아본 것(DrumPlugins.h) 또는 사용자가 드럼 VST로 직접 추가한 것
    bool userAdded = false;    // 사용자가 직접 추가한 파일
    int rank = 1000;           // 저절로 불러올 때의 순서(작을수록 먼저)
};

//==============================================================================
// 드럼에서 온 MIDI를 받아 두었다가 다음 오디오 블록에 놓는다.
//  - 버퍼가 작을 때(3ms 이하): 블록 맨 앞에 바로 놓는다. 평균 반 블록 빨라지고, 흔들림은 3ms 이하라 들리지 않는다.
//  - 버퍼가 클 때: 직전 블록 시점 기준 간격을 그대로 살린다(JUCE 기본 방식). 한 블록 늦지만 박자가 흔들리지 않는다.
class MidiInbox final : public juce::MidiInputCallback
{
public:
    MidiInbox();
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

    // 패드 배치: 모듈이 보낸 음 번호를 플러그인이 아는 번호로 바꿔 넘긴다(음 켬·끔·초크에 적용).
    // map[받은 음] = 보낼 음(0~127), keepNote = 그대로, dropNote = 넘기지 않음. MIDI 스레드가 읽으므로 값 하나씩 원자적으로 바꾼다.
    static constexpr juce::uint8 keepNote = 255, dropNote = 254;
    void setNoteMap (const std::array<juce::uint8, 128>& map, bool enabled) noexcept;

    // 마지막으로 받은 '음 켬': (지금까지 받은 횟수 << 8) | 받은 음 번호(바꾸기 전). 패드 맞추기 창이 읽는다.
    juce::uint32 getLastNoteOn() const noexcept   { return lastNoteOn.load (std::memory_order_relaxed); }

    // 드럼 플러그인에 넘길 신호인지. 음·하이햇 페달(CC4)·위치 CC·초크(폴리 애프터터치)는 받은 순서 그대로 넘기고,
    // 클록·시작/정지 같은 시스템 신호(DTX-PRO는 클록을 늘 보낸다), 킷 바꿈 신호(Program Change·Bank Select)는 거른다.
    // 모듈에서 킷을 바꿀 때 플러그인 프리셋까지 바뀌지 않게 하려는 것이다.
    static bool shouldForward (const juce::uint8* data, int size) noexcept;

    // '신호 보기' 창용: 받은 신호를 모두(거른 것도) 따로 쌓아 둔다. 메시지 스레드가 읽어 간다.
    struct MonitorEvent
    {
        double time = 0.0;          // Time::getMillisecondCounterHiRes
        juce::uint8 data[3] {};
        juce::uint8 size = 0;
        bool forwarded = false;     // 플러그인에 넘겼는지
        juce::uint8 mappedNote = 255;   // 패드 배치로 바꿔 넘긴 음 번호(255 = 바꾸지 않음)
    };

    int readMonitor (MonitorEvent* destination, int maxEvents) noexcept;

    // 오디오 스레드. 쌓인 신호를 블록에 놓고, 가장 오래 기다린 시간(ms)을 돌려준다(없으면 -1).
    // now는 이 콜백이 시작된 시각(Time::getMillisecondCounterHiRes)이다.
    double drainInto (juce::MidiBuffer& destination, int numSamples, double sampleRate, bool immediate, double now) noexcept;

private:
    double lastDrainTime = 0.0;   // 오디오 스레드 전용

    struct Event
    {
        double time = 0.0;
        juce::uint8 data[3] {};
        juce::uint8 size = 0;
    };

    static constexpr int capacity = 2048;
    juce::AbstractFifo fifo { capacity };
    std::array<Event, capacity> events;

    std::array<std::atomic<juce::uint8>, 128> noteMap;
    std::atomic<bool> noteMapOn { false };
    std::atomic<juce::uint32> lastNoteOn { 0 };

    static constexpr int monitorCapacity = 1024;   // 넘치면 새 신호는 버린다(창이 닫혀 있을 때)
    juce::AbstractFifo monitorFifo { monitorCapacity };
    std::array<MonitorEvent, monitorCapacity> monitorEvents;
};

//==============================================================================
// 소리가 지나가는 길 전체: MIDI 입력 → VST 악기 → 드럼 채널, 원곡 파일 → 원곡 채널, 둘을 합쳐 출력/녹음.
class Engine final : private juce::AudioIODeviceCallback
{
public:
    // settings가 있으면 저장된 장치·믹서 값을 불러온다. openAudioDevice가 false면 장치를 열지 않는다(자체 점검용).
    Engine (bool openAudioDevice, juce::PropertiesFile* settings);
    ~Engine() override;

    // 버퍼가 3ms 이하면 MIDI를 블록 맨 앞에 바로 놓는다(MidiInbox 설명 참고).
    static bool usesImmediateMidi (int bufferSize, double rate) noexcept   { return rate > 0.0 && bufferSize <= rate * 0.003; }

    juce::AudioDeviceManager deviceManager;
    ChannelSettings drum, song, master;

    //============================================================================== 소리 장치와 지연
    juce::String getAudioDeviceName() const;
    juce::String getAudioDeviceType() const;
    bool isAudioDeviceOpen() const                                 { return deviceManager.getCurrentAudioDevice() != nullptr; }
    bool isWantedAudioDeviceOpen() const                           { return isWantedDeviceOpen(); }   // 저장된(원하던) 장치가 열렸는지
    double getDeviceSampleRate() const;
    int getBufferSize() const;
    juce::Array<int> getAvailableBufferSizes() const;
    bool setBufferSize (int samples, juce::String& error);
    bool hasDriverPanel() const;
    void showDriverPanel();
    float getOutputLatencyMs() const;                              // 장치가 알려 주는 출력 지연, 장치가 없으면 -1
    float getEstimatedLatencyMs() const;                           // 패드 신호가 도착한 뒤 소리가 나기까지(모듈·USB 제외)
    float getDriverExtraLatencyMs() const;                         // 출력 지연 중 버퍼 말고 드라이버가 더 두는 여유분(ms), 장치가 없으면 -1
    int getXRunCount() const                                       { return deviceManager.getXRunCount(); }
    double getCpuUsage() const                                     { return deviceManager.getCpuUsage(); }
    float takeMaxMidiWaitMs() noexcept                             { return maxMidiWaitMs.exchange (0.0f); }

    // 끊김 감시. JUCE의 끊김 수는 이 앱의 처리가 한 블록보다 길어진 경우만 세므로, 따로 잰다.
    //  - 콜백이 "넘겨준 샘플 수로 계산한 예정 시각"보다 얼마나 늦게 왔는지(최근 32번 중 가장 늦은 것과 비교)
    //  - 이 앱이 콜백 안에서 쓴 시간
    //  - 장치 재시작 횟수(재시작은 그 자체로 소리가 비는 구간이다)
    // 값은 늘기만 한다. 읽는 쪽은 앞뒤 차이를 쓴다. 최댓값만 resetMonitorMaxima()로 비운다.
    struct MonitorSnapshot
    {
        juce::uint32 callbacks = 0, ownOverruns = 0, lateNear = 0, late = 0, nearMiss = 0, likelyDropouts = 0;
        juce::uint32 warmupAnomalies = 0, quietEvents = 0, streamStarts = 0, pluginSkips = 0;
        float maxDurationMs = 0.0f, maxExcessMs = 0.0f, blockMs = 0.0f, headroomMs = 0.0f;
    };
    MonitorSnapshot getMonitor() const noexcept;
    void resetMonitorMaxima() noexcept                             { monResetMax = true; }
    void setQuietFor (double milliseconds) noexcept;              // 그동안 생기는 일은 따로 센다(킷 불러오기 등)
    int getDriverOverloads() const;                                // 드라이버가 알려 준 과부하 수, 알려 주지 않는 드라이버면 -1
    juce::uint32 getProblemCount() const noexcept;                 // 화면 경고용: 처리 초과 + 끊김 가능성 + 재시작

    // 사용자가 소리 장치를 직접 고르기 시작하면 자동 재시도를 멈춘다.
    void stopAudioRetry() noexcept                                 { audioRetriesLeft = 0; }
    // 사용자가 설정 창에서 고른 장치를 앞으로 다시 연결할 장치로 삼는다.
    void adoptCurrentDeviceAsWanted();

    // 1초에 한 번쯤 부른다. 새로 꽂힌 MIDI 장치를 켜고, 필요하면 원하던 소리 장치를 다시 잡아 본다.
    void refreshMidiInputs();
    const juce::StringArray& getMidiInputNames() const noexcept   { return midiInputNames; }
    const juce::Array<juce::MidiDeviceInfo>& getMidiInputDevices() const noexcept { return midiInputDevices; }   // 이름 + 장치 경로
    juce::StringArray getAsioDriverNames();                        // 설치된 ASIO 드라이버(장치가 꺼져 있어도 보인다)
    juce::uint32 getMidiNoteCount() const noexcept                { return midiNoteCount.load(); }
    void playTestNote (int noteNumber, float velocity = 0.8f);

    // 확인용: 드럼 모듈에서 MIDI가 들어온 것처럼 넣는다(실제 패드와 같은 경로).
    // 실제 장치와 같은 잠금 안에서 넣어야 받는 쪽 대기열(쓰는 쪽이 하나여야 함)이 안전하다.
    void injectMidiForTest (const juce::MidiMessage& message)
    {
        const juce::ScopedLock lock (deviceManager.getMidiCallbackLock());
        midiInbox.handleIncomingMidiMessage (nullptr, message);
    }

    // '신호 보기' 창: 드럼 모듈에서 받은 신호를 차례로 꺼낸다(메시지 스레드).
    int readMidiMonitor (MidiInbox::MonitorEvent* destination, int maxEvents) noexcept { return midiInbox.readMonitor (destination, maxEvents); }

    //============================================================================== 플러그인
    // 기본 VST3 폴더와, 다른 위치에서 불러온 적 있는 플러그인을 이름순으로 돌려준다.
    std::vector<PluginEntry> findInstalledPlugins();
    const juce::StringArray& getExtraPluginPaths() const noexcept    { return extraPluginPaths; }   // 직접 추가한 .vst3 파일·폴더
    void removeExtraPluginPath (const juce::String& path)            { extraPluginPaths.removeString (path, true); }
    void forgetPluginState (const juce::String& path)                { pluginStates.erase (path); }

    // 패드 배치(모듈 음 번호 → 플러그인 음 번호)와 패드 맞추기용 마지막 음
    void setPadMap (const std::array<juce::uint8, 128>& map, bool enabled) noexcept   { midiInbox.setNoteMap (map, enabled); }
    juce::uint32 getLastNoteOn() const noexcept                      { return midiInbox.getLastNoteOn(); }
    void addExtraPluginPath (const juce::String& path);

    // state가 nullptr이면 그 플러그인을 마지막으로 썼을 때의 상태(킷·맵 등)를 되살린다.
    // 플러그인을 바꾸면 이전 플러그인의 상태는 따로 기억해 둔다.
    bool loadPlugin (const juce::String& path, const juce::MemoryBlock* state, juce::String& error);
    bool hasRememberedState (const juce::String& path) const       { return pluginStates.find (path) != pluginStates.end(); }
    bool isLoadingPlugin() const noexcept                          { return pluginLoadInProgress; }
    void unloadPlugin();
    juce::AudioPluginInstance* getPlugin() const noexcept         { return plugin.get(); }
    const juce::String& getPluginPath() const noexcept            { return pluginPath; }
    const juce::String& getPluginName() const noexcept            { return pluginName; }
    juce::String describePluginBuses() const;

    //============================================================================== 원곡
    juce::String getSongWildcard();
    bool loadSong (const juce::File& file, juce::String& error);
    bool hasSong() const noexcept                                  { return readerSource != nullptr || stemSource != nullptr; }
    const juce::File& getSongFile() const noexcept                 { return songFile; }
    void play();
    void pause();
    void rewind();
    bool isPlaying() const                                         { return transport.isPlaying() || isCountingIn(); }
    bool isCountingIn() const noexcept                             { return countInRemaining.load() > 0; }
    bool hasSongFinished() const                                   { return transport.hasStreamFinished(); }
    double getPosition() const                                     { return transport.getCurrentPosition(); }
    double getLength() const                                       { return transport.getLengthInSeconds(); }
    void setPosition (double seconds);
    juce::AudioThumbnail& getThumbnail() noexcept                  { return thumbnail; }

    //============================================================================== 원곡 드럼 분리
    // 분리된 파일(드럼을 뺀 나머지 + 드럼)을 원곡 대신 재생하고, 원곡의 드럼만 켜고 끈다.
    // 위치는 그대로 이어지고, 드럼을 켜면 원곡과 똑같이 들린다(drums = 원곡 − 나머지).
    bool useSongStems (const juce::File& rest, const juce::File& drums, juce::String& error);
    bool isUsingStems() const noexcept                             { return stemSource != nullptr; }
    void setSongDrumsOn (bool on);
    bool areSongDrumsOn() const noexcept                           { return songDrumsOn.load(); }

    //============================================================================== 메트로놈
    // 원곡을 분석한 박자에 맞춰 클릭을 낸다. 클릭은 들리기만 하고 녹음에는 들어가지 않는다.
    // clickSpeed: 1 = 찾은 박마다, 2 = 박 사이에도 한 번 더(2배 빠르게), 0.5 = 한 박 걸러(절반). 높은 클릭은 원곡의 마디 첫 박 그대로다.
    void setBeatMap (BeatMap map, double clickSpeed = 1.0);       // 빈 것을 주면 지운다
    bool hasBeatMap() const noexcept                               { return beatMapValid.load(); }
    std::atomic<bool> metronomeOn { false };
    static constexpr float maxMetronomeVolume = 1.5f;              // 화면의 150. 100(예전 최대)보다 약 7dB 크다
    std::atomic<float> metronomeVolume { 0.7f };                   // 0..maxMetronomeVolume (화면의 0~150)
    std::atomic<float> metronomeOffsetMs { 0.0f };                 // + 이면 클릭을 늦게
    std::atomic<bool> countInEnabled { true };                     // 메트로놈을 켜고 재생하면 4박을 먼저 센다

    //============================================================================== 녹음
    // parentFolder 안에 "연주 날짜 시각" 폴더를 만들어 믹스·드럼·원곡을 따로 녹음한다.
    bool startRecording (const juce::File& parentFolder, juce::String& error);
    void stopRecording();
    bool isRecording() const noexcept                              { return recording.load(); }
    double getRecordedSeconds() const noexcept;
    const juce::File& getLastTakeFolder() const noexcept           { return lastTakeFolder; }

    //============================================================================== 녹음 다듬기 미리 듣기
    void setPreview (std::shared_ptr<TakePlayer>);
    TakePlayer* getPreview() const noexcept                        { return preview; }

    //============================================================================== 설정
    void saveState (juce::PropertiesFile&);
    juce::String getSavedPluginPath() const                        { return savedPluginPath; }
    juce::MemoryBlock getSavedPluginState() const;
    juce::String getSavedSongPath() const                          { return savedSongPath; }

    // 오디오 장치 없이 같은 처리 경로를 돌려 본다(자체 점검용).
    void prepareOffline (double sampleRate, int blockSize);
    void renderOffline (int numSamples, float& drumPeak, float& mixPeak);
    void renderOfflineInto (float* left, float* right, int numSamples);   // 출력 그대로(메트로놈 포함)
    int getCountInSamples() const noexcept                         { return countInRemaining.load(); }

private:
    //==============================================================================
    struct Strip
    {
        void prepare (double sampleRate);
        void processTone (float* left, float* right, int numSamples, ChannelSettings&) noexcept;
        void applyFader (float* left, float* right, int numSamples, ChannelSettings&) noexcept;

        mixdsp::ToneEq eq;
        mixdsp::Compressor comp;
        juce::SmoothedValue<float> gain;
    };

    struct OutBus
    {
        int start = 0, count = 0;
    };

    using ThreadedWriter = juce::AudioFormatWriter::ThreadedWriter;

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    void prepare (double sampleRate, int blockSize);
    void processChunk (int offset, int numSamples, float* const* out, int numOut);
    void preparePlugin (juce::AudioPluginInstance&, juce::AudioBuffer<float>& buffer, std::vector<OutBus>& buses) const;
    void openWantedAudioDevice();
    bool isWantedDeviceOpen() const;
    void addMidiFromUi (const juce::MidiMessage&);
    void loadSettings (juce::PropertiesFile&);
    void rememberCurrentPluginState();
    // 오디오 스레드가 한 블록 동안 쓰는 예비박 정보(잠금 안에서 복사해 둔다)
    // 예비박은 언제나 원곡의 4박 길이다. 클릭은 빠르기에 따라 2~8번(2배면 박 사이에도, 절반이면 한 박 걸러).
    static constexpr int maxCountInClicks = 8;

    struct CountInBlock
    {
        int remaining = 0;                                  // 이 블록이 시작될 때 남은 예비박(샘플). 0이면 예비박이 아니다.
        double start = 0.0;                                 // 원곡이 시작될 위치(초)
        int count = 0;                                      // 아래 배열에서 쓰는 개수
        std::array<double, maxCountInClicks> beats {};      // 예비박 클릭 시각(원곡 시간 기준)
        std::array<bool, maxCountInClicks> accent {};       // 마디 첫 박이면 높은 클릭(원곡의 마디와 같은 자리)
    };

    void makeClickSounds();
    int prepareCountIn (double start);
    void renderClicks (const CountInBlock& countIn, bool songRolling, double songTime, int numSamples, float* const* out, int numOut, int outOffset) noexcept;
    void startClick (bool accent, int offsetInBlock) noexcept;
    void updateMonitor (double startMs, double endMs, int numSamples) noexcept;

    //==============================================================================
    juce::AudioPluginFormatManager pluginFormats;
    juce::AudioFormatManager audioFormats;

    const bool usesDevices;   // false: 소리 장치·MIDI 입력을 열지 않는다(자체 점검·화면 확인)
    double sampleRate = 0.0;
    int maxBlock = 0;

    // 쓰고 싶은 소리 장치(저장된 값, 없으면 드럼 모듈의 ASIO 드라이버: Yamaha·Roland 등). 열지 못했으면 드럼이 연결될 때 다시 시도한다.
    std::unique_ptr<juce::XmlElement> wantedAudioState;
    bool wantedAudioOpen = true;
    int audioRetriesLeft = 0;
    bool midiScanned = false;

    // MIDI
    MidiInbox midiInbox;
    juce::MidiBuffer incomingMidi, chunkMidi, uiMidi;
    juce::SpinLock uiMidiLock;
    std::atomic<juce::uint32> midiNoteCount { 0 };
    std::atomic<float> maxMidiWaitMs { 0.0f };
    juce::StringArray midiInputNames;
    juce::Array<juce::MidiDeviceInfo> midiInputDevices;

    juce::StringArray enabledMidiIds;                       // 이 엔진이 켠 MIDI 입력
    juce::MidiDeviceListConnection midiListConnection;      // MIDI 장치가 꽂히거나 빠지면 바로 알려 준다(메시지 스레드)

    // 끊김 감시(오디오 스레드 전용 상태)
    struct MonitorState
    {
        bool needsInit = true, offline = false;
        int callbacksSinceStart = 0, lagCount = 0, lagIndex = 0;
        double startMs = 0.0, t0 = 0.0, samples = 0.0, headroomMs = 0.0, localMaxDuration = 0.0, localMaxExcess = 0.0;
        std::array<double, 32> lags {};
    };

    MonitorState monitor;
    std::atomic<juce::uint32> monCallbacks { 0 }, monOwnOverruns { 0 }, monLateNear { 0 }, monLate { 0 }, monNearMiss { 0 },
                              monLikelyDropouts { 0 }, monWarmupAnomalies { 0 }, monQuietEvents { 0 }, monStreamStarts { 0 },
                              monPluginSkips { 0 };
    std::atomic<float> monMaxDurationMs { 0.0f }, monMaxExcessMs { 0.0f }, monBlockMs { 0.0f }, monHeadroomMs { 0.0f };
    std::atomic<bool> monResetMax { false };
    std::atomic<double> quietUntilMs { 0.0 };

    // 카운트인: 메시지 스레드가 박 시각을 채운 뒤 남은 샘플 수를 넣고, 오디오 스레드가 세다가 0이 되면 원곡을 시작한다.
    // 남은 샘플 수를 바꾸는 쪽은 모두 countInLock 안에서 바꾼다(오디오 스레드는 try-lock).
    juce::SpinLock countInLock;
    std::atomic<int> countInRemaining { 0 };
    std::atomic<bool> renderingOffline { false };   // 자체 점검: 장치가 없어도 renderOffline이 예비박을 센다
    double countInStart = 0.0;                   // 원곡이 시작할 위치(초)
    std::array<double, maxCountInClicks> countInBeats {};   // 예비박 클릭 시각(원곡 시간 기준, 시작 위치보다 앞)
    std::array<bool, maxCountInClicks> countInAccent {};    // 예비박 중 마디 첫 박
    int countInCount = 0;

    // 플러그인
    juce::SpinLock pluginLock;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> pluginBuffer;
    std::vector<OutBus> pluginBuses;
    juce::String pluginPath, pluginName;

    // 원곡
    juce::TimeSliceThread readThread { "song reader" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioThumbnailCache thumbnailCache { 4 };
    juce::AudioThumbnail thumbnail { 256, audioFormats, thumbnailCache };
    juce::File songFile;
    std::unique_ptr<StemPairSource> stemSource;
    std::atomic<bool> songDrumsOn { true };
    juce::SmoothedValue<float> songDrumGain;   // 오디오 스레드 전용

    // 메트로놈(박자표는 메시지 스레드가 바꾸고 오디오 스레드는 try-lock으로 읽는다)
    juce::SpinLock beatLock;
    BeatMap beatMap;                   // 원곡에서 찾은 박(예비박 시각을 정한다)
    BeatMap clickMap;                  // 실제로 클릭을 칠 시각: beatMap을 빠르기(clickSpeed)에 맞게 늘이거나 줄인 것
    double clickSpeed = 1.0;           // 메시지 스레드만 바꾼다
    std::atomic<bool> beatMapValid { false };
    int beatMapFirstDownbeat = 0;      // beatLock 안에서만
    int clickFirstDownbeat = 0;        // beatLock 안에서만: clickMap에서 첫 마디 첫 박의 번호
    int clicksPerBar = 4;              // beatLock 안에서만: 마디 하나의 클릭 수(2배 8, 절반 2)
    size_t nextBeat = 0;               // beatLock 안에서만
    double expectedSongTime = -1.0;    // beatLock 안에서만
    double clickClock = 0.0;           // 오디오 스레드: 앞 블록이 끝난 박 시각(이어지는 블록의 시작)
    bool clickClockValid = false;
    double clickClockShift = 0.0;      // 오디오 스레드: clickClock을 잴 때 쓴 클릭 시점(초)
    std::vector<float> clickAccent, clickNormal;

    struct ClickVoice
    {
        const float* data = nullptr;
        int length = 0, position = 0;  // position이 음수면 이 블록 안에서 나중에 시작
    };

    std::array<ClickVoice, 4> clickVoices;
    juce::AudioBuffer<float> clickBuffer;

    // 믹서
    juce::AudioBuffer<float> drumBuffer, songBuffer, mixBuffer, stemBuffer;
    Strip drumStrip, songStrip;
    juce::SmoothedValue<float> masterGain;
    mixdsp::Limiter limiter;

    // 녹음 다듬기 미리 듣기
    juce::SpinLock previewLock;
    std::shared_ptr<TakePlayer> previewOwner;
    TakePlayer* preview = nullptr;

    // 녹음
    juce::TimeSliceThread recordThread { "recorder" };
    juce::SpinLock recordLock;
    std::unique_ptr<ThreadedWriter> mixWriter, drumWriter, songWriter;
    ThreadedWriter* activeMixWriter = nullptr;
    ThreadedWriter* activeDrumWriter = nullptr;
    ThreadedWriter* activeSongWriter = nullptr;
    std::atomic<bool> recording { false };
    std::atomic<juce::int64> recordedSamples { 0 };
    double recordSampleRate = 0.0;
    juce::File recordFolder, lastTakeFolder;
    TakeInfo recordInfo;

    // 저장돼 있던 값(시작할 때 화면이 가져다 쓴다)
    juce::String savedPluginPath, savedSongPath;
    std::map<juce::String, juce::MemoryBlock> pluginStates;   // 플러그인 경로 → 마지막 상태
    bool pluginLoadInProgress = false;                        // loadPlugin 안(메시지 스레드)
    juce::StringArray extraPluginPaths;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Engine)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
