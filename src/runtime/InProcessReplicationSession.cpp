#include "gargantuan/runtime/InProcessReplicationSession.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <exception>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gargantuan {
	namespace {
		void CollectSubtree(const std::shared_ptr<Instance> &instance, std::unordered_set<Instance *> &subtree) {
			if (!instance || !subtree.insert(instance.get()).second) return;
			for (const auto &child : instance->Children) CollectSubtree(child, subtree);
		}
	}

	ReplicationSessionStartResult InProcessReplicationSession::Start(const std::shared_ptr<Instance> &sourceRoot) {
		AssertAuthoritativeMutation("InProcessReplicationSession::Start");
		ReplicationSessionStartResult result;
		try {
			auto snapshot = CaptureSnapshot(sourceRoot);
			auto parsed = DeserializeSnapshot(SerializeSnapshot(snapshot));
			if (!parsed.Succeeded()) {
				result.Errors = std::move(parsed.Errors);
				return result;
			}

			SnapshotLoadResult receiver;
			{
				ScopedChangeJournalSuppression suppressReceiverChanges;
				receiver = LoadSnapshot(*parsed.Value);
			}
			if (!receiver.Succeeded()) {
				result.Errors = std::move(receiver.Errors);
				return result;
			}

			auto session = std::make_shared<InProcessReplicationSession>();
			session->Cursor = parsed.Value->Cursor;
			session->Receiver = std::move(receiver);
			for (const auto &object : parsed.Value->Objects) session->TrackedObjects.insert(object.Id);
			result.Session = std::move(session);
		} catch (const std::exception &error) {
			result.Errors.push_back(error.what());
		}
		return result;
	}

	std::shared_ptr<Instance> InProcessReplicationSession::ResolveReceiver(WireObjectId id) const {
		if (!TrackedObjects.contains(id) && !CandidateObjects.contains(id)) return nullptr;
		return Receiver.Resolve(id);
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyAvailable(std::size_t maximumRecords) {
		AssertAuthoritativeMutation("InProcessReplicationSession::ApplyAvailable");
		auto changes = ChangeJournal::Get().Read(Cursor, maximumRecords);
		if (changes.Status == ChangeReadStatus::ResnapshotRequired) {
			return {ReplicationApplyStatus::ResnapshotRequired, 0, "Source journal cursor was evicted"};
		}
		if (changes.Records.empty()) return {ReplicationApplyStatus::NoChanges, 0, {}};

		std::vector<WireJournalRecord> wireRecords;
		wireRecords.reserve(changes.Records.size());
		for (const auto &record : changes.Records) wireRecords.push_back(EncodeChangeRecord(record));
		auto parsed = DeserializeWireJournalRecords(SerializeWireJournalRecords(wireRecords));
		if (!parsed.Succeeded()) {
			return {
				ReplicationApplyStatus::MalformedRecord,
				0,
				parsed.Errors.empty() ? "Wire journal round trip failed" : parsed.Errors.front(),
			};
		}
		return ApplyWireRecords(*parsed.Value);
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyWireRecords(
		const std::vector<WireJournalRecord> &records
	) {
		AssertAuthoritativeMutation("InProcessReplicationSession::ApplyWireRecords");
		if (records.empty()) return {ReplicationApplyStatus::NoChanges, 0, {}};
		auto ExpectedSequence = Cursor.NextSequence;
		for (const auto &record : records) {
			if (record.Version != WireJournalFormatVersion || !record.Scope.IsValid() ||
				!record.Object.IsValid() || record.Sequence == 0)
				return {ReplicationApplyStatus::MalformedRecord, 0, "Malformed or unsupported wire journal record"};
			if (record.Scope.ToObjectId() != Cursor.Scope)
				return {ReplicationApplyStatus::ApplyRejected, 0, "Wire journal record belongs to another replication scope"};
			if (record.Sequence < ExpectedSequence)
				return {ReplicationApplyStatus::DuplicateRecord, 0, "Duplicate wire journal sequence"};
			if (record.Sequence > ExpectedSequence)
				return {ReplicationApplyStatus::OutOfOrderRecord, 0, "Out-of-order wire journal sequence"};
			++ExpectedSequence;
		}
		if (auto Preflight = PreflightCustomClassRecords(records); !Preflight.Succeeded()) return Preflight;
		std::size_t applied = 0;
		for (const auto &record : records) {
			auto recordResult = ApplyRecord(record);
			if (!recordResult.Succeeded()) {
				recordResult.AppliedRecords = applied;
				return recordResult;
			}
			++Cursor.NextSequence;
			++applied;
		}
		return {ReplicationApplyStatus::Success, applied, {}};
	}

	ReplicationApplyResult InProcessReplicationSession::PreflightCustomClassRecords(
		const std::vector<WireJournalRecord> &records
	) const {
		using CustomPropertyState = std::map<SchemaId, std::map<std::string, WireValue>>;
		std::unordered_map<WireObjectId, SchemaId> ClassIds;
		std::unordered_map<WireObjectId, std::optional<WireObjectId>> Parents;
		std::unordered_map<WireObjectId, CustomPropertyState> CustomPropertyStates;
		std::unordered_map<const Instance *, WireObjectId> IdsByInstance;
		for (const auto &[Object, Instance] : Receiver.Objects) IdsByInstance.emplace(Instance.get(), Object);
		for (const auto &[Object, Instance] : Receiver.Objects) {
			auto *Definition = InstanceClassRegistry::GetDefinition(Instance.get());
			if (Definition) ClassIds.emplace(Object, Definition->Id);
			CustomPropertyStates.emplace(Object, Instance->GetCustomClassPropertyOverrides(
				ScriptSecurityContext::CoreTrusted()
			));
			std::optional<WireObjectId> Parent;
			if (auto Owner = Instance->GetParent()) {
				auto Found = IdsByInstance.find(Owner->get());
				if (Found != IdsByInstance.end()) Parent = Found->second;
			}
			Parents.emplace(Object, Parent);
		}
		const auto &Schema = GetActiveRuntimeSchemaRegistry();
		for (const auto &Record : records) {
			if (Record.Operation == WireJournalOperation::Create) {
				if (!Record.ClassName || !Record.ClassSchemaId || !Record.DefinitionVersion || Record.Parent ||
					Record.PropertyName || Record.DeclaringClassSchemaId || Record.AttributeName ||
					Record.ExtensionSchemaId || Record.ExtensionPropertyName || Record.TagName || Record.Value)
					return {ReplicationApplyStatus::MalformedRecord, 0, "Invalid Create record"};
				auto *Definition = Schema.FindClassById(*Record.ClassSchemaId);
				const auto ExpectedName = Definition && Definition->ConstructionKind == SchemaClassConstructionKind::CustomData
					? Definition->CanonicalName : Definition ? Definition->ClassName : std::string{};
				if (!Definition || Definition->DefinitionVersion != *Record.DefinitionVersion ||
					ExpectedName != *Record.ClassName || !Schema.IsClassConstructible(*Definition))
					return {ReplicationApplyStatus::ApplyRejected, 0,
						"Create class identity is unknown or incompatible"};
				if (!ClassIds.emplace(Record.Object, Definition->Id).second)
					return {ReplicationApplyStatus::MalformedRecord, 0, "Create record contains a duplicate ObjectId"};
				Parents.emplace(Record.Object, std::nullopt);
				CustomPropertyStates.emplace(Record.Object, CustomPropertyState{});
				continue;
			}
			if (Record.Operation == WireJournalOperation::Reparent) {
				if (Parents.contains(Record.Object)) Parents[Record.Object] = Record.Parent;
				continue;
			}
			if (Record.Operation == WireJournalOperation::Destroy) {
				std::unordered_set<WireObjectId> Removed{Record.Object};
				bool Changed;
				do {
					Changed = false;
					for (const auto &[Object, Parent] : Parents)
						if (Parent && Removed.contains(*Parent) && Removed.insert(Object).second) Changed = true;
				} while (Changed);
				for (const auto &Object : Removed) {
					ClassIds.erase(Object);
					Parents.erase(Object);
					CustomPropertyStates.erase(Object);
				}
				continue;
			}
			if (Record.Operation != WireJournalOperation::PropertyUpdate) continue;
			auto Target = ClassIds.find(Record.Object);
			if (Target == ClassIds.end()) {
				if (Record.DeclaringClassSchemaId)
					return {ReplicationApplyStatus::ApplyRejected, 0, "Custom property target is stale or missing"};
				continue;
			}
			auto *TargetClass = Schema.FindClassById(Target->second);
			const InstanceProperty *ReflectedProperty = nullptr;
			if (TargetClass && Record.PropertyName) {
				auto Property = TargetClass->AllProperties.find(*Record.PropertyName);
				if (Property != TargetClass->AllProperties.end()) ReflectedProperty = Property->second;
			}
			const bool CustomProperty = ReflectedProperty && ReflectedProperty->CustomSchemaPropertyType.has_value();
			if (!CustomProperty && !Record.DeclaringClassSchemaId) continue;
			if (!CustomProperty || !Record.DeclaringClassSchemaId || !Record.DefinitionVersion || !Record.Value)
				return {ReplicationApplyStatus::MalformedRecord, 0,
					"Custom PropertyUpdate is missing stable identity, version, property, or value"};
			auto *DeclaringClass = Schema.FindClassById(*Record.DeclaringClassSchemaId);
			auto *Property = Record.PropertyName
				? Schema.FindCustomClassProperty(*Record.DeclaringClassSchemaId, *Record.PropertyName) : nullptr;
			if (!DeclaringClass || DeclaringClass->ConstructionKind != SchemaClassConstructionKind::CustomData ||
				DeclaringClass->DefinitionVersion != *Record.DefinitionVersion || !Property ||
				ReflectedProperty->DeclaringSchemaId != *Record.DeclaringClassSchemaId ||
				!Schema.IsClassDerivedFrom(Target->second, *Record.DeclaringClassSchemaId))
				return {ReplicationApplyStatus::ApplyRejected, 0,
					"Custom property stable identity, version, or target is incompatible"};
			try {
				(void)ValidateSchemaExtensionPropertyValue(Property->Type, *Record.Value);
				auto State = CustomPropertyStates.find(Record.Object);
				if (State == CustomPropertyStates.end())
					return {ReplicationApplyStatus::ApplyRejected, 0, "Custom property target state is stale or missing"};
				auto Current = Property->DefaultValue;
				if (auto Owner = State->second.find(*Record.DeclaringClassSchemaId); Owner != State->second.end())
					if (auto Existing = Owner->second.find(*Record.PropertyName); Existing != Owner->second.end())
						Current = Existing->second;
				if (Current == *Record.Value)
					return {ReplicationApplyStatus::ApplyRejected, 0,
						"Custom PropertyUpdate is a semantic no-op"};

				if (*Record.Value == Property->DefaultValue) {
					auto Owner = State->second.find(*Record.DeclaringClassSchemaId);
					if (Owner != State->second.end()) {
						Owner->second.erase(*Record.PropertyName);
						if (Owner->second.empty()) State->second.erase(Owner);
					}
				} else {
					State->second[*Record.DeclaringClassSchemaId][*Record.PropertyName] = *Record.Value;
				}

				std::size_t Count = 0;
				std::size_t Bytes = 0;
				for (const auto &[DeclaringId, Values] : State->second) {
					auto *Definition = Schema.FindClassById(DeclaringId);
					if (!Definition || Definition->ConstructionKind != SchemaClassConstructionKind::CustomData ||
						!Schema.IsClassDerivedFrom(Target->second, DeclaringId))
						throw std::invalid_argument("Custom property override does not belong to the target class");
					for (const auto &[Name, Value] : Values) {
						auto *DefinitionProperty = Schema.FindCustomClassProperty(DeclaringId, Name);
						if (!DefinitionProperty || Value == DefinitionProperty->DefaultValue)
							throw std::invalid_argument("Custom property override is unknown or redundantly stores its default");
						++Count;
						Bytes += sizeof(SchemaId) + sizeof(Definition->DefinitionVersion) + Name.size() +
							ValidateSchemaExtensionPropertyValue(DefinitionProperty->Type, Value);
						if (Count > MaximumCustomPropertyOverridesPerInstance ||
							Bytes > MaximumCustomPropertyOverrideBytesPerInstance)
							throw std::invalid_argument("Instance exceeds its custom property override limits");
					}
				}
			} catch (const std::exception &Error) {
				return {ReplicationApplyStatus::ApplyRejected, 0, Error.what()};
			}
		}
		return {ReplicationApplyStatus::Success, 0, {}};
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyProperty(
		const WireJournalRecord &record,
		const std::shared_ptr<Instance> &instance
	) {
		if (!record.PropertyName || !record.Value)
			return {ReplicationApplyStatus::MalformedRecord, 0, "PropertyUpdate is missing its name or WireValue"};
		if (*record.PropertyName == "Destroyed") {
			auto destroyed = std::get_if<bool>(&*record.Value);
			return destroyed && *destroyed
				? ReplicationApplyResult{ReplicationApplyStatus::Success, 1, {}}
				: ReplicationApplyResult{ReplicationApplyStatus::MalformedRecord, 0, "Invalid Destroyed lifecycle value"};
		}

		auto *property = instance->FindProperty(*record.PropertyName);
		if (!property || property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
			!property->Write)
			return {ReplicationApplyStatus::ApplyRejected, 0, "Receiver property is unknown, read-only, or not replicated"};
		if (property->CustomSchemaPropertyType) {
			if (!record.DeclaringClassSchemaId || !record.DefinitionVersion ||
				property->DeclaringSchemaId != *record.DeclaringClassSchemaId ||
				property->DeclaringDefinitionVersion != *record.DefinitionVersion)
				return {ReplicationApplyStatus::ApplyRejected, 0, "Custom property stable identity/version does not match"};
			try {
				const auto current = instance->GetCustomClassPropertyValue(
					*record.DeclaringClassSchemaId, *record.PropertyName, ScriptSecurityContext::CoreTrusted()
				);
				if (current == *record.Value)
					return {ReplicationApplyStatus::ApplyRejected, 0, "Custom PropertyUpdate is a semantic no-op"};
				const auto status = instance->ApplyCustomClassPropertyMutation(
					*record.DeclaringClassSchemaId,
					*record.DefinitionVersion,
					*record.PropertyName,
					*record.Value,
					ScriptSecurityContext::CoreTrusted()
				);
				return status == MutationStatus::Success
					? ReplicationApplyResult{ReplicationApplyStatus::Success, 1, {}}
					: ReplicationApplyResult{ReplicationApplyStatus::ApplyRejected, 0, "Receiver rejected custom property value"};
			} catch (const std::exception &error) {
				return {ReplicationApplyStatus::ApplyRejected, 0, error.what()};
			}
		}
		if (record.DeclaringClassSchemaId || record.DefinitionVersion)
			return {ReplicationApplyStatus::MalformedRecord, 0, "Native PropertyUpdate contains custom identity metadata"};
		try {
			if (std::holds_alternative<std::monostate>(*record.Value) ||
				std::holds_alternative<WireObjectReference>(*record.Value)) {
				if (!property->WriteObjectReference)
					return {ReplicationApplyStatus::ApplyRejected, 0, "Wire reference used for a value property"};
				std::shared_ptr<Instance> referenced;
				if (auto *reference = std::get_if<WireObjectReference>(&*record.Value)) {
					referenced = ResolveReceiver(reference->Object);
					if (!referenced)
						return {ReplicationApplyStatus::ApplyRejected, 0, "Wire reference is stale or not yet created"};
				}
				property->WriteObjectReference(instance.get(), referenced);
			} else if (auto *wireEnum = std::get_if<WireEnumItem>(&*record.Value)) {
				if (!property->WriteEnumValue || property->ReflectedTypedef != "Enum." + wireEnum->EnumType)
					return {ReplicationApplyStatus::ApplyRejected, 0, "Wire enum does not match the receiver property"};
				auto enumType = Enums::GetEnums().find(wireEnum->EnumType);
				if (enumType == Enums::GetEnums().end())
					return {ReplicationApplyStatus::ApplyRejected, 0, "Wire enum type is unknown"};
				auto item = enumType->second->FromName(wireEnum->Item);
				if (!item) return {ReplicationApplyStatus::ApplyRejected, 0, "Wire enum item is unknown"};
				property->WriteEnumValue(instance.get(), item->Value);
			} else {
				auto native = DecodeNativeWireValue(*record.Value);
				if (!native || instance->ApplyPropertyMutation(
						*record.PropertyName, *native, Enums::Permission::Engine
					) != MutationStatus::Success)
					return {ReplicationApplyStatus::ApplyRejected, 0, "Receiver rejected the WireValue"};
			}
		} catch (const std::exception &error) {
			return {ReplicationApplyStatus::ApplyRejected, 0, error.what()};
		}
		return {ReplicationApplyStatus::Success, 1, {}};
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyAttribute(
		const WireJournalRecord &record,
		const std::shared_ptr<Instance> &instance
	) {
		if (!record.AttributeName || !record.Value)
			return {ReplicationApplyStatus::MalformedRecord, 0, "AttributeUpdate is missing its name or WireValue"};
		std::optional<WireValue> value = *record.Value;
		if (std::holds_alternative<std::monostate>(*record.Value)) value.reset();
		try {
			const auto current = instance->GetAttributeValue(*record.AttributeName, ScriptSecurityContext::CoreTrusted());
			if (current == value)
				return {ReplicationApplyStatus::ApplyRejected, 0, "AttributeUpdate is a semantic no-op"};
			const auto status = instance->ApplyAttributeMutation(
				*record.AttributeName, std::move(value), ScriptSecurityContext::CoreTrusted()
			);
			if (status != MutationStatus::Success)
				return {ReplicationApplyStatus::ApplyRejected, 0, "Receiver rejected the attribute WireValue"};
		} catch (const std::exception &error) {
			return {ReplicationApplyStatus::ApplyRejected, 0, error.what()};
		}
		return {ReplicationApplyStatus::Success, 1, {}};
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyExtensionProperty(
		const WireJournalRecord &record,
		const std::shared_ptr<Instance> &instance
	) {
		if (!record.ExtensionSchemaId || !record.DefinitionVersion ||
			!record.ExtensionPropertyName || !record.Value)
			return {
				ReplicationApplyStatus::MalformedRecord,
				0,
				"ExtensionPropertyUpdate is missing stable identity, version, property, or value"
			};
		try {
			const auto current = instance->GetExtensionPropertyValue(
				*record.ExtensionSchemaId,
				*record.ExtensionPropertyName,
				ScriptSecurityContext::CoreTrusted()
			);
			if (current == *record.Value)
				return {ReplicationApplyStatus::ApplyRejected, 0, "ExtensionPropertyUpdate is a semantic no-op"};
			const auto status = instance->ApplyExtensionPropertyMutation(
				*record.ExtensionSchemaId,
				*record.DefinitionVersion,
				*record.ExtensionPropertyName,
				*record.Value,
				ScriptSecurityContext::CoreTrusted()
			);
			if (status != MutationStatus::Success)
				return {ReplicationApplyStatus::ApplyRejected, 0, "Receiver rejected the extension property value"};
		} catch (const std::exception &error) {
			return {ReplicationApplyStatus::ApplyRejected, 0, error.what()};
		}
		return {ReplicationApplyStatus::Success, 1, {}};
	}

	ReplicationApplyResult InProcessReplicationSession::ApplyRecord(const WireJournalRecord &record) {
		ScopedChangeJournalSuppression suppressReceiverChanges;
		try {
			switch (record.Operation) {
			case WireJournalOperation::Create: {
					if (!record.ClassName || !record.ClassSchemaId || !record.DefinitionVersion ||
						record.Parent || record.PropertyName || record.DeclaringClassSchemaId || record.AttributeName ||
						record.ExtensionSchemaId || record.ExtensionPropertyName ||
						record.TagName || record.Value ||
						Receiver.Objects.contains(record.Object))
						return {ReplicationApplyStatus::MalformedRecord, 0, "Invalid or duplicate Create record"};
					auto *definition = InstanceClassRegistry::GetDefinitionBySchemaId(*record.ClassSchemaId);
					const auto expectedName = definition && definition->ConstructionKind == SchemaClassConstructionKind::CustomData
						? definition->CanonicalName : definition ? definition->ClassName : std::string{};
					if (!definition || definition->DefinitionVersion != *record.DefinitionVersion ||
						expectedName != *record.ClassName || !InstanceClassRegistry::IsConstructible(*definition))
						return {ReplicationApplyStatus::ApplyRejected, 0, "Create class identity is unknown or incompatible"};
					auto instance = InstanceClassRegistry::Construct(*definition);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Create constructor returned null"};
					Receiver.Objects.emplace(record.Object, std::move(instance));
					CandidateObjects.insert(record.Object);
					break;
				}
				case WireJournalOperation::PropertyUpdate: {
					if (record.Parent || record.ClassName || record.ClassSchemaId || record.AttributeName ||
						record.ExtensionSchemaId || record.ExtensionPropertyName || record.TagName ||
						(record.DeclaringClassSchemaId.has_value() != record.DefinitionVersion.has_value()))
						return {ReplicationApplyStatus::MalformedRecord, 0, "PropertyUpdate contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Property target is stale or missing"};
					return ApplyProperty(record, instance);
				}
				case WireJournalOperation::AttributeUpdate: {
					if (record.Parent || record.ClassName || record.ClassSchemaId || record.PropertyName ||
						record.DeclaringClassSchemaId || record.ExtensionSchemaId ||
						record.DefinitionVersion || record.ExtensionPropertyName || record.TagName)
						return {ReplicationApplyStatus::MalformedRecord, 0, "AttributeUpdate contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Attribute target is stale or missing"};
					return ApplyAttribute(record, instance);
				}
				case WireJournalOperation::ExtensionPropertyUpdate: {
					if (record.Parent || record.ClassName || record.ClassSchemaId || record.PropertyName ||
						record.DeclaringClassSchemaId || record.AttributeName || record.TagName)
						return {ReplicationApplyStatus::MalformedRecord, 0, "ExtensionPropertyUpdate contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance)
						return {ReplicationApplyStatus::ApplyRejected, 0, "Extension property target is stale or missing"};
					return ApplyExtensionProperty(record, instance);
				}
				case WireJournalOperation::TagAdded:
				case WireJournalOperation::TagRemoved: {
					if (record.Parent || record.ClassName || record.ClassSchemaId || record.PropertyName ||
						record.DeclaringClassSchemaId || record.AttributeName ||
						record.ExtensionSchemaId || record.DefinitionVersion || record.ExtensionPropertyName ||
						record.Value || !record.TagName)
						return {ReplicationApplyStatus::MalformedRecord, 0, "Tag record contains invalid metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Tag target is stale or missing"};
					auto dataModel = std::dynamic_pointer_cast<DataModel>(Receiver.Root);
					if (!dataModel) return {ReplicationApplyStatus::ApplyRejected, 0, "Receiver root is not a DataModel"};
					const bool changed = record.Operation == WireJournalOperation::TagAdded
						? dataModel->Tags.Add(dataModel->GetObjectId(), instance->GetObjectId(), *record.TagName, ScriptSecurityContext::CoreTrusted())
						: dataModel->Tags.Remove(dataModel->GetObjectId(), instance->GetObjectId(), *record.TagName, ScriptSecurityContext::CoreTrusted());
					if (!changed) return {
						ReplicationApplyStatus::ApplyRejected,
						0,
						record.Operation == WireJournalOperation::TagAdded
							? "TagAdded duplicates existing membership"
							: "TagRemoved names absent membership",
					};
					break;
				}
				case WireJournalOperation::Reparent: {
					if (record.ClassName || record.ClassSchemaId || record.PropertyName || record.DeclaringClassSchemaId ||
						record.AttributeName || record.ExtensionSchemaId ||
						record.DefinitionVersion || record.ExtensionPropertyName || record.TagName || record.Value)
						return {ReplicationApplyStatus::MalformedRecord, 0, "Reparent contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ResnapshotRequired, 0, "Reparent target predates the snapshot"};
					std::shared_ptr<Instance> parent;
					if (record.Parent) {
						parent = ResolveReceiver(*record.Parent);
						if (!parent) return {ReplicationApplyStatus::ApplyRejected, 0, "Reparent parent is stale or missing"};
					}
					instance->SetParent(parent ? std::optional(parent) : std::nullopt);
					if (record.Parent && TrackedObjects.contains(*record.Parent)) {
						CandidateObjects.erase(record.Object);
						TrackedObjects.insert(record.Object);
					}
					break;
				}
				case WireJournalOperation::Destroy: {
					if (record.Parent || record.ClassName || record.ClassSchemaId || record.PropertyName ||
						record.DeclaringClassSchemaId || record.AttributeName ||
						record.ExtensionSchemaId || record.DefinitionVersion || record.ExtensionPropertyName ||
						record.TagName || record.Value)
						return {ReplicationApplyStatus::MalformedRecord, 0, "Destroy contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Destroy target is stale or missing"};
					std::unordered_set<Instance *> subtree;
					CollectSubtree(instance, subtree);
					instance->Destroy();
					for (auto object = Receiver.Objects.begin(); object != Receiver.Objects.end();) {
						if (!subtree.contains(object->second.get())) {
							++object;
							continue;
						}
						TrackedObjects.erase(object->first);
						CandidateObjects.erase(object->first);
						object = Receiver.Objects.erase(object);
					}
					break;
				}
			}
		} catch (const std::exception &error) {
			return {ReplicationApplyStatus::ApplyRejected, 0, error.what()};
		}
		return {ReplicationApplyStatus::Success, 1, {}};
	}
}
