// SPDX-FileCopyrightText: 2026 PCSX2X6 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <mutex>

#ifdef _WIN32

#include "common/RedtapeWindows.h"
#include "Input/InputSource.h"

#include <string>
#include <unordered_map>
#include <vector>

struct tagRAWINPUT;
typedef struct tagRAWINPUT RAWINPUT;

class RawInputSource final : public InputSource
{
public:
	enum : u32
	{
		NUM_BUTTONS = 3,
	};

	RawInputSource();
	~RawInputSource() override;

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

	/// Called from MainWindow::nativeEvent to dispatch WM_INPUT messages.
	void ProcessRawInput(const RAWINPUT* raw, HWND render_hwnd);

	/// Pointer index for a device path or VID+PID identity, if known.
	std::optional<u32> GetPointerIndexForDevicePath(const std::string_view device_path) const;

	/// All mice as (identity, display_name) for UI dropdowns.
	std::vector<std::pair<std::string, std::string>> GetRawMouseDeviceList() const;

private:
	struct RawMouseDevice
	{
		HANDLE handle = nullptr;
		std::string device_path;
		std::string display_name;
		u32 pointer_index = 0;
		u32 button_state = 0;
	};

	bool EnumerateRawMice();

	/// Saved-identity slots first, then first-free; persists the result.
	void AssignPointerIndices();

	void RebuildHandleMap();

	static std::string GetDeviceIdentifier(u32 index);

	HWND m_hwnd = nullptr;
	bool m_initialized = false;
	std::vector<RawMouseDevice> m_mice;
	mutable std::mutex m_mice_mutex; // Protects m_mice and m_handle_to_mouse_index
	std::unordered_map<HANDLE, u32> m_handle_to_mouse_index;

	static constexpr const char* s_button_names[] = {"LeftButton", "RightButton", "MiddleButton"};
	static constexpr const char* s_button_display_names[] = {"Left Button", "Right Button", "Middle Button"};
};

#endif // _WIN32
