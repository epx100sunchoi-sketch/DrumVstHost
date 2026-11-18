#pragma once

#include <JuceHeader.h>

// 화면 전체에서 쓰는 색과 글꼴.
namespace ui
{
inline const juce::Colour bg        { 0xff14161a };
inline const juce::Colour surface   { 0xff1b1e23 };
inline const juce::Colour control   { 0xff272b32 };
inline const juce::Colour controlHi { 0xff343a45 };
inline const juce::Colour line      { 0xff2b3037 };
inline const juce::Colour outline   { 0xff3a404a };
inline const juce::Colour track     { 0xff0c0e11 };
inline const juce::Colour text      { 0xffe8eaed };
inline const juce::Colour textDim   { 0xff959ca7 };
inline const juce::Colour accent    { 0xffeda73c };
inline const juce::Colour onAccent  { 0xff1d1404 };
inline const juce::Colour green     { 0xff5cc07c };
inline const juce::Colour red       { 0xffe5484d };

const juce::String& fontName();
juce::Font font (float height, bool bold = false);
}

// Windows 제목 표시줄을 어두운 색으로 바꾼다(Platform.cpp).
void setDarkTitleBar (void* nativeWindowHandle);

//==============================================================================
class Look final : public juce::LookAndFeel_V4
{
public:
    Look();

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    int getSliderThumbRadius (juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;
};
