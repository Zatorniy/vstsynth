//-----------------------------------------------------------------------------
// SynthesizerLab - VST3 parameter IDs
// Filename    : src/plugin/pids.h
// Порядок и количество должны совпадать с SynthController::initialize!
//-----------------------------------------------------------------------------
#pragma once

namespace synthlab {

enum ParameterID : Steinberg::Vst::ParamID
{
	kOsc1Wave = 0,      // enum: Sine/Triangle/SawUp/SawDown/Square/Pulse
	kPulseWidth,        // только для Pulse
	kOsc2Wave,          // enum (та же шкала)
	kOsc2Level,         // 0..1
	kDetune,            // -100..+100 центов
	kFMAmount,          // 0..1
	kRingMod,           // 0..1
	kOsc3Role,          // enum: LFO / Ring
	kOsc3Ratio,         // 0..1 -> 0.001..4.0
	kLfoRate,           // 0..12 Гц
	kLfoDepth,          // 0..100 центов
	kUnisonDetune,      // 0..50 центов

	kFilterCutoff,      // 0..1 -> 20..20000 Гц
	kFilterReso,        // 0..1
	kFilterEnvAmt,      // 0..1
	kFiltAttack,        // 0.001..4 c
	kFiltDecay,         // 0.001..4 c
	kFiltSustain,       // 0..1
	kFiltRelease,       // 0.005..8 c

	kAmpAttack,         // 0.001..4 c
	kAmpDecay,          // 0.001..4 c
	kAmpSustain,        // 0..1
	kAmpRelease,        // 0.005..8 c

	kToneLP,            // 0..1 -> 200..20000 Гц
	kToneHP,            // 0..1 -> 20..2000 Гц
	kDelayTime,         // 0..1 -> 0..0.9 c
	kDelayFeedback,     // 0..1
	kDelayMix,          // 0..1
	kVolume,            // 0..1

	kNumParameters
};

} // namespace synthlab
