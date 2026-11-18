#pragma once

#include <JuceHeader.h>

// 화면 언어. 소스의 글자는 한국어로 적고, 영어일 때는 번역표(Language.cpp)에서 찾아 바꾼다.
namespace lang
{
bool isEnglish() noexcept;
void setEnglish (bool shouldBeEnglish) noexcept;      // 화면을 만들기 전에 한 번 정한다(Main.cpp)
const char* toEnglish (const char* koreanUtf8);       // 번역표에 없으면 nullptr

// 번역표를 거치지 않고 두 언어 글자를 직접 준다(파일·폴더 이름처럼 화면 글자와 겹치는 낱말).
inline juce::String pick (const char* korean, const char* english)
{
    return juce::String::fromUTF8 (isEnglish() ? english : korean);
}
}

// 소스의 한글(UTF-8) 문자열을 juce::String으로 바꾼다. 영어 화면이면 번역해서 준다(번역이 없으면 한국어 그대로).
inline juce::String K (const char* utf8)
{
    if (lang::isEnglish())
        if (const char* english = lang::toEnglish (utf8))
            return juce::String::fromUTF8 (english);

    return juce::String::fromUTF8 (utf8);
}

// 사용자 자료가 든 폴더·파일. 지금 언어의 이름이 없고 다른 언어로 만든 것이 이미 있으면 그것을 쓴다
// (언어를 바꾸거나 새 버전으로 바꿔도 녹음·분석 결과가 그대로 보이게).
inline juce::File dataChild (const juce::File& parent, const char* korean, const char* english)
{
    const auto preferred = parent.getChildFile (lang::pick (korean, english));
    const auto other = parent.getChildFile (lang::pick (english, korean));
    return ! preferred.exists() && other.exists() ? other : preferred;
}

// milliseconds 뒤에 function을 한 번만 부른다(메시지 스레드).
// JUCE의 Timer::callAfterDelay는 콜백이 끝날 때까지 타이머가 살아 있어서, 콜백 안에서 메시지가 돌면
// (예: BFD Player가 불러오는 동안 창을 그리며 메시지를 처리함) 같은 콜백이 또 불려 일이 겹치고,
// 끝에서 같은 객체를 두 번 지워 앱이 죽는다(2026-11-18 실제로 일어남). 여기서는 일을 하기 전에 멈추고 정리한다.
inline void callOnceAfterDelay (int milliseconds, std::function<void()> function)
{
    struct Once final : private juce::Timer, private juce::DeletedAtShutdown
    {
        Once (int ms, std::function<void()> f) : fn (std::move (f))   { startTimer (juce::jmax (1, ms)); }

        void timerCallback() override
        {
            stopTimer();
            auto f = std::move (fn);
            delete this;   // 아래 일을 하는 동안 메시지가 돌아도 다시 불리지 않는다

            if (f != nullptr)
                f();
        }

        std::function<void()> fn;
    };

    new Once (milliseconds, std::move (function));
}

// 초를 "2:07" 꼴로 바꾼다.
inline juce::String clockText (double seconds)
{
    const int total = juce::jmax (0, (int) std::floor (seconds + 0.0001));
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}
