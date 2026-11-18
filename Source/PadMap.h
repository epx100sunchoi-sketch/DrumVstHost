#pragma once

#include <JuceHeader.h>
#include "Engine.h"
#include "Look.h"

//==============================================================================
// 패드 배치: 어떤 전자드럼이든 쓸 수 있게, 모듈이 보내는 음 번호를 드럼 VST가 아는 표준 번호(General MIDI)로 바꾼다.
// 줄마다 드럼 한 부분(Kick, Snare, Hi-hat 닫힘 …)과 보낼 음 번호가 있고, 패드를 쳐서 그 줄에 '받는 음'을 넣는다.
// VST에 그 모듈용 프리셋이 있으면(예: BFD의 Yamaha 맵) 쓸 필요가 없다. 모듈(MIDI 입력 이름)마다 따로 저장한다.
struct PadMap
{
    struct Row
    {
        juce::String name;          // 화면에 보일 이름
        int target = 36;            // 플러그인에 보낼 음 번호(0~127)
        juce::Array<int> sources;   // 모듈이 보내는 음 번호들
    };

    std::vector<Row> rows;
    bool enabled = false;

    PadMap();                                             // 기본 줄(받는 음은 비어 있다)
    bool hasSources() const;
    std::array<juce::uint8, 128> toTable() const;         // [받은 음] = 보낼 음 또는 MidiInbox::keepNote
    juce::String rowContaining (int sourceNote) const;    // 그 음을 받는 줄 이름(없으면 빈 문자열)

    // 설정에 넣는 글자: 줄 순서대로 "보낼음=받는음,받는음;…"
    juce::String toString() const;
    void fromString (const juce::String&);
};

//==============================================================================
// '패드 맞추기' 창의 내용. map을 직접 고치고, 바뀔 때마다 onChanged를 부른다(메시지 스레드).
class PadMapPanel final : public juce::Component,
                          private juce::Timer
{
public:
    PadMapPanel (Engine&, PadMap& mapToEdit, const juce::String& deviceName);
    ~PadMapPanel() override;

    std::function<void()> onChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct RowView;

    void timerCallback() override;
    void refresh();
    void changed();
    void startLearning (int row);
    void assign (int row, int note);

    Engine& engine;
    PadMap& map;

    juce::Label hint, lastHit;
    juce::ToggleButton enableButton;
    juce::TextButton clearAllButton;
    juce::OwnedArray<RowView> rowViews;
    int learningRow = -1, flashedRow = -1;
    juce::uint32 lastStamp = 0, ignoreUntil = 0, flashUntil = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadMapPanel)
};
