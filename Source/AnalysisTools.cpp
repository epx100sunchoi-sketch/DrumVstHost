#include "AnalysisTools.h"
#include "Platform.h"

namespace analysis
{
namespace
{
// 설치 도구는 버전과 SHA-256을 고정해서 받는다(받은 파일이 다르면 쓰지 않는다).
constexpr const char* uvUrl = "https://github.com/astral-sh/uv/releases/download/0.13.9/uv-x86_64-pc-windows-msvc.zip";
constexpr const char* uvSha256 = "edd61f7f5728a56cc62455386661860bdfc7bab65872f39b0c25bdcc1d8e1beb";
constexpr const char* pythonVersion = "3.12";

juce::File exeFolder()      { return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(); }
juce::File appCandidate()   { return exeFolder().getChildFile ("analysis"); }
juce::File localCandidate() { return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("DrumVstHost").getChildFile ("analysis"); }

juce::File venvFolder (const juce::File& base)  { return base.getChildFile ("venv"); }
juce::File venvPython (const juce::File& base)  { return venvFolder (base).getChildFile ("Scripts").getChildFile ("python.exe"); }
juce::File modelFolder (const juce::File& base) { return base.getChildFile ("hf"); }
juce::File markerFile (const juce::File& base, Kind kind)
{
    return base.getChildFile (kind == Kind::beats ? "ready-beats.txt" : "ready-separation.txt");
}

bool readyIn (const juce::File& base, Kind kind)
{
    // 표시 파일에는 설치한 venv의 경로가 들어 있다. 폴더를 옮겼으면 venv가 예전 경로를 가리켜 쓸 수 없으므로 다시 설치한다.
    const auto marker = markerFile (base, kind);
    return venvPython (base).existsAsFile() && marker.existsAsFile()
        && marker.loadFileAsString().trim() == venvFolder (base).getFullPathName();
}

bool canWriteTo (const juce::File& folder)
{
    if (! folder.createDirectory())
        return false;

    const auto probe = folder.getNonexistentChildFile ("write-test", ".tmp", false);
    const bool ok = probe.replaceWithText ("ok");
    probe.deleteFile();
    return ok;
}

juce::String lastLineOf (const juce::String& text)
{
    auto lines = juce::StringArray::fromLines (text);
    lines.removeEmptyStrings();
    return lines.isEmpty() ? juce::String() : lines[lines.size() - 1].trim().substring (0, 160);
}

juce::String explainFailure (const juce::String& output)
{
    const auto lower = output.toLowerCase();

    if (lower.contains ("failed to fetch") || lower.contains ("dns error") || lower.contains ("error sending request")
        || lower.contains ("connection") || lower.contains ("timed out") || lower.contains ("network"))
        return K ("인터넷에 연결할 수 없습니다. 연결을 확인하고 다시 시도해 주세요.");

    if (lower.contains ("no space") || lower.contains ("os error 112") || lower.contains ("not enough space"))
        return K ("디스크 공간이 부족합니다. 드럼 빼기 도구까지 받으려면 약 1GB가 필요합니다.");

    return K ("설치하지 못했습니다: ") + lastLineOf (output);
}
} // namespace

//==============================================================================
juce::File installedFolder()
{
    for (const auto& candidate : { appCandidate(), localCandidate() })
        if (venvPython (candidate).existsAsFile())
            return candidate;

    return {};
}

juce::File folderForInstall()
{
    if (const auto installed = installedFolder(); installed != juce::File())
        return installed;

    // 압축을 푼 앱 폴더에 두면 앱과 함께 옮기거나 지우기 쉽다. 쓸 수 없는 곳(Program Files 등)이면 사용자 폴더에 둔다.
    return canWriteTo (appCandidate()) ? appCandidate() : localCandidate();
}

juce::File managedPython()
{
    const auto base = installedFolder();
    return base == juce::File() ? juce::File() : venvPython (base);
}

bool isManagedPython (const juce::File& python)
{
    const auto managed = managedPython();
    return managed != juce::File() && python == managed;
}

bool isReady (Kind kind)
{
    const auto base = installedFolder();
    return base != juce::File() && readyIn (base, kind);
}

//==============================================================================
Installer::Installer() : juce::Thread ("analysis tools installer") {}

Installer::~Installer()
{
    cancel();
}

void Installer::start (bool includeSeparation)
{
    if (isThreadRunning())
        return;

    withSeparation = includeSeparation;
    folder = folderForInstall();

    // uv가 받는 파이썬과 내려받은 꾸러미 보관함도 이 폴더 안에 둔다(시스템이나 다른 파이썬은 건드리지 않는다).
    // 이 앱이 띄우는 자식 프로세스에만 영향이 있고, 파이썬은 -I로 실행해 이 값들을 쓰지 않는다.
    setProcessEnvironmentVariable ("UV_PYTHON_INSTALL_DIR", folder.getChildFile ("python").getFullPathName());
    setProcessEnvironmentVariable ("UV_CACHE_DIR", folder.getChildFile ("cache").getFullPathName());
    setProcessEnvironmentVariable ("UV_PYTHON_DOWNLOADS", "automatic");
    setProcessEnvironmentVariable ("UV_NO_CONFIG", "1");
    setProcessEnvironmentVariable ("UV_NO_PROGRESS", "1");
    setProcessEnvironmentVariable ("NO_COLOR", "1");

    startThread();
}

void Installer::cancel()
{
    if (! isThreadRunning())
        return;

    signalThreadShouldExit();

    {
        const juce::ScopedLock lock (processLock);

        if (process != nullptr)
            process->kill();
    }

    stopThread (15000);
}

void Installer::report (const juce::String& stage, float progress)
{
    stageText = stage;
    juce::WeakReference<Installer> weak (this);

    juce::MessageManager::callAsync ([weak, stage, progress]
    {
        if (auto* installer = weak.get())
            if (installer->onProgress != nullptr)
                installer->onProgress (stage, juce::jlimit (0.0f, 1.0f, progress));
    });
}

void Installer::log (const juce::String& text)
{
    if (logStream != nullptr)
    {
        logStream->writeText (text + "\r\n", false, false, nullptr);
        logStream->flush();
    }
}

void Installer::run()
{
    if (! folder.createDirectory())
    {
        const auto error = K ("설치할 폴더를 만들 수 없습니다: ") + folder.getFullPathName();
        juce::WeakReference<Installer> weak (this);
        juce::MessageManager::callAsync ([weak, error] { if (auto* i = weak.get()) if (i->onFinished != nullptr) i->onFinished (false, error); });
        return;
    }

    const auto logFile = folder.getChildFile ("install-log.txt");
    logFile.deleteFile();
    logStream = std::make_unique<juce::FileOutputStream> (logFile);

    if (logStream->failedToOpen())
        logStream.reset();

    log ("DrumVstHost analysis tools install " + juce::Time::getCurrentTime().toString (true, true)
         + (withSeparation ? " (beats + separation)" : " (beats)") + " into " + folder.getFullPathName());

    juce::String error;
    const bool ok = installAll (error);

    if (threadShouldExit())
    {
        log ("cancelled");
        return;   // 취소한 쪽은 결과를 기다리지 않는다
    }

    log (ok ? juce::String ("done") : "failed: " + error);
    logStream.reset();

    juce::WeakReference<Installer> weak (this);
    juce::MessageManager::callAsync ([weak, ok, error]
    {
        if (auto* installer = weak.get())
            if (installer->onFinished != nullptr)
                installer->onFinished (ok, error);
    });
}

bool Installer::installAll (juce::String& error)
{
    const bool separation = withSeparation;

    // 전체 진행률에서 단계마다 차지하는 몫(드럼 분리 도구가 가장 크다)
    const float uvTo     = separation ? 0.04f : 0.15f;
    const float venvTo   = separation ? 0.10f : 0.45f;
    const float beatsTo  = separation ? 0.18f : 0.97f;
    const float torchTo  = 0.78f;
    const float modelTo  = 0.97f;

    const auto uvExe = folder.getChildFile ("uv").getChildFile ("uv.exe");
    const auto python = venvPython (folder);

    // 1. 설치 도구(uv)
    if (! uvExe.existsAsFile())
    {
        report (K ("설치 도구 받는 중…"), 0.0f);
        const auto zip = folder.getChildFile ("download").getChildFile ("uv.zip");

        if (! download (uvUrl, zip, 0.0f, uvTo, error))
            return false;

        if (juce::SHA256 (zip).toHexString() != uvSha256)
        {
            zip.deleteFile();
            error = K ("내려받은 설치 도구가 원래 파일과 다릅니다(손상되었거나 바뀌었습니다). 다시 시도해 주세요.");
            return false;
        }

        juce::ZipFile archive (zip);
        const int index = archive.getIndexOfFileName ("uv.exe");
        const auto result = index >= 0 ? archive.uncompressEntry (index, uvExe.getParentDirectory())
                                       : juce::Result::fail ("uv.exe not found in the archive");

        if (result.failed() || ! uvExe.existsAsFile())
        {
            error = K ("설치 도구의 압축을 풀지 못했습니다: ") + result.getErrorMessage();
            return false;
        }

        zip.getParentDirectory().deleteRecursively();
        log ("uv ready: " + uvExe.getFullPathName());
    }

    // 2. 파이썬 + 가상환경
    if (! python.existsAsFile())
    {
        report (K ("파이썬 받는 중…"), uvTo);

        if (! runProcess ({ uvExe.getFullPathName(), "venv", "--clear", "--managed-python", "--python", pythonVersion,
                            venvFolder (folder).getFullPathName() }, uvTo, venvTo, 15.0, error))
            return false;
    }

    // 3. 박자 찾기(beats.py)
    if (! readyIn (folder, Kind::beats))
    {
        report (K ("박자 분석 도구 받는 중…"), venvTo);

        if (! runProcess ({ uvExe.getFullPathName(), "pip", "install", "--python", python.getFullPathName(),
                            "numpy", "scipy", "soundfile" }, venvTo, beatsTo - 0.02f, 20.0, error))
            return false;

        if (! runProcess ({ python.getFullPathName(), "-I", "-c", "import numpy, scipy.signal, soundfile; print('beats ok')" },
                          beatsTo - 0.02f, beatsTo, 5.0, error))
            return false;

        markerFile (folder, Kind::beats).replaceWithText (venvFolder (folder).getFullPathName());
    }

    // 4. 드럼 빼기(separate.py): PyTorch(Windows의 PyPI 판은 CPU용) + Demucs + 분리 모델
    if (separation && ! readyIn (folder, Kind::separation))
    {
        report (K ("드럼 분리 도구 받는 중… (몇 분 걸릴 수 있습니다)"), beatsTo);

        if (! runProcess ({ uvExe.getFullPathName(), "pip", "install", "--python", python.getFullPathName(),
                            "torch", "demucs>=4.1" }, beatsTo, torchTo, 90.0, error))
            return false;

        report (K ("드럼 분리 모델 받는 중…"), torchTo);

        // 모델은 venv 옆 hf 폴더에 받는다(separate.py가 그 폴더를 찾아 쓴다). 앱은 평소에 HF_HUB_OFFLINE=1로 두므로 여기서만 푼다.
        const juce::String fetch = "import os, sys\n"
                                   "os.environ['HF_HOME'] = sys.argv[1]\n"
                                   "os.environ['HF_HUB_OFFLINE'] = sys.argv[2]\n"
                                   "from demucs.api import Separator\n"
                                   "Separator(model='hf://htdemucs', device='cpu', progress=False)\n"
                                   "print('model ok', sys.argv[2])\n";
        const auto hf = modelFolder (folder).getFullPathName();

        if (! runProcess ({ python.getFullPathName(), "-I", "-X", "utf8", "-u", "-c", fetch, hf, "0" }, torchTo, modelTo, 30.0, error))
            return false;

        // 인터넷 없이도 불러지는지 확인한다(실제로 분리할 때와 같은 조건).
        if (! runProcess ({ python.getFullPathName(), "-I", "-X", "utf8", "-u", "-c", fetch, hf, "1" }, modelTo, 0.99f, 15.0, error))
            return false;

        markerFile (folder, Kind::separation).replaceWithText (venvFolder (folder).getFullPathName());
    }

    report (K ("설치를 마쳤습니다"), 1.0f);
    return true;
}

bool Installer::download (const juce::String& url, const juce::File& destination, float from, float to, juce::String& error)
{
    log ("download " + url);
    destination.getParentDirectory().createDirectory();
    destination.deleteFile();

    auto stream = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                         .withConnectionTimeoutMs (20000)
                                                         .withNumRedirectsToFollow (10));

    if (stream == nullptr)
    {
        error = K ("인터넷에 연결할 수 없습니다. 연결을 확인하고 다시 시도해 주세요.");
        return false;
    }

    const auto total = stream->getTotalLength();
    juce::int64 done = 0;
    int lastPercent = -1;

    {
        juce::FileOutputStream out (destination);

        if (out.failedToOpen())
        {
            error = K ("파일을 만들 수 없습니다: ") + destination.getFullPathName();
            return false;
        }

        juce::HeapBlock<char> buffer (65536);

        for (;;)
        {
            if (threadShouldExit())
                return false;

            const int n = stream->read (buffer.get(), 65536);

            if (n <= 0)
                break;

            if (! out.write (buffer.get(), (size_t) n))
            {
                error = K ("디스크 공간이 부족합니다. 드럼 빼기 도구까지 받으려면 약 1GB가 필요합니다.");
                return false;
            }

            done += n;

            if (total > 0)
            {
                const int percent = (int) (done * 100 / total);

                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    report (stageText, from + (to - from) * (float) percent / 100.0f);
                }
            }
        }

        out.flush();
    }

    if (done <= 0 || (total > 0 && done != total))
    {
        error = K ("내려받다가 끊겼습니다. 인터넷 연결을 확인하고 다시 시도해 주세요.");
        return false;
    }

    log ("downloaded " + juce::String (done) + " bytes");
    return true;
}

bool Installer::runProcess (const juce::StringArray& command, float from, float to, double expectedSeconds, juce::String& error)
{
    log ("> " + command.joinIntoString (" ").replace ("\n", "; "));

    {
        const juce::ScopedLock lock (processLock);
        process = std::make_unique<juce::ChildProcess>();

        if (threadShouldExit() || ! process->start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            error = K ("프로그램을 실행하지 못했습니다: ") + juce::File (command[0]).getFileName();
            return false;
        }
    }

    std::string raw, line;
    int lastPercent = -1, lastShown = -1;
    const double startedMs = juce::Time::getMillisecondCounterHiRes();

    for (;;)
    {
        if (threadShouldExit())
        {
            const juce::ScopedLock lock (processLock);
            process->kill();
            return false;
        }

        // 진행률을 알려 주지 않는 단계(꾸러미 받기 등)는 걸린 시간에 따라 막대를 천천히 채운다(끝까지는 채우지 않는다).
        if (lastPercent < 0)
        {
            const double elapsed = (juce::Time::getMillisecondCounterHiRes() - startedMs) / 1000.0;
            const auto guess = from + (to - from) * 0.95f * (float) (1.0 - std::exp (-elapsed / juce::jmax (1.0, expectedSeconds)));

            if (const int shown = juce::roundToInt (guess * 100.0f); shown != lastShown)
            {
                lastShown = shown;
                report (stageText, guess);
            }
        }

        char c = 0;

        if (process->readProcessOutput (&c, 1) <= 0)
        {
            if (! process->isRunning())
                break;

            wait (30);
            continue;
        }

        raw.push_back (c);

        if (raw.size() > 65536)
            raw.erase (0, raw.size() - 32768);

        if (c == '\n' || c == '\r')
        {
            if (! line.empty())
                log ("  " + juce::String::fromUTF8 (line.data(), (int) line.size()));

            line.clear();
            continue;
        }

        line.push_back (c);

        // 진행 막대 "  45%|████" (모델 받기 등)
        if (c == '|' && line.size() >= 3 && line[line.size() - 2] == '%')
        {
            int percent = 0, scale = 1;

            for (size_t i = line.size() - 2; i > 0 && std::isdigit ((unsigned char) line[i - 1]) && scale <= 100; --i, scale *= 10)
                percent += (line[i - 1] - '0') * scale;

            if (percent != lastPercent && scale > 1)
            {
                lastPercent = percent;
                report (stageText, from + (to - from) * (float) juce::jlimit (0, 100, percent) / 100.0f);
            }
        }
    }

    if (! line.empty())
        log ("  " + juce::String::fromUTF8 (line.data(), (int) line.size()));

    // 그만두기로 끈 프로세스는 종료 코드가 0으로 남을 수 있으므로, 성공으로 보지 않는다.
    if (threadShouldExit())
        return false;

    const auto exitCode = process->getExitCode();
    const auto output = juce::String::fromUTF8 (raw.data(), (int) raw.size());
    log ("  exit " + juce::String ((int) exitCode));

    if (exitCode != 0)
    {
        error = explainFailure (output);
        return false;
    }

    report (stageText, to);
    return true;
}
} // namespace analysis
