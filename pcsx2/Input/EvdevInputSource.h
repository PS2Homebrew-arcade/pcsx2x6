// SPDX-FileCopyrightText: 2026 PCSX2X6 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <mutex>

#ifdef __linux__

#include "Input/InputSource.h"

#include <atomic>
#include <linux/input.h>
#include <string>
#include <vector>

struct udev;
struct udev_monitor;

class EvdevInputSource final : public InputSource
{
public:
	enum : u32
	{
		NUM_BUTTONS = 3,
	};

	EvdevInputSource();
	~EvdevInputSource() override;

	bool Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	void UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock) override;
	bool ReloadDevices() override;
	void Shutdown() override;
	bool IsInitialized() override;

	void PollEvents() override;
	std::vector<std::pair<std::string, std::string>> EnumerateDevices() override;
	std::vector<InputBindingKey> EnumerateMotors() override;
	bool GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping) override;
	InputLayout GetControllerLayout(u32 index) override;
	void UpdateMotorState(InputBindingKey key, float intensity) override;
	void UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity) override;

	std::optional<InputBindingKey> ParseKeyString(const std::string_view device, const std::string_view binding) override;
	TinyString ConvertKeyToString(InputBindingKey key, bool display = false, bool migration = false) override;
	TinyString ConvertKeyToIcon(InputBindingKey key) override;

	/// Pointer index for a device identity, if known.
	std::optional<u32> GetPointerIndexForIdentity(const std::string_view identity) const;

	/// All pointer devices as (identity, display_name) for UI dropdowns.
	std::vector<std::pair<std::string, std::string>> GetPointerDeviceList() const;

private:
	struct EvdevPointerDevice
	{
		int fd = -1;
		std::string devnode;
		std::string identity;
		std::string base_name;
		std::string display_name;
		u32 pointer_index = 0;
		bool absolute = false;
		bool touch_is_left = false;
		bool grabbed = false;
		bool pos_dirty = false;
		bool rel_initialized = false;
		struct input_absinfo abs_x = {};
		struct input_absinfo abs_y = {};
		s32 cur_x = 0;
		s32 cur_y = 0;
		float rel_x = 0.0f;
		float rel_y = 0.0f;
		u32 button_state = 0;
	};

	struct Candidate
	{
		std::string devnode;
		std::string identity;
		std::string vidpid_identity;
		std::string path_identity;
		std::string base_name;
	};

	std::vector<Candidate> ScanDevices();
	static bool OpenDevice(const Candidate& candidate, EvdevPointerDevice* dev);
	static void CloseDevice(EvdevPointerDevice& dev);

	/// Stored identities claim their saved slot first, then the rest take the lowest free slot.
	bool AddDevices(std::vector<Candidate> candidates);

	void ComputeClaimedSlots(SettingsInterface& si);
	void UpdateGrabs();

	static void EmitButton(EvdevPointerDevice& dev, u32 button, bool down,
		std::vector<std::pair<InputBindingKey, float>>& button_events);
	static void ProcessDeviceEvent(EvdevPointerDevice& dev, const struct input_event& ev,
		std::vector<std::pair<InputBindingKey, float>>& button_events);
	static void ResyncDevice(EvdevPointerDevice& dev, std::vector<std::pair<InputBindingKey, float>>& button_events);
	static void DeliverPosition(EvdevPointerDevice& dev);

	static std::string GetDeviceIdentifier(u32 index);

	udev* m_udev = nullptr;
	udev_monitor* m_monitor = nullptr;
	bool m_initialized = false;
	std::atomic<u32> m_claimed_slots{0};
	std::vector<EvdevPointerDevice> m_devices;
	mutable std::mutex m_devices_mutex; // Protects m_devices

	static constexpr const char* s_button_names[] = {"LeftButton", "RightButton", "MiddleButton"};
	static constexpr const char* s_button_display_names[] = {"Left Button", "Right Button", "Middle Button"};
};

#endif // __linux__
