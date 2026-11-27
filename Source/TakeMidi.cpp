#include "TakeMidi.h"
#include "Take.h"

namespace takemidi
{
//============================================================================== 음 번호
int toGeneralMidi (DrumModule::Brand brand, int note) noexcept
{
    if (brand == DrumModule::Brand::yamaha)
    {
        // DTX-PRO·DTX700·DTX900 기본 배치(drumPadName). 림·엣지·벨은 GM에 따로 없어서 같은 북·심벌로 적는다.
        // 다시 만들 때는 원래 친 음과 짝지어 림·엣지를 되살린다.
        switch (note)
        {
            case 36: return 36;
            case 38: return 38;
            case 40: return 40;
            case 37: return 37;
            case 48: case 14: case 15: return 48;
            case 47: case 18: case 19: return 47;
            case 43: case 20: case 23: return 43;
            case 41: case 24: case 25: return 41;
            case 42: case 79: return 42;            // 몸통·엣지(닫힘)
            case 46: case 78: return 46;            // 몸통·엣지(열림)
            case 44: case 83: case 95: return 44;   // 페달 밟기·스플래시
            case 59: case 55: case 49: return 49;
            case 17: case 16: case 57: return 57;
            case 51: case 52: return 51;
            case 53: return 53;
            default: return -1;
        }
    }

    if (brand == DrumModule::Brand::roland)
    {
        switch (note)
        {
            case 36: return 36;
            case 38: return 38;
            case 40: return 40;
            case 37: return 37;
            case 48: case 50: return 48;
            case 45: case 47: return 45;
            case 43: case 58: return 43;
            case 41: case 39: return 41;
            case 42: case 22: return 42;
            case 46: case 26: return 46;
            case 44: return 44;
            case 49: case 55: return 49;
            case 57: case 52: return 57;
            case 51: case 59: return 51;
            case 53: return 53;
            default: return -1;
        }
    }

    return note >= 35 && note <= 81 ? note : -1;   // 나머지 모듈은 GM 배치로 본다
}

int fromGeneralMidi (DrumModule::Brand brand, int gm) noexcept
{
    if (brand == DrumModule::Brand::yamaha)
    {
        switch (gm)
        {
            case 35: case 36: return 36;
            case 38: case 39: return 38;
            case 40: return 40;
            case 37: return 37;
            case 50: case 48: return 48;
            case 47: return 47;
            case 45: case 43: return 43;
            case 41: return 41;
            case 42: return 42;
            case 46: return 46;
            case 44: return 44;
            case 49: case 55: return 49;
            case 57: case 52: return 57;
            case 51: case 59: return 51;
            case 53: return 53;
            default: return -1;
        }
    }

    if (brand == DrumModule::Brand::roland)
    {
        switch (gm)
        {
            case 35: case 36: return 36;
            case 38: case 39: return 38;
            case 40: return 40;
            case 37: return 37;
            case 50: case 48: return 48;
            case 47: case 45: return 45;
            case 43: return 43;
            case 41: return 41;
            case 42: return 42;
            case 46: return 46;
            case 44: return 44;
            case 49: case 55: return 49;
            case 57: case 52: return 57;
            case 51: case 59: return 51;
            case 53: return 53;
            default: return -1;
        }
    }

    return gm >= 35 && gm <= 81 ? gm : -1;
}

bool isHiHatNote (DrumModule::Brand brand, int note) noexcept
{
    if (brand == DrumModule::Brand::yamaha)
        return note == 42 || note == 46 || note == 78 || note == 79;

    if (brand == DrumModule::Brand::roland)
        return note == 42 || note == 46 || note == 22 || note == 26;

    return note == 42 || note == 46;
}

//============================================================================== 박 시각표
double Grid::secondsAt (double beat) const noexcept
{
    const int n = (int) times.size();

    if (n < 2)
        return beat * 0.5;

    if (beat <= 0.0)
        return times[0] + beat * (times[1] - times[0]);

    if (beat >= n - 1)
        return times[(size_t) n - 1] + (beat - (n - 1)) * (times[(size_t) n - 1] - times[(size_t) n - 2]);

    const int k = (int) std::floor (beat);
    return times[(size_t) k] + (beat - k) * (times[(size_t) k + 1] - times[(size_t) k]);
}

double Grid::beatAt (double seconds) const noexcept
{
    const int n = (int) times.size();

    if (n < 2)
        return seconds / 0.5;

    if (seconds <= times[0])
        return (seconds - times[0]) / (times[1] - times[0]);

    if (seconds >= times[(size_t) n - 1])
        return (n - 1) + (seconds - times[(size_t) n - 1]) / (times[(size_t) n - 1] - times[(size_t) n - 2]);

    const auto it = std::upper_bound (times.begin(), times.end(), seconds);
    const int k = (int) (it - times.begin()) - 1;
    return k + (seconds - times[(size_t) k]) / (times[(size_t) k + 1] - times[(size_t) k]);
}

namespace
{
double medianOf (std::vector<double> values, double fallback)
{
    if (values.empty())
        return fallback;

    std::sort (values.begin(), values.end());
    return values[values.size() / 2];
}

double typicalInterval (const std::vector<double>& t, bool atEnd)
{
    std::vector<double> d;
    const int n = (int) t.size();

    for (int i = 0; i < juce::jmin (4, n - 1); ++i)
    {
        const int k = atEnd ? n - 2 - i : i;
        d.push_back (t[(size_t) k + 1] - t[(size_t) k]);
    }

    return juce::jlimit (0.2, 2.0, medianOf (d, 0.5));
}
} // namespace

Grid makeGrid (const Recording& r)
{
    Grid g;
    const double rate = r.sampleRate > 0.0 ? r.sampleRate : 44100.0;
    const double length = (double) r.lengthSamples / rate;
    std::vector<double> t;
    std::vector<char> down;

    if (r.beats.isValid() && ! r.spans.empty())
    {
        // 원곡이 가장 오래 흐른 구간 하나(보통 녹음 전체). 녹음 중에 곡을 옮겼으면 그 앞뒤는 같은 빠르기로 이어 붙인다.
        int best = -1;
        double bestLength = 0.0;

        for (size_t i = 0; i < r.spans.size(); ++i)
        {
            if (! r.spans[i].rolling)
                continue;

            const auto end = i + 1 < r.spans.size() ? r.spans[i + 1].takeSample : r.lengthSamples;
            const double spanLength = (double) (end - r.spans[i].takeSample) / rate;

            if (spanLength > bestLength)
            {
                bestLength = spanLength;
                best = (int) i;
            }
        }

        if (best >= 0 && bestLength > 1.0)
        {
            const auto& span = r.spans[(size_t) best];
            const double takeStart = (double) span.takeSample / rate;
            const double songEnd = span.songTime + bestLength;

            for (size_t i = 0; i < r.beats.beats.size(); ++i)
            {
                const double song = r.beats.beats[i] + r.clickOffsetSeconds;

                if (song < span.songTime || song >= songEnd)
                    continue;

                const double take = takeStart + (song - span.songTime) + r.gridShiftSeconds;

                if (! t.empty() && take - t.back() < 0.1)   // 너무 가까운 박(분석 잡음)은 건너뛴다
                    continue;

                t.push_back (take);
                down.push_back (r.beats.downbeat[i]);
            }
        }
    }

    if (t.size() >= 4)
    {
        g.fromSong = true;

        // 마디 길이: 마디 첫 박 사이의 박 수 중 가장 흔한 것
        std::map<int, int> counts;
        int last = -1;

        for (int i = 0; i < (int) down.size(); ++i)
        {
            if (down[(size_t) i] == 0)
                continue;

            if (last >= 0 && i - last >= 2 && i - last <= 12)
                ++counts[i - last];

            last = i;
        }

        int bestCount = 0;

        for (const auto& [beatsInBar, count] : counts)
        {
            if (count > bestCount)
            {
                bestCount = count;
                g.beatsPerBar = beatsInBar;
            }
        }
    }
    else
    {
        t = { 0.0, 0.5 };   // 원곡 박자를 모르면 120BPM 4/4, 녹음 시작이 1마디 1박
        down = { 1, 0 };
        g.beatsPerBar = 4;
    }

    int firstDown = 0;

    for (int i = 0; i < (int) down.size(); ++i)
    {
        if (down[(size_t) i] != 0)
        {
            firstDown = i;
            break;
        }
    }

    // 앞으로 늘린다: 녹음 시작(0초)을 덮고, 0번 박이 마디 첫 박이 되게(1마디 1박)
    const double front = typicalInterval (t, false);

    while (t.front() > 1.0e-9 || firstDown % g.beatsPerBar != 0)
    {
        t.insert (t.begin(), t.front() - front);
        ++firstDown;
    }

    // 뒤로 늘린다: 녹음 끝까지 덮는다
    const double back = typicalInterval (t, true);

    while (t.back() < length + 4.0 * back)
        t.push_back (t.back() + back);

    std::vector<double> intervals;

    for (size_t i = 1; i < t.size(); ++i)
        intervals.push_back (t[i] - t[i - 1]);

    g.bpm = 60.0 / medianOf (intervals, 0.5);
    g.times = std::move (t);
    return g;
}

//============================================================================== 파일 쓰기
namespace
{
bool writeFile (const juce::MidiFile& file, const juce::File& destination, juce::String& error)
{
    destination.deleteFile();
    juce::FileOutputStream out (destination);

    if (! out.openedOk() || ! file.writeTo (out, 1))
    {
        error = K ("MIDI 파일을 쓸 수 없습니다: ") + destination.getFullPathName();
        return false;
    }

    return true;
}

juce::MidiMessageSequence makeTempoTrack (const Grid& grid, const juce::String& name)
{
    juce::MidiMessageSequence track;
    track.addEvent (juce::MidiMessage::textMetaEvent (3, name), 0.0);
    track.addEvent (juce::MidiMessage::timeSignatureMetaEvent (grid.beatsPerBar, 4), 0.0);
    int lastMicros = -1;

    for (size_t k = 0; k + 1 < grid.times.size(); ++k)
    {
        const int micros = juce::jlimit (1, 0xffffff, juce::roundToInt ((grid.times[k + 1] - grid.times[k]) * 1.0e6));

        if (micros != lastMicros)
        {
            track.addEvent (juce::MidiMessage::tempoMetaEvent (micros), (double) k * ppq);
            lastMicros = micros;
        }
    }

    return track;
}
} // namespace

bool writeTakeMidi (const Recording& r, const juce::File& folder, Written& written, juce::String& error)
{
    error.clear();
    const double rate = r.sampleRate;

    if (rate <= 0.0)
        return false;

    int noteCount = 0;

    for (const auto& e : r.events)
        if (e.size == 3 && (e.data[0] & 0xf0) == 0x90 && e.data[2] > 0)
            ++noteCount;

    if (noteCount == 0)
        return false;

    const auto grid = makeGrid (r);
    const auto tickOf = [&] (juce::int64 sample)
    {
        return (double) juce::roundToInt (grid.beatAt ((double) sample / rate) * ppq);
    };

    // 연주 원본.mid: 모듈이 보낸 그대로
    juce::MidiMessageSequence raw;
    raw.addEvent (juce::MidiMessage::textMetaEvent (3, "Drum module"), 0.0);

    // 연주.mid: GM 음 번호, 10번 채널. 음 길이는 64분음표(이지드러머는 길이를 쓰지 않는다)
    juce::MidiMessageSequence gm;
    gm.addEvent (juce::MidiMessage::textMetaEvent (3, "Drums"), 0.0);
    int gmNotes = 0;

    for (const auto& e : r.events)
    {
        if (e.size == 0 || e.size > 3)
            continue;

        const double tick = tickOf (e.sample);
        raw.addEvent (juce::MidiMessage (e.data, (int) e.size), tick);

        if (e.size != 3)
            continue;

        const int status = e.data[0] & 0xf0;

        if (status == 0x90 && e.data[2] > 0)
        {
            const int note = toGeneralMidi (r.brand, e.data[1]);

            if (note >= 0)
            {
                gm.addEvent (juce::MidiMessage::noteOn (10, note, (juce::uint8) e.data[2]), tick);
                gm.addEvent (juce::MidiMessage::noteOff (10, note), tick + ppq / 16);
                ++gmNotes;
            }
        }
        else if (status == 0xb0 && e.data[1] == 4)
        {
            gm.addEvent (juce::MidiMessage::controllerEvent (10, 4, e.data[2]), tick);   // 하이햇 페달
        }
        else if (status == 0xa0)
        {
            if (const int note = toGeneralMidi (r.brand, e.data[1]); note >= 0)
                gm.addEvent (juce::MidiMessage::aftertouchChange (10, note, e.data[2]), tick);   // 심벌 초크
        }
    }

    raw.sort();
    gm.sort();
    gm.updateMatchedPairs();

    juce::MidiFile rawFile, gmFile;
    rawFile.setTicksPerQuarterNote (ppq);
    gmFile.setTicksPerQuarterNote (ppq);
    rawFile.addTrack (makeTempoTrack (grid, "DrumVstHost"));
    rawFile.addTrack (raw);
    gmFile.addTrack (makeTempoTrack (grid, "DrumVstHost"));
    gmFile.addTrack (gm);

    if (! writeFile (rawFile, folder.getChildFile (TakeFiles::rawMidi()), error)
        || (gmNotes > 0 && ! writeFile (gmFile, folder.getChildFile (TakeFiles::midi()), error)))
        return false;

    written.startSeconds = grid.times.front();
    written.bpm = grid.bpm;
    written.beatsPerBar = grid.beatsPerBar;
    written.fromSong = grid.fromSong;
    written.notes = gmNotes;
    return true;
}

//============================================================================== 다시 만들기
namespace
{
bool readMidiFile (const juce::File& file, juce::MidiFile& midi, juce::String& error)
{
    juce::FileInputStream in (file);

    if (! in.openedOk() || ! midi.readFrom (in) || midi.getNumTracks() == 0)
    {
        error = K ("MIDI 파일을 읽을 수 없습니다: ") + file.getFileName();
        return false;
    }

    if (midi.getTimeFormat() <= 0)
    {
        error = K ("박자 단위가 아닌(SMPTE) MIDI 파일은 읽을 수 없습니다: ") + file.getFileName();
        return false;
    }

    return true;
}

// MIDI 파일의 템포로 틱 ↔ 초를 바꾼다.
struct TempoMap
{
    struct Change
    {
        double tick = 0.0, seconds = 0.0, secondsPerTick = 0.0;
    };

    std::vector<Change> changes;
    double ppqn = 960.0;

    explicit TempoMap (const juce::MidiFile& file)
    {
        ppqn = (double) file.getTimeFormat();
        std::vector<std::pair<double, double>> tempos;   // 틱, 4분음표당 초

        for (int t = 0; t < file.getNumTracks(); ++t)
            for (const auto* holder : *file.getTrack (t))
                if (holder->message.isTempoMetaEvent())
                    tempos.emplace_back (holder->message.getTimeStamp(), holder->message.getTempoSecondsPerQuarterNote());

        std::stable_sort (tempos.begin(), tempos.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

        changes.push_back ({ 0.0, 0.0, 0.5 / ppqn });

        for (const auto& [tick, secondsPerQuarter] : tempos)
        {
            if (secondsPerQuarter <= 0.0)
                continue;

            const auto& last = changes.back();
            const double seconds = last.seconds + (tick - last.tick) * last.secondsPerTick;

            if (tick <= last.tick)
                changes.back().secondsPerTick = secondsPerQuarter / ppqn;
            else
                changes.push_back ({ tick, seconds, secondsPerQuarter / ppqn });
        }
    }

    double secondsAt (double tick) const noexcept
    {
        auto it = std::upper_bound (changes.begin(), changes.end(), tick, [] (double t, const Change& c) { return t < c.tick; });
        const auto& c = it == changes.begin() ? changes.front() : *(it - 1);
        return c.seconds + (tick - c.tick) * c.secondsPerTick;
    }

    double tickAt (double seconds) const noexcept
    {
        auto it = std::upper_bound (changes.begin(), changes.end(), seconds, [] (double s, const Change& c) { return s < c.seconds; });
        const auto& c = it == changes.begin() ? changes.front() : *(it - 1);
        return c.tick + (seconds - c.seconds) / c.secondsPerTick;
    }
};

struct Hit
{
    double seconds = 0.0;
    int module = 0, gm = -1, velocity = 0, channel = 10;
    bool used = false;
};

struct Fresh
{
    double beat = 0.0, seconds = 0.0;
    int gm = 0, velocity = 0;
};
} // namespace

bool rebuild (const juce::File& folder, const TakeInfo& info, const juce::File& editedFile, Rebuilt& out, juce::String& error)
{
    out = {};
    const double rate = info.sampleRate;
    const auto brand = (DrumModule::Brand) info.midiBrand;

    if (rate <= 0.0 || info.lengthSamples <= 0)
    {
        error = K ("녹음 정보(정보.xml)를 읽을 수 없습니다.");
        return false;
    }

    const auto rawPath = TakeFiles::rawMidiIn (folder);

    if (! rawPath.existsAsFile())
    {
        error = K ("이 녹음에는 연주 원본 MIDI가 없습니다.");
        return false;
    }

    juce::MidiFile raw, edited;

    if (! readMidiFile (rawPath, raw, error) || ! readMidiFile (editedFile, edited, error))
        return false;

    const TempoMap tempo (raw);
    const auto secondsAtBeat = [&] (double beat) { return info.midiStart + tempo.secondsAt (beat * tempo.ppqn); };
    const auto beatAtSeconds = [&] (double seconds) { return tempo.tickAt (seconds - info.midiStart) / tempo.ppqn; };

    int beatsPerBar = 4;

    for (int t = 0; t < raw.getNumTracks(); ++t)
    {
        for (const auto* holder : *raw.getTrack (t))
        {
            if (holder->message.isTimeSignatureMetaEvent())
            {
                int numerator = 4, denominator = 4;
                holder->message.getTimeSignatureInfo (numerator, denominator);
                beatsPerBar = juce::jlimit (1, 16, numerator);
            }
        }
    }

    // 원래 녹음의 신호(모듈이 보낸 그대로)
    std::vector<RawEvent> rawEvents;
    std::vector<Hit> hits;
    std::map<int, int> channelCounts;

    for (int t = 0; t < raw.getNumTracks(); ++t)
    {
        for (const auto* holder : *raw.getTrack (t))
        {
            const auto& m = holder->message;

            if (m.isMetaEvent() || m.isSysEx() || m.getRawDataSize() < 1 || m.getRawDataSize() > 3)
                continue;

            const double seconds = info.midiStart + tempo.secondsAt (m.getTimeStamp());
            RawEvent e;
            e.sample = (juce::int64) std::llround (seconds * rate);
            e.size = (juce::uint8) m.getRawDataSize();
            std::memcpy (e.data, m.getRawData(), e.size);
            rawEvents.push_back (e);

            if (m.isNoteOn())
            {
                Hit h;
                h.seconds = seconds;
                h.module = m.getNoteNumber();
                h.gm = toGeneralMidi (brand, h.module);
                h.velocity = m.getVelocity();
                h.channel = m.getChannel();
                hits.push_back (h);
                ++channelCounts[h.channel];
            }
        }
    }

    int defaultChannel = 10, mostNotes = 0;

    for (const auto& [channel, count] : channelCounts)
    {
        if (count > mostNotes)
        {
            mostNotes = count;
            defaultChannel = channel;
        }
    }

    // 고친 MIDI의 음(박 단위 위치)
    std::vector<Fresh> fresh;
    const double editedPpq = (double) edited.getTimeFormat();

    for (int t = 0; t < edited.getNumTracks(); ++t)
    {
        for (const auto* holder : *edited.getTrack (t))
        {
            const auto& m = holder->message;

            if (m.isNoteOn())
                fresh.push_back ({ m.getTimeStamp() / editedPpq, 0.0, m.getNoteNumber(), m.getVelocity() });
        }
    }

    if (fresh.empty())
    {
        error = K ("고친 MIDI에 음이 하나도 없습니다.");
        return false;
    }

    std::stable_sort (fresh.begin(), fresh.end(), [] (const Fresh& a, const Fresh& b) { return a.beat < b.beat; });

    // 음 높이(GM)마다 원래 친 음의 박 위치
    std::map<int, std::vector<double>> beatsByNote;

    for (const auto& h : hits)
        if (h.gm >= 0)
            beatsByNote[h.gm].push_back (beatAtSeconds (h.seconds));

    for (auto& [note, beats] : beatsByNote)
        std::sort (beats.begin(), beats.end());

    // 고친 MIDI가 몇 마디 밀려 있는지(일부 마디만 끌어냈거나 다른 마디에 놓았다가 끌어낸 경우): 원래 음과 가장 많이 겹치는 마디 수
    const auto matchesWithShift = [&] (double shift)
    {
        int count = 0;

        for (const auto& f : fresh)
        {
            const auto found = beatsByNote.find (f.gm);

            if (found == beatsByNote.end())
                continue;

            const auto& beats = found->second;
            const double b = f.beat + shift;
            const auto it = std::lower_bound (beats.begin(), beats.end(), b);

            if ((it != beats.end() && *it - b <= 0.2) || (it != beats.begin() && b - *(it - 1) <= 0.2))
                ++count;
        }

        return count;
    };

    double lastBeat = fresh.back().beat;

    for (const auto& [note, beats] : beatsByNote)
        lastBeat = juce::jmax (lastBeat, beats.back());

    const int maxBars = juce::jmin (2000, (int) (lastBeat / beatsPerBar) + 2);
    int bestBars = 0, bestCount = matchesWithShift (0.0);

    for (int bars = -maxBars; bars <= maxBars; ++bars)
    {
        if (bars == 0)
            continue;

        if (const int count = matchesWithShift ((double) bars * beatsPerBar); count > bestCount)
        {
            bestCount = count;
            bestBars = bars;
        }
    }

    out.barOffset = bestBars;

    for (auto& f : fresh)
    {
        f.beat += (double) bestBars * beatsPerBar;
        f.seconds = secondsAtBeat (f.beat);
    }

    // 고친 범위: 고친 MIDI의 첫 음이 든 마디 처음부터 마지막 음이 든 마디 끝까지. 그 밖은 원래 녹음 그대로 둔다.
    const double regionStart = secondsAtBeat (std::floor (fresh.front().beat / beatsPerBar) * beatsPerBar);
    const double regionEnd = secondsAtBeat ((std::floor (fresh.back().beat / beatsPerBar) + 1.0) * beatsPerBar);
    const auto inRegion = [&] (double seconds) { return seconds >= regionStart - 1.0e-6 && seconds < regionEnd; };

    // 하이햇 페달(CC4) 값: 그 시각에 마지막으로 받은 값(-1 = 아직 없음)
    std::vector<std::pair<double, int>> pedal;

    for (const auto& e : rawEvents)
        if (e.size == 3 && (e.data[0] & 0xf0) == 0xb0 && e.data[1] == 4)
            pedal.emplace_back ((double) e.sample / rate, (int) e.data[2]);

    const auto pedalAt = [&] (double seconds)
    {
        auto it = std::upper_bound (pedal.begin(), pedal.end(), seconds, [] (double s, const auto& p) { return s < p.first; });
        return it == pedal.begin() ? -1 : (it - 1)->second;
    };

    // 음 높이마다 고친 음과 원래 음을 짝짓는다: 박의 0.3(최소 60ms) 안에서 가장 가까운 쌍부터.
    // (시각 순서대로 하면, 원래 음 바로 앞에 새로 그린 고스트 노트가 그 음을 먼저 가져가 버린다)
    std::map<int, std::vector<int>> hitsByNote;

    for (int i = 0; i < (int) hits.size(); ++i)
        if (hits[(size_t) i].gm >= 0 && inRegion (hits[(size_t) i].seconds))
            hitsByNote[hits[(size_t) i].gm].push_back (i);

    for (auto& [note, list] : hitsByNote)
        std::stable_sort (list.begin(), list.end(), [&] (int a, int b) { return hits[(size_t) a].seconds < hits[(size_t) b].seconds; });

    struct Pair
    {
        double distance = 0.0;
        int fresh = 0, hit = 0;
    };

    std::vector<Pair> pairs;

    for (int fi = 0; fi < (int) fresh.size(); ++fi)
    {
        const auto& f = fresh[(size_t) fi];
        const auto found = hitsByNote.find (f.gm);

        if (found == hitsByNote.end())
            continue;

        const double beatSeconds = secondsAtBeat (f.beat + 0.5) - secondsAtBeat (f.beat - 0.5);
        const double tolerance = juce::jmax (0.06, 0.3 * beatSeconds);
        const auto& list = found->second;
        auto it = std::lower_bound (list.begin(), list.end(), f.seconds - tolerance,
                                    [&] (int index, double s) { return hits[(size_t) index].seconds < s; });

        for (; it != list.end() && hits[(size_t) *it].seconds <= f.seconds + tolerance; ++it)
            pairs.push_back ({ std::abs (hits[(size_t) *it].seconds - f.seconds), fi, *it });
    }

    std::stable_sort (pairs.begin(), pairs.end(), [] (const Pair& a, const Pair& b) { return a.distance < b.distance; });
    std::vector<int> matchOf (fresh.size(), -1);

    for (const auto& pair : pairs)
    {
        if (matchOf[(size_t) pair.fresh] >= 0 || hits[(size_t) pair.hit].used)
            continue;

        matchOf[(size_t) pair.fresh] = pair.hit;
        hits[(size_t) pair.hit].used = true;
    }

    const double tail = 0.010;   // 음 끔은 10ms 뒤

    for (int fi = 0; fi < (int) fresh.size(); ++fi)
    {
        const auto& f = fresh[(size_t) fi];
        const int matched = matchOf[(size_t) fi];
        const int module = matched >= 0 ? hits[(size_t) matched].module : fromGeneralMidi (brand, f.gm);
        const int channel = matched >= 0 ? hits[(size_t) matched].channel : defaultChannel;
        const auto sample = (juce::int64) std::llround (f.seconds * rate);

        if (module < 0 || sample < 0 || sample >= info.lengthSamples)
        {
            ++out.ignored;
            continue;
        }

        if (matched >= 0)
        {
            auto& h = hits[(size_t) matched];
            h.used = true;
            (std::abs (h.seconds - f.seconds) < 0.001 && h.velocity == f.velocity ? out.kept : out.moved)++;
        }
        else
        {
            ++out.added;
        }

        // 하이햇은 그 음을 칠 때의 페달 값을 바로 앞에 넣는다(원래 친 음이면 그때 값, 새 음이면 닫힘·열림에 맞게)
        if (! pedal.empty() && isHiHatNote (brand, module))
        {
            int value = -1;

            if (matched >= 0)
                value = pedalAt (hits[(size_t) matched].seconds);
            else if (f.gm == 42)
                value = 127;
            else if (const int now = pedalAt (f.seconds); now >= 0 && now < 100)
                value = now;
            else
                value = 0;

            if (value >= 0)
            {
                RawEvent cc;
                cc.sample = juce::jmax<juce::int64> (0, sample - 1);
                cc.size = 3;
                cc.data[0] = (juce::uint8) (0xb0 | ((channel - 1) & 0x0f));
                cc.data[1] = 4;
                cc.data[2] = (juce::uint8) juce::jlimit (0, 127, value);
                out.events.push_back (cc);
            }
        }

        RawEvent on, off;
        on.sample = sample;
        on.size = off.size = 3;
        on.data[0] = (juce::uint8) (0x90 | ((channel - 1) & 0x0f));
        on.data[1] = off.data[1] = (juce::uint8) module;
        on.data[2] = (juce::uint8) juce::jlimit (1, 127, f.velocity);
        off.sample = juce::jmin (info.lengthSamples - 1, sample + (juce::int64) (tail * rate));
        off.data[0] = (juce::uint8) (0x80 | ((channel - 1) & 0x0f));
        off.data[2] = 0;
        out.events.push_back (on);
        out.events.push_back (off);
    }

    for (const auto& h : hits)
        if (h.gm >= 0 && inRegion (h.seconds) && ! h.used)
            ++out.removed;

    // 원래 녹음에서 남길 것: 페달·초크 등 음이 아닌 신호 전부, 고친 범위 밖의 음, GM으로 바꾸지 못하는 음(고칠 수 없다)
    for (const auto& e : rawEvents)
    {
        const int status = e.data[0] & 0xf0;
        const bool isNote = e.size == 3 && (status == 0x90 || status == 0x80);

        if (isNote)
        {
            const double seconds = (double) e.sample / rate;

            if (toGeneralMidi (brand, e.data[1]) >= 0 && inRegion (seconds))
                continue;
        }

        if (e.sample >= 0 && e.sample < info.lengthSamples)
            out.events.push_back (e);
    }

    std::stable_sort (out.events.begin(), out.events.end(), [] (const RawEvent& a, const RawEvent& b) { return a.sample < b.sample; });
    return true;
}

//============================================================================== 박자 맞추기
bool quantizeRaw (const juce::File& folder, const TakeInfo& info, double stepBeats, double strength, Rebuilt& out, juce::String& error)
{
    out = {};
    const double rate = info.sampleRate;
    const auto brand = (DrumModule::Brand) info.midiBrand;
    strength = juce::jlimit (0.0, 1.0, strength);

    if (rate <= 0.0 || info.lengthSamples <= 0 || stepBeats <= 0.0)
    {
        error = K ("녹음 정보(정보.xml)를 읽을 수 없습니다.");
        return false;
    }

    const auto rawPath = TakeFiles::rawMidiIn (folder);

    if (! rawPath.existsAsFile())
    {
        error = K ("이 녹음에는 연주 원본 MIDI가 없습니다.");
        return false;
    }

    juce::MidiFile raw;

    if (! readMidiFile (rawPath, raw, error))
        return false;

    const TempoMap tempo (raw);

    struct Item
    {
        RawEvent event;
        double seconds = 0.0;
    };

    std::vector<Item> items;
    std::vector<std::pair<double, int>> pedal;   // 하이햇 페달(CC4): 시각, 값

    for (int t = 0; t < raw.getNumTracks(); ++t)
    {
        for (const auto* holder : *raw.getTrack (t))
        {
            const auto& m = holder->message;

            if (m.isMetaEvent() || m.isSysEx() || m.getRawDataSize() < 1 || m.getRawDataSize() > 3)
                continue;

            Item item;
            item.seconds = info.midiStart + tempo.secondsAt (m.getTimeStamp());
            item.event.sample = (juce::int64) std::llround (item.seconds * rate);
            item.event.size = (juce::uint8) m.getRawDataSize();
            std::memcpy (item.event.data, m.getRawData(), item.event.size);
            items.push_back (item);

            if (item.event.size == 3 && (item.event.data[0] & 0xf0) == 0xb0 && item.event.data[1] == 4)
                pedal.emplace_back (item.seconds, (int) item.event.data[2]);
        }
    }

    std::stable_sort (items.begin(), items.end(), [] (const Item& a, const Item& b) { return a.event.sample < b.event.sample; });
    std::stable_sort (pedal.begin(), pedal.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

    const auto pedalAt = [&] (double seconds)
    {
        auto it = std::upper_bound (pedal.begin(), pedal.end(), seconds, [] (double s, const auto& p) { return s < p.first; });
        return it == pedal.begin() ? -1 : (it - 1)->second;
    };

    std::map<int, juce::int64> shiftOf;   // (채널 << 8) | 음: 아직 안 끝난 음을 옮긴 만큼(음 끔도 같이 옮긴다)

    for (const auto& item : items)
    {
        auto e = item.event;
        const int status = e.data[0] & 0xf0;
        const bool noteOn = e.size == 3 && status == 0x90 && e.data[2] > 0;
        const bool noteOff = e.size == 3 && (status == 0x80 || (status == 0x90 && e.data[2] == 0));
        const int key = ((e.data[0] & 0x0f) << 8) | e.data[1];

        if (noteOn && toGeneralMidi (brand, e.data[1]) >= 0)
        {
            const double beat = tempo.tickAt (item.seconds - info.midiStart) / tempo.ppqn;
            const double target = std::round (beat / stepBeats) * stepBeats;
            const double newBeat = beat + strength * (target - beat);
            const double newSeconds = info.midiStart + tempo.secondsAt (newBeat * tempo.ppqn);
            const auto newSample = juce::jlimit<juce::int64> (0, info.lengthSamples - 1, (juce::int64) std::llround (newSeconds * rate));
            const auto shift = newSample - e.sample;
            shiftOf[key] = shift;
            (std::abs ((double) shift) > 0.001 * rate ? out.moved : out.kept)++;

            if (! pedal.empty() && isHiHatNote (brand, e.data[1]))
            {
                if (const int value = pedalAt (item.seconds); value >= 0)
                {
                    RawEvent cc;
                    cc.sample = juce::jmax<juce::int64> (0, newSample - 1);
                    cc.size = 3;
                    cc.data[0] = (juce::uint8) (0xb0 | (e.data[0] & 0x0f));
                    cc.data[1] = 4;
                    cc.data[2] = (juce::uint8) value;
                    out.events.push_back (cc);
                }
            }

            e.sample = newSample;
        }
        else if (noteOff && toGeneralMidi (brand, e.data[1]) >= 0)
        {
            juce::int64 shift = 0;

            if (const auto found = shiftOf.find (key); found != shiftOf.end())
            {
                shift = found->second;
                shiftOf.erase (found);
            }

            e.sample = juce::jlimit<juce::int64> (0, info.lengthSamples - 1, e.sample + shift);
        }

        if (e.sample >= 0 && e.sample < info.lengthSamples)
            out.events.push_back (e);
    }

    std::stable_sort (out.events.begin(), out.events.end(), [] (const RawEvent& a, const RawEvent& b) { return a.sample < b.sample; });
    return true;
}
} // namespace takemidi
