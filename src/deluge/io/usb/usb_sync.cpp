/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 */

#include "usb_sync.h"
#include "fatfs/fatfs.hpp"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/output.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "tusb.h"
#include <cstring>
#include <optional>

namespace deluge::io::usb {

enum class RxState { WaitMagicH, WaitMagicL, WaitHeader, WaitPayload, WaitChecksum };

enum class TransferState { Idle, SendingFile };

// RX Parser State variables
static RxState rxState = RxState::WaitMagicH;
static uint8_t rxBuffer[1024 + 10];
static uint16_t rxIndex = 0;

// TX File Transfer state
static TransferState transferState = TransferState::Idle;
static std::optional<FatFS::File> activeReadFile;
static uint16_t activeChunkIndex = 0;

void initUsbSync() {
	rxState = RxState::WaitMagicH;
	transferState = TransferState::Idle;
	activeReadFile.reset();
}

static void sendResponsePacket(uint8_t cmd, const uint8_t* payload, uint16_t len) {
	uint8_t msg[1024 + 6];
	msg[0] = 0xDE;
	msg[1] = 0x4C;
	msg[2] = cmd;
	msg[3] = len & 0xFF;
	msg[4] = (len >> 8) & 0xFF;

	if (len > 0 && payload != nullptr) {
		std::memcpy(&msg[5], payload, len);
	}

	uint8_t checksum = cmd ^ (len & 0xFF) ^ ((len >> 8) & 0xFF);
	for (uint16_t i = 0; i < len; i++) {
		checksum ^= payload[i];
	}
	msg[5 + len] = checksum;

	tud_cdc_write(msg, len + 6);
	tud_cdc_write_flush();
}

static void sendDirectoryListing(const char* path) {
	auto dirResult = FatFS::Directory::open(path);
	if (!dirResult) {
		uint8_t errorMsg[1] = {0x01}; // status: 1 (error)
		sendResponsePacket(0x03, errorMsg, 1);
		return;
	}

	uint8_t txBuf[1024];
	txBuf[0] = 0; // status: 0 (success)
	uint16_t txIndex = 1;

	while (true) {
		auto fileInfoResult = dirResult->read();
		if (!fileInfoResult || fileInfoResult->fname[0] == '\0') {
			break;
		}

		bool isDir = (fileInfoResult->fattrib & AM_DIR) != 0;
		uint8_t typeChar = isDir ? 'd' : 'f';
		int nameLen = std::strlen(fileInfoResult->fname);

		// Format: 'd' or 'f' followed by name, null-terminated
		if (txIndex + nameLen + 2 < 1024) {
			txBuf[txIndex++] = typeChar;
			std::memcpy(&txBuf[txIndex], fileInfoResult->fname, nameLen);
			txIndex += nameLen;
			txBuf[txIndex++] = '\0';
		}
		else {
			break;
		}
	}

	sendResponsePacket(0x03, txBuf, txIndex);
}

static void startFileRead(const char* filePath) {
	if (activeReadFile) {
		activeReadFile->close();
		activeReadFile.reset();
	}

	auto fileResult = FatFS::File::open(filePath, FA_READ);
	if (!fileResult) {
		uint8_t errPayload[5] = {2, 0, 0, 0, 0}; // status: 2 (error)
		sendResponsePacket(0x05, errPayload, 5);
		transferState = TransferState::Idle;
		return;
	}

	activeReadFile = std::move(*fileResult);
	activeChunkIndex = 0;
	transferState = TransferState::SendingFile;
}

static void handleReceivedPacket(uint8_t cmd, uint8_t* payload, uint16_t len) {
	if (cmd == 0x02) { // Request File List
		char path[256];
		if (len < sizeof(path)) {
			std::memcpy(path, payload, len);
			path[len] = '\0';
			sendDirectoryListing(path);
		}
	}
	else if (cmd == 0x04) { // Request File Read
		char filePath[256];
		if (len < sizeof(filePath)) {
			std::memcpy(filePath, payload, len);
			filePath[len] = '\0';
			startFileRead(filePath);
		}
	}
	else if (cmd == 0x09) { // Write Parameter
		if (len == 7) {
			uint8_t paramKindByte = payload[0];
			uint16_t paramID = payload[1] | (payload[2] << 8);
			int32_t newValue = payload[3] | (payload[4] << 8) | (payload[5] << 16) | (payload[6] << 24);

			deluge::modulation::params::Kind kind = deluge::modulation::params::Kind::NONE;
			if (paramKindByte == 0)
				kind = deluge::modulation::params::Kind::PATCHED;
			else if (paramKindByte == 1)
				kind = deluge::modulation::params::Kind::UNPATCHED_SOUND;
			else if (paramKindByte == 2)
				kind = deluge::modulation::params::Kind::UNPATCHED_GLOBAL;

			InstrumentClip* clip = getCurrentInstrumentClip();
			if (clip && clip->output) {
				ModelStackWithTimelineCounter modelStack;
				modelStack.song = currentSong;
				modelStack.setTimelineCounter(clip);

				ModelStackWithAutoParam* modelStackWithParam =
				    clip->output->getModelStackWithParam(&modelStack, clip, paramID, kind, true, false);

				if (modelStackWithParam && modelStackWithParam->autoParam) {
					modelStackWithParam->autoParam->setValuePossiblyForRegion(newValue, modelStackWithParam, 0, 0);
				}
			}
		}
	}
	else if (cmd == 0x0A) { // Read Parameter
		if (len == 3) {
			uint8_t paramKindByte = payload[0];
			uint16_t paramID = payload[1] | (payload[2] << 8);

			deluge::modulation::params::Kind kind = deluge::modulation::params::Kind::NONE;
			if (paramKindByte == 0)
				kind = deluge::modulation::params::Kind::PATCHED;
			else if (paramKindByte == 1)
				kind = deluge::modulation::params::Kind::UNPATCHED_SOUND;
			else if (paramKindByte == 2)
				kind = deluge::modulation::params::Kind::UNPATCHED_GLOBAL;

			InstrumentClip* clip = getCurrentInstrumentClip();
			if (clip && clip->output) {
				ModelStackWithTimelineCounter modelStack;
				modelStack.song = currentSong;
				modelStack.setTimelineCounter(clip);

				ModelStackWithAutoParam* modelStackWithParam =
				    clip->output->getModelStackWithParam(&modelStack, clip, paramID, kind, true, false);

				if (modelStackWithParam && modelStackWithParam->autoParam) {
					int32_t val = modelStackWithParam->autoParam->getCurrentValue();
					uint8_t resp[7];
					resp[0] = paramKindByte;
					resp[1] = payload[1];
					resp[2] = payload[2];
					resp[3] = val & 0xFF;
					resp[4] = (val >> 8) & 0xFF;
					resp[5] = (val >> 16) & 0xFF;
					resp[6] = (val >> 24) & 0xFF;
					sendResponsePacket(0x0B, resp, 7);
				}
			}
		}
	}
}

static void processIncomingCdcData() {
	uint32_t avail = tud_cdc_available();
	if (avail == 0) {
		return;
	}

	for (uint32_t i = 0; i < avail; i++) {
		uint8_t b;
		tud_cdc_read(&b, 1);

		switch (rxState) {
		case RxState::WaitMagicH:
			if (b == 0xDE) {
				rxBuffer[0] = b;
				rxIndex = 1;
				rxState = RxState::WaitMagicL;
			}
			break;

		case RxState::WaitMagicL:
			if (b == 0x4C) {
				rxBuffer[1] = b;
				rxIndex = 2;
				rxState = RxState::WaitHeader;
			}
			else {
				rxState = RxState::WaitMagicH;
			}
			break;

		case RxState::WaitHeader:
			rxBuffer[rxIndex++] = b;
			if (rxIndex == 5) {
				uint16_t len = rxBuffer[3] | (rxBuffer[4] << 8);
				if (len > 1024) {
					rxState = RxState::WaitMagicH;
				}
				else if (len > 0) {
					rxState = RxState::WaitPayload;
				}
				else {
					rxState = RxState::WaitChecksum;
				}
			}
			break;

		case RxState::WaitPayload:
			rxBuffer[rxIndex++] = b;
			{
				uint16_t len = rxBuffer[3] | (rxBuffer[4] << 8);
				if (rxIndex == (5 + len)) {
					rxState = RxState::WaitChecksum;
				}
			}
			break;

		case RxState::WaitChecksum: {
			uint8_t checksum = b;
			uint8_t cmd = rxBuffer[2];
			uint16_t len = rxBuffer[3] | (rxBuffer[4] << 8);

			uint8_t calculatedChecksum = cmd ^ (len & 0xFF) ^ ((len >> 8) & 0xFF);
			for (uint16_t idx = 0; idx < len; idx++) {
				calculatedChecksum ^= rxBuffer[5 + idx];
			}

			if (checksum == calculatedChecksum) {
				handleReceivedPacket(cmd, &rxBuffer[5], len);
			}
			rxState = RxState::WaitMagicH;
		} break;
		}
	}
}

static void processOutgoingFileTransfer() {
	if (transferState != TransferState::SendingFile || !activeReadFile) {
		return;
	}

	uint8_t dataBuf[512];
	std::span<std::byte> readSpan(reinterpret_cast<std::byte*>(dataBuf), sizeof(dataBuf));
	auto readResult = activeReadFile->read(readSpan);

	if (!readResult) {
		uint8_t errPayload[5] = {2, (uint8_t)(activeChunkIndex & 0xFF), (uint8_t)((activeChunkIndex >> 8) & 0xFF), 0,
		                         0};
		sendResponsePacket(0x05, errPayload, 5);
		activeReadFile->close();
		activeReadFile.reset();
		transferState = TransferState::Idle;
		return;
	}

	uint16_t bytesRead = readResult->size();
	bool isEof = activeReadFile->eof() || bytesRead < 512;
	uint8_t status = isEof ? 1 : 0;

	uint8_t txPayload[512 + 5];
	txPayload[0] = status;
	txPayload[1] = activeChunkIndex & 0xFF;
	txPayload[2] = (activeChunkIndex >> 8) & 0xFF;
	txPayload[3] = bytesRead & 0xFF;
	txPayload[4] = (bytesRead >> 8) & 0xFF;

	if (bytesRead > 0) {
		std::memcpy(&txPayload[5], dataBuf, bytesRead);
	}

	sendResponsePacket(0x05, txPayload, bytesRead + 5);

	if (isEof) {
		activeReadFile->close();
		activeReadFile.reset();
		transferState = TransferState::Idle;
	}
	else {
		activeChunkIndex++;
	}
}

static void sendPlayheadSync() {
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
		msg[0] = 0xDE;
		msg[1] = 0x4C;
		msg[2] = 0x01;
		msg[3] = 0x09;
		msg[4] = 0x00;

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

void usbSyncTask() {
	if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::UsbSerialSync)) {
		return;
	}
	if (!tud_cdc_connected()) {
		if (activeReadFile) {
			activeReadFile->close();
			activeReadFile.reset();
			transferState = TransferState::Idle;
		}
		return;
	}

	processIncomingCdcData();
	processOutgoingFileTransfer();
	sendPlayheadSync();
}

} // namespace deluge::io::usb
