#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

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
		[[nodiscard]] bool Knows(ObjectId Object) const { return KnownObjects.contains(Object); }
		void ForgetReplica(ObjectId Object);
	};

	struct PublishReplication {
		ObjectId Object;
		SchemaId ClassSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::optional<ObjectId> Parent;
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

	struct TagAddedReplication { ObjectId Object; std::string TagName; };
	struct TagRemovedReplication { ObjectId Object; std::string TagName; };
	struct UnpublishReplication { ObjectId Object; };

	using ReplicationIntent = std::variant<
		PublishReplication,
		PropertyReplicationUpdate,
		ExtensionPropertyReplicationUpdate,
		ReparentReplication,
		AttributeReplicationUpdate,
		TagAddedReplication,
		TagRemovedReplication,
		UnpublishReplication
	>;

	struct ReplicationOperation {
		ReplicationEpoch Epoch;
		ReplicationIntent Intent;

		[[nodiscard]] bool IsValid() const;
	};
}
