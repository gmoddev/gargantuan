#pragma once

#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	inline constexpr std::uint32_t WireJournalFormatVersion = 5;

	enum class WireJournalOperation {
		Create,
		PropertyUpdate,
		AttributeUpdate,
		ExtensionPropertyUpdate,
		TagAdded,
		TagRemoved,
		Reparent,
		Destroy
	};

	struct WireJournalRecord {
		std::uint32_t Version = WireJournalFormatVersion;
		std::uint64_t Sequence = 0;
		WireObjectId Scope;
		WireJournalOperation Operation = WireJournalOperation::Create;
		WireObjectId Object;
		std::optional<WireObjectId> Parent;
		std::optional<std::string> ClassName;
		std::optional<std::string> PropertyName;
		std::optional<std::string> AttributeName;
		std::optional<SchemaId> ExtensionSchemaId;
		std::optional<std::uint32_t> DefinitionVersion;
		std::optional<std::string> ExtensionPropertyName;
		std::optional<std::string> TagName;
		std::optional<WireValue> Value;
	};

	struct WireJournalParseResult {
		std::optional<std::vector<WireJournalRecord>> Value;
		std::vector<std::string> Errors;
		[[nodiscard]] bool Succeeded() const { return Value.has_value(); }
	};

	WireJournalRecord EncodeChangeRecord(const ChangeRecord &record);
	std::string SerializeWireJournalRecords(const std::vector<WireJournalRecord> &records);
	WireJournalParseResult DeserializeWireJournalRecords(std::string_view serialized);
}
