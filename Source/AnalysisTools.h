#pragma once

#include "Common.h"

//==============================================================================
// 메트로놈 박자 찾기(tools\beats.py)와 원곡 드럼 빼기(tools\separate.py)에 쓰는 파이썬 도구를 앱이 직접 받아 설치한다.
// 사용자가 파이썬을 따로 설치할 필요가 없다.
//   <폴더>\uv\uv.exe        설치 도구(Astral uv, 버전과 SHA-256을 고정해 GitHub에서 받는다)
//   <폴더>\python\          uv가 받는 파이썬 3.12(이 폴더 안에만 둔다)
//   <폴더>\venv\            분석용 가상환경. 박자: numpy·scipy·soundfile, 드럼 빼기: + torch(CPU)·demucs
//   <폴더>\hf\              드럼 분리 모델(htdemucs). separate.py가 venv 옆의 hf를 찾아 쓴다.
//   <폴더>\ready-*.txt      설치가 끝났다는 표시(안에 venv 경로. 폴더를 옮기면 다시 설치해야 한다)
//   <폴더>\install-log.txt  마지막 설치 기록(문제가 생기면 이 파일을 보면 된다)
// <폴더>는 앱 폴더의 analysis(쓸 수 있으면), 아니면 %LOCALAPPDATA%\DrumVstHost\analysis.
namespace analysis
{
enum class Kind { beats, separation };

juce::File installedFolder();          // 설치된 곳(파이썬이 있는 곳). 없으면 빈 File
juce::File folderForInstall();         // 새로 설치할 곳
juce::File managedPython();            // 설치된 venv의 python.exe(없으면 빈 File)
bool isManagedPython (const juce::File& python);
bool isReady (Kind);                   // 그 기능에 쓸 것이 다 설치됐는지

//==============================================================================
// 뒤에서 내려받아 설치한다. 진행·결과는 메시지 스레드로 알려 준다(취소하면 결과를 알리지 않는다).
class Installer final : private juce::Thread
{
public:
    Installer();
    ~Installer() override;

    std::function<void (const juce::String& stage, float progress)> onProgress;   // progress 0..1
    std::function<void (bool ok, const juce::String& error)> onFinished;

    void start (bool includeSeparation);
    void cancel();
    bool isRunning() const                   { return isThreadRunning(); }
    bool includesSeparation() const noexcept { return withSeparation; }

private:
    void run() override;
    bool installAll (juce::String& error);
    bool download (const juce::String& url, const juce::File& destination, float from, float to, juce::String& error);
    // expectedSeconds: 진행률을 알려 주지 않는 단계에서 막대를 천천히 채울 때 쓰는 대략의 시간
    bool runProcess (const juce::StringArray& command, float from, float to, double expectedSeconds, juce::String& error);
    void report (const juce::String& stage, float progress);
    void log (const juce::String& text);

    juce::File folder;
    bool withSeparation = false;
    juce::String stageText;
    juce::CriticalSection processLock;
    std::unique_ptr<juce::ChildProcess> process;   // 지금 돌고 있는 것(취소할 때 끈다)
    std::unique_ptr<juce::FileOutputStream> logStream;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Installer)
    JUCE_DECLARE_NON_COPYABLE (Installer)
};
} // namespace analysis
