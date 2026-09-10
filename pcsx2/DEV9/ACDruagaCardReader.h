#pragma once

#include "MemoryTypes.h"

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace ACDruagaCardReader
{
	void Reset();
	void WriteByte(u8 value);
	std::optional<std::vector<u8>> TakeReply();
	bool InsertCard(u8 number);
} // namespace ACDruagaCardReader
