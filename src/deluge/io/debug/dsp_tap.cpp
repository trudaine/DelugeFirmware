/*
 * DSP golden-buffer tap — see dsp_tap.h. Debug-only (trudaine fork).
 */
#include "io/debug/dsp_tap.h"

namespace DspTap {

static int32_t g_buf[kMaxSamples];
static volatile int32_t g_remaining = 0; // entries still to capture (0 = idle/done/stopped)
static int32_t g_writePos = 0;
static int32_t g_captured = 0;           // entries captured since last arm
static volatile int g_source = 0;        // 0 = master (per-sample), 1 = modulator (per-block)
static volatile int g_armOnNextNote = 0; // 0 = no, 1 = master, 2 = modulator

static void armInternal(int source) {
	g_writePos = 0;
	g_captured = 0;
	g_source = source;
	g_remaining = kMaxSamples;
}

void arm() {
	armInternal(0);
}

void armOnNextNote() {
	g_armOnNextNote = 1;
	g_captured = 0;
	g_remaining = 0;
}

void armModulatorOnNextNote() {
	g_armOnNextNote = 2;
	g_captured = 0;
	g_remaining = 0;
}

void stop() {
	g_remaining = 0;
}

void onNoteStart() {
	int a = g_armOnNextNote;
	if (a) {
		g_armOnNextNote = 0;
		armInternal(a == 2 ? 1 : 0);
	}
}

void capture(std::span<StereoSample> buffer) {
	if (g_source != 0 || g_remaining <= 0) {
		return;
	}
	int32_t rem = g_remaining;
	int32_t pos = g_writePos;
	for (StereoSample& s : buffer) {
		if (rem <= 0) {
			break;
		}
		g_buf[pos++] = s.l;
		rem--;
	}
	g_writePos = pos;
	g_captured = pos;
	g_remaining = rem;
}

void captureModulator(int32_t modAmp) {
	if (g_source != 1 || g_remaining <= 0) {
		return;
	}
	g_buf[g_writePos++] = modAmp;
	g_captured = g_writePos;
	g_remaining = g_remaining - 1;
}

int32_t capturedCount() {
	return g_captured;
}

const int32_t* data() {
	return g_buf;
}

} // namespace DspTap
