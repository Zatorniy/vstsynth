//-----------------------------------------------------------------------------
// SynthesizerLab - DSP Engine (header)
// Filename    : src/dsp/SynthEngine.h
// Description : Портативный 8-голосный субтрактивный движок. НЕ зависит от
//               VST3 -> его можно тестировать отдельно от плагина
//               (см. tests/test_dsp.cpp).
//
// Цепочка одного голоса:
//   Osc1 (+Osc2: detune / FM cross-mod, +Osc3: LFO или ring-mod)
//   -> резонансный LP-фильтр (огибающая по клавише)
//   -> VCA (амплитудная ADSR)
// Все голоса -> тембральные master LP/HP -> стерео-дилей -> volume + soft clip
//-----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace synth {

constexpr int kMaxPolyphony = 8;
constexpr double kPi = 3.14159265358979323846;

//------------------------------------------------------------------------
enum class Waveform : int
{
	Sine = 0,
	Triangle,
	SawUp,
	SawDown,
	Square,
	Pulse, ///< ширина импульса = pulseWidth (0..1)
	NumWaveforms
};

/// Значение формы волны (-1..+1) для фазы в [0,1)
double evalWaveform(Waveform wf, double phase, double pulseWidth);

/// MIDI-клавиша -> частота Гц (A4=69 -> 440)
inline double midiKeyToFreq(int key)
{
	return 440.0 * std::pow(2.0, (key - 69) / 12.0);
}

/// Нормализованное значение 0..1 -> экспоненциальная шкала частоты cutoff
inline double normalizedToHz(double v01, double minHz, double maxHz)
{
	v01 = std::clamp(v01, 0.0, 1.0);
	return minHz * std::pow(maxHz / minHz, v01);
}

//------------------------------------------------------------------------
/// Однополюсный LP/HP (для тембра и развязки постоянной)
class OnePole
{
public:
	void setParameters(double cutoffHz, double sampleRate, bool highpass = false);
	double process(double x);
	void reset() { y_ = 0.0; }

private:
	double coef_ = 1.0;
	double y_ = 0.0;
	bool hp_ = false;
};

//------------------------------------------------------------------------
/// Резонансный биквадрат RBJ (double precision), LP/HP
class ResonantFilter
{
public:
	enum Type { kLowpass, kHighpass };
	void setParameters(double cutoffHz, double qLinear /*0..1*/, double sampleRate, Type type);
	double process(double x);
	void reset() { x1_ = x2_ = y1_ = y2_ = 0.0; }

private:
	double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
	double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

//------------------------------------------------------------------------
/// Экспоненциальная ADSR без щелчков
class AdsrEnvelope
{
public:
	enum Stage { kIdle, kAttack, kDecay, kSustain, kRelease };

	void setSampleRate(double sr) { sampleRate_ = sr > 0 ? sr : 44100.0; }
	void setParameters(double attackSec, double decaySec, double sustain01, double releaseSec);
	void noteOn();
	void noteOff();
	double process();
	Stage stage() const { return stage_; }
	bool isIdle() const { return stage_ == kIdle; }
	double current() const { return current_; }

private:
	void updateCoefs();
	double sampleRate_ = 44100.0;
	double attack_ = 0.01, decay_ = 0.2, sustain_ = 0.7, release_ = 0.2;
	double current_ = 0.0;
	double target_ = 1.0, expCoef_ = 0.0;
	Stage stage_ = kIdle;
};

//------------------------------------------------------------------------
struct AdsrTimes
{
	double attack = 0.02, decay = 0.25, sustain = 0.7, release = 0.3; // секунды
};

//------------------------------------------------------------------------
/// Параметры синтезатора. Всё нормализовано 0..1, кроме явных единиц.
struct SynthParams
{
	Waveform osc1Wave = Waveform::SawUp;
	Waveform osc2Wave = Waveform::Square;
	double osc2Level = 0.35;      // 0..1
	double detuneCents = 8.0;     // -100..100 (расстройка osc2)
	double fmAmount = 0.0;        // 0..1  FM: osc2 модулирует частоту osc1
	double ringMod = 0.0;         // 0..1  osc1 * osc3
	int    osc3Role = 0;          // 0 = LFO (вибрато), 1 = ring-модулятор
	double osc3Ratio = 0.0625;    // 0..1 -> 0.001..4.0 от частоты ноты (в роли ring)
	double lfoRateHz = 5.0;       // 0..12 Гц - скорость LFO (вибрато)
	double lfoDepthCents = 25.0;  // 0..100 центов - глубина вибрато
	double unisonDetuneCents = 0; // 0..50 расстройка голосов полифонии

	double filterCutoff = 0.6;    // 0..1 -> 20..20000 Гц
	double filterReso = 0.25;     // 0..1
	double filterEnvAmount = 0.5; // 0..1 -> сколько огибающая поднимает cutoff (в октавах*2)
	AdsrTimes filterAds;
	AdsrTimes ampAds;

	double toneLp = 1.0;          // 0..1 -> 200..20000 Гц master LP
	double toneHp = 0.0;          // 0..1 -> 20..2000 Гц master HP
	double delayTime = 0.25;      // 0..1 -> 0..0.9 c
	double delayFeedback = 0.35;  // 0..1
	double delayMix = 0.2;        // 0..1
	double volume = 0.7;          // 0..1
};

//------------------------------------------------------------------------
class Voice
{
public:
	void noteOn(int midiKey, float velocity, const SynthParams& p, double sampleRate, double unisonIndex);
	void noteOff() { ampEnv_.noteOff(); filtEnv_.noteOff(); keyState_ = kReleasing; }
	bool isActive() const { return keyState_ != kIdle; }
	bool isFree() const { return keyState_ == kIdle; }
	int  midiKey() const { return midiKey_; }
	std::uint64_t startOrder() const { return order_; }
	void setOrder(std::uint64_t o) { order_ = o; }

	/// Один сэмпл голоса; outMono - суммируемый сигнал
	bool process(const SynthParams& p, double sampleRate, double pitchBendSemis, double& outMono);

private:
	enum KeyState { kIdle, kPlaying, kReleasing };
	KeyState keyState_ = kIdle;
	int   midiKey_ = 0;
	double baseFreq_ = 440.0;
	double ampVel_ = 0.0;
	std::uint64_t order_ = 0;

	double phase1_ = 0.0, phase2_ = 0.0, phase3_ = 0.0;
	double voiceDetuneRatio_ = 1.0; // унисон-расстройка этого голоса

	AdsrEnvelope ampEnv_;
	AdsrEnvelope filtEnv_;
	OnePole      dcBlocker_;  // режет постоянную от ring-mod
	ResonantFilter filter_;
};

//------------------------------------------------------------------------
/// Стерео дилей с обратной связью
class StereoDelay
{
public:
	void prepare(double sampleRate);
	void setParameters(double timeSeconds, double feedback01, double mix01, double sampleRate);
	void process(double& left, double& right);
	void reset();

private:
	static constexpr int kMaxSize = 256000;
	std::array<float, kMaxSize> bufL_{}, bufR_{};
	int writeIdx_ = 0;
	int size_ = kMaxSize;
	int delaySamples_ = 0;
	double feedback_ = 0.0;
	double wet_ = 0.0, dry_ = 1.0;
};

//------------------------------------------------------------------------
/// Главный движок: принимает note on/off + блок сэмплов
class SynthEngine
{
public:
	void prepare(double sampleRate, int maxBlockSize);

	void noteOn(int midiKey, float velocity, const SynthParams& p);
	void noteOff(int midiKey);
	void allNotesOff();
	void setPitchBend(double semitones) { pitchBendSemis_ = std::clamp(semitones, -2.0, 2.0); }

	/// Обработать блок. channels - планарные буферы (1 или 2 канала).
	void process(const SynthParams& p, float** channels, int numChannels, int numFrames);

	int activeVoiceCount() const;

private:
	Voice* findFreeVoice();
	Voice* findOldestActiveVoice();

	std::array<Voice, kMaxPolyphony> voices_{};
	std::uint64_t orderCounter_ = 0;
	double sampleRate_ = 44100.0;
	double pitchBendSemis_ = 0.0;
	ResonantFilter masterLp_, masterHp_;
	StereoDelay delay_;
};

} // namespace synth
