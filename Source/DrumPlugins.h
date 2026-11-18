#pragma once

#include "DrumModules.h"

//==============================================================================
// 설치된 VST3 가운데 드럼 악기를 알아본다(플러그인을 띄우지 않고 이름·정보 파일만 본다).
//  1) 잘 알려진 드럼 VST 이름(Toontrack EZdrummer·Superior Drummer, XLN Addictive Drums, Steven Slate Drums,
//     BFD, MT Power Drum Kit, Steinberg Groove Agent, NI Battery 등)
//  2) VST3 묶음 안의 Contents\moduleinfo.json에 'Drum' 분류가 있는 것
// 그리고 드럼 VST마다 모듈(Yamaha·Roland)에 맞는 MIDI 맵 고르는 방법을 알려 준다.
struct DrumPluginInfo
{
    enum class Family { none, superiorDrummer, ezdrummer, addictiveDrums, ssd, bfd, other };

    bool isDrum = false;
    Family family = Family::none;
    juce::String displayName;   // 목록에 보일 이름(예: "EZdrummer 3 (Toontrack)")
    int rank = 1000;            // 저절로 불러올 때의 순서(작을수록 먼저)
};

// 플러그인 파일(.vst3)을 보고 드럼 VST인지 알아본다.
DrumPluginInfo recogniseDrumPlugin (const juce::File& pluginFile);

// 플러그인 이름(엔진이 불러온 뒤 알려 주는 이름)이나 파일 이름으로 종류만 알아본다.
DrumPluginInfo::Family drumPluginFamily (const juce::String& nameOrPath);

// 연결된 모듈과 플러그인에 맞는 MIDI(e-drum) 맵 고르는 방법(말풍선용, 여러 줄). 모르면 일반 안내, 모듈을 모르면 빈 문자열.
juce::String drumMapAdvice (const DrumModule&, const juce::String& pluginNameOrPath);

// 앱과 함께 두는 BFD 키맵(tools\bfd\*.bfd3map)을 BFD 키맵 폴더(C:\ProgramData\BFD Drums\<제품>\System\Maps)에 설치한다.
// 없거나 내용이 다를 때만 쓴다. 설치한 파일 수를 돌려준다.
//  - "Yamaha DTX-PRO (DrumVstHost)": BFD의 DTX900 맵(3존 심벌·탐 림) + DTX502의 하이햇 42·79 + DTX700의 26.
//    DTX-PRO는 Crash 2 몸통을 17, 벨을 16, 엣지를 57로 보내는데(2026-11-18 신호 기록), DTX502 맵에는 57만 있어
//    몸통·벨을 치면 BFD가 소리를 내지 않았다.
int installBundledBfdKeymaps();
bool isBundledBfdKeymapInstalled();

// '신호 보기'에 보일 패드 이름: 모듈 회사의 기본 음 배치 기준(Yamaha DTX·Roland V-Drums), 모르면 General MIDI 이름.
juce::String drumPadName (DrumModule::Brand, int note);
