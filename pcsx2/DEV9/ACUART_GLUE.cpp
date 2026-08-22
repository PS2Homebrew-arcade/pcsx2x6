#include "ACUART_GLUE.h"
#include "ACUART.h"
#include "ACCORE.h"
#include <deque>
#include "common/Console.h"
#include "Config.h"
#include <vector>
#include <fmt/format.h>
#include "Config.h"
#include "CARD_READER.h"

#include "ACMACROS.h"

#define ACUART_LOG(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.WriteLn(Color_Gray, "ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)
#define ACUART_WARND(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.Warning("ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)
#define ACUART_WARN(fmt, ...) Console.Warning("ACUART:" fmt __VA_OPT__(,) __VA_ARGS__)

#define CARDIF_LOG(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.WriteLn(Color_Gray, "CARDIF:" fmt __VA_OPT__(,) __VA_ARGS__)
#define CARDIF_WARND(fmt, ...) if (EmuConfig.Arcade.UARTVerbose) Console.Warning("CARDIF:" fmt __VA_OPT__(,) __VA_ARGS__)
#define CARDIF_WARN(fmt, ...) Console.Warning("CARDIF" fmt __VA_OPT__(,) __VA_ARGS__)

class Bg3HandleDevice : public ACUARTDevice
{
private:
    std::deque<u8> fifo;

    int txCnt = 0;
    u8 prevTx = 0;
    int handleCycles = 0;
    bool handleDone = false;

public:
// clear BG3 acuart HANDLE-handshake state on game boot (fixes HANDLE-ERROR-on-reset)
    void Reset() override
    {
        txCnt = 0;
        prevTx = 0;
        handleCycles = 0;
        handleDone = false;
        fifo.clear();
    }
    
// Battle Gear 3 / Tuned: answer the steering board's boot handshake (BGRLOAD FUN_00133e60) so the game
// doesn't stall on "HANDLE ERROR". With no emulated FFB board we reply ready and keep the drive-board flag
// (EE 0x2694b0) CLEAR, so steering stays on the JVS analog wheel. FFB GROUNDWORK: set s_bg3FfbEnabled=true
// when a real drive board is emulated to run the full calibration handshake.
// (Thanks to Hydreigon223 for the dump.)
    void TxByte(u8 value) override {
	    txCnt++;
	    if ((txCnt & 1) != 0) { prevTx = value; return; } // a {reg,val} command is 2 bytes; reply on the 2nd

	    if (prevTx == 0x20 && value == 0x00) // {0x20,0} starts an init -> re-arm so a re-init replies fresh
	    {
	    	handleDone = false;
	    	handleCycles = 0;
	    }
	    u8 hi = 0x00;
	    if (!handleDone)
	    {
	    	static constexpr bool s_bg3FfbEnabled = false;    // true once a real FFB drive board is emulated
	    	if (prevTx == 0x20 || prevTx == 0x1f)
	    		hi = s_bg3FfbEnabled ? 0x80 : 0x01;           // 0x01 = ready (skip calibrate while FFB off)
	    	else if (prevTx == 0x14 && value == 0x1a)
	    		hi = (++handleCycles > 4) ? 0x00 : 0x80; // calibrate busy-loop (FFB on only)
	    	if (prevTx == 0x11 && value == 0x03)
	    		handleDone = true;                       // last init command
	    }
	    fifo.push_back(hi); // byte0 = busy/ready status
	    fifo.push_back(0);  // byte1
	    ACUART_LOG("INTR");
	    ACCORE::intr(ACCORE::INTRN_UART);
	    prevTx = value;
    }

    bool RxByte(u8& value) {
		if (!fifo.empty())
		{
			value = fifo.front();
			fifo.pop_front();
            return true;
		}
        return false;
    }

    bool HasData() const override
    {
        return !fifo.empty();
    }
};


class RRVHandleDevice : public ACUARTDevice
{
private:
    std::deque<u8> fifo;

    int txCnt = 0;
    u8 prevTx = 0;
    int handleCycles = 0;
    bool handleDone = false;
    u32 s_v257Accum = 0;
    static constexpr u8 V257_STATUS[3] = {'C', '0', '1'}; // drive-board OK status; accepted by every RRV build

public:
    void Reset() override
    {
        txCnt = 0;
        prevTx = 0;
        handleCycles = 0;
        handleDone = false;
        fifo.clear();
    }
    //void TxByte(u8 value) override {}

    bool RxByte(u8& value) {
		if (!fifo.empty()) // RRV reading the serial port: hand it the next status byte we queued ("E00")
		{
			value = fifo.front();
			fifo.pop_front();
            return true;
		}
        return false;
    }

    bool HasData() const override
    {
        return !fifo.empty();
    }

// Ridge Racer V drive-board status streamer (called each DEV9 tick): refill the receive buffer with the
// board's OK status and raise the RX interrupt. Only RRV needs this.
    void Tick(u32 cycles) {
	    if (!(ACUART::IER & 0x01)) // host hasn't enabled the RX-data interrupt yet
	    	return;
	    s_v257Accum += cycles;
	    if (s_v257Accum < 240) // throttle (DEV9async ticks ~tens of kHz with cycles=1) -> a few hundred Hz
	    	return;
	    s_v257Accum = 0;
	    // Keep raising the RX IRQ every tick; reload the status only once the ISR has drained the previous copy.
	    if (fifo.empty())
	    	fifo.assign(V257_STATUS, V257_STATUS + 3);
	    ACUART_LOG("INTR");
	    ACCORE::intr(ACCORE::INTRN_UART); // raise the UART RX interrupt
    }
};


/**
 * ## Super Dragon Ball Z (NM00027) — self-contained IC card reader responder.
 * Initial implementation created [byKeropon](https://github.com/Keropon)
 * The game's IOP "cardif" thread (RE'd from a PCSX2x6 savestate) frames each
 * command to the card R/W as:  
 *     `[0x00][opcode][0x00][datalen]  <datalen payload bytes>  [BCC]`
 * - BCC = XOR of every preceding byte, i.e. XOR over the whole frame == 0.  
 * ### opcodes: 
 *  - 20 REQUEST
 *  - 21 ANTICOLL
 *  - 22 SELECT
 *  - 24 READ
 *  - 25 WRITE
 *  - 26 DECREMENT
 *  - 27 HALT
 *  - 30 CARD_STATUS
 *  - 31 BACKUP
 *  - 33 READ_VALUE
 *  - 34 READ_PAGES
 *  - 35 WRITE_PAGES.
 * A response must be >= 7 bytes, XOR over the whole frame == 0, and must NOT
 * start with 0xFF 0x01 (the reader's ERROR marker; header is read big-endian).
 * This scaffold decodes + logs every command and replies with a valid-BCC
 * frame so the cardif stops timing out and the command sequence can be
 * observed. The per-command response *payloads* (card UID, block data, the
 * "title card" image with magic 0x4E4F4646 / version 0x0100) are filled from a
 * capture: build with Arcade UART Verbose on, run SDBZ, and read the
 * "ACUART SDBZ CMD ..." log lines.
 */
class SDBZHandleDevice : public ACUARTDevice
{
private:
    bool cardInserted[2]{false, false};
    std::deque<u8> s_sdbzRx;  // bytes the game will read back from the reader
    std::vector<u8> s_sdbzTx; // command frame being assembled from TX bytes
    u32 s_sdbzLogCount = 0;
    static constexpr u8 SDBZ_CARD_ID[10] = {0x02,0x00,0x75,0x30,0x98,0x44,0x01,0x81,0x60,0x06};

    enum CARDIF_CMDS {
        RW_POLLING = 0x10,
        CHANGE_SETTING = 0x11,
        RW_AUTH1 = 0x12,
        RW_AUTH2 = 0x13,
        RF_ON = 0x14,
        RF_OFF = 0x15,
        REQUEST = 0x20, 
        ANTICOLL = 0x21, 
        SELECT = 0x22, 
        READ = 0x24, 
        WRITE = 0x25, 
        DECREMENT = 0x26, 
        HALT = 0x27, 
        CARD_STATUS = 0x30, 
        BACKUP = 0x31, 
        READ_VALUE = 0x33, 
        READ_PAGES = 0x34, 
        WRITE_PAGES = 0x35, 
    };
    int GetCommandPacketLen(unsigned int C) {
        switch (C)
        {
        case CARDIF_CMDS::RW_POLLING:
            return 0;
        case CARDIF_CMDS::SELECT:
        case CARDIF_CMDS::READ:
        case CARDIF_CMDS::WRITE:
        case CARDIF_CMDS::HALT:
        case CARDIF_CMDS::READ_PAGES:
            return 0x0f;
        case CARDIF_CMDS::RW_AUTH2:
        case CARDIF_CMDS::BACKUP:
            return 0x17;
        case CARDIF_CMDS::CHANGE_SETTING:
        case CARDIF_CMDS::RW_AUTH1:
        case CARDIF_CMDS::RF_ON:
        case CARDIF_CMDS::RF_OFF:
        case CARDIF_CMDS::REQUEST:
        case CARDIF_CMDS::ANTICOLL:
        case CARDIF_CMDS::DECREMENT:
        case CARDIF_CMDS::CARD_STATUS:
        case CARDIF_CMDS::READ_VALUE:
        case CARDIF_CMDS::WRITE_PAGES:
            return 0x07;
        default:
            Console.Error("%s REQUESTED UNHANDLED COMMAND %02X", __FUNCTION__, C);
        }
    }
    u8 SdbzBcc(const u8* p, size_t n) {
        u8 x = 0;
        for (size_t i = 0; i < n; i++)
            x ^= p[i];
        return x;
    }

    static bool SdbzValidOp(u8 op) {
        switch (op) {
        case RW_POLLING:
        case CHANGE_SETTING:
        case RW_AUTH1:
        case RW_AUTH2:
        case RF_ON:
        case RF_OFF:
        case REQUEST:
        case ANTICOLL: 
        case SELECT:
        case READ:
        case WRITE:
        case DECREMENT: 
        case HALT:
        case CARD_STATUS:
        case BACKUP:
        case READ_VALUE:
        case READ_PAGES:
        case WRITE_PAGES:
            return true;
        default:
            Console.Error("CARDIF:NM00027: UNKNOWN COMMAND %02X", op);
            return false;
        }
    }

    
    void SdbzQueueResponse(std::vector<u8> r) {
        // ACUART_LOG("%s(%p, %X)", __FUNCTION__, r.data(), r.size());
    	r.push_back(SdbzBcc(r.data(), r.size())); // BCC so XOR over the whole frame == 0
    	if (EmuConfig.Arcade.UARTVerbose && s_sdbzLogCount < 512) {
    		std::string hex;
            for (u8 b : r)
                hex += fmt::format("{:02X} ", b);
    		Console.WriteLn(Color_Yellow, "CARDIF SDBZ RSP (%u): %s", (unsigned)r.size(), hex.c_str());
    	}
    	for (u8 b : r)
            s_sdbzRx.push_back(b);
    	ACCORE::intr(ACCORE::INTRN_UART);         // wake the cardif RX wait
    }

public:
	// Keep raising the RX interrupt while the SDBZ reader has queued response
	// bytes, so the cardif thread's acUartRead wait wakes up. Responses are
	// produced synchronously in SdbzOnTxByte; this just re-pokes the IRQ.
    void Tick(u32 cycles) {
	    if (!(ACUART::IER & 0x01)) // host hasn't enabled the RX-data interrupt yet
	    	return;
	    if (!s_sdbzRx.empty())
	    	ACCORE::intr(ACCORE::INTRN_UART);
    }
    void Reset() override {
        s_sdbzRx.clear();
        s_sdbzTx.clear();
        s_sdbzLogCount = 0;
    }


    void TxByte(u8 v) override {
    	s_sdbzTx.push_back(v);
    	// Resync: a command frame is [00][opcode][00][datalen]... Drop leading bytes
    	// until the buffer starts on a plausible header, so a stray/short frame from
    	// the anticollision cascade can't shift every frame after it by a byte.
    	while (s_sdbzTx.size() >= 3 &&
    	       !(s_sdbzTx[0] == 0x00 && s_sdbzTx[2] == 0x00 && SdbzValidOp(s_sdbzTx[1])))
    		s_sdbzTx.erase(s_sdbzTx.begin());
    	if (s_sdbzTx.size() < 4) return;                    // need the 4-byte header first
    	const size_t datalen = s_sdbzTx[3];
    	const size_t frameLen = 4 + datalen + 1;            // header + payload + BCC
    	if (s_sdbzTx.size() < frameLen) return;             // frame incomplete

    	const u8 opcode = s_sdbzTx[1];
    	const bool bccOk = SdbzBcc(s_sdbzTx.data(), frameLen) == 0;
    	if (!bccOk) {
    		s_sdbzTx.erase(s_sdbzTx.begin());               // bad BCC -> resync one byte
    		return;
    	}
    	if (EmuConfig.Arcade.UARTVerbose && s_sdbzLogCount < 512) {
    		std::string hex;
            for (size_t i = 0; i < frameLen; i++)
                hex += fmt::format("{:02X} ", s_sdbzTx[i]);
    		Console.WriteLn(Color_Green, "CARDIF SDBZ cmd=%02X prt=%02X len=%u bcc=%s : %s",
    			opcode, CARDIF::PORT, (unsigned)datalen, bccOk ? "OK" : "BAD", hex.c_str());
    		s_sdbzLogCount++;
    	}

    	// Build a response of the exact expected length (SdbzRespLen). Frame shape
    	// (hypothesis from the captured commands): [0x00][opcode][data...][BCC],
    	// byte0 != 0xFF (0xFF01 = reader error). BCC appended by SdbzQueueResponse.
    	// Content is a best-effort MIFARE card; refine from the CMD/RSP capture.
    	// Reader response format (per MetalliC, Demul): [0x10][cmd][lenH][lenL][data..][xorsum]
    	//   to reader:   00 cmd lenH lenL data xorsum
    	//   from reader: 10 cmd lenH lenL data xorsum   (xorsum -> whole frame XOR == 0)
    	// cmds: 20 check card present, 21 get card id, 22 select, 24 read 1 page(8B),
    	//       25 write page, 26 dec counter, 27 end session, 30 get skey+id,
    	//       34 read several pages, 35 write several pages.
    	std::vector<u8> data;
    	switch (opcode) {
    		case CARDIF_CMDS::REQUEST: 
                data = {0, 0x4}; break; // present -> ATQA 0x0004
    		case CARDIF_CMDS::ANTICOLL: 
                data.assign(SDBZ_CARD_ID, SDBZ_CARD_ID + 10); break;  // card ID
    		case CARDIF_CMDS::CARD_STATUS:
                data.assign(SDBZ_CARD_ID, SDBZ_CARD_ID + 10); break;  // skey + ID
    		case CARDIF_CMDS::READ: 
                data.assign(8, 0x00); break; // one page (8 bytes)
    		case CARDIF_CMDS::READ_PAGES: 
                data.assign(64, 0x00); break; // several pages
    		default:   break;                                                // 22/25/26/27/..: empty = OK
    	}
        size_t to_send = 4 + data.size() + 1; // HEADER + PAYLOAD + BCC
        size_t to_send_min = GetCommandPacketLen((int)opcode);
        to_send = (to_send < to_send_min) ? to_send_min : to_send;
    	std::vector<u8> r;
    	r.reserve(to_send);
    	r.push_back(0x10);                          // reader marker
    	r.push_back(opcode);                        // echo cmd
    	r.push_back((u8)(data.size() >> 8));        // lenH
    	r.push_back((u8)(data.size() & 0xff));      // lenL
    	for (u8 b : data) r.push_back(b);
        while (r.size() < (to_send-1))
            r.push_back(0);
    	SdbzQueueResponse(std::move(r)); // appends xorsum
    	s_sdbzTx.clear();
    }

    bool RxByte(u8& value) {
		if (!s_sdbzRx.empty()) {
			value = s_sdbzRx.front();
			s_sdbzRx.pop_front();
            return true;
		}
        return false;
    }

    bool HasData() const override {
        return !s_sdbzRx.empty();
    }

    void DoCardInput(u32 slot) override {
        
    }

    
};


bool ACUART::SetupGameHandler(const std::string& S) {
    if (S == "NM00001")
        s_device = std::make_unique<RRVHandleDevice>();
    else if (S == "NM00010" || S == "NM00015")
        s_device = std::make_unique<Bg3HandleDevice>();
    else if (S == "NM00022" || S == "NM00021")
        s_device = std::make_unique<ACUARTCardReader>();
    else if (S == "NM00027")
        s_device = std::make_unique<SDBZHandleDevice>();
    else
        return false;
    s_device->Reset();
    return true;
}

void CARDIF::InsertCard(u32 slot) {
    if (ACUART::s_device) ACUART::s_device->DoCardInput(slot);
    static unsigned int bit = 0;
    if (++bit > 7) bit = 0;
    UNK0 ^= (1 << bit);
    Console.WriteLnFmt("UNK0:{:02X} (bit {} changed)", UNK0, bit);
}