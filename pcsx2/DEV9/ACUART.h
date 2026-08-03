#pragma once
#include "ACUART_GLUE.h"
#include "MemoryTypes.h"
#include <memory>

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
