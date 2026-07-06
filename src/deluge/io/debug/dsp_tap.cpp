/*
 * DSP golden-buffer tap — see dsp_tap.h. Debug-only (trudaine fork).
 */
#include "io/debug/dsp_tap.h"

namespace DspTap {

static int32_t g_buf[kMaxSamples];
static volatile int32_t g_remaining = 0; // samples still to capture (0 = idle/done)
static int32_t g_writePos = 0;
static int32_t g_captured = 0;                // samples captured since last arm
static volatile bool g_armOnNextNote = false; // arm at the next note onset

void arm() {
	g_writePos = 0;
	g_captured = 0;
	g_remaining = kMaxSamples;
}

void armOnNextNote() {
	g_armOnNextNote = true;
}

void onNoteStart() {
	if (g_armOnNextNote) {
		g_armOnNextNote = false;
		arm();
	}
}

void capture(std::span<StereoSample> buffer) {
	int32_t rem = g_remaining;
	if (rem <= 0) {
		return;
	}
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

int32_t capturedCount() {
	return g_captured;
}

const int32_t* data() {
	return g_buf;
}

} // namespace DspTap
