#include "MainComponent.h"
#include "DrumPlugins.h"
#include "Platform.h"
#include "SongTools.h"
#include "TakeEditor.h"
#include "Widgets.h"

namespace
{
constexpr int contentLeft = 62, sidePad = 24;
constexpr int section1Height = 156, section2Height = 118, section3Height = 256;
const char* const separationModel = "htdemucs";

juce::String lastLineOf (const juce::String& output)
{
    auto lines = juce::StringArray::fromLines (output.replaceCharacter ('\r', '\n'));
    lines.removeEmptyStrings();
    return lines.isEmpty() ? juce::String() : lines[lines.size() - 1].trim();
}

// 분석·분리 도구가 남긴 출력에서 초심자가 알아볼 만한 설명을 고른다.
juce::String explainToolError (const juce::String& output, const char* fallback)
{
    if (output.contains ("too short"))                       return K ("곡이 너무 짧습니다");
    if (output.contains ("silent"))                          return K ("소리가 거의 없는 파일입니다");
    if (output.contains ("no steady beat"))                  return K ("일정한 박자를 찾지 못했습니다");
    if (output.contains ("ModuleNotFoundError"))             return K ("분석 도구 설치가 덜 됐습니다");
    if (output.contains ("model not downloaded"))            return K ("드럼 분리 모델이 이 PC에 없습니다");
    if (output.contains ("MemoryError") || output.contains ("out of memory") || output.contains ("not enough memory"))
        return K ("메모리가 부족합니다. 다른 프로그램을 닫고 다시 눌러 주세요");

    return K (fallback);
}

// 마디 첫 박 표시를 shift박 뒤로 옮긴다(높은 클릭 위치 고치기). 맨 앞은 4박 주기로 채운다.
void shiftDownbeats (BeatMap& map, int shift)
{
    shift = ((shift % 4) + 4) % 4;

    if (shift == 0 || map.downbeat.empty())
        return;

    const auto old = map.downbeat;
    const int n = (int) old.size();

    for (int i = 0; i < n; ++i)
    {
        int from = i - shift;

        if (from < 0)
            from += 4;

        map.downbeat[(size_t) i] = from < n ? old[(size_t) from] : 0;
    }
}

juce::File downbeatShiftFile (const juce::File& cache)   { return cache.getChildFile ("downbeat_shift.txt"); }
int readDownbeatShift (const juce::File& cache)          { return downbeatShiftFile (cache).loadFileAsString().trim().getIntValue(); }

// 곡마다 기억하는 메트로놈 맞춤: 빠르기(1·2·0.5)와 클릭 시점(ms, + 이면 늦게). 곡 분석 폴더의 metronome.json.
struct MetronomeTweak
{
    static constexpr int maxOffsetMs = 2000;   // 느린 곡(30BPM)에서도 한 박까지 옮길 수 있게
    double speed = 1.0;
    int offsetMs = 0;
};

juce::File metronomeTweakFile (const juce::File& cache)  { return cache.getChildFile ("metronome.json"); }

MetronomeTweak readMetronomeTweak (const juce::File& cache)
{
    MetronomeTweak tweak;
    const auto json = juce::JSON::parse (metronomeTweakFile (cache));

    if (json.isObject())
    {
        const double speed = (double) json.getProperty ("speed", 1.0);
        tweak.speed = speed >= 1.5 ? 2.0 : (speed <= 0.75 ? 0.5 : 1.0);
        tweak.offsetMs = juce::jlimit (-MetronomeTweak::maxOffsetMs, MetronomeTweak::maxOffsetMs, (int) json.getProperty ("offsetMs", 0));
    }

    return tweak;
}

void writeMetronomeTweak (const juce::File& cache, const MetronomeTweak& tweak)
{
    if (cache == juce::File() || ! cache.createDirectory())
        return;

    auto* object = new juce::DynamicObject();
    object->setProperty ("speed", tweak.speed);
    object->setProperty ("offsetMs", tweak.offsetMs);
    metronomeTweakFile (cache).replaceWithText (juce::JSON::toString (juce::var (object)));
}

// 박 길이(ms) 기준으로 1/4박 단위에 맞으면 "½박" 같은 박 단위로, 아니면 ms로 보여 준다.
juce::String beatFractionText (int offsetMs, double beatMs)
{
    if (beatMs <= 0.0)
        return {};

    const double quarters = std::abs (offsetMs) / (beatMs * 0.25);
    const int q = juce::roundToInt (quarters);

    if (q == 0 || std::abs (quarters - q) * beatMs * 0.25 > 3.0)   // 3ms 넘게 어긋나면 박 단위가 아니다
        return {};

    const int whole = q / 4, part = q % 4;
    const char* const parts[] = { "", "\xc2\xbc", "\xc2\xbd", "\xc2\xbe" };   // ¼ ½ ¾
    return (whole > 0 ? juce::String (whole) : juce::String()) + juce::String::fromUTF8 (parts[part]) + K ("박");
}

juce::String clickOffsetText (int offsetMs, double beatMs, bool shortForm)
{
    if (offsetMs == 0)
        return shortForm ? juce::String() : K ("원곡에서 찾은 그대로");

    const auto beats = beatFractionText (offsetMs, beatMs);
    const auto direction = offsetMs > 0 ? K (" 늦게") : K (" 빠르게");

    if (shortForm)
        return beats.isNotEmpty() ? beats + direction : (offsetMs > 0 ? "+" : "-") + juce::String (std::abs (offsetMs)) + "ms";

    return (beats.isNotEmpty() ? beats + direction + " (" : juce::String())
         + juce::String (std::abs (offsetMs)) + (beats.isNotEmpty() ? "ms)" : K ("ms") + direction);
}
constexpr int sectionTop = 14, titleHeight = 24, labelColumn = 110;
constexpr int browseId = 100000, rescanId = 100001, browseFolderId = 100002, removeAddedId = 100003, driverPanelId = 1;

// 패드 배치를 설정에 넣고 꺼낸다: <PADMAPS><MAP device="DTX Drums-1" enabled="1" rows="36=35;38=;…"/></PADMAPS>
PadMap readPadMap (juce::PropertiesFile& settings, const juce::String& device)
{
    PadMap map;

    if (const auto all = settings.getXmlValue ("padMaps"))
    {
        for (const auto* entry : all->getChildWithTagNameIterator ("MAP"))
        {
            if (entry->getStringAttribute ("device") == device)
            {
                map.fromString (entry->getStringAttribute ("rows"));
                map.enabled = entry->getBoolAttribute ("enabled", false);
                break;
            }
        }
    }

    return map;
}

void writePadMap (juce::PropertiesFile& settings, const juce::String& device, const PadMap& map)
{
    auto all = settings.getXmlValue ("padMaps");

    if (all == nullptr || ! all->hasTagName ("PADMAPS"))
        all = std::make_unique<juce::XmlElement> ("PADMAPS");

    juce::XmlElement* target = nullptr;

    for (auto* entry : all->getChildWithTagNameIterator ("MAP"))
        if (entry->getStringAttribute ("device") == device)
            target = entry;

    if (target == nullptr)
    {
        target = all->createNewChildElement ("MAP");
        target->setAttribute ("device", device);
    }

    target->setAttribute ("enabled", map.enabled);
    target->setAttribute ("rows", map.toString());
    settings.setValue ("padMaps", all.get());
}

juce::String describeTakeFolder (const juce::File& folder)
{
    TakeInfo info;

    if (! info.load (folder) || info.recordedAt.toMilliseconds() <= 0)
        return folder.getFileName();

    const auto t = info.recordedAt;
    return juce::String (t.getMonth() + 1) + K ("월 ") + juce::String (t.getDayOfMonth()) + K ("일 ")
         + juce::String (t.getHours()).paddedLeft ('0', 2) + ":" + juce::String (t.getMinutes()).paddedLeft ('0', 2)
         + "   (" + clockText (info.getLengthSeconds()) + ")"
         + (info.songName.isNotEmpty() ? "   " + info.songName : juce::String());
}

// Yamaha Steinberg USB 드라이버 창의 Mode ↔ 레지스트리 LatencyMode(HKCU\Software\Yamaha\Yamaha Steinberg USB Driver).
// 2026-11-22 사용자 PC(DTX-PRO, 44.1kHz, 버퍼 96)에서 창으로 바꿔 확인: Low Latency = 1(출력 지연 = 버퍼 + 274샘플),
// Standard = 2(버퍼 + 318샘플). Stable은 3으로 짐작한다. 창에 없는 0은 버퍼 + 98샘플이다(레지스트리로만 켤 수 있다).
// 예전(0.2.2까지)에는 0을 Low Latency로 잘못 알아, 이미 Low Latency인 사용자에게 바꾸라고 안내했다.
juce::String yamahaModeName (const juce::String& latencyMode)
{
    if (latencyMode == "1")   return "Low Latency";
    if (latencyMode == "2")   return "Standard";
    if (latencyMode == "3")   return "Stable";
    if (latencyMode == "0")   return K ("더 빠르게(실험) - 드라이버 창에 없는 값 0");
    return {};
}

constexpr float yamahaLowLatencyExtraMs = 274.0f * 1000.0f / 44100.0f;   // Low Latency에서 드라이버가 버퍼 말고 더 두는 여유
} // namespace

//==============================================================================
// 좌우 두 줄짜리 가로 레벨 미터. 초록 → 주황 → 빨강.
class LevelMeter final : public juce::Component
{
public:
    LevelMeter() { setInterceptsMouseClicks (false, false); }

    void push (float left, float right)
    {
        const auto follow = [] (float shown, float incoming)
        {
            const float next = juce::jmax (incoming, shown * 0.84f);
            return next < 1.0e-4f ? 0.0f : next;
        };

        const float newLeft = follow (levelL, left), newRight = follow (levelR, right);

        if (newLeft != levelL || newRight != levelR)
        {
            levelL = newLeft;
            levelR = newRight;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        const float barHeight = 5.0f, gap = 3.0f;
        const float top = area.getCentreY() - (barHeight * 2.0f + gap) * 0.5f;

        drawBar (g, { area.getX(), top, area.getWidth(), barHeight }, levelL);
        drawBar (g, { area.getX(), top + barHeight + gap, area.getWidth(), barHeight }, levelR);
    }

private:
    static float toProportion (float gain)
    {
        const float db = juce::Decibels::gainToDecibels (gain, -80.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 54.0f) / 54.0f);
    }

    static void drawBar (juce::Graphics& g, juce::Rectangle<float> bar, float level)
    {
        g.setColour (ui::track);
        g.fillRoundedRectangle (bar, 2.5f);

        const float p = toProportion (level);

        if (p <= 0.0f)
            return;

        const float amberFrom = 42.0f / 54.0f, redFrom = 51.0f / 54.0f;   // -12dB, -3dB

        juce::Graphics::ScopedSaveState state (g);
        juce::Path clip;
        clip.addRoundedRectangle (bar, 2.5f);
        g.reduceClipRegion (clip);

        const auto fill = [&] (float from, float to, juce::Colour colour)
        {
            if (p > from)
            {
                g.setColour (colour);
                g.fillRect (bar.getX() + bar.getWidth() * from, bar.getY(),
                            bar.getWidth() * (juce::jmin (p, to) - from), bar.getHeight());
            }
        };

        fill (0.0f, amberFrom, ui::green);
        fill (amberFrom, redFrom, ui::accent);
        fill (redFrom, 1.0f, ui::red);
    }

    float levelL = 0.0f, levelR = 0.0f;
};

//==============================================================================
// 원곡 파형. 지나간 부분은 강조색으로 칠하고, 누르거나 끌면 그 위치로 이동한다.
class WaveformView final : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit WaveformView (juce::AudioThumbnail& thumbnailToUse) : thumbnail (thumbnailToUse)
    {
        thumbnail.addChangeListener (this);
    }

    ~WaveformView() override
    {
        thumbnail.removeChangeListener (this);
    }

    std::function<void (double)> onSeek;

    // 박자 분석이 끝나면 마디 첫 박 자리에 옅은 세로선을 긋는다.
    void setBars (std::vector<double> barTimes)
    {
        bars = std::move (barTimes);
        repaint();
    }

    void setPosition (double seconds, double lengthSeconds, bool songLoaded)
    {
        const double p = lengthSeconds > 0.0 ? juce::jlimit (0.0, 1.0, seconds / lengthSeconds) : 0.0;

        if (songLoaded != hasSong)
            setMouseCursor (songLoaded ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);

        if (std::abs (p - proportion) > 1.0e-5 || songLoaded != hasSong || lengthSeconds != length)
        {
            proportion = p;
            length = lengthSeconds;
            hasSong = songLoaded;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (ui::track);
        g.fillRoundedRectangle (bounds, 6.0f);

        if (! hasSong)
        {
            g.setColour (ui::textDim);
            g.setFont (ui::font (14.5f));
            g.drawText (K ("음악 파일(MP3·WAV·FLAC)을 여기로 끌어다 놓아도 됩니다"), getLocalBounds(), juce::Justification::centred);
            return;
        }

        const auto inner = getInner();
        const double total = thumbnail.getTotalLength();

        if (length > 0.0 && ! bars.empty())
        {
            g.setColour (ui::text.withAlpha (0.13f));

            for (double t : bars)
                g.fillRect ((float) inner.getX() + (float) (inner.getWidth() * t / length), bounds.getY() + 2.0f, 1.0f, bounds.getHeight() - 4.0f);
        }

        if (total > 0.0)
        {
            g.setColour (juce::Colour (0xff4c525d));
            thumbnail.drawChannel (g, inner, 0.0, total, 0, 1.0f);

            const int played = juce::roundToInt (inner.getWidth() * proportion);

            if (played > 0)
            {
                juce::Graphics::ScopedSaveState state (g);
                g.reduceClipRegion (inner.withWidth (played));
                g.setColour (ui::accent);
                thumbnail.drawChannel (g, inner, 0.0, total, 0, 1.0f);
            }
        }

        const float x = (float) inner.getX() + (float) inner.getWidth() * (float) proportion;
        g.setColour (ui::text);
        g.fillRect (x - 1.0f, bounds.getY() + 4.0f, 2.0f, bounds.getHeight() - 8.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override { seekTo (e.x); }
    void mouseDrag (const juce::MouseEvent& e) override { seekTo (e.x); }

private:
    juce::Rectangle<int> getInner() const { return getLocalBounds().reduced (10, 6); }

    void seekTo (int x)
    {
        if (! hasSong || length <= 0.0)
            return;

        const auto inner = getInner();
        const double p = juce::jlimit (0.0, 1.0, (x - inner.getX()) / (double) juce::jmax (1, inner.getWidth()));

        if (onSeek != nullptr)
            onSeek (p * length);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    juce::AudioThumbnail& thumbnail;
    double proportion = 0.0, length = 0.0;
    bool hasSong = false;
    std::vector<double> bars;
};

//==============================================================================
// 노브 하나 + 이름 + 현재 값. 가운데 값(0)이면 효과가 없다.
class ToneKnob final : public juce::Component
{
public:
    ToneKnob (const juce::String& nameToUse, bool isBipolar, std::atomic<float>& targetToUse, const juce::String& tooltip)
        : name (nameToUse), bipolar (isBipolar), target (targetToUse)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        slider.setMouseClickGrabsKeyboardFocus (false);
        slider.setWantsKeyboardFocus (false);

        if (bipolar)
        {
            slider.setRange (-12.0, 12.0, 0.5);
            slider.getProperties().set ("bipolar", true);
        }
        else
        {
            slider.setRange (0.0, 100.0, 1.0);
        }

        slider.setDoubleClickReturnValue (true, 0.0);
        slider.setTooltip (tooltip + K (" 더블클릭하면 0으로 돌아갑니다."));
        refresh();

        slider.onValueChange = [this]
        {
            target = (float) (bipolar ? slider.getValue() : slider.getValue() / 100.0);
            repaint();

            if (onChange != nullptr)
                onChange();
        };

        addAndMakeVisible (slider);
    }

    std::function<void()> onChange;

    void refresh()
    {
        slider.setValue (bipolar ? (double) target.load() : std::round ((double) target.load() * 100.0), juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        slider.setBounds (0, 0, getHeight(), getHeight());
    }

    void paint (juce::Graphics& g) override
    {
        const int textX = getHeight() + 8;
        const int centre = getHeight() / 2;
        const double value = slider.getValue();

        g.setFont (ui::font (15.0f));
        g.setColour (ui::text);
        g.drawText (name, textX, centre - 20, getWidth() - textX, 20, juce::Justification::centredLeft);

        juce::String valueText ("0");

        if (value != 0.0)
            valueText = bipolar ? (value > 0.0 ? "+" : "") + juce::String (value, 1) : juce::String (juce::roundToInt (value));

        g.setFont (ui::font (13.5f));
        g.setColour (value != 0.0 ? ui::accent : ui::textDim);
        g.drawText (valueText, textX, centre + 1, getWidth() - textX, 18, juce::Justification::centredLeft);
    }

private:
    juce::String name;
    bool bipolar;
    std::atomic<float>& target;
    juce::Slider slider;
};

//==============================================================================
// 믹서 한 줄: 이름 · 음소거 · 볼륨 · 숫자 · 미터 · 소리 다듬기.
class ChannelRow final : public juce::Component
{
public:
    static constexpr int rowHeight = 44, toneHeight = 62;

    ChannelRow (const juce::String& nameToUse, ChannelSettings& settingsToUse, bool fullControls, double defaultVolume)
        : name (nameToUse), settings (settingsToUse), full (fullControls)
    {
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 100.0, 1.0);
        volume.setDoubleClickReturnValue (true, defaultVolume);
        volume.setValue (std::round ((double) settings.volume.load() * 100.0), juce::dontSendNotification);
        volume.setMouseClickGrabsKeyboardFocus (false);
        volume.setWantsKeyboardFocus (false);
        volume.setTooltip (K ("끌어서 볼륨을 조절합니다. 더블클릭하면 처음 값으로 돌아갑니다."));
        volume.onValueChange = [this]
        {
            settings.volume = (float) (volume.getValue() / 100.0);
            repaint();
        };

        addAndMakeVisible (volume);
        addAndMakeVisible (meter);

        if (! full)
            return;

        muteButton.setButtonText (K ("음소거"));
        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, ui::red);
        muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        styleButton (muteButton, K ("이 소리만 잠깐 끕니다."));
        muteButton.onClick = [this]
        {
            settings.mute = muteButton.getToggleState();
            repaint();
        };
        addAndMakeVisible (muteButton);

        toneButton.setButtonText (K ("소리 다듬기"));
        toneButton.setClickingTogglesState (true);
        toneButton.setColour (juce::TextButton::buttonOnColourId, ui::controlHi);
        toneButton.setColour (juce::TextButton::textColourOnId, ui::text);
        styleButton (toneButton, K ("저음·중음·고음(EQ)과 컴프레서를 조절합니다. 모두 0이면 원래 소리 그대로입니다."));
        toneButton.onClick = [this]
        {
            toneOpen = toneButton.getToggleState();
            updateToneVisibility();

            if (onLayoutChanged != nullptr)
                onLayoutChanged();
        };
        addAndMakeVisible (toneButton);

        knobs.add (new ToneKnob (K ("저음"), true, settings.low, K ("킥과 베이스처럼 낮은 소리를 키우거나 줄입니다.")));
        knobs.add (new ToneKnob (K ("중음"), true, settings.mid, K ("스네어와 목소리가 있는 가운데 소리를 키우거나 줄입니다.")));
        knobs.add (new ToneKnob (K ("고음"), true, settings.high, K ("심벌과 하이햇처럼 높고 밝은 소리를 키우거나 줄입니다.")));
        knobs.add (new ToneKnob (K ("컴프레서"), false, settings.comp, K ("큰 소리와 작은 소리의 차이를 줄여 더 단단하게 들리게 합니다.")));

        for (auto* knob : knobs)
        {
            knob->onChange = [this] { repaint(); };
            addChildComponent (knob);
        }

        resetButton.setButtonText (K ("원래대로"));
        styleButton (resetButton, K ("네 개의 노브를 모두 0으로 되돌립니다."));
        resetButton.onClick = [this]
        {
            settings.low = 0.0f;
            settings.mid = 0.0f;
            settings.high = 0.0f;
            settings.comp = 0.0f;

            for (auto* knob : knobs)
                knob->refresh();

            repaint();
        };
        addChildComponent (resetButton);
    }

    std::function<void()> onLayoutChanged;

    int getIdealHeight() const noexcept   { return rowHeight + (toneOpen ? toneHeight : 0); }
    bool isToneOpen() const noexcept      { return toneOpen; }

    void setToneOpen (bool shouldBeOpen)
    {
        if (! full)
            return;

        toneOpen = shouldBeOpen;
        toneButton.setToggleState (shouldBeOpen, juce::dontSendNotification);
        updateToneVisibility();
    }

    void updateMeter()
    {
        meter.push (settings.peakL.exchange (0.0f), settings.peakR.exchange (0.0f));
    }

    void paint (juce::Graphics& g) override
    {
        if (toneOpen)
        {
            g.setColour (ui::surface);
            g.fillRoundedRectangle (tonePanel.toFloat(), 8.0f);
        }

        g.setFont (ui::font (16.0f));
        g.setColour (ui::text);
        g.drawText (name, nameArea, juce::Justification::centredLeft);

        const bool muted = full && settings.mute.load();
        g.setFont (ui::font (15.0f));
        g.setColour (muted ? ui::textDim : ui::text);
        g.drawText (juce::String (juce::roundToInt (volume.getValue())), valueArea, juce::Justification::centredRight);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // 다듬기 값이 하나라도 들어가 있으면 버튼에 점을 찍어 알려 준다.
        if (full && ! settings.toneIsNeutral())
        {
            const auto b = toneButton.getBounds().toFloat();
            g.setColour (ui::accent);
            g.fillEllipse (b.getRight() - 12.0f, b.getY() + 6.0f, 6.0f, 6.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto top = area.removeFromTop (rowHeight);

        nameArea = top.removeFromLeft (48);

        const auto muteArea = top.removeFromLeft (72);
        top.removeFromLeft (12);
        const auto toneArea = top.removeFromRight (112);
        top.removeFromRight (16);
        meter.setBounds (top.removeFromRight (150));
        top.removeFromRight (12);
        valueArea = top.removeFromRight (34);
        top.removeFromRight (2);
        volume.setBounds (top);

        if (! full)
            return;

        muteButton.setBounds (muteArea.withSizeKeepingCentre (72, 30));
        toneButton.setBounds (toneArea.withSizeKeepingCentre (112, 30));

        tonePanel = area.withTrimmedLeft (48).withTrimmedBottom (6);
        auto panel = tonePanel.reduced (14, 6);
        resetButton.setBounds (panel.removeFromRight (88).withSizeKeepingCentre (88, 30));

        for (auto* knob : knobs)
            knob->setBounds (panel.removeFromLeft (152));
    }

private:
    void updateToneVisibility()
    {
        for (auto* knob : knobs)
            knob->setVisible (toneOpen);

        resetButton.setVisible (toneOpen);
        repaint();
    }

    juce::String name;
    ChannelSettings& settings;
    bool full, toneOpen = false;

    juce::Slider volume;
    LevelMeter meter;
    juce::TextButton muteButton, toneButton, resetButton;
    juce::OwnedArray<ToneKnob> knobs;
    juce::Rectangle<int> nameArea, valueArea, tonePanel;
};

//==============================================================================
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel (juce::AudioDeviceManager& manager)
        : selector (manager, 0, 0, 2, 2, false, false, true, false)
    {
        hint.setText (K ("· 드럼 모듈에 꽂은 헤드폰으로 들으려면 오디오 방식을 ASIO, 장치를 모듈 드라이버(Yamaha Steinberg USB, Roland TD-xx 등)로 두세요.\n"
                         "· 버퍼가 작을수록 빨라집니다. 모듈 드라이버는 '드라이버 설정' 창에서 바꿉니다(Yamaha: ASIO 탭 Buffer Size·Mode, Roland: Audio Buffer Size).\n"
                         "· 소리가 뚝뚝 끊기면 버퍼를 한 칸 크게 하세요."),
                      juce::dontSendNotification);
        hint.setFont (ui::font (14.5f));
        hint.setColour (juce::Label::textColourId, ui::textDim);
        hint.setJustificationType (juce::Justification::topLeft);

        selector.setItemHeight (28);

        addAndMakeVisible (hint);
        addAndMakeVisible (selector);
        setSize (600, 440);
    }

    void paint (juce::Graphics& g) override { g.fillAll (ui::surface); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        hint.setBounds (area.removeFromTop (100));
        selector.setBounds (area);
    }

private:
    juce::Label hint;
    juce::AudioDeviceSelectorComponent selector;
};

//==============================================================================
// '신호 보기': 드럼 모듈에서 받은 MIDI를 사람이 읽을 수 있게 보여 주고, exe 옆 'MIDI 기록.txt'에도 적는다.
// 특정 패드가 엉뚱하게 나거나 가끔 안 날 때, 그 패드가 어떤 음 번호를 보내는지(부분마다 다르다) 확인하는 데 쓴다.
class MidiMonitorPanel final : public juce::Component, private juce::Timer
{
public:
    // weakTomVelocity: 지금 쓰는 '약한 탐 신호 거르기' 값(이 세기 이하를 버림, 0 = 끔). 바꾸면 onWeakTomChanged로 알린다.
    MidiMonitorPanel (Engine& engineToUse, DrumModule::Brand moduleBrand, const juce::String& moduleName, const juce::String& pluginName,
                      int weakTomVelocity, std::function<void (int)> onWeakTomChanged)
        : engine (engineToUse), brand (moduleBrand)
    {
        hint.setText (K ("드럼을 치면 받은 신호가 아래에 나옵니다. 크래시·라이드처럼 부분(몸통·엣지·벨)이 있는 패드는 부분마다 음 번호가 "
                         "따로 옵니다. 엉뚱하게 나는 패드를 몇 번 쳐 보면 어느 번호가 문제인지 알 수 있습니다."),
                      juce::dontSendNotification);
        hint.setFont (ui::font (14.0f));
        hint.setColour (juce::Label::textColourId, ui::textDim);
        hint.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (hint);

        // 약한 탐 신호 거르기: 다른 패드를 칠 때 진동이 옮겨 붙어 탐에 들어오는 아주 약한 신호(크로스토크)를 버린다.
        const auto weakTip = K ("하이햇이나 스네어를 칠 때 진동이 옮겨 붙어 탐에 아주 약한 신호가 들어오면(크로스토크), 드럼 VST에 따라 "
                                "'둥' 하는 탐 소리가 섞여 들립니다. 고른 세기 이하인 탐 신호(헤드·림)는 드럼 VST에 넘기지 않습니다. "
                                "버린 신호는 아래 목록에 '약해서 버림'으로 나옵니다. 탐을 아주 여리게 칠 때 소리가 빠지면 값을 낮추세요.");
        weakLabel.setText (K ("약한 탐 신호 거르기"), juce::dontSendNotification);
        weakLabel.setFont (ui::font (14.0f));
        weakLabel.setColour (juce::Label::textColourId, ui::text);
        weakLabel.setTooltip (weakTip);
        addAndMakeVisible (weakLabel);

        weakBox.addItem (K ("끄기"), 1);
        weakBox.addItem (K ("세기 5 이하 버림"), 2);
        weakBox.addItem (K ("세기 9 이하 버림 (권장)"), 3);
        weakBox.addItem (K ("세기 15 이하 버림"), 4);
        weakBox.setSelectedId (weakTomVelocity <= 0 ? 1 : weakTomVelocity <= 5 ? 2 : weakTomVelocity <= 9 ? 3 : 4, juce::dontSendNotification);
        weakBox.setTooltip (weakTip);
        weakBox.onChange = [this, onWeakTomChanged]
        {
            static constexpr int values[] = { 0, 5, 9, 15 };

            if (const int id = weakBox.getSelectedId(); id >= 1 && id <= 4 && onWeakTomChanged != nullptr)
                onWeakTomChanged (values[id - 1]);
        };
        addAndMakeVisible (weakBox);

        view.setMultiLine (true);
        view.setReadOnly (true);
        view.setCaretVisible (false);
        view.setScrollbarsShown (true);
        view.setFont (ui::font (14.5f));
        view.setColour (juce::TextEditor::backgroundColourId, ui::bg);
        view.setColour (juce::TextEditor::textColourId, ui::text);
        view.setColour (juce::TextEditor::outlineColourId, ui::line);
        addAndMakeVisible (view);

        summary.setFont (ui::font (14.0f));
        summary.setColour (juce::Label::textColourId, ui::text);
        summary.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (summary);

        clearButton.setButtonText (K ("지우기"));
        clearButton.onClick = [this] { view.clear(); counts.clear(); summary.setText ({}, juce::dontSendNotification); };
        addAndMakeVisible (clearButton);

        fileButton.setButtonText (K ("기록 파일 열기"));
        fileButton.onClick = [this] { logFile.revealToUser(); };
        addAndMakeVisible (fileButton);

        logFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile (lang::pick ("MIDI 기록.txt", "MIDI log.txt"));
        logFile.replaceWithText (K ("DrumVstHost MIDI 기록 ") + juce::Time::getCurrentTime().toString (true, true) + "\r\n"
                                 + K ("모듈: ") + moduleName + K (", 플러그인: ") + pluginName + "\r\n");

        MidiInbox::MonitorEvent old[256];

        while (engine.readMidiMonitor (old, 256) > 0) {}   // 창을 열기 전에 쌓인 것은 버린다

        started = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (20);
        setSize (680, 560);
    }

    void paint (juce::Graphics& g) override { g.fillAll (ui::surface); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16, 12);
        hint.setBounds (area.removeFromTop (44));
        auto weakRow = area.removeFromTop (30);
        weakLabel.setBounds (weakRow.removeFromLeft (170));
        weakBox.setBounds (weakRow.removeFromLeft (230));
        area.removeFromTop (8);
        auto buttons = area.removeFromBottom (34);
        fileButton.setBounds (buttons.removeFromRight (130));
        buttons.removeFromRight (8);
        clearButton.setBounds (buttons.removeFromRight (90));
        area.removeFromBottom (8);
        summary.setBounds (area.removeFromBottom (44));
        area.removeFromBottom (6);
        view.setBounds (area);
    }

private:
    juce::String describe (const MidiInbox::MonitorEvent& e)
    {
        const int status = e.data[0] & 0xf0;
        const int channel = (e.data[0] & 0x0f) + 1;
        const int a = e.size > 1 ? e.data[1] : 0, b = e.size > 2 ? e.data[2] : 0;
        juce::String text;

        if (e.data[0] == 0xf8 || e.data[0] == 0xfa || e.data[0] == 0xfb || e.data[0] == 0xfc)
            return {};   // 클록·시작/정지: 너무 잦아 보여 주지 않는다(플러그인에도 넘기지 않는다)

        if (status == 0x90 && b > 0)
        {
            ++counts[a];
            // 모듈 회사의 기본 음 배치로 패드 이름을 붙인다(예: Yamaha 52 = Ride 엣지. General MIDI 이름 'China'가 아니다).
            const auto pad = drumPadName (brand, a);
            text << K ("음 ") << juce::String (a).paddedLeft (' ', 3) << K ("  세기 ") << juce::String (b).paddedLeft (' ', 3)
                 << (pad.isNotEmpty() ? "   (" + pad + ")" : juce::String())
                 << (e.mappedNote < 128 ? K ("   패드 배치: ") + juce::String ((int) e.mappedNote) + K ("번으로 넘김") : juce::String());
        }
        else if (status == 0x80 || status == 0x90)
        {
            text << K ("   음 끔 ") << a;
        }
        else if (status == 0xa0)
        {
            text << K ("초크(애프터터치) 음 ") << a << " = " << b;
        }
        else if (status == 0xb0)
        {
            // 하이햇 페달은 움직이는 동안 아주 많이 오므로 0.1초에 한 번만 보여 준다.
            if (a == 4 && e.time - lastPedalShown < 100.0)
                return {};

            if (a == 4)
                lastPedalShown = e.time;

            text << (a == 4 ? K ("하이햇 페달(CC4) = ") + juce::String (b) : "CC " + juce::String (a) + " = " + juce::String (b));
        }
        else if (status == 0xc0)
        {
            text << K ("킷 바꿈(Program Change) ") << a;
        }
        else
        {
            text << juce::String::toHexString (e.data, e.size, 1);
        }

        const double seconds = (e.time - started) / 1000.0;
        return juce::String (seconds, 2).paddedLeft (' ', 8) + "s  ch" + juce::String (channel).paddedLeft (' ', 2) + "  " + text
               + (e.weak ? K ("   (약해서 버림: 크로스토크)") : e.forwarded ? juce::String() : K ("   (걸러짐: 플러그인에 안 넘김)"));
    }

    void timerCallback() override
    {
        MidiInbox::MonitorEvent events[256];
        juce::String added;

        for (int n = engine.readMidiMonitor (events, 256); n > 0; n = engine.readMidiMonitor (events, 256))
            for (int i = 0; i < n; ++i)
                if (const auto line = describe (events[i]); line.isNotEmpty())
                    added << line << "\n";

        if (added.isEmpty())
            return;

        logFile.appendText (added.replace ("\n", "\r\n"));

        // 너무 길어지면 앞부분을 버린다
        if (view.getTotalNumChars() > 40000)
            view.setText (view.getText().getLastCharacters (20000).fromFirstOccurrenceOf ("\n", false, false), false);

        view.moveCaretToEnd();
        view.insertTextAtCaret (added);

        juce::String text = K ("받은 음 번호(횟수): ");

        for (const auto& [note, count] : counts)
            text << note << "x" << count << "  ";

        summary.setText (text, juce::dontSendNotification);
    }

    Engine& engine;
    DrumModule::Brand brand = DrumModule::Brand::none;
    juce::Label hint, summary, weakLabel;
    juce::ComboBox weakBox;
    juce::TextEditor view;
    juce::TextButton clearButton, fileButton;
    juce::File logFile;
    std::map<int, int> counts;
    double started = 0.0, lastPedalShown = 0.0;
};

//==============================================================================
MainComponent::MainComponent (Engine& engineToUse, juce::PropertiesFile& settingsToUse)
    : engine (engineToUse), settings (settingsToUse)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);

    // ① 드럼 연결
    monitorButton.setButtonText (K ("신호 보기"));
    styleButton (monitorButton, K ("드럼 모듈에서 받은 신호(패드마다 음 번호·세기, 하이햇 페달, 초크)를 보여 줍니다. "
                                   "특정 패드가 엉뚱하게 나거나 안 날 때 어떤 번호가 오는지 확인합니다."));
    monitorButton.onClick = [this] { showMidiMonitor(); };
    addAndMakeVisible (monitorButton);

    // 화면 언어 바꾸기: 다른 언어의 이름을 그 언어로 보여 준다(다시 켜면 적용).
    languageButton.setButtonText (lang::isEnglish() ? juce::String::fromUTF8 ("한국어") : juce::String ("English"));
    styleButton (languageButton, K ("화면 언어를 영어로 바꿉니다. 앱을 다시 켜면 적용됩니다."));
    languageButton.onClick = [this]
    {
        settings.setValue ("language", lang::isEnglish() ? "ko" : "en");
        settings.saveIfNeeded();
        languageButton.setButtonText (K ("다시 켜면 적용"));
        languageButton.setEnabled (false);
    };
    addAndMakeVisible (languageButton);

    padButton.setButtonText (K ("패드 맞추기"));
    padButton.setColour (juce::TextButton::buttonOnColourId, ui::accent);
    padButton.setColour (juce::TextButton::textColourOnId, ui::onAccent);
    styleButton (padButton, K ("어떤 전자드럼이든 쓸 수 있게, 패드를 하나씩 쳐서 드럼 VST의 소리(Kick·Snare·Hi-hat…)에 맞춥니다. "
                               "패드가 엉뚱한 소리를 내거나 소리가 안 날 때 씁니다. 색이 켜져 있으면 배치를 쓰는 중입니다."));
    padButton.onClick = [this] { showPadMap(); };
    addAndMakeVisible (padButton);

    settingsButton.setButtonText (K ("소리 장치 설정"));
    styleButton (settingsButton, K ("소리가 나올 장치를 고르고, 소리가 늦거나 끊길 때 조절합니다."));
    settingsButton.onClick = [this] { showSettings(); };
    addAndMakeVisible (settingsButton);

    styleLabel (latencyLabel, 16.0f, ui::text);
    latencyLabel.setInterceptsMouseClicks (true, false);
    latencyLabel.setTooltip (describeLatency());   // 장치가 열리면 내역으로 바뀐다(slowRefresh)
    addAndMakeVisible (latencyLabel);

    bufferBox.setMouseClickGrabsKeyboardFocus (false);
    bufferBox.setWantsKeyboardFocus (false);
    bufferBox.setTooltip (K ("버퍼 크기입니다. 작을수록 빠르고, 너무 작으면 소리가 끊깁니다."));
    bufferBox.onChange = [this] { bufferChosen(); };
    addChildComponent (bufferBox);

    driverButton.setButtonText (K ("버퍼 바꾸기"));
    styleButton (driverButton, bufferAdviceLong (engine.getAudioDeviceType(), engine.getAudioDeviceName()));   // 장치가 바뀌면 다시 쓴다
    driverButton.onClick = [this] { openDriverPanel(); };
    addChildComponent (driverButton);

    fastButton.setButtonText (K ("더 빠르게"));
    fastButton.setColour (juce::TextButton::buttonOnColourId, ui::accent);
    fastButton.setColour (juce::TextButton::textColourOnId, ui::onAccent);
    styleButton (fastButton, K ("실험 기능: Yamaha 드라이버가 USB로 보내기 전에 버퍼 말고 더 두는 여유(Low Latency에서 약 6ms)를 1ms 단위로 "
                                "줄입니다(드라이버 창에 없는 숨은 설정). 여유가 모자라면 소리가 자글거리는데, 이것은 드라이버 안에서 생기는 일이라 앱이 "
                                "알아채지 못합니다. 탐을 쳐 보고 자글거리면 한 단계 낮추세요(DTX-PRO에서는 3ms까지 괜찮았고 4ms는 자글거렸습니다). 드라이버 창에서 Mode를 "
                                "고르고 OK를 누르면 그 Mode로 바뀝니다. 같은 드라이버를 쓰는 다른 프로그램(DAW 등)에도 적용됩니다."));
    fastButton.onClick = [this] { showFasterMenu(); };
    addChildComponent (fastButton);

    // ② 드럼 소리
    pluginBox.setTextWhenNothingSelected (K ("드럼 소리 플러그인을 고르세요"));
    pluginBox.setTextWhenNoChoicesAvailable (K ("설치된 VST3 플러그인이 없습니다"));
    pluginBox.setMouseClickGrabsKeyboardFocus (false);
    pluginBox.setWantsKeyboardFocus (false);
    pluginBox.setTooltip (K ("PC에 설치된 VST3 악기(SSD 5.5, BFD 3.5 등) 중에서 드럼 소리를 낼 것을 고릅니다. "
                             "플러그인을 바꿔도 각 플러그인의 킷과 설정은 따로 기억됩니다."));
    pluginBox.onChange = [this] { pluginChosen(); };
    addAndMakeVisible (pluginBox);

    editorButton.setButtonText (K ("드럼 킷 고르기"));
    styleButton (editorButton, K ("플러그인 창을 열어 드럼 킷과 MIDI 맵을 고르고 소리를 바꿉니다."));
    editorButton.onClick = [this] { showPluginWindow(); };
    addAndMakeVisible (editorButton);

    const auto setupTestButton = [this] (juce::TextButton& button, const char* text, int note)
    {
        button.setButtonText (K (text));
        styleButton (button, K ("드럼 없이도 소리가 나는지 확인합니다."));
        button.onClick = [this, note] { engine.playTestNote (note); };
        addAndMakeVisible (button);
    };

    setupTestButton (kickButton, "킥", 36);
    setupTestButton (snareButton, "스네어", 38);
    setupTestButton (hatButton, "하이햇", 42);

    styleLabel (drumStatus, 13.5f, ui::textDim);
    addAndMakeVisible (drumStatus);

    // ③ 원곡과 녹음
    openSongButton.setButtonText (K ("파일 열기"));
    styleButton (openSongButton, K ("같이 연주할 음악 파일을 고릅니다."));
    openSongButton.onClick = [this] { browseForSong(); };
    addAndMakeVisible (openSongButton);

    styleLabel (songName, 15.5f, ui::textDim);
    songName.setText (K ("아직 고른 곡이 없습니다"), juce::dontSendNotification);
    addAndMakeVisible (songName);

    waveform = std::make_unique<WaveformView> (engine.getThumbnail());
    waveform->onSeek = [this] (double seconds) { engine.setPosition (seconds); };
    addAndMakeVisible (*waveform);

    playButton = std::make_unique<IconButton>();
    playButton->setContent (IconButton::Icon::play, K ("재생"));
    playButton->setTooltip (K ("원곡을 재생하거나 잠시 멈춥니다. (스페이스바)"));
    playButton->onClick = [this] { togglePlay(); };
    addAndMakeVisible (*playButton);

    rewindButton = std::make_unique<IconButton>();
    rewindButton->setContent (IconButton::Icon::rewind, K ("처음으로"));
    rewindButton->setTooltip (K ("곡의 맨 앞으로 돌아갑니다."));
    rewindButton->onClick = [this] { engine.rewind(); };
    addAndMakeVisible (*rewindButton);

    styleLabel (timeLabel, 15.0f, ui::text);
    timeLabel.setText ("0:00 / 0:00", juce::dontSendNotification);
    addAndMakeVisible (timeLabel);

    styleLabel (songStatus, 13.5f, ui::textDim);
    songStatus.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (songStatus);

    recordingsButton.setButtonText (K ("지난 녹음"));
    styleButton (recordingsButton, K ("녹음한 연주를 다시 열어 볼륨과 타이밍을 다듬거나, 녹음 폴더를 엽니다."));
    recordingsButton.onClick = [this] { showRecordingsMenu(); };
    addAndMakeVisible (recordingsButton);

    recordButton = std::make_unique<IconButton>();
    recordButton->setContent (IconButton::Icon::record, K ("녹음"));
    recordButton->setTooltip (K ("지금 들리는 소리를 녹음합니다. 곡이 있으면 재생도 함께 시작하고, 끝나면 다듬기 창이 열립니다."));
    recordButton->onClick = [this] { toggleRecording(); };
    addAndMakeVisible (*recordButton);

    // ③ 연습 도구
    const auto setupToggle = [this] (juce::TextButton& button, const char* text, const char* tooltip)
    {
        button.setButtonText (K (text));
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonOnColourId, ui::accent);
        button.setColour (juce::TextButton::textColourOnId, ui::onAccent);
        styleButton (button, K (tooltip));
        addAndMakeVisible (button);
    };

    setupToggle (metronomeButton, "메트로놈",
                 "원곡을 분석해 찾은 박자에 맞춰 클릭 소리를 냅니다. 곡마다 처음 한 번만 분석합니다(수십 초). 클릭은 녹음에 들어가지 않습니다.");
    metronomeButton.onClick = [this] { metronomeClicked(); };

    setupToggle (songDrumsButton, "원곡 드럼 빼기",
                 "원곡에서 드럼 소리만 빼고 들려줍니다. 곡마다 처음 한 번 분리합니다(1~3분). 다시 누르면 원곡 드럼이 돌아옵니다.");
    songDrumsButton.onClick = [this] { songDrumsClicked(); };

    metronomeVolume.setSliderStyle (juce::Slider::LinearHorizontal);
    metronomeVolume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    // 0~150(100이 예전 최대). 크게 쳐도 클릭이 잘 들리도록 최대를 키웠다.
    const double maxVolume = Engine::maxMetronomeVolume * 100.0;
    metronomeVolume.setRange (0.0, maxVolume, 1.0);
    metronomeVolume.setDoubleClickReturnValue (true, 70.0);
    metronomeVolume.setMouseClickGrabsKeyboardFocus (false);
    metronomeVolume.setWantsKeyboardFocus (false);
    metronomeVolume.setTooltip (K ("메트로놈 클릭 소리 크기입니다. 오른쪽 끝은 예전 최대보다 약 7dB 큽니다. 더블클릭하면 처음 크기로 돌아갑니다."));
    metronomeVolume.setValue (juce::jlimit (0.0, maxVolume, settings.getDoubleValue ("metronomeVolume", 70.0)), juce::dontSendNotification);
    engine.metronomeVolume = (float) (metronomeVolume.getValue() / 100.0);
    metronomeVolume.onValueChange = [this] { engine.metronomeVolume = (float) (metronomeVolume.getValue() / 100.0); };
    addAndMakeVisible (metronomeVolume);

    for (auto* label : { &metronomeStatus, &songDrumsStatus })
    {
        styleLabel (*label, 13.5f, ui::textDim);
        label->setInterceptsMouseClicks (true, false);   // 자세한 오류를 말풍선으로
        addAndMakeVisible (*label);
    }

    metronomeStatus.addMouseListener (this, false);
    songDrumsStatus.addMouseListener (this, false);   // 분석 도구를 설치하는 동안 눌러서 그만둘 수 있게   // 박자를 찾은 뒤에는 눌러서 예비박·강박 위치를 고친다

    // 지난번 연습 도구 상태(곡을 불러오면 그 곡에 맞춰 다시 적용된다)
    engine.countInEnabled = settings.getBoolValue ("countIn", true);
    metronomeButton.setToggleState (settings.getBoolValue ("metronomeOn", false), juce::dontSendNotification);
    songDrumsButton.setToggleState (settings.getBoolValue ("songDrumsOff", false), juce::dontSendNotification);
    engine.metronomeOn = metronomeButton.getToggleState();

    // ④ 볼륨 맞추기
    drumRow = std::make_unique<ChannelRow> (K ("드럼"), engine.drum, true, 100.0);
    songRow = std::make_unique<ChannelRow> (K ("원곡"), engine.song, true, 70.0);
    masterRow = std::make_unique<ChannelRow> (K ("전체"), engine.master, false, 100.0);

    for (auto* row : { drumRow.get(), songRow.get(), masterRow.get() })
    {
        row->onLayoutChanged = [this] { setSize (windowWidth, getIdealHeight()); resized(); };
        addAndMakeVisible (*row);
    }

    drumRow->setToneOpen (settings.getBoolValue ("drumToneOpen", false));
    songRow->setToneOpen (settings.getBoolValue ("songToneOpen", false));

    const auto defaultFolder = dataChild (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(),
                                          "녹음", "Recordings");
    recordFolder = juce::File (settings.getValue ("recordFolder", defaultFolder.getFullPathName()));

    // 약한 탐 신호 거르기(모듈을 알아보면 그 회사의 탐 음 번호로 다시 적용된다)
    weakTomVelocity = juce::jlimit (0, 30, settings.getIntValue ("weakTomVelocity", defaultWeakTomVelocity));
    applyWeakTomFilter();

    xrunGraceUntil = juce::Time::getMillisecondCounter() + 5000;

    setDrumStatus ({}, false);
    updateEnabledStates();
    setSize (windowWidth, getIdealHeight());

    slowRefresh();
    refreshBufferControls (true);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    midiJob.reset();   // 고친 MIDI로 다시 만드는 중이면 멈추고 기다린다(엔진을 쓰므로 먼저)
    closeTakeEditor();

    if (engine.isRecording())
        engine.stopRecording();

    pluginWindow.reset();
    settingsWindow.reset();
}

//==============================================================================
void MainComponent::restoreSession()
{
    populatePluginBox();

    auto path = engine.getSavedPluginPath();
    bool openWindow = false;

    // 처음 실행했거나 쓰던 플러그인이 지워졌으면, 설치된 드럼 VST 가운데 가장 알맞은 것(Toontrack·XLN 먼저)을
    // 저절로 불러온다. 처음 쓰는 플러그인이면 킷과 MIDI 맵을 고를 수 있게 플러그인 창도 함께 연다.
    if (path.isEmpty() || ! juce::File (path).exists())
    {
        const auto missing = path;
        path.clear();

        for (const auto& entry : pluginEntries)
        {
            if (entry.isDrum)
            {
                path = entry.path;
                openWindow = ! engine.hasRememberedState (path);
                autoLoadNote = (missing.isNotEmpty() ? juce::File (missing).getFileNameWithoutExtension() + K ("이(가) 없어서 ") : juce::String())
                             + K ("설치된 드럼 VST '") + entry.name + K ("'를 저절로 불러왔습니다.");
                break;
            }
        }

        if (path.isEmpty() && missing.isEmpty() && pluginEntries.size() == 1)   // 드럼으로 알아본 것은 없지만 플러그인이 하나뿐
        {
            path = pluginEntries.front().path;
            openWindow = ! engine.hasRememberedState (path);
        }
    }

    // 지난번에 이 플러그인을 불러오다가 앱이 꺼졌으면(플러그인 업데이트·망가진 킷 상태 등) 이번에는 저절로 불러오지 않는다.
    // 그러지 않으면 켤 때마다 같은 자리에서 꺼져 앱을 쓸 수 없게 된다. 두 번 연달아 그랬으면 저장해 둔 킷 상태를 버린다.
    if (const auto crashed = settings.getValue ("loadingPlugin"); crashed.isNotEmpty() && crashed == path)
    {
        const int times = settings.getIntValue ("loadingPluginCrashes", 0) + 1;
        const auto name = juce::File (path).getFileNameWithoutExtension();

        if (times >= 2)
            engine.forgetPluginState (path);

        if (autosave)
        {
            settings.setValue ("loadingPluginCrashes", times);
            settings.removeValue ("loadingPlugin");
            settings.saveIfNeeded();
        }

        setDrumStatus (K ("지난번에 '") + name + K ("'을(를) 불러오다가 앱이 꺼져서 이번에는 불러오지 않았습니다. 목록에서 다시 고르면 ")
                           + (times >= 2 ? K ("킷 상태를 처음으로 돌려 불러옵니다.") : K ("한 번 더 불러옵니다.")), true);
        drumStatusHoldUntil = juce::Time::getMillisecondCounter() + 60000;   // 모듈 안내로 바로 덮이지 않게
        path.clear();
    }

    if (path.isNotEmpty() && juce::File (path).exists())
        startLoadingPlugin (path, openWindow);

    const juce::File songFile (engine.getSavedSongPath());

    if (engine.getSavedSongPath().isNotEmpty() && songFile.existsAsFile())
        loadSong (songFile);
}

void MainComponent::saveSession()
{
    settings.setValue ("recordFolder", recordFolder.getFullPathName());
    settings.setValue ("drumToneOpen", drumRow->isToneOpen());
    settings.setValue ("songToneOpen", songRow->isToneOpen());
    settings.setValue ("metronomeVolume", metronomeVolume.getValue());
    settings.setValue ("metronomeOn", metronomeButton.getToggleState());
    settings.setValue ("songDrumsOff", songDrumsButton.getToggleState());
    settings.setValue ("countIn", engine.countInEnabled.load());
}

void MainComponent::saveAllNow()
{
    // 앱을 닫을 때만 저장하면 비정상 종료 때 킷·버퍼 설정을 잃는다. 중요한 순간마다 바로 쓴다.
    // 플러그인을 불러오는 중에는(그사이 메시지가 돌아 여기로 올 수 있다) 끝난 뒤에 저장한다.
    if (! autosave || loadingPlugin || engine.isLoadingPlugin())
        return;

    saveSession();
    engine.saveState (settings);
    settings.saveIfNeeded();
}

void MainComponent::setToneOpen (bool drumOpen, bool songOpen)
{
    drumRow->setToneOpen (drumOpen);
    songRow->setToneOpen (songOpen);
    setSize (windowWidth, getIdealHeight());
    resized();
}

void MainComponent::setRecordFolder (const juce::File& folder)
{
    recordFolder = folder;
}

void MainComponent::startRecordingForTest()
{
    if (! engine.isRecording())
        toggleRecording();
}

void MainComponent::stopRecordingForTest()
{
    if (engine.isRecording())
        finishRecording();
}

int MainComponent::getIdealHeight() const
{
    return section1Height + section2Height + section3Height
         + sectionTop + titleHeight + 6
         + drumRow->getIdealHeight() + songRow->getIdealHeight() + masterRow->getIdealHeight()
         + 14;
}

void MainComponent::updateEnabledStates()
{
    const bool hasPlugin = engine.getPlugin() != nullptr && ! loadingPlugin;
    editorButton.setEnabled (hasPlugin);
    kickButton.setEnabled (hasPlugin);
    snareButton.setEnabled (hasPlugin);
    hatButton.setEnabled (hasPlugin);
    pluginBox.setEnabled (! loadingPlugin);

    const bool hasSong = engine.hasSong();
    playButton->setEnabled (hasSong);
    rewindButton->setEnabled (hasSong);
    metronomeButton.setEnabled (hasSong);
    songDrumsButton.setEnabled (hasSong);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (ui::bg);

    const bool drumConnected = ! midiNames.isEmpty();
    const bool done[] = { drumConnected && deviceOpen, engine.getPlugin() != nullptr, engine.hasSong(), false };
    const char* const titles[] = { "드럼 연결", "드럼 소리", "원곡", "볼륨 맞추기" };

    for (int i = 0; i < 4; ++i)
    {
        const auto area = sections[i];

        if (i > 0)
        {
            g.setColour (ui::line);
            g.fillRect (0, area.getY(), getWidth(), 1);
        }

        const juce::Rectangle<float> badge ((float) sidePad, (float) (area.getY() + sectionTop), 24.0f, 24.0f);

        if (done[i])
        {
            g.setColour (ui::accent);
            g.fillEllipse (badge);
            g.setColour (ui::onAccent);
        }
        else
        {
            g.setColour (ui::outline);
            g.drawEllipse (badge.reduced (0.75f), 1.5f);
            g.setColour (ui::textDim);
        }

        g.setFont (ui::font (14.5f, true));
        g.drawText (juce::String (i + 1), badge, juce::Justification::centred);

        g.setColour (ui::text);
        g.setFont (ui::font (17.0f, true));
        g.drawText (K (titles[i]), contentLeft, area.getY() + sectionTop, 300, titleHeight, juce::Justification::centredLeft);
    }

    // ① 드럼 연결: 불빛 + 상태 문장
    {
        const float cy = (float) midiRow.getCentreY();
        const juce::Rectangle<float> led ((float) midiRow.getX(), cy - 5.0f, 10.0f, 10.0f);

        if (drumConnected)
        {
            if (ledLit)
            {
                g.setColour (ui::accent.withAlpha (0.3f));
                g.fillEllipse (led.expanded (4.0f));
            }

            g.setColour (ledLit ? ui::accent : ui::green);
            g.fillEllipse (led);
        }
        else
        {
            g.setColour (ui::textDim);
            g.drawEllipse (led.reduced (0.75f), 1.5f);
        }

        juce::String primary, secondary;

        if (drumConnected)
        {
            if (connectedModule.isKnown())
            {
                primary = connectedModule.displayName();   // "Roland TD-17", "Yamaha (DTX Drums-1)"
            }
            else
            {
                primary = midiNames[0];

                if (midiNames.size() == 2)
                    primary << ", " << midiNames[1];
                else if (midiNames.size() > 2)
                    primary << K (" 외 ") << (midiNames.size() - 1) << K ("개");
            }

            primary << K (" 연결됨");
            secondary = moduleAdvice.isNotEmpty() ? moduleAdvice : K ("패드를 치면 왼쪽 불이 깜빡입니다");
        }
        else
        {
            primary = K ("연결된 드럼이 없습니다");
            secondary = K ("드럼 모듈의 전원과 USB 케이블을 확인하세요");
        }

        const auto font = ui::font (15.5f);
        int x = midiRow.getX() + 20;
        g.setFont (font);
        g.setColour (ui::text);
        g.drawText (primary, x, midiRow.getY(), midiRow.getRight() - x, midiRow.getHeight(), juce::Justification::centredLeft);

        x += juce::GlyphArrangement::getStringWidthInt (font, primary) + 14;
        g.setFont (ui::font (14.0f));
        g.setColour (drumConnected && moduleAdvice.isNotEmpty() ? ui::accent : ui::textDim);
        g.drawText (secondary, x, midiRow.getY(), juce::jmax (0, midiRow.getRight() - x), midiRow.getHeight(), juce::Justification::centredLeft);
    }

    // ① 소리 나오는 곳
    {
        const int labelX = deviceRow.getX() + 20;
        const int x = labelX + labelColumn;

        g.setFont (ui::font (14.0f));
        g.setColour (ui::textDim);
        g.drawText (K ("소리 나오는 곳"), labelX, deviceRow.getY(), labelColumn, deviceRow.getHeight(), juce::Justification::centredLeft);

        g.setFont (ui::font (15.5f));
        g.setColour (deviceOpen ? ui::text : ui::red);
        g.drawText (deviceOpen ? deviceName : K ("소리 장치를 열지 못했습니다. 오른쪽 버튼으로 장치를 골라 주세요."),
                    x, deviceRow.getY(), deviceRow.getRight() - x, deviceRow.getHeight(), juce::Justification::centredLeft);
    }

    // ① 지연: 이름 · (값은 라벨) · 버퍼 · 안내
    {
        const int labelX = latencyRow.getX() + 20;

        g.setFont (ui::font (14.0f));
        g.setColour (ui::textDim);
        g.drawText (K ("지연"), labelX, latencyRow.getY(), labelColumn, latencyRow.getHeight(), juce::Justification::centredLeft);

        if (! bufferTextArea.isEmpty())
        {
            g.setFont (ui::font (14.5f));
            g.setColour (ui::text);
            g.drawText (bufferText, bufferTextArea, juce::Justification::centredLeft);
        }

        int hintX = bufferTextArea.getRight() + 12;

        if (bufferBox.isVisible())
            hintX = bufferBox.getRight() + 14;
        else if (fastButton.isVisible())
            hintX = fastButton.getRight() + 14;
        else if (driverButton.isVisible())
            hintX = driverButton.getRight() + 14;

        juce::Colour colour;
        const auto hint = getLatencyHint (colour);
        g.setFont (ui::font (13.5f));
        g.setColour (colour);
        g.drawFittedText (hint, hintX, latencyRow.getY(), juce::jmax (0, latencyRow.getRight() - hintX), latencyRow.getHeight(),
                          juce::Justification::centredLeft, 1, 0.85f);
    }

    // ② 소리 확인
    g.setFont (ui::font (14.0f));
    g.setColour (ui::textDim);
    g.drawText (K ("소리 확인"), testLabelArea, juce::Justification::centredRight);
}

void MainComponent::resized()
{
    const int right = getWidth() - sidePad;
    const int contentWidth = right - contentLeft;
    int y = 0;

    // ① 드럼 연결
    sections[0] = { 0, y, getWidth(), section1Height };
    {
        languageButton.setBounds (right - 112, y + sectionTop - 2, 112, 26);

        int cy = y + sectionTop + titleHeight + 6;
        midiRow = { contentLeft, cy, contentWidth, 26 };
        monitorButton.setBounds (midiRow.removeFromRight (96));
        midiRow.removeFromRight (8);
        padButton.setBounds (midiRow.removeFromRight (110));
        midiRow.removeFromRight (8);
        cy += 26 + 4;

        juce::Rectangle<int> row (contentLeft, cy, contentWidth, 32);
        settingsButton.setBounds (row.removeFromRight (132));
        deviceRow = row.withTrimmedRight (12);
        cy += 32 + 4;

        latencyRow = { contentLeft, cy, contentWidth, 32 };
        int x = latencyRow.getX() + 20 + labelColumn;
        latencyLabel.setBounds (x, cy, 76, 32);
        x += 80;

        if (bufferBox.isVisible())
        {
            bufferBox.setBounds (x, cy + 1, 200, 30);
            bufferTextArea = {};
        }
        else
        {
            const int textWidth = bufferText.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt (ui::font (14.5f), bufferText) + 4;
            bufferTextArea = { x, cy, textWidth, 32 };
            x += textWidth + (textWidth > 0 ? 10 : 0);
            driverButton.setBounds (x, cy + 1, 104, 30);
            fastButton.setBounds (driverButton.getRight() + 8, cy + 1, 92, 30);
        }
    }
    y += section1Height;

    // ② 드럼 소리
    sections[1] = { 0, y, getWidth(), section2Height };
    {
        int cy = y + sectionTop + titleHeight + 8;
        juce::Rectangle<int> row (contentLeft, cy, contentWidth, 36);

        hatButton.setBounds (row.removeFromRight (74));
        row.removeFromRight (6);
        snareButton.setBounds (row.removeFromRight (74));
        row.removeFromRight (6);
        kickButton.setBounds (row.removeFromRight (60));
        row.removeFromRight (10);
        testLabelArea = row.removeFromRight (70);

        pluginBox.setBounds (row.removeFromLeft (300));
        row.removeFromLeft (8);
        editorButton.setBounds (row.removeFromLeft (132));

        cy += 36 + 4;
        drumStatus.setBounds (contentLeft, cy, contentWidth, 18);
    }
    y += section2Height;

    // ③ 원곡
    sections[2] = { 0, y, getWidth(), section3Height };
    {
        int cy = y + sectionTop + titleHeight + 8;
        juce::Rectangle<int> fileRow (contentLeft, cy, contentWidth, 34);
        openSongButton.setBounds (fileRow.removeFromLeft (104));
        fileRow.removeFromLeft (12);
        songName.setBounds (fileRow);
        cy += 34 + 8;

        waveform->setBounds (contentLeft, cy, contentWidth, 64);
        cy += 64 + 8;

        {
            juce::Rectangle<int> tools (contentLeft, cy, contentWidth, 34);
            metronomeButton.setBounds (tools.removeFromLeft (112));
            tools.removeFromLeft (10);
            metronomeVolume.setBounds (tools.removeFromLeft (96));
            tools.removeFromLeft (10);

            songDrumsStatus.setBounds (tools.removeFromRight (180));
            tools.removeFromRight (10);
            songDrumsButton.setBounds (tools.removeFromRight (136));
            tools.removeFromRight (10);
            metronomeStatus.setBounds (tools);   // 빠르기·클릭 시점까지 보이도록 남는 자리를 다 쓴다
        }

        cy += 34 + 8;

        juce::Rectangle<int> row (contentLeft, cy, contentWidth, 38);
        playButton->setBounds (row.removeFromLeft (112));
        row.removeFromLeft (8);
        rewindButton->setBounds (row.removeFromLeft (112));
        row.removeFromLeft (14);
        timeLabel.setBounds (row.removeFromLeft (110));

        recordButton->setBounds (row.removeFromRight (150));
        row.removeFromRight (8);
        recordingsButton.setBounds (row.removeFromRight (96));
        row.removeFromRight (12);
        songStatus.setBounds (row);
    }
    y += section3Height;

    // ④ 볼륨 맞추기
    sections[3] = { 0, y, getWidth(), getHeight() - y };
    {
        int cy = y + sectionTop + titleHeight + 6;

        for (auto* row : { drumRow.get(), songRow.get(), masterRow.get() })
        {
            row->setBounds (contentLeft, cy, contentWidth, row->getIdealHeight());
            cy += row->getIdealHeight();
        }
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        togglePlay();
        return true;
    }

    return false;
}

//==============================================================================
bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1;
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.isEmpty())
        return;

    const juce::File file (files[0]);

    if (file.hasFileExtension ("vst3"))
    {
        if (! loadingPlugin)
        {
            engine.addExtraPluginPath (file.getFullPathName());
            startLoadingPlugin (file.getFullPathName(), ! engine.hasRememberedState (file.getFullPathName()));
        }
    }
    else if (file.isDirectory() && isTakeFolder (file))
    {
        openTakeEditor (file);
    }
    else
    {
        loadSong (file);
    }
}

//==============================================================================
void MainComponent::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();

    drumRow->updateMeter();
    songRow->updateMeter();
    masterRow->updateMeter();

    // 재생 상태
    const bool playing = engine.isPlaying();

    if (playing != shownPlaying)
    {
        shownPlaying = playing;
        playButton->setContent (playing ? IconButton::Icon::pause : IconButton::Icon::play,
                                playing ? K ("일시정지") : K ("재생"));
    }

    const double length = engine.hasSong() ? engine.getLength() : 0.0;
    const double position = engine.hasSong() ? engine.getPosition() : 0.0;
    waveform->setPosition (position, length, engine.hasSong());

    const auto timeText = clockText (position) + " / " + clockText (length);

    if (timeText != shownTime)
    {
        shownTime = timeText;
        timeLabel.setText (timeText, juce::dontSendNotification);
    }

    // 녹음. 장치가 바뀌어(샘플 속도가 달라져) 엔진이 녹음을 스스로 멈췄으면 버튼을 눌러 끝낸 것처럼 마무리한다.
    if (wasRecording && ! engine.isRecording())
        finishRecording();

    wasRecording = engine.isRecording();

    if (engine.isRecording())
    {
        recordButton->setContent (IconButton::Icon::stop, K ("녹음 끝내기 ") + clockText (engine.getRecordedSeconds()), true);

        // 곡이 끝나면 드럼 여운이 담길 만큼만 더 기다렸다가 저절로 끝낸다.
        if (recordStartedPlayback && recordStopAt == 0 && engine.hasSongFinished())
            recordStopAt = now + 1500;

        if (recordStopAt != 0 && now >= recordStopAt)
            finishRecording();
    }

    if (songStatusUntil != 0 && now >= songStatusUntil)
        setSongStatus ({}, false);

    // 드럼을 치면 불빛이 깜빡인다
    const auto noteCount = engine.getMidiNoteCount();

    if (noteCount != lastNoteCount)
    {
        lastNoteCount = noteCount;
        ledUntil = now + 130;
    }

    const bool lit = now < ledUntil;

    if (lit != ledLit)
    {
        ledLit = lit;
        repaint (midiRow.withWidth (40).expanded (8));
    }

    if (++tick % 30 == 0)
        slowRefresh();

    if (tick % (30 * 120) == 0)   // 2분마다
        saveAllNow();
}

void MainComponent::slowRefresh()
{
    engine.refreshMidiInputs();
    checkYamahaDriverSettings();

    // 장치·버퍼가 바뀌었으면 여기서 먼저 알아채고 경고 유예를 건다(바꾸면서 장치가 다시 시작되는 것은 끊김이 아니다).
    refreshBufferControls (false);

    const auto names = engine.getMidiInputNames();
    const auto newDevice = engine.getAudioDeviceName();
    const bool open = engine.isAudioDeviceOpen();
    const float newLatency = engine.getEstimatedLatencyMs();
    const auto now = juce::Time::getMillisecondCounter();

    if (fastNoticeUntil != 0 && now >= fastNoticeUntil)   // '더 빠르게' 알림을 다 보여 줬다
    {
        fastNoticeUntil = 0;
        fastNotice.clear();
        repaint();
    }

    // 10초마다 플러그인 폴더를 훑어 새로 설치된 드럼 VST를 알아챈다.
    if (++pluginScanCounter % 10 == 0)
        checkForNewPlugins();

    // 연결된 드럼 모듈(Yamaha·Roland 등)과 그 모듈에 맞는 안내
    {
        const auto module = identifyDrumModule (engine.getMidiInputDevices());
        bool driverInstalled = false;

        for (const auto& driver : engine.getAsioDriverNames())
            if (module.isKnown() && driverBrand (driver) == module.brand)
                driverInstalled = true;

        const bool driverOpen = engine.getAudioDeviceType() == "ASIO" && module.isKnown()
                                && driverBrand (engine.getAudioDeviceName()) == module.brand;
        const auto advice = moduleConnectionAdvice (module, driverInstalled, driverOpen);

        if (module.displayName() != connectedModule.displayName() || advice != moduleAdvice)
        {
            const bool brandChanged = module.brand != connectedModule.brand;
            connectedModule = module;
            moduleAdvice = advice;

            if (brandChanged)
                applyWeakTomFilter();   // 탐 음 번호는 모듈 회사마다 다르다

            if (! loadingPlugin && juce::Time::getMillisecondCounter() >= drumStatusHoldUntil)
                setDrumStatus ({}, false);   // 모듈에 맞는 맵 안내로 바꾼다

            repaint();
        }

        // 패드 배치는 MIDI 입력 이름마다 따로 둔다(알아본 모듈이면 그 입력, 아니면 첫 번째 입력).
        const auto padDevice = module.isKnown() && module.midiInput.isNotEmpty() ? module.midiInput
                                                                                 : (names.isEmpty() ? juce::String() : names[0]);

        if (! padMapLoaded || padDevice != padMapDevice)
            loadPadMapFor (padDevice);
    }

    // 설정 창이 열려 있는 동안의 장치 재시작('장치 다시 시작', 드라이버 설정 등)은 사용자가 한 일이라 끊김으로 보지 않는다.
    if (const auto starts = engine.getMonitor().streamStarts; starts != lastStreamStarts)
    {
        if (settingsWindow != nullptr && settingsWindow->isVisible())
            ignoredRestarts += (juce::int64) (starts - lastStreamStarts);

        lastStreamStarts = starts;
    }

    // 소리 끊김: 버퍼가 너무 작거나 컴퓨터가 바쁠 때 생긴다. 장치를 막 열었을 때는 세지 않는다.
    const auto problems = (juce::int64) engine.getProblemCount() - ignoredRestarts;

    if (now < xrunGraceUntil || lastXruns < 0 || problems < lastXruns)
        lastXruns = problems;
    else if (problems > lastXruns)
    {
        lastXruns = problems;
        xrunWarningUntil = now + 8000;

        if (yamahaFasterMs() > 0)
            fastRevertPending = true;   // '더 빠르게' 때문일 수 있다: 한 단계 낮춘다
    }

    // '더 빠르게'를 한 단계 낮춘다(녹음 중이면 녹음이 끝난 뒤). 그새 드라이버 창에서 Mode를 바꿨으면 할 일이 없다.
    if (fastRevertPending && yamahaFasterMs() == 0)
        fastRevertPending = false;

    if (fastRevertPending && ! engine.isRecording())
    {
        setYamahaFasterMs (yamahaFasterMs() - 1, true);
        return;   // 장치를 다시 열었으니 나머지는 다음 번에
    }

    const bool warning = now < xrunWarningUntil;

    if (names != midiNames || newDevice != deviceName || open != deviceOpen || warning != showingXrunWarning
        || std::abs (newLatency - latencyMs) > 0.05f)
    {
        midiNames = names;
        deviceName = newDevice;
        deviceOpen = open;
        showingXrunWarning = warning;
        latencyMs = newLatency;

        if (latencyMs > 0.0f)
        {
            latencyLabel.setText (K ("약 ") + juce::String (juce::roundToInt (latencyMs)) + "ms", juce::dontSendNotification);
            latencyLabel.setColour (juce::Label::textColourId, latencyMs <= 10.0f ? ui::green : (latencyMs <= 20.0f ? ui::accent : ui::red));
        }
        else
        {
            latencyLabel.setText ("-", juce::dontSendNotification);
            latencyLabel.setColour (juce::Label::textColourId, ui::textDim);
        }

        latencyLabel.setTooltip (describeLatency());
        repaint();
    }

    writeLatencyReport();
}

//==============================================================================
void MainComponent::refreshBufferControls (bool force)
{
    const auto sizes = engine.getAvailableBufferSizes();
    const int current = engine.getBufferSize();
    const double rate = engine.getDeviceSampleRate();
    const bool panel = engine.hasDriverPanel();

    // 출력 지연도 넣는다: Yamaha 드라이버 창에서 Mode만 바꿔도 장치가 다시 시작된다.
    juce::String signature;
    signature << engine.getAudioDeviceType() << '|' << engine.getAudioDeviceName() << '|' << current << '|' << rate << '|' << (panel ? 1 : 0)
              << '|' << juce::roundToInt (engine.getOutputLatencyMs() * 10.0f);

    for (auto s : sizes)
        signature << ',' << s;

    if (! force && signature == bufferSignature)
        return;

    const bool deviceChanged = bufferSignature.isNotEmpty() && signature != bufferSignature;
    bufferSignature = signature;
    driverButton.setTooltip (bufferAdviceLong (engine.getAudioDeviceType(), engine.getAudioDeviceName()));

    if (deviceChanged)
    {
        // 사용자가 장치·버퍼를 바꾼 것: 그 때문에 장치가 다시 시작된 것은 끊김 경고로 보이지 않는다.
        xrunGraceUntil = juce::Time::getMillisecondCounter() + 4000;
        xrunWarningUntil = 0;
        lastXruns = -1;
        saveAllNow();   // 버퍼·장치가 바뀌면 바로 저장
    }

    const auto describe = [rate] (int size)
    {
        return juce::String (size) + K (" 샘플 · ") + juce::String (size * 1000.0 / rate, 1) + "ms";
    };

    bufferBox.clear (juce::dontSendNotification);

    if (current <= 0 || rate <= 0.0)
    {
        bufferText.clear();
        bufferBox.setVisible (false);
        driverButton.setVisible (false);
    }
    else if (sizes.size() <= 1)
    {
        // 드라이버가 크기를 하나만 허용한다(Yamaha 등). 바꾸려면 드라이버 설정 창을 연다.
        bufferText = K ("버퍼 ") + juce::String (current) + K (" 샘플");
        bufferBox.setVisible (false);
        driverButton.setVisible (panel);
    }
    else
    {
        bufferText.clear();

        for (auto s : sizes)
            if (s >= 16 && s <= 4096)
                bufferBox.addItem (describe (s), s);

        if (! sizes.contains (current))
            bufferBox.addItem (describe (current), current);

        if (panel)
        {
            bufferBox.addSeparator();
            bufferBox.addItem (K ("드라이버 설정 열기…"), driverPanelId);
        }

        bufferBox.setSelectedId (current, juce::dontSendNotification);
        bufferBox.setVisible (true);
        driverButton.setVisible (false);
    }

    refreshFastButton();
    resized();
    repaint();
}

//------------------------------------------------------------------------------ 더 빠르게(실험): Yamaha 드라이버 숨은 값 0
bool MainComponent::isYamahaAsio() const
{
    return engine.getAudioDeviceType() == "ASIO" && driverBrand (engine.getAudioDeviceName()) == DrumModule::Brand::yamaha;
}

int MainComponent::yamahaFasterMs() const
{
    if (! isYamahaAsio())
        return 0;

    // 장치를 열 때 읽은 "LatencyMode/PeriodFrames/OutHubOffset"
    const auto parts = juce::StringArray::fromTokens (engine.getDriverSettingsAtStart(), "/", {});

    if (parts.size() < 1 || parts[0] != "0")
        return 0;

    return parts.size() >= 3 && parts[2].isNotEmpty() ? 4 - juce::jlimit (0, 4, parts[2].getIntValue()) : 4;
}

void MainComponent::refreshFastButton()
{
    // 버퍼 크기를 드라이버 창에서만 바꾸는 Yamaha 드라이버이고, 드라이버 설정(LatencyMode)이 있을 때만 보인다.
    const bool show = isYamahaAsio() && driverButton.isVisible() && ! bufferBox.isVisible()
                      && Engine::readYamahaDriverSettings().upToFirstOccurrenceOf ("/", false, false).isNotEmpty();
    const int faster = show ? yamahaFasterMs() : 0;
    fastButton.setVisible (show);
    fastButton.setButtonText (faster > 0 ? juce::String (faster) + K ("ms 빠르게") : K ("더 빠르게"));
    fastButton.setToggleState (faster > 0, juce::dontSendNotification);
}

void MainComponent::showFasterMenu()
{
    const int current = yamahaFasterMs();
    juce::PopupMenu menu;
    menu.addSectionHeader (K ("드라이버 여유 줄이기(실험) - 자글거리면 한 단계 낮추세요"));
    menu.addItem (1, K ("끄기 (드라이버 창의 Mode 그대로)"), true, current == 0);
    menu.addItem (2, K ("1ms 빠르게"), true, current == 1);
    menu.addItem (3, K ("2ms 빠르게"), true, current == 2);
    menu.addItem (4, K ("3ms 빠르게"), true, current == 3);
    menu.addItem (5, K ("4ms 빠르게 (DTX-PRO에서 자글거림)"), true, current == 4);

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (fastButton), [safe, current] (int result)
    {
        if (safe != nullptr && result >= 1 && result <= 5 && result - 1 != current)
            safe->setYamahaFasterMs (result - 1, false);
    });
}

void MainComponent::setYamahaFasterMs (int fasterMs, bool becauseOfDropouts)
{
    if (! isYamahaAsio())
        return;

    const auto now = juce::Time::getMillisecondCounter();

    if (engine.isRecording())
    {
        fastNotice = K ("녹음 중에는 바꿀 수 없습니다. 녹음을 마친 뒤 눌러 주세요.");
        fastNoticeUntil = now + 6000;
        repaint();
        return;
    }

    // 끌 때 되돌릴 Mode(드라이버 창에서 고른 것)를 기억해 둔다. 이미 0이면 전에 기억한 값을 그대로 둔다.
    const auto current = Engine::readYamahaDriverSettings().upToFirstOccurrenceOf ("/", false, false);

    if (current.isNotEmpty() && current != "0")
        settings.setValue ("yamahaModeBeforeFast", juce::jlimit (1, 3, current.getIntValue()));

    if (! Engine::writeYamahaFasterStep (juce::jlimit (0, 4, fasterMs), juce::jlimit (1, 3, settings.getIntValue ("yamahaModeBeforeFast", 1))))
    {
        fastNotice = K ("드라이버 설정을 바꾸지 못했습니다.");
        fastNoticeUntil = now + 8000;
        refreshFastButton();
        repaint();
        return;
    }

    settings.saveIfNeeded();

    // 장치를 다시 열어 드라이버가 새 값을 읽게 한다(드라이버 창에서 바꾼 것처럼). 다시 여는 동안의 끊김은 경고하지 않는다.
    yamahaReopenedFor = Engine::readYamahaDriverSettings();
    yamahaPendingSettings.clear();
    yamahaChangedAt = 0;
    fastRevertPending = false;
    xrunGraceUntil = now + 5000;
    xrunWarningUntil = 0;
    lastXruns = -1;
    engine.restartAudioDevice();
    refreshBufferControls (true);

    if (becauseOfDropouts)
        fastNotice = K ("소리가 끊겨서 '더 빠르게'를 한 단계 낮췄습니다.");
    else if (fasterMs > 0)
        fastNotice = juce::String (fasterMs) + K ("ms 빠르게 했습니다. 탐을 쳐 보고 자글거리면 한 단계 낮추세요.");
    else
        fastNotice = K ("'더 빠르게'를 끄고 드라이버 Mode를 원래대로 되돌렸습니다.");

    fastNoticeUntil = now + (becauseOfDropouts ? 20000 : 8000);
    repaint();
}

void MainComponent::bufferChosen()
{
    const int id = bufferBox.getSelectedId();

    if (id == driverPanelId)
    {
        bufferBox.setSelectedId (engine.getBufferSize(), juce::dontSendNotification);
        openDriverPanel();
        return;
    }

    if (id < 16 || id == engine.getBufferSize())
        return;

    engine.stopAudioRetry();
    juce::String error;
    bufferError = engine.setBufferSize (id, error) ? juce::String() : K ("버퍼를 바꾸지 못했습니다: ") + error;

    xrunGraceUntil = juce::Time::getMillisecondCounter() + 3000;
    xrunWarningUntil = 0;
    lastXruns = -1;

    slowRefresh();
    refreshBufferControls (true);
}

void MainComponent::checkYamahaDriverSettings()
{
    // Yamaha Steinberg USB 드라이버 창에서 Mode(Low Latency 등)나 Buffer Size를 바꾸면 드라이버는 '지연이 바뀌었다'고만 알리는데,
    // JUCE는 이 알림(kAsioLatenciesChanged)을 무시해서 장치를 다시 열 때까지 예전 지연을 알려 준다(2026-11-22 확인:
    // Mode를 바꿔도 화면이 약 11ms 그대로). 그래서 드라이버가 설정을 적어 두는 곳을 보고, 바뀌면 장치를 한 번 다시 연다.
    // 앱의 '버퍼 바꾸기'로 열었든 시작 메뉴에서 열었든 같다. 녹음 중에는 끝난 뒤에 한다.
    // 기준은 장치가 시작될 때 엔진이 읽어 둔 값이다(드라이버가 그때 읽은 값과 같다). 화면이 나중에 기준을 잡으면
    // 장치를 연 직후 바뀐 설정을 놓친다(0.2.1).
    if (engine.getAudioDeviceType() != "ASIO" || driverBrand (engine.getAudioDeviceName()) != DrumModule::Brand::yamaha)
    {
        yamahaPendingSettings.clear();
        yamahaChangedAt = 0;
        return;
    }

    const auto atStart = engine.getDriverSettingsAtStart();
    const auto current = Engine::readYamahaDriverSettings();

    // 같은 값으로 이미 한 번 다시 열었는데도 다르면(장치를 못 열었거나 드라이버가 다른 값을 쓴 경우) 되풀이하지 않는다.
    if (atStart.isEmpty() || current == atStart || current == yamahaReopenedFor)
    {
        yamahaPendingSettings.clear();
        yamahaChangedAt = 0;
        return;
    }

    // 드라이버 창에서 여러 번 바꿀 수 있으므로 같은 값이 1.5초 동안 그대로면 다시 연다.
    const auto now = juce::Time::getMillisecondCounter();

    if (current != yamahaPendingSettings)
    {
        yamahaPendingSettings = current;
        yamahaChangedAt = now;
        return;
    }

    if (now - yamahaChangedAt < 1500 || engine.isRecording())
        return;

    yamahaReopenedFor = current;
    yamahaPendingSettings.clear();
    yamahaChangedAt = 0;
    xrunGraceUntil = now + 5000;
    xrunWarningUntil = 0;
    lastXruns = -1;
    engine.restartAudioDevice();
    refreshBufferControls (true);
}

void MainComponent::openDriverPanel()
{
    engine.stopAudioRetry();
    engine.setQuietFor (4000);

    juce::Component::SafePointer<MainComponent> safe (this);
    callOnceAfterDelay (50, [safe]
    {
        if (safe == nullptr)
            return;

        safe->engine.showDriverPanel();
        safe->xrunGraceUntil = juce::Time::getMillisecondCounter() + 4000;
        safe->xrunWarningUntil = 0;
        safe->lastXruns = -1;
        safe->bufferError.clear();
        safe->slowRefresh();
        safe->refreshBufferControls (true);
    });
}

juce::String MainComponent::getLatencyHint (juce::Colour& colour) const
{
    colour = ui::textDim;

    if (! deviceOpen)
        return {};

    if (bufferError.isNotEmpty())
    {
        colour = ui::red;
        return bufferError;
    }

    if (fastNotice.isNotEmpty() && juce::Time::getMillisecondCounter() < fastNoticeUntil)
    {
        colour = ui::accent;
        return fastNotice;
    }

    if (showingXrunWarning)
    {
        colour = ui::red;
        return K ("소리가 끊겼습니다. 버퍼를 한 칸 크게 해 주세요.");
    }

    // Yamaha Steinberg USB 드라이버는 Mode에 따라 버퍼 말고도 여유 버퍼를 더 둔다. 드라이버 창이 어떤 Mode였는지는
    // 장치를 열 때 읽어 둔 레지스트리 값으로 안다(yamahaModeName). Low Latency가 아닐 때만 바꾸는 방법을 알려 준다.
    if (engine.getAudioDeviceType() == "ASIO" && driverBrand (engine.getAudioDeviceName()) == DrumModule::Brand::yamaha)
    {
        if (const int faster = yamahaFasterMs(); faster > 0)
            return juce::String (faster) + K ("ms 빠르게(실험) 쓰는 중: 탐을 칠 때 자글거리면 한 단계 낮추세요");

        if (const auto mode = engine.getDriverSettingsAtStart().upToFirstOccurrenceOf ("/", false, false); mode.getIntValue() >= 2)
        {
            colour = ui::accent;
            const int saving = juce::jmax (1, juce::roundToInt (engine.getDriverExtraLatencyMs() - yamahaLowLatencyExtraMs));
            return K ("'버퍼 바꾸기'에서 Mode를 Low Latency로 바꾸면 약 ") + juce::String (saving) + K ("ms 빨라집니다");
        }
    }

    if (latencyMs > 20.0f)
    {
        colour = ui::accent;

        if (engine.getAudioDeviceType() != "ASIO" || driverButton.isVisible())
            return bufferAdviceShort (engine.getAudioDeviceType(), engine.getAudioDeviceName());

        return K ("버퍼를 64~128 샘플로 낮춰 보세요.");
    }

    return K ("버퍼가 작을수록 빠르고, 너무 작으면 소리가 끊깁니다.");
}

juce::String MainComponent::describeLatency() const
{
    juce::String text = K ("패드 신호가 PC에 도착한 뒤 헤드폰에서 소리가 나기까지 걸리는 시간입니다.");
    const auto b = engine.getLatencyBreakdown();

    if (b.isValid())
    {
        const auto ms = [&b] (double samples) { return juce::String (b.toMs (samples), 1) + "ms"; };

        text << "\n" << K ("· 버퍼 ") << b.buffer << K (" 샘플: ") << ms (b.buffer)
             << "\n" << K ("· 드라이버가 더 두는 여유: ") << ms (b.driverExtra)
             << "\n" << K ("· 패드 신호 대기(평균): ") << ms (b.midiWait)
             << "\n" << K ("· 드럼 VST 자체 지연: ") << ms (b.plugin)
             << "\n" << K ("합계 약 ") << juce::String (b.toMs (b.totalSamples()), 1) << "ms";
    }

    // Yamaha 드라이버 창의 Mode(여유 버퍼가 Mode마다 다르다)
    if (engine.getAudioDeviceType() == "ASIO" && driverBrand (engine.getAudioDeviceName()) == DrumModule::Brand::yamaha)
    {
        if (const int faster = yamahaFasterMs(); faster > 0)
            text << "\n" << K ("· 드라이버 여유: Low Latency보다 ") << faster << K ("ms 짧게(더 빠르게, 실험)");
        else if (const auto mode = yamahaModeName (engine.getDriverSettingsAtStart().upToFirstOccurrenceOf ("/", false, false)); mode.isNotEmpty())
            text << "\n" << K ("· 드라이버 Mode: ") << mode;
    }

    text << "\n" << K ("드럼 모듈과 USB 전송 시간(대략 1~2ms)은 빠져 있습니다. 10ms 아래면 대부분 느끼지 못합니다.");
    return text;
}

void MainComponent::writeLatencyReport()
{
    // 확인용 실행(--no-save, 스냅숏 등)은 사용자가 만든 진단 파일을 덮어쓰지 않는다.
    if (! autosave)
        return;

    const auto b = engine.getLatencyBreakdown();
    const juce::String yamahaKey ("Software\\Yamaha\\Yamaha Steinberg USB Driver");
    const auto yamaha = describeRegistryKey (false, yamahaKey, 1);

    juce::String signature;
    signature << engine.getAudioDeviceType() << '|' << engine.getAudioDeviceName() << '|' << b.rate << '|' << b.buffer << '|'
              << b.reportedOutput << '|' << b.reportedInput << '|' << b.plugin << '|' << b.pluginName << '|'
              << midiNames.joinIntoString (",") << '|' << yamaha.joinIntoString (",");

    if (signature == latencyReportSignature)
        return;

    latencyReportSignature = signature;

    const auto ms = [&b] (double samples) { return juce::String (b.toMs (samples), 2) + " ms"; };
    const auto row = [&] (const char* name, double samples, const juce::String& note)
    {
        return "  " + juce::String (name).paddedRight (' ', 20) + juce::String (samples, 1).paddedLeft (' ', 7) + " samples  "
               + ms (samples).paddedLeft (' ', 9) + (note.isNotEmpty() ? "  " + note : juce::String());
    };

    juce::StringArray sizes;

    for (auto s : engine.getAvailableBufferSizes())
        sizes.add (juce::String (s));

    juce::StringArray lines;
    lines.add (juce::String ("DrumVstHost ") + JUCE_APPLICATION_VERSION_STRING + " latency report, "
               + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S"));
    lines.add ("Audio device: " + engine.getAudioDeviceType() + " / " + engine.getAudioDeviceName()
               + (b.isValid() ? ", " + juce::String (b.rate, 0) + " Hz, buffer " + juce::String (b.buffer) + " samples" : juce::String (" (not open)")));
    lines.add ("  buffer sizes the driver offers here: " + (sizes.isEmpty() ? juce::String ("-") : sizes.joinIntoString (" ")));

    if (b.isValid())
    {
        lines.add ("  latency reported by the driver: output " + juce::String (b.reportedOutput) + " samples (" + ms (b.reportedOutput)
                   + "), input " + juce::String (b.reportedInput) + " samples (" + ms (b.reportedInput) + ")");
        lines.add ("Estimated pad-to-ear latency: " + ms (b.totalSamples()) + " (the window shows " + latencyLabel.getText() + ")");
        lines.add (row ("buffer", b.buffer, {}));
        lines.add (row ("driver extra", b.driverExtra, "output latency beyond the buffer"));
        lines.add (row ("MIDI wait (average)", b.midiWait, "fixed delay " + ms (b.midiDelay) + ", the rest depends on when the hit arrives"));
        lines.add (row ("plugin", b.plugin, b.pluginName.isNotEmpty() ? b.pluginName : juce::String ("no plugin")));
        lines.add ("  not included: drum module processing and USB transfer (about 1-2 ms)");
    }

    lines.add ("MIDI inputs: " + (midiNames.isEmpty() ? juce::String ("-") : midiNames.joinIntoString (", ")));
    lines.add ("Drum module: " + (connectedModule.isKnown() ? connectedModule.displayName() : juce::String ("not recognised")));

    if (! yamaha.isEmpty())
    {
        lines.add ("Yamaha Steinberg USB Driver settings this app reads (HKCU\\" + yamahaKey + "):");

        for (auto line : yamaha)
        {
            if (line.startsWith ("LatencyMode = "))
            {
                const auto mode = line.fromFirstOccurrenceOf ("= ", false, false).trim();
                line << "  (" << (mode == "0" ? juce::String ("0: the app's 'Faster' (experimental), not offered in the driver window")
                                              : mode == "1" || mode == "2" || mode == "3" ? yamahaModeName (mode) : juce::String ("?")) << ")";
            }

            lines.add ("  " + line);
        }

        for (const auto& line : describeRegistryKey (true, yamahaKey, 1))
            lines.add ("  HKLM: " + line);
    }

    const auto file = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory()
                          .getChildFile (lang::pick ("지연 진단.txt", "Latency report.txt"));
    file.replaceWithText (lines.joinIntoString ("\r\n") + "\r\n");   // 폴더에 쓸 수 없으면 그냥 넘어간다
}

void MainComponent::showMidiMonitor()
{
    if (monitorWindow == nullptr)
    {
        juce::Component::SafePointer<MainComponent> safe (this);
        monitorWindow = std::make_unique<ToolWindow> (K ("신호 보기"),
                                                      new MidiMonitorPanel (engine, connectedModule.brand, connectedModule.displayName(), engine.getPluginName(),
                                                                            weakTomVelocity,
                                                                            [safe] (int velocity)
                                                                            {
                                                                                if (safe != nullptr)
                                                                                    safe->setWeakTomVelocity (velocity);
                                                                            }),
                                                      true);
        monitorWindow->centreAroundComponent (this, monitorWindow->getWidth(), monitorWindow->getHeight());

        // 닫으면 없앤다(받은 신호 쌓기를 멈추고, 다음에 열 때 기록을 새로 시작한다).
        monitorWindow->onClose = [safe]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->monitorWindow.reset();
            });
        };
    }

    monitorWindow->setVisible (true);
    monitorWindow->toFront (true);
}

void MainComponent::showSettings()
{
    engine.stopAudioRetry();

    if (settingsWindow == nullptr)
    {
        settingsWindow = std::make_unique<ToolWindow> (K ("소리 장치 설정"), new SettingsPanel (engine.deviceManager), false);
        settingsWindow->centreAroundComponent (this, settingsWindow->getWidth(), settingsWindow->getHeight());

        // 설정 창에서 고른 장치를 앞으로 다시 연결할 장치로 삼고 바로 저장한다.
        juce::Component::SafePointer<MainComponent> safe (this);
        settingsWindow->onClose = [safe]
        {
            if (safe == nullptr)
                return;

            safe->engine.adoptCurrentDeviceAsWanted();
            safe->xrunGraceUntil = juce::Time::getMillisecondCounter() + 4000;
            safe->xrunWarningUntil = 0;
            safe->lastXruns = -1;
            safe->saveAllNow();
        };
    }

    settingsWindow->setVisible (true);
    settingsWindow->toFront (true);
}

//==============================================================================
void MainComponent::populatePluginBox()
{
    pluginEntries = engine.findInstalledPlugins();
    knownPluginPaths.clear();

    for (const auto& entry : pluginEntries)
        knownPluginPaths.add (entry.path);

    syncPluginBox();
}

void MainComponent::checkForNewPlugins()
{
    // 앱을 켜 둔 채 드럼 VST를 새로 설치해도 목록에 저절로 나타나게 한다(파일만 훑으므로 가볍다).
    // 목록을 열어 둔 동안에는 항목 번호가 바뀌지 않게 건너뛴다.
    if (loadingPlugin || engine.isLoadingPlugin() || pluginBox.isPopupActive())
        return;

    auto entries = engine.findInstalledPlugins();
    juce::StringArray paths;

    for (const auto& entry : entries)
        paths.add (entry.path);

    if (paths == knownPluginPaths)
        return;

    juce::String newDrumName, newDrumPath;

    for (const auto& entry : entries)
    {
        if (entry.isDrum && ! knownPluginPaths.contains (entry.path))
        {
            newDrumName = entry.name;
            newDrumPath = entry.path;
            break;
        }
    }

    knownPluginPaths = paths;
    pluginEntries = std::move (entries);
    syncPluginBox();

    if (newDrumPath.isEmpty())
        return;

    if (engine.getPlugin() == nullptr)
    {
        // 아직 드럼 소리가 없으면 바로 불러온다(처음 쓰는 것이면 킷을 고를 수 있게 창도 연다).
        autoLoadNote = K ("새로 설치된 드럼 VST '") + newDrumName + K ("'를 저절로 불러왔습니다.");
        startLoadingPlugin (newDrumPath, ! engine.hasRememberedState (newDrumPath));
    }
    else
    {
        setDrumStatus (K ("새로 설치된 드럼 VST '") + newDrumName + K ("'를 찾았습니다. 위 목록에서 고르면 바로 씁니다."), false);
    }
}

void MainComponent::syncPluginBox()
{
    const auto current = engine.getPluginPath();

    if (current.isNotEmpty()
        && std::none_of (pluginEntries.begin(), pluginEntries.end(), [&] (const PluginEntry& e) { return e.path == current; }))
    {
        const auto drum = recogniseDrumPlugin (juce::File (current));
        pluginEntries.push_back ({ drum.isDrum ? drum.displayName : juce::File (current).getFileNameWithoutExtension(), current, drum.isDrum, false, drum.rank });
    }

    pluginBox.clear (juce::dontSendNotification);
    int selected = 0;
    bool drumHeading = false, otherHeading = false;

    // 드럼 VST를 위에, 그 밖의 VST3(효과·다른 악기)는 따로 아래에 둔다.
    for (size_t i = 0; i < pluginEntries.size(); ++i)
    {
        const auto& entry = pluginEntries[i];

        if (entry.isDrum && ! drumHeading)
        {
            pluginBox.addSectionHeading (K ("드럼 VST"));
            drumHeading = true;
        }
        else if (! entry.isDrum && ! otherHeading)
        {
            if (drumHeading)
                pluginBox.addSeparator();

            pluginBox.addSectionHeading (K ("다른 VST3 (드럼 소리가 아닐 수 있음)"));
            otherHeading = true;
        }

        pluginBox.addItem (entry.userAdded ? entry.name + K (" (직접 추가)") : entry.name, (int) i + 1);

        if (entry.path == current)
            selected = (int) i + 1;
    }

    pluginBox.addSeparator();
    pluginBox.addSectionHeading (K ("목록에 없는 드럼 VST"));
    pluginBox.addItem (K ("드럼 VST 직접 추가… (.vst3 파일 고르기)"), browseId);
    pluginBox.addItem (K ("VST3가 들어 있는 폴더 추가…"), browseFolderId);

    if (! engine.getExtraPluginPaths().isEmpty())
        pluginBox.addItem (K ("직접 추가한 것 목록에서 빼기…"), removeAddedId);

    pluginBox.addItem (K ("목록 새로 고치기 (방금 설치한 플러그인)"), rescanId);
    pluginBox.setSelectedId (selected, juce::dontSendNotification);
}

void MainComponent::pluginChosen()
{
    const int id = pluginBox.getSelectedId();

    if (id == browseId)
    {
        syncPluginBox();
        browseForPlugin();
        return;
    }

    if (id == browseFolderId || id == removeAddedId)
    {
        syncPluginBox();

        if (id == browseFolderId)
            browseForPluginFolder();
        else
            showRemovePluginMenu();

        return;
    }

    if (id == rescanId)
    {
        populatePluginBox();
        setDrumStatus (K ("플러그인 목록을 새로 고쳤습니다. 모두 ") + juce::String ((int) pluginEntries.size()) + K ("개입니다."), false);
        return;
    }

    if (id >= 1 && id <= (int) pluginEntries.size())
    {
        const auto path = pluginEntries[(size_t) id - 1].path;

        if (path != engine.getPluginPath())
            startLoadingPlugin (path, ! engine.hasRememberedState (path));
    }
}

void MainComponent::browseForPlugin()
{
    chooser = std::make_unique<juce::FileChooser> (K ("드럼 VST3 플러그인 파일 고르기"),
                                                   juce::File ("C:\\Program Files\\Common Files\\VST3"),
                                                   "*.vst3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file.exists())
                              {
                                  engine.addExtraPluginPath (file.getFullPathName());
                                  startLoadingPlugin (file.getFullPathName(), ! engine.hasRememberedState (file.getFullPathName()));
                              }
                          });
}

void MainComponent::browseForPluginFolder()
{
    chooser = std::make_unique<juce::FileChooser> (K ("VST3 플러그인이 들어 있는 폴더 고르기"),
                                                   juce::File ("C:\\Program Files\\Common Files\\VST3"));

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();

                              if (! folder.isDirectory())
                                  return;

                              const int before = (int) pluginEntries.size();
                              engine.addExtraPluginPath (folder.getFullPathName());
                              populatePluginBox();
                              saveAllNow();

                              const int found = (int) pluginEntries.size() - before;
                              setDrumStatus (found > 0 ? K ("폴더에서 플러그인 ") + juce::String (found) + K ("개를 찾아 목록에 넣었습니다.")
                                                       : K ("그 폴더에서 새 VST3 플러그인을 찾지 못했습니다."), found <= 0);
                          });
}

void MainComponent::showRemovePluginMenu()
{
    const auto paths = engine.getExtraPluginPaths();
    juce::PopupMenu menu;
    menu.addSectionHeader (K ("목록에서 뺄 것을 고르세요 (파일은 지우지 않습니다)"));

    for (int i = 0; i < paths.size(); ++i)
        menu.addItem (i + 1, paths[i]);

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pluginBox), [safe, paths] (int result)
    {
        if (safe == nullptr || result < 1 || result > paths.size())
            return;

        safe->engine.removeExtraPluginPath (paths[result - 1]);
        safe->populatePluginBox();
        safe->saveAllNow();
        safe->setDrumStatus (K ("목록에서 뺐습니다: ") + juce::File (paths[result - 1]).getFileName(), false);
    });
}

//------------------------------------------------------------------------------ 패드 배치
void MainComponent::loadPadMapFor (const juce::String& device)
{
    padWindow.reset();   // 다른 모듈의 배치를 고치고 있었으면 닫는다
    padMapDevice = device;
    padMapLoaded = true;
    padMap = readPadMap (settings, device);
    applyPadMap (false);
}

void MainComponent::applyPadMap (bool save)
{
    const bool active = padMap.enabled && padMap.hasSources();
    engine.setPadMap (padMap.toTable(), active);
    padButton.setToggleState (active, juce::dontSendNotification);

    if (save && autosave)
    {
        writePadMap (settings, padMapDevice, padMap);
        settings.saveIfNeeded();
    }
}

//------------------------------------------------------------------------------ 약한 탐 신호 거르기
void MainComponent::applyWeakTomFilter()
{
    engine.setMinVelocity (weakTomHitTable (connectedModule.brand, weakTomVelocity));
}

void MainComponent::setWeakTomVelocity (int maxDroppedVelocity)
{
    weakTomVelocity = juce::jlimit (0, 30, maxDroppedVelocity);
    applyWeakTomFilter();

    if (autosave)
    {
        settings.setValue ("weakTomVelocity", weakTomVelocity);
        settings.saveIfNeeded();
    }
}

void MainComponent::showPadMap()
{
    if (padWindow == nullptr)
    {
        auto* panel = new PadMapPanel (engine, padMap, padMapDevice);
        juce::Component::SafePointer<MainComponent> safe (this);
        panel->onChanged = [safe]
        {
            if (safe != nullptr)
                safe->applyPadMap (true);
        };

        padWindow = std::make_unique<ToolWindow> (K ("패드 맞추기"), panel, false);
        padWindow->centreAroundComponent (this, padWindow->getWidth(), padWindow->getHeight());
    }

    padWindow->setVisible (true);
    padWindow->toFront (true);
}

juce::Component* MainComponent::openPadMapForTest()
{
    padMap.rows[0].sources.add (35);
    padMap.rows[12].sources.addArray ({ 16, 17, 57 });
    padMap.enabled = true;
    padWindow.reset();
    showPadMap();
    return padWindow != nullptr ? padWindow->getContentComponent() : nullptr;
}

juce::Component* MainComponent::openMidiMonitorForTest()
{
    monitorWindow.reset();
    showMidiMonitor();

    // 하이햇을 칠 때 탐 3(43)에 약한 신호가 같이 들어오는 모습(2026-11-22 DTX-PRO 기록과 같은 세기)
    engine.injectMidiForTest (juce::MidiMessage::controllerEvent (10, 4, 0));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 46, (juce::uint8) 70));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 43, (juce::uint8) 4));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 44, (juce::uint8) 76));
    engine.injectMidiForTest (juce::MidiMessage::controllerEvent (10, 4, 126));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 43, (juce::uint8) 8));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 42, (juce::uint8) 65));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 43, (juce::uint8) 82));
    engine.injectMidiForTest (juce::MidiMessage::noteOn (10, 20, (juce::uint8) 64));

    return monitorWindow != nullptr ? monitorWindow->getContentComponent() : nullptr;
}

void MainComponent::startLoadingPlugin (const juce::String& path, bool openWindowAfter)
{
    if (loadingPlugin || engine.isLoadingPlugin())   // 이미 불러오는 중(겹치면 앱이 죽을 수 있다)
        return;

    pluginWindow.reset();
    loadingPlugin = true;
    updateEnabledStates();
    setDrumStatus (K ("플러그인을 불러오는 중입니다. 드럼 샘플이 크면 몇 초 걸립니다…"), false);

    // 안내 문구가 먼저 화면에 그려지도록 잠깐 뒤에 시작한다.
    juce::Component::SafePointer<MainComponent> safe (this);
    callOnceAfterDelay (80, [safe, path, openWindowAfter]
    {
        if (safe != nullptr)
            safe->finishLoadingPlugin (path, openWindowAfter);
    });
}

void MainComponent::finishLoadingPlugin (const juce::String& path, bool openWindowAfter)
{
    // 킷을 읽는 동안 생기는 늦은 콜백은 끊김 경고로 세지 않는다(따로 센다).
    engine.setQuietFor (10000);

    // 확인용: 킷을 처음 읽을 때처럼 플러그인 불러오기가 오래 걸리는 상황을 흉내 낸다(DVH_TEST_SLOW_PLUGIN=밀리초).
    // 'pump'가 붙으면 그동안 메시지를 계속 처리한다(불러오는 중에 메시지를 돌리는 플러그인 흉내).
    if (const auto slow = juce::SystemStats::getEnvironmentVariable ("DVH_TEST_SLOW_PLUGIN", {}); slow.isNotEmpty())
    {
        if (slow.containsIgnoreCase ("pump"))
            pumpMessagesFor (slow.getIntValue());
        else
            juce::Thread::sleep (slow.getIntValue());
    }

    // 불러오는 동안 앱이 꺼지면 다음에 켤 때 알 수 있게 표시를 남긴다(restoreSession이 본다).
    if (autosave)
    {
        settings.setValue ("loadingPlugin", path);
        settings.saveIfNeeded();
    }

    juce::String error;
    const bool ok = engine.loadPlugin (path, nullptr, error);

    if (autosave)
    {
        settings.removeValue ("loadingPlugin");

        if (ok)
            settings.removeValue ("loadingPluginCrashes");

        settings.saveIfNeeded();
    }

    engine.setQuietFor (6000);   // 플러그인이 샘플을 뒤에서 마저 읽는 동안
    loadingPlugin = false;
    xrunGraceUntil = juce::Time::getMillisecondCounter() + 4000;
    syncPluginBox();
    updateEnabledStates();

    if (! ok)
    {
        setDrumStatus (K ("불러오지 못했습니다: ") + error, true);
    }
    else if (! engine.getPlugin()->acceptsMidi())
    {
        setDrumStatus (K ("이 플러그인은 MIDI를 받지 않습니다. 드럼 소리를 내는 악기 플러그인을 골라 주세요."), true);
    }
    else
    {
        setDrumStatus (autoLoadNote, false);
        saveAllNow();

        if (openWindowAfter)
            showPluginWindow();
    }

    autoLoadNote.clear();
    repaint();
}

void MainComponent::setDrumStatus (const juce::String& text, bool isError)
{
    juce::String shown = text, details;

    // 모듈과 플러그인에 맞는 MIDI 맵 고르는 방법은 말풍선에 둔다(한 줄에 다 들어가지 않는다).
    if (! isError && engine.getPlugin() != nullptr)
        details = drumMapAdvice (connectedModule, engine.getPluginPath());

    if (shown.isEmpty())
    {
        if (engine.getPlugin() == nullptr)
        {
            shown = K ("드럼 소리를 낼 VST3 플러그인(EZdrummer, Superior Drummer, Addictive Drums, SSD, BFD 등)을 목록에서 고르세요.");
        }
        else
        {
            shown = details.isNotEmpty()
                        ? K ("패드를 쳐 보세요. 엉뚱한 소리가 나면 플러그인의 MIDI 맵을 ") + connectedModule.brandName()
                              + K ("용으로 바꿉니다(여기에 마우스를 올리면 방법이 나옵니다).")
                        : K ("패드를 치거나 '소리 확인'을 눌러 보세요. 소리가 안 나거나 엉뚱하면 '드럼 킷 고르기'에서 킷과 MIDI 맵을 확인합니다.");
        }
    }

    drumStatus.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    drumStatus.setText (shown, juce::dontSendNotification);
    drumStatus.setTooltip (details);
}

void MainComponent::showPluginWindow()
{
    auto* plugin = engine.getPlugin();

    if (plugin == nullptr)
        return;

    if (pluginWindow == nullptr)
    {
        engine.setQuietFor (3000);   // 큰 플러그인 화면을 처음 그리는 동안
        juce::AudioProcessorEditor* editor = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;

        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (*plugin);

        pluginWindow = std::make_unique<ToolWindow> (engine.getPluginName(), editor, editor->isResizable());
        pluginWindow->centreWithSize (pluginWindow->getWidth(), pluginWindow->getHeight());
        pluginWindow->onClose = [this] { saveAllNow(); };   // 킷을 바꿨을 테니 바로 저장
    }

    pluginWindow->setVisible (true);
    pluginWindow->toFront (true);
}

//==============================================================================
void MainComponent::browseForSong()
{
    auto startFolder = juce::File (settings.getValue ("lastSongFolder"));

    if (! startFolder.isDirectory())
        startFolder = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    chooser = std::make_unique<juce::FileChooser> (K ("같이 연주할 음악 파일 고르기"), startFolder, engine.getSongWildcard());

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file.existsAsFile())
                                  loadSong (file);
                          });
}

void MainComponent::loadSong (const juce::File& file)
{
    juce::String error;

    if (engine.loadSong (file, error))
    {
        songName.setColour (juce::Label::textColourId, ui::text);
        songName.setText (file.getFileNameWithoutExtension(), juce::dontSendNotification);
        settings.setValue ("lastSongFolder", file.getParentDirectory().getFullPathName());
        setSongStatus ({}, false);
        prepareSongTools();
    }
    else
    {
        setSongStatus (error, true, 8);
    }

    updateEnabledStates();
    repaint();
}

void MainComponent::togglePlay()
{
    if (! engine.hasSong())
        return;

    if (engine.isPlaying())
        engine.pause();
    else
        engine.play();
}

void MainComponent::toggleRecording()
{
    if (engine.isRecording())
    {
        finishRecording();
        return;
    }

    if (takeEditor != nullptr)
        takeEditor->stopPreview();

    juce::String error;

    if (! engine.startRecording (recordFolder, error))   // 고친 MIDI로 다시 만드는 중이면 거절한다
    {
        setSongStatus (error, true, 8);
        return;
    }

    recordStopAt = 0;
    recordStartedPlayback = engine.hasSong();

    if (engine.hasSong() && ! engine.isPlaying())
        engine.play();

    setSongStatus ({}, false);
}

void MainComponent::finishRecording()
{
    wasRecording = false;   // 다음 타이머에서 한 번 더 마무리하지 않게
    const double seconds = engine.getRecordedSeconds();
    engine.stopRecording();

    if (recordStartedPlayback)
        engine.pause();

    recordStopAt = 0;
    recordStartedPlayback = false;
    recordButton->setContent (IconButton::Icon::record, K ("녹음"));

    if (stemsPendingAfterRecording)   // 녹음 중에 분리가 끝났으면 이제 적용한다
    {
        stemsPendingAfterRecording = false;
        applyStems();
    }

    const auto take = engine.getLastTakeFolder();

    if (seconds < 1.0 || ! take.isDirectory())
    {
        setSongStatus (K ("녹음이 너무 짧습니다."), true, 6);
        return;
    }

    setSongStatus (K ("녹음했습니다 (") + clockText (seconds) + ")", false, 10);
    openTakeEditor (take);
}

void MainComponent::showRecordingsMenu()
{
    juce::Array<juce::File> takes;

    for (const auto& folder : findRecentTakes (recordFolder, 20))
    {
        TakeInfo info;

        if (! info.load (folder) || info.getLengthSeconds() >= 1.0)
            takes.add (folder);

        if (takes.size() >= 12)
            break;
    }

    juce::PopupMenu menu;

    if (takes.isEmpty())
    {
        menu.addItem (9999, K ("아직 녹음한 연주가 없습니다"), false);
    }
    else
    {
        menu.addSectionHeader (K ("다듬을 녹음 고르기"));

        for (int i = 0; i < takes.size(); ++i)
            menu.addItem (100 + i, describeTakeFolder (takes[i]));
    }

    menu.addSeparator();
    menu.addItem (1, K ("다른 녹음 열기…"));
    menu.addItem (2, K ("녹음 폴더 열기"));
    menu.addItem (3, K ("저장 위치 바꾸기…"));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (recordingsButton),
                        [this, takes] (int result)
                        {
                            if (result >= 100 && result < 100 + takes.size())
                            {
                                openTakeEditor (takes[result - 100]);
                            }
                            else if (result == 1)
                            {
                                browseForTake();
                            }
                            else if (result == 2)
                            {
                                recordFolder.createDirectory();
                                recordFolder.startAsProcess();
                            }
                            else if (result == 3)
                            {
                                chooser = std::make_unique<juce::FileChooser> (K ("녹음을 저장할 폴더 고르기"), recordFolder);
                                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                                      [this] (const juce::FileChooser& fc)
                                                      {
                                                          const auto folder = fc.getResult();

                                                          if (folder.isDirectory())
                                                              recordFolder = folder;
                                                      });
                            }
                        });
}

void MainComponent::browseForTake()
{
    chooser = std::make_unique<juce::FileChooser> (K ("다듬을 녹음 폴더 고르기 (드럼.wav와 원곡.wav가 있는 폴더)"), recordFolder);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();

                              if (folder == juce::File())
                                  return;

                              if (isTakeFolder (folder))
                                  openTakeEditor (folder);
                              else
                                  setSongStatus (K ("녹음 폴더가 아닙니다."), true, 6);
                          });
}

void MainComponent::openTakeEditor (const juce::File& takeFolder)
{
    closeTakeEditor();
    setSongStatus (K ("녹음을 불러오는 중…"), false);

    // 안내 문구가 먼저 그려지도록 잠깐 뒤에 불러온다(긴 녹음은 1초쯤 걸린다).
    juce::Component::SafePointer<MainComponent> safe (this);
    callOnceAfterDelay (40, [safe, takeFolder]
    {
        if (safe == nullptr)
            return;

        juce::String error;
        auto take = TakePlayer::load (takeFolder, error);

        if (take == nullptr)
        {
            safe->setSongStatus (error, true, 8);
            return;
        }

        safe->setSongStatus ({}, false);

        // 다시 만들기·되돌리기 뒤에 다시 연 창이면, 바꾸던 볼륨·타이밍·구간을 이어 쓴다(파일에는 저장하지 않는다)
        if (safe->pendingEditState.has_value() && safe->pendingEditFolder == takeFolder)
        {
            const auto carried = *safe->pendingEditState;
            auto& info = take->info;
            info.edited = carried.edited;
            info.editDrum = carried.editDrum;
            info.editSong = carried.editSong;
            info.editSongDrums = carried.editSongDrums;
            info.offsetMs = carried.offsetMs;
            info.trimStart = carried.trimStart;
            info.trimEnd = carried.trimEnd;
        }

        safe->pendingEditState.reset();
        safe->engine.setPreview (take);

        auto* editor = new TakeEditor (safe->engine, take);
        safe->takeEditor = editor;
        editor->onSeparateSongDrums = [safe]
        {
            if (safe != nullptr)
                safe->startTakeSeparation();
        };

        editor->onImportMidi = [safe] (const juce::File& file)
        {
            if (safe != nullptr)
                safe->startMidiRebuild (file);
        };

        editor->onQuantize = [safe] (double stepBeats, double strength)
        {
            if (safe != nullptr)
                safe->startMidiQuantize (stepBeats, strength);
        };

        editor->onRerenderKit = [safe]
        {
            // 연주는 그대로(세기 0 = 아무 음도 옮기지 않음), 지금 드럼 VST·킷 소리로만 다시 만든다.
            // 박자 맞추기·고친 MIDI를 거친 녹음이면 그 상태(연주 (고친 뒤).mid)를 그대로 쓴다.
            if (safe != nullptr)
                safe->beginMidiJob (K ("지금 킷 소리로 다시 만드는 중…"),
                                    [] (const TakeInfo& info, const juce::File& takeFolder, takemidi::Rebuilt& out, juce::String& error)
                                    {
                                        const auto current = TakeFiles::currentMidiIn (takeFolder);
                                        const auto source = current.existsAsFile() ? current : TakeFiles::rawMidiIn (takeFolder);
                                        return takemidi::quantizeRaw (source, info, 0.5, 0.0, out, error);
                                    });
        };

        editor->onAddGhosts = [safe] (int amount)
        {
            // 약한 스네어(고스트 노트)를 더해 다시 만든다. 박자 맞추기·고친 MIDI를 거친 녹음이면 그 상태 위에 더한다.
            if (safe != nullptr)
                safe->beginMidiJob (K ("고스트 노트를 넣는 중…"),
                                    [amount] (const TakeInfo& info, const juce::File& takeFolder, takemidi::Rebuilt& out, juce::String& error)
                                    {
                                        const auto current = TakeFiles::currentMidiIn (takeFolder);
                                        const auto source = current.existsAsFile() ? current : TakeFiles::rawMidiIn (takeFolder);
                                        return takemidi::addGhosts (source, info, amount, out, error);
                                    });
        };

        editor->onRestoreRecordedDrums = [safe]
        {
            if (safe != nullptr)
                safe->restoreRecordedDrums();
        };

        if (safe->midiJobBusy && safe->midiJobFolder == takeFolder)
            editor->showMidiState (safe->midiJobText, false, true);
        else if (safe->pendingEditMessage.isNotEmpty() && safe->pendingEditFolder == takeFolder)
            editor->showMidiState (safe->pendingEditMessage, false, false);

        safe->pendingEditMessage.clear();

        if (safe->takeSeparationBusy && safe->takeSeparationFolder == takeFolder)   // 창을 닫았다 다시 열었다
            editor->showSeparationState (safe->takeSeparationText, false, true);

        safe->takeWindow = std::make_unique<ToolWindow> (K ("녹음 다듬기 · ") + takeFolder.getFileName(), editor, false);
        safe->takeWindow->onClose = [safe]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->closeTakeEditor();
            });
        };

        auto* top = safe->getTopLevelComponent();

        if (top != nullptr)
            safe->takeWindow->centreAroundComponent (top, safe->takeWindow->getWidth(), safe->takeWindow->getHeight());

        safe->takeWindow->setVisible (true);

        // 메인 창을 내려 둔 상태(예: 곡이 끝나 녹음이 저절로 멈춤)라면 이 창도 내려 둔다.
        if (auto* mainWindow = dynamic_cast<juce::DocumentWindow*> (top); mainWindow != nullptr && mainWindow->isMinimised())
            safe->takeWindow->setMinimised (true);
        else
            safe->takeWindow->toFront (true);
    });
}

void MainComponent::closeTakeEditor()
{
    // 닫을 때는 저장하지 않는다. 다듬은 값(볼륨·타이밍·구간)은 '완성본 저장'을 눌렀을 때만 정보.xml에 남는다.
    if (takeEditor != nullptr)
        takeEditor->stopPreview();

    engine.setPreview (nullptr);
    takeWindow.reset();
    takeEditor = nullptr;
}

//==============================================================================
// 다듬기 창의 '고친 MIDI 불러오기'. 맞추기(TakeMidi)와 다시 만들기(Engine::renderDrumsFromMidi)는 뒤 스레드에서 한다.
class MainComponent::BackgroundJob final : public juce::Thread
{
public:
    explicit BackgroundJob (std::function<void (BackgroundJob&)> work)
        : juce::Thread ("midi rebuild"), fn (std::move (work)) {}

    ~BackgroundJob() override   { stopThread (60000); }
    void run() override         { if (fn != nullptr) fn (*this); }

private:
    std::function<void (BackgroundJob&)> fn;
};

void MainComponent::startMidiRebuild (const juce::File& editedMidi)
{
    beginMidiJob (K ("고친 MIDI를 원래 연주와 맞추는 중…"),
                  [editedMidi] (const TakeInfo& info, const juce::File& folder, takemidi::Rebuilt& out, juce::String& error)
                  {
                      return takemidi::rebuild (folder, info, editedMidi, out, error);
                  });
}

// 다듬기 창 '박자 맞추기': 녹음한 연주를 원곡 박 격자에 끌어당겨, 고친 MIDI와 같은 길로 드럼을 다시 만든다.
void MainComponent::startMidiQuantize (double stepBeats, double strength)
{
    beginMidiJob (K ("박자를 맞추는 중…"),
                  [stepBeats, strength] (const TakeInfo& info, const juce::File& folder, takemidi::Rebuilt& out, juce::String& error)
                  {
                      return takemidi::quantizeRaw (TakeFiles::rawMidiIn (folder), info, stepBeats, strength, out, error);
                  });
}

void MainComponent::beginMidiJob (const juce::String& busyText, MidiJobPrepare prepare)
{
    if (takeEditor == nullptr || midiJobBusy)
        return;

    if (engine.isRecording())
    {
        takeEditor->showMidiState (K ("녹음 중에는 다시 만들 수 없습니다."), true, false);
        return;
    }

    if (engine.getPlugin() == nullptr || engine.isLoadingPlugin())
    {
        takeEditor->showMidiState (K ("드럼 VST를 먼저 불러와 주세요. 다시 만들 때는 지금 고른 드럼 VST와 킷을 씁니다."), true, false);
        return;
    }

    const auto folder = takeEditor->getTakeFolder();
    auto& take = takeEditor->getTake();
    TakeInfo info = take.info;
    info.sampleRate = take.getSampleRate();
    info.lengthSamples = take.getLength();

    TakeInfo carry = take.info;
    takeEditor->captureEditState (carry);
    takeEditor->stopPreview();

    midiJob.reset();
    midiJobBusy = true;
    midiJobFolder = folder;
    midiJobText = busyText;
    takeEditor->showMidiState (midiJobText, false, true);

    juce::Component::SafePointer<MainComponent> safe (this);
    auto shownPercent = std::make_shared<std::atomic<int>> (-1);
    auto& engineRef = engine;

    midiJob = std::make_unique<BackgroundJob> ([safe, &engineRef, folder, prepare, info, carry, shownPercent] (BackgroundJob& job)
    {
        takemidi::Rebuilt rebuilt;
        juce::String error;
        bool ok = prepare (info, folder, rebuilt, error);
        const auto temp = folder.getChildFile ("drums (rebuilding).wav");

        if (ok)
        {
            const auto progress = [&job, safe, shownPercent] (double done)
            {
                const int percent = juce::jlimit (0, 100, (int) (done * 100.0));

                if (shownPercent->exchange (percent) != percent)
                    juce::MessageManager::callAsync ([safe, percent]
                    {
                        if (safe != nullptr)
                            safe->showMidiProgress (percent);
                    });

                return ! job.threadShouldExit();
            };

            ok = engineRef.renderDrumsFromMidi (rebuilt.events, info.lengthSamples, info.sampleRate, temp, progress, error);
        }

        if (ok)
        {
            // 처음 녹음한 드럼은 한 번만 따로 남긴다(여러 번 다시 만들어도 처음 것)
            const auto current = TakeFiles::drumIn (folder);
            const auto recorded = TakeFiles::recordedDrumIn (folder);
            const bool movedAway = recorded.existsAsFile() ? current.deleteFile()
                                                           : current.moveFileTo (folder.getChildFile (TakeFiles::recordedDrum()));

            if (! movedAway || ! temp.moveFileTo (current))
            {
                ok = false;
                error = K ("새 드럼 파일로 바꾸지 못했습니다. 녹음 폴더를 다른 프로그램이 쓰고 있는지 확인해 주세요.");
            }
        }

        if (ok)
        {
            // 지금 드럼.wav를 만든 연주를 남겨, 킷만 바꿔 다시 만들어도 고친 박자가 유지되게 한다(실패해도 치명적이지 않다)
            juce::String currentError;

            if (! takemidi::writeRawFormat (rebuilt.events, info, TakeFiles::rawMidiIn (folder),
                                            folder.getChildFile (TakeFiles::currentMidi()), currentError))
                DBG (currentError);
        }

        temp.deleteFile();

        auto summary = rebuilt;
        summary.events = {};

        juce::MessageManager::callAsync ([safe, folder, ok, error, summary, carry]
        {
            if (safe != nullptr)
                safe->finishMidiRebuild (folder, ok, error, summary, carry);
        });
    });

    midiJob->startThread();
}

void MainComponent::showMidiProgress (int percent)
{
    if (! midiJobBusy)
        return;

    midiJobText = K ("드럼을 다시 만드는 중… ") + juce::String (percent) + "%";

    if (takeEditor != nullptr && takeEditor->getTakeFolder() == midiJobFolder)
        takeEditor->showMidiState (midiJobText, false, true);
}

void MainComponent::finishMidiRebuild (const juce::File& folder, bool ok, const juce::String& error,
                                       const takemidi::Rebuilt& summary, const TakeInfo& carry)
{
    midiJob.reset();   // 스레드는 이미 끝나 간다
    midiJobBusy = false;
    midiJobText.clear();
    const bool editorOpen = takeEditor != nullptr && takeEditor->getTakeFolder() == folder;

    if (! ok)
    {
        if (editorOpen)
            takeEditor->showMidiState (error, true, false);
        else
            setSongStatus (error, true, 10);

        return;
    }

    auto message = K ("다시 만들었습니다: 그대로 둔 음 ") + juce::String (summary.kept)
                 + K (", 옮기거나 세기를 바꾼 음 ") + juce::String (summary.moved)
                 + K (", 더한 음 ") + juce::String (summary.added)
                 + K (", 뺀 음 ") + juce::String (summary.removed);

    if (summary.ignored > 0)
        message << K (", 모듈에 없는 악기라 넣지 못한 음 ") << juce::String (summary.ignored);

    if (summary.barOffset != 0)
        message << K (" (고친 MIDI가 ") << juce::String (std::abs (summary.barOffset)) << K ("마디 어긋나 있어 맞췄습니다)");

    if (editorOpen)
        reopenTakeEditor (folder, carry, message);
    else
        setSongStatus (message, false, 10);
}

void MainComponent::restoreRecordedDrums()
{
    if (takeEditor == nullptr || midiJobBusy)
        return;

    const auto folder = takeEditor->getTakeFolder();
    const auto recorded = TakeFiles::recordedDrumIn (folder);

    if (! recorded.existsAsFile())
        return;

    TakeInfo carry = takeEditor->getTake().info;
    takeEditor->captureEditState (carry);
    takeEditor->stopPreview();

    const auto current = TakeFiles::drumIn (folder);

    if (! current.deleteFile() || ! recorded.moveFileTo (current))
    {
        takeEditor->showMidiState (K ("되돌리지 못했습니다. 녹음 폴더를 다른 프로그램이 쓰고 있는지 확인해 주세요."), true, false);
        return;
    }

    TakeFiles::currentMidiIn (folder).deleteFile();   // 고친 연주 기록도 처음 상태로 돌아간다

    reopenTakeEditor (folder, carry, K ("처음 녹음한 드럼으로 되돌렸습니다."));
}

void MainComponent::reopenTakeEditor (const juce::File& folder, const TakeInfo& carry, const juce::String& message)
{
    pendingEditState = carry;
    pendingEditFolder = folder;
    pendingEditMessage = message;
    openTakeEditor (folder);
}

//==============================================================================
// 다듬기 창의 '원곡 드럼 분리'. 메인 창의 '원곡 드럼 빼기'와 같은 도구(separate.py)를 녹음의 원곡.wav에 쓴다.
//   원곡 드럼.wav = 원곡.wav − (드럼을 뺀 원곡). 다듬기 창은 원곡 드럼을 남길 만큼만 남기고 나머지를 뺀다.
//   separate.py는 44.1kHz만 받는다. 녹음이 48kHz 등이면 44.1kHz로 바꿔 나누고, 결과를 녹음 속도로 되돌려 뺀다
//   (뺄셈으로 만들므로 '원곡 드럼을 0으로' 하면 되돌린 결과와 정확히 같아진다).
//   작업 파일은 임시 폴더에 두고 끝나면 지운다. 다듬기 창을 닫아도 계속하며, 결과는 녹음 폴더에 남는다.
namespace
{
juce::File takeSeparationWork()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("DrumVstHost take separation");
}

juce::File songDrumsDestination (const juce::File& takeFolder)
{
    return TakeFiles::songDrumsIn (takeFolder);   // 녹음할 때 만드는 이름과 같다(화면 언어)
}
} // namespace

void MainComponent::showTakeSeparationState (const juce::File& folder, const juce::String& text, bool isError, bool busy)
{
    takeSeparationText = busy ? text : juce::String();

    if (takeEditor != nullptr && takeEditor->getTakeFolder() == folder)
        takeEditor->showSeparationState (text, isError, busy);
}

void MainComponent::startTakeSeparation()
{
    if (takeEditor == nullptr)
        return;

    const auto folder = takeEditor->getTakeFolder();

    if (takeSeparationBusy)
    {
        if (folder != takeSeparationFolder)
            takeEditor->showSeparationState (K ("다른 녹음의 원곡 드럼을 분리하는 중입니다. 끝난 뒤에 다시 눌러 주세요."), true, false,
                                             takeSeparationFolder.getFileName());

        return;
    }

    if (! toolsFolder().getChildFile ("separate.py").existsAsFile())
    {
        takeEditor->showSeparationState (K ("분석 스크립트가 없습니다"), true, false,
                                         K ("앱 폴더의 tools\\separate.py가 필요합니다. 내려받은 압축 파일을 다시 풀어 주세요."));
        return;
    }

    if (isInstallingTools() || ! analysisToolsReady (true))
    {
        takeEditor->showSeparationState (K ("드럼 분리 도구가 없습니다. 메인 창의 '원곡 드럼 빼기'를 눌러 먼저 설치해 주세요."), true, false);
        return;
    }

    const auto song = TakeFiles::songIn (folder);

    // 이미 분리해 둔 곡(곡 분석 폴더)이면 Demucs를 다시 돌리지 않고 가져다 맞춘다(cutAlignedRestForTake).
    // 후보: 지금 불러온 곡 + 녹음 정보의 곡 이름으로 시작하는 분석 폴더.
    juce::Array<juce::File> stemCandidates;

    if (engine.hasSong())
        if (const auto current = songcache::folderFor (engine.getSongFile()); songcache::hasStems (current))
            stemCandidates.add (current);

    {
        TakeInfo takeInfo;
        takeInfo.load (folder);
        const auto prefix = juce::File::createLegalFileName (takeInfo.songName).substring (0, 40).trim();

        if (prefix.isNotEmpty())
            for (const auto& child : songcache::rootFolder().findChildFiles (juce::File::findDirectories, false))
                if (child.getFileName().startsWith (prefix + " ") && songcache::hasStems (child) && ! stemCandidates.contains (child))
                    stemCandidates.add (child);
    }

    takeSeparationBusy = true;
    takeSeparationFolder = folder;
    showTakeSeparationState (folder, K ("분리 준비 중…"), false, true);

    // 원곡 파일을 읽고(필요하면 44.1kHz로 바꾸고) 나서 분리 도구를 돌린다. 큰 파일이라 뒤에서 한다.
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::Thread::launch ([safe, folder, song, stemCandidates]
    {
        juce::String error;
        double rate = 0.0;

        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (song));

            if (reader != nullptr && reader->lengthInSamples > 0)
                rate = reader->sampleRate;
        }

        const auto work = takeSeparationWork();
        work.deleteRecursively();
        auto input = song;
        bool ok = false;

        if (rate <= 0.0)
            error = K ("녹음의 원곡 파일을 읽을 수 없습니다.");
        else if (! work.createDirectory())
            error = K ("작업 폴더를 만들 수 없습니다: ") + work.getFullPathName();
        else if (std::abs (rate - 44100.0) > 0.5)
            ok = writeResampledCopy (song, input = work.getChildFile ("input.wav"), 44100.0, error);
        else
            ok = true;

        if (ok)
        {
            for (const auto& candidate : stemCandidates)
            {
                juce::String detail;
                const auto reused = work.getChildFile ("no_drums.wav");

                if (cutAlignedRestForTake (input, candidate, reused, detail))
                {
                    juce::MessageManager::callAsync ([safe, folder, reused, rate]
                    {
                        if (safe != nullptr)
                            safe->completeTakeSeparation (folder, reused, rate);
                    });
                    return;
                }
            }
        }

        juce::MessageManager::callAsync ([safe, folder, input, rate, ok, error]
        {
            if (safe == nullptr)
                return;

            if (ok)
                safe->runTakeSeparation (folder, input, rate);
            else
                safe->finishTakeSeparation (folder, false, K ("드럼을 분리하지 못했습니다"), error);
        });
    });
}

void MainComponent::runTakeSeparation (const juce::File& folder, const juce::File& input, double takeRate)
{
    const auto work = takeSeparationWork();
    auto job = std::make_unique<ProcessJob>();
    juce::Component::SafePointer<MainComponent> safe (this);

    job->onProgress = [safe, folder] (float progress)
    {
        if (safe != nullptr && safe->takeSeparationBusy)
            safe->showTakeSeparationState (folder, K ("드럼 분리 중… ") + juce::String (juce::roundToInt (progress * 100.0f)) + "%", false, true);
    };

    job->onFinished = [safe, folder, work, takeRate] (bool done, const juce::String& output)
    {
        if (safe == nullptr)
            return;

        const auto separated = work.getChildFile (separationModel).getChildFile ("no_drums.wav");

        if (! done || separated.getSize() < 1000)
        {
            safe->finishTakeSeparation (folder, false, explainToolError (output, "드럼을 분리하지 못했습니다"), lastLineOf (output));
            return;
        }

        safe->completeTakeSeparation (folder, separated, takeRate);
    };

    const juce::StringArray command { getAnalysisPython().getFullPathName(), "-I", "-X", "utf8", "-u",
                                      toolsFolder().getChildFile ("separate.py").getFullPathName(),
                                      input.getFullPathName(), work.getFullPathName(), separationModel };

    const bool started = job->start (command);
    takeSeparationJob = std::move (job);   // 끝난 작업은 다음 분리 때 바뀐다(끝났다는 알림 안에서 없애지 않는다)

    if (! started)
        finishTakeSeparation (folder, false, K ("드럼을 분리하지 못했습니다"), K ("파이썬을 실행하지 못했습니다."));
}

// 분리 결과(드럼을 뺀 원곡, 44.1kHz)로 원곡 드럼 = 원곡 − 그것을 만든다. Demucs가 끝났을 때와 분리 재사용 모두 이 길로 온다.
void MainComponent::completeTakeSeparation (const juce::File& folder, const juce::File& separated, double takeRate)
{
    showTakeSeparationState (folder, K ("마무리하는 중…"), false, true);
    const auto work = takeSeparationWork();
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::Thread::launch ([safe, folder, work, separated, takeRate]
    {
        juce::String error;
        auto rest = separated;
        bool ok = true;

        if (std::abs (takeRate - 44100.0) > 0.5)
            ok = writeResampledCopy (separated, rest = work.getChildFile ("no_drums_take.wav"), takeRate, error);

        if (ok)
            ok = makeExactDrumStem (TakeFiles::songIn (folder), rest, songDrumsDestination (folder), error);

        work.deleteRecursively();

        juce::MessageManager::callAsync ([safe, folder, ok, error]
        {
            if (safe != nullptr)
                safe->finishTakeSeparation (folder, ok, ok ? juce::String() : K ("드럼을 분리하지 못했습니다"), error);
        });
    });
}

void MainComponent::finishTakeSeparation (const juce::File& folder, bool ok, const juce::String& message, const juce::String& details)
{
    takeSeparationBusy = false;
    takeSeparationText = {};

    if (! ok)
        takeSeparationWork().deleteRecursively();

    if (takeEditor == nullptr || takeEditor->getTakeFolder() != folder)
        return;   // 창을 닫았으면 다음에 열 때 원곡 드럼 파일을 같이 읽는다

    if (ok)
        takeEditor->attachSeparatedSongDrums (songDrumsDestination (folder));
    else
        takeEditor->showSeparationState (message, true, false, details);
}

void MainComponent::setSongStatus (const juce::String& text, bool isError, int secondsToShow)
{
    songStatus.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    songStatus.setText (text, juce::dontSendNotification);
    songStatusUntil = secondsToShow > 0 ? juce::Time::getMillisecondCounter() + (juce::uint32) secondsToShow * 1000u : 0;
}

//==============================================================================
// ③ 연습 도구
//   메트로놈: 원곡을 분석해 찾은 박자표대로 클릭한다. 재생을 누르면 박자에 맞춘 4박을 먼저 세고
//             엔진이 정확한 샘플에서 원곡을 시작하므로 사용자가 맞출 것이 없다.
//   원곡 드럼 빼기: Demucs로 '드럼을 뺀 원곡'을 만들고, 원곡 − 그것 = 드럼으로 켜고 끈다.
//   결과는 곡마다 <앱 폴더>\곡 분석\ 아래에 남겨서 같은 곡은 다음부터 바로 쓴다.
void MainComponent::prepareSongTools()
{
    ++songGeneration;   // 이전 곡을 위한 작업의 결과는 이제 버린다

    // 이전 곡 작업은 멈추고 없앤다. 이미 보낸 진행률·결과 메시지도 없앤 작업을 가리키게 되어 무시된다.
    beatJob.reset();
    separationJob.reset();
    copyWaiters.clear();
    copyingOriginal = false;
    stemsPendingAfterRecording = false;
    beatBpm = 0.0;
    songCache = songcache::folderFor (engine.getSongFile());

    // 이 곡에 맞춰 둔 클릭 빠르기·시점(없으면 그대로)
    const auto tweak = readMetronomeTweak (songCache);
    clickSpeed = tweak.speed;
    engine.metronomeOffsetMs = (float) tweak.offsetMs;

    // 메트로놈: 분석해 둔 곡이면 바로, 아니면 메트로놈이 켜져 있을 때만 분석한다.
    const bool analysed = songcache::beats (songCache).existsAsFile() && showBeatMap();

    if (! analysed && metronomeButton.getToggleState())
        startBeatAnalysis();
    else
        refreshMetronomeStatus();

    // 원곡 드럼 빼기
    if (songDrumsButton.getToggleState())
    {
        if (songcache::hasStems (songCache))
            applyStems();
        else
            startSeparation();
    }
    else
    {
        setToolStatus (songDrumsStatus, {}, false);
    }
}

juce::File MainComponent::getAnalysisPython()
{
    return findAnalysisPython (settings.getValue ("analysisPython"));
}

void MainComponent::setToolStatus (juce::Label& label, const juce::String& text, bool isError, const juce::String& details)
{
    label.setColour (juce::Label::textColourId, isError ? ui::red : ui::textDim);
    label.setText (text, juce::dontSendNotification);
    label.setTooltip (details);
}

void MainComponent::ensureAnalysisCopy (std::function<void (bool, const juce::String&)> then)
{
    // 분석·분리는 앱이 직접 해독한 같은 WAV로 해야 시각이 정확히 맞는다(MP3를 따로 읽으면 수십 ms 어긋날 수 있다).
    const auto destination = songcache::original (songCache);

    if (destination.getSize() > 1000)
    {
        then (true, {});
        return;
    }

    copyWaiters.push_back (std::move (then));

    if (copyingOriginal)
        return;

    copyingOriginal = true;
    const auto song = engine.getSongFile();
    const int generation = songGeneration;
    juce::Component::SafePointer<MainComponent> safe (this);

    // 긴 곡은 해독에 몇 초 걸리므로 화면이 멈추지 않게 뒤에서 한다.
    juce::Thread::launch ([safe, song, destination, generation]
    {
        juce::String error;
        const bool ok = writeAnalysisCopy (song, destination, error);

        juce::MessageManager::callAsync ([safe, generation, ok, error]
        {
            if (safe == nullptr || generation != safe->songGeneration)
                return;

            safe->copyingOriginal = false;
            auto waiters = std::move (safe->copyWaiters);
            safe->copyWaiters.clear();

            for (auto& waiter : waiters)
                if (safe != nullptr)
                    waiter (ok, error);
        });
    });
}

//------------------------------------------------------------------------------ 메트로놈
void MainComponent::metronomeClicked()
{
    const bool on = metronomeButton.getToggleState();

    if (! engine.hasSong())
    {
        metronomeButton.setToggleState (false, juce::dontSendNotification);
        engine.metronomeOn = false;
        return;
    }

    engine.metronomeOn = on;

    if (on && ! engine.hasBeatMap())
    {
        if (! (songcache::beats (songCache).existsAsFile() && showBeatMap()))
            startBeatAnalysis (true);

        return;
    }

    refreshMetronomeStatus();
}

void MainComponent::startBeatAnalysis (bool askToInstall)
{
    if (beatJob != nullptr && beatJob->isRunning())
        return;

    if (isInstallingTools())
        return;   // 설치가 끝나면 finishToolInstall이 이어서 한다

    const auto script = toolsFolder().getChildFile ("beats.py");

    if (! script.existsAsFile())
    {
        setToolStatus (metronomeStatus, K ("분석 스크립트가 없습니다"), true,
                       K ("앱 폴더의 tools\\beats.py가 필요합니다. 내려받은 압축 파일을 다시 풀어 주세요."));
        metronomeButton.setToggleState (false, juce::dontSendNotification);
        engine.metronomeOn = false;
        return;
    }

    if (! analysisToolsReady (false))
    {
        if (askToInstall)
        {
            offerToolInstall (false);
        }
        else
        {
            setToolStatus (metronomeStatus, K ("박자 찾기 도구가 없습니다"), true,
                           K ("'메트로놈'을 누르면 박자 찾기 도구를 내려받아 설치할 수 있습니다."));
            metronomeButton.setToggleState (false, juce::dontSendNotification);
            engine.metronomeOn = false;
        }

        return;
    }

    const auto python = getAnalysisPython();

    setToolStatus (metronomeStatus, K ("박자 찾는 중…"), false, K ("곡마다 처음 한 번만 찾습니다."));

    const int generation = songGeneration;
    juce::Component::SafePointer<MainComponent> safe (this);

    ensureAnalysisCopy ([safe, generation, python, script] (bool ok, const juce::String& error)
    {
        if (safe == nullptr || generation != safe->songGeneration)
            return;

        if (! ok)
        {
            safe->finishBeatAnalysis (false, error);
            return;
        }

        auto job = std::make_unique<ProcessJob>();

        job->onProgress = [safe, generation] (float progress)
        {
            if (safe != nullptr && generation == safe->songGeneration)
                safe->setToolStatus (safe->metronomeStatus, K ("박자 찾는 중… ") + juce::String (juce::roundToInt (progress * 100.0f)) + "%", false,
                                     K ("곡마다 처음 한 번만 찾습니다."));
        };

        job->onFinished = [safe, generation] (bool done, const juce::String& output)
        {
            if (safe != nullptr && generation == safe->songGeneration)
                safe->finishBeatAnalysis (done, output);
        };

        const juce::StringArray command { python.getFullPathName(), "-I", "-X", "utf8", "-u", script.getFullPathName(),
                                          songcache::original (safe->songCache).getFullPathName(),
                                          songcache::beats (safe->songCache).getFullPathName() };

        const bool started = job->start (command);
        safe->beatJob = std::move (job);

        if (! started)
            safe->finishBeatAnalysis (false, K ("파이썬을 실행하지 못했습니다."));
    });
}

void MainComponent::finishBeatAnalysis (bool ok, const juce::String& output)
{
    if (ok && songcache::beats (songCache).existsAsFile() && showBeatMap())
        return;

    // 실패한 결과 파일이 남아 있으면 다음에 또 실패하므로 지운다.
    songcache::beats (songCache).deleteFile();

    setToolStatus (metronomeStatus, explainToolError (output, "박자를 찾지 못했습니다"), true, lastLineOf (output));
    metronomeButton.setToggleState (false, juce::dontSendNotification);
    engine.metronomeOn = false;
}

bool MainComponent::showBeatMap()
{
    BeatMap map;

    if (! map.loadJson (songcache::beats (songCache)))
        return false;

    shiftDownbeats (map, readDownbeatShift (songCache));
    beatBpm = map.bpm;

    const auto tweak = readMetronomeTweak (songCache);
    clickSpeed = tweak.speed;
    engine.metronomeOffsetMs = (float) tweak.offsetMs;
    engine.setBeatMap (std::move (map), clickSpeed);
    refreshMetronomeStatus();
    return true;
}

void MainComponent::changeMetronomeTweak (std::function<void (MetronomeTweakValues&)> change)
{
    auto tweak = readMetronomeTweak (songCache);
    MetronomeTweakValues values { tweak.speed, tweak.offsetMs };
    change (values);

    tweak.speed = values.speed >= 1.5 ? 2.0 : (values.speed <= 0.75 ? 0.5 : 1.0);
    tweak.offsetMs = juce::jlimit (-MetronomeTweak::maxOffsetMs, MetronomeTweak::maxOffsetMs, values.offsetMs);
    writeMetronomeTweak (songCache, tweak);   // 곡 분석 폴더에 기억해 두므로 다음에 이 곡을 열어도 그대로다

    const bool speedChanged = tweak.speed != clickSpeed;
    clickSpeed = tweak.speed;
    engine.metronomeOffsetMs = (float) tweak.offsetMs;

    if (speedChanged)
        showBeatMap();   // 클릭 시각표를 새 빠르기로 다시 만든다
    else
        refreshMetronomeStatus();
}

void MainComponent::refreshMetronomeStatus()
{
    if (beatJob != nullptr && beatJob->isRunning())
        return;   // 진행률을 보여 주는 중

    if (! engine.hasBeatMap())
    {
        setToolStatus (metronomeStatus, {}, false);
        metronomeStatus.setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    juce::String text = juce::String (juce::roundToInt (beatBpm * clickSpeed)) + " BPM";

    if (clickSpeed == 2.0)
        text << K (" (2배)");
    else if (clickSpeed == 0.5)
        text << K (" (절반)");

    if (const int offset = juce::roundToInt (engine.metronomeOffsetMs.load()); offset != 0)
        text << K (", 클릭 ") << clickOffsetText (offset, beatBpm > 0.0 ? 60000.0 / (beatBpm * clickSpeed) : 0.0, true);

    if (metronomeButton.getToggleState() && engine.countInEnabled.load())
        text << K (", 4박 세고 시작");

    setToolStatus (metronomeStatus, text + K ("  ▾"), false,
                   K ("원곡에서 찾은 박자입니다. 눌러서 클릭 빠르기(2배·절반), 클릭 시점(반 박·1/4박·5ms씩 앞당기기·늦추기), "
                      "'재생 전에 4박 세기', 높은 클릭(마디 첫 박) 위치를 고칩니다. 곡마다 따로 기억합니다."));
    metronomeStatus.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void MainComponent::showMetronomeMenu()
{
    if (! engine.hasBeatMap())
        return;

    const int bpm = juce::roundToInt (beatBpm);
    const int offset = juce::roundToInt (engine.metronomeOffsetMs.load());
    // 한 박 = 지금 들리는 클릭 사이 간격(화면에 보이는 BPM 기준. 2배로 켰으면 그 절반)
    const double beatMs = beatBpm > 0.0 ? 60000.0 / (beatBpm * clickSpeed) : 500.0;
    enum
    {
        countInId = 1, shiftId = 2,
        speedNormalId = 10, speedDoubleId = 11, speedHalfId = 12,
        earlierHalfId = 20, laterHalfId = 21, earlierQuarterId = 22, laterQuarterId = 23,
        earlierFineId = 24, laterFineId = 25, offsetResetId = 26
    };

    juce::PopupMenu menu;
    menu.addItem (countInId, K ("재생 전에 4박 세기"), true, engine.countInEnabled.load());
    menu.addItem (shiftId, K ("높은 클릭(마디 첫 박)을 한 박 뒤로 옮기기"));

    menu.addSectionHeader (K ("클릭 빠르기"));
    menu.addItem (speedNormalId, K ("찾은 박마다 (") + juce::String (bpm) + " BPM)", true, clickSpeed == 1.0);
    menu.addItem (speedDoubleId, K ("2배 빠르게: 박 사이에도 클릭 (") + juce::String (bpm * 2) + " BPM)", true, clickSpeed == 2.0);
    menu.addItem (speedHalfId, K ("절반으로: 한 박 걸러 클릭 (") + juce::String (juce::roundToInt (beatBpm * 0.5)) + " BPM)", true, clickSpeed == 0.5);

    // 클릭 시점: 박 단위(반 박·1/4박)로 크게, 5ms로 미세하게 옮긴다.
    const bool canEarlier = offset > -MetronomeTweak::maxOffsetMs, canLater = offset < MetronomeTweak::maxOffsetMs;
    const auto stepText = [beatMs] (double fraction) { return " (" + juce::String (juce::roundToInt (beatMs * fraction)) + "ms)"; };

    menu.addSectionHeader (K ("클릭 시점 옮기기 (지금: ") + clickOffsetText (offset, beatMs, false) + ")");
    menu.addItem (earlierHalfId, K ("◀◀ 반 박 앞당기기") + stepText (0.5), canEarlier);
    menu.addItem (laterHalfId, K ("▶▶ 반 박 늦추기") + stepText (0.5), canLater);
    menu.addItem (earlierQuarterId, K ("◀ 1/4박 앞당기기") + stepText (0.25), canEarlier);
    menu.addItem (laterQuarterId, K ("▶ 1/4박 늦추기") + stepText (0.25), canLater);
    menu.addItem (earlierFineId, K ("◁ 5ms 앞당기기 (클릭이 조금 늦게 들릴 때)"), canEarlier);
    menu.addItem (laterFineId, K ("▷ 5ms 늦추기 (클릭이 조금 급하게 들릴 때)"), canLater);
    menu.addItem (offsetResetId, K ("시점 처음대로 (0ms)"), offset != 0);

    juce::Component::SafePointer<MainComponent> safe (this);
    const int generation = songGeneration;

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (metronomeStatus), [safe, generation, beatMs] (int result)
    {
        if (safe == nullptr || generation != safe->songGeneration || result == 0)
            return;

        if (result == countInId)
        {
            safe->engine.countInEnabled = ! safe->engine.countInEnabled.load();
            safe->refreshMetronomeStatus();
            safe->saveAllNow();
        }
        else if (result == shiftId)
        {
            // 곡 분석 폴더에 기억해 두므로 다음에 이 곡을 열어도 그대로다.
            const int shift = (readDownbeatShift (safe->songCache) + 1) % 4;
            downbeatShiftFile (safe->songCache).replaceWithText (juce::String (shift));
            safe->showBeatMap();
        }
        else if (result >= speedNormalId && result <= speedHalfId)
        {
            const double speed = result == speedDoubleId ? 2.0 : (result == speedHalfId ? 0.5 : 1.0);
            safe->changeMetronomeTweak ([speed] (MetronomeTweakValues& v) { v.speed = speed; });
        }
        else if (result >= earlierHalfId && result <= offsetResetId)
        {
            const double step = result == earlierHalfId ? -0.5 * beatMs : result == laterHalfId ? 0.5 * beatMs
                              : result == earlierQuarterId ? -0.25 * beatMs : result == laterQuarterId ? 0.25 * beatMs
                              : result == earlierFineId ? -5.0 : result == laterFineId ? 5.0 : 0.0;

            // 박 단위로 옮길 때는 지금까지 옮긴 양(ms)에 더한 뒤 반올림한다(반 박 두 번 = 정확히 한 박 근처).
            safe->changeMetronomeTweak ([result, step] (MetronomeTweakValues& v)
            {
                v.offsetMs = result == offsetResetId ? 0 : juce::roundToInt ((double) v.offsetMs + step);
            });

            // 들으면서 여러 번 밀 수 있게 메뉴를 다시 연다(밖을 누르거나 Esc로 닫는다).
            if (result != offsetResetId)
                juce::MessageManager::callAsync ([safe, generation]
                {
                    if (safe != nullptr && generation == safe->songGeneration && safe->isShowing())
                        safe->showMetronomeMenu();
                });
        }
    });
}

void MainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;

    if (isInstallingTools() && (e.eventComponent == &metronomeStatus || e.eventComponent == &songDrumsStatus))
        showToolInstallMenu (*e.eventComponent);
    else if (e.eventComponent == &metronomeStatus && engine.hasBeatMap())
        showMetronomeMenu();
}

//------------------------------------------------------------------------------ 분석 도구 내려받기
bool MainComponent::analysisToolsReady (bool separation)
{
    const auto python = getAnalysisPython();

    if (! python.existsAsFile())
        return false;

    // 앱이 받아 둔 도구면 그 기능에 쓸 것이 다 설치됐는지 본다. 사용자가 직접 만든 파이썬 환경은 그대로 믿는다
    // (모자라면 실행할 때 '분석 도구 설치가 덜 됐습니다'로 알려 준다).
    if (analysis::isManagedPython (python))
        return analysis::isReady (separation ? analysis::Kind::separation : analysis::Kind::beats);

    return true;
}

bool MainComponent::isInstallingTools() const
{
    return toolInstaller != nullptr && toolInstaller->isRunning();
}

void MainComponent::offerToolInstall (bool separation)
{
    if (isInstallingTools())
        return;

    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::NoIcon)
                       .withAssociatedComponent (this);

    if (separation)
    {
        options = options.withTitle (K ("원곡 드럼 빼기 도구 받기"))
                         .withMessage (K ("원곡에서 드럼을 빼려면 드럼 분리 도구(PyTorch, Demucs)와 분리 모델이 필요합니다. 지금 내려받아 설치할까요?\n\n"
                                          "- 크기: 내려받기 약 300MB, 설치 후 약 1GB\n"
                                          "- 한 번만 받으면 됩니다. 인터넷 속도에 따라 몇 분 걸립니다.\n"
                                          "- 메트로놈 박자 찾기 도구도 함께 들어갑니다. 파이썬을 따로 설치할 필요가 없습니다."))
                         .withButton (K ("내려받기"))
                         .withButton (K ("취소"));
    }
    else
    {
        options = options.withTitle (K ("메트로놈 박자 찾기 도구 받기"))
                         .withMessage (K ("곡의 박자를 찾으려면 박자 찾기 도구가 필요합니다. 지금 내려받아 설치할까요?\n\n"
                                          "- 크기: 내려받기 약 85MB, 설치 후 약 250MB\n"
                                          "- 한 번만 받으면 됩니다. 보통 1분 안에 끝납니다.\n"
                                          "- 파이썬을 따로 설치할 필요가 없습니다.\n\n"
                                          "원곡 드럼 빼기 도구(약 1GB)까지 함께 받을 수도 있습니다."))
                         .withButton (K ("내려받기"))
                         .withButton (K ("드럼 빼기 도구까지 함께"))
                         .withButton (K ("취소"));
    }

    juce::Component::SafePointer<MainComponent> safe (this);

    // 단추 결과: 첫 단추 1, 둘째 2, 마지막(취소) 0
    juce::AlertWindow::showAsync (options, [safe, separation] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 0)
        {
            if (separation)
            {
                safe->songDrumsButton.setToggleState (false, juce::dontSendNotification);
                safe->setToolStatus (safe->songDrumsStatus, {}, false);
            }
            else
            {
                safe->metronomeButton.setToggleState (false, juce::dontSendNotification);
                safe->engine.metronomeOn = false;
                safe->setToolStatus (safe->metronomeStatus, {}, false);
            }

            return;
        }

        safe->startToolInstall (separation || result == 2);
    });
}

void MainComponent::startToolInstall (bool separation)
{
    toolInstaller = std::make_unique<analysis::Installer>();
    juce::Component::SafePointer<MainComponent> safe (this);

    toolInstaller->onProgress = [safe] (const juce::String& stage, float progress)
    {
        if (safe != nullptr)
            safe->showToolInstallProgress (stage, progress);
    };

    toolInstaller->onFinished = [safe] (bool ok, const juce::String& error)
    {
        if (safe != nullptr)
            safe->finishToolInstall (ok, error);
    };

    toolInstaller->start (separation);
    showToolInstallProgress (K ("분석 도구 준비 중…"), 0.0f);
}

void MainComponent::showToolInstallProgress (const juce::String& stage, float progress)
{
    const auto percent = juce::String (juce::roundToInt (progress * 100.0f)) + "%";
    const auto tooltip = stage + "\n" + K ("분석 도구를 내려받아 설치하고 있습니다. 끝나면 하던 일을 저절로 이어서 합니다. 여기를 누르면 그만둘 수 있습니다.");

    // 메트로놈 쪽은 자리가 넓어 단계까지, 드럼 빼기 쪽은 짧게 보여 준다.
    if (metronomeButton.getToggleState() || ! songDrumsButton.getToggleState())
    {
        setToolStatus (metronomeStatus, stage + " " + percent, false, tooltip);
        metronomeStatus.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    if (songDrumsButton.getToggleState())
    {
        setToolStatus (songDrumsStatus, K ("도구 설치 중… ") + percent, false, tooltip);
        songDrumsStatus.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
}

void MainComponent::finishToolInstall (bool ok, const juce::String& error)
{
    toolInstaller.reset();
    songDrumsStatus.setMouseCursor (juce::MouseCursor::NormalCursor);

    if (! ok)
    {
        const auto details = error + "\n" + K ("설치 기록: ") + analysis::folderForInstall().getChildFile ("install-log.txt").getFullPathName();

        if (metronomeButton.getToggleState() || ! songDrumsButton.getToggleState())
            setToolStatus (metronomeStatus, K ("도구를 설치하지 못했습니다"), true, details);

        if (songDrumsButton.getToggleState())
            setToolStatus (songDrumsStatus, K ("도구를 설치하지 못했습니다"), true, details);

        metronomeButton.setToggleState (false, juce::dontSendNotification);
        engine.metronomeOn = false;
        songDrumsButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    // 하던 일을 이어서 한다(곡이 없으면 다음에 곡을 열 때 한다).
    if (metronomeButton.getToggleState() && engine.hasSong() && ! engine.hasBeatMap())
        startBeatAnalysis (false);
    else
        refreshMetronomeStatus();

    if (songDrumsButton.getToggleState() && engine.hasSong())
    {
        if (songcache::hasStems (songCache))
            applyStems();
        else
            startSeparation (true);   // 박자 도구만 받았으면 드럼 빼기 도구를 받을지 다시 묻는다
    }
    else
    {
        setToolStatus (songDrumsStatus, {}, false);
    }
}

void MainComponent::showToolInstallMenu (juce::Component& target)
{
    juce::PopupMenu menu;
    menu.addItem (1, K ("분석 도구 설치 그만두기"));

    juce::Component::SafePointer<MainComponent> safe (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [safe] (int result)
    {
        if (safe == nullptr || result != 1 || ! safe->isInstallingTools())
            return;

        safe->toolInstaller.reset();   // 받던 것을 멈춘다(다음에 다시 받으면 받아 둔 곳부터 이어 간다)
        safe->metronomeButton.setToggleState (false, juce::dontSendNotification);
        safe->engine.metronomeOn = false;
        safe->songDrumsButton.setToggleState (false, juce::dontSendNotification);
        safe->setToolStatus (safe->metronomeStatus, K ("설치를 그만두었습니다"), false);
        safe->setToolStatus (safe->songDrumsStatus, {}, false);
        safe->songDrumsStatus.setMouseCursor (juce::MouseCursor::NormalCursor);
    });
}

//------------------------------------------------------------------------------ 원곡 드럼 빼기
void MainComponent::songDrumsClicked()
{
    const bool removeDrums = songDrumsButton.getToggleState();

    if (! engine.hasSong())
    {
        songDrumsButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    if (removeDrums)
    {
        if (songcache::hasStems (songCache))
            applyStems();
        else
            startSeparation (true);

        return;
    }

    // 원곡 드럼을 다시 켠다. 분리하던 중이면 멈춘다(다시 누르면 처음부터 한다).
    if (separationJob != nullptr && separationJob->isRunning())
        separationJob->cancel();

    stemsPendingAfterRecording = false;   // 드럼을 켜면 원곡 그대로와 같으니 녹음 뒤에 바꿀 것도 없다

    if (engine.isUsingStems())
        engine.setSongDrumsOn (true);

    setToolStatus (songDrumsStatus, {}, false);
}

void MainComponent::startSeparation (bool askToInstall)
{
    if (separationJob != nullptr && separationJob->isRunning())
        return;

    if (isInstallingTools())
    {
        // 다른 도구를 설치하는 중이면 끝난 뒤에 이어서 한다(finishToolInstall).
        setToolStatus (songDrumsStatus, K ("설치가 끝나면 이어서 합니다"), false);
        return;
    }

    const auto script = toolsFolder().getChildFile ("separate.py");

    if (! script.existsAsFile())
    {
        setToolStatus (songDrumsStatus, K ("분석 스크립트가 없습니다"), true,
                       K ("앱 폴더의 tools\\separate.py가 필요합니다. 내려받은 압축 파일을 다시 풀어 주세요."));
        songDrumsButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    if (! analysisToolsReady (true))
    {
        if (askToInstall)
        {
            offerToolInstall (true);
        }
        else
        {
            setToolStatus (songDrumsStatus, K ("드럼 빼기 도구가 없습니다"), true,
                           K ("'원곡 드럼 빼기'를 누르면 드럼 분리 도구를 내려받아 설치할 수 있습니다."));
            songDrumsButton.setToggleState (false, juce::dontSendNotification);
        }

        return;
    }

    const auto python = getAnalysisPython();

    const auto waitHint = K ("곡마다 처음 한 번만 합니다. 2분짜리 곡이면 1분 남짓 걸립니다. 그동안 연주해도 됩니다.");
    setToolStatus (songDrumsStatus, K ("드럼 분리 준비 중…"), false, waitHint);

    const int generation = songGeneration;
    juce::Component::SafePointer<MainComponent> safe (this);

    ensureAnalysisCopy ([safe, generation, python, script, waitHint] (bool ok, const juce::String& error)
    {
        if (safe == nullptr || generation != safe->songGeneration)
            return;

        if (! safe->songDrumsButton.getToggleState())   // 그사이 껐다
        {
            safe->setToolStatus (safe->songDrumsStatus, {}, false);
            return;
        }

        if (! ok)
        {
            safe->finishSeparation (false, error);
            return;
        }

        const auto work = songcache::work (safe->songCache);
        work.deleteRecursively();
        work.createDirectory();

        auto job = std::make_unique<ProcessJob>();

        job->onProgress = [safe, waitHint, generation] (float progress)
        {
            if (safe != nullptr && generation == safe->songGeneration && safe->songDrumsButton.getToggleState())
                safe->setToolStatus (safe->songDrumsStatus, K ("드럼 분리 중… ") + juce::String (juce::roundToInt (progress * 100.0f)) + "%",
                                     false, waitHint);
        };

        job->onFinished = [safe, generation] (bool done, const juce::String& output)
        {
            if (safe != nullptr && generation == safe->songGeneration)
                safe->finishSeparation (done, output);
        };

        const juce::StringArray command { python.getFullPathName(), "-I", "-X", "utf8", "-u", script.getFullPathName(),
                                          songcache::original (safe->songCache).getFullPathName(),
                                          work.getFullPathName(), separationModel };

        const bool started = job->start (command);
        safe->separationJob = std::move (job);

        if (! started)
            safe->finishSeparation (false, K ("파이썬을 실행하지 못했습니다."));
    });
}

void MainComponent::finishSeparation (bool ok, const juce::String& output)
{
    const auto work = songcache::work (songCache);
    const auto separated = work.getChildFile (separationModel).getChildFile ("no_drums.wav");

    if (! ok || separated.getSize() < 1000)
    {
        work.deleteRecursively();
        setToolStatus (songDrumsStatus, explainToolError (output, "드럼을 분리하지 못했습니다"), true, lastLineOf (output));
        songDrumsButton.setToggleState (false, juce::dontSendNotification);
        return;
    }

    if (songDrumsButton.getToggleState())
        setToolStatus (songDrumsStatus, K ("마무리하는 중…"), false);

    // 드럼 = 원곡 − (드럼을 뺀 원곡). 두 파일을 다시 더하면 원곡과 똑같다. 큰 파일이라 뒤에서 만든다.
    const int generation = songGeneration;
    const auto cache = songCache;
    juce::Component::SafePointer<MainComponent> safe (this);

    juce::Thread::launch ([safe, generation, cache, separated, work]
    {
        juce::String error;
        const auto rest = songcache::rest (cache);
        bool done = separated.moveFileTo (rest);

        if (! done)
            error = K ("분리한 파일을 옮기지 못했습니다.");
        else
            done = makeExactDrumStem (songcache::original (cache), rest, songcache::drums (cache), error);

        work.deleteRecursively();

        juce::MessageManager::callAsync ([safe, generation, done, error]
        {
            if (safe == nullptr || generation != safe->songGeneration)
                return;

            if (! done)
            {
                safe->setToolStatus (safe->songDrumsStatus, K ("드럼을 분리하지 못했습니다"), true, error);
                safe->songDrumsButton.setToggleState (false, juce::dontSendNotification);
                return;
            }

            if (safe->songDrumsButton.getToggleState())
                safe->applyStems();
            else
                safe->setToolStatus (safe->songDrumsStatus, {}, false);   // 다음에 누르면 바로 쓴다
        });
    });
}

void MainComponent::applyStems()
{
    if (! engine.isUsingStems())
    {
        // 녹음 중에 원곡을 읽는 방식을 바꾸면 녹음에 이음새가 생길 수 있어 녹음이 끝난 뒤에 바꾼다.
        if (engine.isRecording())
        {
            stemsPendingAfterRecording = true;
            setToolStatus (songDrumsStatus, K ("녹음이 끝나면 적용합니다"), false);
            return;
        }

        juce::String error;

        if (! engine.useSongStems (songcache::rest (songCache), songcache::drums (songCache), error))
        {
            setToolStatus (songDrumsStatus, K ("분리한 파일을 열지 못했습니다"), true, error);
            songDrumsButton.setToggleState (false, juce::dontSendNotification);
            return;
        }
    }

    const bool removeDrums = songDrumsButton.getToggleState();
    engine.setSongDrumsOn (! removeDrums);
    setToolStatus (songDrumsStatus, removeDrums ? K ("원곡 드럼 꺼짐") : juce::String(), false,
                   removeDrums ? K ("원곡에서 드럼만 뺀 소리를 들려주고 있습니다. 다시 누르면 원곡 그대로 돌아옵니다.") : juce::String());
}
