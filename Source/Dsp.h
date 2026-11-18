#pragma once

#include <algorithm>
#include <cmath>

// 믹서 채널에 쓰는 가벼운 DSP: 3밴드 톤(EQ), 원노브 컴프레서, 마스터 리미터.
// 전부 오디오 스레드에서 메모리 할당 없이 동작한다.
namespace mixdsp
{
constexpr double pi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept   { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float gain) noexcept { return 20.0f * std::log10 (std::max (gain, 1.0e-9f)); }

//==============================================================================
struct Biquad
{
    void reset() noexcept
    {
        z1[0] = z1[1] = z2[0] = z2[1] = 0.0f;
    }

    void setCoefficients (double B0, double B1, double B2, double A0, double A1, double A2) noexcept
    {
        const double inv = 1.0 / A0;
        b0 = (float) (B0 * inv);
        b1 = (float) (B1 * inv);
        b2 = (float) (B2 * inv);
        a1 = (float) (A1 * inv);
        a2 = (float) (A2 * inv);
    }

    void setPeak (double sampleRate, double freq, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * pi * freq / sampleRate;
        const double cs = std::cos (w);
        const double alpha = std::sin (w) / (2.0 * q);

        setCoefficients (1.0 + alpha * A, -2.0 * cs, 1.0 - alpha * A,
                         1.0 + alpha / A, -2.0 * cs, 1.0 - alpha / A);
    }

    void setLowShelf (double sampleRate, double freq, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * pi * freq / sampleRate;
        const double cs = std::cos (w);
        const double k = 2.0 * std::sqrt (A) * (std::sin (w) * 0.5 * std::sqrt (2.0));

        setCoefficients (A * ((A + 1.0) - (A - 1.0) * cs + k),
                         2.0 * A * ((A - 1.0) - (A + 1.0) * cs),
                         A * ((A + 1.0) - (A - 1.0) * cs - k),
                         (A + 1.0) + (A - 1.0) * cs + k,
                         -2.0 * ((A - 1.0) + (A + 1.0) * cs),
                         (A + 1.0) + (A - 1.0) * cs - k);
    }

    void setHighShelf (double sampleRate, double freq, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w = 2.0 * pi * freq / sampleRate;
        const double cs = std::cos (w);
        const double k = 2.0 * std::sqrt (A) * (std::sin (w) * 0.5 * std::sqrt (2.0));

        setCoefficients (A * ((A + 1.0) + (A - 1.0) * cs + k),
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * cs),
                         A * ((A + 1.0) + (A - 1.0) * cs - k),
                         (A + 1.0) - (A - 1.0) * cs + k,
                         2.0 * ((A - 1.0) - (A + 1.0) * cs),
                         (A + 1.0) - (A - 1.0) * cs - k);
    }

    inline float tick (int channel, float x) noexcept
    {
        const float y = b0 * x + z1[channel];
        z1[channel] = b1 * x - a1 * y + z2[channel];
        z2[channel] = b2 * x - a2 * y;
        return y;
    }

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1[2] {}, z2[2] {};
};

//==============================================================================
// 저음(120Hz 셸프) / 중음(1kHz 벨) / 고음(6kHz 셸프). 세 값이 모두 0dB면 아무 처리도 하지 않는다.
struct ToneEq
{
    static constexpr double lowFreq = 120.0, midFreq = 1000.0, midQ = 0.75, highFreq = 6000.0;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        low = {};
        mid = {};
        high = {};
        curLow = curMid = curHigh = 0.0f;
        active = false;
    }

    // 블록마다 한 번 호출. 값이 급하게 바뀌어도 지직거리지 않게 조금씩 따라간다.
    void setGains (float lowDb, float midDb, float highDb) noexcept
    {
        const auto approach = [] (float current, float target) noexcept
        {
            const float diff = target - current;
            return std::abs (diff) < 0.02f ? target : current + diff * 0.4f;
        };

        if (const float v = approach (curLow, lowDb); v != curLow)
        {
            curLow = v;
            low.setLowShelf (sampleRate, lowFreq, v);
        }

        if (const float v = approach (curMid, midDb); v != curMid)
        {
            curMid = v;
            mid.setPeak (sampleRate, midFreq, midQ, v);
        }

        if (const float v = approach (curHigh, highDb); v != curHigh)
        {
            curHigh = v;
            high.setHighShelf (sampleRate, highFreq, v);
        }

        const bool nowActive = curLow != 0.0f || curMid != 0.0f || curHigh != 0.0f;

        if (active && ! nowActive)
        {
            low.reset();
            mid.reset();
            high.reset();
        }

        active = nowActive;
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (! active)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            left[i]  = high.tick (0, mid.tick (0, low.tick (0, left[i])));
            right[i] = high.tick (1, mid.tick (1, low.tick (1, right[i])));
        }
    }

    double sampleRate = 48000.0;
    Biquad low, mid, high;
    float curLow = 0.0f, curMid = 0.0f, curHigh = 0.0f;
    bool active = false;
};

//==============================================================================
// 노브 하나(0~1)로 조절하는 컴프레서. 올릴수록 스레숄드가 내려가고 줄어든 만큼 일부를 다시 키운다.
struct Compressor
{
    static constexpr float ratio = 4.0f, kneeDb = 6.0f;

    void prepare (double sampleRate) noexcept
    {
        attack  = 1.0f - std::exp (-1.0f / (0.008f * (float) sampleRate));
        release = 1.0f - std::exp (-1.0f / (0.140f * (float) sampleRate));
        reductionDb = 0.0f;
    }

    void setAmount (float amount01) noexcept
    {
        amount = std::clamp (amount01, 0.0f, 1.0f);
        thresholdDb = -32.0f * amount;
        makeup = dbToGain (-thresholdDb * slope * 0.45f);
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        if (amount <= 0.001f)
        {
            reductionDb = 0.0f;
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = std::max (std::abs (left[i]), std::abs (right[i]));
            const float over = gainToDb (peak) - thresholdDb;

            float target = 0.0f;

            if (over >= kneeDb * 0.5f)
                target = over * slope;
            else if (over > -kneeDb * 0.5f)
                target = slope * (over + kneeDb * 0.5f) * (over + kneeDb * 0.5f) / (2.0f * kneeDb);

            reductionDb += (target - reductionDb) * (target > reductionDb ? attack : release);

            const float gain = dbToGain (-reductionDb) * makeup;
            left[i] *= gain;
            right[i] *= gain;
        }
    }

    static constexpr float slope = 1.0f - 1.0f / ratio;
    float amount = 0.0f, thresholdDb = 0.0f, makeup = 1.0f;
    float attack = 0.0f, release = 0.0f, reductionDb = 0.0f;
};

//==============================================================================
// 출력이 찢어지지 않게 막는 안전장치. 천장(-0.3dB)을 넘는 순간 바로 줄이고 천천히 되돌린다.
struct Limiter
{
    static constexpr float ceiling = 0.966f;

    void prepare (double sampleRate) noexcept
    {
        release = 1.0f - std::exp (-1.0f / (0.080f * (float) sampleRate));
        gain = 1.0f;
    }

    void process (float* left, float* right, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = std::max (std::abs (left[i]), std::abs (right[i]));
            const float target = peak > ceiling ? ceiling / peak : 1.0f;

            if (target < gain)
                gain = target;
            else
                gain += (target - gain) * release;

            left[i] *= gain;
            right[i] *= gain;
        }
    }

    float release = 0.0f, gain = 1.0f;
};

} // namespace mixdsp
