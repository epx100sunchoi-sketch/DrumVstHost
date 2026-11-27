#include "TakeEditor.h"
#include "Widgets.h"

namespace
{
constexpr int editorWidth = 860, pad = 24;
constexpr int formatMp3 = 1, formatWav = 2;
int rememberedFormat = formatMp3;   // 앱을 켜 둔 동안 마지막에 고른 완성본 형식(처음엔 MP3: 파일이 작다)

juce::String sizeText (juce::int64 bytes)
{
    return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + "MB";
}

juce::String signedMs (double value)
{
    const int v = juce::roundToInt (value);

    if (v == 0)
        return "0ms";

    return (v > 0 ? "+" : "-") + juce::String (std::abs (v)) + "ms";
}

juce::String describeTake (const TakePlayer& take)
{
    juce::StringArray parts;
    const auto& info = take.info;

    if (info.recordedAt.toMilliseconds() > 0)
    {
        const auto t = info.recordedAt;
        parts.add (juce::String (t.getMonth() + 1) + K ("월 ") + juce::String (t.getDayOfMonth()) + K ("일 ")
                   + juce::String (t.getHours()).paddedLeft ('0', 2) + ":" + juce::String (t.getMinutes()).paddedLeft ('0', 2) + K (" 녹음"));
    }

    parts.add (clockText (take.getLengthSeconds()));

    if (info.songName.isNotEmpty())
        parts.add (info.songName);

    return parts.joinIntoString ("  ·  ");
}
} // namespace

//==============================================================================
// 이름 · [−] 슬라이더 [+] · 값. 버튼을 누르고 있으면 계속 바뀐다.
class StepSlider final : public juce::Component
{
public:
    static constexpr int nameWidth = 84;   // '원곡 드럼'까지 들어가는 이름 칸

    StepSlider (const juce::String& nameToUse, double minimum, double maximum, double buttonStep, double defaultValue,
                bool bipolar, std::function<juce::String (double)> formatter)
        : name (nameToUse), step (buttonStep), format (std::move (formatter))
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (minimum, maximum, 1.0);
        slider.setDoubleClickReturnValue (true, defaultValue);
        slider.setMouseClickGrabsKeyboardFocus (false);
        slider.setWantsKeyboardFocus (false);

        if (bipolar)
            slider.getProperties().set ("bipolar", true);

        slider.onValueChange = [this]
        {
            repaint();

            if (onChange != nullptr)
                onChange (slider.getValue());
        };

        addAndMakeVisible (slider);

        minus.setButtonText (K ("−"));
        plus.setButtonText ("+");

        for (auto* button : { &minus, &plus })
        {
            styleButton (*button);
            button->setRepeatSpeed (400, 70);
            addAndMakeVisible (*button);
        }

        minus.onClick = [this] { slider.setValue (slider.getValue() - step, juce::sendNotificationSync); };
        plus.onClick  = [this] { slider.setValue (slider.getValue() + step, juce::sendNotificationSync); };
    }

    std::function<void (double)> onChange;

    double getValue() const                                     { return slider.getValue(); }
    void setValue (double value, juce::NotificationType notify) { slider.setValue (value, notify); repaint(); }

    void setTooltips (const juce::String& sliderTip, const juce::String& minusTip, const juce::String& plusTip)
    {
        slider.setTooltip (sliderTip);
        minus.setTooltip (minusTip);
        plus.setTooltip (plusTip);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        nameArea = area.removeFromLeft (nameWidth);
        valueArea = area.removeFromRight (70);
        area.removeFromRight (10);
        minus.setBounds (area.removeFromLeft (36).withSizeKeepingCentre (36, 32));
        area.removeFromLeft (12);
        plus.setBounds (area.removeFromRight (36).withSizeKeepingCentre (36, 32));
        area.removeFromRight (12);
        slider.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (ui::font (16.0f));
        g.setColour (ui::text);
        g.drawText (name, nameArea, juce::Justification::centredLeft);

        g.setFont (ui::font (16.0f));
        g.drawText (format (slider.getValue()), valueArea, juce::Justification::centredRight);
    }

private:
    juce::String name;
    double step;
    std::function<juce::String (double)> format;
    juce::Slider slider;
    juce::TextButton minus, plus;
    juce::Rectangle<int> nameArea, valueArea;
};

//==============================================================================
// 원곡(위)·드럼(아래) 파형. 볼륨을 바꾸면 파형 크기도 바뀐다.
// 양 끝의 손잡이를 끌어 남길 구간을 정하고, 나머지를 누르면 그 위치로 이동한다.
class TakeWaveform final : public juce::Component
{
public:
    explicit TakeWaveform (TakePlayer& takeToUse) : take (takeToUse)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    std::function<void()> onRangeChanged;
    std::function<void (juce::int64)> onSeek;

    void updatePosition()
    {
        const auto pos = take.position.load();

        if (pos != shownPosition)
        {
            shownPosition = pos;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        drawRuler (g);

        const auto lanes = getLanes();
        g.setColour (ui::track);
        g.fillRoundedRectangle (lanes.toFloat(), 6.0f);

        const auto songLane = lanes.withHeight (lanes.getHeight() / 2);
        const auto drumLane = lanes.withTrimmedTop (lanes.getHeight() / 2);

        drawPeaks (g, songLane.reduced (0, 7), take.getSongPeaks(), take.songGain.load(), juce::Colour (0xff7d8593));
        drawPeaks (g, drumLane.reduced (0, 7), take.getDrumPeaks(), take.drumGain.load(), ui::accent);

        g.setColour (ui::line);
        g.fillRect (lanes.getX() + 1, songLane.getBottom(), lanes.getWidth() - 2, 1);

        g.setFont (ui::font (12.5f));
        g.setColour (ui::textDim);
        g.drawText (K ("원곡"), songLane.reduced (12, 5), juce::Justification::topLeft);
        g.drawText (K ("드럼"), drumLane.reduced (12, 5), juce::Justification::topLeft);

        // 남기지 않을 부분은 어둡게
        const float xs = xForSample (take.trimStart.load());
        const float xe = xForSample (take.trimEnd.load());
        g.setColour (ui::bg.withAlpha (0.62f));
        g.fillRect (juce::Rectangle<float> ((float) lanes.getX(), (float) lanes.getY(), xs - (float) lanes.getX(), (float) lanes.getHeight()));
        g.fillRect (juce::Rectangle<float> (xe, (float) lanes.getY(), (float) lanes.getRight() - xe, (float) lanes.getHeight()));

        drawHandle (g, xs, lanes, true);
        drawHandle (g, xe, lanes, false);

        const float xp = xForSample (take.position.load());
        g.setColour (juce::Colours::white);
        g.fillRect (xp - 1.0f, (float) lanes.getY() + 3.0f, 2.0f, (float) lanes.getHeight() - 6.0f);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (hitHandle ((float) e.x) != Drag::none ? juce::MouseCursor::LeftRightResizeCursor
                                                               : juce::MouseCursor::PointingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = hitHandle ((float) e.x);

        if (drag == Drag::none)
        {
            drag = Drag::seek;
            seekTo ((float) e.x);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto x = (float) e.x;
        const juce::int64 minLength = juce::jmin (take.getLength(), (juce::int64) take.getSampleRate());

        if (drag == Drag::start)
        {
            take.trimStart = juce::jlimit<juce::int64> (0, take.trimEnd.load() - minLength, sampleForX (x));

            if (take.position.load() < take.trimStart.load())
                take.seek (take.trimStart.load());
        }
        else if (drag == Drag::end)
        {
            take.trimEnd = juce::jlimit<juce::int64> (take.trimStart.load() + minLength, take.getLength(), sampleForX (x));

            if (take.position.load() >= take.trimEnd.load())
                take.seek (take.trimStart.load());
        }
        else if (drag == Drag::seek)
        {
            seekTo (x);
            return;
        }

        repaint();

        if (onRangeChanged != nullptr)
            onRangeChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag = Drag::none;
    }

private:
    enum class Drag { none, start, end, seek };
    static constexpr int rulerHeight = 22;

    juce::Rectangle<int> getLanes() const      { return getLocalBounds().withTrimmedTop (rulerHeight); }
    juce::Rectangle<int> getInner() const      { return getLanes().reduced (10, 0); }

    float xForSample (juce::int64 sample) const
    {
        const auto inner = getInner();
        return (float) inner.getX() + (float) inner.getWidth() * (float) ((double) sample / (double) juce::jmax<juce::int64> (1, take.getLength()));
    }

    juce::int64 sampleForX (float x) const
    {
        const auto inner = getInner();
        const double p = juce::jlimit (0.0, 1.0, (x - (float) inner.getX()) / (double) juce::jmax (1, inner.getWidth()));
        return (juce::int64) (p * (double) take.getLength());
    }

    Drag hitHandle (float x) const
    {
        const float xs = xForSample (take.trimStart.load());
        const float xe = xForSample (take.trimEnd.load());
        const float ds = std::abs (x - xs), de = std::abs (x - xe);

        if (juce::jmin (ds, de) > 10.0f)
            return Drag::none;

        return de <= ds ? Drag::end : Drag::start;
    }

    void seekTo (float x)
    {
        if (onSeek != nullptr)
            onSeek (sampleForX (x));
    }

    void drawRuler (juce::Graphics& g) const
    {
        const double seconds = take.getLengthSeconds();
        const auto inner = getInner();

        if (seconds <= 0.0)
            return;

        const double pixelsPerSecond = inner.getWidth() / seconds;
        double stepSeconds = 600.0;

        for (double candidate : { 1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0, 120.0, 300.0 })
        {
            if (candidate * pixelsPerSecond >= 70.0)
            {
                stepSeconds = candidate;
                break;
            }
        }

        g.setFont (ui::font (12.5f));

        for (double t = 0.0; t <= seconds + 0.001; t += stepSeconds)
        {
            const float x = (float) inner.getX() + (float) (t * pixelsPerSecond);
            g.setColour (ui::outline);
            g.fillRect (x, (float) rulerHeight - 6.0f, 1.0f, 6.0f);
            g.setColour (ui::textDim);
            g.drawText (clockText (t), juce::Rectangle<float> (x + 3.0f, 0.0f, 60.0f, (float) rulerHeight - 4.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    void drawPeaks (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<float>& peaks, float gain, juce::Colour colour) const
    {
        const auto inner = getInner();
        const int bins = (int) peaks.size();

        if (bins == 0 || inner.getWidth() <= 0)
            return;

        const float centre = (float) area.getCentreY();
        const float half = (float) area.getHeight() * 0.5f;
        g.setColour (colour);

        for (int x = 0; x < inner.getWidth(); ++x)
        {
            const int from = (int) ((juce::int64) x * bins / inner.getWidth());
            const int to = juce::jmax (from + 1, (int) ((juce::int64) (x + 1) * bins / inner.getWidth()));
            float peak = 0.0f;

            for (int b = from; b < juce::jmin (to, bins); ++b)
                peak = juce::jmax (peak, peaks[(size_t) b]);

            const float h = juce::jmax (0.5f, juce::jmin (1.0f, peak * gain) * half);
            g.fillRect ((float) (inner.getX() + x), centre - h, 1.0f, h * 2.0f);
        }
    }

    static void drawHandle (juce::Graphics& g, float x, juce::Rectangle<int> lanes, bool isStart)
    {
        g.setColour (ui::text);
        g.fillRect (x - 1.0f, (float) lanes.getY(), 2.0f, (float) lanes.getHeight());

        const juce::Rectangle<float> tab (isStart ? x - 1.0f : x - 11.0f, (float) lanes.getY(), 12.0f, 26.0f);
        g.fillRoundedRectangle (tab, 3.0f);

        g.setColour (ui::bg.withAlpha (0.7f));

        for (int i = -1; i <= 1; ++i)
            g.fillRect (tab.getCentreX() - 3.0f, tab.getCentreY() + (float) i * 4.0f - 0.5f, 6.0f, 1.0f);
    }

    TakePlayer& take;
    Drag drag = Drag::none;
    juce::int64 shownPosition = -1;
};

//==============================================================================
//==============================================================================
// '연주.mid' 표: 끌어서 이지드러머 Song Track·DAW에 놓는다(파일을 넘기는 끌기). 누르면 탐색기에서 그 파일을 보여 준다.
class TakeEditor::MidiChip final : public juce::Component,
                                   public juce::SettableTooltipClient
{
public:
    juce::File file;

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (0.5f, 3.5f);
        g.setColour (hover ? ui::controlHi : ui::control);
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (ui::accent.withAlpha (hover ? 0.9f : 0.6f));
        g.drawRoundedRectangle (area, 6.0f, 1.2f);

        // 끌 수 있다는 표시(점 여섯 개)
        g.setColour (ui::textDim);
        const float x0 = area.getX() + 12.0f, yMid = area.getCentreY();

        for (int column = 0; column < 2; ++column)
            for (int row = -1; row <= 1; ++row)
                g.fillEllipse (x0 + column * 5.0f, yMid + row * 5.0f - 1.5f, 3.0f, 3.0f);

        g.setColour (ui::text);
        g.setFont (ui::font (15.0f, true));
        g.drawText (file.getFileName(), area.withTrimmedLeft (30.0f).withTrimmedRight (8.0f), juce::Justification::centredLeft, true);
    }

    void mouseEnter (const juce::MouseEvent&) override   { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override    { hover = false; repaint(); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging || e.getDistanceFromDragStart() < 6 || ! file.existsAsFile())
            return;

        dragging = true;
        juce::Component::SafePointer<MidiChip> safe (this);
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, [safe]
        {
            if (safe != nullptr)
                safe->dragging = false;
        });
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasDraggedSinceMouseDown() && file.existsAsFile())
            file.revealToUser();
    }

private:
    bool hover = false, dragging = false;
};

TakeEditor::TakeEditor (Engine& engineToUse, std::shared_ptr<TakePlayer> takeToUse)
    : engine (engineToUse), take (std::move (takeToUse))
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    const auto& info = take->info;
    const float drumStart = info.edited ? info.editDrum : info.drumVolume;
    const float songStart = info.edited ? info.editSong : info.songVolume;

    // 파형과 재생
    waveform = std::make_unique<TakeWaveform> (*take);
    waveform->onSeek = [this] (juce::int64 sample)
    {
        const auto start = take->trimStart.load();
        take->seek (juce::jlimit (start, juce::jmax (start, take->trimEnd.load() - 1), sample));
    };
    waveform->onRangeChanged = [this] { rangeChanged(); };
    addAndMakeVisible (*waveform);

    playButton = std::make_unique<IconButton>();
    playButton->setContent (IconButton::Icon::play, K ("들어보기"));
    playButton->setTooltip (K ("지금 값으로 섞은 소리를 들어 봅니다. (스페이스바)"));
    playButton->onClick = [this] { togglePlay(); };
    addAndMakeVisible (*playButton);

    rewindButton = std::make_unique<IconButton>();
    rewindButton->setContent (IconButton::Icon::rewind, K ("처음으로"));
    rewindButton->setTooltip (K ("남길 구간의 처음으로 돌아갑니다."));
    rewindButton->onClick = [this] { take->seek (take->trimStart.load()); };
    addAndMakeVisible (*rewindButton);

    styleLabel (timeLabel, 15.0f, ui::text);
    addAndMakeVisible (timeLabel);

    styleLabel (rangeLabel, 14.0f, ui::textDim);
    rangeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (rangeLabel);

    resetRangeButton.setButtonText (K ("구간 원래대로"));
    styleButton (resetRangeButton, K ("앞뒤로 자른 것을 없애고 녹음 전체를 남깁니다."));
    resetRangeButton.onClick = [this]
    {
        take->trimStart = 0;
        take->trimEnd = take->getLength();
        waveform->repaint();
        rangeChanged();
    };
    addAndMakeVisible (resetRangeButton);

    // 볼륨
    const auto volumeText = [] (double v) { return juce::String (juce::roundToInt (v)); };

    drumVolume = std::make_unique<StepSlider> (K ("드럼"), 0.0, 100.0, 1.0, std::round (drumStart * 100.0f), false, volumeText);
    drumVolume->setValue (std::round (drumStart * 100.0f), juce::dontSendNotification);
    drumVolume->setTooltips (K ("드럼 볼륨입니다. 처음 값은 녹음할 때와 같습니다. 더블클릭하면 처음 값으로 돌아갑니다."),
                             K ("드럼을 조금 작게"), K ("드럼을 조금 크게"));
    drumVolume->onChange = [this] (double) { applyVolumes(); };
    addAndMakeVisible (*drumVolume);

    songVolume = std::make_unique<StepSlider> (K ("원곡"), 0.0, 100.0, 1.0, std::round (songStart * 100.0f), false, volumeText);
    songVolume->setValue (std::round (songStart * 100.0f), juce::dontSendNotification);
    songVolume->setTooltips (K ("원곡 볼륨입니다. 처음 값은 녹음할 때와 같습니다. 더블클릭하면 처음 값으로 돌아갑니다."),
                             K ("원곡을 조금 작게"), K ("원곡을 조금 크게"));
    songVolume->onChange = [this] (double) { applyVolumes(); };
    addAndMakeVisible (*songVolume);

    // 원곡 드럼: 원곡 드럼 파일이 있으면 조절 막대, 없으면 '원곡 드럼 분리' 단추를 보여 준다.
    const float songDrumsStart = info.edited ? info.editSongDrums : 1.0f;
    songDrumsVolume = std::make_unique<StepSlider> (K ("원곡 드럼"), 0.0, 100.0, 1.0, 100.0, false, volumeText);
    songDrumsVolume->setValue (std::round (songDrumsStart * 100.0f), juce::dontSendNotification);
    songDrumsVolume->setTooltips (K ("원곡에 들어 있는 드럼 소리의 크기입니다. 100이면 녹음 그대로, 0이면 원곡 드럼을 뺍니다. 더블클릭하면 100으로 돌아갑니다."),
                                  K ("원곡 드럼을 조금 작게"), K ("원곡 드럼을 조금 크게"));
    songDrumsVolume->onChange = [this] (double) { applyVolumes(); };
    addChildComponent (*songDrumsVolume);

    separateButton.setButtonText (K ("원곡 드럼 분리"));
    styleButton (separateButton, K ("이 녹음의 원곡에서 드럼만 따로 떼어 냅니다. 한 번만 하면 되고, 3분짜리 녹음이면 2분쯤 걸립니다. 끝나면 원곡 드럼을 줄이거나 뺄 수 있습니다."));
    separateButton.onClick = [this]
    {
        if (onSeparateSongDrums != nullptr)
            onSeparateSongDrums();
    };
    addChildComponent (separateButton);

    styleLabel (songDrumsNote, 14.0f, ui::textDim);
    songDrumsNote.setText (K ("원곡에 든 드럼을 떼어 내면 원곡 드럼만 줄이거나 뺄 수 있습니다."), juce::dontSendNotification);
    addChildComponent (songDrumsNote);
    updateSongDrumsRow();

    // 타이밍
    timing = std::make_unique<StepSlider> (K ("드럼"), -150.0, 150.0, 1.0, 0.0, true, signedMs);
    timing->setValue (info.edited ? (double) info.offsetMs : 0.0, juce::dontSendNotification);
    timing->setTooltips (K ("드럼 소리 전체를 앞(−)이나 뒤(+)로 옮깁니다. 더블클릭하면 0으로 돌아갑니다."),
                         K ("드럼을 1ms 앞으로(빠르게)"), K ("드럼을 1ms 뒤로(늦게)"));
    timing->onChange = [this] (double) { applyTiming(); };
    addAndMakeVisible (*timing);

    if (info.latencyMs > 0.5f)
    {
        const int pull = juce::roundToInt (info.latencyMs);
        latencyPullMs = pull;
        latencyButton.setButtonText (K ("지연만큼 당기기 (") + signedMs (-pull) + ")");
        styleButton (latencyButton, K ("녹음할 때 드럼 소리는 치고 나서 약 ") + juce::String (pull)
                                        + K ("ms 뒤에 났습니다. 들리는 원곡에 맞춰 쳤다면 그만큼 드럼이 늦게 녹음됩니다."));
        latencyButton.onClick = [this, pull] { timing->setValue (-pull, juce::sendNotificationSync); };
        addAndMakeVisible (latencyButton);
    }

    // 저장
    styleLabel (statusLabel, 14.0f, ui::textDim);
    addAndMakeVisible (statusLabel);

    openFolderButton.setButtonText (K ("폴더 열기"));
    styleButton (openFolderButton, K ("이 녹음의 파일이 있는 폴더를 엽니다."));
    openFolderButton.onClick = [this]
    {
        if (lastExport.existsAsFile())
            lastExport.revealToUser();
        else
            take->getFolder().startAsProcess();
    };
    addAndMakeVisible (openFolderButton);

    formatBox.addItem (K ("MP3 (작은 파일)"), formatMp3);
    formatBox.addItem (K ("WAV (원본 음질)"), formatWav);
    formatBox.setSelectedId (rememberedFormat, juce::dontSendNotification);
    formatBox.setTooltip (K ("완성본 파일 형식입니다. MP3(") + juce::String (TakePlayer::mp3Kbps)
                          + K ("kbps)는 WAV의 약 7분의 1 크기라 올리거나 보내기 좋습니다. 다시 편집하거나 원본 음질 그대로 남기려면 WAV를 고르세요."));
    formatBox.onChange = [this] { rememberedFormat = formatBox.getSelectedId(); };
    addAndMakeVisible (formatBox);

    saveButton.setButtonText (K ("완성본 저장"));
    saveButton.setColour (juce::TextButton::buttonColourId, ui::accent);
    saveButton.setColour (juce::TextButton::textColourOffId, ui::onAccent);
    styleButton (saveButton, K ("지금 값으로 섞어서 남길 구간만 저장합니다. 파일 형식(MP3·WAV)은 왼쪽 상자에서 고릅니다."));
    saveButton.onClick = [this] { exportFinal(); };
    addAndMakeVisible (saveButton);

    // 연주 MIDI: 이지드러머 등으로 끌어가 고친 뒤 다시 불러온다
    midiChip = std::make_unique<MidiChip>();
    midiChip->file = TakeFiles::midiIn (take->getFolder());
    midiChip->setTooltip (K ("이지드러머 Song Track이나 DAW로 끌어다 놓으세요. 친 음을 표준 드럼 음 번호(GM)로, 원곡 박자를 템포로 적은 MIDI입니다. 누르면 폴더에서 보여 줍니다."));
    addChildComponent (*midiChip);

    quantizeButton.setButtonText (K ("박자 맞추기…"));
    styleButton (quantizeButton, K ("친 음을 원곡 박 격자(8분~32분음표·셋잇단)에 가까워지게 옮기고, 지금 고른 드럼 VST로 드럼을 다시 만듭니다. 기준은 처음 녹음한 연주이고, 림·엣지·하이햇 페달은 그대로 둡니다."));
    quantizeButton.onClick = [this] { showQuantizeMenu(); };
    addChildComponent (quantizeButton);

    importMidiButton.setButtonText (K ("고친 MIDI 불러오기…"));
    styleButton (importMidiButton, K ("이지드러머 등에서 고친 MIDI 파일로 드럼 소리를 다시 만듭니다. 지금 고른 드럼 VST와 킷을 씁니다. 고친 MIDI를 이 창에 끌어다 놓아도 됩니다."));
    importMidiButton.onClick = [this] { chooseEditedMidi(); };
    addChildComponent (importMidiButton);

    rerenderButton.setButtonText (K ("지금 킷 소리로 다시 만들기"));
    styleButton (rerenderButton, K ("연주는 그대로 두고, 지금 드럼 VST에서 고른 킷 소리로 드럼을 다시 만듭니다. 박자 맞추기나 고친 MIDI로 바꾼 연주가 있으면 그 상태 그대로입니다. 킷이나 드럼 VST를 바꿔 녹음을 다시 들어 보고 싶을 때 쓰세요."));
    rerenderButton.onClick = [this]
    {
        if (! midiBusy && onRerenderKit != nullptr)
            onRerenderKit();
    };
    addChildComponent (rerenderButton);

    restoreDrumsButton.setButtonText (K ("녹음 그대로 되돌리기"));
    styleButton (restoreDrumsButton, K ("고친 MIDI로 다시 만들기 전의 드럼(처음 녹음한 소리)으로 되돌립니다."));
    restoreDrumsButton.onClick = [this]
    {
        if (onRestoreRecordedDrums != nullptr)
            onRestoreRecordedDrums();
    };
    addChildComponent (restoreDrumsButton);

    styleLabel (midiStatus, 14.0f, ui::textDim);
    addAndMakeVisible (midiStatus);
    updateMidiRow();

    // 처음 값 적용
    applyVolumes();
    applyTiming();

    if (info.edited && info.trimEnd > info.trimStart)
    {
        take->trimStart = juce::jlimit<juce::int64> (0, take->getLength(), info.trimStart);
        take->trimEnd = juce::jlimit<juce::int64> (take->trimStart.load() + 1, take->getLength(), info.trimEnd);
    }

    take->seek (take->trimStart.load());
    rangeChanged();
    setStatus (K ("볼륨과 타이밍을 맞춘 뒤 '완성본 저장'을 누르세요. 저장하지 않고 닫으면 바꾼 값은 남지 않습니다."), false);

    setSize (editorWidth, 830);
    startTimerHz (30);
}

TakeEditor::~TakeEditor()
{
    stopTimer();
    take->playing = false;
}

void TakeEditor::stopPreview()
{
    take->playing = false;
}

void TakeEditor::updateSongDrumsRow()
{
    const bool ready = take->hasSongDrums();
    songDrumsVolume->setVisible (ready);
    separateButton.setVisible (! ready);
    songDrumsNote.setVisible (! ready);
    repaint (songDrumsNameArea);
}

void TakeEditor::showSeparationState (const juce::String& text, bool isError, bool busy, const juce::String& details)
{
    separateButton.setEnabled (! busy);
    songDrumsNote.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    songDrumsNote.setText (text, juce::dontSendNotification);
    songDrumsNote.setTooltip (details.isNotEmpty() ? text + "\n" + details : (isError ? text : juce::String()));
}

void TakeEditor::attachSeparatedSongDrums (const juce::File& songDrumsFile)
{
    juce::String error;

    if (! take->attachSongDrums (songDrumsFile, error))
    {
        showSeparationState (error, true, false);
        return;
    }

    updateSongDrumsRow();
    applyVolumes();
    setStatus (K ("원곡 드럼을 분리했습니다. '원곡 드럼'을 줄이면 원곡의 드럼 소리가 작아지고, 0이면 빠집니다."), false);
}

void TakeEditor::saveEditState()
{
    captureEditState (take->info);
    take->info.save (take->getFolder());
}

void TakeEditor::captureEditState (TakeInfo& info) const
{
    info.edited = true;
    info.editDrum = (float) (drumVolume->getValue() / 100.0);
    info.editSong = (float) (songVolume->getValue() / 100.0);
    info.editSongDrums = (float) (songDrumsVolume->getValue() / 100.0);
    info.offsetMs = juce::roundToInt (timing->getValue());
    info.trimStart = take->trimStart.load();
    info.trimEnd = take->trimEnd.load();

    if (info.sampleRate <= 0.0)
        info.sampleRate = take->getSampleRate();

    if (info.lengthSamples <= 0)
        info.lengthSamples = take->getLength();
}

//============================================================================== 연주 MIDI
void TakeEditor::updateMidiRow()
{
    const auto& folder = take->getFolder();
    const auto& info = take->info;
    const bool hasMidi = info.hasMidi && TakeFiles::rawMidiIn (folder).existsAsFile();
    const bool hasGmFile = hasMidi && TakeFiles::midiIn (folder).existsAsFile();

    midiChip->file = TakeFiles::midiIn (folder);
    midiChip->setVisible (hasGmFile);
    quantizeButton.setVisible (hasMidi);
    quantizeButton.setEnabled (! midiBusy);
    importMidiButton.setVisible (hasMidi);
    importMidiButton.setEnabled (! midiBusy);
    rerenderButton.setVisible (hasMidi);
    rerenderButton.setEnabled (! midiBusy);
    restoreDrumsButton.setVisible (TakeFiles::recordedDrumIn (folder).existsAsFile());
    restoreDrumsButton.setEnabled (! midiBusy);

    if (! hasMidi)
    {
        midiTempoText = {};
        midiHintText = K ("이 녹음에는 연주 MIDI가 없습니다. 0.2.4부터는 녹음할 때 연주 MIDI도 함께 남습니다.");
    }
    else
    {
        const auto bars = juce::String (info.midiBeatsPerBar) + "/4";

        midiTempoText = info.midiFromSong
                            ? K ("원곡 박자로 적음 · 약 ") + juce::String (juce::roundToInt (info.midiBpm)) + " BPM · " + bars
                            : K ("원곡 박자를 몰라 120 BPM으로 적음");

        midiHintText = "'" + midiChip->file.getFileName()
                     + K ("' 파일을 이지드러머 Song Track에 끌어다 놓고 박자를 맞추거나(Quantize) 고스트 노트를 넣은 뒤, 고친 MIDI를 이 창에 끌어다 놓으세요. ")
                     + K ("원래 친 음과 짝지어 림·엣지·하이햇 페달은 그대로 살립니다.");

        if (! info.midiFromSong)
            midiHintText += K (" 원곡에 맞춰 고치려면 메트로놈(박자 분석)을 켠 곡으로 녹음하세요.");
    }

    resized();
    repaint();
}

void TakeEditor::showMidiState (const juce::String& text, bool isError, bool busy)
{
    midiBusy = busy;
    midiStatus.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    midiStatus.setText (text, juce::dontSendNotification);
    midiStatus.setTooltip (isError ? text : juce::String());
    saveButton.setEnabled (! busy);
    updateMidiRow();
}

void TakeEditor::showQuantizeMenu()
{
    if (midiBusy || onQuantize == nullptr)
        return;

    // 격자(8분·16분·8분 셋잇단) × 세기. 세기 100%는 격자에 딱, 낮을수록 조금만 끌어당긴다.
    juce::PopupMenu menu;
    menu.addSectionHeader (K ("어느 격자에 끌어당길까요?"));
    static constexpr double steps[] = { 0.5, 0.25, 0.125, 1.0 / 3.0 };
    static constexpr int strengths[] = { 100, 75, 50 };
    const char* names[] = { "8분음표", "16분음표", "32분음표", "8분음표 셋잇단" };

    for (int g = 0; g < 4; ++g)
    {
        juce::PopupMenu sub;

        for (int s = 0; s < 3; ++s)
            sub.addItem (100 + g * 10 + s, K ("세기 ") + juce::String (strengths[s]) + "%"
                                             + (strengths[s] == 100 ? K (" (격자에 딱 맞춤)") : juce::String()));

        menu.addSubMenu (K (names[g]), sub);
    }

    juce::Component::SafePointer<TakeEditor> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (quantizeButton), [safe] (int result)
    {
        if (safe == nullptr || result < 100 || safe->midiBusy || safe->onQuantize == nullptr)
            return;

        const int g = (result - 100) / 10, s = (result - 100) % 10;

        if (g >= 0 && g < 4 && s >= 0 && s < 3)
            safe->onQuantize (steps[g], strengths[s] / 100.0);
    });
}

void TakeEditor::chooseEditedMidi()
{
    if (midiBusy)
        return;

    midiChooser = std::make_unique<juce::FileChooser> (K ("고친 MIDI 파일 고르기"),
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
                                                       "*.mid;*.midi");
    juce::Component::SafePointer<TakeEditor> safe (this);

    midiChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& chooser)
                              {
                                  if (safe == nullptr)
                                      return;

                                  const auto file = chooser.getResult();

                                  if (file.existsAsFile() && safe->onImportMidi != nullptr)
                                      safe->onImportMidi (file);
                              });
}

bool TakeEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && juce::File (files[0]).hasFileExtension ("mid;midi") && take->info.hasMidi;
}

void TakeEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (midiBusy || files.isEmpty())
        return;

    const juce::File file (files[0]);

    // 이 녹음의 연주.mid를 그대로 다시 놓은 경우(끌어가다 이 창에 놓음)는 무시한다
    if (file == TakeFiles::midiIn (take->getFolder()) || file == TakeFiles::rawMidiIn (take->getFolder()))
        return;

    if (onImportMidi != nullptr)
        onImportMidi (file);
}

void TakeEditor::runSnapshotDemo()
{
    if (latencyPullMs > 0)
        timing->setValue (-latencyPullMs, juce::sendNotificationSync);

    take->trimStart = take->getLength() * 8 / 100;
    take->trimEnd = take->getLength() * 92 / 100;
    take->seek (take->getLength() * 35 / 100);
    drumVolume->setValue (90.0, juce::sendNotificationSync);

    if (take->hasSongDrums())
        songDrumsVolume->setValue (40.0, juce::sendNotificationSync);

    rangeChanged();
    waveform->repaint();
}

//==============================================================================
void TakeEditor::applyVolumes()
{
    const auto toGain = [] (double v) { const float x = (float) juce::jlimit (0.0, 1.0, v / 100.0); return x * x; };
    take->drumGain = toGain (drumVolume->getValue());
    take->songGain = toGain (songVolume->getValue());
    take->songDrumAmount = toGain (songDrumsVolume->getValue());   // 원곡 드럼 파일이 없으면 쓰지 않는다
    waveform->repaint();
}

void TakeEditor::applyTiming()
{
    take->offsetSamples = juce::roundToInt (timing->getValue() * take->getSampleRate() / 1000.0);
}

void TakeEditor::rangeChanged()
{
    const double rate = take->getSampleRate();
    const auto start = take->trimStart.load(), end = take->trimEnd.load();
    const bool whole = start <= 0 && end >= take->getLength();

    rangeLabel.setText (whole ? K ("파형 양 끝의 손잡이를 끌면 앞뒤를 자를 수 있습니다")
                              : K ("남길 구간 ") + clockText ((double) start / rate) + " ~ " + clockText ((double) end / rate)
                                    + "  (" + clockText ((double) (end - start) / rate) + ")",
                        juce::dontSendNotification);
    resetRangeButton.setEnabled (! whole);
}

void TakeEditor::togglePlay()
{
    if (take->playing.load())
    {
        take->playing = false;
        return;
    }

    if (engine.isRecording())
    {
        setStatus (K ("녹음하는 동안에는 들어볼 수 없습니다."), true);
        return;
    }

    if (! engine.isAudioDeviceOpen())
    {
        setStatus (K ("소리 장치가 열려 있지 않습니다. 메인 창의 '소리 장치 설정'을 확인하세요."), true);
        return;
    }

    const auto pos = take->position.load();

    if (pos < take->trimStart.load() || pos >= take->trimEnd.load() - 1)
        take->seek (take->trimStart.load());

    engine.pause();   // 메인 창의 원곡 재생과 겹치지 않게
    take->playing = true;
}

void TakeEditor::exportFinal()
{
    const auto folder = take->getFolder();
    const bool asMp3 = formatBox.getSelectedId() != formatWav;
    const auto format = asMp3 ? TakePlayer::ExportFormat::mp3 : TakePlayer::ExportFormat::wav24;
    const auto file = folder.getNonexistentChildFile (folder.getFileName() + lang::pick (" 완성", " final"), asMp3 ? ".mp3" : ".wav", true);

    saveButton.setEnabled (false);
    setStatus (K ("저장하는 중…"), false);

    juce::Component::SafePointer<TakeEditor> safe (this);
    callOnceAfterDelay (30, [safe, file, format]
    {
        if (safe == nullptr)
            return;

        juce::String error;

        if (safe->take->exportMix (file, format, error))
        {
            safe->lastExport = file;
            safe->saveEditState();
            safe->setStatus (K ("저장했습니다: ") + file.getFileName() + " (" + sizeText (file.getSize()) + ")", false);
        }
        else
        {
            safe->setStatus (error, true);
        }

        safe->saveButton.setEnabled (true);
    });
}

void TakeEditor::setStatus (const juce::String& text, bool isError)
{
    statusLabel.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    statusLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
void TakeEditor::timerCallback()
{
    const bool playing = take->playing.load();

    if (playing != shownPlaying)
    {
        shownPlaying = playing;
        playButton->setContent (playing ? IconButton::Icon::pause : IconButton::Icon::play,
                                playing ? K ("멈추기") : K ("들어보기"));
    }

    const double rate = take->getSampleRate();
    const auto text = clockText ((double) take->position.load() / rate) + " / " + clockText (take->getLengthSeconds());

    if (text != shownTime)
    {
        shownTime = text;
        timeLabel.setText (text, juce::dontSendNotification);
    }

    waveform->updatePosition();
}

bool TakeEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }

    return false;
}

void TakeEditor::visibilityChanged()
{
    if (isShowing())
        grabKeyboardFocus();
}

//==============================================================================
void TakeEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);

    g.setColour (ui::text);
    g.setFont (ui::font (19.0f, true));
    g.drawText (K ("녹음 다듬기"), headerArea, juce::Justification::centredLeft);

    g.setColour (ui::textDim);
    g.setFont (ui::font (14.0f));
    g.drawText (describeTake (*take), headerArea, juce::Justification::centredRight);

    g.setColour (ui::line);

    for (int y : dividers)
        g.fillRect (0, y, getWidth(), 1);

    g.setColour (ui::text);
    g.setFont (ui::font (17.0f, true));
    g.drawText (K ("볼륨 맞추기"), volumeTitleArea, juce::Justification::centredLeft);
    g.drawText (K ("타이밍 맞추기"), timingTitleArea, juce::Justification::centredLeft);

    if (! songDrumsVolume->isVisible())   // 조절 막대 대신 '원곡 드럼 분리' 단추가 있을 때의 이름
    {
        g.setFont (ui::font (16.0f));
        g.drawText (K ("원곡 드럼"), songDrumsNameArea, juce::Justification::centredLeft);
    }

    g.setColour (ui::textDim);
    g.setFont (ui::font (14.0f));
    g.drawText (K ("드럼이 원곡보다 늦게(밀려) 들리면 − 쪽으로, 급하게(당겨) 들리면 + 쪽으로 옮기세요."),
                hintArea, juce::Justification::centredLeft);

    g.setColour (ui::text);
    g.setFont (ui::font (17.0f, true));
    g.drawText (K ("연주 MIDI 고치기"), midiTitleArea, juce::Justification::centredLeft);

    g.setColour (ui::textDim);
    g.setFont (ui::font (14.0f));
    g.drawText (midiTempoText, midiTitleArea, juce::Justification::centredRight);
    g.drawFittedText (midiHintText, midiHintArea, juce::Justification::centredLeft, 2, 1.0f);
}

void TakeEditor::resized()
{
    const int width = getWidth() - pad * 2;
    int y = 14;

    headerArea = { pad, y, width, 32 };
    y += 32 + 10;

    waveform->setBounds (pad, y, width, 150);
    y += 150 + 10;

    {
        juce::Rectangle<int> row (pad, y, width, 38);
        playButton->setBounds (row.removeFromLeft (124));
        row.removeFromLeft (8);
        rewindButton->setBounds (row.removeFromLeft (116));
        row.removeFromLeft (14);
        timeLabel.setBounds (row.removeFromLeft (110));
        resetRangeButton.setBounds (row.removeFromRight (124));
        row.removeFromRight (12);
        rangeLabel.setBounds (row);
    }

    y += 38 + 14;
    dividers[0] = y;
    y += 14;

    volumeTitleArea = { pad, y, width, 24 };
    y += 24 + 6;
    drumVolume->setBounds (pad, y, width, 40);
    y += 44;
    songVolume->setBounds (pad, y, width, 40);
    y += 44;

    {
        auto row = juce::Rectangle<int> (pad, y, width, 40);
        songDrumsVolume->setBounds (row);
        songDrumsNameArea = row.removeFromLeft (StepSlider::nameWidth);
        separateButton.setBounds (row.removeFromLeft (150).withSizeKeepingCentre (150, 34));
        row.removeFromLeft (14);
        songDrumsNote.setBounds (row);
    }

    y += 40 + 12;

    dividers[1] = y;
    y += 14;

    timingTitleArea = { pad, y, width, 24 };
    y += 24 + 6;
    timing->setBounds (pad, y, width, 40);
    y += 40 + 6;

    {
        juce::Rectangle<int> row (pad, y, width, 32);

        if (latencyButton.isVisible())
        {
            latencyButton.setBounds (row.removeFromRight (196));
            row.removeFromRight (12);
        }

        hintArea = row;
    }

    y += 32 + 12;
    dividers[2] = y;
    y += 14;

    midiTitleArea = { pad, y, width, 24 };
    y += 24 + 6;

    {
        juce::Rectangle<int> row (pad, y, width, 40);

        if (midiChip->isVisible())
        {
            midiChip->setBounds (row.removeFromLeft (170));
            row.removeFromLeft (8);
        }

        if (quantizeButton.isVisible())
        {
            quantizeButton.setBounds (row.removeFromLeft (140).withSizeKeepingCentre (140, 34));
            row.removeFromLeft (8);
            importMidiButton.setBounds (row.removeFromLeft (180).withSizeKeepingCentre (180, 34));
            row.removeFromLeft (8);
        }

        midiStatus.setBounds (row);
    }

    y += 40 + 4;

    if (rerenderButton.isVisible() || restoreDrumsButton.isVisible())
    {
        juce::Rectangle<int> row (pad, y, width, 40);

        if (rerenderButton.isVisible())
        {
            rerenderButton.setBounds (row.removeFromLeft (200).withSizeKeepingCentre (200, 34));
            row.removeFromLeft (8);
        }

        if (restoreDrumsButton.isVisible())
            restoreDrumsButton.setBounds (row.removeFromLeft (170).withSizeKeepingCentre (170, 34));

        y += 40 + 4;
    }

    midiHintArea = { pad, y, width, 34 };
    y += 34 + 12;
    dividers[3] = y;
    y += 14;

    {
        juce::Rectangle<int> row (pad, y, width, 40);
        saveButton.setBounds (row.removeFromRight (150));
        row.removeFromRight (8);
        formatBox.setBounds (row.removeFromRight (150).withSizeKeepingCentre (150, 34));
        row.removeFromRight (8);
        openFolderButton.setBounds (row.removeFromRight (100));
        row.removeFromRight (12);
        statusLabel.setBounds (row);
    }
}
