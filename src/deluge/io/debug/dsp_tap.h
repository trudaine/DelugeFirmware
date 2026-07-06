/*
 * DSP golden-buffer tap — captures a window of the master output for bit-exact
 * comparison against the Java Deluge emulation (org.deluge). Debug-only: added on
 * the trudaine fork to resolve DSP parity gaps (FM index, saturation, master gain)
 * that read-audits cannot settle. Not part of upstream firmware.
 */
#pragma once

#include "dsp/stereo_sample.h"
#include <cstdint>
#include <span>

namespace DspTap {

// Mono (left-channel) capture window. 4096 samples = ~93 ms at 44.1 kHz, 16 KB SRAM.
constexpr int32_t kMaxSamples = 4096;

/** Begin capturing the next kMaxSamples of master-output left samples. */
void arm();

/** Arm the tap automatically when the next note starts (onset-synced capture of the attack). */
void armOnNextNote();

/** Called from Voice::noteOn — arms the tap if armOnNextNote() was requested. */
void onNoteStart();

/** Called once per audio block from renderAudio with the final master buffer. */
void capture(std::span<StereoSample> buffer);

/** Number of samples captured since the last arm (== kMaxSamples once complete). */
int32_t capturedCount();

/** The captured mono sample buffer (int32/q31, LSB-first per sample when serialized). */
const int32_t* data();

} // namespace DspTap
