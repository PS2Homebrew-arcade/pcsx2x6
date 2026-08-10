#pragma once
#include "common/Pcsx2Types.h"
#include <string>

class ACUARTDevice
{
public:
    virtual ~ACUARTDevice() = default;

    virtual void Reset() {}
    virtual void Init() {}
    virtual void TxByte(u8 value) {}
    virtual bool RxByte(u8& value){return false;}
    virtual void Tick(u32 cycles) {}
    virtual bool HasData() const {return false;}
};