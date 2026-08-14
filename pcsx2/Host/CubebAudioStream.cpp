// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"
#include "Host.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/ScopedGuard.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"

#include "cubeb/cubeb.h"
#include "fmt/format.h"
#include "IconsFontAwesome.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <objbase.h>

#include "wil/resource.h"
#endif

namespace
{
	static constexpr const char* CUBEB_WASAPI_BACKEND = "wasapi";
	static constexpr const char* CUBEB_IAUDIOCLIENT3_BACKEND = "iaudioclient3";
	static constexpr const char* CUBEB_WASAPI_EXCLUSIVE_BACKEND = "wasapi-exclusive";

	u32 GetCubebDeviceMinimumLatencyFrames(
		const cubeb_device_info& device, u32 target_rate, const char* backend_id, u32 fallback_frames)
	{
		const bool is_wasapi = (backend_id && std::strcmp(backend_id, CUBEB_WASAPI_BACKEND) == 0);
		const bool is_iaudioclient3 =
			(backend_id && std::strcmp(backend_id, CUBEB_IAUDIOCLIENT3_BACKEND) == 0);
		const bool is_wasapi_exclusive =
			(backend_id && std::strcmp(backend_id, CUBEB_WASAPI_EXCLUSIVE_BACKEND) == 0);
		if ((!is_wasapi && !is_iaudioclient3 && !is_wasapi_exclusive) || device.default_rate == 0)
			return fallback_frames;

		const u32 device_frames = (is_iaudioclient3 || is_wasapi_exclusive) ? device.latency_lo : device.latency_hi;
		if (device_frames == 0)
			return fallback_frames;

		return static_cast<u32>((static_cast<u64>(device_frames) * target_rate + device.default_rate - 1) /
								device.default_rate);
	}

	class CubebAudioStream : public AudioStream
	{
	public:
		CubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~CubebAudioStream() override;

		void SetPaused(bool paused) override;

		bool Initialize(const char* driver_name, const char* device_name, AudioSynchronizationMode sync_mode, Error* error);

	private:
		static void LogCallback(const char* fmt, ...);
		static long DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
			long nframes);
		static void StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state);

		void DestroyContextAndStream();

#ifdef _WIN32
		// Keep it as the first field, as COM must uninitialize last.
		wil::unique_couninitialize_call m_coUninit{false};
#endif

		cubeb* m_context = nullptr;
		cubeb_stream* stream = nullptr;
		std::vector<float> m_s16_conversion_buffer;
		bool m_s16_output = false;
	};
} // namespace

static TinyString GetCubebErrorString(int rv)
{
	TinyString ret;
	switch (rv)
	{
		// clang-format off
#define C(e) case e: ret.assign(#e); break
		// clang-format on

		C(CUBEB_OK);
		C(CUBEB_ERROR);
		C(CUBEB_ERROR_INVALID_FORMAT);
		C(CUBEB_ERROR_INVALID_PARAMETER);
		C(CUBEB_ERROR_NOT_SUPPORTED);
		C(CUBEB_ERROR_DEVICE_UNAVAILABLE);

		default:
			return "CUBEB_ERROR_UNKNOWN";

#undef C
	}

	ret.append_format(" ({})", rv);
	return ret;
}

CubebAudioStream::CubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

CubebAudioStream::~CubebAudioStream()
{
	DestroyContextAndStream();
}

void CubebAudioStream::LogCallback(const char* fmt, ...)
{
	SmallString str;
	std::va_list ap;
	va_start(ap, fmt);
	str.vsprintf(fmt, ap);
	va_end(ap);
	DEV_LOG(str);
}

void CubebAudioStream::DestroyContextAndStream()
{
	if (stream)
	{
		cubeb_stream_stop(stream);
		cubeb_stream_destroy(stream);
		stream = nullptr;
	}

	if (m_context)
	{
		cubeb_destroy(m_context);
		m_context = nullptr;
	}
#ifdef _WIN32
	m_coUninit.reset();
#endif
}

bool CubebAudioStream::Initialize(
	const char* driver_name, const char* device_name, AudioSynchronizationMode sync_mode, Error* error)
{
	cubeb_set_log_callback(CUBEB_LOG_NORMAL, LogCallback);

#ifdef _WIN32
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(hr))
	{
		Error::SetHResult(error, "CoInitializeEx() failed: ", hr);
		return false;
	}
	wil::unique_couninitialize_call uninit;
#endif

	int rv = cubeb_init(&m_context, "PCSX2", (driver_name && *driver_name) ? driver_name : nullptr);
	if (rv != CUBEB_OK)
	{
		Error::SetStringFmt(error, "Could not initialize cubeb context: {}", GetCubebErrorString(rv));
		return false;
	}

	const char* backend_id = cubeb_get_backend_id(m_context);
	m_s16_output =
		(backend_id && std::strcmp(backend_id, CUBEB_WASAPI_EXCLUSIVE_BACKEND) == 0);

	static constexpr const std::array<std::pair<cubeb_channel_layout, SampleReader>,
		static_cast<size_t>(AudioExpansionMode::Count)>
		channel_setups = {{
			// Disabled
			{CUBEB_LAYOUT_STEREO, StereoSampleReaderImpl},
			// StereoLFE
			{CUBEB_LAYOUT_STEREO_LFE, &SampleReaderImpl<AudioExpansionMode::StereoLFE, READ_CHANNEL_FRONT_LEFT,
										  READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_LFE>},
			// Quadraphonic
			{CUBEB_LAYOUT_QUAD, &SampleReaderImpl<AudioExpansionMode::Quadraphonic, READ_CHANNEL_FRONT_LEFT,
									READ_CHANNEL_FRONT_RIGHT, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>},
			// QuadraphonicLFE
			{CUBEB_LAYOUT_QUAD_LFE,
				&SampleReaderImpl<AudioExpansionMode::QuadraphonicLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>},
			// Surround51
			{CUBEB_LAYOUT_3F2_LFE_BACK,
				&SampleReaderImpl<AudioExpansionMode::Surround51, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>},
			// Surround71
			{CUBEB_LAYOUT_3F4_LFE,
				&SampleReaderImpl<AudioExpansionMode::Surround71, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
					READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT,
					READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT>},
		}};

	cubeb_stream_params params = {};
	params.format = m_s16_output ? CUBEB_SAMPLE_S16LE : CUBEB_SAMPLE_FLOAT32LE;
	params.rate = m_sample_rate;
	params.channels = m_output_channels;
	params.layout = channel_setups[static_cast<size_t>(m_parameters.expansion_mode)].first;
	params.prefs = CUBEB_STREAM_PREF_NONE;

	u32 minimum_latency_frames = 0;
	rv = cubeb_get_min_latency(m_context, &params, &minimum_latency_frames);
	if (rv == CUBEB_ERROR_NOT_SUPPORTED)
	{
		minimum_latency_frames = 0;
		DEV_LOG("Cubeb backend does not support minimum latency queries.");
	}
	else
	{
		if (rv != CUBEB_OK)
		{
			Error::SetStringFmt(error, "cubeb_get_min_latency() failed: {}", GetCubebErrorString(rv));
			DestroyContextAndStream();
			return false;
		}
	}

	cubeb_devid selected_device = nullptr;
	cubeb_device_collection devices;
	bool devices_valid = false;
	if (device_name && *device_name)
	{
		rv = cubeb_enumerate_devices(m_context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
		devices_valid = (rv == CUBEB_OK);
		if (rv == CUBEB_OK)
		{
			for (size_t i = 0; i < devices.count; i++)
			{
				const cubeb_device_info& di = devices.device[i];
				if (di.device_id && std::strcmp(device_name, di.device_id) == 0)
				{
					INFO_LOG("Using output device '{}' ({}).", di.device_id,
						di.friendly_name ? di.friendly_name : di.device_id);
					selected_device = di.devid;
					minimum_latency_frames = GetCubebDeviceMinimumLatencyFrames(
						di, m_sample_rate, cubeb_get_backend_id(m_context), minimum_latency_frames);
					break;
				}
			}

			if (!selected_device)
			{
				Host::AddIconOSDMessage("AudioDeviceUnavailable", ICON_FA_VOLUME_HIGH,
					fmt::format("Requested audio output device '{}' not found, using default.", device_name),
					Host::OSD_ERROR_DURATION);
			}
		}
		else
		{
			WARNING_LOG("cubeb_enumerate_devices() returned {}, using default device.", GetCubebErrorString(rv));
		}
	}

	u32 latency_frames = GetFrameCountForMS(m_sample_rate, m_parameters.output_latency_ms);
	const bool below_minimum = minimum_latency_frames > 0 && latency_frames < minimum_latency_frames;
	if (minimum_latency_frames > 0)
	{
		DEV_LOG("Minimum latency: {} ms ({} audio frames)",
			GetMSForFramesCeil(m_sample_rate, minimum_latency_frames), minimum_latency_frames);
	}
	if (below_minimum)
	{
		WARNING_LOG("Configured output latency requests {} frames below Cubeb's guaranteed minimum of {} frames.",
			latency_frames, minimum_latency_frames);
	}
	else
	{
		DEV_LOG("Requesting Cubeb latency of {} frames.", latency_frames);
	}

	BaseInitialize(channel_setups[static_cast<size_t>(m_parameters.expansion_mode)].second, sync_mode);
	if (m_s16_output)
	{
		// Cubeb accepts at most 96,000 requested frames. Leave another second
		// for a driver-aligned exclusive buffer without allocating in the callback.
		m_s16_conversion_buffer.resize(static_cast<size_t>(96000 + m_sample_rate) * m_output_channels);
	}

	char stream_name[32];
	std::snprintf(stream_name, sizeof(stream_name), "%p", this);

	rv = cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, selected_device, &params,
		latency_frames, &CubebAudioStream::DataCallback, StateCallback, this);
	// Verified subminimum requests have an effect on cubeb backends
	if (rv != CUBEB_OK && below_minimum && !stream)
	{
		WARNING_LOG("Subminimum Cubeb latency request failed with {}; retrying the guaranteed minimum of {} frames.",
			GetCubebErrorString(rv), minimum_latency_frames);
		latency_frames = minimum_latency_frames;
		rv = cubeb_stream_init(m_context, &stream, stream_name, nullptr, nullptr, selected_device, &params,
			latency_frames, &CubebAudioStream::DataCallback, StateCallback, this);
	}

	if (devices_valid)
		cubeb_device_collection_destroy(m_context, &devices);

	if (rv != CUBEB_OK)
	{
		Error::SetStringFmt(error, "cubeb_stream_init() failed: {}", GetCubebErrorString(rv));
		DestroyContextAndStream();
		return false;
	}

	rv = cubeb_stream_start(stream);
	if (rv != CUBEB_OK)
	{
		Error::SetStringFmt(error, "cubeb_stream_start() failed: {}", GetCubebErrorString(rv));
		DestroyContextAndStream();
		return false;
	}

#ifdef _WIN32
	m_coUninit = std::move(uninit);
#endif
	return true;
}

void CubebAudioStream::StateCallback(cubeb_stream* stream, void* user_ptr, cubeb_state state)
{
	// noop
}

long CubebAudioStream::DataCallback(cubeb_stream* stm, void* user_ptr, const void* input_buffer, void* output_buffer,
	long nframes)
{
	CubebAudioStream* const stream = static_cast<CubebAudioStream*>(user_ptr);
	if (!stream->m_s16_output)
	{
		stream->ReadFrames(static_cast<float*>(output_buffer), static_cast<u32>(nframes));
		return nframes;
	}

	const size_t sample_count = static_cast<size_t>(nframes) * stream->m_output_channels;
	if (sample_count > stream->m_s16_conversion_buffer.size())
	{
		ERROR_LOG("WASAPI exclusive callback requested too many frames: {}", nframes);
		return CUBEB_ERROR;
	}

	float* const input = stream->m_s16_conversion_buffer.data();
	stream->ReadFrames(input, static_cast<u32>(nframes));
	s16* const output = static_cast<s16*>(output_buffer);
	for (size_t i = 0; i < sample_count; i++)
	{
		const float sample = std::clamp(input[i], -1.0f, 1.0f);
		output[i] = static_cast<s16>(std::lrintf(sample * (sample < 0.0f ? 32768.0f : 32767.0f)));
	}
	return nframes;
}

void CubebAudioStream::SetPaused(bool paused)
{
	if (paused == m_paused || !stream)
		return;

	if (paused)
	{
		const int rv = cubeb_stream_stop(stream);
		if (rv != CUBEB_OK)
		{
			ERROR_LOG("Could not pause stream: {}", GetCubebErrorString(rv));
			return;
		}

		AudioStream::SetPaused(true);
		return;
	}

	AudioStream::SetPaused(false);
	const int rv = cubeb_stream_start(stream);
	if (rv != CUBEB_OK)
	{
		AudioStream::SetPaused(true);
		ERROR_LOG("Could not resume stream: {}", GetCubebErrorString(rv));
	}
}

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	const char* driver_name, const char* device_name, AudioSynchronizationMode sync_mode, Error* error)
{
	std::unique_ptr<CubebAudioStream> stream = std::make_unique<CubebAudioStream>(sample_rate, parameters);
	if (!stream->Initialize(driver_name, device_name, sync_mode, error))
		stream.reset();
	return stream;
}

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
	std::vector<std::pair<std::string, std::string>> names;
	names.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"));

	auto cubeb_names = cubeb_get_backend_names();
	for (size_t i = 0; i < cubeb_names.count; i++)
	{
		const char* name = cubeb_names.names[i];
		std::string display_name = name;
		if (std::strcmp(name, CUBEB_IAUDIOCLIENT3_BACKEND) == 0)
			display_name = TRANSLATE_STR("AudioStream", "IAudioClient3 (Low Latency)");
		else if (std::strcmp(name, CUBEB_WASAPI_EXCLUSIVE_BACKEND) == 0)
			display_name = TRANSLATE_STR("AudioStream", "WASAPI (Exclusive)");
		names.emplace_back(name, std::move(display_name));
	}

	return names;
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver)
{
	std::vector<AudioStream::DeviceInfo> ret;
	ret.emplace_back(std::string(), TRANSLATE_STR("AudioStream", "Default"), 0);

#ifdef _WIN32
	// For enumeration, we need *any* COM context. multi- or single-threaded.
	// Cubeb theoretically wants an MTA context, but this is only relevant when creating streams,
	// which enumerating devices does not do.
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (hr == RPC_E_CHANGED_MODE)
	{
		hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	}
	if (FAILED(hr))
	{
		ERROR_LOG("CoInitializeEx failed: {}", Error::CreateHResult(hr).GetDescription());
		return ret;
	}
	wil::unique_couninitialize_call uninit;
#endif

	cubeb* context;
	int rv = cubeb_init(&context, "PCSX2", (driver && *driver) ? driver : nullptr);
	if (rv != CUBEB_OK)
	{
		ERROR_LOG("cubeb_init() failed: {}", GetCubebErrorString(rv));
		return ret;
	}

	ScopedGuard context_cleanup([context]() { cubeb_destroy(context); });

	cubeb_device_collection devices;
	rv = cubeb_enumerate_devices(context, CUBEB_DEVICE_TYPE_OUTPUT, &devices);
	if (rv != CUBEB_OK)
	{
		ERROR_LOG("cubeb_enumerate_devices() failed: {}", GetCubebErrorString(rv));
		return ret;
	}

	ScopedGuard devices_cleanup([context, &devices]() { cubeb_device_collection_destroy(context, &devices); });

	// we need stream parameters to query latency
	cubeb_stream_params params = {};
	const char* backend_id = cubeb_get_backend_id(context);
	params.format = (backend_id && std::strcmp(backend_id, CUBEB_WASAPI_EXCLUSIVE_BACKEND) == 0) ?
	                    CUBEB_SAMPLE_S16LE :
	                    CUBEB_SAMPLE_FLOAT32LE;
	params.rate = 48000;
	params.channels = 2;
	params.layout = CUBEB_LAYOUT_UNDEFINED;
	params.prefs = CUBEB_STREAM_PREF_NONE;

	u32 min_latency = 0;
	cubeb_get_min_latency(context, &params, &min_latency);
	ret[0].minimum_latency_frames = min_latency;
	for (size_t i = 0; i < devices.count; i++)
	{
		const cubeb_device_info& di = devices.device[i];
		if (!di.device_id)
			continue;

		ret.emplace_back(di.device_id, di.friendly_name ? di.friendly_name : di.device_id,
			GetCubebDeviceMinimumLatencyFrames(di, params.rate, backend_id, min_latency));
	}

	return ret;
}
