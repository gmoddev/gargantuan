#include "gargantuan/runtime/SpatialRuntimeProjection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>

namespace gargantuan {
	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter, Value Amount = 1) {
			Counter = Amount > std::numeric_limits<Value>::max() - Counter ? std::numeric_limits<Value>::max()
																		   : Counter + Amount;
		}

		bool Finite(const CFrame &Value) {
			if (!std::isfinite(Value.Position.x) || !std::isfinite(Value.Position.y) ||
				!std::isfinite(Value.Position.z))
				return false;
			for (glm::length_t Column = 0; Column < 3; ++Column)
				for (glm::length_t Row = 0; Row < 3; ++Row)
					if (!std::isfinite(Value.Rotation[Column][Row])) return false;
			return true;
		}
	}

	bool SpatialRuntimeProjectionConfiguration::IsValid() const {
		return Index.IsValid() && MaximumSpaces != 0 && MaximumSpaces <= MaximumSpatialRuntimeSpaces;
	}

	const char *SpatialRuntimeProjectionStatusName(SpatialRuntimeProjectionStatus Status) noexcept {
		switch (Status) {
		case SpatialRuntimeProjectionStatus::Success:
			return "success";
		case SpatialRuntimeProjectionStatus::InvalidConfiguration:
			return "invalid configuration";
		case SpatialRuntimeProjectionStatus::InvalidObject:
			return "invalid object";
		case SpatialRuntimeProjectionStatus::InvalidSpace:
			return "invalid space";
		case SpatialRuntimeProjectionStatus::InvalidProjection:
			return "invalid projection";
		case SpatialRuntimeProjectionStatus::DuplicateProjection:
			return "duplicate projection";
		case SpatialRuntimeProjectionStatus::MissingProjection:
			return "missing projection";
		case SpatialRuntimeProjectionStatus::SpaceLimit:
			return "space limit";
		case SpatialRuntimeProjectionStatus::SpaceOccupied:
			return "space occupied";
		case SpatialRuntimeProjectionStatus::DirtyLimit:
			return "dirty limit";
		case SpatialRuntimeProjectionStatus::IndexFailure:
			return "index failure";
		case SpatialRuntimeProjectionStatus::AllocationFailure:
			return "allocation failure";
		}
		return "unknown";
	}

	struct SpatialRuntimeProjectionStore::Implementation {
		struct SpaceSlot {
			std::uint32_t Generation = 1;
			std::size_t ProjectionCount = 0;
			bool Active = false;
			bool Retired = false;
		};

		SpatialRuntimeProjectionConfiguration Configuration;
		SpatialRegionIndex Index;
		std::vector<SpaceSlot> Spaces;
		std::map<ObjectId, SpatialRuntimeProjection> Projections;
		std::vector<ObjectId> DirtyObjects;
		SpatialRuntimeProjectionMetrics Metrics;
		SpatialRegionStatus LastIndexFailure = SpatialRegionStatus::Success;

		explicit Implementation(SpatialRuntimeProjectionConfiguration ConfigurationValue)
			: Configuration(ConfigurationValue), Index(ConfigurationValue.Index),
			  Spaces(ConfigurationValue.MaximumSpaces + 1) {
			Spaces[DefaultSpatialSpace.Slot].Generation = DefaultSpatialSpace.Generation;
			Spaces[DefaultSpatialSpace.Slot].Active = true;
			DirtyObjects.reserve(Configuration.Index.MaximumObjects);
			RefreshGauges();
		}

		void RefreshGauges() {
			Metrics.ActiveSpaces = 0;
			for (std::size_t Slot = 1; Slot < Spaces.size(); ++Slot)
				if (Spaces[Slot].Active) SaturatingIncrement(Metrics.ActiveSpaces);
			Metrics.ActiveProjections = Projections.size();
			Metrics.DirtyProjections = static_cast<std::uint64_t>(
				std::ranges::count_if(Projections, [](const auto &Entry) { return Entry.second.Dirty; })
			);
		}

		bool IsSpaceAlive(SpatialSpaceId Space) const {
			return Space.IsValid() && Space.Slot < Spaces.size() && Spaces[Space.Slot].Active &&
				   Spaces[Space.Slot].Generation == Space.Generation;
		}

		bool ValidProjection(const SpatialPose &Pose, const SpatialBounds &Bounds) const {
			return IsSpaceAlive(Pose.Space) && Finite(Pose.LocalTransform) && Bounds.IsValid();
		}

		SpatialRuntimeProjectionStatus IndexFailed(SpatialRegionStatus Status) {
			LastIndexFailure = Status;
			SaturatingIncrement(Metrics.FailedTransactions);
			return SpatialRuntimeProjectionStatus::IndexFailure;
		}
	};

	SpatialRuntimeProjectionStore::SpatialRuntimeProjectionStore(SpatialRuntimeProjectionConfiguration Configuration)
		: State(Configuration.IsValid() ? std::make_unique<Implementation>(Configuration) : nullptr) {
		if (!State) throw std::invalid_argument("[Spatial:Projection] configuration is invalid");
	}

	SpatialRuntimeProjectionStore::~SpatialRuntimeProjectionStore() = default;

	SpatialSpaceId SpatialRuntimeProjectionStore::GetDefaultSpace() const noexcept {
		return DefaultSpatialSpace;
	}

	std::optional<SpatialSpaceId> SpatialRuntimeProjectionStore::CreateSpace() {
		for (std::size_t Slot = 2; Slot < State->Spaces.size(); ++Slot) {
			auto &Candidate = State->Spaces[Slot];
			if (Candidate.Active || Candidate.Retired) continue;
			Candidate.Active = true;
			SaturatingIncrement(State->Metrics.SpaceCreations);
			State->RefreshGauges();
			return SpatialSpaceId{static_cast<std::uint32_t>(Slot), Candidate.Generation};
		}
		SaturatingIncrement(State->Metrics.FailedTransactions);
		return std::nullopt;
	}

	SpatialRuntimeProjectionStatus SpatialRuntimeProjectionStore::DestroySpace(SpatialSpaceId Space) {
		if (!State->IsSpaceAlive(Space) || Space == DefaultSpatialSpace)
			return SpatialRuntimeProjectionStatus::InvalidSpace;
		auto &Slot = State->Spaces[Space.Slot];
		if (Slot.ProjectionCount != 0) return SpatialRuntimeProjectionStatus::SpaceOccupied;
		Slot.Active = false;
		if (Slot.Generation == std::numeric_limits<std::uint32_t>::max())
			Slot.Retired = true;
		else
			++Slot.Generation;
		SaturatingIncrement(State->Metrics.SpaceDestructions);
		State->RefreshGauges();
		return SpatialRuntimeProjectionStatus::Success;
	}

	bool SpatialRuntimeProjectionStore::IsSpaceAlive(SpatialSpaceId Space) const {
		return State->IsSpaceAlive(Space);
	}

	SpatialRuntimeProjectionStatus
	SpatialRuntimeProjectionStore::Register(ObjectId Object, const SpatialPose &Pose, const SpatialBounds &Bounds) {
		if (!Object.IsValid()) return SpatialRuntimeProjectionStatus::InvalidObject;
		if (!State->IsSpaceAlive(Pose.Space)) return SpatialRuntimeProjectionStatus::InvalidSpace;
		if (!State->ValidProjection(Pose, Bounds)) return SpatialRuntimeProjectionStatus::InvalidProjection;
		if (State->Projections.contains(Object)) return SpatialRuntimeProjectionStatus::DuplicateProjection;
		const auto IndexStatus = State->Index.Register(Object, Pose.Space, Bounds);
		if (IndexStatus != SpatialRegionStatus::Success) return State->IndexFailed(IndexStatus);
		try {
			State->Projections.emplace(Object, SpatialRuntimeProjection{Pose, Bounds, 1, false});
		} catch (const std::bad_alloc &) {
			(void)State->Index.Remove(Object);
			SaturatingIncrement(State->Metrics.FailedTransactions);
			return SpatialRuntimeProjectionStatus::AllocationFailure;
		}
		++State->Spaces[Pose.Space.Slot].ProjectionCount;
		SaturatingIncrement(State->Metrics.ProjectionCreations);
		State->RefreshGauges();
		return SpatialRuntimeProjectionStatus::Success;
	}

	SpatialRuntimeProjectionStatus
	SpatialRuntimeProjectionStore::Update(ObjectId Object, const SpatialPose &Pose, const SpatialBounds &Bounds) {
		auto Found = State->Projections.find(Object);
		if (Found == State->Projections.end()) return SpatialRuntimeProjectionStatus::MissingProjection;
		if (!State->IsSpaceAlive(Pose.Space)) return SpatialRuntimeProjectionStatus::InvalidSpace;
		if (Pose.Space != Found->second.Pose.Space) return SpatialRuntimeProjectionStatus::InvalidSpace;
		if (!State->ValidProjection(Pose, Bounds)) return SpatialRuntimeProjectionStatus::InvalidProjection;
		const auto IndexStatus = State->Index.Update(Object, Pose.Space, Bounds);
		if (IndexStatus != SpatialRegionStatus::Success) return State->IndexFailed(IndexStatus);
		Found->second.Pose = Pose;
		Found->second.Bounds = Bounds;
		SaturatingIncrement(Found->second.Revision);
		SaturatingIncrement(State->Metrics.ProjectionUpdates);
		return SpatialRuntimeProjectionStatus::Success;
	}

	SpatialRuntimeProjectionStatus SpatialRuntimeProjectionStore::Transfer(
		ObjectId Object, const SpatialPose &Destination, const SpatialBounds &DestinationBounds
	) {
		auto Found = State->Projections.find(Object);
		if (Found == State->Projections.end()) return SpatialRuntimeProjectionStatus::MissingProjection;
		if (!State->IsSpaceAlive(Destination.Space)) return SpatialRuntimeProjectionStatus::InvalidSpace;
		if (!State->ValidProjection(Destination, DestinationBounds))
			return SpatialRuntimeProjectionStatus::InvalidProjection;
		if (Destination.Space == Found->second.Pose.Space) return Update(Object, Destination, DestinationBounds);
		const auto SourceSpace = Found->second.Pose.Space;
		const auto IndexStatus = State->Index.Update(Object, Destination.Space, DestinationBounds);
		if (IndexStatus != SpatialRegionStatus::Success) return State->IndexFailed(IndexStatus);
		--State->Spaces[SourceSpace.Slot].ProjectionCount;
		++State->Spaces[Destination.Space.Slot].ProjectionCount;
		Found->second.Pose = Destination;
		Found->second.Bounds = DestinationBounds;
		SaturatingIncrement(Found->second.Revision);
		SaturatingIncrement(State->Metrics.ProjectionTransfers);
		return SpatialRuntimeProjectionStatus::Success;
	}

	SpatialRuntimeProjectionStatus SpatialRuntimeProjectionStore::Remove(ObjectId Object) {
		auto Found = State->Projections.find(Object);
		if (Found == State->Projections.end()) return SpatialRuntimeProjectionStatus::MissingProjection;
		const auto IndexStatus = State->Index.Remove(Object);
		if (IndexStatus != SpatialRegionStatus::Success) return State->IndexFailed(IndexStatus);
		--State->Spaces[Found->second.Pose.Space.Slot].ProjectionCount;
		State->Projections.erase(Found);
		SaturatingIncrement(State->Metrics.ProjectionRemovals);
		State->RefreshGauges();
		return SpatialRuntimeProjectionStatus::Success;
	}

	SpatialRuntimeProjectionStatus SpatialRuntimeProjectionStore::MarkDirty(ObjectId Object) {
		auto Found = State->Projections.find(Object);
		if (Found == State->Projections.end()) return SpatialRuntimeProjectionStatus::MissingProjection;
		if (Found->second.Dirty) {
			SaturatingIncrement(State->Metrics.DirtyCoalesces);
			return SpatialRuntimeProjectionStatus::Success;
		}
		if (State->DirtyObjects.size() >= State->Configuration.Index.MaximumObjects) {
			std::erase_if(State->DirtyObjects, [this](ObjectId Candidate) {
				auto Current = State->Projections.find(Candidate);
				return Current == State->Projections.end() || !Current->second.Dirty;
			});
		}
		if (State->DirtyObjects.size() >= State->Configuration.Index.MaximumObjects) {
			SaturatingIncrement(State->Metrics.FailedTransactions);
			return SpatialRuntimeProjectionStatus::DirtyLimit;
		}
		Found->second.Dirty = true;
		State->DirtyObjects.push_back(Object);
		SaturatingIncrement(State->Metrics.DirtyMarks);
		State->Metrics.DirtyProjections = static_cast<std::uint64_t>(State->DirtyObjects.size());
		State->Metrics.PeakDirtyProjections = std::max(
			State->Metrics.PeakDirtyProjections, State->Metrics.DirtyProjections
		);
		return SpatialRuntimeProjectionStatus::Success;
	}

	std::span<const ObjectId> SpatialRuntimeProjectionStore::GetDirtyObjects() const {
		return State->DirtyObjects;
	}

	void SpatialRuntimeProjectionStore::ClearDirtyObjects() {
		for (const auto Object : State->DirtyObjects) {
			auto Found = State->Projections.find(Object);
			if (Found != State->Projections.end()) Found->second.Dirty = false;
		}
		State->DirtyObjects.clear();
		State->Metrics.DirtyProjections = 0;
	}

	const SpatialRuntimeProjection *SpatialRuntimeProjectionStore::Get(ObjectId Object) const {
		auto Found = State->Projections.find(Object);
		return Found == State->Projections.end() ? nullptr : &Found->second;
	}

	std::optional<SpatialCellAddress> SpatialRuntimeProjectionStore::GetCellAddress(ObjectId Object) const {
		return State->Index.GetPrimaryAddress(Object);
	}

	bool SpatialRuntimeProjectionStore::IsLargeObject(ObjectId Object) const {
		return State->Index.IsLargeObject(Object);
	}

	SpatialRegionStatus SpatialRuntimeProjectionStore::Query(
		std::span<const SpatialRegionQueryVolume> Volumes, SpatialRegionQueryScratch &Scratch
	) {
		for (const auto &Volume : Volumes)
			if (!State->IsSpaceAlive(Volume.Space)) {
				Scratch.Clear();
				return SpatialRegionStatus::InvalidSpace;
			}
		return State->Index.Query(Volumes, Scratch);
	}

	bool SpatialRuntimeProjectionStore::VerifyConsistency() const {
		if (!State->Index.VerifyConsistency() ||
			State->Index.GetMetrics().SpatialObjectCount != State->Projections.size())
			return false;
		std::vector<std::size_t> Counts(State->Spaces.size());
		for (const auto &[Object, Projection] : State->Projections) {
			if (!State->IsSpaceAlive(Projection.Pose.Space) ||
				!State->ValidProjection(Projection.Pose, Projection.Bounds) || !State->Index.Contains(Object))
				return false;
			const auto Address = State->Index.GetPrimaryAddress(Object);
			if (!Address || Address->Space != Projection.Pose.Space) return false;
			++Counts[Projection.Pose.Space.Slot];
		}
		for (std::size_t Slot = 1; Slot < State->Spaces.size(); ++Slot)
			if (Counts[Slot] != State->Spaces[Slot].ProjectionCount) return false;
		return true;
	}

	std::size_t SpatialRuntimeProjectionStore::GetProjectionCount() const {
		return State->Projections.size();
	}

	std::size_t SpatialRuntimeProjectionStore::GetSpaceCount() const {
		return static_cast<std::size_t>(State->Metrics.ActiveSpaces);
	}

	SpatialRuntimeProjectionMetrics SpatialRuntimeProjectionStore::GetMetrics() const {
		State->RefreshGauges();
		return State->Metrics;
	}

	SpatialRegionIndexMetrics SpatialRuntimeProjectionStore::GetIndexMetrics() const {
		return State->Index.GetMetrics();
	}

	SpatialRegionStatus SpatialRuntimeProjectionStore::GetLastIndexFailure() const {
		return State->LastIndexFailure;
	}
}
