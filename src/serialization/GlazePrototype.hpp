#pragma once

#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace gargantuan::GlazePrototype {
	SerializationResult<std::string> EncodeSnapshot(const Snapshot &Value);
	SerializationResult<Snapshot> DecodeSnapshot(std::string_view Encoded);
	SerializationResult<std::string> EncodeJournal(const std::vector<WireJournalRecord> &Value);
	SerializationResult<std::vector<WireJournalRecord>> DecodeJournal(std::string_view Encoded);
}
