#include "gargantuan/classes/DataModel.hpp"

#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/EntitlementService.hpp"
#include "gargantuan/services/InteractionService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/RunService.hpp"
#include "gargantuan/services/Tags.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		thread_local DataModel *DeferredRevisionWorld = nullptr;
		thread_local bool *DeferredRevisionChanged = nullptr;
	}
	void DataModel::EnsureAuthoritativeRevisionAvailable() const {
		if (AuthoritativeRevision == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Authoritative project revision is exhausted");
	}

	void DataModel::AdvanceAuthoritativeRevision() {
		EnsureAuthoritativeRevisionAvailable();
		if (DeferredRevisionWorld == this && DeferredRevisionChanged) {
			*DeferredRevisionChanged = true;
			return;
		}
		if (RevisionBatchActive) {
			RevisionBatchChanged = true;
			return;
		}
		++AuthoritativeRevision;
	}

	void DataModel::BeginAuthoritativeRevisionBatch() {
		EnsureAuthoritativeRevisionAvailable();
		if (RevisionBatchActive) throw std::logic_error("Authoritative project revision batch is already active");
		RevisionBatchActive = true;
		RevisionBatchChanged = false;
	}

	void DataModel::CommitAuthoritativeRevisionBatch() {
		if (!RevisionBatchActive) throw std::logic_error("No authoritative project revision batch is active");
		const bool Changed = RevisionBatchChanged;
		RevisionBatchActive = false;
		RevisionBatchChanged = false;
		if (Changed) AdvanceAuthoritativeRevision();
	}

	void DataModel::CancelAuthoritativeRevisionBatch() noexcept {
		RevisionBatchActive = false;
		RevisionBatchChanged = false;
	}

	bool DataModel::IsProtectedService(const std::shared_ptr<Instance> &InstanceValue) const {
		if (!InstanceValue) return false;
		if (InstanceValue.get() == this) return true;
		auto Parent = InstanceValue->GetParent();
		if (!Parent || Parent->get() != this) return false;
		auto *Definition = InstanceClassRegistry::GetDefinition(InstanceValue.get());
		if (!Definition) return true;
		return IsProtectedServiceClass(Definition->Id);
	}

	bool DataModel::IsProtectedServiceClass(SchemaId ClassId) const {
		if (ClassId == SchemaId::FromNativeName("Engine", "DataModel")) return true;
		for (const auto &[Name, Id] : GetServiceDefinitions()) {
			(void)Name;
			if (ClassId == Id) return true;
		}
		return false;
	}

	bool DataModel::CanAdoptInstances(std::size_t Count) const {
		return Count <= MaximumPersistenceObjects && OwnedInstanceCount <= MaximumPersistenceObjects - Count;
	}

	void DataModel::AdoptInstances(std::size_t Count) {
		if (!CanAdoptInstances(Count)) throw std::length_error("DataModel object-count limit would be exceeded");
		OwnedInstanceCount += Count;
	}

	void DataModel::ReleaseInstance() noexcept {
		if (OwnedInstanceCount > 1) --OwnedInstanceCount;
	}

	void DataModel::InitializeLoadedProjectRevision() {
		// Canonical services required to inspect an ordinary project are part of
		// project establishment, not a later authoring mutation. Construct them
		// before publishing the initial revision.
		(void)GetService("Workspace");
		(void)GetService("AssetService");
		AuthoritativeRevision = InitialProjectRevision;
		Transactions.Reset();
	}

	ScopedAuthoritativeRevisionDeferral::ScopedAuthoritativeRevisionDeferral(DataModel &WorldValue, bool &Changed)
		: World(&WorldValue) {
		if (DeferredRevisionWorld) throw std::logic_error("Authoritative project revision deferral is already active");
		DeferredRevisionWorld = World;
		DeferredRevisionChanged = &Changed;
	}

	ScopedAuthoritativeRevisionDeferral::~ScopedAuthoritativeRevisionDeferral() {
		if (DeferredRevisionWorld == World) {
			DeferredRevisionWorld = nullptr;
			DeferredRevisionChanged = nullptr;
		}
	}

	const DataModel::ServiceDefinitions &DataModel::GetServiceDefinitions() const {
		static const DataModel::ServiceDefinitions CONSTRUCTORS = {
			{"ActionMap", SchemaId::FromNativeName("Engine", "ActionMap")},
			{"AssetService", SchemaId::FromNativeName("Engine", "AssetService")},
			{"EntitlementService", SchemaId::FromNativeName("Engine", "EntitlementService")},
			{"InteractionService", SchemaId::FromNativeName("Engine", "InteractionService")},
			{"Players", SchemaId::FromNativeName("Engine", "Players")},
			{"ProcessService", SchemaId::FromNativeName("Engine", "ProcessService")},
			{"RunService", SchemaId::FromNativeName("Engine", "RunService")},
			{"Tags", SchemaId::FromNativeName("Engine", "Tags")},
			{"UserInputService", SchemaId::FromNativeName("Engine", "UserInputService")},
			{"Workspace", SchemaId::FromNativeName("Engine", "Workspace")},
		};
		return CONSTRUCTORS;
	};
}
