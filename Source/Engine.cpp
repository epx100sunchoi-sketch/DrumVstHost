#include "Engine.h"
#include "DrumPlugins.h"

namespace
{
// 화면의 볼륨(0~1)을 실제 배율로. 제곱 곡선이라 50이면 약 -12dB.
float faderGain (const ChannelSettings& settings, bool canMute) noexcept
{
    if (canMute && settings.mute.load (std::memory_order_relaxed))
        return 0.0f;

    const float v = juce::jlimit (0.0f, 1.0f, settings.volume.load (std::memory_order_relaxed));
    return v * v;
}

void pushPeak (std::atomic<float>& target, float value) noexcept
{
    if (value > target.load (std::memory_order_relaxed))
        target.store (value, std::memory_order_relaxed);
}

float blockPeak (const float* data, int numSamples) noexcept
{
    const auto range = juce::FloatVectorOperations::findMinAndMax (data, numSamples);
    return juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd()));
}

void applyGain (juce::SmoothedValue<float>& gain, float* left, float* right, int numSamples) noexcept
{
    if (gain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gain.getNextValue();
            left[i] *= g;
            right[i] *= g;
        }
    }
    else
    {
        const float g = gain.getTargetValue();
        juce::FloatVectorOperations::multiply (left, g, numSamples);
        juce::FloatVectorOperations::multiply (right, g, numSamples);
    }
}

// 드럼 모듈의 ASIO 드라이버(Yamaha Steinberg USB, Roland TD-xx 등)를 고른다. 연결된 모듈과 같은 회사·모델이면 우선.
// 드라이버는 모듈이 꺼져 있어도 목록에 있으므로, 처음 실행할 때 모듈이 꺼져 있어도 맞게 고른다.
std::unique_ptr<juce::XmlElement> makeDrumModuleAsioState (juce::AudioDeviceManager& manager, const DrumModule& connected)
{
    juce::String best;
    int bestScore = 0;

    for (auto* type : manager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() != "ASIO")
            continue;

        type->scanForDevices();

        for (const auto& name : type->getDeviceNames (false))
        {
            if (const int score = drumDriverScore (name, connected); score > bestScore)
            {
                best = name;
                bestScore = score;
            }
        }
    }

    if (best.isEmpty())
        return {};

    auto xml = std::make_unique<juce::XmlElement> ("DEVICESETUP");
    xml->setAttribute ("deviceType", "ASIO");
    xml->setAttribute ("audioOutputDeviceName", best);
    xml->setAttribute ("audioInputDeviceName", "");
    return xml;
}
} // namespace

//==============================================================================
// 분리된 원곡 두 파일(드럼을 뺀 나머지, 드럼)을 늘 같은 위치로 함께 읽고, 드럼 쪽만 음량을 바꾼다.
// 드럼을 뺀 원곡(0·1번 채널)과 원곡 드럼(2·3번 채널)을 함께 읽는다.
// 둘을 섞는 일은 오디오 스레드가 한다: 미리 읽어 둔 소리(0.7초쯤)에 섞어 두면 켜고 끄기가 그만큼 늦게 들린다.
class StemPairSource final : public juce::PositionableAudioSource
{
public:
    StemPairSource (juce::AudioFormatReader* restReader, juce::AudioFormatReader* drumsReader)
        : rest (restReader, true), drums (drumsReader, true) {}

    void prepareToPlay (int samplesPerBlockExpected, double rate) override
    {
        rest.prepareToPlay (samplesPerBlockExpected, rate);
        drums.prepareToPlay (samplesPerBlockExpected, rate);
    }

    void releaseResources() override
    {
        rest.releaseResources();
        drums.releaseResources();
    }

    // 미리 읽기 스레드에서 불린다.
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        auto& buffer = *info.buffer;
        const int last = buffer.getNumChannels() - 1;

        float* restChannels[] = { buffer.getWritePointer (0), buffer.getWritePointer (juce::jmin (1, last)) };
        juce::AudioBuffer<float> restView (restChannels, 2, info.startSample, info.numSamples);
        rest.getNextAudioBlock (juce::AudioSourceChannelInfo (&restView, 0, info.numSamples));

        if (buffer.getNumChannels() >= 4)
        {
            float* drumChannels[] = { buffer.getWritePointer (2), buffer.getWritePointer (3) };
            juce::AudioBuffer<float> drumView (drumChannels, 2, info.startSample, info.numSamples);
            drums.getNextAudioBlock (juce::AudioSourceChannelInfo (&drumView, 0, info.numSamples));
        }
        else
        {
            drums.setNextReadPosition (drums.getNextReadPosition() + info.numSamples);   // 위치만 맞춘다
        }
    }

    void setNextReadPosition (juce::int64 position) override
    {
        rest.setNextReadPosition (position);
        drums.setNextReadPosition (position);
    }

    juce::int64 getNextReadPosition() const override   { return rest.getNextReadPosition(); }
    juce::int64 getTotalLength() const override        { return rest.getTotalLength(); }
    bool isLooping() const override                    { return false; }

private:
    juce::AudioFormatReaderSource rest, drums;
};

//==============================================================================
int MidiInbox::readMonitor (MonitorEvent* destination, int maxEvents) noexcept
{
    int s1, n1, s2, n2;
    monitorFifo.prepareToRead (maxEvents, s1, n1, s2, n2);

    for (int i = 0; i < n1; ++i)
        destination[i] = monitorEvents[(size_t) (s1 + i)];

    for (int i = 0; i < n2; ++i)
        destination[n1 + i] = monitorEvents[(size_t) (s2 + i)];

    monitorFifo.finishedRead (n1 + n2);
    return n1 + n2;
}

bool MidiInbox::shouldForward (const juce::uint8* data, int size) noexcept
{
    if (data == nullptr || size < 1 || size > 3)   // 시스템 익스클루시브(긴 메시지)는 드럼에 필요 없다
        return false;

    const auto status = data[0];

    if (status < 0x80 || status >= 0xf0)   // 클록·시작/정지·액티브 센싱 등 시스템 메시지
        return false;

    if ((status & 0xf0) == 0xc0)           // Program Change(모듈에서 킷을 바꿀 때 보낸다)
        return false;

    if ((status & 0xf0) == 0xb0 && size >= 2 && (data[1] == 0 || data[1] == 32))   // Bank Select
        return false;

    return true;
}

MidiInbox::MidiInbox()
{
    for (auto& note : noteMap)
        note.store (keepNote);

    for (auto& velocity : minVelocity)
        velocity.store (0);
}

void MidiInbox::setNoteMap (const std::array<juce::uint8, 128>& map, bool enabled) noexcept
{
    for (size_t i = 0; i < map.size(); ++i)
        noteMap[i].store (map[i], std::memory_order_relaxed);

    noteMapOn.store (enabled, std::memory_order_relaxed);
}

void MidiInbox::setMinVelocity (const std::array<juce::uint8, 128>& table) noexcept
{
    for (size_t i = 0; i < table.size(); ++i)
        minVelocity[i].store (table[i], std::memory_order_relaxed);
}

void MidiInbox::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message)
{
    receive (message, juce::Time::getMillisecondCounterHiRes());
}

void MidiInbox::receive (const juce::MidiMessage& message, double time)
{
    const int size = message.getRawDataSize();
    bool forward = shouldForward (message.getRawData(), size);
    bool weak = false;
    juce::uint8 mapped = keepNote;

    // 음 켬·끔·초크(폴리 애프터터치): 약한 신호는 버리고, 패드 맞추기 창에 알리고, 패드 배치가 켜져 있으면 음 번호를 바꾼다.
    // 버린 음의 '음 끔'은 그대로 넘긴다(드럼 플러그인에는 해가 없다).
    if (size == 3)
    {
        const auto* raw = message.getRawData();
        const int status = raw[0] & 0xf0;

        if (status == 0x90 && raw[2] > 0 && raw[1] < 128 && raw[2] < minVelocity[raw[1]].load (std::memory_order_relaxed))
        {
            weak = true;
            forward = false;
        }

        if (status == 0x90 && raw[2] > 0 && ! weak)
            lastNoteOn.store ((((lastNoteOn.load (std::memory_order_relaxed) >> 8) + 1) << 8) | raw[1], std::memory_order_relaxed);

        if (! weak && (status == 0x90 || status == 0x80 || status == 0xa0) && raw[1] < 128 && noteMapOn.load (std::memory_order_relaxed))
        {
            const auto to = noteMap[raw[1]].load (std::memory_order_relaxed);

            if (to == dropNote)
                forward = false;
            else if (to < 128 && to != raw[1])
                mapped = to;
        }
    }

    // '신호 보기'용 사본(거른 신호도). 짧은 메시지만, 액티브 센싱(0xFE)은 너무 잦아 뺀다.
    if (size >= 1 && size <= 3 && message.getRawData()[0] != 0xfe)
    {
        int s1, n1, s2, n2;
        monitorFifo.prepareToWrite (1, s1, n1, s2, n2);

        if (n1 + n2 > 0)
        {
            auto& copy = monitorEvents[(size_t) (n1 > 0 ? s1 : s2)];
            copy.time = time;
            copy.size = (juce::uint8) size;
            std::memcpy (copy.data, message.getRawData(), (size_t) size);
            copy.forwarded = forward;
            copy.weak = weak;
            copy.mappedNote = mapped;
            monitorFifo.finishedWrite (1);
        }
    }

    if (! forward)
        return;

    int start1, size1, start2, size2;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 + size2 < 1)
        return;   // 넘치면 버린다(정상적으로는 일어나지 않는다)

    auto& event = events[(size_t) (size1 > 0 ? start1 : start2)];
    event.time = time;
    event.size = (juce::uint8) size;
    std::memcpy (event.data, message.getRawData(), (size_t) size);
    event.rawNote = size >= 2 ? event.data[1] : 0;

    if (mapped != keepNote)
        event.data[1] = mapped;

    fifo.finishedWrite (1);
}

double MidiInbox::drainInto (juce::MidiBuffer& destination, juce::MidiBuffer* raw, int numSamples, double sampleRate, double delayMs, double now) noexcept
{
    const int ready = fifo.getNumReady();

    if (ready <= 0)
        return -1.0;

    const double blockMs = numSamples * 1000.0 / sampleRate;
    const double staleMs = juce::jmax (40.0, 3.0 * blockMs);   // 장치를 다시 여는 동안 쌓인 신호는 한꺼번에 울리지 않게 버린다

    int start1, size1, start2, size2;
    fifo.prepareToRead (ready, start1, size1, start2, size2);

    const int total = size1 + size2;
    const auto eventAt = [&] (int i) -> const Event&
    {
        return events[(size_t) (i < size1 ? start1 + i : start2 + (i - size1))];
    };

    // 블록 맨 앞(0번 샘플)에 해당하는 받은 시각. 이 시각보다 늦게 받은 신호는 받은 뒤 정확히 delay 만에 울린다.
    double reference = now - juce::jlimit (0.0, blockMs, delayMs);

    // 이 시각보다 먼저 받은 신호(delay보다 오래 기다린 신호)가 있으면 이번에 들어온 첫 음(note-on)을 블록 맨 앞에 놓고,
    // 나머지는 그 음과의 간격을 살려 뒤에 놓는다. 하이햇 페달(CC) 같은 신호가 먼저 왔다고 음이 밀리지 않게
    // 기준은 첫 음이다(음이 없으면 첫 신호). delay가 0이면 늘 이 경우다.
    {
        double firstAny = -1.0, firstNote = -1.0;

        for (int i = 0; i < total; ++i)
        {
            const auto& event = eventAt (i);

            if (now - event.time > staleMs)
                continue;

            if (firstAny < 0.0)
                firstAny = event.time;

            if (firstNote < 0.0 && event.size >= 3 && (event.data[0] & 0xf0) == 0x90 && event.data[2] > 0)
                firstNote = event.time;
        }

        const double first = firstNote >= 0.0 ? firstNote : firstAny;

        if (first >= 0.0 && first < reference)
            reference = first;
    }

    const int lastIndex = juce::jmax (0, numSamples - 1);
    double waited = -1.0;
    int lastPos = 0;

    for (int i = 0; i < total; ++i)
    {
        const auto& event = eventAt (i);

        if (now - event.time > staleMs)
            continue;

        waited = juce::jmax (waited, now - event.time);
        const int pos = juce::jlimit (lastPos, lastIndex, juce::roundToInt ((event.time - reference) * 0.001 * sampleRate));
        lastPos = pos;
        destination.addEvent (event.data, (int) event.size, pos);

        if (raw != nullptr)
        {
            juce::uint8 original[3] { event.data[0], event.data[1], event.data[2] };

            if (event.size >= 2)
                original[1] = event.rawNote;

            raw->addEvent (original, (int) event.size, pos);
        }
    }

    fifo.finishedRead (total);
    return waited;
}

bool MidiInbox::mapForPlugin (juce::uint8* data, int size) const noexcept
{
    if (size != 3)
        return true;

    const int status = data[0] & 0xf0;

    if ((status == 0x90 || status == 0x80 || status == 0xa0) && data[1] < 128 && noteMapOn.load (std::memory_order_relaxed))
    {
        const auto to = noteMap[data[1]].load (std::memory_order_relaxed);

        if (to == dropNote)
            return false;

        if (to < 128)
            data[1] = to;
    }

    return true;
}

//==============================================================================
void Engine::Strip::prepare (double newSampleRate)
{
    eq.prepare (newSampleRate);
    comp.prepare (newSampleRate);
    gain.reset (newSampleRate, 0.03);
}

void Engine::Strip::processTone (float* left, float* right, int numSamples, ChannelSettings& settings) noexcept
{
    eq.setGains (settings.low.load (std::memory_order_relaxed),
                 settings.mid.load (std::memory_order_relaxed),
                 settings.high.load (std::memory_order_relaxed));
    eq.process (left, right, numSamples);

    comp.setAmount (settings.comp.load (std::memory_order_relaxed));
    comp.process (left, right, numSamples);
}

void Engine::Strip::applyFader (float* left, float* right, int numSamples, ChannelSettings& settings) noexcept
{
    gain.setTargetValue (faderGain (settings, true));
    applyGain (gain, left, right, numSamples);

    pushPeak (settings.peakL, blockPeak (left, numSamples));
    pushPeak (settings.peakR, blockPeak (right, numSamples));
}

//==============================================================================
Engine::Engine (bool openAudioDevice, juce::PropertiesFile* settings)
    : usesDevices (openAudioDevice)
{
    juce::addDefaultFormatsToManager (pluginFormats);
    audioFormats.registerBasicFormats();

    song.volume = 0.7f;

    incomingMidi.ensureSize (4096);
    incomingRaw.ensureSize (4096);
    chunkMidi.ensureSize (4096);
    uiMidi.ensureSize (1024);

    readThread.startThread();
    recordThread.startThread();

    if (settings != nullptr)
        loadSettings (*settings);

    prepare (48000.0, 512);

    if (openAudioDevice)
    {
        if (wantedAudioState == nullptr)
            wantedAudioState = makeDrumModuleAsioState (deviceManager, identifyDrumModule (juce::MidiInput::getAvailableDevices()));

        openWantedAudioDevice();
        deviceManager.addAudioCallback (this);
        deviceManager.addMidiInputDeviceCallback ({}, &midiInbox);
        refreshMidiInputs();

        // 드럼 모듈을 껐다 켜거나 USB를 다시 꽂으면 1초마다 하는 확인을 기다리지 않고 바로 다시 연결한다.
        midiListConnection = juce::MidiDeviceListConnection::make ([this] { refreshMidiInputs(); });
    }
}

Engine::~Engine()
{
    midiListConnection.reset();
    deviceManager.removeMidiInputDeviceCallback ({}, &midiInbox);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    stopRecording();
    setPreview (nullptr);

    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
    stemSource.reset();
    thumbnail.setSource (nullptr);

    unloadPlugin();

    readThread.stopThread (2000);
    recordThread.stopThread (2000);
}

//==============================================================================
void Engine::openWantedAudioDevice()
{
    deviceManager.initialise (0, 2, wantedAudioState.get(), true);

    // 원하던 장치(예: 드럼이 꺼져 있을 때의 ASIO)를 못 열면 JUCE는 같은 방식 안에서만 다시 찾는다.
    // 그래도 소리는 나야 하므로 Windows 기본 출력으로 넘어간다. 저장된 선택은 그대로 남는다.
    if (deviceManager.getCurrentAudioDevice() == nullptr)
    {
        deviceManager.setCurrentAudioDeviceType ("Windows Audio", false);

        if (deviceManager.getCurrentAudioDevice() == nullptr)
            deviceManager.initialise (0, 2, nullptr, true);
    }

    wantedAudioOpen = isWantedDeviceOpen();
}

bool Engine::isWantedDeviceOpen() const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (wantedAudioState == nullptr)
        return device != nullptr;

    const auto type = wantedAudioState->getStringAttribute ("deviceType");
    const auto name = wantedAudioState->getStringAttribute ("audioOutputDeviceName");

    return device != nullptr
        && (type.isEmpty() || device->getTypeName() == type)
        && (name.isEmpty() || device->getName() == name);
}

void Engine::adoptCurrentDeviceAsWanted()
{
    if (deviceManager.getCurrentAudioDevice() != nullptr)
        if (auto xml = deviceManager.createStateXml())
            wantedAudioState = std::move (xml);

    wantedAudioOpen = isWantedDeviceOpen();
    audioRetriesLeft = 0;
}

juce::String Engine::getAudioDeviceName() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getName();

    return {};
}

juce::String Engine::getAudioDeviceType() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getTypeName();

    return {};
}

double Engine::getDeviceSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 0.0;
}

int Engine::getBufferSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    return 0;
}

juce::Array<int> Engine::getAvailableBufferSizes() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getAvailableBufferSizes();

    return {};
}

bool Engine::setBufferSize (int samples, juce::String& error)
{
    auto setup = deviceManager.getAudioDeviceSetup();

    if (setup.bufferSize == samples && getBufferSize() == samples)
        return true;

    setup.bufferSize = samples;
    error = deviceManager.setAudioDeviceSetup (setup, true);
    return error.isEmpty();
}

bool Engine::hasDriverPanel() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr && device->hasControlPanel();
}

void Engine::showDriverPanel()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr || ! device->hasControlPanel())
        return;

    // JUCE 설정 창과 같은 방식: 드라이버 창이 떠 있는 동안 이 앱의 창을 막고, 바뀐 값이 있으면 장치를 다시 연다.
    juce::Component modalWindow;
    modalWindow.setOpaque (true);
    modalWindow.addToDesktop (0);
    modalWindow.enterModalState();

    if (device->showControlPanel())
        restartAudioDevice();
}

void Engine::restartAudioDevice()
{
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return;

    setQuietFor (4000);   // 다시 여는 동안 늦은 콜백은 끊김으로 세지 않는다
    deviceManager.closeAudioDevice();
    deviceManager.restartLastAudioDevice();
}

float Engine::getOutputLatencyMs() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();

        if (rate > 0.0)
            return (float) (juce::jmax (device->getCurrentBufferSizeSamples(), device->getOutputLatencyInSamples()) * 1000.0 / rate);
    }

    return -1.0f;
}

Engine::LatencyBreakdown Engine::getLatencyBreakdown() const
{
    LatencyBreakdown b;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        b.rate = device->getCurrentSampleRate();
        b.buffer = device->getCurrentBufferSizeSamples();

        if (! b.isValid())
            return {};

        // 장치가 알려 주는 출력 지연은 지금 채우는 블록까지 포함한다(ASIO 규약). 그보다 작게 알려 주면 버퍼만 센다.
        b.reportedOutput = device->getOutputLatencyInSamples();
        b.reportedInput = device->getInputLatencyInSamples();
        b.driverExtra = juce::jmax (0, b.reportedOutput - b.buffer);

        // 패드 신호는 블록 사이 아무 때나 온다. 고정 시간 D만큼 늦춰 놓되, 이미 D보다 오래 기다린 신호는 블록 맨 앞에 놓는다.
        // 기다린 시간이 0~버퍼(B) 사이에 고르게 퍼져 있으면 평균 대기는 (B² + D²) / 2B 이다(D = 0이면 B/2, D = B면 B).
        const double block = b.buffer;
        b.midiDelay = juce::jmin (block, midiDelayMs (b.buffer, b.rate) * b.rate * 0.001);
        b.midiWait = (block * block + b.midiDelay * b.midiDelay) / (2.0 * block);

        // 플러그인이 스스로 늦추는 만큼(SSD 5.5는 0)
        b.plugin = plugin != nullptr ? juce::jmax (0, plugin->getLatencySamples()) : 0;
        b.pluginName = pluginName;
    }

    return b;
}

float Engine::getDriverExtraLatencyMs() const
{
    const auto b = getLatencyBreakdown();
    return b.isValid() ? (float) b.toMs (b.driverExtra) : -1.0f;
}

float Engine::getEstimatedLatencyMs() const
{
    const auto b = getLatencyBreakdown();
    return b.isValid() ? (float) b.toMs (b.totalSamples()) : -1.0f;
}

Engine::MonitorSnapshot Engine::getMonitor() const noexcept
{
    MonitorSnapshot s;
    s.callbacks       = monCallbacks.load();
    s.ownOverruns     = monOwnOverruns.load();
    s.lateNear        = monLateNear.load();
    s.late            = monLate.load();
    s.nearMiss        = monNearMiss.load();
    s.likelyDropouts  = monLikelyDropouts.load();
    s.warmupAnomalies = monWarmupAnomalies.load();
    s.quietEvents     = monQuietEvents.load();
    s.streamStarts    = monStreamStarts.load();
    s.pluginSkips     = monPluginSkips.load();
    s.maxDurationMs   = monMaxDurationMs.load();
    s.maxExcessMs     = monMaxExcessMs.load();
    s.blockMs         = monBlockMs.load();
    s.headroomMs      = monHeadroomMs.load();
    return s;
}

void Engine::setQuietFor (double milliseconds) noexcept
{
    quietUntilMs = juce::Time::getMillisecondCounterHiRes() + milliseconds;
}

int Engine::getDriverOverloads() const
{
    // JUCE의 ASIO 장치는 드라이버가 과부하를 알려 줄 수 없으면 -1을 돌려준다.
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getXRunCount();

    return -1;
}

juce::uint32 Engine::getProblemCount() const noexcept
{
    return monOwnOverruns.load() + monLikelyDropouts.load() + monStreamStarts.load() + (juce::uint32) juce::jmax (0, getDriverOverloads());
}

void Engine::refreshMidiInputs()
{
    // 장치 없이 만든 엔진(자체 점검·화면 확인용 --no-audio)은 사용 중인 앱의 드럼 MIDI를 가로채지 않는다.
    if (! usesDevices)
        return;

    // 장치를 다시 여는 과정이 MIDI 입력 목록을 비우므로, 재시도를 먼저 하고 그 뒤에 입력을 켠다.
    if (audioRetriesLeft > 0 && ! isWantedDeviceOpen())
    {
        --audioRetriesLeft;
        openWantedAudioDevice();
    }

    const auto available = juce::MidiInput::getAvailableDevices();

    // 사라진 장치는 닫는다. 끊긴 연결이 '켜짐'으로 남아 있으면 모듈을 다시 켜도 새로 열지 않아 패드가 먹통이 된다.
    for (int i = enabledMidiIds.size(); --i >= 0;)
    {
        const auto id = enabledMidiIds[i];
        const bool present = std::any_of (available.begin(), available.end(), [&] (const juce::MidiDeviceInfo& d) { return d.identifier == id; });

        if (! present)
        {
            deviceManager.setMidiInputDeviceEnabled (id, false);
            enabledMidiIds.remove (i);
        }
    }

    juce::StringArray names;
    bool added = false;

    for (const auto& device : available)
    {
        if (! deviceManager.isMidiInputDeviceEnabled (device.identifier))
            deviceManager.setMidiInputDeviceEnabled (device.identifier, true);

        enabledMidiIds.addIfNotAlreadyThere (device.identifier);
        added = added || ! midiInputNames.contains (device.name);
        names.add (device.name);
    }

    // 드럼이 방금 (다시) 연결됐는데 원하던 소리 장치가 열려 있지 않으면, 곧 준비될 테니 몇 번 더 열어 본다.
    if (added && midiScanned && ! isWantedDeviceOpen())
    {
        // 다른 회사 모듈로 바꿔 꽂은 경우(예: Yamaha → Roland): 원하던 장치가 드럼 모듈 드라이버였고 지금 열리지 않으면,
        // 새 모듈의 드라이버로 바꾼다. 사용자가 고른 다른 오디오 장치(드럼 모듈이 아닌 것)는 건드리지 않는다.
        const auto module = identifyDrumModule (available);
        const auto wantedName = wantedAudioState != nullptr ? wantedAudioState->getStringAttribute ("audioOutputDeviceName") : juce::String();
        const auto wantedBrand = driverBrand (wantedName);

        if (module.isKnown() && (wantedAudioState == nullptr || wantedBrand != DrumModule::Brand::none) && wantedBrand != module.brand)
            if (auto xml = makeDrumModuleAsioState (deviceManager, module); xml != nullptr && driverBrand (xml->getStringAttribute ("audioOutputDeviceName")) == module.brand)
                wantedAudioState = std::move (xml);

        audioRetriesLeft = 4;
    }

    midiInputNames = names;
    midiInputDevices = available;
    midiScanned = true;
}

juce::StringArray Engine::getAsioDriverNames()
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == "ASIO")
            return type->getDeviceNames (false);

    return {};
}

void Engine::addMidiFromUi (const juce::MidiMessage& message)
{
    const juce::SpinLock::ScopedLockType lock (uiMidiLock);
    uiMidi.addEvent (message, 0);
}

void Engine::playTestNote (int noteNumber, float velocity)
{
    addMidiFromUi (juce::MidiMessage::noteOn (10, noteNumber, velocity));

    juce::WeakReference<Engine> weak (this);
    callOnceAfterDelay (160, [weak, noteNumber]
    {
        if (auto* engine = weak.get())
            engine->addMidiFromUi (juce::MidiMessage::noteOff (10, noteNumber));
    });
}

//==============================================================================
std::vector<PluginEntry> Engine::findInstalledPlugins()
{
    // 파일만 훑는다(플러그인을 띄우지 않는다). 드럼 VST는 이름·정보 파일로 알아보고 목록 앞에 둔다.
    std::vector<PluginEntry> result;
    juce::StringArray seen;

    // asDrum: 사용자가 드럼 VST로 직접 추가한 파일(이름으로 알아보지 못해도 드럼 목록에 둔다)
    const auto add = [&] (const juce::String& path, bool asDrum)
    {
        if (seen.contains (path, true))
            return;

        seen.add (path);
        const juce::File file (path);
        const auto drum = recogniseDrumPlugin (file);
        result.push_back ({ drum.isDrum ? drum.displayName : file.getFileNameWithoutExtension(), path,
                            drum.isDrum || asDrum, asDrum, drum.isDrum ? drum.rank : 150 });
    };

    // 직접 추가한 것을 먼저 넣어야 기본 위치에 있는 같은 파일도 '직접 추가'로 남는다.
    // .vst3 파일(묶음 폴더 포함)은 드럼 VST로, 그 밖의 폴더는 안을 훑어 찾는다.
    for (const auto& path : extraPluginPaths)
        if (const juce::File file (path); file.exists() && file.hasFileExtension ("vst3"))
            add (path, true);

    for (auto* format : pluginFormats.getFormats())
        for (const auto& path : format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false))
            add (path, false);

    for (const auto& path : extraPluginPaths)
        if (const juce::File file (path); file.isDirectory() && ! file.hasFileExtension ("vst3"))
            for (auto* format : pluginFormats.getFormats())
                for (const auto& found : format->searchPathsForPlugins (juce::FileSearchPath (path), true, false))
                    add (found, false);

    std::sort (result.begin(), result.end(), [] (const PluginEntry& a, const PluginEntry& b)
    {
        if (a.isDrum != b.isDrum)
            return a.isDrum;

        if (a.isDrum && a.rank != b.rank)
            return a.rank < b.rank;

        return a.name.compareIgnoreCase (b.name) < 0;
    });

    return result;
}

void Engine::addExtraPluginPath (const juce::String& path)
{
    extraPluginPaths.addIfNotAlreadyThere (path, true);
}

juce::MemoryBlock Engine::getSavedPluginState() const
{
    const auto found = pluginStates.find (savedPluginPath);
    return found != pluginStates.end() ? found->second : juce::MemoryBlock();
}

void Engine::rememberCurrentPluginState()
{
    if (plugin == nullptr || pluginPath.isEmpty())
        return;

    juce::MemoryBlock state;
    plugin->getStateInformation (state);

    if (! state.isEmpty())
        pluginStates[pluginPath] = std::move (state);
}

void Engine::preparePlugin (juce::AudioPluginInstance& instance, juce::AudioBuffer<float>& buffer, std::vector<OutBus>& buses) const
{
    instance.setProcessingPrecision (juce::AudioProcessor::singlePrecision);
    instance.setRateAndBufferSizeDetails (sampleRate, maxBlock);
    instance.prepareToPlay (sampleRate, maxBlock);

    const int channels = juce::jmax (2, instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());
    buffer.setSize (channels, maxBlock, false, true, false);

    // 플러그인 안에서 출력을 여러 갈래로 나눠 놓았더라도 전부 들리도록 모든 출력 버스를 합친다.
    buses.clear();
    int start = 0;

    for (int bus = 0; bus < instance.getBusCount (false); ++bus)
    {
        const int count = instance.getChannelCountOfBus (false, bus);

        if (count > 0)
            buses.push_back ({ start, count });

        start += count;
    }
}

bool Engine::loadPlugin (const juce::String& pathToLoad, const juce::MemoryBlock* stateToUse, juce::String& error)
{
    // 플러그인에 따라(예: BFD Player) 불러오는 동안 메시지를 처리하므로, 그사이 다른 일이 이 함수를
    // 다시 부르거나 넘겨받은 값을 지울 수 있다. 겹쳐 부르면 거절하고, 넘겨받은 값은 복사해서 쓴다.
    if (pluginLoadInProgress)
    {
        error = K ("다른 플러그인을 불러오는 중입니다. 잠시 뒤에 다시 골라 주세요.");
        return false;
    }

    if (renderingDrums.load())
    {
        error = K ("고친 MIDI로 드럼을 다시 만드는 중에는 드럼 VST를 바꿀 수 없습니다. 끝난 뒤에 다시 골라 주세요.");
        return false;
    }

    const juce::ScopedValueSetter<bool> busy (pluginLoadInProgress, true);
    const juce::String path = pathToLoad;
    std::optional<juce::MemoryBlock> givenState;

    if (stateToUse != nullptr)
        givenState = *stateToUse;

    juce::OwnedArray<juce::PluginDescription> types;

    for (auto* format : pluginFormats.getFormats())
        if (format->fileMightContainThisPluginType (path))
            format->findAllTypesForFile (types, path);

    if (types.isEmpty())
    {
        error = K ("플러그인 파일을 읽을 수 없습니다. VST3 형식(.vst3)인지 확인해 주세요.");
        return false;
    }

    const juce::PluginDescription* chosen = types.getFirst();

    for (const auto* type : types)
    {
        if (type->isInstrument)
        {
            chosen = type;
            break;
        }
    }

    const auto chosenName = chosen->name;   // 아래에서 메시지가 돌아도 types는 이 함수 것이라 남지만, 이름은 미리 둔다
    auto instance = pluginFormats.createPluginInstance (*chosen, sampleRate, maxBlock, error);

    if (instance == nullptr)
    {
        if (error.isEmpty())
            error = K ("플러그인을 시작하지 못했습니다.");

        return false;
    }

    // 지금 쓰던 플러그인의 킷·설정을 기억해 두고, 새 플러그인은 지난번 상태로 되살린다.
    rememberCurrentPluginState();

    juce::MemoryBlock state;

    if (givenState.has_value())
        state = *givenState;
    else if (const auto found = pluginStates.find (path); found != pluginStates.end())
        state = found->second;   // 복사: 불러오는 동안 저장이 끼어들어 원본이 바뀌어도 안전하게

    if (! state.isEmpty())
        instance->setStateInformation (state.getData(), (int) state.getSize());

    juce::AudioBuffer<float> newBuffer;
    std::vector<OutBus> newBuses;
    preparePlugin (*instance, newBuffer, newBuses);

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        std::swap (plugin, instance);
        std::swap (pluginBuffer, newBuffer);
        std::swap (pluginBuses, newBuses);
    }

    if (instance != nullptr)   // 이전 플러그인
    {
        instance->releaseResources();
        instance.reset();
    }

    pluginPath = path;
    pluginName = chosenName;
    return true;
}

void Engine::unloadPlugin()
{
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        std::swap (plugin, old);
        pluginBuses.clear();
    }

    if (old != nullptr)
        old->releaseResources();

    pluginPath.clear();
    pluginName.clear();
}

juce::String Engine::describePluginBuses() const
{
    if (plugin == nullptr)
        return "(none)";

    juce::String text;
    text << "inputs=" << plugin->getTotalNumInputChannels()
         << " outputs=" << plugin->getTotalNumOutputChannels()
         << " outputBuses=" << plugin->getBusCount (false)
         << " activeBuses=" << (int) pluginBuses.size()
         << " acceptsMidi=" << (plugin->acceptsMidi() ? 1 : 0)
         << " hasEditor=" << (plugin->hasEditor() ? 1 : 0)
         << " latencySamples=" << plugin->getLatencySamples();
    return text;
}

//==============================================================================
juce::String Engine::getSongWildcard()
{
    return audioFormats.getWildcardForAllFormats();
}

bool Engine::loadSong (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = K ("파일을 찾을 수 없습니다.");
        return false;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0)
    {
        error = K ("이 파일은 열 수 없습니다. MP3, WAV, FLAC 파일을 써 주세요.");
        return false;
    }

    const double fileRate = reader->sampleRate;
    auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    // 예비박 중이면 그만둔다(그대로 두면 오디오 스레드가 바꾸는 도중의 재생기를 시작하려 든다).
    {
        const juce::SpinLock::ScopedLockType lock (countInLock);
        countInRemaining.store (0);
    }

    transport.stop();
    transport.setSource (nullptr);
    stemSource.reset();
    readerSource = std::move (newSource);
    transport.setSource (readerSource.get(), 32768, &readThread, fileRate, 2);
    transport.setPosition (0.0);

    thumbnail.setSource (new juce::FileInputSource (file));
    songFile = file;
    setBeatMap ({});   // 박자표는 곡마다 다시 정한다
    return true;
}

bool Engine::useSongStems (const juce::File& restFile, const juce::File& drumsFile, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> restReader (audioFormats.createReaderFor (restFile));
    std::unique_ptr<juce::AudioFormatReader> drumsReader (audioFormats.createReaderFor (drumsFile));

    if (restReader == nullptr || drumsReader == nullptr)
    {
        error = K ("분리된 파일을 열 수 없습니다.");
        return false;
    }

    if (std::abs (restReader->sampleRate - drumsReader->sampleRate) > 0.5)
    {
        error = K ("분리된 두 파일의 샘플 속도가 다릅니다.");
        return false;
    }

    const double rate = restReader->sampleRate;
    auto newSource = std::make_unique<StemPairSource> (restReader.release(), drumsReader.release());

    // 예비박 중이었으면 먼저 멈춘다. 오디오 스레드는 예비박을 끝낼 때 같은 잠금 안에서 재생기를 시작하므로,
    // 잠금 뒤에 읽은 위치·재생 상태는 그 시작까지 반영한 값이다.
    bool hadCountIn = false;
    {
        const juce::SpinLock::ScopedLockType lock (countInLock);
        hadCountIn = countInRemaining.exchange (0) > 0;
    }

    // 듣던 위치와 재생 상태를 그대로 이어 간다.
    const double position = transport.getCurrentPosition();
    const bool wasPlaying = transport.isPlaying();

    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
    stemSource = std::move (newSource);
    transport.setSource (stemSource.get(), 32768, &readThread, rate, 4);   // 4채널: 나머지 L R, 드럼 L R
    transport.setPosition (position);

    if (wasPlaying)
        transport.start();
    else if (hadCountIn)
        play();   // 같은 자리에서 예비박부터 다시

    return true;
}

void Engine::setSongDrumsOn (bool on)
{
    songDrumsOn = on;   // 오디오 스레드가 30ms에 걸쳐 바꾼다
}

//==============================================================================
namespace
{
int firstDownbeatOf (const BeatMap& map)
{
    for (size_t i = 0; i < map.downbeat.size(); ++i)
        if (map.downbeat[i] != 0)
            return (int) i;

    return 0;
}

int positiveModulo (int value, int divisor) noexcept
{
    return ((value % divisor) + divisor) % divisor;
}

double normaliseClickSpeed (double speed) noexcept
{
    return speed >= 1.5 ? 2.0 : (speed <= 0.75 ? 0.5 : 1.0);
}
} // namespace

void Engine::setBeatMap (BeatMap map, double newClickSpeed)
{
    const bool valid = map.isValid();
    const int firstDownbeat = firstDownbeatOf (map);
    const double speed = normaliseClickSpeed (newClickSpeed);

    // 클릭 시각표: 2배면 박과 박 사이 한가운데에도 치고, 절반이면 마디 첫 박부터 한 박 걸러 친다.
    // 높은 클릭은 원곡의 마디 첫 박에만 둔다(사이에 넣은 클릭은 낮은 클릭).
    BeatMap clicks;
    clicks.method = map.method;
    clicks.bpm = map.bpm * speed;

    if (valid && speed == 2.0)
    {
        for (size_t i = 0; i < map.beats.size(); ++i)
        {
            clicks.beats.push_back (map.beats[i]);
            clicks.downbeat.push_back (map.downbeat[i]);

            if (i + 1 < map.beats.size())
            {
                clicks.beats.push_back (0.5 * (map.beats[i] + map.beats[i + 1]));
                clicks.downbeat.push_back (0);
            }
        }
    }
    else if (valid && speed == 0.5)
    {
        for (size_t i = 0; i < map.beats.size(); ++i)
        {
            if (positiveModulo ((int) i - firstDownbeat, 2) == 0)
            {
                clicks.beats.push_back (map.beats[i]);
                clicks.downbeat.push_back (map.downbeat[i]);
            }
        }
    }
    else
    {
        clicks.beats = map.beats;
        clicks.downbeat = map.downbeat;
    }

    // 아주 짧은 곡에서 한 박 걸렀더니 박이 모자라면 찾은 박 그대로 친다(메트로놈이 통째로 꺼지지 않게).
    double usedSpeed = speed;

    if (valid && ! clicks.isValid())
    {
        clicks.beats = map.beats;
        clicks.downbeat = map.downbeat;
        clicks.bpm = map.bpm;
        usedSpeed = 1.0;
    }

    const int clickFirst = firstDownbeatOf (clicks);
    const int perBar = usedSpeed == 2.0 ? 8 : (usedSpeed == 0.5 ? 2 : 4);

    {
        const juce::SpinLock::ScopedLockType lock (beatLock);
        std::swap (beatMap, map);
        std::swap (clickMap, clicks);
        beatMapFirstDownbeat = firstDownbeat;
        clickFirstDownbeat = clickFirst;
        clicksPerBar = perBar;
        clickSpeed = usedSpeed;
        nextBeat = 0;
        expectedSongTime = -1.0;
    }

    beatMapValid = valid && clickMap.isValid();
    // 예전 박자표(map, clicks)는 여기, 메시지 스레드에서 정리된다.
}

int Engine::prepareCountIn (double start)
{
    // 박자표는 메시지 스레드만 바꾸므로 여기서는 잠그지 않고 읽어도 된다.
    // 장치가 없으면 예비박을 셀 오디오 스레드도 없어서 원곡이 영영 시작되지 않으므로 세지 않는다.
    if (! metronomeOn.load() || ! countInEnabled.load() || ! beatMapValid.load()
        || ! (isAudioDeviceOpen() || renderingOffline.load()))
        return 0;

    const auto& beats = beatMap.beats;
    const double firstPeriod = beats[1] - beats[0];
    double next = 0.0, period = firstPeriod;
    int nextIndex = 0;   // 박자표에서 next의 번호(첫 박보다 앞이면 음수)

    if (firstPeriod < 0.15)
        return 0;

    if (start <= beats[0])
    {
        // 첫 박보다 앞(전주 전 무음 등)은 첫 박에서 같은 간격으로 거슬러 올라간 박을 쓴다.
        const double steps = std::floor ((beats[0] - start) / firstPeriod + 1.0e-9);
        next = beats[0] - steps * firstPeriod;
        nextIndex = -(int) steps;
    }
    else
    {
        const auto it = std::lower_bound (beats.begin(), beats.end(), start - 1.0e-6);

        if (it == beats.end())
            return 0;

        const auto n = (size_t) (it - beats.begin());
        next = beats[n];
        nextIndex = (int) n;
        period = n > 0 ? beats[n] - beats[n - 1] : firstPeriod;
    }

    if (period < 0.15 || period > 2.0)
        return 0;

    // 시작 위치 다음에 오는 박 바로 앞의 4박을 먼저 들려준다. 원곡은 정확히 start부터 시작한다.
    // 높은 클릭은 원곡의 마디 첫 박 자리에 둔다(곡 중간에서 시작해도 마디가 이어진다).
    // 빠르기가 2배면 박 사이에도, 절반이면 원곡에서 치는 박과 같은 자리(마디 첫 박부터 한 박 걸러)에만 친다.
    int count = 0;

    for (int i = 0; i < 4; ++i)
    {
        const double t = next - (4 - i) * period;
        const int index = nextIndex - 4 + i;
        const bool accent = index >= 0 ? beatMap.downbeat[(size_t) index] != 0
                                       : positiveModulo (index - beatMapFirstDownbeat, 4) == 0;

        // 예비박 클릭은 원곡 시작 위치보다 앞의 것만 둔다(시작 위치부터는 박자표에서 친다: 두 번 치지 않게).
        if ((clickSpeed != 0.5 || positiveModulo (index - beatMapFirstDownbeat, 2) == 0) && t < start - 1.0e-9)
        {
            countInBeats[(size_t) count] = t;
            countInAccent[(size_t) count] = accent;
            ++count;
        }

        if (clickSpeed == 2.0 && t + 0.5 * period < start - 1.0e-9)
        {
            countInBeats[(size_t) count] = t + 0.5 * period;
            countInAccent[(size_t) count] = false;
            ++count;
        }
    }

    countInCount = count;
    countInStart = start;

    // 3~4박(첫 클릭을 건너뛰어도 길이는 같다). 클릭을 앞당겼으면 그만큼 일찍 시작해 첫 예비박이 빠지지 않게 한다.
    const double earlier = juce::jmax (0.0, -(double) metronomeOffsetMs.load() * 0.001);
    const double preRoll = start - (next - 4 * period) + earlier;
    const int block = juce::jmax (1, maxBlock);
    return (int) std::ceil (preRoll * sampleRate / block) * block;
}

void Engine::startClick (bool accent, int offsetInBlock) noexcept
{
    for (auto& voice : clickVoices)
    {
        if (voice.data == nullptr)
        {
            const auto& wave = accent ? clickAccent : clickNormal;
            voice.data = wave.data();
            voice.length = (int) wave.size();
            voice.position = -offsetInBlock;
            return;
        }
    }
}

void Engine::makeClickSounds()
{
    // 짧게 울리는 두 음높이의 클릭. 마디 첫 박은 더 높고 크다.
    const auto make = [this] (double frequency, float level)
    {
        std::vector<float> wave ((size_t) juce::jmax (16, (int) (sampleRate * 0.045)));
        const double twoPi = juce::MathConstants<double>::twoPi;

        for (size_t i = 0; i < wave.size(); ++i)
        {
            const double t = (double) i / sampleRate;
            const double envelope = (1.0 - std::exp (-t / 0.0004)) * std::exp (-t / 0.011);
            wave[i] = (float) (level * envelope * (std::sin (twoPi * frequency * t) + 0.3 * std::sin (twoPi * frequency * 2.0 * t)));
        }

        return wave;
    };

    clickAccent = make (2093.0, 0.55f);
    clickNormal = make (1568.0, 0.40f);
    clickVoices = {};
}

void Engine::renderClicks (const CountInBlock& countIn, bool songRolling, double songTime, int numSamples, float* const* out, int numOut, int outOffset) noexcept
{
    float* click = clickBuffer.getWritePointer (0);
    juce::FloatVectorOperations::clear (click, numSamples);

    // 이 블록이 맡는 박 시각의 범위 [t0, t1). 클릭을 늦게(+) 하면 같은 박이 그만큼 뒤 블록에서 울린다.
    // 이어지는 블록은 앞 블록이 끝난 곳에서 시작한다: 곡과 장치의 샘플 속도가 다르면 재생 위치가 블록마다
    // 1샘플씩 흔들려, 그대로 쓰면 경계의 박을 빠뜨리거나 두 번 친다. 20ms 넘게 어긋나면(위치 이동) 다시 맞춘다.
    const bool on = metronomeOn.load (std::memory_order_relaxed);
    const double shift = (double) metronomeOffsetMs.load (std::memory_order_relaxed) * 0.001;
    const bool countingIn = countIn.remaining > 0;
    const bool running = countingIn || songRolling;
    double t0 = songTime - shift;

    // 클릭 시점(shift)을 방금 바꿨으면 이어 붙이지 않고 새 값으로 다시 맞춘다(재생 중에 5ms씩 옮겨도 바로 들리게).
    if (running && clickClockValid && shift == clickClockShift && std::abs (t0 - clickClock) < 0.02)
        t0 = clickClock;

    clickClockShift = shift;

    const double t1 = t0 + numSamples / sampleRate;
    clickClock = t1;
    clickClockValid = running;
    const auto offsetOf = [&] (double t) { return juce::jlimit (0, numSamples - 1, juce::roundToInt ((t - t0) * sampleRate)); };

    // 박이 블록 경계에 정확히 놓이면, 블록 길이를 더해 가며 생기는 아주 작은 계산 오차 때문에 양쪽 블록에서 다 빠질 수 있다.
    // 그래서 각 블록이 맡는 범위를 반 샘플 앞으로 당긴 [lo, hi)로 본다(이어지는 블록끼리는 여전히 빈틈없이 맞물린다).
    const double eps = 0.5 / sampleRate;
    const double hi = t1 - eps;

    // 예비박과 원곡 박의 경계(박 시각): 원곡이 시작되는 순간에 울릴 박. 예비박이 이 블록 안에서 끝나도 두 쪽이 겹치거나 빠지지 않는다.
    // 클릭을 늦추면(+) 원곡 시작 위치 앞의 박 몇 개가 원곡이 시작된 뒤에 울리고(박자표에서 친다),
    // 앞당기면(−) 원곡 시작 위치부터의 박 몇 개가 예비박 동안 울린다. 어느 쪽이든 박마다 한 번씩만 친다.
    const double songStartT = countIn.start - shift;

    // 박자표(와 첫 박 앞으로 같은 간격으로 늘인 박)에서 박 시각이 [lo, hi)인 클릭을 친다.
    // 이어지는 블록의 범위는 빈틈없이 맞물리므로 매번 찾아도 빠지거나 두 번 치지 않는다.
    const auto clicksFromMap = [&] (double lo, double hiLimit)
    {
        if (lo >= hiLimit)
            return;

        const juce::SpinLock::ScopedTryLockType lock (beatLock);

        if (! lock.isLocked() || ! clickMap.isValid())
            return;

        const auto& beats = clickMap.beats;
        const auto isDownbeat = [this] (int index) { return positiveModulo (index - clickFirstDownbeat, clicksPerBar) == 0; };

        // 첫 박 앞(무음 전주 등)은 첫 박에서 같은 간격으로 거슬러 올라간 박을 친다.
        const double firstPeriod = beats[1] - beats[0];

        if (lo < beats[0] && firstPeriod > 0.07)
        {
            const int kMin = juce::jmax (1, (int) std::ceil ((beats[0] - hiLimit) / firstPeriod + 1.0e-9));
            const int kMax = (int) std::floor ((beats[0] - lo) / firstPeriod);

            for (int k = kMax; k >= kMin; --k)
            {
                const double t = beats[0] - k * firstPeriod;

                if (t >= lo && t < hiLimit)
                    startClick (isDownbeat (-k), offsetOf (t));
            }
        }

        for (auto it = std::lower_bound (beats.begin(), beats.end(), lo); it != beats.end() && *it < hiLimit; ++it)
            startClick (clickMap.downbeat[(size_t) (it - beats.begin())] != 0, offsetOf (*it));
    };

    if (on && countingIn)
    {
        const double end = juce::jmin (hi, songStartT - eps);

        // 예비박(원곡 시작 위치보다 앞의 박, 마디 첫 박 자리만 높게)
        for (size_t i = 0; i < (size_t) juce::jlimit (0, maxCountInClicks, countIn.count); ++i)
            if (countIn.beats[i] >= t0 - eps && countIn.beats[i] < end)
                startClick (countIn.accent[i], offsetOf (countIn.beats[i]));

        // 클릭을 앞당겼으면 원곡 시작 위치부터의 박 중 예비박 동안 울릴 것
        clicksFromMap (juce::jmax (t0, countIn.start) - eps, end);
    }

    if (on && songRolling)
        clicksFromMap ((countingIn ? juce::jmax (t0, songStartT) : t0) - eps, hi);

    bool any = false;

    for (auto& voice : clickVoices)
    {
        if (voice.data == nullptr)
            continue;

        any = true;

        for (int i = juce::jmax (0, -voice.position); i < numSamples; ++i)
        {
            const int index = voice.position + i;

            if (index >= voice.length)
                break;

            click[i] += voice.data[index];
        }

        voice.position += numSamples;

        if (voice.position >= voice.length)
            voice.data = nullptr;
    }

    if (! any)
        return;

    const float v = juce::jlimit (0.0f, maxMetronomeVolume, metronomeVolume.load (std::memory_order_relaxed));
    const float gain = v * v * faderGain (master, false);

    if (gain <= 0.0f)
        return;

    // 클릭은 리미터 뒤(녹음 뒤)에 더해지므로, 크게 친 박과 겹쳐 0dB를 넘는 부분만 부드럽게 누른다(듣는 소리만).
    constexpr float knee = 0.9f;

    for (int ch = 0; ch < juce::jmin (2, numOut); ++ch)
    {
        if (out[ch] == nullptr)
            continue;

        float* o = out[ch] + outOffset;
        juce::FloatVectorOperations::addWithMultiply (o, click, gain, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::abs (o[i]);

            if (a > knee)
                o[i] = std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), o[i]);
        }
    }
}

void Engine::play()
{
    if (! hasSong())
        return;

    if (preview != nullptr)
        preview->playing = false;   // 녹음 다듬기 미리 듣기와 겹치지 않게

    if (isCountingIn() || transport.isPlaying())
        return;

    if (transport.hasStreamFinished() || getPosition() >= getLength() - 0.05)
        transport.setPosition (0.0);

    // 메트로놈이 켜져 있으면 원곡 박자에 맞춘 4박을 먼저 세고, 오디오 스레드가 정확한 순간에 원곡을 시작한다.
    const double start = transport.getCurrentPosition();
    {
        const juce::SpinLock::ScopedLockType lock (countInLock);

        if (countInRemaining.load() > 0 || transport.isPlaying())
            return;

        if (const int samples = prepareCountIn (start); samples > 0)
        {
            countInRemaining.store (samples, std::memory_order_release);
            return;
        }
    }

    transport.start();
}

void Engine::pause()
{
    {
        const juce::SpinLock::ScopedLockType lock (countInLock);
        countInRemaining.store (0);
    }

    transport.stop();
}

void Engine::rewind()
{
    setPosition (0.0);
}

void Engine::setPosition (double seconds)
{
    if (! hasSong())
        return;

    // 예비박을 세는 중에 위치를 옮기면 예비박은 그만두고 바로 그 위치부터 재생한다.
    bool wasCountingIn = false;
    {
        const juce::SpinLock::ScopedLockType lock (countInLock);
        wasCountingIn = countInRemaining.exchange (0) > 0;
    }

    transport.setPosition (juce::jlimit (0.0, juce::jmax (0.0, getLength()), seconds));

    if (wasCountingIn)
        transport.start();
}

//==============================================================================
bool Engine::startRecording (const juce::File& parentFolder, juce::String& error)
{
    if (renderingDrums.load())
    {
        error = K ("고친 MIDI로 드럼을 다시 만드는 중입니다. 끝난 뒤에 녹음해 주세요.");
        return false;
    }

    stopRecording();

    if (preview != nullptr)
        preview->playing = false;

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    const auto folder = parentFolder.getChildFile (lang::pick ("연주 ", "Take ") + stamp);

    if (! folder.createDirectory())
    {
        error = K ("녹음 폴더를 만들 수 없습니다: ") + folder.getFullPathName();
        return false;
    }

    const double rate = sampleRate;

    const auto makeWriter = [this, rate] (const juce::File& file, bool floatingPoint) -> std::unique_ptr<ThreadedWriter>
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

        if (stream == nullptr)
            return {};

        auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2);
        options = floatingPoint ? options.withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)
                                : options.withBitsPerSample (24);

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, options);

        if (writer == nullptr)
            return {};

        return std::make_unique<ThreadedWriter> (writer.release(), recordThread, 1 << 17);
    };

    auto newMix  = makeWriter (folder.getChildFile (TakeFiles::mix()), false);
    auto newDrum = makeWriter (folder.getChildFile (TakeFiles::drum()), true);
    auto newSong = makeWriter (folder.getChildFile (TakeFiles::song()), true);

    // 드럼을 분리한 곡이면 원곡에 들어간 원곡 드럼도 따로 남긴다(다듬기 창의 '원곡 드럼' 조절). 못 만들어도 녹음은 한다.
    auto newSongDrums = isUsingStems() ? makeWriter (folder.getChildFile (TakeFiles::songDrums()), true)
                                       : std::unique_ptr<ThreadedWriter>();

    if (newMix == nullptr || newDrum == nullptr || newSong == nullptr)
    {
        newMix.reset();
        newDrum.reset();
        newSong.reset();
        folder.deleteRecursively();
        error = K ("녹음 파일을 만들 수 없습니다. 폴더에 쓸 수 있는지 확인해 주세요.");
        return false;
    }

    // 다듬기 창이 처음 보여 줄 값: 녹음을 시작할 때 들리던 그대로
    recordInfo = {};
    recordInfo.sampleRate = rate;
    recordInfo.drumVolume = drum.mute.load() ? 0.0f : drum.volume.load();
    recordInfo.songVolume = song.mute.load() ? 0.0f : song.volume.load();
    recordInfo.latencyMs  = getEstimatedLatencyMs();
    recordInfo.songName   = hasSong() ? songFile.getFileNameWithoutExtension() : juce::String();
    recordInfo.recordedAt = juce::Time::getCurrentTime();

    // 연주 MIDI: 칸을 미리 잡고 원곡 박·메트로놈 시점·지연·모듈 종류를 적어 둔다(오디오 스레드는 아직 쓰지 않는다)
    recMidi.assign (recMidiCapacity, {});
    recSpans.assign (recSpanCapacity, {});
    recMidiCount = 0;
    recSpanCount = 0;
    recSpanStarted = false;
    recLastRolling = false;
    recLastSongTime = 0.0;
    recLastChunk = 0;

    {
        const juce::SpinLock::ScopedLockType lock (beatLock);
        recBeats = beatMapValid.load() ? beatMap : BeatMap();
    }

    recClickOffset = metronomeOffsetMs.load() / 1000.0;
    const auto latency = getLatencyBreakdown();
    recGridShift = latency.isValid() ? latency.toMs (latency.buffer + latency.driverExtra + latency.midiWait) / 1000.0 : 0.0;
    recBrand = identifyDrumModule (midiInputDevices).brand;

    mixWriter  = std::move (newMix);
    drumWriter = std::move (newDrum);
    songWriter = std::move (newSong);
    songDrumWriter = std::move (newSongDrums);
    recordFolder = folder;
    recordSampleRate = rate;
    recordedSamples = 0;

    {
        const juce::SpinLock::ScopedLockType lock (recordLock);
        activeMixWriter  = mixWriter.get();
        activeDrumWriter = drumWriter.get();
        activeSongWriter = songWriter.get();
        activeSongDrumWriter = songDrumWriter.get();
    }

    recordingSongDrums = songDrumWriter != nullptr;
    recording = true;
    return true;
}

void Engine::stopRecording()
{
    {
        const juce::SpinLock::ScopedLockType lock (recordLock);
        activeMixWriter  = nullptr;
        activeDrumWriter = nullptr;
        activeSongWriter = nullptr;
        activeSongDrumWriter = nullptr;
    }

    const bool wasRecording = recording.exchange (false);
    recordingSongDrums = false;

    mixWriter.reset();     // 남은 데이터를 다 쓰고 파일을 닫는다
    drumWriter.reset();
    songWriter.reset();
    songDrumWriter.reset();

    if (wasRecording && recordFolder != juce::File())
    {
        recordInfo.lengthSamples = recordedSamples.load();
        writeRecordedMidi();
        recordInfo.save (recordFolder);
        lastTakeFolder = recordFolder;
    }

    recordFolder = juce::File();
}

double Engine::getRecordedSeconds() const noexcept
{
    return recordSampleRate > 0.0 ? (double) recordedSamples.load() / recordSampleRate : 0.0;
}

// 녹음을 멈춘 뒤(오디오 스레드가 더는 쓰지 않을 때) 연주 MIDI를 녹음 폴더에 쓴다. 친 음이 없으면 쓰지 않는다.
void Engine::writeRecordedMidi()
{
    takemidi::Recording r;
    r.sampleRate = recordSampleRate;
    r.lengthSamples = recordInfo.lengthSamples;
    r.brand = recBrand;
    r.events.assign (recMidi.begin(), recMidi.begin() + (std::ptrdiff_t) juce::jmin (recMidiCount, recMidi.size()));
    r.spans.assign (recSpans.begin(), recSpans.begin() + (std::ptrdiff_t) juce::jmin (recSpanCount, recSpans.size()));
    r.beats = recBeats;
    r.clickOffsetSeconds = recClickOffset;
    r.gridShiftSeconds = recGridShift;

    std::vector<takemidi::RawEvent>().swap (recMidi);   // 녹음 동안 잡아 둔 칸을 돌려준다
    std::vector<takemidi::SongSpan>().swap (recSpans);
    recMidiCount = recSpanCount = 0;

    takemidi::Written written;
    juce::String error;

    if (takemidi::writeTakeMidi (r, recordFolder, written, error))
    {
        recordInfo.hasMidi = true;
        recordInfo.midiStart = written.startSeconds;
        recordInfo.midiBpm = written.bpm;
        recordInfo.midiBrand = (int) r.brand;
        recordInfo.midiBeatsPerBar = written.beatsPerBar;
        recordInfo.midiFromSong = written.fromSong;
    }
    else if (error.isNotEmpty())
    {
        DBG (error);
    }
}

bool Engine::renderDrumsFromMidi (const std::vector<takemidi::RawEvent>& events, juce::int64 lengthSamples, double takeSampleRate,
                                  const juce::File& destination, const std::function<bool (double)>& progress, juce::String& error)
{
    if (plugin == nullptr)
    {
        error = K ("드럼 VST를 먼저 불러와 주세요. 다시 만들 때는 지금 고른 드럼 VST와 킷을 씁니다.");
        return false;
    }

    if (sampleRate <= 0.0 || maxBlock <= 0)
    {
        error = K ("소리 장치가 열려 있어야 다시 만들 수 있습니다.");
        return false;
    }

    if (std::abs (takeSampleRate - sampleRate) > 0.5)
    {
        error = K ("녹음할 때와 지금 소리 장치의 샘플레이트가 달라 다시 만들 수 없습니다(녹음 ") + juce::String (takeSampleRate, 0)
              + K (" Hz, 지금 ") + juce::String (sampleRate, 0) + " Hz).";
        return false;
    }

    if (isRecording())
    {
        error = K ("녹음 중에는 다시 만들 수 없습니다.");
        return false;
    }

    if (renderingDrums.exchange (true))
    {
        error = K ("이미 다시 만드는 중입니다.");
        return false;
    }

    // 이제 오디오 스레드는 플러그인을 건너뛴다. 이미 처리 중인 블록이 끝나기를 기다린다.
    { const juce::SpinLock::ScopedLockType lock (pluginLock); }

    const auto finish = [this] (juce::AudioPluginInstance* used)
    {
        {
            const juce::SpinLock::ScopedLockType lock (pluginLock);

            if (used != nullptr && plugin.get() == used)
            {
                used->setNonRealtime (false);
                used->reset();
            }
        }

        renderingDrums = false;
    };

    juce::AudioPluginInstance* used = nullptr;
    int channels = 0;
    std::vector<OutBus> buses;

    {
        const juce::SpinLock::ScopedLockType lock (pluginLock);
        used = plugin.get();
        channels = pluginBuffer.getNumChannels();
        buses = pluginBuses;

        if (used != nullptr)
        {
            used->setNonRealtime (true);   // 디스크에서 샘플을 읽는 플러그인도 빠짐없이 내게
            used->reset();
        }
    }

    if (used == nullptr || channels <= 0)
    {
        finish (used);
        error = K ("드럼 VST를 먼저 불러와 주세요. 다시 만들 때는 지금 고른 드럼 VST와 킷을 씁니다.");
        return false;
    }

    destination.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
    std::unique_ptr<juce::AudioFormatWriter> writer;

    if (stream != nullptr)
    {
        auto options = juce::AudioFormatWriterOptions().withSampleRate (takeSampleRate).withNumChannels (2)
                           .withBitsPerSample (32).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        juce::WavAudioFormat wav;
        writer = wav.createWriterFor (stream, options);
    }

    if (writer == nullptr)
    {
        finish (used);
        error = K ("드럼 파일을 쓸 수 없습니다: ") + destination.getFullPathName();
        return false;
    }

    const int block = maxBlock;
    juce::AudioBuffer<float> buffer (channels, block);
    std::vector<float> left ((size_t) block), right ((size_t) block);
    Strip strip;
    strip.prepare (takeSampleRate);
    juce::MidiBuffer midi;
    midi.ensureSize (4096);
    size_t next = 0;
    bool ok = true;

    for (juce::int64 position = 0; position < lengthSamples; position += block)
    {
        const int n = (int) juce::jmin<juce::int64> (block, lengthSamples - position);
        midi.clear();

        for (; next < events.size() && events[next].sample < position + n; ++next)
        {
            const auto& e = events[next];

            if (e.size == 0 || e.size > 3)
                continue;

            juce::uint8 data[3] { e.data[0], e.data[1], e.data[2] };

            if (midiInbox.mapForPlugin (data, (int) e.size) && MidiInbox::shouldForward (data, (int) e.size))
                midi.addEvent (data, (int) e.size, (int) juce::jmax<juce::int64> (0, e.sample - position));
        }

        {
            const juce::SpinLock::ScopedLockType lock (pluginLock);

            if (plugin.get() != used)
            {
                error = K ("다시 만드는 중에 드럼 VST가 바뀌어 멈췄습니다.");
                ok = false;
                break;
            }

            buffer.setSize (channels, n, false, false, true);
            buffer.clear();
            used->processBlock (buffer, midi);
        }

        std::fill (left.begin(), left.begin() + n, 0.0f);
        std::fill (right.begin(), right.begin() + n, 0.0f);

        for (const auto& bus : buses)
        {
            if (bus.start + bus.count > channels)
                break;

            const float* a = buffer.getReadPointer (bus.start);
            const float* b = bus.count > 1 ? buffer.getReadPointer (bus.start + 1) : a;
            juce::FloatVectorOperations::add (left.data(), a, n);
            juce::FloatVectorOperations::add (right.data(), b, n);
        }

        strip.processTone (left.data(), right.data(), n, drum);   // 녹음할 때처럼 드럼 채널 EQ·컴프레서까지(볼륨은 빼고)
        const float* channelsOut[] = { left.data(), right.data() };

        if (! writer->writeFromFloatArrays (channelsOut, 2, n))
        {
            error = K ("드럼 파일을 쓸 수 없습니다: ") + destination.getFullPathName();
            ok = false;
            break;
        }

        if (progress != nullptr && ! progress ((double) (position + n) / (double) lengthSamples))
        {
            error = K ("다시 만들기를 멈췄습니다.");
            ok = false;
            break;
        }
    }

    writer.reset();
    finish (used);

    if (! ok)
        destination.deleteFile();

    return ok;
}

//==============================================================================
void Engine::setPreview (std::shared_ptr<TakePlayer> newPreview)
{
    std::shared_ptr<TakePlayer> old;

    {
        const juce::SpinLock::ScopedLockType lock (previewLock);
        old = std::move (previewOwner);
        previewOwner = std::move (newPreview);
        preview = previewOwner.get();
    }

    // old는 여기(메시지 스레드)에서 정리된다.
}

//==============================================================================
void Engine::loadSettings (juce::PropertiesFile& p)
{
    const auto loadChannel = [&p] (const juce::String& prefix, ChannelSettings& c, double defaultVolume)
    {
        c.volume = (float) juce::jlimit (0.0, 1.0, p.getDoubleValue (prefix + "Volume", defaultVolume));
        c.low    = (float) juce::jlimit (-12.0, 12.0, p.getDoubleValue (prefix + "Low", 0.0));
        c.mid    = (float) juce::jlimit (-12.0, 12.0, p.getDoubleValue (prefix + "Mid", 0.0));
        c.high   = (float) juce::jlimit (-12.0, 12.0, p.getDoubleValue (prefix + "High", 0.0));
        c.comp   = (float) juce::jlimit (0.0, 1.0, p.getDoubleValue (prefix + "Comp", 0.0));
    };

    loadChannel ("drum", drum, 1.0);
    loadChannel ("song", song, 0.7);
    loadChannel ("master", master, 1.0);

    savedPluginPath = p.getValue ("pluginPath");
    savedSongPath = p.getValue ("songPath");
    wantedAudioState = p.getXmlValue ("audioState");

    // 플러그인마다 마지막 상태(킷·맵 등)
    if (const auto states = p.getXmlValue ("pluginStates"))
    {
        for (const auto* entry : states->getChildIterator())
        {
            juce::MemoryBlock state;

            if (state.fromBase64Encoding (entry->getStringAttribute ("state")) && ! state.isEmpty())
                pluginStates[entry->getStringAttribute ("path")] = std::move (state);
        }
    }

    // 예전 형식(플러그인 하나만 저장하던 때)
    if (savedPluginPath.isNotEmpty() && pluginStates.find (savedPluginPath) == pluginStates.end())
    {
        juce::MemoryBlock state;

        if (state.fromBase64Encoding (p.getValue ("pluginState")) && ! state.isEmpty())
            pluginStates[savedPluginPath] = std::move (state);
    }

    extraPluginPaths = juce::StringArray::fromLines (p.getValue ("extraPluginPaths"));
    extraPluginPaths.removeEmptyStrings();
}

void Engine::saveState (juce::PropertiesFile& p)
{
    if (auto xml = deviceManager.createStateXml())
    {
        // 드라이버 설정 창에서 버퍼를 바꾼 경우에도 실제로 쓰고 있는 값을 남긴다.
        if (auto* device = deviceManager.getCurrentAudioDevice())
            if (xml->getStringAttribute ("deviceType") == device->getTypeName()
                && xml->getStringAttribute ("audioOutputDeviceName") == device->getName())
                xml->setAttribute ("audioDeviceBufferSize", device->getCurrentBufferSizeSamples());

        p.setValue ("audioState", xml.get());
    }

    const auto saveChannel = [&p] (const juce::String& prefix, const ChannelSettings& c)
    {
        p.setValue (prefix + "Volume", (double) c.volume.load());
        p.setValue (prefix + "Low", (double) c.low.load());
        p.setValue (prefix + "Mid", (double) c.mid.load());
        p.setValue (prefix + "High", (double) c.high.load());
        p.setValue (prefix + "Comp", (double) c.comp.load());
    };

    saveChannel ("drum", drum);
    saveChannel ("song", song);
    saveChannel ("master", master);

    // 플러그인을 못 불러온 채로 끝낼 때는 예전 값을 지우지 않는다.
    // 다른 플러그인을 불러오는 중이면(그사이 끼어든 저장) 지금 플러그인에 손대지 않는다.
    if (! pluginLoadInProgress)
        rememberCurrentPluginState();

    if (plugin != nullptr)
        p.setValue ("pluginPath", pluginPath);

    juce::XmlElement states ("PLUGINSTATES");

    for (const auto& [path, state] : pluginStates)
    {
        auto* entry = states.createNewChildElement ("PLUGIN");
        entry->setAttribute ("path", path);
        entry->setAttribute ("state", state.toBase64Encoding());
    }

    p.setValue ("pluginStates", &states);
    p.removeValue ("pluginState");   // 예전 형식은 위로 옮겼다
    p.setValue ("extraPluginPaths", extraPluginPaths.joinIntoString ("\n"));

    if (songFile != juce::File())
        p.setValue ("songPath", songFile.getFullPathName());
}

//==============================================================================
void Engine::prepare (double newSampleRate, int newBlockSize)
{
    const juce::SpinLock::ScopedLockType lock (pluginLock);

    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlock = juce::jmax (32, newBlockSize);

    drumBuffer.setSize (2, maxBlock, false, true, false);
    songBuffer.setSize (4, maxBlock, false, true, false);   // 0·1 원곡(또는 드럼을 뺀 원곡), 2·3 원곡 드럼(분리했을 때만)
    mixBuffer.setSize (2, maxBlock, false, true, false);
    stemBuffer.setSize (6, maxBlock, false, true, false);   // 0·1 드럼, 2·3 원곡, 4·5 원곡에 들어간 원곡 드럼(녹음용)
    clickBuffer.setSize (1, maxBlock, false, true, false);
    makeClickSounds();

    drumStrip.prepare (sampleRate);
    songStrip.prepare (sampleRate);
    songDrumEq.prepare (sampleRate);
    masterGain.reset (sampleRate, 0.03);
    songDrumGain.reset (sampleRate, 0.03);
    songDrumGain.setCurrentAndTargetValue (songDrumsOn.load() ? 1.0f : 0.0f);
    limiter.prepare (sampleRate);

    transport.prepareToPlay (maxBlock, sampleRate);

    if (plugin != nullptr)
    {
        plugin->releaseResources();
        preparePlugin (*plugin, pluginBuffer, pluginBuses);
    }
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double rate = device->getCurrentSampleRate();
    const int buffer = device->getCurrentBufferSizeSamples();

    if (recording.load() && rate != recordSampleRate)
        stopRecording();

    prepare (rate, buffer);
    renderingOffline = false;

    // 끊김 감시를 처음부터 다시 잰다. JUCE는 이 함수와 오디오 콜백을 동시에 부르지 않는다.
    // 드라이버의 여유(출력 지연 − 버퍼 − 1ms)는 콜백이 그만큼 늦어도 소리가 이어질 거라는 낙관적 추정이다.
    monitor.needsInit = true;
    monitor.offline = false;
    monitor.headroomMs = rate > 0.0 ? juce::jmax (0.0, (device->getOutputLatencyInSamples() - buffer) * 1000.0 / rate - 1.0) : 0.0;
    monHeadroomMs = (float) monitor.headroomMs;
    monBlockMs = rate > 0.0 ? (float) (buffer * 1000.0 / rate) : 0.0f;

    // 드라이버가 방금 장치를 열면서 읽은 설정과 같은 값이다(화면이 나중에 읽으면 그 사이 바뀐 것을 놓친다).
    {
        const auto settings = readYamahaDriverSettings();
        const juce::ScopedLock lock (driverSettingsLock);
        driverSettingsAtStart = settings;
    }

    monStreamStarts.fetch_add (1);
}

juce::String Engine::readYamahaDriverSettings()
{
    const juce::String key ("HKEY_CURRENT_USER\\Software\\Yamaha\\Yamaha Steinberg USB Driver\\");
    return juce::WindowsRegistry::getValue (key + "LatencyMode") + "/" + juce::WindowsRegistry::getValue (key + "PeriodFrames")
         + "/" + juce::WindowsRegistry::getValue (key + "OutHubOffset");
}

bool Engine::writeYamahaFasterStep (int fasterMs, int modeWhenOff)
{
    const juce::String key ("HKEY_CURRENT_USER\\Software\\Yamaha\\Yamaha Steinberg USB Driver\\");

    if (fasterMs < 0 || fasterMs > 4 || modeWhenOff < 1 || modeWhenOff > 3 || ! juce::WindowsRegistry::valueExists (key + "LatencyMode"))
        return false;

    if (fasterMs == 0)
    {
        // 드라이버 창의 Mode로 되돌리고, 앱이 넣은 값은 지운다(0이 아닐 때는 효과가 없지만 남겨 두지 않는다).
        juce::WindowsRegistry::setValue (key + "LatencyMode", (juce::uint32) modeWhenOff);
        juce::WindowsRegistry::deleteValue (key + "OutHubOffset");
        return juce::WindowsRegistry::getValue (key + "LatencyMode") == juce::String (modeWhenOff);
    }

    juce::WindowsRegistry::setValue (key + "OutHubOffset", (juce::uint32) (4 - fasterMs));
    juce::WindowsRegistry::setValue (key + "LatencyMode", (juce::uint32) 0);
    return juce::WindowsRegistry::getValue (key + "LatencyMode") == "0"
        && juce::WindowsRegistry::getValue (key + "OutHubOffset") == juce::String (4 - fasterMs);
}

juce::String Engine::getDriverSettingsAtStart() const
{
    const juce::ScopedLock lock (driverSettingsLock);
    return driverSettingsAtStart;
}

void Engine::audioDeviceStopped()
{
    countInRemaining.store (0);   // 장치가 멈추면 예비박도 그만둔다
}

void Engine::prepareOffline (double newSampleRate, int blockSize)
{
    prepare (newSampleRate, blockSize);
    monitor.offline = true;   // 오프라인 점검에서는 시각이 실제와 달라서 끊김 감시를 하지 않는다
    renderingOffline = true;
}

void Engine::renderOfflineInto (float* left, float* right, int numSamples)
{
    for (int done = 0; done < numSamples; done += maxBlock)
    {
        float* outs[] = { left + done, right + done };
        audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, juce::jmin (maxBlock, numSamples - done), {});
    }
}

void Engine::updateMonitor (double startMs, double endMs, int numSamples) noexcept
{
    if (monitor.offline || sampleRate <= 0.0)
        return;

    const double blockMs = numSamples * 1000.0 / sampleRate;
    const double duration = endMs - startMs;
    monCallbacks.fetch_add (1, std::memory_order_relaxed);

    if (monResetMax.exchange (false, std::memory_order_relaxed))
    {
        monitor.localMaxDuration = 0.0;
        monitor.localMaxExcess = 0.0;
        monMaxDurationMs.store (0.0f, std::memory_order_relaxed);
        monMaxExcessMs.store (0.0f, std::memory_order_relaxed);
    }

    if (monitor.needsInit)
    {
        monitor.needsInit = false;
        monitor.startMs = startMs;
        monitor.t0 = startMs;
        monitor.samples = 0.0;
        monitor.callbacksSinceStart = 0;
        monitor.lagCount = 0;
        monitor.lagIndex = 0;
    }

    // 예정 시각: 처음 콜백 + 지금까지 넘겨준 샘플 수. 늦은 정도는 최근 32번 중 가장 늦었던 것과 비교한다.
    // (USB 드라이버가 콜백을 두 번 붙여서 주거나 늦은 뒤 따라잡는 경우는 늦은 것으로 세지 않는다.)
    const double lag = startMs - (monitor.t0 + monitor.samples * 1000.0 / sampleRate);
    monitor.samples += numSamples;

    double reference = -1.0e9;

    for (int i = 0; i < monitor.lagCount; ++i)
        reference = juce::jmax (reference, monitor.lags[(size_t) i]);

    const bool ringFull = monitor.lagCount >= (int) monitor.lags.size();
    monitor.lags[(size_t) monitor.lagIndex] = lag;
    monitor.lagIndex = (monitor.lagIndex + 1) % (int) monitor.lags.size();
    monitor.lagCount = juce::jmin ((int) monitor.lags.size(), monitor.lagCount + 1);

    if (duration > monitor.localMaxDuration)
    {
        monitor.localMaxDuration = duration;
        monMaxDurationMs.store ((float) duration, std::memory_order_relaxed);
    }

    // 장치를 연 직후(64번, 1초 안)는 플러그인 준비 등으로 들쭉날쭉해서 따로 센다.
    const bool warm = ++monitor.callbacksSinceStart > 64 && startMs - monitor.startMs > 1000.0 && ringFull;

    if (! warm)
    {
        if (duration > blockMs)
            monWarmupAnomalies.fetch_add (1, std::memory_order_relaxed);

        return;
    }

    const double excess = lag - reference;

    if (excess > monitor.localMaxExcess)
    {
        monitor.localMaxExcess = excess;
        monMaxExcessMs.store ((float) excess, std::memory_order_relaxed);
    }

    // 킷을 불러오는 중처럼 일부러 무거운 일을 하는 동안 생긴 일은 따로 센다.
    const bool quiet = startMs < quietUntilMs.load (std::memory_order_relaxed);
    const auto count = [&] (std::atomic<juce::uint32>& counter)
    {
        (quiet ? monQuietEvents : counter).fetch_add (1, std::memory_order_relaxed);
    };

    constexpr double jitterMs = 1.0;

    if (duration > blockMs)
        count (monOwnOverruns);

    if (excess > juce::jmax (blockMs, 2.0 * jitterMs))
        count (monLate);
    else if (excess > juce::jmax (0.5 * blockMs, jitterMs))
        count (monLateNear);

    // 늦게 와서 늦게 끝난 정도: 한 블록을 넘으면 아슬아슬, 드라이버 여유까지 넘으면 끊겼을 가능성이 크다.
    const double lateCompletion = juce::jmax (0.0, excess) + duration;

    if (lateCompletion > blockMs + monitor.headroomMs)
        count (monLikelyDropouts);
    else if (lateCompletion > blockMs)
        count (monNearMiss);
}

void Engine::renderOffline (int numSamples, float& drumPeak, float& mixPeak)
{
    std::vector<float> left ((size_t) maxBlock), right ((size_t) maxBlock);
    float* outs[] = { left.data(), right.data() };

    drum.peakL = 0.0f;
    drum.peakR = 0.0f;
    mixPeak = 0.0f;

    for (int done = 0; done < numSamples; done += maxBlock)
    {
        const int n = juce::jmin (maxBlock, numSamples - done);
        audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, n, {});
        mixPeak = juce::jmax (mixPeak, blockPeak (left.data(), n), blockPeak (right.data(), n));
    }

    drumPeak = juce::jmax (drum.peakL.load(), drum.peakR.load());
}

//==============================================================================
void Engine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* outputChannelData, int numOutputChannels,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    // 이 콜백이 시작된 시각. 패드 신호를 놓는 기준과 끊김 감시가 같은 시계를 쓴다.
    const double callbackStart = juce::Time::getMillisecondCounterHiRes();

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    incomingMidi.clear();
    incomingRaw.clear();

    const double waited = midiInbox.drainInto (incomingMidi, &incomingRaw, numSamples, sampleRate, midiDelayMs (maxBlock, sampleRate), callbackStart);

    // 최댓값 갱신(읽는 쪽이 0으로 비우는 것과 겹쳐도 값이 사라지지 않게 비교-교환)
    for (auto current = maxMidiWaitMs.load (std::memory_order_relaxed); waited > (double) current;)
        if (maxMidiWaitMs.compare_exchange_weak (current, (float) waited, std::memory_order_relaxed))
            break;

    {
        const juce::SpinLock::ScopedTryLockType lock (uiMidiLock);

        if (lock.isLocked() && ! uiMidi.isEmpty())
        {
            incomingMidi.addEvents (uiMidi, 0, -1, 0);
            uiMidi.clear();
        }
    }

    for (const auto meta : incomingMidi)
    {
        if (meta.getMessage().isNoteOn())
        {
            midiNoteCount.fetch_add (1, std::memory_order_relaxed);
            break;
        }
    }

    for (int offset = 0; offset < numSamples; offset += maxBlock)
        processChunk (offset, juce::jmin (maxBlock, numSamples - offset), outputChannelData, numOutputChannels);

    updateMonitor (callbackStart, juce::Time::getMillisecondCounterHiRes(), numSamples);
}

void Engine::processChunk (int offset, int numSamples, float* const* out, int numOut)
{
    float* drumL = drumBuffer.getWritePointer (0);
    float* drumR = drumBuffer.getWritePointer (1);
    juce::FloatVectorOperations::clear (drumL, numSamples);
    juce::FloatVectorOperations::clear (drumR, numSamples);

    // --- 드럼: MIDI → VST 악기(고친 MIDI로 다시 만드는 동안은 건너뛴다: 같은 플러그인을 그쪽이 쓴다)
    if (! renderingDrums.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (pluginLock);

        if (lock.isLocked() && plugin != nullptr && ! plugin->isSuspended())
        {
            chunkMidi.clear();
            chunkMidi.addEvents (incomingMidi, offset, numSamples, -offset);

            // 길이만 이 블록에 맞춘다. 미리 잡아 둔 메모리를 그대로 쓰므로 출력이 48채널이어도 새로 할당하지 않는다.
            const int channels = pluginBuffer.getNumChannels();
            pluginBuffer.setSize (channels, numSamples, false, false, true);
            pluginBuffer.clear();

            plugin->processBlock (pluginBuffer, chunkMidi);

            for (const auto& bus : pluginBuses)
            {
                if (bus.start + bus.count > channels)
                    break;

                const float* a = pluginBuffer.getReadPointer (bus.start);
                const float* b = bus.count > 1 ? pluginBuffer.getReadPointer (bus.start + 1) : a;
                juce::FloatVectorOperations::add (drumL, a, numSamples);
                juce::FloatVectorOperations::add (drumR, b, numSamples);
            }
        }
        else if (! lock.isLocked())
        {
            monPluginSkips.fetch_add (1, std::memory_order_relaxed);   // 킷을 바꾸는 중이라 이번 블록은 드럼을 건너뜀
        }
    }

    // --- 예비박: 이 블록 몫만큼 줄이고, 이 블록 안에서 끝나면 바로 그 샘플부터 원곡을 시작한다.
    // (예비박 길이는 블록 크기의 배수라 보통은 블록 경계에서 시작한다. 블록 크기가 바뀌는 장치도 정확히 맞춘다.)
    CountInBlock countIn;
    int songStartsAt = 0;

    if (countInRemaining.load (std::memory_order_acquire) > 0)
    {
        const juce::SpinLock::ScopedTryLockType lock (countInLock);

        if (lock.isLocked())
        {
            if (const int remaining = countInRemaining.load (std::memory_order_acquire); remaining > 0)
            {
                countIn.remaining = remaining;
                countIn.start = countInStart;
                countIn.beats = countInBeats;
                countIn.accent = countInAccent;
                countIn.count = countInCount;

                const int left = juce::jmax (0, remaining - numSamples);
                countInRemaining.store (left, std::memory_order_release);

                if (left == 0)
                {
                    songStartsAt = remaining;
                    transport.start();
                }
            }
        }
    }

    // --- 원곡. 메트로놈이 같은 시각에 맞추도록 이 블록이 시작되는 곡 위치를 먼저 읽는다.
    // 예비박 중의 곡 위치는 '원곡이 시작될 위치 − 남은 예비박'(아직 오지 않은 시각)으로 센다.
    const bool songRolling = transport.isPlaying();
    const double songTime = countIn.remaining > 0 ? countIn.start - (double) countIn.remaining / sampleRate
                                                  : (double) transport.getNextReadPosition() / sampleRate;

    // 2·3번 채널은 드럼을 분리한 곡일 때만 채워진다. 원곡 그대로일 때 지난 값이 남지 않게 먼저 비운다.
    songBuffer.clear (2, 0, numSamples);
    songBuffer.clear (3, 0, numSamples);

    if (songStartsAt > 0)
        songBuffer.clear (0, juce::jmin (songStartsAt, numSamples));

    if (songStartsAt < numSamples)
    {
        juce::AudioSourceChannelInfo info (&songBuffer, songStartsAt, numSamples - songStartsAt);
        transport.getNextAudioBlock (info);
    }

    float* songL = songBuffer.getWritePointer (0);
    float* songR = songBuffer.getWritePointer (1);

    // 원곡 드럼 켜고 끄기: 30ms에 걸쳐 바꾼다. 켜 두면 원곡과 똑같다(드럼 = 원곡 − 나머지).
    // 녹음 중이면 원곡에 실제로 들어간 원곡 드럼(배율을 곱한 것)을 4·5번에 따로 남긴다. 다듬기 창이 이만큼을 빼서
    // 원곡 드럼만 줄이거나 없앤다(원곡.wav − 이것 = 드럼을 뺀 원곡).
    const bool keepSongDrums = recordingSongDrums.load (std::memory_order_relaxed);
    float* appliedL = stemBuffer.getWritePointer (4);
    float* appliedR = stemBuffer.getWritePointer (5);

    {
        songDrumGain.setTargetValue (songDrumsOn.load (std::memory_order_relaxed) ? 1.0f : 0.0f);
        const float* songDrumsL = songBuffer.getReadPointer (2);
        const float* songDrumsR = songBuffer.getReadPointer (3);

        if (songDrumGain.isSmoothing())
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = songDrumGain.getNextValue();
                songL[i] += g * songDrumsL[i];
                songR[i] += g * songDrumsR[i];

                if (keepSongDrums)
                {
                    appliedL[i] = g * songDrumsL[i];
                    appliedR[i] = g * songDrumsR[i];
                }
            }
        }
        else if (const float g = songDrumGain.getCurrentValue(); g > 0.0f)
        {
            juce::FloatVectorOperations::addWithMultiply (songL, songDrumsL, g, numSamples);
            juce::FloatVectorOperations::addWithMultiply (songR, songDrumsR, g, numSamples);

            if (keepSongDrums)
            {
                juce::FloatVectorOperations::copyWithMultiply (appliedL, songDrumsL, g, numSamples);
                juce::FloatVectorOperations::copyWithMultiply (appliedR, songDrumsR, g, numSamples);
            }
        }
        else if (keepSongDrums)
        {
            juce::FloatVectorOperations::clear (appliedL, numSamples);
            juce::FloatVectorOperations::clear (appliedR, numSamples);
        }
    }

    drumStrip.processTone (drumL, drumR, numSamples, drum);
    songStrip.processTone (songL, songR, numSamples, song);

    // 원곡 EQ는 선형이라 원곡 드럼에도 똑같이 걸면 빼기가 정확하다(원곡 컴프레서를 쓴 경우만 조금 어긋난다).
    // 값은 늘 따라가게 해서 녹음을 시작한 첫 블록부터 원곡과 같은 EQ가 걸리게 한다.
    songDrumEq.setGains (song.low.load (std::memory_order_relaxed), song.mid.load (std::memory_order_relaxed),
                         song.high.load (std::memory_order_relaxed));

    if (keepSongDrums)
        songDrumEq.process (appliedL, appliedR, numSamples);

    // 녹음 중이면 볼륨을 적용하기 전의 드럼·원곡을 따로 남긴다(다듬기 창에서 다시 섞는다).
    const bool rec = recording.load (std::memory_order_relaxed);

    if (rec)
    {
        stemBuffer.copyFrom (0, 0, drumL, numSamples);
        stemBuffer.copyFrom (1, 0, drumR, numSamples);
        stemBuffer.copyFrom (2, 0, songL, numSamples);
        stemBuffer.copyFrom (3, 0, songR, numSamples);
    }

    drumStrip.applyFader (drumL, drumR, numSamples, drum);
    songStrip.applyFader (songL, songR, numSamples, song);

    // --- 합치기 → (다듬기 미리 듣기) → 전체 볼륨 → 리미터
    float* mixL = mixBuffer.getWritePointer (0);
    float* mixR = mixBuffer.getWritePointer (1);
    juce::FloatVectorOperations::copy (mixL, drumL, numSamples);
    juce::FloatVectorOperations::copy (mixR, drumR, numSamples);
    juce::FloatVectorOperations::add (mixL, songL, numSamples);
    juce::FloatVectorOperations::add (mixR, songR, numSamples);

    {
        const juce::SpinLock::ScopedTryLockType lock (previewLock);

        if (lock.isLocked() && preview != nullptr)
            preview->renderAdding (mixL, mixR, numSamples, sampleRate);
    }

    masterGain.setTargetValue (faderGain (master, false));
    applyGain (masterGain, mixL, mixR, numSamples);
    limiter.process (mixL, mixR, numSamples);

    pushPeak (master.peakL, blockPeak (mixL, numSamples));
    pushPeak (master.peakR, blockPeak (mixR, numSamples));

    // --- 출력
    if (numOut >= 2 && out[0] != nullptr && out[1] != nullptr)
    {
        juce::FloatVectorOperations::copy (out[0] + offset, mixL, numSamples);
        juce::FloatVectorOperations::copy (out[1] + offset, mixR, numSamples);
    }
    else if (numOut >= 1 && out[0] != nullptr)
    {
        juce::FloatVectorOperations::copy (out[0] + offset, mixL, numSamples);
        juce::FloatVectorOperations::add (out[0] + offset, mixR, numSamples);
        juce::FloatVectorOperations::multiply (out[0] + offset, 0.5f, numSamples);
    }

    // --- 녹음
    if (rec)
    {
        const juce::SpinLock::ScopedTryLockType lock (recordLock);

        if (lock.isLocked() && activeMixWriter != nullptr)
        {
            const float* mix[] = { mixL, mixR };
            activeMixWriter->write (mix, numSamples);

            if (activeDrumWriter != nullptr)
            {
                const float* drums[] = { stemBuffer.getReadPointer (0), stemBuffer.getReadPointer (1) };
                activeDrumWriter->write (drums, numSamples);
            }

            if (activeSongWriter != nullptr)
            {
                const float* songs[] = { stemBuffer.getReadPointer (2), stemBuffer.getReadPointer (3) };
                activeSongWriter->write (songs, numSamples);
            }

            if (activeSongDrumWriter != nullptr)
            {
                const float* songDrums[] = { stemBuffer.getReadPointer (4), stemBuffer.getReadPointer (5) };
                activeSongDrumWriter->write (songDrums, numSamples);
            }

            // 연주 MIDI: 모듈이 보낸 신호를 이 블록에 놓인 자리 그대로(녹음 첫 샘플부터 센 위치)
            const auto base = recordedSamples.load (std::memory_order_relaxed);

            for (auto it = incomingRaw.findNextSamplePosition (offset); it != incomingRaw.cend(); ++it)
            {
                const auto meta = *it;

                if (meta.samplePosition >= offset + numSamples)
                    break;

                if (meta.numBytes < 1 || meta.numBytes > 3)
                    continue;

                if (recMidiCount < recMidi.size())
                {
                    auto& e = recMidi[recMidiCount++];
                    e.sample = base + (meta.samplePosition - offset);
                    e.size = (juce::uint8) meta.numBytes;
                    std::memcpy (e.data, meta.data, (size_t) meta.numBytes);
                }
            }

            // 원곡 위치: 흐르기 시작했거나 멈췄거나 건너뛰었을 때만 적는다(예비박도 흐르는 것으로 센다)
            const bool rolling = songRolling || countIn.remaining > 0;
            const bool jumped = rolling ? std::abs (songTime - (recLastSongTime + recLastChunk / sampleRate)) > 2.0 / sampleRate
                                        : songTime != recLastSongTime;

            if ((! recSpanStarted || rolling != recLastRolling || jumped) && recSpanCount < recSpans.size())
                recSpans[recSpanCount++] = { base, songTime, rolling };

            recSpanStarted = true;
            recLastRolling = rolling;
            recLastSongTime = songTime;
            recLastChunk = numSamples;

            recordedSamples.fetch_add (numSamples, std::memory_order_relaxed);
        }
    }

    // --- 메트로놈: 녹음 뒤에 더해서 들리기만 한다.
    renderClicks (countIn, songRolling, songTime, numSamples, out, numOut, offset);
}
