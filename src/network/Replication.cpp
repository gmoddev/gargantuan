#include "gargantuan/network/Replication.hpp"

#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <set>
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
					ValidateProtocolString(Value.ClassName, MaximumProtocolIdentifierBytes, "Replication class name");
					ValidateProtocolString(Value.Name, MaximumProtocolStringBytes, "Replication object name");
					if (Value.Properties.size() > MaximumSnapshotPropertiesPerObject ||
						Value.Attributes.size() > MaximumAttributesPerInstance ||
						Value.Extensions.size() > MaximumCustomExtensionDefinitions ||
						Value.CustomProperties.size() > MaximumCustomClassDefinitions ||
						Value.Tags.size() > MaximumTagsPerInstance) return false;
					for (const auto &[Name, PropertyValue] : Value.Properties) {
						ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Replication property name");
						ValidateProtocolWireValue(PropertyValue);
					}
					(void)ValidateAttributeCollection(Value.Attributes);
					std::set<SchemaId> UniqueExtensions;
					for (const auto &State : Value.Extensions) {
						if (!State.ExtensionSchemaId.IsValid() || State.DefinitionVersion == 0 ||
							State.Properties.size() > MaximumExtensionOverridesPerInstance ||
							!UniqueExtensions.insert(State.ExtensionSchemaId).second) return false;
						for (const auto &[Name, PropertyValue] : State.Properties) {
							ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Replication extension property name");
							ValidateProtocolWireValue(PropertyValue);
						}
					}
					std::set<SchemaId> UniqueCustomClasses;
					for (const auto &State : Value.CustomProperties) {
						if (!State.DeclaringClassSchemaId.IsValid() || State.DefinitionVersion == 0 ||
							State.Properties.size() > MaximumCustomPropertyOverridesPerInstance ||
							!UniqueCustomClasses.insert(State.DeclaringClassSchemaId).second) return false;
						for (const auto &[Name, PropertyValue] : State.Properties) {
							ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Replication custom property name");
							ValidateProtocolWireValue(PropertyValue);
						}
					}
					std::set<std::string> UniqueTags;
					for (const auto &Tag : Value.Tags) {
						ValidateTagName(Tag);
						if (!UniqueTags.insert(Tag).second) return false;
					}
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
				} else if constexpr (std::is_same_v<Type, UnpublishReplication> ||
					std::is_same_v<Type, DestroyReplication>) {
					return true;
				}
				return false;
			}, Intent);
		} catch (...) {
			return false;
		}
	}
}
