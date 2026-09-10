#include "ACDruagaCardReader.h"

#include "VMManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ACDruagaCardReader
{
static constexpr size_t CARD_SIZE = 0x300;
static constexpr size_t MAX_FRAME_SIZE = 64;

enum class Command : u8
{
	PrepareAccess = 0x40,
	Read16 = 0x41,
	Write16 = 0x42,
	Control = 0x44,
	FinishAccess = 0x47,
	Read48 = 0x48,
	Write48 = 0x49,
	Configure = 0x50,
	Initialize = 0x56,
	PollStatus = 0x80,
	TransportStart = 0x81,
	TransportContinue = 0x82,
	Identify = 0x83,
};

enum class CardPhase
{
	Absent,
	TransportRequested,
	TransportContinuing,
	Ready,
	Releasing,
};

struct Request
{
	Command command;
	std::vector<u8> payload;
};

static std::vector<u8> s_tx;
static std::deque<std::vector<u8>> s_replies;
static std::array<u8, CARD_SIZE> s_card{};
static std::string s_card_path;
static FileSystem::ManagedCFilePtr s_card_file;
static CardPhase s_phase = CardPhase::Absent;
static bool s_card_mounted = false;

static std::vector<u8> SerializeFrame(Command command, const u8* payload, size_t payload_size)
{
	std::vector<u8> frame;
	frame.reserve(payload_size + 7);
	frame.push_back(0x02);
	frame.push_back(static_cast<u8>(command));
	frame.push_back(static_cast<u8>(payload_size));
	frame.insert(frame.end(), payload, payload + payload_size);
	frame.push_back(0x03);
	u8 checksum = 0;
	for (size_t i = 1; i < frame.size(); i++)
		checksum ^= frame[i];
	frame.insert(frame.end(), {checksum, 0x0d, 0x0a});
	return frame;
}

template <size_t N>
static void QueueReply(Command command, const std::array<u8, N>& payload)
{
	s_replies.push_back(SerializeFrame(command, payload.data(), payload.size()));
}

static bool HasPayloadSize(const Request& request, size_t expected)
{
	if (request.payload.size() == expected)
		return true;
	Console.Error("ACUART: Druaga reader command 0x%02x has %zu payload bytes; expected %zu",
		static_cast<u8>(request.command), request.payload.size(), expected);
	return false;
}

static int CardFileDescriptor()
{
	if (!s_card_file)
		return -1;
#ifdef _WIN32
	return _fileno(s_card_file.get());
#else
	return fileno(s_card_file.get());
#endif
}

static bool ReadCardFile()
{
	const int fd = CardFileDescriptor();
	if (fd < 0)
		return false;

	size_t completed = 0;
	while (completed < s_card.size())
	{
#ifdef _WIN32
		if (_lseeki64(fd, static_cast<s64>(completed), SEEK_SET) < 0)
			return false;
		const int result = _read(fd, s_card.data() + completed, static_cast<unsigned int>(s_card.size() - completed));
#else
		const ssize_t result = pread(fd, s_card.data() + completed, s_card.size() - completed, static_cast<off_t>(completed));
#endif
		if (result > 0)
		{
			completed += static_cast<size_t>(result);
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		return false;
	}
	return true;
}

static bool WriteCardRange(size_t offset, const u8* data, size_t size)
{
	const int fd = CardFileDescriptor();
	if (fd < 0 || offset > CARD_SIZE || size > CARD_SIZE - offset)
		return false;

	size_t completed = 0;
	while (completed < size)
	{
#ifdef _WIN32
		if (_lseeki64(fd, static_cast<s64>(offset + completed), SEEK_SET) < 0)
			break;
		const int result = _write(fd, data + completed, static_cast<unsigned int>(size - completed));
#else
		const ssize_t result = pwrite(fd, data + completed, size - completed, static_cast<off_t>(offset + completed));
#endif
		if (result > 0)
		{
			completed += static_cast<size_t>(result);
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		break;
	}

	const bool written = completed == size;
	if (!written)
		Console.Error("ACUART: Druaga reader failed to write %zu bytes at card offset 0x%zx in '%s'",
			size, offset, s_card_path.c_str());
	return written;
}

static void UnmountCard()
{
	s_card_file.reset();
	s_card_path.clear();
	s_card_mounted = false;
	s_phase = CardPhase::Absent;
}

static u32 ReadCardU32(size_t offset)
{
	return static_cast<u32>(s_card[offset]) |
		(static_cast<u32>(s_card[offset + 1]) << 8) |
		(static_cast<u32>(s_card[offset + 2]) << 16) |
		(static_cast<u32>(s_card[offset + 3]) << 24);
}

static void QueueOperationReply(Command command, bool success)
{
	QueueReply(command, std::array<u8, 2>{success ? u8{0x00} : u8{0x02}, 0x00});
}

template <size_t N>
static std::optional<size_t> BlockOffset(u8 block)
{
	const size_t offset = static_cast<size_t>(block) * N;
	return (offset <= s_card.size() && N <= s_card.size() - offset) ? std::optional<size_t>(offset) : std::nullopt;
}

template <size_t N>
static void ReadBlock(const Request& request)
{
	if (!HasPayloadSize(request, 1))
		return;
	std::array<u8, N + 2> payload{};
	const std::optional<size_t> offset = BlockOffset<N>(request.payload[0]);
	if (!s_card_mounted || !offset.has_value())
		payload[0] = 0x02;
	else
		std::memcpy(payload.data() + 2, s_card.data() + *offset, N);
	QueueReply(request.command, payload);
}

template <size_t N>
static void WriteBlock(const Request& request)
{
	if (!HasPayloadSize(request, N + 1))
		return;
	const std::optional<size_t> offset = BlockOffset<N>(request.payload[0]);
	if (!s_card_mounted || !offset.has_value())
	{
		QueueOperationReply(request.command, false);
		return;
	}
	const u8* const data = request.payload.data() + 1;
	const bool written = WriteCardRange(*offset, data, N);
	if (written)
		std::memcpy(s_card.data() + *offset, data, N);
	QueueOperationReply(request.command, written);
}

static void HandleRequest(const Request& request)
{
	switch (request.command)
	{
		case Command::Identify:
			if (HasPayloadSize(request, 0))
				QueueReply(request.command, std::array<u8, 4>{'3', '5', '0', '0'});
			break;
		case Command::Initialize:
			if (HasPayloadSize(request, 0))
				QueueReply(request.command, std::array<u8, 2>{0x00, 0x00});
			break;
		case Command::Configure:
			if (HasPayloadSize(request, 13))
				QueueReply(request.command, std::array<u8, 2>{0x00, 0x00});
			break;
		case Command::PollStatus:
			if (HasPayloadSize(request, 0))
			{
				const u8 status = (s_phase == CardPhase::Absent) ? 0x00 :
					((s_phase == CardPhase::Ready || s_phase == CardPhase::Releasing) ? 0x02 : 0x01);
				QueueReply(request.command, std::array<u8, 1>{status});
			}
			break;
		case Command::TransportStart:
			if (HasPayloadSize(request, 1))
			{
				if (request.payload[0] == '1' && s_phase == CardPhase::TransportRequested)
					s_phase = CardPhase::TransportContinuing;
				else if (request.payload[0] == '6' && s_card_mounted)
					s_phase = CardPhase::Releasing;
				QueueReply(request.command, std::array<u8, 1>{'1'});
			}
			break;
		case Command::TransportContinue:
			if (HasPayloadSize(request, 1))
			{
				if (request.payload[0] == '1' && s_phase == CardPhase::TransportContinuing)
					s_phase = CardPhase::Ready;
				else if (request.payload[0] == '1' && s_phase == CardPhase::Releasing)
					UnmountCard();
				QueueReply(request.command, std::array<u8, 1>{'1'});
			}
			break;
		case Command::PrepareAccess:
			if (HasPayloadSize(request, 4))
				QueueOperationReply(request.command, s_card_mounted);
			break;
		case Command::FinishAccess:
			if (HasPayloadSize(request, 0))
				QueueOperationReply(request.command, s_card_mounted);
			break;
		case Command::Read16:
			ReadBlock<16>(request);
			break;
		case Command::Read48:
			ReadBlock<48>(request);
			break;
		case Command::Write16:
			WriteBlock<16>(request);
			break;
		case Command::Write48:
			WriteBlock<48>(request);
			break;
		case Command::Control:
		{
			if (!HasPayloadSize(request, 5) || !s_card_mounted || request.payload[0] != 2)
			{
				QueueOperationReply(request.command, false);
				break;
			}
			const u32 amount = static_cast<u32>(request.payload[1]) |
				(static_cast<u32>(request.payload[2]) << 8) |
				(static_cast<u32>(request.payload[3]) << 16) |
				(static_cast<u32>(request.payload[4]) << 24);
			u32 generation = ReadCardU32(0x20);
			u32 complement = ReadCardU32(0x24);
			const u32 repeated = ReadCardU32(0x28);
			if (generation != repeated || generation != ~complement)
			{
				QueueOperationReply(request.command, false);
				break;
			}
			std::array<u8, 12> header{};
			std::memcpy(header.data(), s_card.data() + 0x20, header.size());
			generation -= amount;
			complement = ~generation;
			const auto write_u32 = [&header](size_t offset, u32 value) {
				header[offset] = static_cast<u8>(value);
				header[offset + 1] = static_cast<u8>(value >> 8);
				header[offset + 2] = static_cast<u8>(value >> 16);
				header[offset + 3] = static_cast<u8>(value >> 24);
			};
			write_u32(0, generation);
			write_u32(4, complement);
			std::copy_n(header.data(), 4, header.data() + 8);
			const bool written = WriteCardRange(0x20, header.data(), header.size());
			if (written)
				std::memcpy(s_card.data() + 0x20, header.data(), header.size());
			QueueOperationReply(request.command, written);
			break;
		}
		default:
			Console.Error("ACUART: Druaga reader command 0x%02x is not implemented", static_cast<u8>(request.command));
			break;
	}
}

void Reset()
{
	s_tx.clear();
	s_replies.clear();
	s_card.fill(0);
	UnmountCard();
}

void WriteByte(u8 value)
{
	if (s_tx.empty() && value != 0x02)
		return;
	s_tx.push_back(value);
	if (s_tx.size() < 3)
		return;
	const size_t frame_size = static_cast<size_t>(s_tx[2]) + 7;
	if (frame_size > MAX_FRAME_SIZE)
	{
		Console.Error("ACUART: Druaga reader rejected oversized frame (%zu bytes)", frame_size);
		s_tx.clear();
		return;
	}
	if (s_tx.size() < frame_size)
		return;

	const size_t delimiter = frame_size - 4;
	u8 checksum = 0;
	for (size_t i = 1; i <= delimiter; i++)
		checksum ^= s_tx[i];
	const bool valid = s_tx[delimiter] == 0x03 && s_tx[delimiter + 1] == checksum &&
		s_tx[delimiter + 2] == 0x0d && s_tx[delimiter + 3] == 0x0a;
	if (!valid)
	{
		Console.Error("ACUART: Druaga reader rejected an invalid frame");
		s_tx.clear();
		return;
	}
	Request request{static_cast<Command>(s_tx[1]), std::vector<u8>(s_tx.begin() + 3, s_tx.begin() + delimiter)};
	s_tx.clear();
	HandleRequest(request);
}

std::optional<std::vector<u8>> TakeReply()
{
	if (s_replies.empty())
		return std::nullopt;
	std::vector<u8> reply = std::move(s_replies.front());
	s_replies.pop_front();
	return reply;
}

bool InsertCard(u8 number)
{
	if (number > 9 || s_phase != CardPhase::Absent)
	{
		Console.Warning("ACUART: Druaga reader cannot insert card%u while another card is active", number);
		return false;
	}
	const std::string game_directory = VMManager::GetArcadeGameDataDirectory();
	if (game_directory.empty())
	{
		Console.Error("ACUART: Druaga reader cannot insert a card without an active arcade game data directory");
		return false;
	}

	const std::string directory = Path::Combine(game_directory, "cards");
	const std::string path = Path::Combine(directory, "card" + std::to_string(number) + ".bin");
	FileSystem::ManagedCFilePtr file = FileSystem::OpenManagedSharedCFile(
		path.c_str(), "r+b", FileSystem::FileShareMode::DenyReadWrite);
	if (!file)
	{
		Console.Error("ACUART: Druaga reader cannot exclusively open IC card '%s'", path.c_str());
		return false;
	}

#ifndef _WIN32
	struct flock lock = {};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (fcntl(fileno(file.get()), F_SETLK, &lock) != 0)
	{
		Console.Error("ACUART: Druaga reader cannot lock IC card '%s'", path.c_str());
		return false;
	}
#endif

	if (FileSystem::FSize64(file.get()) != CARD_SIZE)
	{
		Console.Error("ACUART: Druaga IC card '%s' does not have the required %zu bytes", path.c_str(), CARD_SIZE);
		return false;
	}

	s_card_path = path;
	s_card_file = std::move(file);
	if (!ReadCardFile())
	{
		Console.Error("ACUART: Druaga reader cannot read IC card '%s'", path.c_str());
		UnmountCard();
		return false;
	}
	s_card_mounted = true;
	s_phase = CardPhase::TransportRequested;
	return true;
}
} // namespace ACDruagaCardReader
