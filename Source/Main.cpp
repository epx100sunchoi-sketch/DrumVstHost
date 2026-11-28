#include <JuceHeader.h>
#include "AnalysisTools.h"
#include "Engine.h"
#include "Look.h"
#include "MainComponent.h"
#include "DrumPlugins.h"
#include "PadMap.h"
#include "Platform.h"
#include "SongTools.h"
#include "TakeEditor.h"

namespace
{
// JUCE가 제공하는 소리 장치 설정 화면의 영어 문구를 한국어로 바꾼다.
const char* const koreanStrings = R"KO(language: Korean
countries: kr

"Audio device type:" = "오디오 방식:"
"Device:" = "장치:"
"Output:" = "출력 장치:"
"Input:" = "입력 장치:"
"Test" = "소리 테스트"
"Plays a test tone" = "삑 소리를 내서 장치가 동작하는지 확인합니다"
"Sample rate:" = "샘플 속도:"
"Audio buffer size:" = "버퍼 크기:"
"Control Panel" = "드라이버 설정"
"Opens the device's own control panel" = "장치 드라이버의 설정 창을 엽니다"
"Reset Device" = "장치 다시 시작"
"Resets the audio interface - sometimes needed after changing a device's properties in its custom control panel" = "드라이버 설정을 바꾼 뒤 소리가 이상하면 눌러 주세요"
"Active output channels:" = "사용할 출력:"
"Active input channels:" = "사용할 입력:"
"Show advanced settings..." = "고급 설정 보기..."
"Error when trying to open audio device!" = "소리 장치를 열지 못했습니다"
"(no audio output channels found)" = "(출력 채널이 없습니다)"
"(no audio input channels found)" = "(입력 채널이 없습니다)"
"none" = "없음"
"OK" = "확인"
"Cancel" = "취소"
)KO";

juce::String argAfter (const juce::StringArray& args, const juce::String& flag)
{
    const int index = args.indexOf (flag);
    return index >= 0 && index + 1 < args.size() ? args[index + 1].unquoted() : juce::String();
}

juce::String dbText (float gain)
{
    return juce::String (juce::Decibels::gainToDecibels (gain, -120.0f), 1) + " dB";
}

// 메시지 스레드에서 fn을 실행하고 끝날 때까지 기다린다(점검 도구용).
// 시간 제한을 두지 않는다: 먼저 돌아가 버리면 메시지 스레드가 나중에 이미 사라진 fn·done(과 fn이 잡은 지역 변수)을 쓴다.
// 앱이 끝나는 중이라 보낸 일이 실행되지 않을 때만 포기한다.
template <typename Fn>
bool runOnMessageThread (Fn&& fn)
{
    juce::WaitableEvent done;

    if (! juce::MessageManager::callAsync ([&fn, &done] { fn(); done.signal(); }))
        return false;

    while (! done.wait (500))
    {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm == nullptr || mm->hasStopMessageBeenSent())
            return false;
    }

    return true;
}
} // namespace

//==============================================================================
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (Engine& engine, juce::PropertiesFile& settingsToUse)
        : juce::DocumentWindow ("Drum VST Host", ui::bg,
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          settings (settingsToUse)
    {
        setUsingNativeTitleBar (true);
        main = new MainComponent (engine, settings);
        setContentOwned (main, true);
        setResizable (false, false);

        const int x = settings.getIntValue ("windowX", -100000);
        const int y = settings.getIntValue ("windowY", -100000);
        const auto screen = juce::Desktop::getInstance().getDisplays().getTotalBounds (true);

        if (x != -100000 && screen.contains (juce::Rectangle<int> (x, y, 240, 120)))
            setTopLeftPosition (x, y);
        else
            centreWithSize (getWidth(), getHeight());

        if (auto* peer = getPeer())
            setDarkTitleBar (peer->getNativeHandle());

        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    MainComponent& getMain() noexcept { return *main; }

    void savePosition()
    {
        if (isMinimised())   // 내려 둔 창의 좌표는 화면 밖이다
            return;

        settings.setValue ("windowX", getX());
        settings.setValue ("windowY", getY());
    }

private:
    juce::PropertiesFile& settings;
    MainComponent* main = nullptr;
};

//==============================================================================
// 소리 장치 없이 엔진 전체(플러그인 → 믹서 → 녹음 → 다듬기 저장)를 돌려 보고 결과를 파일로 남긴다.
//   DrumVstHost.exe --selftest 결과.txt [--plugin 경로] [--settle 밀리초]
//   "<결과 파일>.state-in" 파일이 있으면 그 상태로 플러그인을 불러온다.
class SelfTest final : private juce::Thread
{
public:
    // savedSettings가 있으면 거기 저장된 플러그인 상태(킷)를 쓴다.
    SelfTest (const juce::File& reportFile, const juce::String& pluginOverride, int settleMsToUse, juce::PropertiesFile* savedSettings)
        : juce::Thread ("selftest"), report (reportFile), settleMs (settleMsToUse)
    {
        engine = std::make_unique<Engine> (false, savedSettings);
        engine->prepareOffline (sampleRate, blockSize);

        log ("JUCE " + juce::SystemStats::getJUCEVersion());

        for (const auto& device : juce::MidiInput::getAvailableDevices())
            log ("midi-in: " + device.name);

        for (auto* type : engine->deviceManager.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            log ("audio-out [" + type->getTypeName() + "]: " + type->getDeviceNames (false).joinIntoString (" | "));
        }

        const auto found = engine->findInstalledPlugins();

        for (const auto& entry : found)
            log ("vst3: " + entry.name + (entry.isDrum ? " [drum, rank " + juce::String (entry.rank) + "]" : juce::String()) + " @ " + entry.path);

        testDrumKnowledge();

        auto path = pluginOverride;

        if (path.isEmpty() && savedSettings != nullptr)
            path = engine->getSavedPluginPath();

        if (path.isEmpty() && ! found.empty())
            path = found.front().path;

        if (path.isNotEmpty())
        {
            juce::MemoryBlock stateIn;
            const auto stateInFile = report.getSiblingFile (report.getFileName() + ".state-in");
            const bool hasStateIn = stateInFile.existsAsFile() && stateInFile.loadFileAsData (stateIn);

            // 상태 파일이 없으면 nullptr → 저장된 상태(있으면)를 쓴다.
            juce::String error;
            const bool remembered = ! hasStateIn && engine->hasRememberedState (path);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const bool ok = engine->loadPlugin (path, hasStateIn ? &stateIn : nullptr, error);
            const int elapsed = juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - start);

            log ("plugin-load: " + juce::String (ok ? "ok" : "FAILED " + error) + " (" + juce::String (elapsed) + " ms)"
                 + (hasStateIn ? " with state " + juce::String ((int) stateIn.getSize()) + " bytes" : juce::String())
                 + (remembered ? " with saved kit" : juce::String()));

            if (ok)
                log ("plugin: " + engine->getPluginName() + " " + engine->describePluginBuses());
        }
        else
        {
            log ("plugin-load: no VST3 plugin found");
        }

        toneFile = report.getSiblingFile ("selftest-tone.wav");
        writeTone (toneFile);

        juce::String error;
        log ("song-load: " + juce::String (engine->loadSong (toneFile, error) ? "ok" : "FAILED " + error));
        log ("song-length: " + juce::String (engine->getLength(), 2) + " s");

        startThread();
    }

    ~SelfTest() override
    {
        stopThread (15000);
        engine.reset();
        toneFile.deleteFile();
    }

private:
    static constexpr double sampleRate = 48000.0;
    static constexpr int blockSize = 512;

    void log (const juce::String& line)
    {
        const juce::ScopedLock lock (logLock);
        lines.add (line);
    }

    static void writeTone (const juce::File& file)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

        if (stream == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (44100.0)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (16));

        if (writer == nullptr)
            return;

        juce::AudioBuffer<float> buffer (2, 44100 * 6);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float v = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / 44100.0);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // 실제 속도에 가깝게 돌린다(원곡은 뒤에서 미리 읽어 두는 방식이라 너무 빨리 돌리면 비어 버린다).
    void render (double seconds, float& drumPeak, float& mixPeak)
    {
        drumPeak = 0.0f;
        mixPeak = 0.0f;
        const int blocks = (int) (seconds * sampleRate / blockSize);

        for (int i = 0; i < blocks && ! threadShouldExit(); ++i)
        {
            float d = 0.0f, m = 0.0f;
            engine->renderOffline (blockSize, d, m);
            drumPeak = juce::jmax (drumPeak, d);
            mixPeak = juce::jmax (mixPeak, m);
            wait (9);
        }
    }

    void hit (int note)
    {
        juce::MessageManager::callAsync ([this, note] { engine->playTestNote (note, 0.9f); });
    }

    void run() override
    {
        wait (settleMs);   // 플러그인이 샘플을 읽을 시간

        float d = 0.0f, m = 0.0f;

        render (0.4, d, m);
        log ("idle: drum " + dbText (d) + ", mix " + dbText (m));

        const std::pair<int, const char*> notes[] = { { 36, "kick" }, { 38, "snare" }, { 42, "hihat" } };

        for (const auto& [note, label] : notes)
        {
            hit (note);
            render (0.7, d, m);
            log (juce::String (label) + " (note " + juce::String (note) + "): drum " + dbText (d) + ", mix " + dbText (m));
        }

        engine->play();
        render (1.0, d, m);
        log ("song only (vol 70): mix " + dbText (m) + "  [expected about " + dbText (0.25f * 0.49f) + "]");

        // 값을 바꾼 직후의 전환 구간은 버리고 그 뒤를 잰다.
        engine->song.mid = 12.0f;
        render (0.3, d, m);
        render (0.4, d, m);
        log ("song + mid EQ +12: mix " + dbText (m));
        engine->song.mid = 0.0f;

        engine->song.comp = 1.0f;
        render (0.6, d, m);
        render (0.4, d, m);
        log ("song + compressor 100: mix " + dbText (m));
        engine->song.comp = 0.0f;

        engine->song.mute = true;
        render (0.3, d, m);
        render (0.3, d, m);
        log ("song muted: mix " + dbText (m));
        engine->song.mute = false;
        render (0.3, d, m);

        testRecording();
        testTakeMidi();
        testStemReuse();
        testMidiRecording();
        testTakeRates();
        engine->pause();
        testMidiInput();
        testMetronome();
        testStems();
        log ("done");

        juce::MessageManager::callAsync ([this]
        {
            {
                const juce::ScopedLock lock (logLock);
                report.replaceWithText (lines.joinIntoString ("\r\n") + "\r\n");
            }

            juce::JUCEApplication::quit();
        });
    }

    // 실제 패드와 같은 경로(MidiInbox)로 스네어를 넣어 드럼 소리가 나는지 본다.
    // 큰 버퍼(512, 일정 시간 늦춰 놓기)와 작은 버퍼(64, 블록 맨 앞에 바로) 두 방식을 모두 확인한다.
    void testMidiInput()
    {
        const auto hitThroughInbox = [this] (const juce::String& label)
        {
            float d = 0.0f, m = 0.0f;
            render (0.2, d, m);   // 앞 소리가 사라지게

            engine->injectMidiForTest (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 110));
            render (0.5, d, m);
            engine->injectMidiForTest (juce::MidiMessage::noteOff (10, 38));
            log ("midi-input " + label + ": drum " + dbText (d) + ", max wait " + juce::String (engine->takeMaxMidiWaitMs(), 1) + " ms");
        };

        hitThroughInbox ("buffer 512 (delay " + juce::String (Engine::midiDelayMs (blockSize, sampleRate), 2) + " ms)");

        engine->prepareOffline (44100.0, 64);
        hitThroughInbox ("buffer 64 (delay " + juce::String (Engine::midiDelayMs (64, 44100.0), 2) + " ms)");

        testMidiPlacement();
    }

    // 패드 신호를 블록 어디에 놓는지(MidiInbox): 받은 시각과 콜백 시각을 정해 두고 놓인 자리를 본다.
    // 늦추는 시간은 '버퍼 - 3ms'(3ms 이하 버퍼는 0). 그보다 오래 기다린 신호는 맨 앞, 같은 블록 안의 간격은 그대로.
    void testMidiPlacement()
    {
        const auto place = [] (int block, double rate, std::initializer_list<double> waitedMs) -> juce::String
        {
            auto inbox = std::make_unique<MidiInbox>();
            const double now = 1000000.0;
            int note = 38;

            for (auto waited : waitedMs)
                inbox->receive (juce::MidiMessage::noteOn (10, note++, (juce::uint8) 100), now - waited);

            juce::MidiBuffer buffer;
            inbox->drainInto (buffer, nullptr, block, rate, Engine::midiDelayMs (block, rate), now);

            juce::StringArray positions;

            for (const auto meta : buffer)
                positions.add (juce::String (meta.samplePosition));

            return positions.joinIntoString (",");
        };

        const auto check = [this, &place] (const juce::String& label, int block, double rate, std::initializer_list<double> waitedMs,
                                           const juce::String& expected)
        {
            const auto got = place (block, rate, waitedMs);
            log ("midi-place " + label + ": " + got + (got == expected ? " ok" : " MISMATCH, expected " + expected));
        };

        check ("buffer 64 @44.1k, waited 1.0 ms", 64, 44100.0, { 1.0 }, "0");
        check ("buffer 128 @44.1k, waited 0.2 ms", 128, 44100.0, { 0.2 }, "0");
        check ("buffer 192 @44.1k, waited 0.5 ms", 192, 44100.0, { 0.5 }, "38");
        check ("buffer 192 @44.1k, waited 2.0 ms", 192, 44100.0, { 2.0 }, "0");
        check ("buffer 192 @44.1k, flam waited 3.0/2.0 ms", 192, 44100.0, { 3.0, 2.0 }, "0,44");
        check ("buffer 192 @44.1k, flam waited 1.0/0.5 ms", 192, 44100.0, { 1.0, 0.5 }, "16,38");
        check ("buffer 512 @48k, waited 2.0 ms", 512, 48000.0, { 2.0 }, "272");
        check ("buffer 512 @48k, waited 9.0 ms", 512, 48000.0, { 9.0 }, "0");
        check ("buffer 512 @48k, stale 60 ms", 512, 48000.0, { 60.0 }, "");
    }

    // 연주 MIDI(TakeMidi): 원곡 박자로 쓴 템포·마디, 고치지 않은 MIDI 되돌리기, 박자 맞춤·고스트 노트·지운 음, 마디가 밀린 MIDI
    static int countNoteOns (const std::vector<takemidi::RawEvent>& events, int note)
    {
        int n = 0;

        for (const auto& e : events)
            if (e.size == 3 && (e.data[0] & 0xf0) == 0x90 && e.data[2] > 0 && (note < 0 || e.data[1] == note))
                ++n;

        return n;
    }

    void testTakeMidi()
    {
        const auto folder = report.getSiblingFile ("selftest-take-midi");
        folder.deleteRecursively();
        folder.createDirectory();

        takemidi::Recording r;
        r.sampleRate = 44100.0;
        r.lengthSamples = (juce::int64) (20.0 * r.sampleRate);
        r.brand = DrumModule::Brand::yamaha;
        r.gridShiftSeconds = 0.010;
        r.beats.bpm = 120.0;

        for (int k = 0; k < 60; ++k)   // 원곡 0.25초부터 120BPM, 마디 첫 박은 1번 박부터 4박마다(못갖춘마디)
        {
            r.beats.beats.push_back (0.25 + 0.5 * k);
            r.beats.downbeat.push_back ((char) (k % 4 == 1 ? 1 : 0));
        }

        r.spans.push_back ({ 0, 1.0, true });   // 녹음 0초 = 원곡 1초

        const auto beatTime = [] (double k) { return 0.25 + 0.5 * k - 1.0 + 0.010; };
        const auto add = [&] (double seconds, int b0, int b1, int b2)
        {
            takemidi::RawEvent e;
            e.sample = (juce::int64) std::llround (seconds * r.sampleRate);
            e.size = 3;
            e.data[0] = (juce::uint8) b0;
            e.data[1] = (juce::uint8) b1;
            e.data[2] = (juce::uint8) b2;
            r.events.push_back (e);
        };
        const auto note = [&] (double seconds, int number, int velocity)
        {
            add (seconds, 0x99, number, velocity);
            add (seconds + 0.03, 0x89, number, 0);
        };

        add (beatTime (3.5), 0xb9, 4, 126);   // 하이햇 닫힘

        for (int k = 4; k < 28; ++k)
        {
            note (beatTime (k), 42, 70);                            // 몸통(박)

            if (k == 20)
            {
                add (beatTime (k + 0.5) - 0.05, 0xb9, 4, 0);        // 열고
                note (beatTime (k + 0.5), 46, 80);                  // 열린 하이햇
                add (beatTime (k + 1.0) - 0.02, 0xb9, 4, 126);      // 다시 닫음
            }
            else
            {
                note (beatTime (k + 0.5), 79, 65);                  // 엣지(반 박)
            }

            if (k % 2 == 0)
                note (beatTime (k), 36, 100);
            else
                note (beatTime (k) + 0.015, 38, 95);                // 스네어는 15ms 늦게
        }

        note (beatTime (24.25), 15, 90);   // 탐 1 림
        note (beatTime (25.25), 60, 90);   // GM으로 못 바꾸는 음(늘 원래대로 남아야 한다)

        std::stable_sort (r.events.begin(), r.events.end(), [] (const auto& a, const auto& b) { return a.sample < b.sample; });

        takemidi::Written written;
        juce::String e;
        const bool wrote = takemidi::writeTakeMidi (r, folder, written, e);
        const auto grid = takemidi::makeGrid (r);
        const double downTick = grid.beatAt (beatTime (5)) * takemidi::ppq;   // 녹음 안의 첫 마디 첫 박
        const double barTicks = 4.0 * takemidi::ppq;
        const bool aligned = std::abs (downTick - barTicks * std::round (downTick / barTicks)) < 1.0;

        log ("take midi write: " + juce::String (wrote ? "ok" : "FAILED " + e) + ", " + juce::String (written.bpm, 1) + " BPM "
             + juce::String (written.beatsPerBar) + "/4, from song " + juce::String ((int) written.fromSong) + ", start "
             + juce::String (written.startSeconds, 3) + " s, notes " + juce::String (written.notes) + ", first downbeat tick "
             + juce::String (downTick, 1) + (aligned ? " ok" : " MISALIGNED"));

        TakeInfo info;
        info.sampleRate = r.sampleRate;
        info.lengthSamples = r.lengthSamples;
        info.hasMidi = true;
        info.midiStart = written.startSeconds;
        info.midiBrand = (int) r.brand;
        info.midiBeatsPerBar = written.beatsPerBar;
        info.midiFromSong = written.fromSong;

        // 고치지 않은 연주.mid를 그대로 불러오면 원래 신호(엣지·림 포함)와 같아야 한다
        {
            takemidi::Rebuilt same;
            const bool ok = takemidi::rebuild (folder, info, TakeFiles::midiIn (folder), same, e);
            int found = 0, total = 0;

            for (const auto& a : r.events)
            {
                if ((a.data[0] & 0xf0) != 0x90 || a.data[2] == 0)
                    continue;

                ++total;

                for (const auto& b : same.events)
                {
                    if ((b.data[0] & 0xf0) == 0x90 && b.data[2] > 0 && b.data[1] == a.data[1] && b.data[2] == a.data[2]
                        && std::abs ((double) (b.sample - a.sample)) < 0.001 * r.sampleRate)
                    {
                        ++found;
                        break;
                    }
                }
            }

            log ("take midi unchanged: " + juce::String (ok ? "ok" : "FAILED " + e) + ", kept " + juce::String (same.kept) + " moved "
                 + juce::String (same.moved) + " added " + juce::String (same.added) + " removed " + juce::String (same.removed)
                 + ", notes back " + juce::String (found) + "/" + juce::String (total)
                 + (found == total && same.moved == 0 && same.added == 0 && same.removed == 0 ? " ok" : " MISMATCH"));
        }

        // 이지드러머에서 고친 것처럼: 다른 PPQ(480), 8분음표로 박자 맞춤, 킥 하나 지움, 16분 고스트 스네어 하나 더함
        juce::MidiFile gmFile;
        {
            juce::FileInputStream in (TakeFiles::midiIn (folder));
            gmFile.readFrom (in);
        }

        const auto makeEdited = [&] (double shiftBeats, const juce::File& out)
        {
            juce::MidiFile edited;
            edited.setTicksPerQuarterNote (480);
            juce::MidiMessageSequence notes;
            bool droppedKick = false;

            for (int i = 0; i < gmFile.getNumTracks(); ++i)
            {
                for (const auto* holder : *gmFile.getTrack (i))
                {
                    const auto& m = holder->message;

                    if (! m.isNoteOn())
                        continue;

                    if (m.getNoteNumber() == 36 && ! droppedKick)
                    {
                        droppedKick = true;
                        continue;
                    }

                    const double beat = std::round (m.getTimeStamp() / takemidi::ppq * 2.0) / 2.0 + shiftBeats;
                    notes.addEvent (juce::MidiMessage::noteOn (10, m.getNoteNumber(), m.getVelocity()), beat * 480.0);
                    notes.addEvent (juce::MidiMessage::noteOff (10, m.getNoteNumber()), beat * 480.0 + 30.0);
                }
            }

            const double ghostBeat = std::round (grid.beatAt (beatTime (10.75)) * 4.0) / 4.0 + shiftBeats;
            notes.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 25), ghostBeat * 480.0);
            notes.addEvent (juce::MidiMessage::noteOff (10, 38), ghostBeat * 480.0 + 30.0);
            notes.sort();
            notes.updateMatchedPairs();
            edited.addTrack (notes);
            out.deleteFile();
            juce::FileOutputStream stream (out);
            edited.writeTo (stream, 1);
            return ghostBeat - shiftBeats;
        };

        const auto describe = [&] (const takemidi::Rebuilt& q, double ghostBeat)
        {
            // 스네어는 박에 맞았는지, 엣지·림·GM에 없는 음은 그대로인지, 열린 하이햇 바로 앞 페달 값이 0인지
            int snaresOnBeat = 0, snares = 0, pedalOpen = -1, ghostOk = 0;

            for (size_t i = 0; i < q.events.size(); ++i)
            {
                const auto& ev = q.events[i];

                if ((ev.data[0] & 0xf0) != 0x90 || ev.data[2] == 0)
                    continue;

                const double seconds = (double) ev.sample / r.sampleRate;
                const double beat = grid.beatAt (seconds);

                if (ev.data[1] == 38 && ev.data[2] == 25)
                    ghostOk = std::abs (beat - ghostBeat) * 0.5 < 0.001 ? 1 : 0;
                else if (ev.data[1] == 38)
                {
                    ++snares;
                    snaresOnBeat += std::abs (beat - std::round (beat)) * 0.5 < 0.001 ? 1 : 0;
                }

                if (ev.data[1] == 46 && i > 0 && (q.events[i - 1].data[0] & 0xf0) == 0xb0 && q.events[i - 1].data[1] == 4)
                    pedalOpen = q.events[i - 1].data[2];
            }

            return "kept " + juce::String (q.kept) + " moved " + juce::String (q.moved) + " added " + juce::String (q.added)
                 + " removed " + juce::String (q.removed) + " ignored " + juce::String (q.ignored) + " bar offset " + juce::String (q.barOffset)
                 + ", snares on beat " + juce::String (snaresOnBeat) + "/" + juce::String (snares)
                 + ", edges " + juce::String (countNoteOns (q.events, 79)) + "/" + juce::String (countNoteOns (r.events, 79))
                 + ", tom rim " + juce::String (countNoteOns (q.events, 15)) + ", unmapped " + juce::String (countNoteOns (q.events, 60))
                 + ", open hat pedal " + juce::String (pedalOpen) + ", ghost " + juce::String (ghostOk);
        };

        {
            const auto edited = folder.getChildFile ("edited.mid");
            const double ghostBeat = makeEdited (0.0, edited);
            takemidi::Rebuilt q;
            const bool ok = takemidi::rebuild (folder, info, edited, q, e);
            const bool expected = ok && q.added == 1 && q.removed == 1 && q.moved == 13 && countNoteOns (q.events, 79) == countNoteOns (r.events, 79)
                               && countNoteOns (q.events, 15) == 1 && countNoteOns (q.events, 60) == 1;
            log ("take midi quantized: " + juce::String (ok ? "ok" : "FAILED " + e) + ", " + describe (q, ghostBeat)
                 + (expected ? "  [as expected]" : "  [UNEXPECTED: want moved 13 (12 snares + rim), added 1, removed 1, edges/rim/unmapped kept, pedal 0, ghost 1]"));
        }

        {
            const auto edited = folder.getChildFile ("edited shifted.mid");
            const double ghostBeat = makeEdited (8.0, edited);   // 2마디 뒤에 놓았다가 끌어낸 MIDI
            takemidi::Rebuilt q;
            const bool ok = takemidi::rebuild (folder, info, edited, q, e);
            log ("take midi shifted 2 bars: " + juce::String (ok ? "ok" : "FAILED " + e) + ", " + describe (q, ghostBeat)
                 + (ok && q.barOffset == -2 && q.added == 1 && q.removed == 1 ? "  [as expected]" : "  [UNEXPECTED: want bar offset -2]"));
        }

        // 앱 안 박자 맞추기(quantizeRaw): 8분음표 100% → 15ms 늦던 스네어가 격자에 붙는다. 50% → 절반(약 7.5ms)만 간다.
        const auto snareOffGrid = [&] (const takemidi::Rebuilt& q)
        {
            double worst = 0.0;

            for (const auto& ev : q.events)
            {
                if (ev.size != 3 || (ev.data[0] & 0xf0) != 0x90 || ev.data[2] == 0 || ev.data[1] != 38)
                    continue;

                const double beat = grid.beatAt ((double) ev.sample / r.sampleRate);
                worst = juce::jmax (worst, std::abs (beat * 2.0 - std::round (beat * 2.0)) * 0.25);   // 8분 격자에서 벗어난 초(120BPM)
            }

            return worst * 1000.0;
        };

        {
            takemidi::Rebuilt q;
            const bool ok = takemidi::quantizeRaw (TakeFiles::rawMidiIn (folder), info, 0.5, 1.0, q, e);
            const double off = snareOffGrid (q);
            log ("take midi quantize 8th 100%: " + juce::String (ok ? "ok" : "FAILED " + e) + ", kept " + juce::String (q.kept)
                 + " moved " + juce::String (q.moved) + ", snare off grid " + juce::String (off, 2) + " ms, edges "
                 + juce::String (countNoteOns (q.events, 79)) + ", unmapped " + juce::String (countNoteOns (q.events, 60))
                 + (ok && q.moved == 13 && off < 2.0 && countNoteOns (q.events, 79) == countNoteOns (r.events, 79)
                        ? "  [as expected]" : "  [UNEXPECTED: want moved 13 (12 snares + tom rim), snares within 2 ms, edges kept]"));
        }

        {
            takemidi::Rebuilt q;
            const bool ok = takemidi::quantizeRaw (TakeFiles::rawMidiIn (folder), info, 0.5, 0.5, q, e);
            const double off = snareOffGrid (q);
            log ("take midi quantize 8th 50%: " + juce::String (ok ? "ok" : "FAILED " + e) + ", moved " + juce::String (q.moved)
                 + ", snare off grid " + juce::String (off, 2) + " ms"
                 + (ok && off > 5.0 && off < 10.0 ? "  [as expected: about half of 15 ms]" : "  [UNEXPECTED: want about 7.5 ms]"));
        }

        {
            // 세기 0 = '지금 킷 소리로 다시 만들기': 아무 음도 옮기지 않아야 한다
            takemidi::Rebuilt q;
            const bool ok = takemidi::quantizeRaw (TakeFiles::rawMidiIn (folder), info, 0.5, 0.0, q, e);
            log ("take midi rerender (strength 0): " + juce::String (ok ? "ok" : "FAILED " + e) + ", kept " + juce::String (q.kept)
                 + " moved " + juce::String (q.moved)
                 + (ok && q.moved == 0 && q.kept == 73 ? "  [as expected]" : "  [UNEXPECTED: want kept 73 moved 0]"));
        }

        {
            // 박자를 맞춘 뒤 킷만 바꿔 다시 만들면 고친 박자가 유지돼야 한다:
            // 박자 맞추기 결과 → 연주 (고친 뒤).mid로 저장 → 그 파일을 세기 0으로 읽어도 스네어가 격자 위에 남는지
            takemidi::Rebuilt quantized, again;
            const auto current = folder.getChildFile (TakeFiles::currentMidi());
            const bool ok = takemidi::quantizeRaw (TakeFiles::rawMidiIn (folder), info, 0.5, 1.0, quantized, e)
                         && takemidi::writeRawFormat (quantized.events, info, TakeFiles::rawMidiIn (folder), current, e)
                         && takemidi::quantizeRaw (current, info, 0.5, 0.0, again, e);
            const double off = ok ? snareOffGrid (again) : -1.0;
            log ("take midi kit rebuild keeps timing: " + juce::String (ok ? "ok" : "FAILED " + e)
                 + ", snare off grid " + juce::String (off, 2) + " ms, moved " + juce::String (ok ? again.moved : -1)
                 + (ok && off >= 0.0 && off < 2.0 && again.moved == 0 ? "  [as expected]" : "  [UNEXPECTED: want on grid, moved 0]"));
        }

        {
            // 고스트를 먼저 넣고 박자를 맞춰도 고스트가 살아 있어야 한다(둘 다 지금 상태의 연주 기준):
            // 고스트 넣기 → 연주 (고친 뒤).mid로 저장 → 16분음표 100% 맞추기 → 약한 스네어 24개 유지, 늦던 스네어 12개만 이동
            takemidi::Rebuilt withGhosts, then;
            const auto current = folder.getChildFile (TakeFiles::currentMidi());
            const bool ok = takemidi::addGhosts (TakeFiles::rawMidiIn (folder), info, 1, withGhosts, e)
                         && takemidi::writeRawFormat (withGhosts.events, info, TakeFiles::rawMidiIn (folder), current, e)
                         && takemidi::quantizeRaw (current, info, 0.25, 1.0, then, e);
            int quiet = 0;

            for (const auto& ev : then.events)
                if (ev.size == 3 && (ev.data[0] & 0xf0) == 0x90 && ev.data[2] > 0 && ev.data[1] == 38 && ev.data[2] <= 45)
                    ++quiet;

            log ("take midi ghosts then quantize: " + juce::String (ok ? "ok" : "FAILED " + e)
                 + ", quiet snares " + juce::String (quiet) + " (added " + juce::String (withGhosts.added) + ") moved " + juce::String (ok ? then.moved : -1)
                 + (ok && quiet == withGhosts.added && then.moved >= 12 && then.moved <= 12 + withGhosts.added
                        ? "  [as expected]" : "  [UNEXPECTED: want all ghosts kept, 12 snares + off-grid ghosts moved]"));
            current.deleteFile();
        }

        {
            // 고스트 노트 넣기(보통): 세게 친 스네어 앞뒤 빈 16분 자리에 약한 스네어가 더해지고, 기존 음은 그대로여야 한다
            takemidi::Rebuilt g;
            const bool ok = takemidi::addGhosts (TakeFiles::rawMidiIn (folder), info, 1, g, e);
            int quiet = 0, loud = 0;

            for (const auto& ev : g.events)
            {
                if (ev.size != 3 || (ev.data[0] & 0xf0) != 0x90 || ev.data[2] == 0 || ev.data[1] != 38)
                    continue;

                (ev.data[2] <= 45 ? quiet : loud)++;
            }

            log ("take midi ghosts: " + juce::String (ok ? "ok" : "FAILED " + e) + ", kept " + juce::String (g.kept)
                 + " added " + juce::String (g.added) + ", quiet snares " + juce::String (quiet) + " loud " + juce::String (loud)
                 + (ok && g.kept == 74 && g.added >= 12 && g.added <= 60 && quiet == g.added && loud == 12
                        ? "  [as expected]" : "  [UNEXPECTED: want kept 74, 12~60 quiet ghosts, loud 12]"));
        }

        folder.deleteRecursively();
    }

    // '원곡 드럼 분리' 재사용(cutAlignedRestForTake): 이미 분리해 둔 곡을 녹음 자리에 맞춰 잘라 쓰는지.
    // 가짜 곡(사인 둘 = 밴드, 0.5초마다 잡음 버스트 = 드럼)을 만들어, 무음 0.3초 + 곡 3~15초를 녹음이라 치고 확인한다.
    void testStemReuse()
    {
        const auto folder = report.getSiblingFile ("selftest-stem-reuse");
        folder.deleteRecursively();
        const auto cache = folder.getChildFile ("cache");
        cache.createDirectory();

        const double sr = 44100.0;
        const int songN = (int) (20.0 * sr);
        std::vector<float> restS ((size_t) songN), drumS ((size_t) songN), origS ((size_t) songN);
        juce::Random random (4242);

        for (int i = 0; i < songN; ++i)
        {
            const double time = i / sr;
            restS[(size_t) i] = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * time)
                              + 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 331.0 * time);
        }

        const int burst = (int) (0.03 * sr);

        for (int k = 0; k < 40; ++k)
            for (int i = 0; i < burst && k * 22050 + i < songN; ++i)
                drumS[(size_t) (k * 22050 + i)] = 0.5f * (random.nextFloat() * 2.0f - 1.0f) * (1.0f - (float) i / (float) burst);

        for (int i = 0; i < songN; ++i)
            origS[(size_t) i] = restS[(size_t) i] + drumS[(size_t) i];

        const auto writeStereo = [] (const juce::File& file, const std::vector<float>& signal, int offset, int count, int silence, float gain)
        {
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

            if (stream == nullptr)
                return false;

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2)
                                                 .withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)));

            if (writer == nullptr)
                return false;

            juce::AudioBuffer<float> buffer (2, silence + count);
            buffer.clear();

            for (int i = 0; i < count; ++i)
            {
                const float v = gain * signal[(size_t) (offset + i)];
                buffer.setSample (0, silence + i, v);
                buffer.setSample (1, silence + i, v);
            }

            return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        };

        const int offset = (int) (3.0 * sr), takeCount = (int) (12.0 * sr), prefix = (int) (0.3 * sr);
        const auto takeFile = folder.getChildFile ("take song.wav");
        bool wrote = writeStereo (songcache::original (cache), origS, 0, songN, 0, 1.0f)
                  && writeStereo (songcache::rest (cache), restS, 0, songN, 0, 1.0f)
                  && writeStereo (songcache::drums (cache), drumS, 0, songN, 0, 1.0f)
                  && writeStereo (takeFile, origS, offset, takeCount, prefix, 1.0f);

        if (! wrote)
        {
            log ("stem reuse: FAILED writing test files");
            folder.deleteRecursively();
            return;
        }

        const auto restOut = folder.getChildFile ("rest out.wav");
        juce::String detail;
        const bool ok = cutAlignedRestForTake (takeFile, cache, restOut, detail);
        double worstDrum = -1.0;
        float prefixPeak = 0.0f;

        if (ok)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (restOut));

            if (reader != nullptr)
            {
                const int n = (int) reader->lengthInSamples;
                juce::AudioBuffer<float> rest (2, n);
                reader->read (&rest, 0, n, 0, true, true);

                // 녹음 − 결과 = 원곡 드럼이어야 한다. 무음 앞부분과 (판정 단위가 걸치는) 경계 한 칸은 빼고 본다.
                const int checkFrom = ((prefix / 11025) + 1) * 11025 + 441;
                worstDrum = 0.0;

                for (int i = 0; i < juce::jmin (n, prefix + takeCount); ++i)
                {
                    const float take = i < prefix ? 0.0f : origS[(size_t) (offset + i - prefix)];
                    const float computed = take - rest.getSample (0, i);

                    if (i < prefix)
                        prefixPeak = juce::jmax (prefixPeak, std::abs (computed));
                    else if (i >= checkFrom)
                        worstDrum = juce::jmax (worstDrum, (double) std::abs (computed - drumS[(size_t) (offset + i - prefix)]));
                }
            }
        }

        log ("stem reuse: " + juce::String (ok ? "ok" : "FAILED") + " (" + detail + "), drum error peak "
             + juce::String (worstDrum, 4) + ", silent prefix peak " + juce::String (prefixPeak, 4)
             + (ok && worstDrum >= 0.0 && worstDrum < 1.0e-3 && prefixPeak < 1.0e-3 ? " ok" : " MISMATCH"));

        // 소리가 다르면(1.3배 크게 녹음된 셈) 재사용하지 않고 물러나야 한다(그냥 Demucs로 가게).
        const auto louder = folder.getChildFile ("take louder.wav");
        juce::String detail2;
        const bool refused = ! writeStereo (louder, origS, offset, takeCount, prefix, 1.3f)
                          || ! cutAlignedRestForTake (louder, cache, folder.getChildFile ("rest2.wav"), detail2);
        log ("stem reuse louder take: " + juce::String (refused ? "refused ok" : "ACCEPTED (MISMATCH)") + " (" + detail2 + ")");

        folder.deleteRecursively();
    }

    // 녹음할 때 패드 신호(드럼 모듈 경로)가 연주 MIDI로 남는지, 그 MIDI로 드럼을 다시 만들면 녹음과 같은 자리에 소리가 나는지
    void testMidiRecording()
    {
        const auto folder = report.getSiblingFile ("selftest-midi-recording");
        folder.deleteRecursively();
        juce::String error;
        float d = 0.0f, m = 0.0f;

        if (! engine->startRecording (folder, error))
        {
            log ("midi recording: FAILED " + error);
            return;
        }

        render (0.2, d, m);
        float loudest = 0.0f;

        for (int i = 0; i < 6; ++i)
        {
            const int number = i % 2 == 0 ? 36 : 38;
            engine->injectMidiForTest (juce::MidiMessage::noteOn (10, number, (juce::uint8) 100));
            render (0.05, d, m);
            loudest = juce::jmax (loudest, d);
            engine->injectMidiForTest (juce::MidiMessage::noteOff (10, number));
            render (0.35, d, m);
            loudest = juce::jmax (loudest, d);
        }

        log ("midi recording: drum peak while playing " + dbText (loudest));

        engine->stopRecording();
        const auto take = engine->getLastTakeFolder();
        TakeInfo info;
        info.load (take);

        juce::MidiFile raw;
        int rawNotes = 0;

        if (juce::FileInputStream in (TakeFiles::rawMidiIn (take)); in.openedOk() && raw.readFrom (in))
            for (int t = 0; t < raw.getNumTracks(); ++t)
                for (const auto* holder : *raw.getTrack (t))
                    rawNotes += holder->message.isNoteOn() ? 1 : 0;

        log ("midi recording: raw notes " + juce::String (rawNotes) + "/6, gm file " + juce::String (TakeFiles::midiIn (take).existsAsFile() ? "yes" : "NO")
             + ", info midi " + juce::String ((int) info.hasMidi) + ", start " + juce::String (info.midiStart, 3) + " s, "
             + juce::String (info.midiBpm, 1) + " BPM" + (rawNotes == 6 && info.hasMidi ? " ok" : " MISMATCH"));

        // 플러그인이 있으면: 고치지 않은 MIDI로 다시 만든 드럼이 녹음된 드럼과 같은 자리에서 시작하는지
        if (engine->getPlugin() != nullptr && info.hasMidi)
        {
            takemidi::Rebuilt same;
            juce::String e;
            const auto rendered = take.getChildFile ("rendered.wav");
            const bool ok = takemidi::rebuild (take, info, TakeFiles::midiIn (take), same, e)
                         && engine->renderDrumsFromMidi (same.events, info.lengthSamples, info.sampleRate, rendered, nullptr, e);

            juce::String onsetDebug;
            const auto onsets = [&onsetDebug] (const juce::File& file)
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::Array<double> found;

                if (reader == nullptr)
                    return found;

                juce::AudioBuffer<float> buffer (2, (int) reader->lengthInSamples);
                const bool read = reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
                const float peak = buffer.getMagnitude (0, buffer.getNumSamples());

                if (! read)
                    onsetDebug << " [" << file.getFileName() << " not read]";

                // 소리 시작: 10%를 넘은 샘플 중 앞서 10%를 넘은 지 150ms가 지난 것
                const int gap = (int) (0.15 * reader->sampleRate);
                int lastLoud = -gap - 1;

                for (int i = 0; i < buffer.getNumSamples(); ++i)
                {
                    const float v = juce::jmax (std::abs (buffer.getSample (0, i)), std::abs (buffer.getSample (1, i)));

                    if (v > peak * 0.1f)
                    {
                        if (i - lastLoud > gap)
                            found.add (i * 1000.0 / reader->sampleRate);

                        lastLoud = i;
                    }
                }

                return found;
            };

            const auto a = onsets (TakeFiles::drumIn (take));
            const auto b = onsets (rendered);

            const auto peakOf = [] (const juce::File& file)
            {
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                juce::Range<float> range[2];

                if (reader == nullptr)
                    return -1.0f;

                reader->readMaxLevels (0, reader->lengthInSamples, range, 2);
                return juce::jmax (std::abs (range[0].getStart()), std::abs (range[0].getEnd()));
            };

            log ("midi re-render: recorded drum peak " + dbText (peakOf (TakeFiles::drumIn (take))) + ", rendered peak "
                 + dbText (peakOf (rendered)) + ", events " + juce::String ((int) same.events.size()));
            double worst = 0.0;

            for (int i = 0; i < juce::jmin (a.size(), b.size()); ++i)
                worst = juce::jmax (worst, std::abs (a[i] - b[i]));

            log ("midi re-render: " + juce::String (ok ? "ok" : "FAILED " + e) + ", onsets recorded " + juce::String (a.size())
                 + " rendered " + juce::String (b.size()) + ", worst difference " + juce::String (worst, 2) + " ms" + onsetDebug
                 + (ok && a.size() == b.size() && worst < 1.0 ? " ok" : " MISMATCH"));

            // 박자 맞추기 → '연주 (고친 뒤).mid' 저장 → 킷 재렌더: 맞춘 박이 실제 소리에서도 유지되는지.
            // (여기 녹음의 박은 멋대로라 120BPM 격자로 옮겨지므로, 맞춘 소리는 원래 녹음과 자리가 달라야 한다)
            {
                takemidi::Rebuilt quantized, kit;
                const auto current = take.getChildFile (TakeFiles::currentMidi());
                const auto renderedQuantized = take.getChildFile ("rendered quantized.wav");
                const auto renderedKit = take.getChildFile ("rendered kit.wav");
                juce::String e2;
                const bool ok2 = takemidi::quantizeRaw (TakeFiles::rawMidiIn (take), info, 0.5, 1.0, quantized, e2)
                              && engine->renderDrumsFromMidi (quantized.events, info.lengthSamples, info.sampleRate, renderedQuantized, nullptr, e2)
                              && takemidi::writeRawFormat (quantized.events, info, TakeFiles::rawMidiIn (take), current, e2)
                              && takemidi::quantizeRaw (current, info, 0.5, 0.0, kit, e2)
                              && engine->renderDrumsFromMidi (kit.events, info.lengthSamples, info.sampleRate, renderedKit, nullptr, e2);

                const auto afterQuantize = onsets (renderedQuantized);
                const auto afterKit = onsets (renderedKit);
                double kitDifference = 0.0, movedByQuantize = 0.0;

                for (int i = 0; i < juce::jmin (afterQuantize.size(), afterKit.size()); ++i)
                    kitDifference = juce::jmax (kitDifference, std::abs (afterQuantize[i] - afterKit[i]));

                for (int i = 0; i < juce::jmin (a.size(), afterQuantize.size()); ++i)
                    movedByQuantize = juce::jmax (movedByQuantize, std::abs (a[i] - afterQuantize[i]));

                log ("midi kit chain: " + juce::String (ok2 ? "ok" : "FAILED " + e2) + ", onsets quantized " + juce::String (afterQuantize.size())
                     + " kit-rebuilt " + juce::String (afterKit.size()) + ", quantized vs kit worst " + juce::String (kitDifference, 2)
                     + " ms, moved by quantize " + juce::String (movedByQuantize, 1) + " ms"
                     + (ok2 && afterKit.size() == afterQuantize.size() && kitDifference < 1.0 && movedByQuantize > 5.0
                            ? " ok" : " MISMATCH"));
            }
        }

        if (! juce::SystemStats::getEnvironmentVariable ("DVH_KEEP_TEST_FILES", {}).isNotEmpty())
            folder.deleteRecursively();
    }

    // 녹음(믹스·드럼·원곡) → 테이크 불러오기 → 볼륨·타이밍·구간을 바꿔 저장 → 다시 읽어 확인
    void testRecording()
    {
        const auto folder = report.getSiblingFile ("selftest-recording");
        folder.deleteRecursively();
        juce::String error;
        float d = 0.0f, m = 0.0f;

        if (! engine->startRecording (folder, error))
        {
            log ("record: FAILED " + error);
            return;
        }

        hit (36);
        render (1.0, d, m);
        engine->stopRecording();

        const auto take = engine->getLastTakeFolder();
        const auto size = [&] (const juce::String& name) { return juce::String (take.getChildFile (name).getSize()); };

        log ("record: " + juce::String (engine->getRecordedSeconds(), 2) + " s in '" + take.getFileName() + "': mix "
             + size (TakeFiles::mix()) + ", drum " + size (TakeFiles::drum()) + ", song " + size (TakeFiles::song())
             + ", info " + size (TakeFiles::info()) + " bytes, song drums file "
             + (TakeFiles::songDrumsIn (take).existsAsFile() ? "yes" : "no (expected: song not separated)"));

        juce::String takeError;
        auto player = TakePlayer::load (take, takeError);

        if (player == nullptr)
        {
            log ("take-load: FAILED " + takeError);
            folder.deleteRecursively();
            return;
        }

        const auto peakOf = [] (const std::vector<float>& peaks)
        {
            float peak = 0.0f;

            for (auto p : peaks)
                peak = juce::jmax (peak, p);

            return peak;
        };

        log ("take-load: " + juce::String (player->getLength()) + " samples @ " + juce::String (player->getSampleRate(), 0)
             + " Hz, recorded volumes drum " + juce::String (player->info.drumVolume, 2) + " / song " + juce::String (player->info.songVolume, 2)
             + ", drum stem peak " + dbText (peakOf (player->getDrumPeaks())) + ", song stem peak " + dbText (peakOf (player->getSongPeaks())));

        player->drumGain = 1.0f;
        player->songGain = 0.25f;
        player->offsetSamples = -(int) (player->getSampleRate() * 0.01);   // 드럼을 10ms 앞으로
        player->trimStart = player->getLength() / 10;
        player->trimEnd = player->getLength() - player->getLength() / 10;

        const auto out = take.getChildFile (lang::pick ("완성.wav", "final.wav"));
        juce::String exportError;
        const bool ok = player->exportMix (out, TakePlayer::ExportFormat::wav24, exportError);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (ok ? formats.createReaderFor (out) : nullptr);

        if (reader != nullptr)
        {
            juce::Range<float> range[2];
            reader->readMaxLevels (0, reader->lengthInSamples, range, 2);
            const float peak = juce::jmax (std::abs (range[0].getStart()), std::abs (range[0].getEnd()));
            log ("export: ok, " + juce::String (reader->lengthInSamples) + " samples (expected "
                 + juce::String (player->trimEnd.load() - player->trimStart.load()) + "), peak " + dbText (peak));
        }
        else
        {
            log ("export: FAILED " + exportError);
        }

        // 다듬기 창의 '들어보기'와 같은 경로: 엔진 출력에 더해지고 위치가 움직여야 한다.
        engine->pause();
        render (0.3, d, m);
        log ("after pause, no preview: drum " + dbText (d) + ", mix " + dbText (m) + ", song playing " + juce::String ((int) engine->isPlaying()));
        engine->setPreview (player);
        player->seek (player->trimStart.load());
        player->playing = true;
        const auto before = player->position.load();
        render (0.3, d, m);
        log ("preview: mix " + dbText (m) + " (expected about " + dbText (0.25f * 0.25f) + "), moved "
             + juce::String (player->position.load() - before) + " samples, still playing " + juce::String ((int) player->playing.load()));
        engine->setPreview (nullptr);

        reader.reset();
        player.reset();
        folder.deleteRecursively();
    }

    // 다듬기 창 미리 듣기가 장치 샘플레이트와 상관없이 녹음 속도 그대로인지, 완성본 MP3가 제대로 써지는지.
    // 44.1kHz로 녹음된 1kHz 사인파를 48kHz·44.1kHz 장치로 재생해 주파수와 진행 속도를 잰다.
    void testTakeRates()
    {
        const auto folder = report.getSiblingFile ("selftest-take-rates");
        folder.deleteRecursively();
        folder.createDirectory();

        constexpr double recordRate = 44100.0, toneHz = 1000.0;
        const int length = (int) (recordRate * 4.0);

        const auto writeWav = [&] (const juce::String& name, bool tone)
        {
            juce::AudioBuffer<float> buffer (2, length);
            buffer.clear();

            if (tone)
            {
                for (int i = 0; i < length; ++i)
                {
                    const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * toneHz * i / recordRate);
                    buffer.setSample (0, i, v);
                    buffer.setSample (1, i, v);
                }
            }

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = folder.getChildFile (name).createOutputStream();
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (recordRate).withNumChannels (2).withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            return writer != nullptr && writer->writeFromAudioSampleBuffer (buffer, 0, length);
        };

        if (! writeWav (TakeFiles::song(), true) || ! writeWav (TakeFiles::drum(), false))
        {
            log ("take-rates: FAILED to write test take");
            return;
        }

        juce::String error;
        auto player = TakePlayer::load (folder, error);

        if (player == nullptr)
        {
            log ("take-rates: FAILED " + error);
            return;
        }

        player->songGain = 1.0f;
        player->drumGain = 1.0f;

        // 위로 지나는 0점 사이 간격으로 주파수를 잰다(선형 보간으로 소수 위치까지).
        const auto measure = [] (const std::vector<float>& x, double rate)
        {
            double first = -1.0, last = -1.0;
            int crossings = 0;

            for (size_t i = 1; i < x.size(); ++i)
            {
                if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                {
                    const double t = (double) (i - 1) + (double) (-x[i - 1] / (x[i] - x[i - 1]));

                    if (first < 0.0) first = t;
                    last = t;
                    ++crossings;
                }
            }

            return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
        };

        for (const double deviceRate : { 48000.0, 44100.0, 96000.0 })
        {
            player->seek ((juce::int64) (player->getSampleRate() * 0.5));
            player->playing = true;
            const auto before = player->position.load();
            const int n = (int) deviceRate;   // 장치로 1초
            std::vector<float> left ((size_t) n, 0.0f), right ((size_t) n, 0.0f);

            for (int done = 0; done < n; done += 480)
            {
                const int count = juce::jmin (480, n - done);
                player->renderAdding (left.data() + done, right.data() + done, count, deviceRate);
            }

            const double hz = measure (left, deviceRate);
            const double moved = (double) (player->position.load() - before);
            const bool good = std::abs (hz - toneHz) < 0.5 && std::abs (moved - recordRate) < 2.0;
            log ("take-rates device " + juce::String (deviceRate, 0) + " Hz: tone " + juce::String (hz, 2) + " Hz (expected 1000), moved "
                 + juce::String (moved, 0) + " recorded samples per second (expected 44100) " + (good ? "OK" : "CHECK"));
            player->playing = false;
        }

        // 완성본: 같은 구간을 WAV와 MP3로 저장해 크기·길이·소리를 비교한다.
        player->trimStart = (juce::int64) (recordRate * 0.5);
        player->trimEnd = (juce::int64) (recordRate * 3.5);
        player->songGain = 0.5f;

        const auto wavFile = folder.getChildFile ("final.wav"), mp3File = folder.getChildFile ("final.mp3");
        juce::String wavError, mp3Error;
        const bool wavOk = player->exportMix (wavFile, TakePlayer::ExportFormat::wav24, wavError);
        const bool mp3Ok = player->exportMix (mp3File, TakePlayer::ExportFormat::mp3, mp3Error);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (mp3Ok ? formats.createReaderFor (mp3File) : nullptr);
        juce::String decoded = "not readable";

        if (reader != nullptr)
        {
            juce::AudioBuffer<float> back (2, (int) reader->lengthInSamples);
            reader->read (&back, 0, back.getNumSamples(), 0, true, true);
            const int middle = back.getNumSamples() / 2;
            std::vector<float> tone ((size_t) juce::jmin (back.getNumSamples() - middle, 44100));
            std::copy (back.getReadPointer (0) + middle, back.getReadPointer (0) + middle + (int) tone.size(), tone.begin());
            decoded = juce::String (reader->sampleRate, 0) + " Hz, " + juce::String ((double) reader->lengthInSamples / reader->sampleRate, 3)
                    + " s (expected 3.000 plus encoder padding), tone " + juce::String (measure (tone, reader->sampleRate), 2)
                    + " Hz, peak " + dbText (back.getMagnitude (0, back.getNumSamples()));
        }

        log ("final-wav: " + (wavOk ? juce::String (wavFile.getSize()) + " bytes" : "FAILED " + wavError));
        log ("final-mp3: " + (mp3Ok ? juce::String (mp3File.getSize()) + " bytes (" + juce::String ((double) wavFile.getSize() / juce::jmax<juce::int64> (1, mp3File.getSize()), 1)
                                          + "x smaller than WAV), decoded " + decoded
                                    : "FAILED " + mp3Error));

        reader.reset();
        player.reset();

        if (juce::SystemStats::getEnvironmentVariable ("DVH_KEEP_TEST_FILES", {}).isEmpty())
            folder.deleteRecursively();
    }

    //==========================================================================
    // 드럼 모듈·드라이버·드럼 VST 알아보기와 MIDI 거르기(장치 없이 이름만으로). 이름 형태는 2026-11-18 조사 기준.
    void testDrumKnowledge()
    {
        const auto check = [this] (const juce::String& label, const juce::String& got, const juce::String& expected)
        {
            log ("drum-knowledge " + label + ": " + (got == expected ? "OK " : "CHECK ") + got + (got == expected ? juce::String() : "  (expected " + expected + ")"));
        };

        const auto module = [] (const juce::String& name, const juce::String& id = {})
        {
            juce::Array<juce::MidiDeviceInfo> inputs;
            inputs.add (juce::MidiDeviceInfo (name, id));
            return identifyDrumModule (inputs);
        };

        juce::StringArray names;
        const auto shown = [] (const DrumModule& m) { return m.isKnown() ? m.displayName() + (m.usbAudio ? "" : " (MIDI only)") : juce::String ("-"); };

        for (const auto* name : { "DTX Drums-1", "TD-17", "5- TD-27", "2 - TD-17", "IN (3- TD-50X)", "TD-1DMK", "V71", "Arturia KeyStep" })
            names.add (shown (module (name)));

        names.add (shown (module ("USB MIDI", "\\\\?\\usb#vid_0582&pid_02e6&mi_00")));
        check ("modules", names.joinIntoString (" | "),
               "Yamaha (DTX Drums-1) | Roland TD-17 | Roland TD-27 | Roland TD-17 | Roland TD-50X | Roland TD-1 (MIDI only) | Roland V71 | - | Roland (USB MIDI)");

        juce::StringArray scores;
        const auto td17 = module ("TD-17");

        for (const auto* driver : { "Yamaha Steinberg USB ASIO", "TD-17", "Roland TD-27", "ASIO4ALL v2", "Realtek ASIO" })
            scores.add (juce::String (drumDriverScore (driver, td17)));

        check ("driver scores for TD-17", scores.joinIntoString (" "), "10 70 30 0 0");

        juce::StringArray plugins;

        for (const auto* file : { "Superior Drummer 3.vst3", "EZdrummer 3.vst3", "Addictive Drums 2.vst3", "SSDSampler5.vst3",
                                  "BFDPlayer.vst3", "MT-PowerDrumKit.vst3", "EZkeys 2.vst3", "Addictive Keys.vst3", "ValhallaRoom.vst3" })
        {
            const auto info = recogniseDrumPlugin (juce::File ("C:\\Test\\VST3").getChildFile (file));
            plugins.add (info.isDrum ? info.displayName + "/" + juce::String (info.rank) : juce::String ("-"));
        }

        check ("plugins", plugins.joinIntoString (" | "),
               "Superior Drummer 3 (Toontrack)/10 | EZdrummer 3 (Toontrack)/20 | Addictive Drums 2 (XLN Audio)/30 | SSD 5 (Steven Slate Drums)/40 | "
               "BFD Player/52 | MT Power Drum Kit/60 | - | - | -");

        juce::StringArray forwarded;
        const juce::uint8 messages[][3] = { { 0x99, 38, 100 }, { 0x89, 38, 64 }, { 0xb9, 4, 90 }, { 0xa9, 49, 127 }, { 0xf8, 0, 0 },
                                             { 0xfe, 0, 0 }, { 0xc9, 5, 0 }, { 0xb9, 0, 125 }, { 0xb9, 32, 1 } };
        const int sizes[] = { 3, 3, 3, 3, 1, 1, 2, 3, 3 };

        for (size_t i = 0; i < std::size (sizes); ++i)
            forwarded.add (MidiInbox::shouldForward (messages[i], sizes[i]) ? "pass" : "drop");

        check ("midi filter (note on, note off, CC4, aftertouch, clock, sensing, program, bank MSB, bank LSB)",
               forwarded.joinIntoString (" "), "pass pass pass pass drop drop drop drop drop");

        // 패드 배치: 받은 음 번호를 바꿔 넘기고(17 -> 57), '넘기지 않음'으로 둔 음은 거른다. 끄면 그대로 넘긴다.
        {
            PadMap map;
            map.rows[12].sources.add (17);   // Crash 2(57)
            map.rows[12].sources.add (16);
            map.rows[1].sources.add (40);    // Snare(38)
            PadMap copy;
            copy.fromString (map.toString());
            check ("pad map text round trip", copy.toString(), map.toString());

            auto table = map.toTable();
            table[20] = MidiInbox::dropNote;
            MidiInbox::MonitorEvent events[16];
            juce::StringArray seen;

            for (const bool on : { true, false })
            {
                while (engine->readMidiMonitor (events, 16) > 0) {}
                engine->setPadMap (table, on);

                for (const int note : { 17, 40, 36, 20 })
                    engine->injectMidiForTest (juce::MidiMessage::noteOn (10, note, (juce::uint8) 100));

                const int n = engine->readMidiMonitor (events, 16);

                for (int i = 0; i < n; ++i)
                    seen.add (juce::String ((int) events[i].data[1]) + (events[i].forwarded ? "" : "x")
                              + (events[i].mappedNote < 128 ? ">" + juce::String ((int) events[i].mappedNote) : juce::String()));

                seen.add ("|");
            }

            check ("pad map (on, then off)", seen.joinIntoString (" "), "17>57 40>38 36 20x | 17 40 36 20 |");
            engine->setPadMap (table, false);
        }

        // 약한 탐 신호 거르기(Yamaha, 세기 9 이하 버림): 탐 헤드(43)·림(20)의 약한 신호만 버리고, 같은 세기라도
        // 하이햇 페달(44)·스네어(38)는 넘긴다. 버린 신호는 패드 맞추기 창의 '마지막 음'으로 세지 않는다. 끄면 모두 넘긴다.
        {
            check ("weak tom table (yamaha 43, 20, 44 / roland 39 / GM 39 at 9)",
                   juce::String ((int) weakTomHitTable (DrumModule::Brand::yamaha, 9)[43]) + " " + juce::String ((int) weakTomHitTable (DrumModule::Brand::yamaha, 9)[20])
                       + " " + juce::String ((int) weakTomHitTable (DrumModule::Brand::yamaha, 9)[44]) + " / " + juce::String ((int) weakTomHitTable (DrumModule::Brand::roland, 9)[39])
                       + " / " + juce::String ((int) weakTomHitTable (DrumModule::Brand::none, 9)[39]),
                   "10 10 0 / 10 / 0");

            MidiInbox::MonitorEvent events[32];
            juce::StringArray seen;
            const std::pair<int, int> hits[] = { { 43, 4 }, { 43, 9 }, { 43, 10 }, { 20, 5 }, { 44, 5 }, { 38, 5 }, { 43, 80 } };

            for (const int level : { 9, 0 })
            {
                while (engine->readMidiMonitor (events, 32) > 0) {}
                engine->setMinVelocity (weakTomHitTable (DrumModule::Brand::yamaha, level));

                for (const auto& [note, velocity] : hits)
                    engine->injectMidiForTest (juce::MidiMessage::noteOn (10, note, (juce::uint8) velocity));

                const int n = engine->readMidiMonitor (events, 32);

                for (int i = 0; i < n; ++i)
                    seen.add (juce::String ((int) events[i].data[1]) + "/" + juce::String ((int) events[i].data[2])
                              + (events[i].weak ? "w" : "") + (events[i].forwarded ? "" : "x"));

                seen.add ("|");
            }

            check ("weak tom hits dropped (level 9, then off)", seen.joinIntoString (" "),
                   "43/4wx 43/9wx 43/10 20/5wx 44/5 38/5 43/80 | 43/4 43/9 43/10 20/5 44/5 38/5 43/80 |");

            engine->setMinVelocity (weakTomHitTable (DrumModule::Brand::yamaha, 9));
            engine->injectMidiForTest (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 90));
            engine->injectMidiForTest (juce::MidiMessage::noteOn (10, 43, (juce::uint8) 3));
            check ("weak tom hit not counted as last note", juce::String ((int) (engine->getLastNoteOn() & 0xff)), "38");
            engine->setMinVelocity (weakTomHitTable (DrumModule::Brand::yamaha, 0));
            while (engine->readMidiMonitor (events, 32) > 0) {}
        }
    }

    //==========================================================================
    // 44.1kHz 32비트 실수 WAV: 110Hz 사인(sineLevel) 위에 한 샘플짜리 펄스(pulse)를 times(초)에 찍는다.
    static void writePulses (const juce::File& file, double seconds, const std::vector<double>& times, float pulse, float sineLevel)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

        if (stream == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                       .withSampleRate (44100.0)
                                                       .withNumChannels (2)
                                                       .withBitsPerSample (32)
                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));

        if (writer == nullptr)
            return;

        juce::AudioBuffer<float> buffer (2, (int) (seconds * 44100.0));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float v = sineLevel * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * i / 44100.0);
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }

        for (double t : times)
        {
            const int i = juce::roundToInt (t * 44100.0);

            if (i < buffer.getNumSamples())
            {
                buffer.addSample (0, i, pulse);
                buffer.addSample (1, i, pulse);
            }
        }

        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // 엔진 출력을 seconds초 받는다(한 번에 chunk 샘플씩, 원곡 미리 읽기가 따라오도록 실제 속도에 가깝게).
    std::vector<float> capture (double seconds, int chunk)
    {
        const int total = (int) (seconds * 44100.0);
        std::vector<float> left ((size_t) total), right ((size_t) total);

        for (int done = 0; done < total && ! threadShouldExit(); done += chunk)
        {
            const int n = juce::jmin (chunk, total - done);
            engine->renderOfflineInto (left.data() + done, right.data() + done, n);
            wait (juce::jmax (1, n * 850 / 44100));
        }

        return left;
    }

    // 조용하다가(0.15초 이상) 소리가 나는 샘플들
    static std::vector<int> onsetsOf (const std::vector<float>& x, float threshold)
    {
        std::vector<int> onsets;
        int quiet = 1 << 30;

        for (int i = 0; i < (int) x.size(); ++i)
        {
            if (std::abs (x[(size_t) i]) > threshold)
            {
                if (quiet >= 6615)
                    onsets.push_back (i);

                quiet = 0;
            }
            else
            {
                ++quiet;
            }
        }

        return onsets;
    }

    // 메트로놈: 재생을 누르면 원곡 박자에 맞춘 4박을 세고 원곡이 정확한 샘플에서 시작해야 한다.
    // 같은 조건으로 두 번 돌려 '클릭만'(원곡 끔)과 '원곡만'(클릭 볼륨 0)을 받아 소리가 시작되는 샘플을 비교한다.
    void testMetronome()
    {
        constexpr double rate = 44100.0;
        const auto songFile = report.getSiblingFile ("selftest-beats.wav");
        std::vector<double> beats;

        for (int k = 0; k < 15; ++k)
            beats.push_back (0.5 + 0.5 * k);   // 120 BPM, 첫 박 0.5초, 4박마다 마디 첫 박

        writePulses (songFile, 8.0, beats, 0.8f, 0.0f);

        juce::String error;

        if (! engine->loadSong (songFile, error))
        {
            log ("metronome: FAILED to load test song " + error);
            return;
        }

        BeatMap map;
        map.bpm = 120.0;
        map.beats = beats;
        map.downbeat.assign (beats.size(), 0);

        for (size_t k = 0; k < beats.size(); k += 4)
            map.downbeat[k] = 1;

        engine->setBeatMap (map);
        engine->metronomeOn = true;
        engine->countInEnabled = true;
        engine->drum.mute = true;

        // 오프라인에서는 따로 도는 오디오 스레드가 없어서, 멈춘 뒤 한 블록을 돌려야 재생기가 멈춤을 받아들인다
        // (그러지 않으면 다음 블록에서 한 블록을 더 읽어 위치가 밀린다. 실제 장치에서는 stop()이 이것을 기다린다).
        const auto settle = [this] { capture (0.05, 512); };

        const auto scenario = [&] (const juce::String& label, double startAt, int chunk, int expectedCountIn)
        {
            std::vector<float> outputs[2];
            int countIn[2] = { 0, 0 };

            for (int pass = 0; pass < 2; ++pass)
            {
                const bool clicksOnly = pass == 0;
                engine->prepareOffline (rate, 512);
                engine->song.mute = clicksOnly;
                engine->metronomeVolume = clicksOnly ? 1.0f : 0.0f;
                engine->setPosition (startAt);
                capture (0.3, 512);   // 음소거 전환이 끝나게

                engine->play();
                countIn[pass] = engine->getCountInSamples();
                outputs[pass] = capture (startAt < 1.0 ? 5.0 : 4.0, chunk);
                engine->pause();
                settle();
            }

            const auto clicks = onsetsOf (outputs[0], 1.0e-6f);
            const auto pulses = onsetsOf (outputs[1], 0.05f);
            const int songStart = countIn[1];

            // 클릭의 첫 샘플은 0이라(감싸는 곡선이 0에서 시작) 소리가 나는 샘플보다 한 샘플 앞이 시작이다.
            juce::String clickText, pulseText, accents;
            float loudest = 0.0f;

            for (int c : clicks)
                for (int i = c; i < juce::jmin ((int) outputs[0].size(), c + 1800); ++i)
                    loudest = juce::jmax (loudest, std::abs (outputs[0][(size_t) i]));

            for (int c : clicks)
            {
                float peak = 0.0f;

                for (int i = c; i < juce::jmin ((int) outputs[0].size(), c + 1800); ++i)
                    peak = juce::jmax (peak, std::abs (outputs[0][(size_t) i]));

                accents << (peak > loudest * 0.85f ? "A" : "n");
                clickText << " " << (c - 1 - songStart);
            }

            int worst = 0, matched = 0;

            for (int p : pulses)
            {
                pulseText << " " << (p - songStart);
                int best = 1 << 30;

                for (int c : clicks)
                    best = std::abs (c - 1 - p) < std::abs (best) ? (c - 1 - p) : best;

                if (std::abs (best) < 1000)
                {
                    ++matched;
                    worst = juce::jmax (worst, std::abs (best));
                }
            }

            // 클릭은 원곡보다 최대 3샘플(0.07ms) 이를 수 있다: JUCE 재생기가 보간용으로 3샘플을 미리 읽어 두어
            // 재생 위치가 그만큼 앞서 보인다. 귀로는 구분할 수 없는 크기라 4샘플까지 통과로 본다.
            const bool ok = countIn[0] == expectedCountIn && countIn[1] == expectedCountIn
                            && matched == (int) pulses.size() && ! pulses.empty() && worst <= 4;

            log ("metronome " + label + ": " + (ok ? "OK" : "CHECK") + " count-in " + juce::String (countIn[0]) + "/" + juce::String (countIn[1])
                 + " smp (expected " + juce::String (expectedCountIn) + " = " + juce::String (expectedCountIn / rate, 3) + " s), clicks "
                 + juce::String ((int) clicks.size()) + ", song pulses " + juce::String ((int) pulses.size()) + ", pulses with a click "
                 + juce::String (matched) + ", worst offset " + juce::String (worst) + " smp");
            log ("  clicks rel. to song start:" + clickText.substring (0, 160));
            log ("  pulses rel. to song start:" + pulseText.substring (0, 160));
            log ("  accents: " + accents);
        };

        // 처음부터: 첫 박(0.5초) 앞이라 박 간격으로 거슬러 센다. 예비박 -2.0 -1.5 -1.0 -0.5초, 0초 클릭, 그다음 원곡 박.
        // 예비박 2초 = 88200샘플 → 블록(512)의 배수로 올림 = 88576. 강박: nAnnn AnnnA… (마디 첫 박 = -1.5초, 0.5초, 2.5초)
        scenario ("from 0.0 s, block 512", 0.0, 512, 88576);
        // 박 사이(3.2초)에서, 블록 크기와 예비박 길이가 맞지 않게(300) → 블록 가운데서 원곡이 시작되는 경로
        // 예비박 1.5·2.0·2.5·3.0초 → 3.2−1.5 = 1.7초 = 74970샘플 → 75264. 강박: nnAnnnAn (2.5초, 4.5초)
        scenario ("from 3.2 s, chunk 300", 3.2, 300, 75264);

        // 클릭 빠르기(2배·절반)와 클릭 시점(+20ms): 처음부터 재생해 클릭만 받고, 원곡 시작 기준 클릭 시각과 높은 클릭 자리를 본다.
        // 원곡 박은 0.5초부터 0.5초 간격, 마디 첫 박은 0.5·2.5초(예비박 자리로는 -1.5초).
        const auto speedScenario = [&] (const juce::String& label, double speed, float offsetMs, double first, double step, const std::vector<double>& accentTimes)
        {
            engine->setBeatMap (map, speed);
            engine->metronomeOffsetMs = offsetMs;
            engine->prepareOffline (rate, 512);
            engine->song.mute = true;
            engine->metronomeVolume = 1.0f;
            engine->setPosition (0.0);
            capture (0.3, 512);

            engine->play();
            const int countIn = engine->getCountInSamples();
            const auto out = capture (5.0, 512);
            engine->pause();
            settle();

            const auto clicks = onsetsOf (out, 1.0e-6f);
            const double limit = ((double) out.size() - countIn - 2000) / rate;   // 이 시각 전의 클릭은 모두 받았어야 한다
            std::vector<double> expected;

            for (double t = first; t + offsetMs * 0.001 < limit; t += step)
                expected.push_back (t + offsetMs * 0.001);

            float loudest = 0.0f;
            std::vector<float> peaks;

            for (int c : clicks)
            {
                float peak = 0.0f;

                for (int i = c; i < juce::jmin ((int) out.size(), c + 1800); ++i)
                    peak = juce::jmax (peak, std::abs (out[(size_t) i]));

                peaks.push_back (peak);
                loudest = juce::jmax (loudest, peak);
            }

            int worst = 0;
            bool accentsRight = true;
            juce::String times, accents;

            for (size_t k = 0; k < clicks.size(); ++k)
            {
                const double t = (double) (clicks[k] - 1 - countIn) / rate;
                const bool accent = peaks[k] > loudest * 0.85f;
                times << " " << juce::String (t, 3);
                accents << (accent ? "A" : "n");

                if (k < expected.size())
                    worst = juce::jmax (worst, juce::roundToInt (std::abs (t - expected[k]) * rate));

                const bool shouldAccent = std::any_of (accentTimes.begin(), accentTimes.end(),
                                                       [&] (double a) { return std::abs (a + offsetMs * 0.001 - t) < 0.01; });
                accentsRight = accentsRight && accent == shouldAccent;
            }

            const bool ok = clicks.size() == expected.size() && worst <= 4 && accentsRight;
            log ("metronome " + label + ": " + (ok ? "OK" : "CHECK") + " clicks " + juce::String ((int) clicks.size()) + " (expected "
                 + juce::String ((int) expected.size()) + "), worst offset " + juce::String (worst) + " smp, accents " + (accentsRight ? "right" : "WRONG"));
            log ("  clicks (s, rel. to song start):" + times.substring (0, 200));
            log ("  accents: " + accents);
        };

        speedScenario ("speed x2", 2.0, 0.0f, -2.0, 0.25, { -1.5, 0.5, 2.5 });
        speedScenario ("speed x0.5", 0.5, 0.0f, -1.5, 1.0, { -1.5, 0.5, 2.5 });
        speedScenario ("offset +20 ms", 1.0, 20.0f, -2.0, 0.5, { -1.5, 0.5, 2.5 });
        // 반 박(0.25초) 앞당기기·늦추기: 원곡 시작 근처의 박이 예비박·원곡 쪽 어디서든 빠지거나 두 번 울리지 않아야 한다.
        speedScenario ("offset -1/2 beat", 1.0, -250.0f, -2.0, 0.5, { -1.5, 0.5, 2.5 });
        speedScenario ("offset +1/2 beat", 1.0, 250.0f, -2.0, 0.5, { -1.5, 0.5, 2.5 });
        speedScenario ("offset -1/4 beat, speed x2", 2.0, -125.0f, -2.0, 0.25, { -1.5, 0.5, 2.5 });
        engine->setBeatMap (map, 1.0);
        engine->metronomeOffsetMs = 0.0f;

        // 예비박 중에 멈추면 원곡이 시작되지 않아야 한다.
        engine->prepareOffline (rate, 512);
        engine->setPosition (1.0);
        engine->play();
        capture (0.5, 512);
        engine->pause();
        settle();
        capture (2.5, 512);
        log ("metronome pause during count-in: playing " + juce::String ((int) engine->isPlaying())
             + ", position " + juce::String (engine->getPosition(), 3) + " s (expected 1.000)");

        // 예비박 중에 위치를 옮기면 예비박을 그만두고 바로 재생한다.
        engine->setPosition (1.0);
        engine->play();
        capture (0.3, 512);
        engine->setPosition (2.0);
        const bool countingAfterSeek = engine->isCountingIn();
        capture (0.5, 512);
        log ("metronome seek during count-in: counting " + juce::String ((int) countingAfterSeek) + ", playing " + juce::String ((int) engine->isPlaying())
             + ", position " + juce::String (engine->getPosition(), 2) + " s (expected about 2.5)");
        engine->pause();
        settle();

        engine->metronomeOn = false;
        engine->metronomeVolume = 0.7f;
        engine->song.mute = false;
        engine->drum.mute = false;
        engine->setBeatMap ({});
        songFile.deleteFile();
    }

    // 원곡 드럼 빼기: 나머지 + (원곡 − 나머지)는 원곡과 같고, 드럼을 끄면 나머지만 남는다.
    // 켜고 끄기는 미리 읽어 둔 소리와 상관없이 바로(30ms) 들려야 한다.
    void testStems()
    {
        constexpr double rate = 44100.0;
        const auto original = report.getSiblingFile ("selftest-original.wav");
        const auto rest = report.getSiblingFile ("selftest-rest.wav");
        const auto drums = report.getSiblingFile ("selftest-drums.wav");
        std::vector<double> hits;

        for (int k = 0; k < 11; ++k)
            hits.push_back (0.25 + 0.5 * k);

        writePulses (original, 6.0, hits, 0.6f, 0.2f);   // 사인(드럼 아닌 소리) + 펄스(드럼)
        writePulses (rest, 6.0, {}, 0.0f, 0.2f);         // 사인만

        juce::String error;

        if (! makeExactDrumStem (original, rest, drums, error))
        {
            log ("stems: FAILED to make drum stem " + error);
            return;
        }

        engine->metronomeOn = false;
        engine->drum.mute = true;
        engine->song.mute = false;

        const auto play = [&] (const juce::File& song, bool stems, bool drumsOn, double from, double seconds)
        {
            juce::String e;
            engine->prepareOffline (rate, 512);
            engine->loadSong (song, e);

            if (stems)
                engine->useSongStems (rest, drums, e);

            engine->setSongDrumsOn (drumsOn);
            engine->setPosition (from);
            capture (0.3, 512);   // 드럼 켜고 끄기 전환이 끝나게
            engine->play();
            auto out = capture (seconds, 512);
            engine->pause();
            capture (0.05, 512);   // 멈춤을 받아들이게(위 settle 설명)
            return out;
        };

        const auto maxDifference = [] (const std::vector<float>& a, const std::vector<float>& b)
        {
            float worst = 0.0f;

            for (size_t i = 0; i < juce::jmin (a.size(), b.size()); ++i)
                worst = juce::jmax (worst, std::abs (a[i] - b[i]));

            return worst;
        };

        const auto originalOut = play (original, false, true, 1.0, 2.0);
        const auto stemsOn = play (original, true, true, 1.0, 2.0);
        const auto restOut = play (rest, false, true, 1.0, 2.0);
        const auto stemsOff = play (original, true, false, 1.0, 2.0);
        const float pulseSize = maxDifference (originalOut, restOut);

        log ("stems: drums on vs original max diff " + dbText (maxDifference (originalOut, stemsOn))
             + ", drums off vs rest max diff " + dbText (maxDifference (restOut, stemsOff))
             + " (pulse size " + dbText (pulseSize) + ")");

        // 재생 중에 끄기: 미리 읽어 둔 0.7초와 상관없이 다음 펄스(0.25초 뒤)부터 사라져야 한다.
        {
            juce::String e;
            engine->prepareOffline (rate, 512);
            engine->loadSong (original, e);
            engine->useSongStems (rest, drums, e);
            engine->setSongDrumsOn (true);
            engine->setPosition (1.0);
            capture (0.3, 512);
            engine->play();
            const auto before = capture (1.0, 512);   // 1.0~2.0초: 펄스 2.25? 아님 → 1.25, 1.75
            engine->setSongDrumsOn (false);
            const auto after = capture (1.0, 512);    // 2.0~3.0초: 펄스 2.25, 2.75 → 사라져야 함
            engine->pause();

            float peakBefore = 0.0f, peakAfter = 0.0f;

            for (auto v : before)
                peakBefore = juce::jmax (peakBefore, std::abs (v));

            for (auto v : after)
                peakAfter = juce::jmax (peakAfter, std::abs (v));

            log ("stems toggle while playing: peak before " + dbText (peakBefore) + ", right after turning drums off " + dbText (peakAfter)
                 + " (sine only would be about " + dbText (0.2f * 0.49f) + ")");
        }

        // 원곡 그대로 재생하다가 분리한 파일로 바꿔도 위치와 재생 상태가 이어져야 한다.
        {
            juce::String e;
            engine->prepareOffline (rate, 512);
            engine->loadSong (original, e);
            engine->setSongDrumsOn (true);
            engine->setPosition (2.0);
            engine->play();
            capture (0.5, 512);
            const double positionBefore = engine->getPosition();
            const bool ok = engine->useSongStems (rest, drums, e);
            log ("stems switch while playing: " + juce::String (ok ? "ok" : "FAILED " + e) + ", position " + juce::String (positionBefore, 3)
                 + " -> " + juce::String (engine->getPosition(), 3) + " s, playing " + juce::String ((int) engine->isPlaying()));
            engine->pause();
        }

        // 분리한 원곡으로 녹음하면 원곡 드럼.wav도 남는다. 다듬기 창에서 원곡 드럼을 0으로 하면
        // 펄스(드럼)가 빠지고 사인만 남아야 한다.
        {
            juce::String e;
            const auto folder = report.getSiblingFile ("selftest-stem-recording");
            folder.deleteRecursively();
            engine->prepareOffline (rate, 512);
            engine->loadSong (original, e);
            engine->useSongStems (rest, drums, e);
            engine->setSongDrumsOn (true);
            engine->setPosition (1.0);
            capture (0.3, 512);
            engine->play();
            const bool recording = engine->startRecording (folder, e);
            capture (2.0, 512);
            engine->stopRecording();
            engine->pause();

            const auto take = engine->getLastTakeFolder();
            auto player = recording ? TakePlayer::load (take, e) : nullptr;

            if (player == nullptr)
            {
                log ("stem recording: FAILED " + e);
            }
            else
            {
                const auto exportPeak = [&] (float amount)
                {
                    player->drumGain = 0.0f;
                    player->songGain = 1.0f;
                    player->songDrumAmount = amount;
                    const auto out = take.getChildFile ("song drums " + juce::String (amount, 2) + ".wav");
                    juce::String exportError;

                    if (! player->exportMix (out, TakePlayer::ExportFormat::wav24, exportError))
                        return 0.0f;

                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (out));

                    if (reader == nullptr)
                        return 0.0f;

                    juce::Range<float> range[2];
                    reader->readMaxLevels (0, reader->lengthInSamples, range, 2);
                    return juce::jmax (std::abs (range[0].getStart()), std::abs (range[0].getEnd()));
                };

                log ("stem recording: song drums file " + juce::String (player->hasSongDrums() ? "loaded" : "MISSING")
                     + ", export peak with song drums 100 " + dbText (exportPeak (1.0f)) + ", 50 " + dbText (exportPeak (0.25f))
                     + ", 0 " + dbText (exportPeak (0.0f)) + " (pulse about " + dbText (0.6f) + ", sine only about " + dbText (0.2f) + ")");
            }

            folder.deleteRecursively();
        }

        // 48kHz 녹음을 '원곡 드럼 분리'할 때는 44.1kHz로 바꿔 나눈 뒤 되돌린다. 주파수마다 크기·시각이 얼마나 바뀌는지 잰다.
        {
            const auto source = report.getSiblingFile ("selftest-48k.wav");
            const auto down = report.getSiblingFile ("selftest-48k-down.wav");
            const auto back = report.getSiblingFile ("selftest-48k-back.wav");
            juce::StringArray parts;
            juce::String failure;

            for (double hz : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0 })
            {
                const double w = 2.0 * juce::MathConstants<double>::pi * hz / 48000.0;
                juce::AudioBuffer<float> tone (2, 48000 * 2);

                for (int i = 0; i < tone.getNumSamples(); ++i)
                {
                    const float v = 0.5f * (float) std::sin (w * i);
                    tone.setSample (0, i, v);
                    tone.setSample (1, i, v);
                }

                source.deleteFile();

                if (std::unique_ptr<juce::OutputStream> stream = source.createOutputStream())
                {
                    juce::WavAudioFormat wav;

                    if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                       .withSampleRate (48000.0)
                                                                       .withNumChannels (2)
                                                                       .withBitsPerSample (32)
                                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)))
                        writer->writeFromAudioSampleBuffer (tone, 0, tone.getNumSamples());
                }

                down.deleteFile();
                back.deleteFile();
                juce::String e;
                const bool ok = writeResampledCopy (source, down, 44100.0, e) && writeResampledCopy (down, back, 48000.0, e);
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (ok ? formats.createReaderFor (back) : nullptr);

                if (reader == nullptr)
                {
                    failure = e;
                    break;
                }

                // 가운데 1초를 sin·cos에 투영해 크기와 위상(= 몇 샘플 늦은지)을 구한다.
                juce::AudioBuffer<float> result (2, (int) reader->lengthInSamples);
                reader->read (&result, 0, result.getNumSamples(), 0, true, true);
                double a = 0.0, b = 0.0;
                int n = 0;

                for (int i = 24000; i < juce::jmin (72000, result.getNumSamples()); ++i, ++n)
                {
                    a += result.getSample (0, i) * std::sin (w * i);
                    b += result.getSample (0, i) * std::cos (w * i);
                }

                const double amplitude = 2.0 * std::sqrt (a * a + b * b) / juce::jmax (1, n);
                const double late = -std::atan2 (b, a) / w;   // y = A sin (w (i - late))
                parts.add (juce::String (hz / 1000.0, hz < 1000.0 ? 1 : 0) + "k " + juce::String (juce::Decibels::gainToDecibels (amplitude / 0.5), 2)
                           + " dB " + juce::String (late, 2) + " smp");
            }

            log ("resample 48k -> 44.1k -> 48k (level, samples late): "
                 + (failure.isNotEmpty() ? "FAILED " + failure : parts.joinIntoString (", ")));

            for (const auto& f : { source, down, back })
                f.deleteFile();
        }

        engine->drum.mute = false;
        engine->setSongDrumsOn (true);
        juce::String e;
        engine->loadSong (toneFile, e);   // 다른 점검이 쓰던 곡으로 되돌리고 시험 파일을 지운다

        for (const auto& f : { original, rest, drums })
            f.deleteFile();
    }

    juce::File report, toneFile;
    int settleMs;
    std::unique_ptr<Engine> engine;
    juce::CriticalSection logLock;
    juce::StringArray lines;
};

//==============================================================================
// 드럼 모듈의 ASIO 드라이버만 열어 드라이버가 알려 주는 지연을 한 줄로 적고 끝낸다(플러그인·저장된 설정 없이).
// Yamaha 드라이버 설정(레지스트리)을 바꿔 가며 드라이버 여유가 어떻게 달라지는지 빨리 재는 데 쓴다.
//   DrumVstHost.exe --driver-latency 결과.txt
class DriverLatencyCheck final : private juce::Timer
{
public:
    explicit DriverLatencyCheck (const juce::File& reportFile) : report (reportFile)
    {
        engine = std::make_unique<Engine> (true, nullptr);
        started = juce::Time::getMillisecondCounter();
        startTimer (250);
    }

private:
    void timerCallback() override
    {
        auto* device = engine->deviceManager.getCurrentAudioDevice();
        const bool open = device != nullptr && device->isOpen() && device->getTypeName() == "ASIO"
                          && device->getOutputLatencyInSamples() > 0;

        if (! open && juce::Time::getMillisecondCounter() - started < 8000)
        {
            engine->refreshMidiInputs();   // 장치 열기를 다시 해 본다(앱의 1초 확인과 같은 일)
            return;
        }

        stopTimer();
        juce::String text;

        if (device != nullptr)
            text << device->getTypeName() << " / " << device->getName() << " | rate " << juce::String (device->getCurrentSampleRate(), 0)
                 << " | buffer " << device->getCurrentBufferSizeSamples() << " | out " << device->getOutputLatencyInSamples()
                 << " | in " << device->getInputLatencyInSamples() << " | registry " << Engine::readYamahaDriverSettings();
        else
            text << "no device";

        report.replaceWithText (text + "\r\n");
        juce::JUCEApplication::quit();
    }

    juce::File report;
    std::unique_ptr<Engine> engine;
    juce::uint32 started = 0;
};

//==============================================================================
// 실제 소리 장치와 저장된 드럼 킷으로 버퍼 크기마다 끊김과 처리 부하를 잰다. 출력은 무음이다.
//   DrumVstHost.exe --probe-latency 결과.txt [--seconds 초] [--soak 초]
//   --soak을 주면 저장된 원곡을 재생하는 경우와 녹음까지 하는 경우도 그 시간만큼 잰다.
class LatencyProbe final : private juce::Thread
{
public:
    LatencyProbe (juce::PropertiesFile& settings, const juce::File& reportFile, int secondsPerSize, int soakSecondsToUse,
                  const juce::String& songPathToUse)
        : juce::Thread ("latency probe"), report (reportFile), seconds (juce::jmax (2, secondsPerSize)),
          soakSeconds (juce::jmax (0, soakSecondsToUse)), songPath (songPathToUse)
    {
        engine = std::make_unique<Engine> (true, &settings);
        engine->master.volume = 0.0f;
        startThread();
    }

    ~LatencyProbe() override
    {
        stopThread (30000);
        engine.reset();
    }

private:
    void log (const juce::String& line)
    {
        const juce::ScopedLock lock (logLock);
        lines.add (line);
    }

    static juce::String ms (double value) { return juce::String (value, 1) + " ms"; }

    void describeDevice()
    {
        auto* device = engine->deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
        {
            log ("device: none");
            return;
        }

        juce::StringArray sizes, rates;

        for (auto s : device->getAvailableBufferSizes())
            sizes.add (juce::String (s));

        for (auto r : device->getAvailableSampleRates())
            rates.add (juce::String (r, 0));

        log ("device: " + device->getTypeName() + " / " + device->getName()
             + ", rate " + juce::String (device->getCurrentSampleRate(), 0)
             + ", buffer " + juce::String (device->getCurrentBufferSizeSamples())
             + " (driver default " + juce::String (device->getDefaultBufferSize()) + ")"
             + ", output latency " + juce::String (device->getOutputLatencyInSamples()) + " smp"
             + ", input latency " + juce::String (device->getInputLatencyInSamples()) + " smp");
        log ("  buffer sizes: " + sizes.joinIntoString (" "));
        log ("  sample rates: " + rates.joinIntoString (" "));
        log ("  midi inputs: " + engine->getMidiInputNames().joinIntoString (", "));
    }

    void finish()
    {
        juce::MessageManager::callAsync ([this]
        {
            {
                const juce::ScopedLock lock (logLock);
                report.replaceWithText (lines.joinIntoString ("\r\n") + "\r\n");
            }

            juce::JUCEApplication::quit();
        });
    }

    void run() override
    {
        int originalSize = 0;
        bool wantedDevice = false;

        runOnMessageThread ([&]
        {
            describeDevice();
            wantedDevice = engine->isWantedAudioDeviceOpen();

            const int overloads = engine->getDriverOverloads();
            log ("  driver overload report: " + (overloads < 0 ? juce::String ("not supported by this driver")
                                                                : juce::String (overloads) + " so far"));
        });

        // 다른 프로그램(보통 이미 켜 둔 DrumVstHost)이 ASIO를 잡고 있으면 다른 장치로 열리므로 재지 않는다.
        if (! wantedDevice)
        {
            log ("ABORT: the saved sound device could not be opened (is DrumVstHost or another app using it?)");
            finish();
            return;
        }

        runOnMessageThread ([&]
        {
            originalSize = engine->getBufferSize();

            auto state = engine->getSavedPluginState();
            juce::String error;
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const bool ok = engine->loadPlugin (engine->getSavedPluginPath(), state.isEmpty() ? nullptr : &state, error);

            log ("plugin: " + (ok ? engine->getPluginName() + " " + engine->describePluginBuses() : "FAILED " + error)
                 + " (" + juce::String (juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - start)) + " ms, saved state "
                 + juce::String ((int) state.getSize()) + " bytes)");
        });

        wait (12000);   // 킷 샘플을 읽을 시간

        // 원곡(장시간 시험용): 저장된 곡
        if (soakSeconds > 0)
        {
            runOnMessageThread ([&]
            {
                juce::String error;
                const auto path = songPath.isNotEmpty() ? songPath : engine->getSavedSongPath();
                const juce::File song (path);
                const bool ok = path.isNotEmpty() && engine->loadSong (song, error);
                log ("song: " + (ok ? song.getFileName() + " (" + juce::String (engine->getLength(), 1) + " s)" : "none " + error));
            });
        }

        juce::Array<int> available;
        runOnMessageThread ([&] { available = engine->getAvailableBufferSizes(); });

        juce::Array<int> sizes;

        for (int size : { 32, 48, 64, 96, 128, 192, 256, 512 })
            if (available.contains (size))
                sizes.add (size);

        if (sizes.isEmpty() && ! available.isEmpty())   // 예: Windows Audio의 480
            sizes.add (available.getFirst());

        for (int size : sizes)
        {
            if (threadShouldExit())
                break;

            int actual = 0;
            float outputMs = 0.0f, estimatedMs = 0.0f;
            juce::String error;

            runOnMessageThread ([&]
            {
                engine->setBufferSize (size, error);
                actual = engine->getBufferSize();
                outputMs = engine->getOutputLatencyMs();
                estimatedMs = engine->getEstimatedLatencyMs();
            });

            log ("buffer " + juce::String (size) + " -> " + juce::String (actual) + " smp: output latency " + ms (outputMs)
                 + ", estimated hit-to-sound " + ms (estimatedMs) + (error.isNotEmpty() ? " | error " + error : juce::String()));

            wait (1500);
            scenario ("  drums only", seconds, false, false);

            if (soakSeconds > 0 && engine->hasSong())
            {
                scenario ("  drums + song", soakSeconds, true, false);
                scenario ("  drums + song + recording", soakSeconds, true, true);
            }
        }

        runOnMessageThread ([&] { juce::String error; engine->setBufferSize (originalSize, error); });   // 설정 파일은 바꾸지 않는다
        log ("done");
        finish();
    }

    // 드럼 패턴을 seconds초 동안 치면서 끊김·CPU·콜백 간격을 잰다. 필요하면 원곡 재생과 녹음을 같이 켠다.
    void scenario (const juce::String& label, int durationSeconds, bool withSong, bool withRecording)
    {
        const auto recordFolder = report.getSiblingFile ("probe-recording");

        if (withSong)
            runOnMessageThread ([&] { engine->setPosition (juce::jmin (20.0, engine->getLength() * 0.2)); engine->play(); });

        if (withRecording)
            runOnMessageThread ([&] { juce::String error; engine->startRecording (recordFolder, error); });

        wait (700);

        const auto before = engine->getMonitor();
        engine->resetMonitorMaxima();
        engine->takeMaxMidiWaitMs();
        const int xrunsBefore = engine->getXRunCount();
        engine->drum.peakL = 0.0f;
        engine->drum.peakR = 0.0f;
        engine->song.peakL = 0.0f;
        engine->song.peakR = 0.0f;
        double cpuSum = 0.0, cpuMax = 0.0;
        float midiWaitMax = 0.0f;
        int count = 0;

        static constexpr int pattern[] = { 36, 42, 38, 42, 36, 46, 38, 49, 48, 45, 43, 51 };
        const int steps = durationSeconds * 20;

        for (int i = 0; i < steps && ! threadShouldExit(); ++i)
        {
            // 실제 드럼 모듈과 같은 MIDI 입력 경로로 넣는다(패드 신호를 기다린 시간도 함께 잰다).
            const int note = pattern[i % (int) std::size (pattern)];
            engine->injectMidiForTest (juce::MidiMessage::noteOn (10, note, (juce::uint8) 120));
            wait (25);
            engine->injectMidiForTest (juce::MidiMessage::noteOff (10, note));
            wait (25);

            const double cpu = engine->getCpuUsage();
            cpuSum += cpu;
            cpuMax = juce::jmax (cpuMax, cpu);
            midiWaitMax = juce::jmax (midiWaitMax, engine->takeMaxMidiWaitMs());
            ++count;
        }

        const int xruns = engine->getXRunCount() - xrunsBefore;
        const auto after = engine->getMonitor();
        const float drumPeak = juce::jmax (engine->drum.peakL.exchange (0.0f), engine->drum.peakR.exchange (0.0f));
        const float songPeak = juce::jmax (engine->song.peakL.exchange (0.0f), engine->song.peakR.exchange (0.0f));
        double recorded = 0.0;

        if (withRecording)
            runOnMessageThread ([&] { recorded = engine->getRecordedSeconds(); engine->stopRecording(); });

        if (withSong)
            runOnMessageThread ([&] { engine->pause(); });

        recordFolder.deleteRecursively();

        const auto delta = [] (juce::uint32 a, juce::uint32 b) { return juce::String ((juce::int64) (b - a)); };

        log (label + " (" + juce::String (durationSeconds) + " s): juce xruns " + juce::String (xruns)
             + " | cpu avg " + juce::String (cpuSum / juce::jmax (1, count) * 100.0, 1) + "% max " + juce::String (cpuMax * 100.0, 1) + "%"
             + " | drum peak " + dbText (drumPeak) + (withSong ? ", song peak " + dbText (songPeak) : juce::String())
             + (withRecording ? ", recorded " + juce::String (recorded, 1) + " s" : juce::String()));
        log ("      monitor: callbacks " + delta (before.callbacks, after.callbacks) + " every " + ms (after.blockMs)
             + " (driver headroom " + ms (after.headroomMs) + ")"
             + " | likely dropouts " + delta (before.likelyDropouts, after.likelyDropouts)
             + ", near misses " + delta (before.nearMiss, after.nearMiss)
             + ", own overruns " + delta (before.ownOverruns, after.ownOverruns)
             + ", late " + delta (before.late, after.late) + " / slightly late " + delta (before.lateNear, after.lateNear)
             + ", restarts " + delta (before.streamStarts, after.streamStarts)
             + ", plugin skips " + delta (before.pluginSkips, after.pluginSkips)
             + ", warm-up " + delta (before.warmupAnomalies, after.warmupAnomalies)
             + " | longest callback " + ms (after.maxDurationMs) + ", worst lateness " + ms (after.maxExcessMs)
             + " | midi wait max " + ms (midiWaitMax));
    }

    juce::File report;
    int seconds, soakSeconds;
    juce::String songPath;
    std::unique_ptr<Engine> engine;
    juce::CriticalSection logLock;
    juce::StringArray lines;
};

//==============================================================================
// 확인용: 분석 도구를 앱 안의 설치 기능으로 받아 설치한 뒤, 그 도구로 박자 찾기(와 드럼 분리)를 실제로 한 번 돌려 본다.
class InstallTest final
{
public:
    InstallTest (const juce::File& reportFile, bool withSeparation, int cancelAfterSeconds)
        : report (reportFile), separation (withSeparation)
    {
        report.deleteFile();
        log ("install-analysis: " + juce::String (separation ? "beats + separation" : "beats")
             + ", folder " + analysis::folderForInstall().getFullPathName()
             + ", ready before: beats " + juce::String ((int) analysis::isReady (analysis::Kind::beats))
             + ", separation " + juce::String ((int) analysis::isReady (analysis::Kind::separation)));

        started = juce::Time::getMillisecondCounterHiRes();

        installer.onProgress = [this] (const juce::String& stage, float progress)
        {
            const int percent = juce::roundToInt (progress * 100.0f);

            if (stage != lastStage || percent >= lastPercent + 10)
            {
                lastStage = stage;
                lastPercent = percent;
                log (juce::String (seconds(), 1) + " s  " + stage + " " + juce::String (percent) + "%");
            }
        };

        installer.onFinished = [this] (bool ok, const juce::String& error)
        {
            log ("install " + juce::String (ok ? "OK" : "FAILED: " + error) + " after " + juce::String (seconds(), 1) + " s");
            log ("ready after: beats " + juce::String ((int) analysis::isReady (analysis::Kind::beats))
                 + ", separation " + juce::String ((int) analysis::isReady (analysis::Kind::separation))
                 + ", python " + analysis::managedPython().getFullPathName()
                 + ", lookup picks " + findAnalysisPython ({}).getFullPathName());

            if (ok)
                verify();

            log ("done");
            juce::JUCEApplication::quit();
        };

        installer.start (separation);

        // 도중에 그만두기 시험: 정한 시간 뒤 멈추고, 결과를 알리지 않는지·표시 파일이 그대로인지 본다.
        if (cancelAfterSeconds > 0)
            callOnceAfterDelay (cancelAfterSeconds * 1000, [this]
            {
                const double t0 = seconds();
                installer.cancel();
                log ("cancelled at " + juce::String (t0, 1) + " s (stop took " + juce::String (seconds() - t0, 1) + " s), still running "
                     + juce::String ((int) installer.isRunning()) + ", ready: beats " + juce::String ((int) analysis::isReady (analysis::Kind::beats))
                     + ", separation " + juce::String ((int) analysis::isReady (analysis::Kind::separation)));

                callOnceAfterDelay (1500, [this]
                {
                    log ("done");
                    juce::JUCEApplication::quit();
                });
            });
    }

private:
    double seconds() const { return (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0; }

    void log (const juce::String& line)
    {
        report.appendText (line + "\r\n");
    }

    // 120 BPM 클릭(마디 첫 박은 크게)과 낮은 사인파로 된 12초짜리 곡을 만들어 실제 스크립트로 분석한다.
    void verify()
    {
        const auto folder = report.getSiblingFile ("install-test-work");
        folder.deleteRecursively();
        folder.createDirectory();
        const auto song = folder.getChildFile ("song.wav");

        {
            constexpr double rate = 44100.0;
            juce::AudioBuffer<float> buffer (2, (int) (rate * 12.0));
            buffer.clear();

            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double t = i / rate;
                const double beat = std::fmod (t - 0.25, 0.5);
                const int index = (int) std::floor ((t - 0.25) / 0.5);
                float v = 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * t);

                if (t >= 0.25 && beat < 0.03)
                    v += (index % 4 == 0 ? 0.8f : 0.5f) * (float) (std::exp (-beat / 0.006) * std::sin (juce::MathConstants<double>::twoPi * 1800.0 * beat));

                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = song.createOutputStream();
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)
                                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        const auto python = findAnalysisPython ({}).getFullPathName();
        const auto beatsJson = folder.getChildFile ("beats.json");

        if (run ({ python, "-I", "-X", "utf8", "-u", toolsFolder().getChildFile ("beats.py").getFullPathName(),
                   song.getFullPathName(), beatsJson.getFullPathName() }, "beats.py"))
        {
            const auto json = juce::JSON::parse (beatsJson);
            const auto beats = json.getProperty ("beats", {});
            log ("beats: bpm " + juce::String ((double) json.getProperty ("bpm", 0.0), 2) + " (expected 120), "
                 + juce::String (beats.isArray() ? beats.size() : 0) + " beats, first "
                 + (beats.isArray() && beats.size() > 0 ? juce::String ((double) beats[0], 3) : juce::String ("-")) + " s (expected 0.250)");
        }

        if (separation)
        {
            const auto out = folder.getChildFile ("separated");

            if (run ({ python, "-I", "-X", "utf8", "-u", toolsFolder().getChildFile ("separate.py").getFullPathName(),
                       song.getFullPathName(), out.getFullPathName(), "htdemucs" }, "separate.py"))
            {
                const auto rest = out.getChildFile ("htdemucs").getChildFile ("no_drums.wav");
                log ("separate: no_drums.wav " + juce::String (rest.getSize()) + " bytes (expected about "
                     + juce::String (song.getSize()) + ")");
            }
        }
    }

    bool run (const juce::StringArray& command, const juce::String& label)
    {
        const double t0 = seconds();
        juce::ChildProcess process;

        if (! process.start (command, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            log (label + ": could not start");
            return false;
        }

        const auto output = process.readAllProcessOutput();
        const auto code = process.getExitCode();
        auto lines = juce::StringArray::fromLines (output);
        lines.removeEmptyStrings();
        log (label + ": exit " + juce::String ((int) code) + " in " + juce::String (seconds() - t0, 1) + " s, last line: "
             + (lines.isEmpty() ? juce::String() : lines[lines.size() - 1].substring (0, 200)));
        return code == 0;
    }

    juce::File report;
    bool separation;
    analysis::Installer installer;
    double started = 0.0;
    juce::String lastStage;
    int lastPercent = -100;
};

//==============================================================================
class App final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "DrumVstHost"; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    bool moreThanOneInstanceAllowed() override
    {
        const auto commandLine = getCommandLineParameters();
        return commandLine.contains ("--selftest") || commandLine.contains ("--snapshot") || commandLine.contains ("--probe-latency")
            || commandLine.contains ("--install-analysis") || commandLine.contains ("--driver-latency");
    }

    void initialise (const juce::String& commandLine) override
    {
        // 드럼 분리(파이썬)가 물려받는 환경: 분리 모델은 이미 받아 둔 것만 쓴다(인터넷에서 몰래 받지 않게;
        // separate.py가 'hf://' 이름으로 불러 받아 둔 것이 없으면 오류로 끝난다).
        // UTF-8·버퍼 없는 출력은 실행 옵션(-X utf8 -u)으로 준다. -I 때문에 PYTHON* 환경 변수는 무시된다.
        setProcessEnvironmentVariable ("HF_HUB_OFFLINE", "1");

        const auto args = juce::StringArray::fromTokens (commandLine, true);

        juce::PropertiesFile::Options options;
        options.applicationName = "DrumVstHost";
        options.filenameSuffix = ".settings";
        options.folderName = "DrumVstHost";
        options.osxLibrarySubFolder = "Application Support";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        properties.setStorageParameters (options);

        auto* settings = properties.getUserSettings();

        //   --settings-file 파일   다른 설정 파일을 쓴다(확인용: 사용자 설정을 건드리지 않게)
        if (const auto file = argAfter (args, "--settings-file"); file.isNotEmpty())
        {
            testSettings = std::make_unique<juce::PropertiesFile> (juce::File (file), options);
            settings = testSettings.get();
        }

        // 화면 언어: 설정에서 고른 것(language = ko·en), 없으면 Windows 표시 언어가 한국어일 때만 한국어, 그 밖에는 영어.
        //   --lang ko|en   이번 실행만 그 언어로(확인용)
        {
            auto language = argAfter (args, "--lang");

            if (language.isEmpty())
                language = settings->getValue ("language");

            if (language.isEmpty())
                language = juce::SystemStats::getDisplayLanguage().startsWithIgnoreCase ("ko") ? "ko" : "en";

            lang::setEnglish (! language.startsWithIgnoreCase ("ko"));
        }

        // 비정상 종료되면 원인을 찾을 수 있게 그 순간의 호출 경로를 exe 옆에 남긴다.
        installCrashReporter (juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                  .getParentDirectory().getChildFile (lang::pick ("충돌 기록.txt", "Crash log.txt")));

        if (! lang::isEnglish())   // JUCE에 들어 있는 글자(파일 고르기 창 등)의 한국어 번역
            juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (juce::String::fromUTF8 (koreanStrings), false));

        look = std::make_unique<Look>();
        juce::LookAndFeel::setDefaultLookAndFeel (look.get());

        //   --install-analysis 결과.txt [--with-separation]   분석 도구를 앱의 설치 기능으로 받아 설치하고 실제로 돌려 본다
        if (const auto file = argAfter (args, "--install-analysis"); file.isNotEmpty())
        {
            installTest = std::make_unique<InstallTest> (juce::File (file), args.contains ("--with-separation"),
                                                         argAfter (args, "--cancel-after").getIntValue());
            return;
        }

        //   --selftest 결과.txt [--use-saved-kit]   저장된 킷으로 점검하려면 --use-saved-kit (설정은 바꾸지 않는다)
        if (args.contains ("--selftest"))
        {
            const auto settle = argAfter (args, "--settle");
            selfTest = std::make_unique<SelfTest> (juce::File (argAfter (args, "--selftest")),
                                                   argAfter (args, "--plugin"),
                                                   settle.isNotEmpty() ? settle.getIntValue() : 4000,
                                                   args.contains ("--use-saved-kit") ? settings : nullptr);
            return;
        }

        if (const auto file = argAfter (args, "--driver-latency"); file.isNotEmpty())
        {
            driverCheck = std::make_unique<DriverLatencyCheck> (juce::File (file));
            return;
        }

        if (args.contains ("--probe-latency"))
        {
            const auto seconds = argAfter (args, "--seconds");
            probe = std::make_unique<LatencyProbe> (*settings, juce::File (argAfter (args, "--probe-latency")),
                                                    seconds.isNotEmpty() ? seconds.getIntValue() : 6,
                                                    argAfter (args, "--soak").getIntValue(),
                                                    argAfter (args, "--song"));
            return;
        }

        const bool snapshot = args.contains ("--snapshot");
        const bool fresh = args.contains ("--fresh");   // 저장된 값 없이 처음 실행한 것처럼

        // --no-audio: 소리 장치·드럼 MIDI를 열지 않는다(사용 중인 앱 옆에서 화면만 확인할 때)
        engine = std::make_unique<Engine> (! args.contains ("--no-audio"), fresh ? nullptr : settings);
        window = std::make_unique<MainWindow> (*engine, *settings);
        saveOnExit = ! snapshot && ! fresh && ! args.contains ("--no-save");

        if (args.contains ("--minimised"))   // 확인용 실행이 사용 중인 화면을 가리지 않게
            window->setMinimised (true);

        window->getMain().setAutosave (saveOnExit);

        if (! args.contains ("--no-restore"))
            window->getMain().restoreSession();

        if (snapshot)
            scheduleSnapshot (juce::File (argAfter (args, "--snapshot")),
                              juce::jmax (500, argAfter (args, "--delay").getIntValue()),
                              args.contains ("--tone-open"));

        // 아래는 모양·동작 확인용 옵션이다.
        if (const auto song = argAfter (args, "--song"); song.isNotEmpty())
            window->getMain().openSong (juce::File (song));

        if (args.contains ("--play"))
            engine->play();

        if (args.contains ("--silent"))
            engine->master.volume = 0.0f;

        if (args.contains ("--editor"))
            callOnceAfterDelay (5000, [this]
            {
                if (window != nullptr)
                    window->getMain().openPluginWindow();
            });

        if (args.contains ("--hit"))
            hitter = std::make_unique<Hitter> (*engine);

        //   --record-folder 폴더   녹음 위치를 이번 실행에서만 바꾼다
        //   --record-test 초       6초 뒤 녹음을 시작해 정한 시간 뒤 끝낸다(끝나면 다듬기 창이 열린다)
        //   --open-take 폴더       녹음 다듬기 창을 연다
        //   --quit-after 초        정한 시간 뒤 정상 종료한다
        if (const auto folder = argAfter (args, "--record-folder"); folder.isNotEmpty())
            window->getMain().setRecordFolder (juce::File (folder));

        if (const auto take = argAfter (args, "--open-take"); take.isNotEmpty())
            callOnceAfterDelay (1500, [this, take]
            {
                if (window != nullptr)
                    window->getMain().openTakeEditor (juce::File (take));
            });

        if (const auto seconds = argAfter (args, "--record-test"); seconds.isNotEmpty())
        {
            const int lengthMs = juce::jmax (1, seconds.getIntValue()) * 1000;

            callOnceAfterDelay (6000, [this, lengthMs]
            {
                if (window == nullptr)
                    return;

                window->getMain().startRecordingForTest();

                callOnceAfterDelay (lengthMs, [this]
                {
                    if (window != nullptr)
                        window->getMain().stopRecordingForTest();
                });
            });
        }

        if (const auto seconds = argAfter (args, "--quit-after"); seconds.isNotEmpty())
            callOnceAfterDelay (juce::jmax (1, seconds.getIntValue()) * 1000, [] { juce::JUCEApplication::quit(); });

        //   --snapshot-tool-offer 화면.png [--with-separation]   분석 도구 내려받기 안내 창을 열어 PNG로 저장한 뒤 끝낸다
        if (const auto png = argAfter (args, "--snapshot-tool-offer"); png.isNotEmpty())
        {
            const bool separation = args.contains ("--with-separation");

            callOnceAfterDelay (2000, [this, png, separation]
            {
                if (window == nullptr)
                    return;

                window->getMain().offerToolInstallForTest (separation);

                callOnceAfterDelay (800, [this, png]
                {
                    for (int i = juce::Desktop::getInstance().getNumComponents(); --i >= 0;)
                    {
                        if (auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Desktop::getInstance().getComponent (i)))
                        {
                            const auto image = alert->createComponentSnapshot (alert->getLocalBounds(), true, 2.0f);
                            const juce::File file (png);
                            file.deleteFile();

                            if (auto stream = file.createOutputStream())
                            {
                                juce::PNGImageFormat format;
                                format.writeImageToStream (image, *stream);
                            }

                            alert->exitModalState (0);
                            break;
                        }
                    }

                    quit();
                });
            });
        }

        //   --snapshot-pad 화면.png   패드 맞추기 창을 열어 PNG로 저장한 뒤 끝낸다
        if (const auto png = argAfter (args, "--snapshot-pad"); png.isNotEmpty())
            callOnceAfterDelay (2500, [this, png]
            {
                if (window == nullptr)
                    return;

                if (auto* panel = window->getMain().openPadMapForTest())
                {
                    const auto image = panel->createComponentSnapshot (panel->getLocalBounds(), true, 2.0f);
                    const juce::File file (png);
                    file.deleteFile();

                    if (auto stream = file.createOutputStream())
                    {
                        juce::PNGImageFormat format;
                        format.writeImageToStream (image, *stream);
                    }
                }

                quit();
            });

        //   --snapshot-monitor 화면.png   '신호 보기' 창을 열어 예시 신호를 넣고 PNG로 저장한 뒤 끝낸다
        if (const auto png = argAfter (args, "--snapshot-monitor"); png.isNotEmpty())
            callOnceAfterDelay (2500, [this, png]
            {
                if (window == nullptr)
                    return;

                if (auto* panel = window->getMain().openMidiMonitorForTest())
                {
                    juce::Component::SafePointer<juce::Component> safePanel (panel);

                    callOnceAfterDelay (800, [this, safePanel, png]
                    {
                        if (safePanel != nullptr)
                            saveSnapshot (*safePanel, juce::File (png));

                        quit();
                    });
                    return;
                }

                quit();
            });

        //   --snapshot-take 화면.png   다듬기 창이 열리면 예시 값(지연만큼 당기기, 앞뒤 자르기)을 넣고 PNG로 저장한 뒤 끝낸다
        //     [--separate-song-drums]   원곡 드럼 파일이 없으면 '원곡 드럼 분리'를 누른 화면(…-before.png)을 남기고,
        //                               분리가 끝나기를 기다렸다가 찍는다(진짜 분리 도구를 돌린다)
        if (const auto png = argAfter (args, "--snapshot-take"); png.isNotEmpty())
            waitForTakeEditor (juce::File (png), 120, args.contains ("--separate-song-drums"));
    }

    static void saveSnapshot (juce::Component& component, const juce::File& png)
    {
        const auto image = component.createComponentSnapshot (component.getLocalBounds(), true, 2.0f);
        png.deleteFile();

        if (auto stream = png.createOutputStream())
        {
            juce::PNGImageFormat format;
            format.writeImageToStream (image, *stream);
        }
    }

    void waitForTakeEditor (const juce::File& png, int triesLeft, bool separate)
    {
        callOnceAfterDelay (500, [this, png, triesLeft, separate]
        {
            if (window == nullptr)
                return;

            auto* editor = window->getMain().getTakeEditor();

            if (editor == nullptr)
            {
                if (triesLeft > 0)
                    waitForTakeEditor (png, triesLeft - 1, separate);
                else
                    quit();

                return;
            }

            if (separate && ! editor->hasSongDrums() && editor->onSeparateSongDrums != nullptr)
            {
                editor->onSeparateSongDrums();
                callOnceAfterDelay (3000, [this, png]
                {
                    if (window != nullptr)
                        if (auto* ed = window->getMain().getTakeEditor())
                            saveSnapshot (*ed, png.getSiblingFile (png.getFileNameWithoutExtension() + "-before.png"));

                    waitForTakeSeparation (png, 900);
                });
                return;
            }

            snapshotTakeEditor (png);
        });
    }

    void waitForTakeSeparation (const juce::File& png, int secondsLeft)
    {
        callOnceAfterDelay (1000, [this, png, secondsLeft]
        {
            if (window == nullptr)
                return;

            if (window->getMain().isTakeSeparationBusy() && secondsLeft > 0)
                waitForTakeSeparation (png, secondsLeft - 1);
            else
                snapshotTakeEditor (png);
        });
    }

    void snapshotTakeEditor (const juce::File& png)
    {
        if (window == nullptr)
            return;

        if (auto* editor = window->getMain().getTakeEditor())
            editor->runSnapshotDemo();

        callOnceAfterDelay (600, [this, png]
        {
            if (window == nullptr)
                return;

            if (auto* ed = window->getMain().getTakeEditor())
                saveSnapshot (*ed, png);

            quit();
        });
    }

    void shutdown() override
    {
        if (saveOnExit && engine != nullptr && window != nullptr)
        {
            auto* settings = testSettings != nullptr ? testSettings.get() : properties.getUserSettings();
            window->getMain().saveSession();
            window->savePosition();
            engine->saveState (*settings);
            settings->saveIfNeeded();
        }

        hitter.reset();
        probe.reset();
        driverCheck.reset();
        selfTest.reset();
        installTest.reset();
        window.reset();
        engine.reset();

        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        look.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (window != nullptr)
            window->toFront (true);
    }

private:
    // 화면을 PNG로 저장하고 끝낸다(모양 확인용).
    //   DrumVstHost.exe --snapshot 화면.png [--delay 밀리초] [--tone-open] [--no-restore] [--fresh]
    void scheduleSnapshot (const juce::File& file, int delayMs, bool toneOpen)
    {
        callOnceAfterDelay (delayMs, [this, file, toneOpen]
        {
            if (window == nullptr)
                return;

            if (toneOpen)
                window->getMain().setToneOpen (true, true);

            callOnceAfterDelay (400, [this, file]
            {
                if (window == nullptr)
                    return;

                auto& main = window->getMain();
                const auto image = main.createComponentSnapshot (main.getLocalBounds(), true, 2.0f);

                file.deleteFile();

                if (auto stream = file.createOutputStream())
                {
                    juce::PNGImageFormat png;
                    png.writeImageToStream (image, *stream);
                }

                quit();
            });
        });
    }

    // 0.4초마다 킥·스네어·하이햇을 번갈아 친다(--hit).
    struct Hitter final : private juce::Timer
    {
        explicit Hitter (Engine& e) : engine (e) { startTimer (400); }

        void timerCallback() override
        {
            static constexpr int notes[] = { 36, 42, 38, 42 };
            engine.playTestNote (notes[step++ % 4], 0.9f);
        }

        Engine& engine;
        int step = 0;
    };

    std::unique_ptr<Hitter> hitter;
    std::unique_ptr<Look> look;
    juce::ApplicationProperties properties;
    std::unique_ptr<juce::PropertiesFile> testSettings;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<SelfTest> selfTest;
    std::unique_ptr<InstallTest> installTest;
    std::unique_ptr<LatencyProbe> probe;
    std::unique_ptr<DriverLatencyCheck> driverCheck;
    bool saveOnExit = false;
};

START_JUCE_APPLICATION (App)
