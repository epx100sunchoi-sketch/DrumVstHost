#pragma once

#include <JuceHeader.h>
#include "Engine.h"
#include "Look.h"

class TakeWaveform;
class StepSlider;
class IconButton;

//==============================================================================
// 녹음 다듬기 창의 내용. 원곡·드럼 볼륨과 드럼 타이밍, 남길 구간을 바꿔 미리 듣고 완성본을 저장한다.
// 연주 MIDI가 있는 녹음이면 이지드러머 등으로 끌어가 고친 뒤, 고친 MIDI로 드럼 소리를 다시 만들 수 있다(메인 창이 한다).
class TakeEditor final : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer
{
public:
    TakeEditor (Engine&, std::shared_ptr<TakePlayer>);
    ~TakeEditor() override;

    const juce::File& getTakeFolder() const noexcept   { return take->getFolder(); }
    TakePlayer& getTake() const noexcept               { return *take; }
    bool hasSongDrums() const noexcept                 { return take->hasSongDrums(); }
    void stopPreview();

    // '원곡 드럼 분리'(원곡 드럼 파일이 없는 예전 녹음). 메인 창이 분리 작업을 돌리고 결과를 알려 준다.
    std::function<void()> onSeparateSongDrums;
    void showSeparationState (const juce::String& text, bool isError, bool busy, const juce::String& details = {});
    void attachSeparatedSongDrums (const juce::File& songDrumsFile);
    void saveEditState();                              // 정보.xml에 지금 값을 남긴다('완성본 저장'이 끝났을 때만 부른다)
    void captureEditState (TakeInfo&) const;           // 지금 값(볼륨·타이밍·구간)을 적기만 한다(다시 열 때 이어 쓰려고)
    void runSnapshotDemo();                            // 확인용: 지연만큼 당기기 + 앞뒤 자르기를 해 둔다

    // 연주 MIDI: 고친 MIDI 파일로 드럼을 다시 만들기, 앱 안 박자 맞추기, 녹음 그대로 되돌리기. 메인 창이 작업을 돌리고 결과를 알려 준다.
    std::function<void (const juce::File&)> onImportMidi;
    std::function<void (double stepBeats, double strength)> onQuantize;
    std::function<void()> onRestoreRecordedDrums;
    void showMidiState (const juce::String& text, bool isError, bool busy);
    void setStatusText (const juce::String& text, bool isError)   { setStatus (text, isError); }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void togglePlay();
    void applyVolumes();
    void applyTiming();
    void rangeChanged();
    void exportFinal();
    void setStatus (const juce::String& text, bool isError);
    void updateSongDrumsRow();

    Engine& engine;
    std::shared_ptr<TakePlayer> take;

    std::unique_ptr<TakeWaveform> waveform;
    std::unique_ptr<IconButton> playButton, rewindButton;
    std::unique_ptr<StepSlider> drumVolume, songVolume, songDrumsVolume, timing;
    juce::Label timeLabel, rangeLabel, statusLabel, songDrumsNote;
    juce::TextButton resetRangeButton, latencyButton, openFolderButton, saveButton, separateButton;
    juce::ComboBox formatBox;   // 완성본 형식: MP3·WAV
    juce::Rectangle<int> headerArea, volumeTitleArea, timingTitleArea, hintArea, songDrumsNameArea;
    int dividers[4] {};

    // 연주 MIDI
    class MidiChip;             // 끌어서 이지드러머·DAW에 놓는 '연주.mid'
    std::unique_ptr<MidiChip> midiChip;
    juce::TextButton quantizeButton, importMidiButton, restoreDrumsButton;
    void showQuantizeMenu();
    juce::Label midiStatus;
    juce::Rectangle<int> midiTitleArea, midiHintArea;
    juce::String midiTempoText, midiHintText;
    std::unique_ptr<juce::FileChooser> midiChooser;
    bool midiBusy = false;
    void chooseEditedMidi();
    void updateMidiRow();
    bool shownPlaying = false;
    int latencyPullMs = 0;
    juce::String shownTime;
    juce::File lastExport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeEditor)
};
