//-----------------------------------------------------------------------------
// SynthesizerLab - VST3 AudioProcessor (обёртка над synth::SynthEngine)
// Filename    : src/plugin/processor.cpp
//-----------------------------------------------------------------------------

#include "cids.h"
#include "pids.h"

#include "../dsp/SynthEngine.h"

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/utility/rttransfer.h"
#include "base/source/fstreamer.h"

#include <algorithm>
#include <cmath>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace synthlab {

//------------------------------------------------------------------------
struct ParamSnapshot
{
	synth::SynthParams params;
};

/// Нормализованное 0..1 -> экспоненциальная шкала [min..max]
static inline double expScale (double v, double mn, double mx)
{
	return mn * std::pow (mx / mn, std::clamp (v, 0.0, 1.0));
}

/// Обратная шкала: значение в единицах -> нормализованное 0..1
static inline double invExpScale (double value, double mn, double mx)
{
	return std::log (std::clamp (value, mn, mx) / mn) / std::log (mx / mn);
}

/// Маппинг нормализованного значения параметра в поле SynthParams
static void applyParam (synth::SynthParams& p, ParamID id, double v)
{
	v = std::clamp (v, 0.0, 1.0);
	switch (id)
	{
		case ParameterID::kOsc1Wave:     p.osc1Wave = static_cast<synth::Waveform> (std::min ((int)(v * 5.999), 5)); break;
		case ParameterID::kPulseWidth:   p.pulseWidth = std::clamp (v, 0.02, 0.98); break;
		case ParameterID::kOsc2Wave:     p.osc2Wave = static_cast<synth::Waveform> (std::min ((int)(v * 5.999), 5)); break;
		case ParameterID::kOsc2Level:    p.osc2Level = v; break;
		case ParameterID::kDetune:       p.detuneCents = v * 200.0 - 100.0; break;
		case ParameterID::kFMAmount:     p.fmAmount = v; break;
		case ParameterID::kRingMod:      p.ringMod = v; break;
		case ParameterID::kOsc3Role:     p.osc3Role = v < 0.5 ? 0 : 1; break;
		case ParameterID::kOsc3Ratio:    p.osc3Ratio = expScale (v, 0.001, 4.0); break;
		case ParameterID::kLfoRate:      p.lfoRateHz = v * 12.0; break;
		case ParameterID::kLfoDepth:     p.lfoDepthCents = v * 100.0; break;
		case ParameterID::kUnisonDetune: p.unisonDetuneCents = v * 50.0; break;
		case ParameterID::kFilterCutoff: p.filterCutoff = v; break;
		case ParameterID::kFilterReso:   p.filterReso = v; break;
		case ParameterID::kFilterEnvAmt: p.filterEnvAmount = v; break;
		case ParameterID::kFiltAttack:   p.filterAds.attack = expScale (v, 0.001, 4.0); break;
		case ParameterID::kFiltDecay:    p.filterAds.decay = expScale (v, 0.001, 4.0); break;
		case ParameterID::kFiltSustain:  p.filterAds.sustain = v; break;
		case ParameterID::kFiltRelease:  p.filterAds.release = expScale (v, 0.005, 8.0); break;
		case ParameterID::kAmpAttack:    p.ampAds.attack = expScale (v, 0.001, 4.0); break;
		case ParameterID::kAmpDecay:     p.ampAds.decay = expScale (v, 0.001, 4.0); break;
		case ParameterID::kAmpSustain:   p.ampAds.sustain = v; break;
		case ParameterID::kAmpRelease:   p.ampAds.release = expScale (v, 0.005, 8.0); break;
		case ParameterID::kToneLP:       p.toneLp = v; break;
		case ParameterID::kToneHP:       p.toneHp = v; break;
		case ParameterID::kDelayTime:    p.delayTime = v; break;
		case ParameterID::kDelayFeedback:p.delayFeedback = v; break;
		case ParameterID::kDelayMix:     p.delayMix = v; break;
		case ParameterID::kVolume:       p.volume = v; break;
		default: break;
	}
}

/// Нормализованное представление текущих параметров (для getState)
static void snapshotToNormalized (const synth::SynthParams& p, double* vals)
{
	vals[ParameterID::kOsc1Wave] = (double)p.osc1Wave / 5.0;
	vals[ParameterID::kPulseWidth] = p.pulseWidth;
	vals[ParameterID::kOsc2Wave] = (double)p.osc2Wave / 5.0;
	vals[ParameterID::kOsc2Level] = p.osc2Level;
	vals[ParameterID::kDetune] = (p.detuneCents + 100.0) / 200.0;
	vals[ParameterID::kFMAmount] = p.fmAmount;
	vals[ParameterID::kRingMod] = p.ringMod;
	vals[ParameterID::kOsc3Role] = p.osc3Role ? 1.0 : 0.0;
	vals[ParameterID::kOsc3Ratio] = invExpScale (p.osc3Ratio, 0.001, 4.0);
	vals[ParameterID::kLfoRate] = p.lfoRateHz / 12.0;
	vals[ParameterID::kLfoDepth] = p.lfoDepthCents / 100.0;
	vals[ParameterID::kUnisonDetune] = p.unisonDetuneCents / 50.0;
	vals[ParameterID::kFilterCutoff] = p.filterCutoff;
	vals[ParameterID::kFilterReso] = p.filterReso;
	vals[ParameterID::kFilterEnvAmt] = p.filterEnvAmount;
	vals[ParameterID::kFiltAttack] = invExpScale (p.filterAds.attack, 0.001, 4.0);
	vals[ParameterID::kFiltDecay] = invExpScale (p.filterAds.decay, 0.001, 4.0);
	vals[ParameterID::kFiltSustain] = p.filterAds.sustain;
	vals[ParameterID::kFiltRelease] = invExpScale (p.filterAds.release, 0.005, 8.0);
	vals[ParameterID::kAmpAttack] = invExpScale (p.ampAds.attack, 0.001, 4.0);
	vals[ParameterID::kAmpDecay] = invExpScale (p.ampAds.decay, 0.001, 4.0);
	vals[ParameterID::kAmpSustain] = p.ampAds.sustain;
	vals[ParameterID::kAmpRelease] = invExpScale (p.ampAds.release, 0.005, 8.0);
	vals[ParameterID::kToneLP] = p.toneLp;
	vals[ParameterID::kToneHP] = p.toneHp;
	vals[ParameterID::kDelayTime] = p.delayTime;
	vals[ParameterID::kDelayFeedback] = p.delayFeedback;
	vals[ParameterID::kDelayMix] = p.delayMix;
	vals[ParameterID::kVolume] = p.volume;
}

//------------------------------------------------------------------------
class SynthProcessor : public AudioEffect
{
public:
	SynthProcessor ();
	tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE;
	tresult PLUGIN_API terminate () SMTG_OVERRIDE;
	tresult PLUGIN_API setupProcessing (ProcessSetup& setup) SMTG_OVERRIDE;
	tresult PLUGIN_API setBusArrangements (SpeakerArrangement* inputs, int32 numIns,
	                                       SpeakerArrangement* outputs, int32 numOuts) SMTG_OVERRIDE;
	tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) SMTG_OVERRIDE
	{
		return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
	}
	tresult PLUGIN_API process (ProcessData& data) SMTG_OVERRIDE;
	tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE;
	tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE;

	static FUnknown* createInstance (void*) { return static_cast<AudioProcessor*> (new SynthProcessor ()); }

private:
	void handleParameterChanges (IParameterChanges* changes);

	synth::SynthEngine engine;
	RTTransferT<ParamSnapshot> rtState;
	ParamSnapshot lastKnown; // актуальная копия для getState/setState на UI-тредe
};

//------------------------------------------------------------------------
SynthProcessor::SynthProcessor ()
{
	setControllerClass (kControllerUID);
	lastKnown.params = synth::SynthParams{};
	rtState.setObject (new ParamSnapshot (lastKnown));
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::initialize (FUnknown* context)
{
	auto res = AudioEffect::initialize (context);
	if (res != kResultTrue)
		return res;

	addEventInput ("MIDI Input", 16);
	addAudioOutput ("Output", SpeakerArr::kStereo);
	return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::terminate ()
{
	rtState.clear_rt ();
	rtState.clear_ui ();
	return AudioEffect::terminate ();
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::setupProcessing (ProcessSetup& setup)
{
	engine.prepare (setup.sampleRate, setup.maxSamplesPerBlock);
	engine.allNotesOff ();
	return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::setBusArrangements (SpeakerArrangement* inputs, int32 numIns,
                                                      SpeakerArrangement* outputs, int32 numOuts)
{
	if (numOuts == 1 && SpeakerArr::getChannelCount (outputs[0]) == 2)
		return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
	return kResultFalse;
}

//------------------------------------------------------------------------
void SynthProcessor::handleParameterChanges (IParameterChanges* changes)
{
	if (!changes || changes->getParameterCount () == 0)
		return;

	ParamSnapshot* snap = rtState.syncN2O ();
	if (!snap)
		return;

	for (int32 i = 0; i < changes->getParameterCount (); ++i)
	{
		auto* queue = changes->getParameterData (i);
		if (!queue || queue->getPointCount () == 0)
			continue;
		Vst::ParamValue value;
		if (queue->getPoint (queue->getPointCount () - 1, nullptr, nullptr, value) != kResultTrue)
			continue;
		applyParam (snap->params, queue->getParameterId (), value);
	}
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::process (ProcessData& data)
{
	if (data.numOutputs == 0 || data.numSamples <= 0 || !data.outputs[0].channelBuffers32)
		return kResultOk;

	handleParameterChanges (data.inputParameterChanges);
	const ParamSnapshot* snap = rtState.syncO2N ();
	if (snap)
		lastKnown = *snap; // держим UI-копию для getState
	const auto& p = lastKnown.params;

	// --- MIDI-события ---
	if (data.inputEvents)
	{
		int32 numEvents = 0;
		Event* events = data.inputEvents->getEvents (numEvents);
		for (int32 i = 0; i < numEvents; ++i)
		{
			const Event& e = events[i];
			switch (e.type)
			{
				case Event::kNoteOnEvent:
					engine.noteOn (e.noteOn.pitch, e.noteOn.velocity, p);
					break;
				case Event::kNoteOffEvent:
					engine.noteOff (e.noteOff.pitch);
					break;
				case Event::kLegacyMIDICCOutEvent:
					// Pitch bend: controlNumber==kPitchBend(129); value=LSB, value2=MSB -> 0..16383
					if (e.midiCCOut.controlNumber == kPitchBend)
					{
						const int pb = ((int)(uint8)e.midiCCOut.value2 << 7) | (int)(uint8)e.midiCCOut.value;
						engine.setPitchBend ((pb - 8192) / 8192.0 * 2.0);
					}
					break;
				default: break;
			}
		}
	}

	// --- рендер блока ---
	float** outCh = data.outputs[0].channelBuffers32;
	const int32 numChannels = data.outputs[0].numChannels;
	std::fill_n (outCh[0], data.numSamples, 0.0f);
	if (numChannels > 1)
		std::fill_n (outCh[1], data.numSamples, 0.0f);

	engine.process (p, outCh, numChannels, data.numSamples);
	data.outputs[0].silenceFlags = 0;
	return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::setState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;
	IBStreamer streamer (state, kLittleEndian);
	ParamSnapshot snap;
	for (int i = 0; i < (int)ParameterID::kNumParameters; ++i)
	{
		double v = 0.0;
		if (!streamer.readDouble (v))
			return kResultFalse;
		applyParam (snap.params, (ParamID)i, v);
	}
	lastKnown = snap;
	auto* rt = new ParamSnapshot (snap);
	rtState.transferObject_ui (rt);
	return kResultTrue;
}

//------------------------------------------------------------------------
tresult PLUGIN_API SynthProcessor::getState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;
	OBStreamer streamer (state, kLittleEndian);
	double vals[(int)ParameterID::kNumParameters];
	snapshotToNormalized (lastKnown.params, vals);
	for (int i = 0; i < (int)ParameterID::kNumParameters; ++i)
		streamer.writeDouble (std::clamp (vals[i], 0.0, 1.0));
	return kResultTrue;
}

//------------------------------------------------------------------------
} // namespace synthlab
