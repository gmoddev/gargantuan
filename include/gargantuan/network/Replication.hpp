#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace gargantuan::network {
	struct ReplicaStateChannel {
		ObjectId Object;
		StateChannelId Channel;
		auto operator<=>(const ReplicaStateChannel &) const = default;
	};

	struct ReplicationView {
		ConnectionId Connection;
		ReplicationEpoch Epoch;
		std::unordered_set<ObjectId> KnownObjects;
		std::unordered_set<ObjectId> RelevantObjects;
		std::map<ReplicaStateChannel, RealtimeStateSequence> LatestStateSequences;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool Knows(ObjectId Object) const {
			return KnownObjects.contains(Object);
		}
		void ForgetReplica(ObjectId Object);
	};

	struct PublishReplication {
		ObjectId Object;
		SchemaId ClassSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::optional<ObjectId> Parent;
		std::string ClassName;
		std::string Name;
		std::map<std::string, WireValue> Properties;
		std::map<std::string, WireValue> Attributes;
		std::vector<SnapshotExtensionState> Extensions;
		std::vector<SnapshotCustomClassState> CustomProperties;
		std::vector<std::string> Tags;
	};

	struct StructuralMaterializationTemplateKey {
		ObjectId World;
		ObjectId Object;
		std::uint64_t StructuralRevision = 0;
		auto operator<=>(const StructuralMaterializationTemplateKey &) const = default;
	};

	struct StructuralMaterializationTemplate {
		StructuralMaterializationTemplateKey Key;
		PublishReplication Publication;
		std::size_t RetainedBytes = 0;
	};

	struct StructuralPropertyPatchList {
		static constexpr std::size_t InlineCapacity = 2;

	  private:
		std::array<std::string_view, InlineCapacity> Inline{};
		std::vector<std::string_view> Overflow;
		std::size_t Count = 0;

	  public:
		void Add(std::string_view Name) {
			if (Count < InlineCapacity)
				Inline[Count] = Name;
			else
				Overflow.push_back(Name);
			++Count;
		}
		[[nodiscard]] std::size_t Size() const {
			return Count;
		}
		[[nodiscard]] std::string_view operator[](std::size_t Index) const {
			return Index < InlineCapacity ? Inline[Index] : Overflow[Index - InlineCapacity];
		}
		[[nodiscard]] bool Contains(std::string_view Name) const {
			for (std::size_t Index = 0; Index < Count; ++Index)
				if ((*this)[Index] == Name) return true;
			return false;
		}
	};

	// This server-local operation is encoded as an ordinary PublishReplication.
	// Peer-specific nil patches remain outside the immutable shared template.
	struct PreparedPublishReplication {
		ObjectId Object;
		std::shared_ptr<const StructuralMaterializationTemplate> Template;
		StructuralPropertyPatchList NilProperties;
	};

	struct PropertyReplicationUpdate {
		ObjectId Object;
		std::string PropertyName;
		WireValue Value;
		std::optional<SchemaId> DeclaringClassSchemaId;
		std::optional<std::uint32_t> DefinitionVersion;
	};

	struct ExtensionPropertyReplicationUpdate {
		ObjectId Object;
		SchemaId ExtensionSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Value;
	};

	struct ReparentReplication {
		ObjectId Object;
		std::optional<ObjectId> Parent;
	};

	struct AttributeReplicationUpdate {
		ObjectId Object;
		std::string AttributeName;
		std::optional<WireValue> Value;
	};

	struct TagAddedReplication {
		ObjectId Object;
		std::string TagName;
	};
	struct TagRemovedReplication {
		ObjectId Object;
		std::string TagName;
	};
	struct UnpublishReplication {
		ObjectId Object;
	};
	struct DestroyReplication {
		ObjectId Object;
	};

	using ReplicationIntent = std::variant<
		PublishReplication,
		PreparedPublishReplication,
		PropertyReplicationUpdate,
		ExtensionPropertyReplicationUpdate,
		ReparentReplication,
		AttributeReplicationUpdate,
		TagAddedReplication,
		TagRemovedReplication,
		UnpublishReplication,
		DestroyReplication>;

	struct ReplicationOperation {
		ReplicationEpoch Epoch;
		ReplicationIntent Intent;

		[[nodiscard]] bool IsValid() const;
	};

	[[nodiscard]] bool IsPublishReplication(const ReplicationIntent &Intent);
	[[nodiscard]] bool IsValidPublishReplication(const PublishReplication &Publish) noexcept;
	[[nodiscard]] ObjectId GetReplicationObject(const ReplicationIntent &Intent);
}
