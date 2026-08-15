#include "gargantuan/network/Replication.hpp"

#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <type_traits>

namespace gargantuan::network {
	bool ReplicationView::IsValid() const {
		if (!Connection.IsValid() || !Epoch.IsValid()) return false;
		for (const auto Object : KnownObjects) if (!Object.IsValid()) return false;
		for (const auto Object : RelevantObjects) if (!Object.IsValid()) return false;
		for (const auto &[State, Sequence] : LatestStateSequences)
			if (!State.Object.IsValid() || !State.Channel.IsValid() || !Sequence.IsValid() ||
				!KnownObjects.contains(State.Object)) return false;
		return true;
	}

	void ReplicationView::ForgetReplica(ObjectId Object) {
		KnownObjects.erase(Object);
		std::erase_if(LatestStateSequences,
			[&](const auto &Entry) { return Entry.first.Object == Object; });
	}

	bool ReplicationOperation::IsValid() const {
		if (!Epoch.IsValid()) return false;
		try {
			return std::visit([](const auto &Value) {
				using Type = std::decay_t<decltype(Value)>;
				if (!Value.Object.IsValid()) return false;
				if constexpr (std::is_same_v<Type, PublishReplication>) {
					return Value.ClassSchemaId.IsValid() && Value.DefinitionVersion != 0 &&
						(!Value.Parent || Value.Parent->IsValid());
				} else if constexpr (std::is_same_v<Type, PropertyReplicationUpdate>) {
					ValidateProtocolString(Value.PropertyName, MaximumProtocolIdentifierBytes, "Replication property name");
					ValidateProtocolWireValue(Value.Value);
					const bool HasSchema = Value.DeclaringClassSchemaId.has_value();
					const bool HasVersion = Value.DefinitionVersion.has_value();
					return HasSchema == HasVersion && (!HasSchema ||
						(Value.DeclaringClassSchemaId->IsValid() && *Value.DefinitionVersion != 0));
				} else if constexpr (std::is_same_v<Type, ExtensionPropertyReplicationUpdate>) {
					ValidateProtocolString(Value.PropertyName, MaximumProtocolIdentifierBytes, "Replication extension property name");
					ValidateProtocolWireValue(Value.Value);
					return Value.ExtensionSchemaId.IsValid() && Value.DefinitionVersion != 0;
				} else if constexpr (std::is_same_v<Type, ReparentReplication>) {
					return !Value.Parent || Value.Parent->IsValid();
				} else if constexpr (std::is_same_v<Type, AttributeReplicationUpdate>) {
					ValidateAttributeName(Value.AttributeName);
					if (Value.Value) (void)ValidateAttributeValue(*Value.Value);
					return true;
				} else if constexpr (std::is_same_v<Type, TagAddedReplication> ||
					std::is_same_v<Type, TagRemovedReplication>) {
					ValidateTagName(Value.TagName);
					return true;
				} else if constexpr (std::is_same_v<Type, UnpublishReplication>) {
					return true;
				}
				return false;
			}, Intent);
		} catch (...) {
			return false;
		}
	}
}
