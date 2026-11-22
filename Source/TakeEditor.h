#pragma once

#include <JuceHeader.h>
#include "Engine.h"
#include "Look.h"

class TakeWaveform;
class StepSlider;
class IconButton;

//==============================================================================
// 녹음 다듬기 창의 내용. 원곡·드럼 볼륨과 드럼 타이밍, 남길 구간을 바꿔 미리 듣고 완성본을 저장한다.
class TakeEditor final : public juce::Component,
                         private juce::Timer
{
public:
    TakeEditor (Engine&, std::shared_ptr<TakePlayer>);
    ~TakeEditor() override;

    const juce::File& getTakeFolder() const noexcept   { return take->getFolder(); }
    bool hasSongDrums() const noexcept                 { return take->hasSongDrums(); }
    void stopPreview();

    // '원곡 드럼 분리'(원곡 드럼 파일이 없는 예전 녹음). 메인 창이 분리 작업을 돌리고 결과를 알려 준다.
    std::function<void()> onSeparateSongDrums;
    void showSeparationState (const juce::String& text, bool isError, bool busy, const juce::String& details = {});
    void attachSeparatedSongDrums (const juce::File& songDrumsFile);
    void saveEditState();                              // 정보.xml에 지금 값을 남긴다('완성본 저장'이 끝났을 때만 부른다)
    void runSnapshotDemo();                            // 확인용: 지연만큼 당기기 + 앞뒤 자르기를 해 둔다

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;

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
    int dividers[3] {};
    bool shownPlaying = false;
    int latencyPullMs = 0;
    juce::String shownTime;
    juce::File lastExport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeEditor)
};
