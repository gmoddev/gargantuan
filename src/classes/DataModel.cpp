#include "gargantuan/classes/DataModel.hpp"

#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/RunService.hpp"
#include "gargantuan/services/Tags.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <limits>
#include <stdexcept>

namespace gargantuan {
	void DataModel::EnsureAuthoritativeRevisionAvailable() const {
		if (AuthoritativeRevision == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Authoritative project revision is exhausted");
	}

	void DataModel::AdvanceAuthoritativeRevision() {
		EnsureAuthoritativeRevisionAvailable();
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
		if (Changed) ++AuthoritativeRevision;
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

	void DataModel::InitializeLoadedProjectRevision() {
		AuthoritativeRevision = InitialProjectRevision;
	}

	const DataModel::ServiceDefinitions &DataModel::GetServiceDefinitions() const {
		static const DataModel::ServiceDefinitions CONSTRUCTORS = {
			{"ProcessService", SchemaId::FromNativeName("Engine", "ProcessService")},
			{"RunService", SchemaId::FromNativeName("Engine", "RunService")},
			{"Tags", SchemaId::FromNativeName("Engine", "Tags")},
			{"UserInputService", SchemaId::FromNativeName("Engine", "UserInputService")},
			{"Workspace", SchemaId::FromNativeName("Engine", "Workspace")},
		};
		return CONSTRUCTORS;
	};
}
