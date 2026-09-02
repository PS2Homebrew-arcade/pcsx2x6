// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Namco System 246/256 "UE PCB" USB-Ethernet device (ADMtek Pegasus / AN986)
// with a built-in TCP netplay transport.
//
// The UE PCB is the USB-Ethernet board Namco used for cabinet-to-cabinet
// versus play (e.g. Gundam SEED Destiny: Rengou vs. Z.A.F.T. II). This device
// emulates the board's AN986 chip at the register/endpoint level, so the
// game's own AN986.IRX driver and AVE-TCP network stack run unmodified on the
// emulated IOP. Ethernet frames the game transmits are tunneled over a plain
// TCP connection to the other emulator instance(s); 2-4 players, same PC or
// across a LAN/VPN.
//
// Register-level behavior was modeled on captures from a real UE PCB.
// Device descriptors were originally ported from the Play! emulator's HLE
// Iop/UsbUePcbDevice.

#pragma once
#include "USB/deviceproxy.h"

namespace usb_uepcb
{
	class UePcbDevice final : public DeviceProxy
	{
	public:
		const char* Name() const override;
		const char* TypeName() const override;
		const char* IconName() const override;
		USBDevice* CreateDevice(SettingsInterface& si, u32 port, u32 subtype) const override;
		bool Freeze(USBDevice* dev, StateWrapper& sw) const override;
		std::span<const SettingInfo> Settings(u32 subtype) const override;
	};
} // namespace usb_uepcb
