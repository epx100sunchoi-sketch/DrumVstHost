#include "SongTools.h"
#include "AnalysisTools.h"

namespace
{
juce::File appFolder()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
}

std::unique_ptr<juce::AudioFormatWriter> makeFloatWavWriter (const juce::File& file, double rate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

    if (stream == nullptr)
        return {};

    juce::WavAudioFormat wav;
    return wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                            .withSampleRate (rate)
                                            .withNumChannels (2)
                                            .withBitsPerSample (32)
                                            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
}

// 쓰다 만 파일: 일이 끝나기 전에 함수를 떠나면(오류) 지운다. 쓰는 객체(writer)보다 먼저 만들어야
// 쓰는 객체가 닫힌 뒤에 지울 수 있다.
struct PartialFile
{
    juce::File file;
    bool keep = false;
    ~PartialFile() { if (! keep) file.deleteFile(); }
};
} // namespace

//==============================================================================
juce::File songcache::folderFor (const juce::File& song)
{
    const auto key = song.getFullPathName() + "|" + juce::String (song.getSize()) + "|"
                   + juce::String (song.getLastModificationTime().toMilliseconds());
    const auto hash = juce::String::toHexString (key.hashCode64()).paddedLeft ('0', 16).substring (0, 10);
    auto name = juce::File::createLegalFileName (song.getFileNameWithoutExtension()).substring (0, 40).trim();

    if (name.isEmpty())
        name = "song";

    return dataChild (appFolder(), "곡 분석", "Song analysis").getChildFile (name + " " + hash);
}

bool songcache::hasStems (const juce::File& folder)
{
    return rest (folder).getSize() > 1000 && drums (folder).getSize() > 1000;
}

juce::File findAnalysisPython (const juce::String& configuredPath)
{
    juce::Array<juce::File> candidates;

    if (configuredPath.isNotEmpty())
        candidates.add (juce::File (configuredPath));

    // 앱이 직접 받아 설치한 도구(AnalysisTools.h)를 먼저 쓴다.
    if (const auto managed = analysis::managedPython(); managed != juce::File())
        candidates.add (managed);

    const auto app = appFolder();
    candidates.add (app.getChildFile (".venv\\Scripts\\python.exe"));
    candidates.add (app.getChildFile ("python\\.venv\\Scripts\\python.exe"));
    candidates.add (app.getParentDirectory().getChildFile ("drum-cover-score\\.venv\\Scripts\\python.exe"));

    for (const auto& candidate : candidates)
        if (candidate.existsAsFile())
            return candidate;

    return {};
}

juce::File toolsFolder()
{
    return appFolder().getChildFile ("tools");
}

//==============================================================================
bool writeAnalysisCopy (const juce::File& song, const juce::File& destination, juce::String& error)
{
    if (destination.getSize() > 1000)
        return true;

    return writeResampledCopy (song, destination, 44100.0, error);
}

bool writeResampledCopy (const juce::File& song, const juce::File& destination, double targetRate, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (song));

    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        error = K ("원곡을 읽을 수 없습니다.");
        return false;
    }

    const double ratio = reader->sampleRate / targetRate;   // 출력 한 샘플마다 쓰는 입력 샘플 수

    destination.getParentDirectory().createDirectory();

    // 같은 곡을 곧바로 다시 열어 두 번 만들게 돼도 서로 덮어쓰지 않게 임시 이름을 따로 쓴다.
    const auto partial = destination.getSiblingFile (destination.getFileNameWithoutExtension() + ".part-"
                                                     + juce::Uuid().toString().substring (0, 8) + ".wav");
    PartialFile guard { partial };   // 오류로 떠나면 쓰다 만 파일을 지운다
    auto writer = makeFloatWavWriter (partial, targetRate);

    if (writer == nullptr)
    {
        error = K ("분석용 파일을 만들 수 없습니다: ") + partial.getFullPathName();
        return false;
    }

    constexpr int block = 65536;

    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        juce::AudioBuffer<float> buffer (2, block);

        for (juce::int64 pos = 0; pos < reader->lengthInSamples; pos += block)
        {
            const int n = (int) juce::jmin<juce::int64> (block, reader->lengthInSamples - pos);
            reader->read (&buffer, 0, n, pos, true, true);

            if (! writer->writeFromAudioSampleBuffer (buffer, 0, n))
            {
                error = K ("분석용 파일을 쓰지 못했습니다. 디스크 공간을 확인해 주세요.");
                return false;
            }
        }
    }
    else
    {
        // 샘플 속도를 44.1kHz로 바꾼다. 보간기의 고정 지연만큼 앞을 버려 시각을 그대로 맞춘다.
        juce::WindowedSincInterpolator interpolators[2];
        const juce::int64 outLength = (juce::int64) std::ceil ((double) reader->lengthInSamples / ratio);
        int skipLeft = juce::roundToInt (juce::WindowedSincInterpolator::getBaseLatency() / ratio);

        juce::AudioBuffer<float> pending (2, block * 2);
        juce::AudioBuffer<float> out (2, block);
        int pendingCount = 0;
        juce::int64 inputPos = 0, written = 0;

        while (written < outLength)
        {
            const int space = pending.getNumSamples() - pendingCount;

            if (space > 0)
            {
                reader->read (&pending, pendingCount, space, inputPos, true, true);   // 끝을 넘으면 0으로 채워진다
                inputPos += space;
                pendingCount += space;
            }

            const int produce = juce::jlimit (0, block, (int) ((pendingCount - 4) / ratio));

            if (produce <= 0)
                break;

            int used = 0;

            for (int ch = 0; ch < 2; ++ch)
                used = interpolators[ch].process (ratio, pending.getReadPointer (ch), out.getWritePointer (ch), produce);

            for (int ch = 0; ch < 2; ++ch)
                std::memmove (pending.getWritePointer (ch), pending.getReadPointer (ch) + used, sizeof (float) * (size_t) (pendingCount - used));

            pendingCount -= used;

            const int start = juce::jmin (skipLeft, produce);
            skipLeft -= start;
            const int toWrite = (int) juce::jmin<juce::int64> (produce - start, outLength - written);

            if (toWrite > 0)
            {
                if (! writer->writeFromAudioSampleBuffer (out, start, toWrite))
                {
                    error = K ("분석용 파일을 쓰지 못했습니다. 디스크 공간을 확인해 주세요.");
                    return false;
                }

                written += toWrite;
            }
        }
    }

    writer.reset();

    if (! partial.moveFileTo (destination))
    {
        partial.deleteFile();

        if (destination.getSize() > 1000)   // 다른 쪽이 먼저 만들었다
            return true;

        error = K ("분석용 파일 이름을 바꾸지 못했습니다.");
        return false;
    }

    return true;
}

bool makeExactDrumStem (const juce::File& original, const juce::File& rest, const juce::File& destination, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> originalReader (formats.createReaderFor (original));
    std::unique_ptr<juce::AudioFormatReader> restReader (formats.createReaderFor (rest));

    if (originalReader == nullptr || restReader == nullptr)
    {
        error = K ("분리된 파일을 읽을 수 없습니다.");
        return false;
    }

    if (std::abs (originalReader->sampleRate - restReader->sampleRate) > 0.5)
    {
        error = K ("분리된 파일의 샘플 속도가 원곡과 다릅니다.");
        return false;
    }

    const auto partial = destination.getSiblingFile (destination.getFileNameWithoutExtension() + ".part.wav");
    PartialFile guard { partial };   // 오류로 떠나면 쓰다 만 파일을 지운다
    auto writer = makeFloatWavWriter (partial, restReader->sampleRate);

    if (writer == nullptr)
    {
        error = K ("드럼 파일을 만들 수 없습니다.");
        return false;
    }

    constexpr int block = 65536;
    juce::AudioBuffer<float> a (2, block), b (2, block);
    const juce::int64 length = restReader->lengthInSamples;

    for (juce::int64 pos = 0; pos < length; pos += block)
    {
        const int n = (int) juce::jmin<juce::int64> (block, length - pos);
        originalReader->read (&a, 0, n, pos, true, true);
        restReader->read (&b, 0, n, pos, true, true);

        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::subtract (a.getWritePointer (ch), b.getReadPointer (ch), n);

        if (! writer->writeFromAudioSampleBuffer (a, 0, n))
        {
            error = K ("드럼 파일을 쓰지 못했습니다. 디스크 공간을 확인해 주세요.");
            return false;
        }
    }

    writer.reset();

    if (! partial.moveFileTo (destination))
    {
        error = K ("드럼 파일 이름을 바꾸지 못했습니다.");
        return false;
    }

    return true;
}

//==============================================================================
bool BeatMap::loadJson (const juce::File& file)
{
    const auto json = juce::JSON::parse (file);

    if (! json.isObject())
        return false;

    bpm = (double) json.getProperty ("bpm", 0.0);
    method = json.getProperty ("method", "").toString();
    beats.clear();
    downbeat.clear();

    if (const auto* list = json.getProperty ("beats", {}).getArray())
        for (const auto& v : *list)
            beats.push_back ((double) v);

    std::sort (beats.begin(), beats.end());
    downbeat.assign (beats.size(), 0);

    // 마디 첫 박은 가장 가까운 박에 표시한다(30ms 안).
    if (const auto* list = json.getProperty ("downbeats", {}).getArray())
    {
        for (const auto& v : *list)
        {
            const double t = (double) v;
            const auto it = std::lower_bound (beats.begin(), beats.end(), t);
            size_t best = beats.size();
            double bestDistance = 0.03;

            for (auto candidate : { it, it == beats.begin() ? it : it - 1 })
            {
                if (candidate != beats.end() && std::abs (*candidate - t) <= bestDistance)
                {
                    bestDistance = std::abs (*candidate - t);
                    best = (size_t) (candidate - beats.begin());
                }
            }

            if (best < beats.size())
                downbeat[best] = 1;
        }
    }

    return isValid();
}

//==============================================================================
ProcessJob::~ProcessJob()
{
    cancel();
}

bool ProcessJob::start (const juce::StringArray& command)
{
    cancel();
    cancelled = false;

    if (! process.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return false;

    startThread();
    return true;
}

void ProcessJob::cancel()
{
    if (isThreadRunning())
    {
        cancelled = true;
        process.kill();
        stopThread (8000);
    }
}

void ProcessJob::run()
{
    juce::WeakReference<ProcessJob> weak (this);
    std::string raw;   // 출력의 끝부분(바이트 그대로; 끝날 때 한 번에 글자로 바꾼다)
    int lastPercent = -1;

    for (;;)
    {
        // 한 바이트씩 읽는다: JUCE는 요청한 바이트가 다 찰 때까지 기다리므로, 크게 읽으면 짧은 진행률 줄이 늦게 보인다.
        char c = 0;
        const int n = process.readProcessOutput (&c, 1);

        if (n <= 0)
        {
            if (! process.isRunning() || cancelled.load())
                break;

            wait (30);
            continue;
        }

        raw.push_back (c);

        if (raw.size() > 65536)   // 앞부분은 버린다(줄 경계에서 잘라 글자가 깨지지 않게)
        {
            const auto cut = raw.find ('\n', raw.size() - 16384);
            raw.erase (0, cut == std::string::npos ? raw.size() - 16384 : cut + 1);
        }

        // 진행 막대 "  45%|████" 에서 '|' 바로 앞의 숫자를 읽는다(바이트 그대로: 막대 글자는 UTF-8이라 건드리지 않는다).
        if (c != '|' || raw.size() < 3 || raw[raw.size() - 2] != '%')
            continue;

        int percent = 0, digits = 0;

        for (size_t i = raw.size() - 2, scale = 1; i > 0 && digits < 3 && std::isdigit ((unsigned char) raw[i - 1]); --i, scale *= 10, ++digits)
            percent += (raw[i - 1] - '0') * (int) scale;

        if (digits == 0)
            continue;

        percent = juce::jlimit (0, 100, percent);

        if (percent != lastPercent)
        {
            lastPercent = percent;
            juce::MessageManager::callAsync ([weak, percent]
            {
                if (auto* job = weak.get())
                    if (job->onProgress != nullptr)
                        job->onProgress ((float) percent / 100.0f);
            });
        }
    }

    if (cancelled.load())
        return;   // 취소한 쪽은 결과를 기다리지 않는다

    const auto tail = juce::String::fromUTF8 (raw.data(), (int) raw.size()).getLastCharacters (8000);
    const bool ok = process.getExitCode() == 0;

    juce::MessageManager::callAsync ([weak, ok, tail]
    {
        if (auto* job = weak.get())
            if (job->onFinished != nullptr)
                job->onFinished (ok, tail);
    });
}
