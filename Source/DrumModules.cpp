#include "DrumModules.h"

namespace
{
bool isWordChar (juce::juce_wchar c) noexcept
{
    return juce::CharacterFunctions::isLetterOrDigit (c);
}

bool containsAny (const juce::String& upper, std::initializer_list<const char*> words)
{
    for (const char* word : words)
        if (upper.contains (word))
            return true;

    return false;
}

} // namespace

//==============================================================================
juce::String findRolandModel (const juce::String& text)
{
    const auto upper = text.toUpperCase();

    // TD-17, TD17, TD 27, TD-50X, TD-1(DMK) — 앞뒤가 다른 낱말에 붙어 있으면 아니다(예: "STD-1").
    for (int i = upper.indexOf ("TD"); i >= 0; i = upper.indexOf (i + 1, "TD"))
    {
        if (i > 0 && isWordChar (upper[i - 1]))
            continue;

        int p = i + 2;

        if (p < upper.length() && (upper[p] == '-' || upper[p] == ' '))
            ++p;

        const int digitsStart = p;

        while (p < upper.length() && juce::CharacterFunctions::isDigit (upper[p]) && p - digitsStart < 3)
            ++p;

        if (p == digitsStart)
            continue;

        auto model = "TD-" + upper.substring (digitsStart, p);

        if (p < upper.length() && upper[p] == 'X')
        {
            model << 'X';
            ++p;
        }

        return model;
    }

    // V-Drums 3/5/7 시리즈 모듈(V31, V51, V71)
    for (const char* model : { "V31", "V51", "V71" })
    {
        const int i = upper.indexOf (model);

        if (i >= 0 && (i == 0 || ! isWordChar (upper[i - 1])) && ! isWordChar (upper[i + 3]))
            return model;
    }

    return {};
}

juce::String DrumModule::brandName() const
{
    switch (brand)
    {
        case Brand::yamaha: return "Yamaha";
        case Brand::roland: return "Roland";
        case Brand::alesis: return "Alesis";
        case Brand::efnote: return "EFNOTE";
        case Brand::none:   break;
    }

    return {};
}

juce::String DrumModule::displayName() const
{
    if (! isKnown())
        return midiInput;

    return model.isNotEmpty() ? brandName() + " " + model : brandName() + " (" + midiInput + ")";
}

DrumModule identifyDrumModule (const juce::Array<juce::MidiDeviceInfo>& midiInputs)
{
    DrumModule best;

    for (const auto& input : midiInputs)
    {
        DrumModule module;
        module.midiInput = input.name;
        const auto upper = input.name.toUpperCase();
        const auto id = input.identifier.toLowerCase();   // Windows 장치 경로: USB 제조사 번호(vid_xxxx)가 들어 있다

        if (const auto model = findRolandModel (upper); model.isNotEmpty())
        {
            module.brand = DrumModule::Brand::roland;
            module.model = model;
            module.usbAudio = model != "TD-1";   // TD-1(TD-1DMK)은 USB MIDI만 된다
        }
        else if (upper.contains ("ROLAND") || id.contains ("vid_0582"))
        {
            module.brand = DrumModule::Brand::roland;
        }
        else if (containsAny (upper, { "DTX", "EAD10", "YAMAHA" }) || id.contains ("vid_0499"))
        {
            // DTX-PRO는 Windows에서 "DTX Drums-1"로 보인다(2026-11-18 이 PC).
            module.brand = DrumModule::Brand::yamaha;

            for (const char* model : { "DTX-PROX", "DTX-PRO", "EAD10" })
            {
                if (upper.contains (model))
                {
                    module.model = model;
                    break;
                }
            }
        }
        else if (upper.contains ("ALESIS"))
        {
            module.brand = DrumModule::Brand::alesis;
        }
        else if (upper.contains ("EFNOTE"))
        {
            module.brand = DrumModule::Brand::efnote;
        }

        // 모델까지 알아본 것을 가장 먼저, 그다음 회사만 알아본 것
        if (module.isKnown() && (! best.isKnown() || (best.model.isEmpty() && module.model.isNotEmpty())))
            best = module;
    }

    return best;
}

DrumModule::Brand driverBrand (const juce::String& deviceName)
{
    const auto upper = deviceName.toUpperCase();

    if (findRolandModel (upper).isNotEmpty() || upper.contains ("ROLAND"))
        return DrumModule::Brand::roland;

    if (upper.contains ("YAMAHA") || upper.contains ("DTX"))
        return DrumModule::Brand::yamaha;

    if (upper.contains ("ALESIS"))
        return DrumModule::Brand::alesis;

    if (upper.contains ("EFNOTE"))
        return DrumModule::Brand::efnote;

    return DrumModule::Brand::none;
}

int drumDriverScore (const juce::String& asioDriverName, const DrumModule& connected)
{
    const auto brand = driverBrand (asioDriverName);

    if (brand == DrumModule::Brand::none)
        return 0;

    int score = 10;

    if (connected.isKnown() && brand == connected.brand)
        score += 20;

    if (connected.model.isNotEmpty() && findRolandModel (asioDriverName) == connected.model)
        score += 40;

    return score;
}

//==============================================================================
juce::String bufferAdviceShort (const juce::String& deviceType, const juce::String& deviceName)
{
    if (deviceType != "ASIO")
        return K ("소리 장치 설정에서 ASIO를 고르면 빨라집니다.");

    switch (driverBrand (deviceName))
    {
        case DrumModule::Brand::yamaha: return K ("드라이버 창에서 Buffer Size 64~128, Mode는 Low Latency로");
        case DrumModule::Brand::roland: return K ("드라이버 창에서 Audio Buffer Size를 작게(64~128)");
        case DrumModule::Brand::alesis:
        case DrumModule::Brand::efnote:
        case DrumModule::Brand::none:   break;
    }

    return K ("드라이버 창에서 버퍼(Buffer Size)를 64~128로");
}

juce::String bufferAdviceLong (const juce::String& deviceType, const juce::String& deviceName)
{
    if (deviceType != "ASIO")
        return K ("지금은 Windows 기본 방식이라 지연이 큽니다. '소리 장치 설정'에서 오디오 방식을 ASIO로 고르세요.");

    switch (driverBrand (deviceName))
    {
        case DrumModule::Brand::yamaha:
            return K ("드럼 모듈 드라이버(Yamaha Steinberg USB) 설정 창을 엽니다. ASIO 탭에서 Buffer Size를 작게(64~128), "
                      "Mode를 Low Latency로 바꾸면 지연이 줄어듭니다. 소리가 끊기면 한 칸 크게 하세요.");
        case DrumModule::Brand::roland:
            return K ("드럼 모듈 드라이버(Roland) 설정 창을 엽니다. Audio Buffer Size를 작게(보통 64~128) 하면 지연이 줄어듭니다. "
                      "창이 안 열리면 시작 메뉴 > Roland > (모델 이름) Driver Settings에서 바꿉니다. 소리가 끊기면 한 칸 크게 하세요.");
        case DrumModule::Brand::alesis:
        case DrumModule::Brand::efnote:
        case DrumModule::Brand::none:
            break;
    }

    return K ("소리 장치 드라이버의 설정 창을 엽니다. 버퍼(Buffer Size)를 작게(64~128) 하면 지연이 줄어듭니다. 소리가 끊기면 한 칸 크게 하세요.");
}

juce::String moduleConnectionAdvice (const DrumModule& module, bool moduleDriverInstalled, bool moduleDriverOpen)
{
    if (module.brand != DrumModule::Brand::roland)
        return {};

    if (! module.usbAudio)
        return K ("이 모듈은 USB로 MIDI만 보냅니다. 소리는 PC의 헤드폰 단자로 들어요");

    if (! moduleDriverInstalled)
        return K ("모듈 헤드폰으로 들으려면 모듈의 USB Driver Mode를 VENDOR로 바꾸고 껐다 켜세요");

    if (! moduleDriverOpen)
        return K ("모듈 헤드폰으로 들으려면 소리 장치 설정에서 ASIO와 Roland 드라이버를 고르세요");

    return {};
}
