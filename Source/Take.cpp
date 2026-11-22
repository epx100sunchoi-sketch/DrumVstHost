#include "Take.h"
#include "Dsp.h"
#include "Mp3Writer.h"

namespace
{
constexpr int peakBins = 3000;

juce::int64 readInt64 (const juce::XmlElement& xml, const char* name, juce::int64 fallback)
{
    return xml.hasAttribute (name) ? xml.getStringAttribute (name).getLargeIntValue() : fallback;
}

// source의 [sourceStart, sourceStart + numSamples) 구간을 배율을 곱해 더한다. 범위 밖은 무음.
// startGain과 endGain이 다르면 블록 안에서 고르게 바꾼다. 배율이 음수면 그만큼 뺀다(원곡 드럼 빼기).
void addSource (const juce::AudioBuffer<float>& source, juce::int64 sourceStart, int numSamples,
                float* left, float* right, float startGain, float endGain) noexcept
{
    if (startGain == 0.0f && endGain == 0.0f)
        return;

    if (source.getNumSamples() <= 0)
        return;

    const juce::int64 sourceLength = source.getNumSamples();
    const juce::int64 from = juce::jmax<juce::int64> (0, -sourceStart);
    const juce::int64 to = juce::jmin<juce::int64> (numSamples, sourceLength - sourceStart);

    if (to <= from)
        return;

    const float* a = source.getReadPointer (0) + sourceStart;
    const float* b = source.getReadPointer (source.getNumChannels() > 1 ? 1 : 0) + sourceStart;

    if (startGain == endGain)
    {
        const int count = (int) (to - from);
        juce::FloatVectorOperations::addWithMultiply (left + from, a + from, startGain, count);
        juce::FloatVectorOperations::addWithMultiply (right + from, b + from, startGain, count);
        return;
    }

    const float step = (endGain - startGain) / (float) juce::jmax (1, numSamples);

    for (juce::int64 i = from; i < to; ++i)
    {
        const float g = startGain + step * (float) i;
        left[i] += a[i] * g;
        right[i] += b[i] * g;
    }
}

// data의 소수 위치 pos 값을 4점 3차(Catmull-Rom) 보간으로 읽는다. 범위 밖은 무음. 정수 위치면 그 샘플 그대로다.
inline float readAt (const float* data, juce::int64 length, double pos) noexcept
{
    const auto i = (juce::int64) std::floor (pos);
    const float f = (float) (pos - (double) i);
    const auto at = [data, length] (juce::int64 k) { return k >= 0 && k < length ? data[k] : 0.0f; };
    const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

std::vector<float> computePeaks (const juce::AudioBuffer<float>& buffer, juce::int64 length)
{
    std::vector<float> peaks ((size_t) peakBins, 0.0f);
    const int available = buffer.getNumSamples();

    if (length <= 0)
        return peaks;

    for (int bin = 0; bin < peakBins; ++bin)
    {
        const int from = (int) juce::jmin<juce::int64> (available, length * bin / peakBins);
        const int to = (int) juce::jmin<juce::int64> (available, length * (bin + 1) / peakBins);
        float peak = 0.0f;

        if (to > from)
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                peak = juce::jmax (peak, buffer.getMagnitude (ch, from, to - from));

        peaks[(size_t) bin] = peak;
    }

    return peaks;
}
} // namespace

//==============================================================================
bool TakeInfo::load (const juce::File& folder)
{
    const auto xml = juce::XmlDocument::parse (TakeFiles::infoIn (folder));

    if (xml == nullptr || ! xml->hasTagName ("TAKE"))
        return false;

    sampleRate    = xml->getDoubleAttribute ("sampleRate", 0.0);
    lengthSamples = readInt64 (*xml, "lengthSamples", 0);
    drumVolume    = (float) xml->getDoubleAttribute ("drumVolume", 1.0);
    songVolume    = (float) xml->getDoubleAttribute ("songVolume", 0.7);
    latencyMs     = (float) xml->getDoubleAttribute ("latencyMs", -1.0);
    songName      = xml->getStringAttribute ("song");
    recordedAt    = juce::Time (readInt64 (*xml, "recordedAt", 0));

    if (const auto* edit = xml->getChildByName ("EDIT"))
    {
        edited    = true;
        editDrum  = (float) edit->getDoubleAttribute ("drum", drumVolume);
        editSong  = (float) edit->getDoubleAttribute ("song", songVolume);
        editSongDrums = (float) juce::jlimit (0.0, 1.0, edit->getDoubleAttribute ("songDrums", 1.0));
        offsetMs  = edit->getIntAttribute ("offsetMs", 0);
        trimStart = readInt64 (*edit, "trimStart", 0);
        trimEnd   = readInt64 (*edit, "trimEnd", 0);
    }

    return true;
}

bool TakeInfo::save (const juce::File& folder) const
{
    juce::XmlElement xml ("TAKE");
    xml.setAttribute ("sampleRate", sampleRate);
    xml.setAttribute ("lengthSamples", juce::String (lengthSamples));
    xml.setAttribute ("drumVolume", (double) drumVolume);
    xml.setAttribute ("songVolume", (double) songVolume);
    xml.setAttribute ("latencyMs", (double) latencyMs);
    xml.setAttribute ("song", songName);
    xml.setAttribute ("recordedAt", juce::String (recordedAt.toMilliseconds()));

    if (edited)
    {
        auto* edit = xml.createNewChildElement ("EDIT");
        edit->setAttribute ("drum", (double) editDrum);
        edit->setAttribute ("song", (double) editSong);
        edit->setAttribute ("songDrums", (double) editSongDrums);
        edit->setAttribute ("offsetMs", offsetMs);
        edit->setAttribute ("trimStart", juce::String (trimStart));
        edit->setAttribute ("trimEnd", juce::String (trimEnd));
    }

    return xml.writeTo (TakeFiles::infoIn (folder));
}

bool isTakeFolder (const juce::File& folder)
{
    return TakeFiles::drumIn (folder).existsAsFile()
        && TakeFiles::songIn (folder).existsAsFile();
}

juce::Array<juce::File> findRecentTakes (const juce::File& recordFolder, int maxCount)
{
    juce::Array<juce::File> takes;

    if (! recordFolder.isDirectory())
        return takes;

    for (const auto& entry : juce::RangedDirectoryIterator (recordFolder, false, "*", juce::File::findDirectories))
        if (isTakeFolder (entry.getFile()))
            takes.add (entry.getFile());

    // 폴더 이름에 날짜·시각이 들어 있어서 이름순이 곧 시간순이다.
    std::sort (takes.begin(), takes.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compare (b.getFileName()) > 0;
    });

    if (takes.size() > maxCount)
        takes.removeRange (maxCount, takes.size() - maxCount);

    return takes;
}

//==============================================================================
bool TakePlayer::readStereo (const juce::File& file, juce::AudioBuffer<float>& buffer, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max() / 2)
        return false;

    buffer.setSize (2, (int) reader->lengthInSamples);
    buffer.clear();

    if (reader->lengthInSamples > 0)
        reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);

    sampleRate = reader->sampleRate > 0.0 ? reader->sampleRate : 44100.0;
    return true;
}

std::shared_ptr<TakePlayer> TakePlayer::load (const juce::File& folder, juce::String& error)
{
    std::shared_ptr<TakePlayer> take (new TakePlayer());
    take->folder = folder;
    take->info.load (folder);   // 없어도 소리 파일만 있으면 된다

    double drumRate = 0.0, songRate = 0.0;

    if (! readStereo (TakeFiles::drumIn (folder), take->drum, drumRate)
        || ! readStereo (TakeFiles::songIn (folder), take->song, songRate))
    {
        error = K ("녹음 파일(드럼.wav, 원곡.wav)을 읽을 수 없습니다.");
        return {};
    }

    take->rate = songRate;
    take->length = juce::jmax (take->drum.getNumSamples(), take->song.getNumSamples());

    if (take->length <= 0)
    {
        error = K ("녹음된 소리가 없습니다.");
        return {};
    }

    // 원곡 드럼이 있으면 같이 올린다(없거나 맞지 않으면 그냥 없는 것으로 친다).
    if (const auto file = TakeFiles::songDrumsIn (folder); file.existsAsFile())
    {
        double songDrumsRate = 0.0;

        if (readStereo (file, take->songDrums, songDrumsRate) && std::abs (songDrumsRate - take->rate) < 0.5)
            take->songDrumsReady.store (true, std::memory_order_release);
        else
            take->songDrums.setSize (2, 0);
    }

    take->drumPeaks = computePeaks (take->drum, take->length);
    take->songPeaks = computePeaks (take->song, take->length);
    take->trimStart = 0;
    take->trimEnd = take->length;
    return take;
}

bool TakePlayer::attachSongDrums (const juce::File& file, juce::String& error)
{
    if (hasSongDrums())
        return true;

    juce::AudioBuffer<float> buffer;
    double fileRate = 0.0;

    if (! readStereo (file, buffer, fileRate) || std::abs (fileRate - rate) >= 0.5)
    {
        error = K ("분리한 원곡 드럼 파일을 읽을 수 없습니다.");
        return false;
    }

    // 오디오 스레드는 songDrumsReady가 true가 된 뒤에만 songDrums를 읽는다.
    songDrums = std::move (buffer);
    songDrumsReady.store (true, std::memory_order_release);
    return true;
}

float TakePlayer::songDrumGainFor (float songGainValue) const noexcept
{
    if (! hasSongDrums())
        return 0.0f;

    // 원곡.wav에는 원곡 드럼이 이미 다 들어 있다. 남길 만큼(amount)만 남기도록 나머지를 뺀다.
    const float amount = juce::jlimit (0.0f, 1.0f, songDrumAmount.load (std::memory_order_relaxed));
    return songGainValue * (amount - 1.0f);
}

void TakePlayer::seek (juce::int64 samplePosition) noexcept
{
    const auto clamped = juce::jlimit<juce::int64> (0, length, samplePosition);
    position = clamped;
    seekRequest = clamped;
}

void TakePlayer::renderAdding (float* left, float* right, int numSamples, double outputRate) noexcept
{
    const float targetDrum = drumGain.load (std::memory_order_relaxed);
    const float targetSong = songGain.load (std::memory_order_relaxed);
    const float targetSongDrum = songDrumGainFor (targetSong);

    if (currentDrumGain < 0.0f)
    {
        currentDrumGain = targetDrum;
        currentSongGain = targetSong;
        currentSongDrumGain = targetSongDrum;
    }

    auto pos = position.load();

    if (const auto requested = seekRequest.exchange (-1); requested >= 0)
    {
        pos = requested;
        readFraction = 0.0;
    }

    if (! playing.load())
    {
        currentDrumGain = targetDrum;
        currentSongGain = targetSong;
        currentSongDrumGain = targetSongDrum;
        return;
    }

    // songDrumsReady는 한 번 true가 되면 바뀌지 않고, 그 뒤로 songDrums도 바뀌지 않는다.
    const bool withSongDrums = hasSongDrums();

    const auto end = juce::jmin (length, trimEnd.load());

    // 장치의 샘플레이트가 녹음 때와 다르면 녹음 샘플을 outputRate 한 칸마다 rate/outputRate칸씩 읽는다.
    // (그대로 흘려보내면 44.1kHz 녹음이 48kHz 장치에서 8.8% 빠르고 1.5반음 높게 들린다)
    if (const double step = outputRate > 0.0 ? rate / outputRate : 1.0; std::abs (step - 1.0) > 1.0e-9)
    {
        const int offset = offsetSamples.load (std::memory_order_relaxed);
        const int lastSong = song.getNumChannels() > 1 ? 1 : 0, lastDrum = drum.getNumChannels() > 1 ? 1 : 0;
        const float* songL = song.getReadPointer (0);
        const float* songR = song.getReadPointer (lastSong);
        const float* drumL = drum.getReadPointer (0);
        const float* drumR = drum.getReadPointer (lastDrum);
        const juce::int64 songLength = song.getNumSamples(), drumLength = drum.getNumSamples();
        const float songStep = (targetSong - currentSongGain) / (float) juce::jmax (1, numSamples);
        const float drumStep = (targetDrum - currentDrumGain) / (float) juce::jmax (1, numSamples);
        const float songDrumStep = (targetSongDrum - currentSongDrumGain) / (float) juce::jmax (1, numSamples);
        const bool subtract = withSongDrums && (currentSongDrumGain != 0.0f || targetSongDrum != 0.0f);
        const float* songDrumL = subtract ? songDrums.getReadPointer (0) : nullptr;
        const float* songDrumR = subtract ? songDrums.getReadPointer (songDrums.getNumChannels() > 1 ? 1 : 0) : nullptr;
        const juce::int64 songDrumLength = subtract ? songDrums.getNumSamples() : 0;

        double source = (double) pos + readFraction;

        for (int i = 0; i < numSamples && source < (double) end; ++i, source += step)
        {
            const float gs = currentSongGain + songStep * (float) i;
            const float gd = currentDrumGain + drumStep * (float) i;
            const double d = source - (double) offset;

            left[i]  += readAt (songL, songLength, source) * gs + readAt (drumL, drumLength, d) * gd;
            right[i] += readAt (songR, songLength, source) * gs + readAt (drumR, drumLength, d) * gd;

            if (subtract)
            {
                const float gsd = currentSongDrumGain + songDrumStep * (float) i;
                left[i]  += readAt (songDrumL, songDrumLength, source) * gsd;
                right[i] += readAt (songDrumR, songDrumLength, source) * gsd;
            }
        }

        currentDrumGain = targetDrum;
        currentSongGain = targetSong;
        currentSongDrumGain = targetSongDrum;

        if (source >= (double) end)
        {
            playing = false;
            position = trimStart.load();
            readFraction = 0.0;
        }
        else
        {
            const auto whole = (juce::int64) std::floor (source);
            readFraction = source - (double) whole;
            position = whole;
        }

        return;
    }

    readFraction = 0.0;
    const int count = (int) juce::jlimit<juce::int64> (0, numSamples, end - pos);

    if (count > 0)
    {
        const int offset = offsetSamples.load (std::memory_order_relaxed);
        addSource (song, pos, count, left, right, currentSongGain, targetSong);
        addSource (drum, pos - offset, count, left, right, currentDrumGain, targetDrum);

        if (withSongDrums)
            addSource (songDrums, pos, count, left, right, currentSongDrumGain, targetSongDrum);
    }

    currentDrumGain = targetDrum;
    currentSongGain = targetSong;
    currentSongDrumGain = targetSongDrum;
    pos += count;

    if (pos >= end)
    {
        playing = false;
        position = trimStart.load();
    }
    else
    {
        position = pos;
    }
}

bool TakePlayer::exportMix (const juce::File& destination, ExportFormat format, juce::String& error) const
{
    const auto start = juce::jlimit<juce::int64> (0, length, trimStart.load());
    const auto end = juce::jlimit<juce::int64> (start, length, trimEnd.load());
    const juce::int64 total = end - start;

    if (total < 1)
    {
        error = K ("저장할 구간이 없습니다.");
        return false;
    }

    std::unique_ptr<juce::AudioFormatWriter> writer;
    Mp3FileWriter mp3;

    if (format == ExportFormat::mp3)
    {
        if (! mp3.open (destination, rate, mp3Kbps, error))
            return false;
    }
    else
    {
        destination.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();

        if (stream == nullptr)
        {
            error = K ("파일을 만들 수 없습니다: ") + destination.getFullPathName();
            return false;
        }

        juce::WavAudioFormat wav;
        writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                  .withSampleRate (rate)
                                                  .withNumChannels (2)
                                                  .withBitsPerSample (24));

        if (writer == nullptr)
        {
            error = K ("WAV 파일을 쓸 수 없습니다.");
            return false;
        }
    }

    const float drumG = drumGain.load(), songG = songGain.load();
    const float songDrumG = songDrumGainFor (songG);
    const int offset = offsetSamples.load();
    const int fade = juce::jmax (1, juce::roundToInt (rate * 0.01));   // 앞뒤 10ms

    mixdsp::Limiter limiter;
    limiter.prepare (rate);

    constexpr int blockSize = 8192;
    juce::AudioBuffer<float> block (2, blockSize);

    for (juce::int64 done = 0; done < total; done += blockSize)
    {
        const int n = (int) juce::jmin<juce::int64> (blockSize, total - done);
        block.clear();
        float* left = block.getWritePointer (0);
        float* right = block.getWritePointer (1);

        addSource (song, start + done, n, left, right, songG, songG);
        addSource (drum, start + done - offset, n, left, right, drumG, drumG);

        if (hasSongDrums())
            addSource (songDrums, start + done, n, left, right, songDrumG, songDrumG);

        if (done < fade || total - done - n < fade)
        {
            for (int i = 0; i < n; ++i)
            {
                const juce::int64 t = done + i;
                const float g = juce::jmin (1.0f, (float) t / (float) fade, (float) (total - 1 - t) / (float) fade);

                if (g < 1.0f)
                {
                    left[i] *= juce::jmax (0.0f, g);
                    right[i] *= juce::jmax (0.0f, g);
                }
            }
        }

        limiter.process (left, right, n);

        const bool written = writer != nullptr ? writer->writeFromAudioSampleBuffer (block, 0, n)
                                               : mp3.write (left, right, n);

        if (! written)
        {
            error = K ("파일을 쓰는 중에 문제가 생겼습니다. 디스크 공간을 확인해 주세요.");
            return false;   // MP3는 쓰다 만 파일을 지운다
        }
    }

    if (writer != nullptr)
    {
        writer.reset();   // 남은 데이터를 쓰고 파일을 닫는다
        return true;
    }

    return mp3.finish (error);
}
