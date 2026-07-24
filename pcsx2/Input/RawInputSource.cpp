// SPDX-FileCopyrightText: 2026 PCSX2X6 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef _WIN32

#include "Input/RawInputSource.h"
#include "Input/InputManager.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include "Host.h"

#include "fmt/format.h"

#include <algorithm>

#include <hidsdi.h>

RawInputSource::RawInputSource() = default;

RawInputSource::~RawInputSource() = default;

std::string RawInputSource::GetDeviceIdentifier(u32 index)
{
	return fmt::format("RawMouse-{}", index);
}

bool RawInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	settings_lock.unlock();
	const std::optional<WindowInfo> toplevel_wi(Host::GetTopLevelWindowInfo());
	settings_lock.lock();

	if (!toplevel_wi.has_value() || toplevel_wi->type != WindowInfo::Type::Win32)
	{
		Console.Error("(RawInput) Missing top level window.");
		return false;
	}

	m_hwnd = static_cast<HWND>(toplevel_wi->window_handle);

	if (!EnumerateRawMice())
	{
		Console.Error("(RawInput) Failed to enumerate raw input devices.");
		return false;
	}

	settings_lock.unlock();
	AssignPointerIndices();
	settings_lock.lock();
	RebuildHandleMap();

	m_initialized = true;

	if (m_mice.empty())
	{
		Console.Warning("(RawInput) No mice found.");
		return true;
	}

	Console.WriteLn("(RawInput) Initialized with %zu mice.", m_mice.size());
	for (const auto& mouse : m_mice)
	{
		Console.WriteLn("  [pointer %u] %s", mouse.pointer_index, mouse.display_name.c_str());
		Console.WriteLn("    path: %s", mouse.device_path.c_str());
		InputManager::OnInputDeviceConnected(GetDeviceIdentifier(mouse.pointer_index), mouse.display_name);
	}

	return true;
}

void RawInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
}

static std::string GetDisplayNameForDevice(const std::string& device_path, u32 index)
{
	std::string product_name;
	const std::wstring wdevice_path = StringUtil::UTF8StringToWideString(device_path);
	if (!wdevice_path.empty())
	{
		HANDLE hid_handle = CreateFileW(wdevice_path.c_str(), 0,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (hid_handle != INVALID_HANDLE_VALUE)
		{
			wchar_t product_string[256] = {};
			if (HidD_GetProductString(hid_handle, product_string, sizeof(product_string)) && product_string[0] != L'\0')
				product_name = StringUtil::WideStringToUTF8String(product_string);
			CloseHandle(hid_handle);
		}
	}

	std::string tag; // static VID:PID tag to tell apart same-named devices
	const size_t vid_pos = device_path.find("VID_");
	const size_t pid_pos = device_path.find("PID_");
	if (vid_pos != std::string::npos && vid_pos + 8 <= device_path.size() &&
		pid_pos != std::string::npos && pid_pos + 8 <= device_path.size())
		tag = fmt::format("[{}:{}]", device_path.substr(vid_pos + 4, 4), device_path.substr(pid_pos + 4, 4));

	if (!product_name.empty())
		return tag.empty() ? fmt::format("{} (Mouse {})", product_name, index) :
							 fmt::format("{} {} (Mouse {})", product_name, tag, index);
	if (!tag.empty())
		return fmt::format("Mouse {} {}", index, tag);

	return fmt::format("Mouse {}", index);
}

static std::string ExtractVidPid(const std::string& device_path)
{
	// Extract "VID_XXXX&PID_XXXX" from a HID device path for port-independent matching.
	const size_t vid_pos = device_path.find("VID_");
	const size_t pid_pos = device_path.find("PID_");
	if (vid_pos == std::string::npos || pid_pos == std::string::npos)
		return {};
	const std::string vid = (vid_pos + 8 <= device_path.size()) ? device_path.substr(vid_pos, 8) : "";
	const std::string pid = (pid_pos + 8 <= device_path.size()) ? device_path.substr(pid_pos, 8) : "";
	if (vid.empty() || pid.empty())
		return {};
	return vid + "&" + pid; // "VID_XXXX&PID_XXXX"
}

static std::string GetDeviceIdentity(const std::string& device_path)
{
	// VID+PID when the path has one (survives port changes); otherwise the full path (I2C/Bluetooth/PS2).
	const std::string vidpid = ExtractVidPid(device_path);
	return vidpid.empty() ? device_path : vidpid;
}

static u32 FindStoredSlot(const std::string& identity)
{
	for (u32 slot = 0; slot < InputManager::MAX_POINTER_DEVICES; slot++)
	{
		if (Host::GetBaseStringSettingValue("RawInput", fmt::format("Pointer{}Device", slot).c_str(), "") == identity)
			return slot;
	}
	return InputManager::MAX_POINTER_DEVICES;
}

bool RawInputSource::ReloadDevices()
{
	// Re-enumerate raw mice. Match new HANDLEs to existing device_paths
	// so that pointer_index stays stable mid-session.
	std::lock_guard<std::mutex> guard(m_mice_mutex);

	UINT device_count = 0;
	if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
		return false;

	std::vector<RAWINPUTDEVICELIST> device_list(device_count);
	if (GetRawInputDeviceList(device_list.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
		return false;

	// Fresh enumeration keyed by full device path: unique per physical port, so twin devices
	// sharing a VID+PID and devices without one (I2C touchpads, Bluetooth) can't collide.
	std::unordered_map<std::string, HANDLE> new_devices;
	for (UINT i = 0; i < device_count; i++)
	{
		if (device_list[i].dwType != RIM_TYPEMOUSE)
			continue;

		UINT name_size = 0;
		GetRawInputDeviceInfoW(device_list[i].hDevice, RIDI_DEVICENAME, nullptr, &name_size);
		if (name_size == 0)
			continue;

		std::wstring wpath(name_size, L'\0');
		if (GetRawInputDeviceInfoW(device_list[i].hDevice, RIDI_DEVICENAME, wpath.data(), &name_size) == static_cast<UINT>(-1))
			continue;

		while (!wpath.empty() && wpath.back() == L'\0')
			wpath.pop_back();

		std::string path = StringUtil::WideStringToUTF8String(wpath);
		if (!path.empty())
			new_devices[std::move(path)] = device_list[i].hDevice;
	}

	// Update existing mice. Exact path first (same port), then VID+PID on another path
	// (replugged elsewhere); path goes first so twins can't steal each other's port match.
	bool changed = false;
	std::vector<bool> matched(m_mice.size(), false);
	for (size_t m = 0; m < m_mice.size(); m++)
	{
		const auto it = new_devices.find(m_mice[m].device_path);
		if (it == new_devices.end())
			continue;
		m_mice[m].handle = it->second;
		matched[m] = true;
		new_devices.erase(it);
	}
	for (size_t m = 0; m < m_mice.size(); m++)
	{
		if (matched[m])
			continue;
		const std::string vidpid = ExtractVidPid(m_mice[m].device_path);
		if (vidpid.empty())
			continue;
		const auto it = std::find_if(new_devices.begin(), new_devices.end(),
			[&](const auto& d) { return ExtractVidPid(d.first) == vidpid; });
		if (it == new_devices.end())
			continue;
		m_mice[m].handle = it->second;
		m_mice[m].device_path = it->first;
		matched[m] = true;
		new_devices.erase(it);
		Console.WriteLn("(RawInput) Updated handle for pointer %u (%s).", m_mice[m].pointer_index, m_mice[m].display_name.c_str());
	}
	for (size_t m = m_mice.size(); m-- > 0;)
	{
		if (matched[m])
			continue;
		Console.WriteLn("(RawInput) Device removed: pointer %u (%s).", m_mice[m].pointer_index, m_mice[m].display_name.c_str());
		const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, m_mice[m].pointer_index, 0);
		InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(m_mice[m].pointer_index));
		m_mice.erase(m_mice.begin() + m);
		changed = true;
	}

	// Any remaining entries are newly connected devices.
	for (const auto& [path, handle] : new_devices)
	{
		if (m_mice.size() >= InputManager::MAX_POINTER_DEVICES)
			break;

		// Saved slot if free (restores assignment after full disconnect/reconnect), else first free.
		const u32 preferred_slot = FindStoredSlot(GetDeviceIdentity(path));
		u32 taken_mask = 0;
		for (const auto& m : m_mice)
			if (m.pointer_index < InputManager::MAX_POINTER_DEVICES)
				taken_mask |= (1u << m.pointer_index);

		u32 assigned_slot;
		if (preferred_slot < InputManager::MAX_POINTER_DEVICES && !(taken_mask & (1u << preferred_slot)))
		{
			assigned_slot = preferred_slot;
		}
		else
		{
			assigned_slot = static_cast<u32>(std::countr_zero(~taken_mask));
			if (assigned_slot >= InputManager::MAX_POINTER_DEVICES)
				break;
		}

		RawMouseDevice dev;
		dev.handle = handle;
		dev.device_path = path;
		dev.display_name = GetDisplayNameForDevice(path, assigned_slot);
		dev.pointer_index = assigned_slot;
		dev.button_state = 0;

		Console.WriteLn("(RawInput) New device: pointer %u (%s).", dev.pointer_index, dev.display_name.c_str());
		InputManager::OnInputDeviceConnected(GetDeviceIdentifier(dev.pointer_index), dev.display_name);
		m_mice.push_back(std::move(dev));
		changed = true;
	}

	RebuildHandleMap();
	return changed;
}

void RawInputSource::Shutdown()
{
	std::lock_guard<std::mutex> guard(m_mice_mutex);
	for (const auto& mouse : m_mice)
	{
		const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, mouse.pointer_index, 0);
		InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(mouse.pointer_index));
	}

	m_mice.clear();
	m_handle_to_mouse_index.clear();
	m_hwnd = nullptr;
	m_initialized = false;
}

bool RawInputSource::IsInitialized()
{
	return m_initialized;
}

void RawInputSource::PollEvents()
{
	// Events arrive via MainWindow::nativeEvent -> ProcessRawInput.
}

std::vector<std::pair<std::string, std::string>> RawInputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> devices;
	for (const auto& mouse : m_mice)
		devices.emplace_back(GetDeviceIdentifier(mouse.pointer_index), mouse.display_name);
	return devices;
}

std::vector<InputBindingKey> RawInputSource::EnumerateMotors()
{
	return {};
}

bool RawInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	return false;
}

InputLayout RawInputSource::GetControllerLayout(u32 index)
{
	return InputLayout::Unknown;
}

void RawInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void RawInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
}

std::optional<InputBindingKey> RawInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("RawMouse-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> device_index = StringUtil::FromChars<s32>(device.substr(9));
	if (!device_index.has_value() || device_index.value() < 0)
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::RawInput;
	key.source_index = static_cast<u32>(device_index.value());

	for (u32 i = 0; i < NUM_BUTTONS; i++)
	{
		if (binding == s_button_names[i])
		{
			key.source_subtype = InputSubclass::ControllerButton;
			key.data = i;
			return key;
		}
	}

	if (binding.starts_with("Button"))
	{
		const std::optional<u32> button_index = StringUtil::FromChars<u32>(binding.substr(6));
		if (!button_index.has_value())
			return std::nullopt;

		key.source_subtype = InputSubclass::ControllerButton;
		key.data = button_index.value();
		return key;
	}

	return std::nullopt;
}

TinyString RawInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_type == InputSourceType::RawInput)
	{
		if (key.source_subtype == InputSubclass::ControllerButton)
		{
			if (display)
			{
				if (key.data < NUM_BUTTONS)
					ret.format("RawMouse-{} {}", u32{key.source_index}, s_button_display_names[key.data]);
				else
					ret.format("RawMouse-{} Button {}", u32{key.source_index}, key.data + 1);
			}
			else
			{
				if (key.data < NUM_BUTTONS)
					ret.format("RawMouse-{}/{}", u32{key.source_index}, s_button_names[key.data]);
				else
					ret.format("RawMouse-{}/Button{}", u32{key.source_index}, key.data);
			}
		}
	}

	return ret;
}

TinyString RawInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	return {};
}

bool RawInputSource::EnumerateRawMice()
{
	std::lock_guard<std::mutex> guard(m_mice_mutex);
	UINT device_count = 0;
	if (GetRawInputDeviceList(nullptr, &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
	{
		Console.Error("(RawInput) GetRawInputDeviceList(count) failed: %08X", GetLastError());
		return false;
	}

	if (device_count == 0)
		return true;

	std::vector<RAWINPUTDEVICELIST> device_list(device_count);
	if (GetRawInputDeviceList(device_list.data(), &device_count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
	{
		Console.Error("(RawInput) GetRawInputDeviceList(enum) failed: %08X", GetLastError());
		return false;
	}

	for (UINT i = 0; i < device_count; i++)
	{
		if (device_list[i].dwType != RIM_TYPEMOUSE)
			continue;

		const HANDLE handle = device_list[i].hDevice;

		UINT name_size = 0;
		GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, nullptr, &name_size);

		std::wstring wdevice_path;
		std::string device_path;
		if (name_size > 0)
		{
			wdevice_path.resize(name_size, L'\0');
			if (GetRawInputDeviceInfoW(handle, RIDI_DEVICENAME, wdevice_path.data(), &name_size) != static_cast<UINT>(-1))
			{
				while (!wdevice_path.empty() && wdevice_path.back() == L'\0')
					wdevice_path.pop_back();
				device_path = StringUtil::WideStringToUTF8String(wdevice_path);
			}
		}

		if (device_path.empty())
			continue;

		if (m_mice.size() >= InputManager::MAX_POINTER_DEVICES)
			break;

		RawMouseDevice dev;
		dev.handle = handle;
		dev.device_path = std::move(device_path);
		dev.pointer_index = 0; // assigned later by AssignPointerIndices
		dev.button_state = 0;

		m_mice.push_back(std::move(dev));
	}

	return true;
}

void RawInputSource::AssignPointerIndices()
{
	// Runs from Initialize() on the CPU thread, before any event flows; settings access is safe.
	if (m_mice.empty())
		return;

	bool slot_taken[InputManager::MAX_POINTER_DEVICES] = {};
	std::vector<bool> assigned(m_mice.size(), false);
	for (size_t m = 0; m < m_mice.size(); m++)
	{
		const u32 slot = FindStoredSlot(GetDeviceIdentity(m_mice[m].device_path));
		if (slot >= InputManager::MAX_POINTER_DEVICES || slot_taken[slot])
			continue;
		m_mice[m].pointer_index = slot;
		slot_taken[slot] = true;
		assigned[m] = true;
	}

	u32 next_free = 0;
	for (size_t m = 0; m < m_mice.size(); m++)
	{
		if (assigned[m])
			continue;
		while (next_free < InputManager::MAX_POINTER_DEVICES && slot_taken[next_free])
			next_free++;
		if (next_free >= InputManager::MAX_POINTER_DEVICES)
			break;
		m_mice[m].pointer_index = next_free;
		slot_taken[next_free] = true;
	}

	// Save the final assignment so ReloadDevices can restore it after disconnect/reconnect.
	for (auto& mouse : m_mice)
	{
		mouse.display_name = GetDisplayNameForDevice(mouse.device_path, mouse.pointer_index);
		Host::SetBaseStringSettingValue("RawInput",
			fmt::format("Pointer{}Device", mouse.pointer_index).c_str(), GetDeviceIdentity(mouse.device_path).c_str());
	}
	Host::CommitBaseSettingChanges();
}

void RawInputSource::RebuildHandleMap()
{
	m_handle_to_mouse_index.clear();
	for (u32 i = 0; i < static_cast<u32>(m_mice.size()); i++)
		m_handle_to_mouse_index[m_mice[i].handle] = i;
}

std::optional<u32> RawInputSource::GetPointerIndexForDevicePath(const std::string_view device_path) const
{
	std::lock_guard<std::mutex> guard(m_mice_mutex);
	for (const auto& mouse : m_mice)
	{
		if (mouse.device_path == device_path)
			return mouse.pointer_index;
		// Also match by VID+PID (stored by GetRawMouseDeviceList).
		const std::string vidpid = ExtractVidPid(mouse.device_path);
		if (!vidpid.empty() && vidpid == device_path)
			return mouse.pointer_index;
	}
	return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> RawInputSource::GetRawMouseDeviceList() const
{
	std::lock_guard<std::mutex> guard(m_mice_mutex);
	std::vector<std::pair<std::string, std::string>> result;
	for (const auto& mouse : m_mice)
		result.emplace_back(GetDeviceIdentity(mouse.device_path), mouse.display_name);
	return result;
}

void RawInputSource::ProcessRawInput(const RAWINPUT* raw, HWND render_hwnd)
{
	if (!m_initialized || raw->header.dwType != RIM_TYPEMOUSE)
		return;

	std::lock_guard<std::mutex> guard(m_mice_mutex);
	const auto it = m_handle_to_mouse_index.find(raw->header.hDevice);
	if (it == m_handle_to_mouse_index.end())
		return;

	const u32 mouse_idx = it->second;
	pxAssert(mouse_idx < m_mice.size());
	RawMouseDevice& mouse = m_mice[mouse_idx];
	const RAWMOUSE& rm = raw->data.mouse;
	const u32 pointer_index = mouse.pointer_index;

	const HWND coord_hwnd = render_hwnd ? render_hwnd : m_hwnd;

	// Position: absolute devices only (lightguns); relative mice keep the Qt pointer path.
	if (rm.usFlags & MOUSE_MOVE_ABSOLUTE)
	{
		const bool is_virtual_desktop = (rm.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
		const int screen_w = GetSystemMetrics(is_virtual_desktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
		const int screen_h = GetSystemMetrics(is_virtual_desktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

		if (screen_w > 0 && screen_h > 0)
		{
			POINT pt;
			pt.x = static_cast<LONG>(static_cast<float>(rm.lLastX) / 65535.0f * static_cast<float>(screen_w));
			pt.y = static_cast<LONG>(static_cast<float>(rm.lLastY) / 65535.0f * static_cast<float>(screen_h));

			if (is_virtual_desktop)
			{
				pt.x += GetSystemMetrics(SM_XVIRTUALSCREEN);
				pt.y += GetSystemMetrics(SM_YVIRTUALSCREEN);
			}

			if (ScreenToClient(coord_hwnd, &pt))
			{
				InputManager::UpdatePointerAbsolutePosition(
					pointer_index,
					static_cast<float>(pt.x),
					static_cast<float>(pt.y));
			}
		}
	}
	static constexpr struct
	{
		USHORT down_flag;
		USHORT up_flag;
		u32 button_index;
	} button_map[] = {
		{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0},
		{RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1},
		{RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2},
	};

	for (const auto& bm : button_map)
	{
		if (rm.usButtonFlags & bm.down_flag)
		{
			mouse.button_state |= (1u << bm.button_index);
			const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index);
			Host::RunOnCPUThread([key]() { InputManager::InvokeEvents(key, 1.0f, GenericInputBinding::Unknown); });
		}
		else if (rm.usButtonFlags & bm.up_flag)
		{
			mouse.button_state &= ~(1u << bm.button_index);
			const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::RawInput, pointer_index, bm.button_index);
			Host::RunOnCPUThread([key]() { InputManager::InvokeEvents(key, 0.0f, GenericInputBinding::Unknown); });
		}
	}
}

#endif // _WIN32
