#pragma once

#include <JuceHeader.h>
#include "DrumModules.h"
#include "Engine.h"
#include "Look.h"
#include "PadMap.h"
#include "AnalysisTools.h"

class WaveformView;
class ChannelRow;
class IconButton;
class ToolWindow;
class TakeEditor;
class ProcessJob;

//==============================================================================
// 화면 전체. 위에서 아래로 ① 드럼 연결 → ② 드럼 소리 → ③ 원곡 → ④ 볼륨 맞추기 순서다.
class MainComponent final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer
{
public:
    static constexpr int windowWidth = 860;

    MainComponent (Engine&, juce::PropertiesFile&);
    ~MainComponent() override;

    void restoreSession();                       // 지난번에 쓰던 플러그인과 곡을 다시 불러온다
    void saveSession();
    void setAutosave (bool shouldSave) noexcept  { autosave = shouldSave; }
    void saveAllNow();                           // 화면·엔진 설정을 지금 바로 파일에 쓴다(autosave일 때만)
    void setToneOpen (bool drumOpen, bool songOpen);
    void openPluginWindow()                      { showPluginWindow(); }
    void openSong (const juce::File& file)       { loadSong (file); }
    void openTakeEditor (const juce::File& takeFolder);
    TakeEditor* getTakeEditor() const noexcept   { return takeEditor; }
    bool isTakeSeparationBusy() const noexcept   { return takeSeparationBusy; }
    void setRecordFolder (const juce::File& folder);
    juce::Component* openPadMapForTest();
    juce::Component* openMidiMonitorForTest();   // 확인용: '신호 보기' 창을 열고 예시 신호(약해서 버린 탐 포함)를 넣는다
    void offerToolInstallForTest (bool separation)  { offerToolInstall (separation); }        // 확인용: 패드 맞추기 창을 열고(예시 값을 넣어) 그 내용을 돌려준다
    void startRecordingForTest();                // 확인용: 녹음 버튼을 누른 것과 같다
    void stopRecordingForTest();

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void slowRefresh();
    int getIdealHeight() const;
    void updateEnabledStates();

    // ① 소리 장치와 지연
    void refreshBufferControls (bool force);
    void bufferChosen();
    void openDriverPanel();
    juce::String getLatencyHint (juce::Colour& colour) const;
    juce::String describeLatency() const;        // 지연 풍선 도움말: 버퍼·드라이버 여유·패드 신호 대기·플러그인 지연
    void writeLatencyReport();                   // 앱 폴더의 '지연 진단.txt'를 새로 쓴다(내용이 바뀐 경우만)
    juce::String latencyReportSignature;
    void showSettings();

    // ② 드럼 소리
    void populatePluginBox();
    void syncPluginBox();
    void pluginChosen();
    void browseForPlugin();
    void browseForPluginFolder();
    void showRemovePluginMenu();
    void startLoadingPlugin (const juce::String& path, bool openWindowAfter);
    void finishLoadingPlugin (const juce::String& path, bool openWindowAfter);
    void showPluginWindow();
    void setDrumStatus (const juce::String& text, bool isError);

    // ③ 원곡과 녹음
    void browseForSong();
    void loadSong (const juce::File&);
    void togglePlay();
    void toggleRecording();
    void finishRecording();
    void showRecordingsMenu();
    void browseForTake();
    void closeTakeEditor();

    // 다듬기 창의 '원곡 드럼 분리': 녹음 폴더의 원곡.wav를 Demucs로 나눠 원곡 드럼.wav를 만든다(한 번에 한 녹음).
    void startTakeSeparation();
    void runTakeSeparation (const juce::File& folder, const juce::File& input, double takeRate);
    void completeTakeSeparation (const juce::File& folder, const juce::File& separated, double takeRate);
    void finishTakeSeparation (const juce::File& folder, bool ok, const juce::String& message, const juce::String& details);
    void showTakeSeparationState (const juce::File& folder, const juce::String& text, bool isError, bool busy);
    std::unique_ptr<ProcessJob> takeSeparationJob;
    juce::File takeSeparationFolder;
    juce::String takeSeparationText;   // 다듬기 창을 다시 열었을 때 보여 줄 진행 상황
    bool takeSeparationBusy = false;

    // 다듬기 창의 '고친 MIDI 불러오기': 고친 연주 MIDI를 원래 녹음과 맞춘 뒤(TakeMidi.h) 지금 드럼 VST로 드럼.wav를 다시 만든다.
    // 처음 녹음한 드럼은 '드럼 (녹음 그대로).wav'로 남기고, 끝나면 다듬기 창을 다시 연다(바꾸던 볼륨·타이밍은 이어 쓴다).
    void startMidiRebuild (const juce::File& editedMidi);
    void startMidiQuantize (double stepBeats, double strength);   // 다듬기 창 '박자 맞추기'(takemidi::quantizeRaw)
    using MidiJobPrepare = std::function<bool (const TakeInfo&, const juce::File& folder, takemidi::Rebuilt&, juce::String&)>;
    void beginMidiJob (const juce::String& busyText, MidiJobPrepare prepare);
    void restoreRecordedDrums();
    void reopenTakeEditor (const juce::File& folder, const TakeInfo& carry, const juce::String& message);
    void showMidiProgress (int percent);
    void finishMidiRebuild (const juce::File& folder, bool ok, const juce::String& error, const takemidi::Rebuilt& summary, const TakeInfo& carry);
    class BackgroundJob;
    std::unique_ptr<BackgroundJob> midiJob;
    juce::File midiJobFolder;
    juce::String midiJobText;
    bool midiJobBusy = false;
    std::optional<TakeInfo> pendingEditState;   // 다시 여는 다듬기 창에 이어 쓸 값(pendingEditFolder의 녹음일 때만)
    juce::File pendingEditFolder;
    juce::String pendingEditMessage;

    void setSongStatus (const juce::String& text, bool isError, int secondsToShow = 0);

    // ③ 연습 도구: 메트로놈(원곡 박자 분석), 원곡 드럼 빼기(분리)
    void prepareSongTools();
    void metronomeClicked();
    void songDrumsClicked();
    void startBeatAnalysis (bool askToInstall = false);   // askToInstall: 도구가 없으면 내려받을지 묻는다(사용자가 누른 경우)
    void finishBeatAnalysis (bool ok, const juce::String& output);
    void startSeparation (bool askToInstall = false);

    // 분석 도구(파이썬·박자 찾기·드럼 분리)를 앱 안에서 내려받아 설치한다(AnalysisTools.h).
    bool analysisToolsReady (bool separation);
    void offerToolInstall (bool separation);
    void startToolInstall (bool separation);
    void showToolInstallProgress (const juce::String& stage, float progress);
    void finishToolInstall (bool ok, const juce::String& error);
    void showToolInstallMenu (juce::Component& target);
    bool isInstallingTools() const;
    std::unique_ptr<analysis::Installer> toolInstaller;
    void finishSeparation (bool ok, const juce::String& output);
    void applyStems();
    void ensureAnalysisCopy (std::function<void (bool, const juce::String&)> then);
    bool showBeatMap();                          // 저장된 박자를 엔진에 넣는다. 파일이 망가졌으면 false
    void refreshMetronomeStatus();
    void showMetronomeMenu();

    // 곡마다 기억하는 클릭 빠르기(1·2·0.5)·시점(ms)을 바꾸고 바로 적용한다.
    struct MetronomeTweakValues
    {
        double speed = 1.0;
        int offsetMs = 0;
    };

    void changeMetronomeTweak (std::function<void (MetronomeTweakValues&)> change);
    void setToolStatus (juce::Label&, const juce::String& text, bool isError, const juce::String& details = {});
    juce::File getAnalysisPython();

    //==============================================================================
    Engine& engine;
    juce::PropertiesFile& settings;
    juce::TooltipWindow tooltips { this, 700 };

    juce::Rectangle<int> sections[4];
    juce::Rectangle<int> midiRow, deviceRow, latencyRow, bufferTextArea, testLabelArea;

    // ① 드럼 연결
    juce::TextButton settingsButton, driverButton, monitorButton, padButton, languageButton;

    // 패드 배치(모듈 음 번호 → 드럼 VST 음 번호). 연결된 MIDI 입력 이름마다 따로 기억한다.
    PadMap padMap;
    juce::String padMapDevice;
    bool padMapLoaded = false;
    juce::uint32 drumStatusHoldUntil = 0;   // 이 시각까지는 드럼 상태 글자를 모듈 안내로 바꾸지 않는다
    void loadPadMapFor (const juce::String& device);
    void applyPadMap (bool save);
    void showPadMap();

    // '더 빠르게(실험)': Yamaha 드라이버의 출력 여유를 1~4ms 줄인다(Engine::writeYamahaFasterStep).
    // 몇 ms인지는 장치를 열 때 읽은 값으로 본다. 앱이 알아챈 끊김이 생기면 한 단계 낮춘다(녹음 중이면 끝난 뒤).
    // DTX-PRO에서 4ms는 탐을 칠 때 자글거렸다(앱은 알아채지 못한다: 드라이버→USB 단계의 일이라). 들어 보고 고르게 한다.
    juce::TextButton fastButton;
    bool fastRevertPending = false;
    juce::uint32 fastNoticeUntil = 0;
    juce::String fastNotice;
    bool isYamahaAsio() const;
    int yamahaFasterMs() const;                  // 0 = 드라이버 창의 Mode 그대로
    void setYamahaFasterMs (int fasterMs, bool becauseOfDropouts);
    void showFasterMenu();
    void refreshFastButton();

    // 약한 탐 신호 거르기(크로스토크): 이 세기 이하인 탐 신호는 플러그인에 넘기지 않는다(0 = 끔). '신호 보기' 창에서 바꾼다.
    static constexpr int defaultWeakTomVelocity = 9;
    int weakTomVelocity = defaultWeakTomVelocity;
    void applyWeakTomFilter();
    void setWeakTomVelocity (int maxDroppedVelocity);
    juce::ComboBox bufferBox;
    juce::Label latencyLabel;
    juce::StringArray midiNames;
    DrumModule connectedModule;            // MIDI 입력에서 알아본 드럼 모듈(Yamaha·Roland 등)
    juce::String moduleAdvice;             // 연결 안내(예: Roland가 MIDI만 되는 모드)
    juce::String deviceName, bufferSignature, bufferText, bufferError;
    float latencyMs = -1.0f;
    bool deviceOpen = false, ledLit = false, showingXrunWarning = false;
    juce::uint32 lastNoteCount = 0, ledUntil = 0, xrunWarningUntil = 0, xrunGraceUntil = 0;
    juce::int64 lastXruns = -1;   // 끊김으로 볼 일의 수(엔진의 getProblemCount)
    juce::uint32 lastStreamStarts = 0;

    // Yamaha 드라이버 창의 Mode·Buffer Size(레지스트리)가 바뀐 것을 보면 장치를 한 번 다시 연다(checkYamahaDriverSettings).
    juce::String yamahaPendingSettings, yamahaReopenedFor;
    juce::uint32 yamahaChangedAt = 0;
    void checkYamahaDriverSettings();
    juce::int64 ignoredRestarts = 0;   // 설정 창에서 사용자가 일으킨 장치 재시작(끊김으로 보지 않는다)
    bool wasRecording = false;

    // ② 드럼 소리
    juce::ComboBox pluginBox;
    juce::TextButton editorButton, kickButton, snareButton, hatButton;
    juce::Label drumStatus;
    std::vector<PluginEntry> pluginEntries;
    juce::StringArray knownPluginPaths;    // 마지막으로 훑었을 때의 플러그인(새로 설치된 것을 알아채려고)
    juce::String autoLoadNote;             // 저절로 불러온 플러그인 안내(불러오기가 끝나면 보여 준다)
    bool loadingPlugin = false;
    int pluginScanCounter = 0;
    void checkForNewPlugins();

    // ③ 원곡과 녹음
    juce::TextButton openSongButton, recordingsButton;
    juce::Label songName, timeLabel, songStatus;
    std::unique_ptr<WaveformView> waveform;
    std::unique_ptr<IconButton> playButton, rewindButton, recordButton;
    juce::File recordFolder;
    bool shownPlaying = false, recordStartedPlayback = false;
    juce::uint32 recordStopAt = 0, songStatusUntil = 0;
    juce::String shownTime;

    // ③ 연습 도구
    juce::TextButton metronomeButton, songDrumsButton;
    juce::Slider metronomeVolume;
    juce::Label metronomeStatus, songDrumsStatus;
    std::unique_ptr<ProcessJob> beatJob, separationJob;
    juce::File songCache;
    std::vector<std::function<void (bool, const juce::String&)>> copyWaiters;
    bool copyingOriginal = false, stemsPendingAfterRecording = false;
    int songGeneration = 0;   // 곡을 바꾸면 늘린다. 이전 곡 작업의 결과는 버린다.
    double beatBpm = 0.0;
    double clickSpeed = 1.0;   // 지금 곡의 클릭 빠르기(곡 분석 폴더의 metronome.json)

    // ④ 볼륨 맞추기
    std::unique_ptr<ChannelRow> drumRow, songRow, masterRow;

    std::unique_ptr<ToolWindow> pluginWindow, settingsWindow, takeWindow, monitorWindow, padWindow;
    void showMidiMonitor();
    TakeEditor* takeEditor = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;
    int tick = 0;
    bool autosave = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
