#pragma once

#include "Common.h"

//==============================================================================
// 전자드럼 모듈(Yamaha DTX, Roland V-Drums 등)을 MIDI 입력·ASIO 드라이버 이름으로 알아보고,
// 모듈마다 다른 안내(소리 장치, 버퍼 바꾸는 곳, 드럼 플러그인에서 고를 MIDI 맵)를 만든다.
// 앱은 MIDI 음을 바꾸지 않고 그대로 플러그인에 넘긴다. 어떤 패드가 어떤 소리를 낼지는 플러그인의 MIDI 맵이 정한다.
//
// 근거(2026-11-18 조사, Roland 매뉴얼·MIDI Implementation, 이 PC의 SSD5·BFD Player 맵 파일):
//  - Roland는 Windows MIDI 이름이 모델명("TD-17", "5- TD-27")이고 USB 제조사 번호가 0582다(Yamaha 0499).
//  - Roland TD-02/07/17/27/50/50X는 공장 설정이 USB Driver Mode = GENERIC(MIDI만)이다. VENDOR로 바꾸고 껐다 켜면
//    Roland 드라이버(ASIO)로 모듈 헤드폰에서 PC 소리를 듣는다. TD-1은 MIDI만 된다.
//  - 노트 배치·하이햇 페달(CC4)·초크(폴리 애프터터치)는 그대로 넘기면 된다. 클록·킷 바꿈 신호는 엔진이 거른다.
struct DrumModule
{
    enum class Brand { none, yamaha, roland, alesis, efnote };

    Brand brand = Brand::none;
    juce::String model;        // 화면에 보일 모델(예: "TD-17"). 이름에서 알 수 없으면 비어 있다.
    juce::String midiInput;    // 알아본 MIDI 입력 이름(예: "DTX Drums-1", "TD-17")
    bool usbAudio = true;      // 모듈 USB로 PC 소리를 모듈 헤드폰에서 들을 수 있는지(모르면 true)

    bool isKnown() const noexcept       { return brand != Brand::none; }
    juce::String brandName() const;
    juce::String displayName() const;   // "Roland TD-17", "Yamaha DTX"
};

// 연결된 MIDI 입력들 가운데 전자드럼 모듈을 찾는다(이름, 없으면 장치 경로의 USB 제조사 번호). 없으면 brand == none.
DrumModule identifyDrumModule (const juce::Array<juce::MidiDeviceInfo>& midiInputs);

// 이름에서 Roland 모델(TD-17, TD-50X, V71 등)을 찾는다. 없으면 빈 문자열.
juce::String findRolandModel (const juce::String& text);

// 장치(드라이버) 이름으로 어느 회사 것인지
DrumModule::Brand driverBrand (const juce::String& deviceName);

// ASIO 드라이버 이름이 전자드럼 모듈의 드라이버로 보이면 양수. 연결된 모듈과 같은 회사·모델이면 더 크다.
int drumDriverScore (const juce::String& asioDriverName, const DrumModule& connected);

// 지금 쓰는 소리 장치에서 버퍼(지연)를 줄이는 방법. 짧은 것은 화면 한 줄, 긴 것은 말풍선용.
juce::String bufferAdviceShort (const juce::String& deviceType, const juce::String& deviceName);
juce::String bufferAdviceLong (const juce::String& deviceType, const juce::String& deviceName);

// 연결 상태에 대한 짧은 안내(예: Roland가 MIDI만 되는 모드라 모듈 헤드폰으로 못 듣는 경우). 할 말이 없으면 빈 문자열.
juce::String moduleConnectionAdvice (const DrumModule&, bool moduleDriverInstalled, bool moduleDriverOpen);
