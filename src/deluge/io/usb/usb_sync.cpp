/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 */

#include "usb_sync.h"
#include "playback/playback_handler.h"
#include "tusb.h"

namespace deluge::io::usb {

void initUsbSync() {
	// Initialization logic (if any)
}

void usbSyncTask() {
	if (!tud_cdc_connected()) {
		return;
	}

	uint32_t tick = (uint32_t)playbackHandler.getActualSwungTickCount();
	uint32_t bpm = (uint32_t)(playbackHandler.calculateBPMForDisplay() * 1000.0f);
	uint8_t playState =
	    playbackHandler.isEitherClockActive() ? (playbackHandler.recording != RecordingMode::OFF ? 2 : 1) : 0;

	static uint32_t lastTick = 0xFFFFFFFF;
	static uint32_t lastBpm = 0;
	static uint8_t lastPlayState = 0xFF;

	if (tick != lastTick || bpm != lastBpm || playState != lastPlayState) {
		lastTick = tick;
		lastBpm = bpm;
		lastPlayState = playState;

		uint8_t msg[15];
		msg[0] = 0xDE; // Magic H
		msg[1] = 0x4C; // Magic L
		msg[2] = 0x01; // Cmd: Playhead Sync
		msg[3] = 0x09; // Length L (9 bytes payload)
		msg[4] = 0x00; // Length H

		msg[5] = tick & 0xFF;
		msg[6] = (tick >> 8) & 0xFF;
		msg[7] = (tick >> 16) & 0xFF;
		msg[8] = (tick >> 24) & 0xFF;

		msg[9] = bpm & 0xFF;
		msg[10] = (bpm >> 8) & 0xFF;
		msg[11] = (bpm >> 16) & 0xFF;
		msg[12] = (bpm >> 24) & 0xFF;

		msg[13] = playState;

		uint8_t checksum = 0x01 ^ 0x09 ^ 0x00;
		for (int i = 5; i < 14; i++) {
			checksum ^= msg[i];
		}
		msg[14] = checksum;

		tud_cdc_write(msg, 15);
		tud_cdc_write_flush();
	}
}

} // namespace deluge::io::usb
