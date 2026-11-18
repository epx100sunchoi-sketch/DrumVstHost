#include "Look.h"

const juce::String& ui::fontName()
{
    static const juce::String name = []
    {
        const auto installed = juce::Font::findAllTypefaceNames();

        for (const char* candidate : { "Pretendard", "Noto Sans KR", "Malgun Gothic" })
            if (installed.contains (candidate))
                return juce::String (candidate);

        return juce::String ("Malgun Gothic");
    }();

    return name;
}

juce::Font ui::font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (fontName(), height, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
Look::Look()
    : juce::LookAndFeel_V4 (ColourScheme { ui::bg,          // windowBackground
                                           ui::control,     // widgetBackground
                                           ui::surface,     // menuBackground
                                           ui::outline,     // outline
                                           ui::text,        // defaultText
                                           ui::controlHi,   // defaultFill
                                           ui::text,        // highlightedText
                                           ui::controlHi,   // highlightedFill
                                           ui::text })      // menuText
{
    setDefaultSansSerifTypefaceName (ui::fontName());

    setColour (juce::ResizableWindow::backgroundColourId, ui::bg);

    setColour (juce::TextButton::buttonColourId, ui::control);
    setColour (juce::TextButton::buttonOnColourId, ui::accent);
    setColour (juce::TextButton::textColourOffId, ui::text);
    setColour (juce::TextButton::textColourOnId, ui::onAccent);

    setColour (juce::ComboBox::backgroundColourId, ui::control);
    setColour (juce::ComboBox::textColourId, ui::text);
    setColour (juce::ComboBox::outlineColourId, ui::outline);
    setColour (juce::ComboBox::arrowColourId, ui::textDim);

    setColour (juce::PopupMenu::backgroundColourId, ui::surface);
    setColour (juce::PopupMenu::textColourId, ui::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ui::controlHi);
    setColour (juce::PopupMenu::highlightedTextColourId, ui::text);

    setColour (juce::Label::textColourId, ui::text);

    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff2b3038));
    setColour (juce::TooltipWindow::textColourId, ui::text);
    setColour (juce::TooltipWindow::outlineColourId, ui::outline);

    setColour (juce::ListBox::backgroundColourId, ui::track);
    setColour (juce::ListBox::outlineColourId, ui::outline);
    setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff4a505b));

    setColour (juce::ToggleButton::textColourId, ui::text);
    setColour (juce::ToggleButton::tickColourId, ui::accent);
    setColour (juce::ToggleButton::tickDisabledColourId, ui::textDim);

    setColour (juce::AlertWindow::backgroundColourId, ui::surface);
    setColour (juce::AlertWindow::textColourId, ui::text);
    setColour (juce::AlertWindow::outlineColourId, ui::outline);
}

juce::Font Look::getTextButtonFont (juce::TextButton&, int) { return ui::font (15.0f); }
juce::Font Look::getComboBoxFont (juce::ComboBox&)          { return ui::font (15.0f); }
juce::Font Look::getPopupMenuFont()                         { return ui::font (15.0f); }

//==============================================================================
void Look::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                 bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto colour = backgroundColour;

    if (! button.isEnabled())
        colour = colour.withMultipliedAlpha (0.45f);
    else if (down)
        colour = colour.brighter (0.16f);
    else if (highlighted)
        colour = colour.brighter (0.08f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, 6.0f);

    if (! button.getToggleState())
    {
        g.setColour (ui::outline.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    }
}

void Look::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const float alpha = box.isEnabled() ? 1.0f : 0.5f;

    g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (ui::outline.withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    const float cx = (float) width - 18.0f, cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (cx - 5.0f, cy - 2.5f);
    arrow.lineTo (cx, cy + 2.5f);
    arrow.lineTo (cx + 5.0f, cy - 2.5f);

    g.setColour (ui::textDim.withMultipliedAlpha (alpha));
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void Look::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (12, 1, box.getWidth() - 42, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

//==============================================================================
int Look::getSliderThumbRadius (juce::Slider&) { return 9; }

void Look::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                             float sliderPos, float minSliderPos, float maxSliderPos,
                             juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float cy = (float) y + (float) height * 0.5f;
    const juce::Rectangle<float> track ((float) x, cy - 3.0f, (float) width, 6.0f);

    g.setColour (ui::track);
    g.fillRoundedRectangle (track, 3.0f);

    const bool bipolar = (bool) slider.getProperties().getWithDefault ("bipolar", false);

    if (bipolar)
    {
        // 가운데(0)에서 손잡이까지 칠하고, 0 자리에 작은 눈금을 둔다.
        const float zeroX = (float) slider.getPositionOfValue (0.0);
        const float from = juce::jmin (zeroX, sliderPos), to = juce::jmax (zeroX, sliderPos);

        if (to - from > 1.0f)
        {
            g.setColour (slider.isEnabled() ? ui::accent : ui::textDim);
            g.fillRect (juce::Rectangle<float> (from, track.getY(), to - from, track.getHeight()));
        }

        g.setColour (ui::textDim);
        g.fillRect (zeroX - 1.0f, cy - 9.0f, 2.0f, 18.0f);
    }
    else if (sliderPos > track.getX() + 1.0f)
    {
        g.setColour (slider.isEnabled() ? ui::accent : ui::textDim);
        g.fillRoundedRectangle (track.withRight (sliderPos), 3.0f);
    }

    juce::Rectangle<float> thumb (0.0f, 0.0f, 16.0f, 26.0f);
    thumb.setCentre (sliderPos, cy);

    g.setColour (slider.isMouseOverOrDragging() ? juce::Colours::white : ui::text);
    g.fillRoundedRectangle (thumb, 4.0f);
    g.setColour (ui::bg.withAlpha (0.55f));
    g.fillRect (sliderPos - 0.75f, cy - 6.0f, 1.5f, 12.0f);
}

void Look::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                             float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (3.0f);
    const auto centre = bounds.getCentre();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 1.5f;
    const float angle = startAngle + pos * (endAngle - startAngle);
    const juce::PathStrokeType stroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (ui::track);
    g.strokePath (arc, stroke);

    const bool bipolar = (bool) slider.getProperties().getWithDefault ("bipolar", false);
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    if (std::abs (angle - from) > 0.02f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (ui::accent);
        g.strokePath (value, stroke);
    }

    const float body = radius - 5.5f;
    g.setColour (slider.isMouseOverOrDragging() ? ui::controlHi : ui::control);
    g.fillEllipse (centre.x - body, centre.y - body, body * 2.0f, body * 2.0f);
    g.setColour (ui::outline);
    g.drawEllipse (centre.x - body, centre.y - body, body * 2.0f, body * 2.0f, 1.0f);

    const auto tip = centre.getPointOnCircumference (body - 2.5f, angle);
    const auto inner = centre.getPointOnCircumference (body * 0.35f, angle);
    g.setColour (ui::text);
    g.drawLine ({ inner, tip }, 2.0f);
}
