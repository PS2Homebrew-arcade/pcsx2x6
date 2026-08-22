#pragma once
#include "ACUART_GLUE.h"
#include "MemoryTypes.h"
#include <memory>
#include <map>

#define ACUART_BASE 0x12418000 // everything, both reg set and I/O is done in that little 0xFFF range
#define IS_ACUART_RANGE(a) ((a & 0xFFFFF000) == ACUART_BASE)

namespace ACUART {
    bool SetupGameHandler(const std::string& S);
    u16 Read16(u32 addr);
    void Write16(u32 addr, u16 val);
    extern std::unique_ptr<ACUARTDevice> s_device;
    extern u16 IER;
    extern u16 LCR;
    extern u16 MCR;
    extern u16 SCR;
    extern u16 DLL;
    extern u16 DLH;
    extern u16 FCR_SHADOW;
}

/**
 * CARDIF:
 * just like with most of the expansion board regs, only low byte is considered from this u16 MMIO window
 * the sifcmd 1 from sensor thread will be sent if:
 * CARDIF_PORT, CARDIF_UNK0, CARDIF_UNK1, CARDIF_UNK2, CARDIF_UNK3 one of these changed
 * internal var `DAT_000012fc` is 0
 */

#define CARDIF_REGBASE0 0x1020
#define CARDIF_REGBASE1 0x10B0


#define CARDIF_PORT 0x10200000 
#define CARDIF_NPORT (CARDIF::PORT & 0x80)

#define CARDIF_UNK0 0x10200002 // sensor thread checks for value changes otherwise skips 0xB0200002
#define CARDIF_UNK1 0x10B00000 // sensor thread checks for value changes otherwise skips 0xB0B00000
#define CARDIF_UNK2 0x10B00002
#define CARDIF_UNK3 0x10B00004

namespace CARDIF {
    void InsertCard(u32 slot);
    void Write16(u32 mem, u16 val);
    u16 Read16(u32 mem);

    extern std::map<u32,u16>REG;
/**
  * PORT CONTROL REGISTER
  * values seen: `0E, 0F, 8B, 8F`  
  * BITS:
  * - 0x80: reader slot, (unset: reader 1, set: reader 2)
  */
	extern u16 PORT;
/**
 * SENSOR REGISTER
 * tested values:  
 * - 0xFF, 0xAF: card readers detected
 * - 0x80, 0x81, 0xF0: card readers not detected
 * - 0xAA, 0xEB: Card reader detected, sends command 0x20 (doesnt when in 0xFF value), stuck on "please wait..." while entering testmode
 * BITS: bitfield most likely, tested values: 0xFF: both card readers found, 0x80: no card reader found
 * - 0x4 : when set SENSOR is ON and lock is LOCK
 * - 0x80: unknown
 * 
*/
	extern u16 UNK0;
	extern u16 UNK1;
	extern u16 UNK2;
	extern u16 UNK3;
}