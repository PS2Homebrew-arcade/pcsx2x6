/*
    YACardEmu -> PCSX2x6 ACUART device adapter
    ----------------
    Copyright (C) 2020-2025 wutno (https://github.com/GXTX)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#ifndef ACUARTCARDREADER_H
#define ACUARTCARDREADER_H

#include "ACUART.h"
#include "ACCORE.h"
#include "common/Pcsx2Types.h"
#include "common/Console.h"

#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

/// TODO:PRINTER
// #include "Printer.h"


/**
 * ACUARTDevice adapter for card readers that do not use NAMCO EXCARD PCB (like IDOLMASTER and COBRA)
 */
class ACUARTCardReader : public ACUARTDevice
{
public:
	enum StatusCode {
		Okay,
		SendAck,
		SizeError,
		SyncError,
		SyntaxError,
		ChecksumError,
		EmptyResponseError,
		ServerWaitingReply,
		DontReply,
	};

	struct Settings {
		std::string cardName{};
		std::string cardPath{};
		bool insertedCard = false;
		bool hasCard = false;
		bool waitingForCard = false;
		bool reportDispenserEmpty = false;
		std::string mech = "C1231LR";
		std::mutex lock;
	};

	StatusCode m_status = StatusCode::Okay;

	ACUARTCardReader();
	~ACUARTCardReader() override = default;

	// New ACUARTDevice interface.
	void Reset() override;
	void Init() override;
	void TxByte(u8 value) override;
	bool RxByte(u8& value) override;
	void DoCardInput(u32) override;
	void Tick(u32 cycles) override {
		IRQAccum += cycles;
		if (IRQAccum < 240)
			return;
		IRQAccum = 0;
		if (HasData())
			ACCORE::intr(ACCORE::INTRN_UART);
	}
	bool HasData() const override;

	// Kept intact from YACardEmu for compatibility/debugging.
	StatusCode BuildPacket(std::vector<uint8_t>& readBuffer);
	StatusCode ReceivePacket(std::vector<uint8_t>& writeBuffer);
	StatusCode Process(std::vector<uint8_t>& read, std::vector<uint8_t>& write);

	Settings m_cardSettings{};
	std::string printName = "print.bin";

protected:
	enum class R {
		NO_CARD,
		EJECT,
		READ_WRITE_HEAD,
		THERMAL_HEAD,
		DISPENSER_THERMAL,
		MAX_POSITIONS,
	};
	enum class P {
		NO_ERR                 = 0x30,
		READ_ERR               = 0x31,
		WRITE_ERR              = 0x32,
		CARD_JAM               = 0x33,
		MOTOR_ERR              = 0x34,
		PRINT_ERR              = 0x35,
		ILLEGAL_ERR            = 0x38,
		BATTERY_ERR            = 0x40,
		SYSTEM_ERR             = 0x41,
		TRACK_1_READ_ERR       = 0x51,
		TRACK_2_READ_ERR       = 0x52,
		TRACK_3_READ_ERR       = 0x53,
		TRACK_1_AND_2_READ_ERR = 0x54,
		TRACK_1_AND_3_READ_ERR = 0x55,
		TRACK_2_AND_3_READ_ERR = 0x56,
	};
	enum class S {
		NO_JOB           = 0x30,
		ILLEGAL_COMMAND  = 0x32,
		RUNNING_COMMAND  = 0x33,
		WAITING_FOR_CARD = 0x34,
		DISPENSER_EMPTY  = 0x35,
		NO_DISPENSER     = 0x36,
		CARD_FULL        = 0x37,
	};

	struct Status {
		R r = R::NO_CARD;
		P p = P::NO_ERR;
		S s = S::NO_JOB;

		void Reset() {
			p = P::NO_ERR;
			s = S::NO_JOB;
		}

		void SoftReset() {
			p = P::NO_ERR;
			s = S::NO_JOB;
		}
	};
	Status status = {};
	u32 IRQAccum = 0;

	static constexpr uint8_t START_OF_TEXT = 0x02;
	static constexpr uint8_t END_OF_TEXT = 0x03;
	static constexpr uint8_t ENQUIRY = 0x05;
	static constexpr uint8_t ACK = 0x06;
	static constexpr uint8_t NACK = 0x15;
	static constexpr uint8_t CARD_SIZE = 0xCF;
	static constexpr uint8_t TRACK_SIZE = 0x45;
	static constexpr uint8_t NUM_TRACKS = 3;
	const std::string versionString = "AP:S1234-5678,OS:S9012-3456,0000";

	uint8_t GetByte(uint8_t** buffer);
	void HandlePacket();

	std::vector<std::vector<uint8_t>> cardData{{}, {}, {}};

	void UpdateStatusInBuffer();
	void SetPError(P error_code);
	void SetSError(S error_code);

	void ClearCardData();
	void ReadCard();
	void WriteCard();

	virtual void Command_10_Initalize();
	void Command_20_ReadStatus();
	void Command_33_ReadData2();
	void Command_35_GetData();
	void Command_40_Cancel();
	void Command_53_WriteData2();
	void Command_78_PrintSettings2();
	void Command_7A_RegisterFont();
	void Command_7B_PrintImage();
	void Command_7C_PrintL();
	void Command_7D_Erase();
	void Command_7E_PrintBarcode();
	void Command_80_EjectCard();
	void Command_A0_Clean();
	void Command_B0_DispenseCardS31();
	void Command_C0_ControlLED();
	void Command_C1_SetPrintRetry();
	virtual void Command_D0_ShutterControl();
	void Command_E1_SetRTC();
	void Command_F0_GetVersion();
	void Command_F1_GetRTC();
	void Command_F5_CheckBattery();

	int currentStep{};
	uint8_t currentCommand{};
	bool runningCommand{false};

	std::vector<uint8_t> currentPacket{};
	std::vector<uint8_t> commandBuffer{0, 0, 0, 0};
	std::vector<uint8_t> printBuffer{};

	std::time_t startTime{};
	std::time_t setTime{};

    /// TODO:PRINTER
	// std::unique_ptr<Printer> m_printer = std::make_unique<Printer>();

	// UART transport buffers. YACardEmu's packet parser remains unchanged;
	// these buffers only adapt it to the byte-oriented ACUARTDevice API.
	std::vector<uint8_t> m_uartRxBuffer;
	std::deque<uint8_t> m_uartTxBuffer;

	uint8_t GetPositionValue()
	{
	    static constexpr std::array<uint8_t, static_cast<size_t>(R::MAX_POSITIONS)> positionValues{{
	    	0x30, 0x34, 0x31, 0x32, 0x33
	    }};
		return positionValues[static_cast<uint8_t>(status.r)];
	}

	void ProcessNewPosition() {
	    if (status.r == R::EJECT) {
			Console.WriteLnFmt("{}: status eject", __FUNCTION__);
	    	m_cardSettings.insertedCard = false;
	    	m_cardSettings.hasCard = false;
	    	MoveCard(R::NO_CARD);
	    }

	    if (m_cardSettings.insertedCard && 
			status.r == R::NO_CARD) {
			Console.WriteLnFmt("{}: (insertedCard && status.r == R::NO_CARD)", __FUNCTION__);
	    	ReadCard();
	    	MoveCard(R::READ_WRITE_HEAD);

	    	if (runningCommand && status.s == S::WAITING_FOR_CARD)
	    		status.s = S::RUNNING_COMMAND;
	    }
    }

	virtual bool HasCard() {
		return status.r != R::NO_CARD;
	}
	virtual void MoveCard(R to) {
		status.r = to;
		std::lock_guard<std::mutex> card_lock(m_cardSettings.lock);
		m_cardSettings.hasCard = (to != R::NO_CARD);
	}
	virtual void EjectCard();
	virtual void DispenseCard();
};

#endif
