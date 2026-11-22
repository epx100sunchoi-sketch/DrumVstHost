#include "DrumPlugins.h"

namespace
{
// 영문·숫자만 남기고 소문자로("Superior Drummer 3.vst3" → "superiordrummer3vst3")
juce::String squash (const juce::String& text)
{
    juce::String out;

    for (auto c : text.toLowerCase())
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            out << juce::String::charToString (c);

    return out;
}

struct Known
{
    const char* pattern;        // squash한 파일 이름이 이것으로 시작하면
    const char* displayName;
    DrumPluginInfo::Family family;
    int rank;
};

// 순서가 중요하다: 더 구체적인 이름을 먼저 둔다. rank는 저절로 불러올 때의 순서(사용자가 주로 쓰는 Toontrack·XLN을 먼저).
const Known knownDrumPlugins[] =
{
    { "superiordrummer3", "Superior Drummer 3 (Toontrack)",   DrumPluginInfo::Family::superiorDrummer, 10 },
    { "superiordrummer",  "Superior Drummer (Toontrack)",     DrumPluginInfo::Family::superiorDrummer, 11 },
    { "ezdrummer3",       "EZdrummer 3 (Toontrack)",          DrumPluginInfo::Family::ezdrummer,       20 },
    { "ezdrummer",        "EZdrummer (Toontrack)",            DrumPluginInfo::Family::ezdrummer,       21 },
    { "addictivedrums2",  "Addictive Drums 2 (XLN Audio)",    DrumPluginInfo::Family::addictiveDrums,  30 },
    { "addictivedrums",   "Addictive Drums (XLN Audio)",      DrumPluginInfo::Family::addictiveDrums,  31 },
    { "ssdsampler",       "SSD 5 (Steven Slate Drums)",       DrumPluginInfo::Family::ssd,             40 },
    { "stevenslatedrums", "Steven Slate Drums",               DrumPluginInfo::Family::ssd,             41 },
    { "bfd3",             "BFD3",                             DrumPluginInfo::Family::bfd,             50 },
    { "bfdplayer",        "BFD Player",                       DrumPluginInfo::Family::bfd,             52 },
    { "bfd",              "BFD",                              DrumPluginInfo::Family::bfd,             51 },
    { "mtpowerdrum",      "MT Power Drum Kit",                DrumPluginInfo::Family::other,           60 },
    { "perfectdrums",     "Perfect Drums",                    DrumPluginInfo::Family::other,           65 },
    { "grooveagent",      "Groove Agent (Steinberg)",         DrumPluginInfo::Family::other,           70 },
    { "battery",          "Battery (Native Instruments)",     DrumPluginInfo::Family::other,           80 },
    { "drumgizmo",        "DrumGizmo",                        DrumPluginInfo::Family::other,           85 },
    { "sitala",           "Sitala",                           DrumPluginInfo::Family::other,           90 },
};

// VST3 묶음의 Contents\moduleinfo.json(VST3 SDK 3.7.6+)에 'Drum' 분류가 있는지. 플러그인을 띄우지 않고 본다.
// 형식이 JSON5일 수 있어서(주석·끝 쉼표) 파싱하지 않고 "Sub Categories" 목록 안의 글자만 본다.
bool moduleInfoSaysDrum (const juce::File& pluginFile)
{
    const auto info = pluginFile.getChildFile ("Contents").getChildFile ("moduleinfo.json");

    if (! pluginFile.isDirectory() || ! info.existsAsFile() || info.getSize() > 1024 * 1024)
        return false;

    const auto text = info.loadFileAsString();

    for (int i = text.indexOf ("Sub Categories"); i >= 0; i = text.indexOf (i + 1, "Sub Categories"))
    {
        const int open = text.indexOf (i, "[");
        const int close = open >= 0 ? text.indexOf (open, "]") : -1;

        if (open >= 0 && close > open && text.substring (open, close).containsIgnoreCase ("Drum"))
            return true;
    }

    return false;
}
} // namespace

//==============================================================================
DrumPluginInfo recogniseDrumPlugin (const juce::File& pluginFile)
{
    DrumPluginInfo info;
    const auto key = squash (pluginFile.getFileNameWithoutExtension());

    for (const auto& known : knownDrumPlugins)
    {
        if (key.startsWith (known.pattern))
        {
            info.isDrum = true;
            info.family = known.family;
            info.displayName = known.displayName;
            info.rank = known.rank;
            return info;
        }
    }

    if (moduleInfoSaysDrum (pluginFile))
    {
        info.isDrum = true;
        info.family = DrumPluginInfo::Family::other;
        info.displayName = pluginFile.getFileNameWithoutExtension();
        info.rank = 200;
    }

    return info;
}

DrumPluginInfo::Family drumPluginFamily (const juce::String& nameOrPath)
{
    const auto key = squash (juce::File::isAbsolutePath (nameOrPath) ? juce::File (nameOrPath).getFileNameWithoutExtension() : nameOrPath);

    for (const auto& known : knownDrumPlugins)
        if (key.startsWith (known.pattern))
            return known.family;

    return DrumPluginInfo::Family::none;
}

juce::String drumMapAdvice (const DrumModule& module, const juce::String& pluginNameOrPath)
{
    if (! module.isKnown())
        return {};

    using Family = DrumPluginInfo::Family;
    const bool roland = module.brand == DrumModule::Brand::roland;
    const bool yamaha = module.brand == DrumModule::Brand::yamaha;
    const auto family = drumPluginFamily (pluginNameOrPath);
    juce::String text;

    // 이 PC의 SSD 5.5(Steven Slate)·BFD Player 맵 목록 기준(2026-11-18). TD-17/27/50, DTX-PRO 전용 맵은 없어서
    // 기본 음 배치가 같은 TD-30·DTX900을 고른다.
    if (family == Family::ssd && (roland || yamaha))
    {
        text << K ("SSD: '드럼 킷 고르기' 창 → Mapping → Input Conversion → Load Preset → ")
             << (roland ? "Roland TD30" : "Yamaha DTX900") << K (" → Open → Set as Default");
    }
    else if (family == Family::bfd && yamaha)
    {
        // 앱이 설치하는 3존 맵(Crash·Ride의 몸통·엣지·벨, 탐 림)을 먼저 권한다.
        text << K ("BFD: '드럼 킷 고르기' 창 → 프리셋 이름 옆 ▼ → Engine → Keymap → ")
             << (isBundledBfdKeymapInstalled() ? K ("Yamaha DTX-PRO (DrumVstHost)  (심벌 3존을 모두 씁니다. 목록에 없으면 플러그인을 다시 불러오세요)")
                                               : juce::String ("Yamaha DTX900"));
    }
    else if (family == Family::bfd && roland)
    {
        text << K ("BFD: '드럼 킷 고르기' 창 → 프리셋 이름 옆 ▼ → Engine → Keymap → Roland TD-30");
    }
    else
    {
        text << K ("'드럼 킷 고르기' 창의 MIDI 맵(e-drum map) 설정에서 ") << module.displayName() << K ("에 맞는 맵을 고르세요.");
    }

    if (roland)
        text << K ("\n모듈 자체 소리가 같이 나면 모듈 설정에서 Local Control을 OFF로 하세요(모듈만으로 칠 때는 다시 ON).");

    return text;
}

//==============================================================================
namespace
{
juce::File bundledBfdKeymapFolder()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getChildFile ("tools").getChildFile ("bfd");
}

// C:\ProgramData\BFD Drums\<BFD Player, BFD3 …>\System\Maps
juce::Array<juce::File> bfdKeymapFolders()
{
    juce::Array<juce::File> result;
    const auto root = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory).getChildFile ("BFD Drums");

    for (const auto& product : root.findChildFiles (juce::File::findDirectories, false))
        if (const auto maps = product.getChildFile ("System").getChildFile ("Maps"); maps.isDirectory())
            result.add (maps);

    return result;
}
} // namespace

int installBundledBfdKeymaps()
{
    int installed = 0;

    for (const auto& map : bundledBfdKeymapFolder().findChildFiles (juce::File::findFiles, false, "*.bfd3map"))
    {
        const auto content = map.loadFileAsString();

        if (content.length() < 100)
            continue;

        for (const auto& folder : bfdKeymapFolders())
        {
            const auto target = folder.getChildFile (map.getFileName());

            if (target.existsAsFile() && target.loadFileAsString() == content)
                continue;

            if (map.copyFileTo (target))
                ++installed;
        }
    }

    return installed;
}

bool isBundledBfdKeymapInstalled()
{
    for (const auto& folder : bfdKeymapFolders())
        if (folder.getChildFile ("Yamaha DTX-PRO (DrumVstHost).bfd3map").existsAsFile())
            return true;

    return false;
}

juce::String drumPadName (DrumModule::Brand brand, int note)
{
    if (brand == DrumModule::Brand::yamaha)
    {
        // Yamaha DTX-PRO·DTX700·DTX900 기본 배치(BFD의 Yamaha 맵과 2026-11-18 신호 기록으로 확인)
        switch (note)
        {
            case 36: return "Kick";
            case 38: return "Snare";
            case 40: return K ("Snare 림");
            case 37: return K ("Snare 크로스스틱");
            case 48: return "Tom 1";
            case 14: case 15: return K ("Tom 1 림");
            case 47: return "Tom 2";
            case 18: case 19: return K ("Tom 2 림");
            case 43: return "Tom 3";
            case 20: case 23: return K ("Tom 3 림");
            case 41: return "Tom 4";
            case 24: case 25: return K ("Tom 4 림");
            case 42: case 46: return K ("Hi-hat 몸통");
            case 78: case 79: return K ("Hi-hat 엣지");
            case 44: return K ("Hi-hat 페달");
            case 83: case 95: return K ("Hi-hat 페달 스플래시");
            case 59: return K ("Crash 1 몸통");
            case 55: return K ("Crash 1 벨");
            case 49: return K ("Crash 1 엣지");
            case 17: return K ("Crash 2 몸통");
            case 16: return K ("Crash 2 벨");
            case 57: return K ("Crash 2 엣지");
            case 51: return K ("Ride 몸통");
            case 53: return K ("Ride 벨");
            case 52: return K ("Ride 엣지");
            default: break;
        }

        return {};
    }

    if (brand == DrumModule::Brand::roland)
    {
        // Roland V-Drums 기본 배치(TD-17·TD-27·TD-50X MIDI Implementation)
        switch (note)
        {
            case 36: return "Kick";
            case 38: return "Snare";
            case 40: return K ("Snare 림");
            case 37: return K ("Snare 크로스스틱");
            case 48: return "Tom 1";
            case 50: return K ("Tom 1 림");
            case 45: return "Tom 2";
            case 47: return K ("Tom 2 림");
            case 43: return "Tom 3";
            case 58: return K ("Tom 3 림");
            case 41: return "Tom 4";
            case 39: return K ("Tom 4 림");
            case 46: return K ("Hi-hat 열림 몸통");
            case 26: return K ("Hi-hat 열림 엣지");
            case 42: return K ("Hi-hat 닫힘 몸통");
            case 22: return K ("Hi-hat 닫힘 엣지");
            case 44: return K ("Hi-hat 페달");
            case 49: return K ("Crash 1 몸통");
            case 55: return K ("Crash 1 엣지");
            case 57: return K ("Crash 2 몸통");
            case 52: return K ("Crash 2 엣지");
            case 51: return K ("Ride 몸통");
            case 59: return K ("Ride 엣지");
            case 53: return K ("Ride 벨");
            default: break;
        }

        return note >= 27 && note <= 34 ? "AUX" : juce::String();
    }

    // General MIDI 드럼 이름(모듈·플러그인마다 실제 소리는 다를 수 있다)
    static const char* const names[] = { "Bass Drum 2", "Bass Drum 1", "Side Stick", "Snare", "Hand Clap", "Snare (Electric)",
                                         "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat",
                                         "Low-Mid Tom", "Hi-Mid Tom", "Crash 1", "High Tom", "Ride 1", "China", "Ride Bell",
                                         "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2" };
    return note >= 35 && note <= 59 ? "GM: " + juce::String (names[note - 35]) : juce::String();
}

juce::Array<int> tomPadNotes (DrumModule::Brand brand)
{
    // drumPadName의 탐 헤드·림과 같은 번호
    if (brand == DrumModule::Brand::yamaha)
        return { 48, 14, 15, 47, 18, 19, 43, 20, 23, 41, 24, 25 };

    if (brand == DrumModule::Brand::roland)
        return { 48, 50, 45, 47, 43, 58, 41, 39 };

    return { 41, 43, 45, 47, 48, 50 };   // General MIDI 탐
}

std::array<juce::uint8, 128> weakTomHitTable (DrumModule::Brand brand, int maxDroppedVelocity)
{
    std::array<juce::uint8, 128> table {};

    if (maxDroppedVelocity > 0)
        for (const int note : tomPadNotes (brand))
            table[(size_t) note] = (juce::uint8) juce::jlimit (1, 127, maxDroppedVelocity + 1);

    return table;
}
