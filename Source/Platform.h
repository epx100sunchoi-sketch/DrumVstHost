#pragma once

#include <JuceHeader.h>

// JUCE를 거치지 않는 Windows 전용 보조 함수(Platform.cpp).

// Windows 제목 표시줄을 어두운 색으로 바꾼다.
void setDarkTitleBar (void* nativeWindowHandle);

// 이 프로세스(와 여기서 띄우는 자식 프로세스)의 환경 변수를 바꾼다.
void setProcessEnvironmentVariable (const juce::String& name, const juce::String& value);

// 확인용: milliseconds 동안 Windows 메시지를 직접 돌린다(불러오는 중에 메시지를 처리하는 플러그인 흉내).
void pumpMessagesFor (int milliseconds);

// 앱이 비정상 종료되면 그 순간의 호출 경로를 logFile에 남긴다(같은 폴더의 .pdb가 있으면 함수 이름·줄 번호까지).
// 이 앱 코드 안의 메모리 접근 오류와 힙 손상만 기록한다(플러그인이 스스로 처리하는 예외는 건드리지 않는다).
void installCrashReporter (const juce::File& logFile);
