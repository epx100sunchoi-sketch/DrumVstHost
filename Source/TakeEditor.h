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
    void stopPreview();
    void saveEditState();                              // 정보.xml에 지금 값을 남긴다
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

    Engine& engine;
    std::shared_ptr<TakePlayer> take;

    std::unique_ptr<TakeWaveform> waveform;
    std::unique_ptr<IconButton> playButton, rewindButton;
    std::unique_ptr<StepSlider> drumVolume, songVolume, timing;
    juce::Label timeLabel, rangeLabel, statusLabel;
    juce::TextButton resetRangeButton, latencyButton, openFolderButton, saveButton;
    juce::ComboBox formatBox;   // 완성본 형식: MP3·WAV
    juce::Rectangle<int> headerArea, volumeTitleArea, timingTitleArea, hintArea;
    int dividers[3] {};
    bool shownPlaying = false;
    int latencyPullMs = 0;
    juce::String shownTime;
    juce::File lastExport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeEditor)
};
