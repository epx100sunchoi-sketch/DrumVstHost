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

juce::File songcache::rootFolder()
{
    return dataChild (appFolder(), "곡 분석", "Song analysis");
}

//==============================================================================
// '원곡 드럼 분리'의 분리 결과 재사용(SongTools.h 설명). 자리 찾기는 소리를 1/16로 줄여 거칠게 훑고 원래 크기로 다듬는다.
namespace
{
bool readWav44 (const juce::File& file, std::vector<float>& left, std::vector<float>& right, juce::String& why)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        why = "cannot read " + file.getFileName();
        return false;
    }

    if (std::abs (reader->sampleRate - 44100.0) > 0.5)
    {
        why = file.getFileName() + " is not 44.1kHz";
        return false;
    }

    const int n = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) 44100 * 60 * 30);
    juce::AudioBuffer<float> buffer (2, n);
    reader->read (&buffer, 0, n, 0, true, true);
    left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + n);
    right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + n);
    return true;
}

std::vector<float> decimated (const std::vector<float>& left, const std::vector<float>& right, int factor)
{
    std::vector<float> out;
    out.reserve (left.size() / (size_t) factor + 1);

    for (size_t i = 0; i + (size_t) factor <= left.size(); i += (size_t) factor)
    {
        float sum = 0.0f;

        for (int k = 0; k < factor; ++k)
            sum += left[i + (size_t) k] + right[i + (size_t) k];

        out.push_back (sum / (2.0f * (float) factor));
    }

    return out;
}
} // namespace

bool cutAlignedRestForTake (const juce::File& takeSong44, const juce::File& cacheFolder,
                            const juce::File& restOut, juce::String& detail)
{
    std::vector<float> takeL, takeR, origL, origR, restL, restR;

    if (! readWav44 (takeSong44, takeL, takeR, detail)
        || ! readWav44 (songcache::original (cacheFolder), origL, origR, detail)
        || ! readWav44 (songcache::rest (cacheFolder), restL, restR, detail))
        return false;

    const auto takeN = (juce::int64) takeL.size();
    const auto origN = (juce::int64) origL.size();

    if (takeN < 44100 * 2 || origN < 44100 * 2)
    {
        detail = "too short";
        return false;
    }

    // --- 자리 찾기(거칠게): 녹음에서 소리가 가장 큰 2초 창을 원곡 전체에서 찾는다.
    constexpr int factor = 16;   // 거친 탐색 비용(원곡 길이 × 창)을 줄인다. 다듬기에서 ±2칸을 다시 본다.
    const auto takeD = decimated (takeL, takeR, factor);
    const auto origD = decimated (origL, origR, factor);
    const int windowD = 2 * 44100 / factor;

    if ((int) takeD.size() < windowD + 1 || (int) origD.size() < windowD + 1)
    {
        detail = "too short";
        return false;
    }

    int windowStartD = 0;
    {
        double energy = 0.0, best = -1.0;

        for (int i = 0; i < windowD; ++i)
            energy += (double) takeD[(size_t) i] * takeD[(size_t) i];

        for (int s = 0; s + windowD < (int) takeD.size(); ++s)
        {
            if (energy > best)
            {
                best = energy;
                windowStartD = s;
            }

            energy += (double) takeD[(size_t) (s + windowD)] * takeD[(size_t) (s + windowD)]
                    - (double) takeD[(size_t) s] * takeD[(size_t) s];
        }

        if (best < 1.0e-6)
        {
            detail = "take is silent";
            return false;
        }
    }

    double windowEnergy = 0.0;

    for (int i = 0; i < windowD; ++i)
        windowEnergy += (double) takeD[(size_t) (windowStartD + i)] * takeD[(size_t) (windowStartD + i)];

    int bestOffsetD = 0;
    double bestScore = -1.0;

    for (int o = 0; o + windowD <= (int) origD.size(); ++o)
    {
        double dot = 0.0, originEnergy = 0.0;
        const float* a = takeD.data() + windowStartD;
        const float* b = origD.data() + o;

        for (int i = 0; i < windowD; ++i)
        {
            dot += (double) a[i] * b[i];
            originEnergy += (double) b[i] * b[i];
        }

        const double score = dot / std::sqrt (juce::jmax (1.0e-12, windowEnergy * originEnergy));

        if (score > bestScore)
        {
            bestScore = score;
            bestOffsetD = o - windowStartD;   // 원곡 위치 = 녹음 위치 + offset
        }
    }

    if (bestScore < 0.6)
    {
        detail = "no match (score " + juce::String (bestScore, 2) + ")";
        return false;
    }

    // --- 자리 다듬기(원래 크기): 거친 자리 주변 ±2칸에서 잔차가 가장 작은 자리
    juce::int64 offset = (juce::int64) bestOffsetD * factor;
    {
        const juce::int64 windowStart = (juce::int64) windowStartD * factor;
        const int window = 44100;
        double bestResidual = -1.0;
        juce::int64 bestFine = offset;

        for (juce::int64 o = offset - 2 * factor; o <= offset + 2 * factor; ++o)
        {
            double residual = 0.0;

            for (int i = 0; i < window; ++i)
            {
                const juce::int64 t = windowStart + i, s = t + o;
                const float orig = s >= 0 && s < origN ? origL[(size_t) s] : 0.0f;
                const double difference = (double) takeL[(size_t) t] - orig;
                residual += difference * difference;
            }

            if (bestResidual < 0.0 || residual < bestResidual)
            {
                bestResidual = residual;
                bestFine = o;
            }
        }

        offset = bestFine;
    }

    // --- 구간별 확인: 0.25초 단위로 녹음과 원곡(그 자리)의 잔차를 재서, 맞는 구간만 분리 결과를 쓴다.
    constexpr int block = 11025;
    const int blocks = (int) ((takeN + block - 1) / block);
    std::vector<char> useCache ((size_t) blocks, 0);
    float peak = 0.0f;

    for (const float v : takeL)
        peak = juce::jmax (peak, std::abs (v));

    const double quiet = (double) block * (double) peak * (double) peak * 1.0e-6;
    int loud = 0, loudMatched = 0;

    for (int b = 0; b < blocks; ++b)
    {
        const juce::int64 start = (juce::int64) b * block;
        const int n = (int) juce::jmin<juce::int64> (block, takeN - start);
        double takeEnergy = 0.0, origEnergy = 0.0, residual = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const juce::int64 t = start + i, s = t + offset;
            const double tv = takeL[(size_t) t];
            const double ov = s >= 0 && s < origN ? origL[(size_t) s] : 0.0;
            takeEnergy += tv * tv;
            origEnergy += ov * ov;
            residual += (tv - ov) * (tv - ov);
        }

        const bool bothQuiet = takeEnergy <= quiet && origEnergy <= quiet;
        const bool matched = bothQuiet || residual <= 0.02 * juce::jmax (takeEnergy, origEnergy);
        useCache[(size_t) b] = (char) (matched ? 1 : 0);

        if (takeEnergy > quiet)
        {
            ++loud;
            loudMatched += matched ? 1 : 0;
        }
    }

    detail = "offset " + juce::String (offset) + " smp (score " + juce::String (bestScore, 2) + "), matched "
           + juce::String (loudMatched) + "/" + juce::String (loud) + " loud blocks";

    if (loud < 8 || loudMatched < loud * 9 / 10)
        return false;

    // --- 쓰기: 맞는 구간은 분리 결과(드럼을 뺀 원곡), 아닌 구간은 녹음 그대로(= 원곡 드럼 0). 경계는 10ms 섞는다.
    const auto partial = restOut.getSiblingFile (restOut.getFileNameWithoutExtension() + ".part.wav");
    PartialFile guard { partial };
    auto writer = makeFloatWavWriter (partial, 44100.0);

    if (writer == nullptr)
    {
        detail = "cannot write " + restOut.getFileName();
        return false;
    }

    const auto restN = (juce::int64) restL.size();
    const auto sourceAt = [&] (juce::int64 t, bool cache, int channel)
    {
        if (! cache)
            return channel == 0 ? takeL[(size_t) t] : takeR[(size_t) t];

        const juce::int64 s = t + offset;

        if (s < 0 || s >= restN)
            return 0.0f;

        return channel == 0 ? restL[(size_t) s] : restR[(size_t) s];
    };

    juce::AudioBuffer<float> out (2, block);
    constexpr int fade = 441;

    for (int b = 0; b < blocks; ++b)
    {
        const juce::int64 start = (juce::int64) b * block;
        const int n = (int) juce::jmin<juce::int64> (block, takeN - start);
        const bool cache = useCache[(size_t) b] != 0;
        const bool previous = b > 0 ? useCache[(size_t) b - 1] != 0 : cache;

        for (int ch = 0; ch < 2; ++ch)
        {
            float* p = out.getWritePointer (ch);

            for (int i = 0; i < n; ++i)
            {
                const juce::int64 t = start + i;

                if (previous != cache && i < fade)
                {
                    const float mix = (float) i / (float) fade;
                    p[i] = sourceAt (t, previous, ch) * (1.0f - mix) + sourceAt (t, cache, ch) * mix;
                }
                else
                {
                    p[i] = sourceAt (t, cache, ch);
                }
            }
        }

        if (! writer->writeFromAudioSampleBuffer (out, 0, n))
        {
            detail = "cannot write " + restOut.getFileName();
            return false;
        }
    }

    writer.reset();

    if (! partial.moveFileTo (restOut))
    {
        detail = "cannot rename " + restOut.getFileName();
        return false;
    }

    guard.keep = true;
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
