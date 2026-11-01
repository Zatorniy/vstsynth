//-----------------------------------------------------------------------------
// SynthesizerLab - VST3 class UIDs
// Filename    : src/plugin/cids.h
//-----------------------------------------------------------------------------
#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace synthlab {

static const Steinberg::FUID kProcessorUID (0xD1E57B92, 0x4A6C3F08, 0xB9A21E76, 0x5C0F4D31);
static const Steinberg::FUID kControllerUID (0xA7F32C18, 0x9E0D4B65, 0x8C31AF57, 0x2D6E90B4);

#define SynthLabVST3Category "Instrument|Synth"

} // namespace synthlab
