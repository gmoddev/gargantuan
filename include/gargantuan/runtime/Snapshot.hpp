#pragma once

#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	class Instance;

	inline constexpr std::uint32_t SnapshotFormatVersion = 6;

	struct SnapshotExtensionState {
		SchemaId ExtensionSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::map<std::string, WireValue> Properties;
	};

	struct SnapshotCustomClassState {
		SchemaId DeclaringClassSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::map<std::string, WireValue> Properties;
	};

	struct SnapshotObject {
		WireObjectId Id;
		SchemaId ClassSchemaId;
		std::uint32_t ClassDefinitionVersion = 0;
		std::string ClassName;
		std::string Name;
		std::optional<WireObjectId> Parent;
		std::map<std::string, WireValue> Properties;
		std::map<std::string, WireValue> Attributes;
		std::vector<SnapshotExtensionState> Extensions;
		std::vector<SnapshotCustomClassState> CustomProperties;
		std::vector<std::string> Tags;
	};

	struct Snapshot {
		std::uint32_t Version = SnapshotFormatVersion;
		ChangeCursor Cursor;
		std::vector<SnapshotObject> Objects;
	};

	struct SnapshotParseResult {
		std::optional<Snapshot> Value;
		std::vector<std::string> Errors;
		[[nodiscard]] bool Succeeded() const { return Value.has_value(); }
	};

	struct SnapshotLoadResult {
		std::shared_ptr<Instance> Root;
		std::unordered_map<WireObjectId, std::shared_ptr<Instance>> Objects;
		std::vector<std::string> Errors;
		[[nodiscard]] bool Succeeded() const { return Root != nullptr && Errors.empty(); }
		[[nodiscard]] std::shared_ptr<Instance> Resolve(WireObjectId id) const;
	};

	Snapshot CaptureSnapshot(const std::shared_ptr<Instance> &root);
	std::string SerializeSnapshot(const Snapshot &snapshot);
	SnapshotParseResult DeserializeSnapshot(std::string_view serialized);
	SnapshotLoadResult LoadSnapshot(const Snapshot &snapshot);
}
