/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute
 * it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "io/midi/sysex.h"
#include "io/debug/dsp_tap.h"
#include "io/debug/print.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "util/chainload.h"

#include "util/pack.h"

#include <algorithm>

// DSP golden-buffer tap readback (debug-only, trudaine fork): stream one chunk of the captured
// master-output samples back as a Deluge SysEx reply. The 1 KB sysex_fmt_buffer forces chunking,
// so the host requests chunk 0, 1, ... until it has capturedCount samples. Reply payload (before
// 7-bit packing): [capturedCount:4 LE][chunkIndex:1][nSamples:2 LE][nSamples * int32 LE].
static void dspTapSendChunk(MIDICable& cable, uint8_t chunkIndex) {
	constexpr int32_t kSamplesPerChunk = 180;
	int32_t captured = DspTap::capturedCount();
	const int32_t* buf = DspTap::data();
	int32_t start = (int32_t)chunkIndex * kSamplesPerChunk;
	int32_t n = 0;
	if (start < captured) {
		n = std::min(kSamplesPerChunk, captured - start);
	}

	uint8_t raw[7 + kSamplesPerChunk * 4];
	raw[0] = captured & 0xFF;
	raw[1] = (captured >> 8) & 0xFF;
	raw[2] = (captured >> 16) & 0xFF;
	raw[3] = (captured >> 24) & 0xFF;
	raw[4] = chunkIndex;
	raw[5] = n & 0xFF;
	raw[6] = (n >> 8) & 0xFF;
	int32_t rawLen = 7;
	for (int32_t i = 0; i < n; i++) {
		int32_t v = buf[start + i];
		raw[rawLen++] = v & 0xFF;
		raw[rawLen++] = (v >> 8) & 0xFF;
		raw[rawLen++] = (v >> 16) & 0xFF;
		raw[rawLen++] = (v >> 24) & 0xFF;
	}

	const uint8_t hdr[] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x41, 0x00}; // 0x41 = DSP-tap reply
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, hdr, sizeof(hdr));
	int32_t packed = pack_8bit_to_7bit(reply + sizeof(hdr), 1024 - (int32_t)sizeof(hdr) - 1, raw, rawLen);
	reply[sizeof(hdr) + packed] = 0xF7;
	cable.sendSysex(reply, sizeof(hdr) + packed + 1);
}

void Debug::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}

	// Subcommand is second byte of payload
	switch (data[1]) {
	case 0:
		if (data[2] == 1) {
			midiDebugCable = &cable;
		}
		else if (data[2] == 0) {
			midiDebugCable = nullptr;
		}
		break;

	case 1:
#ifdef ENABLE_SYSEX_LOAD
		loadPacketReceived(data, len);
#endif
		break;

	case 2:
#ifdef ENABLE_SYSEX_LOAD
		loadCheckAndRun(data, len);
#endif
		break;

	case 3:
		// DSP golden-buffer tap: arm capture of the next window of master output.
		DspTap::arm();
		break;

	case 4:
		// DSP golden-buffer tap: read back chunk data[2] of the captured samples.
		dspTapSendChunk(cable, data[2]);
		break;

	case 5:
		// DSP golden-buffer tap: arm MASTER at the next note onset (onset-synced attack capture).
		DspTap::armOnNextNote();
		break;

	case 6:
		// DSP golden-buffer tap: arm MODULATOR amplitude at the next note onset (per-block envelope).
		DspTap::armModulatorOnNextNote();
		break;

	case 7:
		// DSP golden-buffer tap: freeze capture (read the partial per-block modulator buffer).
		DspTap::stop();
		break;

	default:
		break;
	}
}

void Debug::sysexDebugPrint(MIDICable& cable, const char* msg, bool nl) {
	if (!msg) {
		return; // Do not do that
	}
	// data[4]: reserved, could serve as a message identifier to filter messages
	// per category
	// 	uint8_t reply_hdr[] = {0xf0, 0x7d, 0x03, 0x40, 0x00};
	uint8_t reply_hdr[] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x40, 0x00};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	// memcpy(reply, reply_hdr, 5);
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	size_t len = strlen(msg);
	//	len = std::min(len, sizeof(midiEngine.sysex_fmt_buffer) - 7);
	len = std::min(len, sizeof(midiEngine.sysex_fmt_buffer) - (sizeof(reply_hdr) + 2));
	//	memcpy(reply + 5, msg, len);
	memcpy(reply + sizeof(reply_hdr), msg, len);
	for (int32_t i = 0; i < len; i++) {
		//		reply[5 + i] &= 0x7F; // only ascii debug messages
		reply[sizeof(reply_hdr) + i] &= 0x7F; // only ascii debug messages
	}
	if (nl) {
		//	reply[5 + len] = '\n';
		reply[sizeof(reply_hdr) + len] = '\n';
		len++;
	}
	//	reply[5 + len] = 0xf7;
	reply[sizeof(reply_hdr) + len] = 0xf7;
	//	device->sendSysex(reply, len + 6);
	cable.sendSysex(reply, len + sizeof(reply_hdr) + 1);
}
#ifdef ENABLE_SYSEX_LOAD
#include "gui/l10n/l10n.h"
#include "hid/display/oled.h"
#include "hid/led/pad_leds.h"
#include "memory/general_memory_allocator.h"
#include "model/settings/runtime_feature_settings.h"

static uint8_t* load_buf;
static size_t load_bufsize;
static size_t load_codesize;

static void firstPacket(uint8_t* data, int32_t len) {
	uint8_t tmpbuf[0x40] __attribute__((aligned(CACHE_LINE_SIZE)));

	unpack_7bit_to_8bit(tmpbuf, 0x40, data + 9, 0x4a);
	uint32_t user_code_start = *(uint32_t*)(tmpbuf + OFF_USER_CODE_START);
	uint32_t user_code_end = *(uint32_t*)(tmpbuf + OFF_USER_CODE_END);
	load_codesize = (int32_t)(user_code_end - user_code_start);
	if (load_bufsize < load_codesize) {
		if (load_buf != nullptr) {
			delugeDealloc(load_buf);
		}
		load_bufsize = load_codesize + (511 - ((load_codesize - 1) & 511));

		load_buf = (uint8_t*)GeneralMemoryAllocator::get().allocMaxSpeed(load_bufsize);
		if (load_buf == nullptr) {
			// fail :(
			return;
		}
	}

	// Pad LED Progress Bar Init
	PadLEDs::clearAllPadsWithoutSending();
	PadLEDs::sendOutMainPadColours();
	PadLEDs::sendOutSidebarColours();
	deluge::hid::display::OLED::clearMainImage();
	deluge::hid::display::OLED::sendMainImage();

	boostTask(midiEngine.routine_task_id);
}

void Debug::loadPacketReceived(uint8_t* data, int32_t len) {
	uint32_t handshake = runtimeFeatureSettings.get(RuntimeFeatureSettingType::DevSysexAllowed);
	if (handshake == 0) {
		return; // not allowed
	}

	const int size = 512;
	const int packed_size = 586; // ceil(512+512/7)
	if (len < packed_size + 10) {
		return;
	}

	uint32_t handshake_received;
	unpack_7bit_to_8bit((uint8_t*)&handshake_received, 4, data + 2, 5);
	if (handshake != handshake_received) {
		return;
	}

	int pos = 512 * (data[7] + 0x80 * data[8]);

	if (pos == 0) {
		firstPacket(data, len);
	}

	if (load_buf == nullptr || pos + 512 > load_bufsize) {
		return;
	}

	unpack_7bit_to_8bit(load_buf + pos, size, data + 9, packed_size);

	// Pad LED Progress Bar Step
	uint32_t pad = (18 * 8 * pos) / (load_bufsize - 0xffff);
	uint8_t col = pad % 18;
	uint8_t row = pad / 18;
	PadLEDs::image[row][col] = RGB((255 / 7) * row, 0, 255 - (255 / 7) * row);
	if ((pos / 512) % 16 == 0) {
		PadLEDs::sendOutMainPadColours();
		PadLEDs::sendOutSidebarColours();
	}
}

void Debug::loadCheckAndRun(uint8_t* data, int32_t len) {
	uint32_t handshake = runtimeFeatureSettings.get(RuntimeFeatureSettingType::DevSysexAllowed);
	if (handshake == 0) {
		return; // not allowed
	}

	if (len < 17 || load_buf == nullptr) {
		return; // cannot do that
	}

	uint32_t fields[3];

	unpack_7bit_to_8bit((uint8_t*)fields, sizeof(fields), data + 2, 14);

	if (handshake != fields[0]) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_BAD_KEY));
		return;
	}

	uint32_t code_file_size = fields[1];

	uint32_t checksum = get_crc(load_buf, code_file_size);

	if (checksum != fields[2]) {
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_CHECKSUM_FAIL));
		return;
	}

	chainload_from_buf(load_buf, load_codesize);
}
#endif
