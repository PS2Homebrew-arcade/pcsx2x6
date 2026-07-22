// SPDX-FileCopyrightText: 2026 PCSX2X6 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef __linux__

#include "Input/EvdevInputSource.h"
#include "Input/InputManager.h"
#include "ImGui/ImGuiManager.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include "Host.h"

#include "fmt/format.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <libudev.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool TestBit(const unsigned long* bits, u32 bit)
{
	static constexpr u32 bits_per_long = sizeof(unsigned long) * 8;
	return (bits[bit / bits_per_long] >> (bit % bits_per_long)) & 1u;
}

static std::optional<u32> MapButtonCode(u16 code, bool touch_is_left)
{
	switch (code)
	{
		case BTN_LEFT:
			return 0;
		case BTN_RIGHT:
			return 1;
		case BTN_MIDDLE:
			return 2;
		case BTN_SIDE:
			return 3;
		case BTN_EXTRA:
			return 4;
		case BTN_TOUCH:
			// Contact acts as the left button on devices without one (guns/touchscreens reporting only BTN_TOUCH).
			return touch_is_left ? std::optional<u32>(0) : std::nullopt;
		default:
			// Merged virtual light guns (e.g. Batocera) expose their extra buttons as BTN_1..BTN_8.
			if (code >= BTN_1 && code <= BTN_8)
				return 5 + (code - BTN_1);
			return std::nullopt;
	}
}

static bool PropIsSet(udev_device* dev, const char* name)
{
	const char* value = udev_device_get_property_value(dev, name);
	return value && value[0] == '1';
}

static bool IsPointerCandidate(udev_device* dev)
{
	// ID_INPUT_GUN is not a standard udev tag; distributions with lightgun udev rules (e.g. Batocera) set it.
	return PropIsSet(dev, "ID_INPUT_MOUSE") || PropIsSet(dev, "ID_INPUT_TOUCHPAD") ||
		   PropIsSet(dev, "ID_INPUT_TOUCHSCREEN") || PropIsSet(dev, "ID_INPUT_TABLET") ||
		   PropIsSet(dev, "ID_INPUT_GUN");
}

// sysfs attribute values end with a newline and may carry padding; strip both without copying.
static std::string_view TrimView(const char* str)
{
	std::string_view sv(str ? str : "");
	while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r' || sv.back() == ' ' || sv.back() == '\t'))
		sv.remove_suffix(1);
	while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
		sv.remove_prefix(1);
	return sv;
}

static u32 FindStoredSlot(const std::string& identity)
{
	for (u32 slot = 0; slot < InputManager::MAX_POINTER_DEVICES; slot++)
	{
		if (Host::GetBaseStringSettingValue("Evdev", fmt::format("Pointer{}Device", slot).c_str(), "") == identity)
			return slot;
	}
	return InputManager::MAX_POINTER_DEVICES;
}

EvdevInputSource::EvdevInputSource() = default;

EvdevInputSource::~EvdevInputSource()
{
	Shutdown();
}

std::string EvdevInputSource::GetDeviceIdentifier(u32 index)
{
	return fmt::format("EvdevMouse-{}", index);
}

std::vector<EvdevInputSource::Candidate> EvdevInputSource::ScanDevices()
{
	std::vector<Candidate> candidates;

	udev_enumerate* enumerate = udev_enumerate_new(m_udev);
	if (!enumerate)
		return candidates;

	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_scan_devices(enumerate);

	udev_list_entry* entry;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate))
	{
		udev_device* dev = udev_device_new_from_syspath(m_udev, udev_list_entry_get_name(entry));
		if (!dev)
			continue;

		const char* devnode = udev_device_get_devnode(dev);
		if (devnode && std::string_view(devnode).starts_with("/dev/input/event") && IsPointerCandidate(dev))
		{
			udev_device* input_parent = udev_device_get_parent_with_subsystem_devtype(dev, "input", nullptr);

			Candidate c;
			c.devnode = devnode;

			const char* id_path = udev_device_get_property_value(dev, "ID_PATH");
			if (id_path)
				c.path_identity = fmt::format("path:{}", id_path);

			// Identity cascade: serial, then VID:PID, then physical port, then device name
			// (uinput virtual devices expose none of the first three).
			const char* vid = udev_device_get_property_value(dev, "ID_VENDOR_ID");
			const char* pid = udev_device_get_property_value(dev, "ID_MODEL_ID");
			const std::string_view uniq = TrimView(input_parent ? udev_device_get_sysattr_value(input_parent, "uniq") : nullptr);
			const std::string_view name = TrimView(input_parent ? udev_device_get_sysattr_value(input_parent, "name") : nullptr);
			if (vid && pid)
				c.vidpid_identity = fmt::format("vidpid:{}:{}", vid, pid);
			// Serials shorter than 8 chars are firmware placeholders shared by every unit (e.g. Sinden "HIDLG").
			if (uniq.size() >= 8)
				c.identity = fmt::format("uniq:{}", uniq);
			else if (!c.vidpid_identity.empty())
				c.identity = c.vidpid_identity;
			else if (!c.path_identity.empty())
				c.identity = c.path_identity;
			else if (!name.empty())
				c.identity = fmt::format("name:{}", name);
			else
				c.identity = fmt::format("node:{}", devnode);

			std::string tag; // static VID:PID tag to tell apart same-named devices
			if (vid && pid)
			{
				tag = fmt::format("[{}:{}]", vid, pid);
				for (char& ch : tag)
					ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			}

			const std::string product = name.empty() ? std::string("Pointer Device") : std::string(name);
			c.base_name = tag.empty() ? product : fmt::format("{} {}", product, tag);

			candidates.push_back(std::move(c));
		}

		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);

	// Devices sharing an identity (same-model twins, shared placeholder serial) degrade to the next
	// level that separates them: VID:PID, else the physical port, else the node.
	std::vector<std::string> original;
	original.reserve(candidates.size());
	for (const Candidate& c : candidates)
		original.push_back(c.identity);
	for (size_t i = 0; i < candidates.size(); i++)
	{
		if (std::count(original.begin(), original.end(), original[i]) < 2)
			continue;
		Candidate& c = candidates[i];
		const auto vidpid_count = std::count_if(candidates.begin(), candidates.end(),
			[&](const Candidate& o) { return o.vidpid_identity == c.vidpid_identity; });
		if (!c.vidpid_identity.empty() && c.identity != c.vidpid_identity && vidpid_count < 2)
			c.identity = c.vidpid_identity;
		else if (!c.path_identity.empty() && c.identity != c.path_identity)
			c.identity = c.path_identity;
		else
			c.identity = fmt::format("node:{}", c.devnode);
	}

	return candidates;
}

bool EvdevInputSource::OpenDevice(const Candidate& candidate, EvdevPointerDevice* dev)
{
	const int fd = open(candidate.devnode.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		Console.Warning("(Evdev) Failed to open %s: %s. Is the user in the 'input' group?",
			candidate.devnode.c_str(), strerror(errno));
		return false;
	}

	unsigned long abs_bits[(ABS_MAX / (sizeof(unsigned long) * 8)) + 1] = {};
	unsigned long key_bits[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1] = {};
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

	dev->fd = fd;
	dev->devnode = candidate.devnode;
	dev->identity = candidate.identity;
	dev->base_name = candidate.base_name;
	dev->absolute = TestBit(abs_bits, ABS_X) && TestBit(abs_bits, ABS_Y);
	dev->touch_is_left = !TestBit(key_bits, BTN_LEFT) && TestBit(key_bits, BTN_TOUCH);

	if (dev->absolute)
	{
		if (ioctl(fd, EVIOCGABS(ABS_X), &dev->abs_x) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &dev->abs_y) < 0)
		{
			dev->absolute = false;
		}
		else
		{
			dev->cur_x = dev->abs_x.value;
			dev->cur_y = dev->abs_y.value;
		}
	}

	return true;
}

void EvdevInputSource::CloseDevice(EvdevPointerDevice& dev)
{
	if (dev.fd < 0)
		return;
	if (dev.grabbed)
		ioctl(dev.fd, EVIOCGRAB, 0);
	close(dev.fd);
	dev.fd = -1;
	dev.grabbed = false;
}

// Stored identities claim their saved slot in the first pass so scan order can't steal it,
// then the rest take the lowest free slot.
bool EvdevInputSource::AddDevices(std::vector<Candidate> candidates)
{
	u32 taken = 0;
	for (const EvdevPointerDevice& dev : m_devices)
		taken |= 1u << dev.pointer_index;

	bool changed = false;
	for (const bool stored_pass : {true, false})
	{
		for (Candidate& candidate : candidates)
		{
			if (candidate.devnode.empty())
				continue;
			u32 slot = FindStoredSlot(candidate.identity);
			const bool has_stored = slot < InputManager::MAX_POINTER_DEVICES && !(taken & (1u << slot));
			if (stored_pass != has_stored)
				continue;
			if (!has_stored)
			{
				slot = static_cast<u32>(std::countr_zero(~taken));
				if (slot >= InputManager::MAX_POINTER_DEVICES)
					return changed;
			}

			EvdevPointerDevice dev;
			if (OpenDevice(candidate, &dev))
			{
				dev.pointer_index = slot;
				dev.display_name = fmt::format("{} (Mouse {})", dev.base_name, slot);
				taken |= 1u << slot;
				Console.WriteLn("(Evdev) Pointer %u: %s", slot, dev.display_name.c_str());
				Console.WriteLn("    node: %s, identity: %s", dev.devnode.c_str(), dev.identity.c_str());
				InputManager::OnInputDeviceConnected(GetDeviceIdentifier(slot), dev.display_name);
				m_devices.push_back(std::move(dev));
				changed = true;
			}
			candidate.devnode.clear();
		}
	}
	return changed;
}

void EvdevInputSource::EmitButton(EvdevPointerDevice& dev, u32 button, bool down,
	std::vector<std::pair<InputBindingKey, float>>& button_events)
{
	const u32 mask = 1u << button;
	const u32 new_state = down ? (dev.button_state | mask) : (dev.button_state & ~mask);
	if (new_state == dev.button_state)
		return;
	dev.button_state = new_state;
	button_events.emplace_back(
		MakeGenericControllerButtonKey(InputSourceType::Evdev, dev.pointer_index, static_cast<s32>(button)),
		down ? 1.0f : 0.0f);
}

void EvdevInputSource::DeliverPosition(EvdevPointerDevice& dev)
{
	const float window_width = ImGuiManager::GetWindowWidth();
	const float window_height = ImGuiManager::GetWindowHeight();
	if (window_width <= 0.0f || window_height <= 0.0f)
		return;

	float x, y;
	if (dev.absolute)
	{
		const s32 range_x = dev.abs_x.maximum - dev.abs_x.minimum;
		const s32 range_y = dev.abs_y.maximum - dev.abs_y.minimum;
		if (range_x <= 0 || range_y <= 0)
			return;

		// The device range maps to the render window, so windowed mode covers exactly the game view.
		x = std::clamp(static_cast<float>(dev.cur_x - dev.abs_x.minimum) / static_cast<float>(range_x), 0.0f, 1.0f) * window_width;
		y = std::clamp(static_cast<float>(dev.cur_y - dev.abs_y.minimum) / static_cast<float>(range_y), 0.0f, 1.0f) * window_height;
	}
	else
	{
		// Relative devices accumulate raw counts into a window-space position, starting centered.
		if (!dev.rel_initialized)
		{
			dev.rel_x = window_width * 0.5f;
			dev.rel_y = window_height * 0.5f;
			dev.rel_initialized = true;
		}
		dev.rel_x = std::clamp(dev.rel_x, 0.0f, window_width);
		dev.rel_y = std::clamp(dev.rel_y, 0.0f, window_height);
		x = dev.rel_x;
		y = dev.rel_y;
	}
	InputManager::UpdatePointerAbsolutePosition(dev.pointer_index, x, y);
}

void EvdevInputSource::ResyncDevice(EvdevPointerDevice& dev, std::vector<std::pair<InputBindingKey, float>>& button_events)
{
	// The kernel dropped events; re-read the actual state instead of trusting the stream.
	if (dev.absolute && ioctl(dev.fd, EVIOCGABS(ABS_X), &dev.abs_x) >= 0 && ioctl(dev.fd, EVIOCGABS(ABS_Y), &dev.abs_y) >= 0)
	{
		dev.cur_x = dev.abs_x.value;
		dev.cur_y = dev.abs_y.value;
		dev.pos_dirty = true;
	}

	unsigned long key_state[(KEY_MAX / (sizeof(unsigned long) * 8)) + 1] = {};
	if (ioctl(dev.fd, EVIOCGKEY(sizeof(key_state)), key_state) < 0)
		return;

	static constexpr u16 codes[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA, BTN_TOUCH,
		BTN_1, BTN_2, BTN_3, BTN_4, BTN_5, BTN_6, BTN_7, BTN_8};
	for (const u16 code : codes)
	{
		if (code == BTN_LEFT && dev.touch_is_left)
			continue;
		const std::optional<u32> button = MapButtonCode(code, dev.touch_is_left);
		if (button.has_value())
			EmitButton(dev, button.value(), TestBit(key_state, code), button_events);
	}
}

void EvdevInputSource::ProcessDeviceEvent(EvdevPointerDevice& dev, const struct input_event& ev,
	std::vector<std::pair<InputBindingKey, float>>& button_events)
{
	switch (ev.type)
	{
		case EV_ABS:
		{
			if (ev.code == ABS_X)
			{
				dev.cur_x = ev.value;
				dev.pos_dirty = true;
			}
			else if (ev.code == ABS_Y)
			{
				dev.cur_y = ev.value;
				dev.pos_dirty = true;
			}
			break;
		}

		case EV_REL:
		{
			if (ev.code == REL_X)
			{
				dev.rel_x += static_cast<float>(ev.value);
				dev.pos_dirty = true;
			}
			else if (ev.code == REL_Y)
			{
				dev.rel_y += static_cast<float>(ev.value);
				dev.pos_dirty = true;
			}
			break;
		}

		case EV_KEY:
		{
			if (ev.value > 1)
				break;
			const std::optional<u32> button = MapButtonCode(static_cast<u16>(ev.code), dev.touch_is_left);
			if (button.has_value())
				EmitButton(dev, button.value(), ev.value != 0, button_events);
			break;
		}

		case EV_SYN:
		{
			if (ev.code == SYN_REPORT)
			{
				if (dev.pos_dirty)
				{
					DeliverPosition(dev);
					dev.pos_dirty = false;
				}
			}
			else if (ev.code == SYN_DROPPED)
			{
				ResyncDevice(dev, button_events);
			}
			break;
		}

		default:
			break;
	}
}

void EvdevInputSource::UpdateGrabs()
{
	// Exclusive grab keeps aimed devices from also driving the desktop cursor; released on pause/stop.
	const bool want_grabs = VMManager::GetState() == VMState::Running;
	const u32 claimed = m_claimed_slots.load(std::memory_order_relaxed);
	for (EvdevPointerDevice& dev : m_devices)
	{
		const bool want = want_grabs && ((claimed >> dev.pointer_index) & 1u);
		if (want == dev.grabbed)
			continue;
		if (ioctl(dev.fd, EVIOCGRAB, want ? 1 : 0) >= 0)
		{
			dev.grabbed = want;
			Console.WriteLn("(Evdev) %s %s.", want ? "Grabbed" : "Released", dev.display_name.c_str());
		}
		else if (!want)
		{
			dev.grabbed = false;
		}
	}
}

bool EvdevInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	m_udev = udev_new();
	if (!m_udev)
	{
		Console.Error("(Evdev) udev_new() failed.");
		return false;
	}

	settings_lock.unlock();
	{
		std::lock_guard<std::mutex> guard(m_devices_mutex);
		AddDevices(ScanDevices());

		// Save the assignment so devices keep their slot across sessions and replugs.
		for (const EvdevPointerDevice& dev : m_devices)
			Host::SetBaseStringSettingValue("Evdev",
				fmt::format("Pointer{}Device", dev.pointer_index).c_str(), dev.identity.c_str());
		Host::CommitBaseSettingChanges();

		Console.WriteLn("(Evdev) Initialized with %zu pointer devices.", m_devices.size());
	}
	settings_lock.lock();

	ComputeClaimedSlots(si);

	m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
	if (m_monitor)
	{
		udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "input", nullptr);
		udev_monitor_enable_receiving(m_monitor);
	}

	m_initialized = true;
	return true;
}

void EvdevInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	ComputeClaimedSlots(si);
}

void EvdevInputSource::ComputeClaimedSlots(SettingsInterface& si)
{
	// Grab only the devices explicitly aimed with by a gun; never the desktop mouse.
	u32 claimed = 0;
	for (u32 port = 0; port < USB::NUM_PORTS; port++)
	{
		if (USB::GetConfigDevice(si, port) != "guncon2")
			continue;
		const std::string binding = USB::GetConfigString(si, port, "guncon2", "Pointer");
		if (!std::string_view(binding).starts_with("Pointer-"))
			continue;
		const std::optional<u32> index = StringUtil::FromChars<u32>(std::string_view(binding).substr(8));
		if (index.has_value() && index.value() < InputManager::MAX_POINTER_DEVICES)
			claimed |= 1u << index.value();
	}
	m_claimed_slots.store(claimed, std::memory_order_relaxed);
}

bool EvdevInputSource::ReloadDevices()
{
	std::lock_guard<std::mutex> guard(m_devices_mutex);
	std::vector<Candidate> candidates = ScanDevices();

	// Same node first, then same identity on another node (replugged elsewhere); node goes first
	// so twins can't steal each other's port match.
	bool changed = false;
	std::vector<bool> matched(m_devices.size(), false);
	for (size_t d = 0; d < m_devices.size(); d++)
	{
		const auto it = std::find_if(candidates.begin(), candidates.end(),
			[&](const Candidate& c) { return c.devnode == m_devices[d].devnode; });
		if (it == candidates.end())
			continue;
		matched[d] = true;
		candidates.erase(it);
	}
	for (size_t d = 0; d < m_devices.size(); d++)
	{
		if (matched[d])
			continue;
		const auto it = std::find_if(candidates.begin(), candidates.end(),
			[&](const Candidate& c) { return c.identity == m_devices[d].identity; });
		if (it == candidates.end())
			continue;
		EvdevPointerDevice fresh;
		if (OpenDevice(*it, &fresh))
		{
			fresh.pointer_index = m_devices[d].pointer_index;
			fresh.display_name = fmt::format("{} (Mouse {})", fresh.base_name, fresh.pointer_index);
			CloseDevice(m_devices[d]);
			m_devices[d] = std::move(fresh);
			matched[d] = true;
			Console.WriteLn("(Evdev) Updated node for pointer %u (%s).", m_devices[d].pointer_index, m_devices[d].display_name.c_str());
		}
		candidates.erase(it);
	}
	for (size_t d = m_devices.size(); d-- > 0;)
	{
		if (matched[d])
			continue;
		Console.WriteLn("(Evdev) Device removed: pointer %u (%s).", m_devices[d].pointer_index, m_devices[d].display_name.c_str());
		const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::Evdev, m_devices[d].pointer_index, 0);
		InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(m_devices[d].pointer_index));
		CloseDevice(m_devices[d]);
		m_devices.erase(m_devices.begin() + d);
		changed = true;
	}

	changed |= AddDevices(std::move(candidates));
	return changed;
}

void EvdevInputSource::Shutdown()
{
	{
		std::lock_guard<std::mutex> guard(m_devices_mutex);
		for (EvdevPointerDevice& dev : m_devices)
		{
			const InputBindingKey key = MakeGenericControllerButtonKey(InputSourceType::Evdev, dev.pointer_index, 0);
			InputManager::OnInputDeviceDisconnected(key, GetDeviceIdentifier(dev.pointer_index));
			CloseDevice(dev);
		}
		m_devices.clear();
	}

	if (m_monitor)
	{
		udev_monitor_unref(m_monitor);
		m_monitor = nullptr;
	}
	if (m_udev)
	{
		udev_unref(m_udev);
		m_udev = nullptr;
	}
	m_initialized = false;
}

bool EvdevInputSource::IsInitialized()
{
	return m_initialized;
}

void EvdevInputSource::PollEvents()
{
	if (!m_initialized)
		return;

	bool reload = false;
	if (m_monitor)
	{
		while (udev_device* dev = udev_monitor_receive_device(m_monitor))
		{
			const char* devnode = udev_device_get_devnode(dev);
			if (devnode && std::string_view(devnode).starts_with("/dev/input/event"))
				reload = true;
			udev_device_unref(dev);
		}
	}
	if (reload)
		ReloadDevices();

	// Events are collected under the lock and fired after, so a handler can safely reenter the source.
	std::vector<std::pair<InputBindingKey, float>> button_events;
	bool lost_device = false;
	{
		std::lock_guard<std::mutex> guard(m_devices_mutex);
		UpdateGrabs();
		for (EvdevPointerDevice& dev : m_devices)
		{
			for (;;)
			{
				struct input_event evs[64];
				const ssize_t r = read(dev.fd, evs, sizeof(evs));
				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					if (errno != EAGAIN && errno != EWOULDBLOCK)
						lost_device = true;
					break;
				}
				if (r == 0)
					break;
				const size_t count = static_cast<size_t>(r) / sizeof(struct input_event);
				for (size_t i = 0; i < count; i++)
					ProcessDeviceEvent(dev, evs[i], button_events);
			}
		}
	}
	if (lost_device)
		ReloadDevices();

	for (const auto& [key, value] : button_events)
		InputManager::InvokeEvents(key, value, GenericInputBinding::Unknown);
}

std::vector<std::pair<std::string, std::string>> EvdevInputSource::EnumerateDevices()
{
	std::lock_guard<std::mutex> guard(m_devices_mutex);
	std::vector<std::pair<std::string, std::string>> devices;
	for (const auto& dev : m_devices)
		devices.emplace_back(GetDeviceIdentifier(dev.pointer_index), dev.display_name);
	return devices;
}

std::vector<InputBindingKey> EvdevInputSource::EnumerateMotors()
{
	return {};
}

bool EvdevInputSource::GetGenericBindingMapping(const std::string_view device, InputManager::GenericInputBindingMapping* mapping)
{
	return false;
}

InputLayout EvdevInputSource::GetControllerLayout(u32 index)
{
	return InputLayout::Unknown;
}

void EvdevInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
}

void EvdevInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
}

std::optional<InputBindingKey> EvdevInputSource::ParseKeyString(const std::string_view device, const std::string_view binding)
{
	if (!device.starts_with("EvdevMouse-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> device_index = StringUtil::FromChars<s32>(device.substr(11));
	if (!device_index.has_value() || device_index.value() < 0)
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::Evdev;
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

TinyString EvdevInputSource::ConvertKeyToString(InputBindingKey key, bool display, bool migration)
{
	TinyString ret;

	if (key.source_type == InputSourceType::Evdev)
	{
		if (key.source_subtype == InputSubclass::ControllerButton)
		{
			if (display)
			{
				if (key.data < NUM_BUTTONS)
					ret.format("EvdevMouse-{} {}", u32{key.source_index}, s_button_display_names[key.data]);
				else
					ret.format("EvdevMouse-{} Button {}", u32{key.source_index}, key.data + 1);
			}
			else
			{
				if (key.data < NUM_BUTTONS)
					ret.format("EvdevMouse-{}/{}", u32{key.source_index}, s_button_names[key.data]);
				else
					ret.format("EvdevMouse-{}/Button{}", u32{key.source_index}, key.data);
			}
		}
	}

	return ret;
}

TinyString EvdevInputSource::ConvertKeyToIcon(InputBindingKey key)
{
	return {};
}

std::optional<u32> EvdevInputSource::GetPointerIndexForIdentity(const std::string_view identity) const
{
	std::lock_guard<std::mutex> guard(m_devices_mutex);
	for (const auto& dev : m_devices)
	{
		if (dev.identity == identity)
			return dev.pointer_index;
	}
	return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> EvdevInputSource::GetPointerDeviceList() const
{
	std::lock_guard<std::mutex> guard(m_devices_mutex);
	std::vector<std::pair<std::string, std::string>> result;
	for (const auto& dev : m_devices)
		result.emplace_back(dev.identity, dev.display_name);
	return result;
}

#endif // __linux__
