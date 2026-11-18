#pragma once

#include <JuceHeader.h>
#include "Common.h"
#include "Look.h"

// 여러 창에서 같이 쓰는 작은 부품들.

inline void styleButton (juce::Button& button, const juce::String& tooltip = {})
{
    button.setMouseClickGrabsKeyboardFocus (false);
    button.setWantsKeyboardFocus (false);

    if (tooltip.isNotEmpty())
        button.setTooltip (tooltip);
}

inline void styleLabel (juce::Label& label, float size, juce::Colour colour)
{
    label.setFont (ui::font (size));
    label.setColour (juce::Label::textColourId, colour);
    label.setMinimumHorizontalScale (1.0f);
    label.setInterceptsMouseClicks (false, false);
    label.setBorderSize ({ 0, 0, 0, 0 });
}

//==============================================================================
// 아이콘 + 글자 버튼 (재생, 처음으로, 녹음).
class IconButton final : public juce::Button
{
public:
    enum class Icon { play, pause, rewind, record, stop };

    IconButton() : juce::Button ({}) { styleButton (*this); }

    void setContent (Icon newIcon, const juce::String& newText, bool emphasised = false)
    {
        if (icon == newIcon && text == newText && strong == emphasised)
            return;

        icon = newIcon;
        text = newText;
        strong = emphasised;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const float alpha = isEnabled() ? 1.0f : 0.45f;
        auto fill = strong ? ui::red : ui::control;

        if (! isEnabled())
            fill = fill.withMultipliedAlpha (0.45f);
        else if (down)
            fill = fill.brighter (0.16f);
        else if (highlighted)
            fill = fill.brighter (0.08f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 6.0f);

        if (! strong)
        {
            g.setColour (ui::outline.withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));
            g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
        }

        const auto font = ui::font (15.0f);
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text);
        const float iconWidth = 12.0f, gap = 8.0f;
        const float left = bounds.getCentreX() - (iconWidth + gap + textWidth) * 0.5f;
        const float cy = bounds.getCentreY();
        const auto contentColour = (strong ? juce::Colours::white : ui::text).withMultipliedAlpha (alpha);

        g.setColour (icon == Icon::record && ! strong ? ui::red.withMultipliedAlpha (alpha) : contentColour);
        g.fillPath (makeIcon (icon, left + iconWidth * 0.5f, cy));

        g.setColour (contentColour);
        g.setFont (font);
        g.drawText (text, juce::Rectangle<float> (left + iconWidth + gap, bounds.getY(), textWidth + 6.0f, bounds.getHeight()),
                    juce::Justification::centredLeft, false);
    }

private:
    static juce::Path makeIcon (Icon icon, float cx, float cy)
    {
        juce::Path p;

        switch (icon)
        {
            case Icon::play:
                p.addTriangle (cx - 4.5f, cy - 6.0f, cx - 4.5f, cy + 6.0f, cx + 6.0f, cy);
                break;

            case Icon::pause:
                p.addRectangle (cx - 5.0f, cy - 6.0f, 3.6f, 12.0f);
                p.addRectangle (cx + 1.4f, cy - 6.0f, 3.6f, 12.0f);
                break;

            case Icon::rewind:
                p.addRectangle (cx - 6.0f, cy - 6.0f, 2.6f, 12.0f);
                p.addTriangle (cx + 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f, cx - 2.6f, cy);
                break;

            case Icon::record:
                p.addEllipse (cx - 5.5f, cy - 5.5f, 11.0f, 11.0f);
                break;

            case Icon::stop:
                p.addRoundedRectangle (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.5f);
                break;
        }

        return p;
    }

    Icon icon = Icon::play;
    juce::String text;
    bool strong = false;
};

//==============================================================================
// 플러그인 창·설정 창·녹음 다듬기 창에 쓰는 보조 창. 닫기를 누르면 숨기고 onClose를 부른다.
class ToolWindow final : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& title, juce::Component* contentToOwn, bool canResize)
        : juce::DocumentWindow (title, ui::surface, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (contentToOwn, true);
        setResizable (canResize, false);

        if (auto* peer = getPeer())
            setDarkTitleBar (peer->getNativeHandle());
    }

    std::function<void()> onClose;

    void closeButtonPressed() override
    {
        setVisible (false);

        if (onClose != nullptr)
            onClose();
    }
};
