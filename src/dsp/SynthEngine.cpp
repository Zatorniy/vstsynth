//-----------------------------------------------------------------------------
// SynthesizerLab - DSP Engine (implementation)
// Filename    : src/dsp/SynthEngine.cpp
//-----------------------------------------------------------------------------
#include "SynthEngine.h"

namespace synth {

//------------------------------------------------------------------------
double evalWaveform(Waveform wf, double phase, double pulseWidth)
{
	phase = phase - std::floor(phase); // [0,1)
	switch (wf)
	{
		case Waveform::Sine:     return std::sin(2.0 * kPi * phase);
		case Waveform::Triangle: return 4.0 * std::fabs(phase - 0.5) - 1.0;
		case Waveform::SawUp:    return 2.0 * phase - 1.0;
		case Waveform::SawDown:  return 1.0 - 2.0 * phase;
		case Waveform::Square:   return phase < 0.5 ? 1.0 : -1.0;
		case Waveform::Pulse:    return phase < std::clamp(pulseWidth, 0.01, 0.99) ? 1.0 : -1.0;
		default:                 return 0.0;
	}
}

//------------------------------------------------------------------------
// OnePole
void OnePole::setParameters(double cutoffHz, double sampleRate, bool highpass)
{
	cutoffHz = std::clamp(cutoffHz, 10.0, sampleRate * 0.45);
	coef_ = 1.0 - std::exp(-2.0 * kPi * cutoffHz / sampleRate);
	hp_ = highpass;
}

double OnePole::process(double x)
{
	y_ += coef_ * (x - y_);
	return hp_ ? (x - y_) : y_;
}

//------------------------------------------------------------------------
// ResonantFilter (RBJ Audio EQ Cookbook)
void ResonantFilter::setParameters(double cutoffHz, double qLinear, double sampleRate, Type type)
{
	const double f = std::clamp(cutoffHz, 10.0, sampleRate * 0.45);
	const double q = 0.5 + std::clamp(qLinear, 0.0, 1.0) * 14.5; // Q 0.5..15
	const double w = 2.0 * kPi * f / sampleRate;
	const double cw = std::cos(w);
	const double sw = std::sin(w);
	const double alpha = sw / (2.0 * q);

	double b0, b1, b2, a0, a1, a2;
	if (type == kLowpass)
	{
		b0 = (1.0 - cw) / 2.0; b1 = 1.0 - cw; b2 = b0;
		a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
	}
	else
	{
		b0 = (1.0 + cw) / 2.0; b1 = -(1.0 + cw); b2 = b0;
		a0 = 1.0 + alpha; a1 = -2.0 * cw; a2 = 1.0 - alpha;
	}
	const double inv = 1.0 / a0;
	b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
	a1_ = a1 * inv; a2_ = a2 * inv;
}

double ResonantFilter::process(double x)
{
	const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
	x2_ = x1_; x1_ = x;
	y2_ = y1_; y1_ = y;
	return y;
}

//------------------------------------------------------------------------
// AdsrEnvelope
void AdsrEnvelope::setParameters(double attackSec, double decaySec, double sustain01, double releaseSec)
{
	attack_  = std::max(attackSec, 0.0005);
	decay_   = std::max(decaySec, 0.0005);
	sustain_ = std::clamp(sustain01, 0.0, 1.0);
	release_ = std::max(releaseSec, 0.005);
}

void AdsrEnvelope::updateCoefs()
{
	// экспоненциальное движение к target за ~3 временных константы
	double tau = 0.01;
	switch (stage_)
	{
		case kAttack:  target_ = 1.0;               tau = attack_ / 3.0;  break;
		case kDecay:   target_ = sustain_;          tau = decay_ / 3.0;   break;
		case kRelease: target_ = 0.0;               tau = release_ / 3.0; break;
		default:       target_ = current_;          tau = 1.0;            break;
	}
	expCoef_ = std::exp(-1.0 / (tau * sampleRate_));
}

void AdsrEnvelope::noteOn()
{
	current_ = 0.0;
	stage_ = kAttack;
	updateCoefs();
}

void AdsrEnvelope::noteOff()
{
	if (stage_ == kIdle)
		return;
	stage_ = kRelease;
	updateCoefs();
}

double AdsrEnvelope::process()
{
	switch (stage_)
	{
		case kAttack:
			current_ += (target_ - current_) * (1.0 - expCoef_);
			if (current_ > 0.999) { stage_ = kDecay; updateCoefs(); }
			break;
		case kDecay:
			current_ += (target_ - current_) * (1.0 - expCoef_);
			if (std::fabs(current_ - target_) < 0.002) stage_ = kSustain;
			break;
		case kSustain:
			current_ = sustain_;
			break;
		case kRelease:
			current_ += (target_ - current_) * (1.0 - expCoef_);
			if (current_ < 0.001) { current_ = 0.0; stage_ = kIdle; }
			break;
		case kIdle:
		default:
			current_ = 0.0;
			break;
	}
	return current_;
}

//------------------------------------------------------------------------
// Voice
void Voice::noteOn(int midiKey, float velocity, const SynthParams& p, double sampleRate, double unisonIndex)
{
	midiKey_ = midiKey;
	baseFreq_ = midiKeyToFreq(midiKey);
	ampVel_ = std::clamp(static_cast<double>(velocity), 0.0, 1.0);
	keyState_ = kPlaying;

	// унисонная расстройка голоса: index чередуется +/-
	const double sign = (static_cast<int>(unisonIndex) & 1) ? -1.0 : 1.0;
	const double cents = sign * p.unisonDetuneCents * ((unisonIndex / 2.0) + 1.0);
	voiceDetuneRatio_ = std::pow(2.0, cents / 1200.0);

	phase1_ = 0.0;
	phase2_ = std::fmod(unisonIndex * 0.37, 1.0); // разносим фазы голосов
	phase3_ = 0.0;

	ampEnv_.setSampleRate(sampleRate);
	ampEnv_.setParameters(p.ampAds.attack, p.ampAds.decay, p.ampAds.sustain, p.ampAds.release);
	ampEnv_.noteOn();

	filtEnv_.setSampleRate(sampleRate);
	filtEnv_.setParameters(p.filterAds.attack, p.filterAds.decay, p.filterAds.sustain, p.filterAds.release);
	filtEnv_.noteOn();

	dcBlocker_.setParameters(20.0, sampleRate, true);
	filter_.reset();
	dcBlocker_.reset();
}

bool Voice::process(const SynthParams& p, double sampleRate, double pitchBendSemis, double& outMono)
{
	if (keyState_ == kIdle)
	{
		outMono = 0.0;
		return false;
	}

	const double env = ampEnv_.process();
	const double fenv = filtEnv_.process();

	// базовая частота с pitch bend и унисоном
	const double noteFreq = baseFreq_ * std::pow(2.0, pitchBendSemis / 12.0) * voiceDetuneRatio_;

	// Osc3: в роли LFO даёт вибрато, в роли ring-модулятора - частоту от ratio
	const double osc3Ratio = 0.001 + p.osc3Ratio * 3.999;
	const double f3 = (p.osc3Role == 0)
		? std::clamp(p.lfoRateHz, 0.05, 20.0)
		: noteFreq * osc3Ratio;

	// LFO-вибрато: фаза osc3 -> смещение частоты osc1/osc2 в центах
	phase3_ += f3 / sampleRate;
	phase3_ -= std::floor(phase3_);
	const double vibratoCents = (p.osc3Role == 0)
		? evalWaveform(Waveform::Sine, phase3_, 0.5) * p.lfoDepthCents
		: 0.0;
	const double vibRatio = std::pow(2.0, vibratoCents / 1200.0);

	// FM: osc2 модулирует частоту osc1 (до 2x частоты ноты девиации)
	const double modIdx = p.fmAmount * noteFreq * 2.0;
	const double osc2Freq = noteFreq * std::pow(2.0, p.detuneCents / 1200.0) * vibRatio;
	const double s2prev = evalWaveform(p.osc2Wave, phase2_, 0.5); // значение до обновления фазы
	const double f1 = noteFreq * vibRatio + modIdx * s2prev;

	phase1_ += f1 / sampleRate;
	phase1_ -= std::floor(phase1_);
	phase2_ += osc2Freq / sampleRate;
	phase2_ -= std::floor(phase2_);

	const double s1 = evalWaveform(p.osc1Wave, phase1_, 0.5);
	const double s2 = s2prev;

	double sig = s1 + s2 * p.osc2Level;

	if (p.ringMod > 0.0)
	{
		const double s3 = evalWaveform(Waveform::Sine, phase3_, 0.5);
		sig = sig * (1.0 - p.ringMod) + (sig * s3) * p.ringMod;
		sig = dcBlocker_.process(sig);
	}

	// Фильтр: cutoff + огибающая (до 5 октав вверх)
	const double baseHz = normalizedToHz(p.filterCutoff, 20.0, 20000.0);
	const double envOct = fenv * p.filterEnvAmount * 5.0;
	const double cutHz = std::min(baseHz * std::pow(2.0, envOct), sampleRate * 0.45);
	filter_.setParameters(cutHz, p.filterReso, sampleRate, ResonantFilter::kLowpass);
	const double filtered = filter_.process(sig);

	outMono = filtered * env * ampVel_ * (0.85 / std::sqrt(static_cast<double>(kMaxPolyphony)));

	if (keyState_ == kReleasing && ampEnv_.isIdle())
	{
		keyState_ = kIdle;
		return false;
	}
	return true;
}

//------------------------------------------------------------------------
// StereoDelay
void StereoDelay::prepare(double sampleRate)
{
	size_ = static_cast<int>(sampleRate * 1.5);
	if (size_ > kMaxSize) size_ = kMaxSize;
	writeIdx_ = 0;
	delaySamples_ = std::max(1, static_cast<int>(sampleRate * 0.25));
}

void StereoDelay::setParameters(double timeSeconds, double feedback01, double mix01, double sampleRate)
{
	delaySamples_ = std::clamp(static_cast<int>(timeSeconds * sampleRate), 1, size_ - 1);
	feedback_ = std::clamp(feedback01, 0.0, 0.92);
	wet_ = std::clamp(mix01, 0.0, 1.0);
	dry_ = 1.0 - wet_;
}

void StereoDelay::process(double& left, double& right)
{
	const int readIdx = (writeIdx_ - delaySamples_ + size_) % size_;

	const double wetL = bufL_[readIdx];
	const double wetR = bufR_[readIdx];

	bufL_[writeIdx_] = static_cast<float>(left + wetL * feedback_);
	bufR_[writeIdx_] = static_cast<float>(right + wetR * feedback_);

	left  = left * dry_ + wetL * wet_;
	right = right * dry_ + wetR * wet_;

	writeIdx_ = (writeIdx_ + 1) % size_;
}

void StereoDelay::reset()
{
	bufL_.fill(0.f);
	bufR_.fill(0.f);
	writeIdx_ = 0;
}

//------------------------------------------------------------------------
// SynthEngine
void SynthEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
	sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
	masterLp_.reset();
	masterHp_.reset();
	delay_.prepare(sampleRate_);
	for (auto& v : voices_)
		v.noteOff(); // гарантированно в выключенное состояние
}

Voice* SynthEngine::findFreeVoice()
{
	for (auto& v : voices_)
		if (v.isFree())
			return &v;
	return nullptr;
}

Voice* SynthEngine::findOldestActiveVoice()
{
	Voice* oldest = nullptr;
	for (auto& v : voices_)
	{
		if (!v.isActive())
			continue;
		if (!oldest || v.startOrder() < oldest->startOrder())
			oldest = &v;
	}
	return oldest;
}

void SynthEngine::noteOn(int midiKey, float velocity, const SynthParams& p)
{
	midiKey = std::clamp(midiKey, 0, 127);

	// если этот ключ уже играет - перезапускаем самый старый его голос
	Voice* v = findFreeVoice();
	if (!v)
	{
		// voice steal: сначала тот же ключ, иначе самый старый
		for (auto& vv : voices_)
			if (vv.isActive() && vv.midiKey() == midiKey) { v = &vv; break; }
		if (!v)
			v = findOldestActiveVoice();
	}
	if (!v)
		return;

	const double idx = static_cast<double>((orderCounter_ % kMaxPolyphony));
	v->noteOn(midiKey, velocity, p, sampleRate_, idx);
	v->setOrder(orderCounter_++);
}

void SynthEngine::noteOff(int midiKey)
{
	for (auto& v : voices_)
		if (v.isActive() && v.midiKey() == midiKey)
			v.noteOff();
}

void SynthEngine::allNotesOff()
{
	for (auto& v : voices_)
		v.noteOff();
	delay_.reset();
}

int SynthEngine::activeVoiceCount() const
{
	int n = 0;
	for (auto& v : voices_)
		if (v.isActive()) ++n;
	return n;
}

static inline float softClip(double x)
{
	// tanh-подобный лимитер
	return static_cast<float>(std::tanh(x * 1.1) / std::tanh(1.1) * 0.95);
}

void SynthEngine::process(const SynthParams& p, float** channels, int numChannels, int numFrames)
{
	if (!channels || numChannels <= 0 || numFrames <= 0)
		return;

	const double sr = sampleRate_;
	const double gain = p.volume * p.volume * 4.0; // 0..4

	masterLp_.setParameters(normalizedToHz(p.toneLp, 200.0, 20000.0), 0.7, sr, ResonantFilter::kLowpass);
	masterHp_.setParameters(normalizedToHz(p.toneHp, 20.0, 2000.0), 0.7, sr, ResonantFilter::kHighpass);

	delay_.setParameters(p.delayTime * 0.9, p.delayFeedback, p.delayMix, sr);

	const bool stereoOut = numChannels > 1;

	for (int i = 0; i < numFrames; ++i)
	{
		double left = 0.0, right = 0.0;
		for (auto& v : voices_)
		{
			if (!v.isActive()) continue;
			double s = 0.0;
			v.process(p, sr, pitchBendSemis_, s);
			// лёгкая стерео-раскладка по порядку голоса
			if (stereoOut)
			{
				const double pan = ((static_cast<int>(v.startOrder()) % kMaxPolyphony) / (kMaxPolyphony - 1.0)) * 2.0 - 1.0;
				left  += s * (1.0 - std::max(0.0, pan) * 0.5);
				right += s * (1.0 - std::max(0.0, -pan) * 0.5);
			}
			else
			{
				left += s;
			}
		}
		right = stereoOut ? right : left;

		left  = masterHp_.process(masterLp_.process(left));
		right = masterHp_.process(masterLp_.process(right));

		left *= gain;
		right *= gain;

		delay_.process(left, right);

		channels[0][i] = softClip(left);
		if (stereoOut)
			channels[1][i] = softClip(right);
	}
}

} // namespace synth
