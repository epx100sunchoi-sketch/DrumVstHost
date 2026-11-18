#include "PadMap.h"
#include "Widgets.h"

namespace
{
constexpr int panelWidth = 720, rowHeight = 30, topHeight = 138, bottomHeight = 52, sidePad = 18;

juce::String noteList (const juce::Array<int>& notes)
{
    juce::StringArray parts;

    for (int n : notes)
        parts.add (juce::String (n));

    return parts.joinIntoString (", ");
}
} // namespace

//==============================================================================
PadMap::PadMap()
{
    // 보낼 음은 General MIDI 드럼 배치다. 드럼 VST는 대부분 이 번호를 기본으로 알아듣는다.
    const auto add = [this] (const juce::String& name, int note) { rows.push_back ({ name, note, {} }); };

    add ("Kick", 36);
    add ("Snare", 38);
    add (K ("Snare 림"), 40);
    add (K ("Snare 크로스스틱"), 37);
    add (K ("Hi-hat 닫힘"), 42);
    add (K ("Hi-hat 열림"), 46);
    add (K ("Hi-hat 페달(밟기)"), 44);
    add ("Tom 1", 48);
    add ("Tom 2", 45);
    add ("Tom 3", 43);
    add ("Tom 4", 41);
    add ("Crash 1", 49);
    add ("Crash 2", 57);
    add ("Ride", 51);
    add (K ("Ride 벨"), 53);
    add ("China", 52);
    add ("Splash", 55);
    add (K ("그 밖의 패드 1"), 56);
    add (K ("그 밖의 패드 2"), 54);
    add (K ("그 밖의 패드 3"), 39);
}

bool PadMap::hasSources() const
{
    return std::any_of (rows.begin(), rows.end(), [] (const Row& row) { return ! row.sources.isEmpty(); });
}

std::array<juce::uint8, 128> PadMap::toTable() const
{
    std::array<juce::uint8, 128> table;
    table.fill (MidiInbox::keepNote);

    for (const auto& row : rows)
        for (int source : row.sources)
            if (source >= 0 && source < 128 && row.target >= 0 && row.target < 128)
                table[(size_t) source] = (juce::uint8) row.target;

    return table;
}

juce::String PadMap::rowContaining (int sourceNote) const
{
    for (const auto& row : rows)
        if (row.sources.contains (sourceNote))
            return row.name;

    return {};
}

juce::String PadMap::toString() const
{
    juce::StringArray parts;

    for (const auto& row : rows)
        parts.add (juce::String (row.target) + "=" + noteList (row.sources).removeCharacters (" "));

    return parts.joinIntoString (";");
}

void PadMap::fromString (const juce::String& text)
{
    const auto parts = juce::StringArray::fromTokens (text, ";", "");

    for (size_t i = 0; i < rows.size() && (int) i < parts.size(); ++i)
    {
        const auto target = parts[(int) i].upToFirstOccurrenceOf ("=", false, false).trim();
        rows[i].sources.clear();

        if (target.containsOnly ("0123456789") && target.isNotEmpty())
            rows[i].target = juce::jlimit (0, 127, target.getIntValue());

        for (const auto& token : juce::StringArray::fromTokens (parts[(int) i].fromFirstOccurrenceOf ("=", false, false), ",", ""))
            if (token.trim().isNotEmpty() && token.trim().containsOnly ("0123456789"))
                rows[i].sources.addIfNotAlreadyThere (juce::jlimit (0, 127, token.getIntValue()));
    }
}

//==============================================================================
struct PadMapPanel::RowView final : public juce::Component
{
    RowView (PadMapPanel& ownerToUse, int indexToUse) : owner (ownerToUse), index (indexToUse)
    {
        styleLabel (name, 15.0f, ui::text);
        addAndMakeVisible (name);

        styleLabel (sources, 15.0f, ui::text);
        addAndMakeVisible (sources);

        // 보낼 음 번호는 눌러서 고칠 수 있다(VST가 다른 번호를 쓸 때).
        target.setFont (ui::font (15.0f));
        target.setColour (juce::Label::textColourId, ui::textDim);
        target.setColour (juce::Label::outlineColourId, ui::line);
        target.setJustificationType (juce::Justification::centred);
        target.setEditable (true, false, false);
        target.setTooltip (K ("드럼 VST에 보낼 음 번호입니다(General MIDI 기준). VST가 다른 번호를 쓰면 눌러서 고칩니다."));
        target.onTextChange = [this]
        {
            auto& row = owner.map.rows[(size_t) index];
            const auto text = target.getText().trim();

            if (text.isNotEmpty() && text.containsOnly ("0123456789"))
                row.target = juce::jlimit (0, 127, text.getIntValue());

            owner.changed();
        };
        addAndMakeVisible (target);

        styleButton (learn, K ("누른 뒤 이 소리를 낼 패드(또는 패드의 한 부분)를 한 번 칩니다. 여러 부분을 같은 소리로 하려면 다시 눌러 하나씩 더합니다."));
        learn.onClick = [this] { owner.startLearning (owner.learningRow == index ? -1 : index); };
        addAndMakeVisible (learn);

        clear.setButtonText (K ("비우기"));
        styleButton (clear, K ("이 줄에 넣은 패드를 지웁니다."));
        clear.onClick = [this]
        {
            owner.map.rows[(size_t) index].sources.clear();
            owner.changed();
        };
        addAndMakeVisible (clear);
    }

    void update (bool learning, bool flashed)
    {
        const auto& row = owner.map.rows[(size_t) index];
        name.setText (row.name, juce::dontSendNotification);
        sources.setText (learning ? K ("패드를 치세요…") : (row.sources.isEmpty() ? juce::String ("-") : noteList (row.sources)),
                         juce::dontSendNotification);
        sources.setColour (juce::Label::textColourId, learning ? ui::accent : (row.sources.isEmpty() ? ui::textDim : ui::text));

        if (! target.isBeingEdited())
            target.setText (juce::String (row.target), juce::dontSendNotification);

        learn.setButtonText (learning ? K ("그만") : K ("쳐서 넣기"));
        clear.setEnabled (! row.sources.isEmpty());

        if (highlighted != (learning || flashed))
        {
            highlighted = learning || flashed;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (highlighted)
        {
            g.setColour (ui::accent.withAlpha (0.14f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.0f, 1.0f), 5.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 2);
        clear.setBounds (area.removeFromRight (78));
        area.removeFromRight (6);
        learn.setBounds (area.removeFromRight (104));
        area.removeFromRight (12);
        target.setBounds (area.removeFromRight (56));
        area.removeFromRight (12);
        name.setBounds (area.removeFromLeft (190));
        sources.setBounds (area);
    }

    PadMapPanel& owner;
    const int index;
    juce::Label name, sources, target;
    juce::TextButton learn, clear;
    bool highlighted = false;
};

//==============================================================================
PadMapPanel::PadMapPanel (Engine& engineToUse, PadMap& mapToEdit, const juce::String& deviceName)
    : engine (engineToUse), map (mapToEdit)
{
    setOpaque (true);

    styleLabel (hint, 14.0f, ui::textDim);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setText (K ("쓰는 전자드럼의 패드가 엉뚱한 소리를 내거나 소리가 안 날 때 씁니다. 줄마다 '쳐서 넣기'를 누르고 그 소리를 낼 패드를 한 번 치면, "
                     "모듈이 보내는 음 번호를 드럼 VST의 표준 번호로 바꿔 넘깁니다. 드럼 VST의 MIDI 맵은 기본(General MIDI)으로 두세요. ")
                  + (deviceName.isNotEmpty() ? K ("이 배치는 '") + deviceName + K ("'에만 쓰이고 저절로 기억됩니다.") : juce::String()),
                  juce::dontSendNotification);
    addAndMakeVisible (hint);

    enableButton.setButtonText (K ("패드 배치 쓰기"));
    enableButton.setToggleState (map.enabled, juce::dontSendNotification);
    enableButton.setMouseClickGrabsKeyboardFocus (false);
    enableButton.setTooltip (K ("끄면 모듈이 보낸 음 번호를 그대로 드럼 VST에 넘깁니다(넣어 둔 배치는 남습니다)."));
    enableButton.onClick = [this]
    {
        map.enabled = enableButton.getToggleState();
        changed();
    };
    addAndMakeVisible (enableButton);

    styleLabel (lastHit, 14.0f, ui::textDim);
    lastHit.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (lastHit);

    clearAllButton.setButtonText (K ("모두 비우기"));
    styleButton (clearAllButton, K ("모든 줄에 넣은 패드를 지우고 보낼 음 번호도 처음 값으로 되돌립니다."));
    clearAllButton.onClick = [this]
    {
        const bool wasEnabled = map.enabled;
        map = PadMap();
        map.enabled = wasEnabled;
        learningRow = -1;
        changed();
    };
    addAndMakeVisible (clearAllButton);

    for (int i = 0; i < (int) map.rows.size(); ++i)
        addAndMakeVisible (rowViews.add (new RowView (*this, i)));

    lastStamp = engine.getLastNoteOn();
    refresh();
    setSize (panelWidth, topHeight + rowHeight * (int) map.rows.size() + bottomHeight);
    startTimerHz (30);
}

PadMapPanel::~PadMapPanel()
{
    stopTimer();
}

void PadMapPanel::paint (juce::Graphics& g)
{
    g.fillAll (ui::surface);

    // 열 이름
    g.setColour (ui::textDim);
    g.setFont (ui::font (13.0f));
    const int y = topHeight - 22;
    const int right = getWidth() - sidePad - 6;
    g.drawText (K ("소리"), sidePad + 6, y, 190, 18, juce::Justification::centredLeft);
    g.drawText (K ("모듈이 보내는 음"), sidePad + 196, y, 200, 18, juce::Justification::centredLeft);
    g.drawText (K ("보낼 음"), right - 78 - 6 - 104 - 12 - 56 - 10, y, 76, 18, juce::Justification::centred);
}

void PadMapPanel::resized()
{
    auto area = getLocalBounds().reduced (sidePad, 0);
    auto top = area.removeFromTop (topHeight);
    top.removeFromTop (12);
    hint.setBounds (top.removeFromTop (78));
    enableButton.setBounds (top.removeFromTop (26).removeFromLeft (220));

    auto bottom = area.removeFromBottom (bottomHeight).reduced (0, 9);
    clearAllButton.setBounds (bottom.removeFromRight (120));
    bottom.removeFromRight (12);
    lastHit.setBounds (bottom);

    for (auto* row : rowViews)
        row->setBounds (area.removeFromTop (rowHeight));
}

void PadMapPanel::refresh()
{
    const bool flashing = juce::Time::getMillisecondCounter() < flashUntil;

    for (int i = 0; i < rowViews.size(); ++i)
        rowViews[i]->update (i == learningRow, flashing && i == flashedRow);

    enableButton.setToggleState (map.enabled, juce::dontSendNotification);
}

void PadMapPanel::changed()
{
    refresh();

    if (onChanged != nullptr)
        onChanged();
}

void PadMapPanel::startLearning (int row)
{
    learningRow = row;
    lastStamp = engine.getLastNoteOn();   // 누르기 전에 친 것은 넣지 않는다
    refresh();
}

void PadMapPanel::assign (int row, int note)
{
    // 한 음은 한 줄에만 둔다(다른 줄에 있었으면 옮긴다).
    for (auto& other : map.rows)
        other.sources.removeAllInstancesOf (note);

    map.rows[(size_t) row].sources.add (note);
    map.rows[(size_t) row].sources.sort();

    if (! map.enabled)
        map.enabled = true;   // 넣었으면 바로 쓴다
}

void PadMapPanel::timerCallback()
{
    const auto stamp = engine.getLastNoteOn();
    const auto now = juce::Time::getMillisecondCounter();

    if (stamp != lastStamp)
    {
        lastStamp = stamp;
        const int note = (int) (stamp & 0xff);

        // 림을 치면 몸통 음이 거의 동시에 한 번 더 오는 모듈이 있다. 처음 것만 받고 0.3초 동안은 넘긴다.
        if (learningRow >= 0 && now >= ignoreUntil)
        {
            const int row = learningRow;
            assign (row, note);
            learningRow = -1;
            ignoreUntil = now + 300;
            flashedRow = row;
            flashUntil = now + 500;
            lastHit.setText (K ("넣었습니다: 음 ") + juce::String (note) + " -> " + map.rows[(size_t) row].name
                             + " (" + juce::String (map.rows[(size_t) row].target) + ")", juce::dontSendNotification);
            changed();
            return;
        }

        // 맞추는 중이 아니면 방금 친 패드가 어느 줄인지 보여 준다.
        flashedRow = -1;

        for (int i = 0; i < (int) map.rows.size(); ++i)
            if (map.rows[(size_t) i].sources.contains (note))
                flashedRow = i;

        flashUntil = now + 350;
        lastHit.setText (K ("방금 받은 음: ") + juce::String (note)
                         + (flashedRow >= 0 ? " -> " + map.rows[(size_t) flashedRow].name + " (" + juce::String (map.rows[(size_t) flashedRow].target) + ")"
                                            : K (" (넣은 줄 없음: 그대로 넘깁니다)")),
                         juce::dontSendNotification);
        refresh();
    }
    else if (flashedRow >= 0 && now >= flashUntil)
    {
        flashedRow = -1;
        refresh();
    }
}
