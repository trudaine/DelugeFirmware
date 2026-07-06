/*
 * DSP golden-buffer tap — captures a window of the master output OR the per-block modulator
 * amplitude for bit-exact comparison against the Java Deluge emulation (org.deluge). Debug-only:
 * added on the trudaine fork to resolve DSP parity gaps (FM index/envelope, saturation, master
 * gain) that read-audits cannot settle. Not part of upstream firmware.
 */
#pragma once

#include "dsp/stereo_sample.h"
#include <cstdint>
#include <span>

namespace DspTap {

// Capture window. As master samples: 4096 = ~93 ms @44.1 kHz. As per-block modulator values:
// 4096 blocks ≈ 11.9 s (one value per audio block) — covers a full note's decay. 16 KB SRAM.
constexpr int32_t kMaxSamples = 4096;

/** Begin capturing the next kMaxSamples of master-output left samples (per-sample). */
void arm();

/** Arm the MASTER tap at the next note onset (onset-synced attack capture). */
void armOnNextNote();

/** Arm the MODULATOR-amplitude tap at the next note onset (one value per block, whole envelope). */
void armModulatorOnNextNote();

/** Freeze the capture (for the slow per-block modulator capture — read the partial buffer). */
void stop();

/** Called once per audio block from renderAudio with the final master buffer (master source). */
void capture(std::span<StereoSample> buffer);

/** Called once per block per FM voice from Voice::render with the modulator amplitude (mod source). */
void captureModulator(int32_t modAmp);

/** Called from Voice::noteOn — arms the tap if an arm-on-next-note was requested. */
void onNoteStart();

/** Number of entries captured since the last arm. */
int32_t capturedCount();

/** The captured buffer (master samples or per-block modulator values, int32). */
const int32_t* data();

} // namespace DspTap
