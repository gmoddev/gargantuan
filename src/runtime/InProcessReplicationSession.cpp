#include "gargantuan/runtime/InProcessReplicationSession.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <exception>
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
		std::size_t applied = 0;
		for (const auto &record : records) {
			if (record.Version != WireJournalFormatVersion || !record.Scope.IsValid() ||
				!record.Object.IsValid() || record.Sequence == 0)
				return {ReplicationApplyStatus::MalformedRecord, applied, "Malformed or unsupported wire journal record"};
			if (record.Scope.ToObjectId() != Cursor.Scope)
				return {ReplicationApplyStatus::ApplyRejected, applied, "Wire journal record belongs to another replication scope"};
			if (record.Sequence < Cursor.NextSequence)
				return {ReplicationApplyStatus::DuplicateRecord, applied, "Duplicate wire journal sequence"};
			if (record.Sequence > Cursor.NextSequence)
				return {ReplicationApplyStatus::OutOfOrderRecord, applied, "Out-of-order wire journal sequence"};

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

	ReplicationApplyResult InProcessReplicationSession::ApplyRecord(const WireJournalRecord &record) {
		ScopedChangeJournalSuppression suppressReceiverChanges;
		try {
			switch (record.Operation) {
				case WireJournalOperation::Create: {
					if (!record.ClassName || record.Parent || record.PropertyName || record.AttributeName || record.Value ||
						Receiver.Objects.contains(record.Object))
						return {ReplicationApplyStatus::MalformedRecord, 0, "Invalid or duplicate Create record"};
					auto *definition = InstanceClassRegistry::GetDefinitionByName(*record.ClassName);
					if (!definition || !definition->Constructor)
						return {ReplicationApplyStatus::ApplyRejected, 0, "Create names an unknown class"};
					auto instance = definition->Constructor();
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Create constructor returned null"};
					Receiver.Objects.emplace(record.Object, std::move(instance));
					CandidateObjects.insert(record.Object);
					break;
				}
				case WireJournalOperation::PropertyUpdate: {
					if (record.Parent || record.ClassName || record.AttributeName)
						return {ReplicationApplyStatus::MalformedRecord, 0, "PropertyUpdate contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Property target is stale or missing"};
					return ApplyProperty(record, instance);
				}
				case WireJournalOperation::AttributeUpdate: {
					if (record.Parent || record.ClassName || record.PropertyName)
						return {ReplicationApplyStatus::MalformedRecord, 0, "AttributeUpdate contains unrelated metadata"};
					auto instance = ResolveReceiver(record.Object);
					if (!instance) return {ReplicationApplyStatus::ApplyRejected, 0, "Attribute target is stale or missing"};
					return ApplyAttribute(record, instance);
				}
				case WireJournalOperation::Reparent: {
					if (record.ClassName || record.PropertyName || record.AttributeName || record.Value)
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
					if (record.Parent || record.ClassName || record.PropertyName || record.AttributeName || record.Value)
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
