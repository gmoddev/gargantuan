// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderDirtyAccumulator.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		using ProfileClock = std::chrono::steady_clock;

		std::uint64_t ProfileNanoseconds(ProfileClock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
		}
	}

	RenderDirtyAccumulator::RenderDirtyAccumulator(RenderDirtyLimits LimitsValue) : Limits(LimitsValue) {
		if (Limits.MaximumScopes == 0 || Limits.MaximumConsumers == 0 || Limits.MaximumDistinctObjects == 0 ||
			Limits.MaximumDeformableBytes == 0 || Limits.MaximumUiBytes == 0 ||
			Limits.MaximumPublicationBytes == 0 || Limits.MaximumDiagnostics == 0)
			throw std::invalid_argument("Render dirty limits must be nonzero");
	}

	RenderDirtyAccumulator &RenderDirtyAccumulator::Get() {
		static RenderDirtyAccumulator Accumulator;
		return Accumulator;
	}

	RenderUpdateDomain RenderDirtyAccumulator::Classify(const ChangePayload &Payload) {
		if (const auto *Created = std::get_if<ObjectCreatedChange>(&Payload)) {
			if (Created->ClassName == "Lighting" || Created->ClassName == "Sky")
				return RenderUpdateDomain::Environment | RenderUpdateDomain::Hierarchy;
			return RenderUpdateDomain::Transform | RenderUpdateDomain::Material | RenderUpdateDomain::Visibility |
				RenderUpdateDomain::Geometry | RenderUpdateDomain::Hierarchy;
		}
		if (std::holds_alternative<ObjectDestroyedChange>(Payload))
			return RenderUpdateDomain::Visibility | RenderUpdateDomain::Hierarchy | RenderUpdateDomain::Environment;
		if (std::holds_alternative<ObjectReparentedChange>(Payload))
			return RenderUpdateDomain::Visibility | RenderUpdateDomain::Hierarchy | RenderUpdateDomain::Environment;
		const auto *Property = std::get_if<PropertyUpdatedChange>(&Payload);
		if (!Property) return RenderUpdateDomain::None;
		if (Property->PropertyName == "CFrame" || Property->PropertyName == "Size")
			return RenderUpdateDomain::Transform;
		if (Property->PropertyName == "Color" || Property->PropertyName == "Transparency" ||
			Property->PropertyName == "CastShadow")
			return RenderUpdateDomain::Material;
		if (Property->PropertyName == "Shape" || Property->PropertyName == "Mesh") return RenderUpdateDomain::Geometry;
		if (Property->PropertyName == "Material") return RenderUpdateDomain::Material;
		if (Property->PropertyName == "Destroyed" || Property->PropertyName == "Visible")
			return RenderUpdateDomain::Visibility;
		if (Property->PropertyName == "Ambient" || Property->PropertyName == "SunColor" ||
			Property->PropertyName == "Brightness" || Property->PropertyName == "ClockTime" ||
			Property->PropertyName == "ExposureCompensation" || Property->PropertyName == "EnvironmentColor" ||
			Property->PropertyName == "FogEnabled" || Property->PropertyName == "FogColor" ||
			Property->PropertyName == "FogStart" || Property->PropertyName == "FogEnd" ||
			Property->PropertyName == "Enabled" || Property->PropertyName.starts_with("Skybox"))
			return RenderUpdateDomain::Environment;
		return RenderUpdateDomain::None;
	}

	std::uint64_t RenderDirtyAccumulator::AdvanceVersion(ScopeState &State) noexcept {
		if (State.NextVersion == std::numeric_limits<std::uint64_t>::max()) {
			MarkFailure(State, "Render dirty version exhausted; a full resync is required");
			return State.NextVersion;
		}
		return State.NextVersion++;
	}

	void RenderDirtyAccumulator::MarkFailure(ScopeState &State, std::string Diagnostic) noexcept {
		if (State.FullResyncVersion == 0) {
			State.FullResyncVersion = State.NextVersion;
			if (State.NextVersion != std::numeric_limits<std::uint64_t>::max()) ++State.NextVersion;
		}
		if (State.Diagnostics.size() >= Limits.MaximumDiagnostics) return;
		try { State.Diagnostics.push_back(std::move(Diagnostic)); }
		catch (...) { MarkGlobalFailureLocked(); }
	}

	void RenderDirtyAccumulator::MarkGlobalFailureLocked() noexcept {
		if (GlobalFailureVersion != 0) {
			if (ConsumerGlobalVersions.empty()) return;
			for (const auto &[Consumer, Version] : ConsumerGlobalVersions) {
				(void)Consumer;
				if (Version < GlobalFailureVersion) return;
			}
		}
		if (NextGlobalVersion == std::numeric_limits<std::uint64_t>::max()) {
			GlobalFailureVersion = NextGlobalVersion;
			return;
		}
		GlobalFailureVersion = NextGlobalVersion++;
	}

	void RenderDirtyAccumulator::MarkGlobalFailure() noexcept {
		try {
			std::scoped_lock Lock(Mutex);
			MarkGlobalFailureLocked();
		} catch (...) {}
	}

	void RenderDirtyAccumulator::RecordChange(
		ObjectId Scope,
		ObjectId Object,
		const ChangePayload &Payload
	) noexcept {
		const bool Profile = ProfilingEnabled.load(std::memory_order_relaxed);
		const auto ClassificationStart = Profile ? ProfileClock::now() : ProfileClock::time_point{};
		const auto Domains = Classify(Payload);
		if (Profile) {
			ProfileClassificationCalls.fetch_add(1, std::memory_order_relaxed);
			ProfileClassificationNanoseconds.fetch_add(
				ProfileNanoseconds(ProfileClock::now() - ClassificationStart), std::memory_order_relaxed
			);
		}
		if (Domains == RenderUpdateDomain::None) return;
		const auto AccumulationStart = Profile ? ProfileClock::now() : ProfileClock::time_point{};
		Mark(Scope, Object, Domains);
		if (Profile) {
			ProfileAccumulationCalls.fetch_add(1, std::memory_order_relaxed);
			ProfileAccumulationNanoseconds.fetch_add(
				ProfileNanoseconds(ProfileClock::now() - AccumulationStart), std::memory_order_relaxed
			);
		}
	}

	void RenderDirtyAccumulator::Mark(
		ObjectId Scope,
		ObjectId Object,
		RenderUpdateDomain Domains,
		std::size_t EstimatedDeformableBytes
	) noexcept {
		if (!Scope.IsValid() || !Object.IsValid() || Domains == RenderUpdateDomain::None) return;
		try {
			std::scoped_lock Lock(Mutex);
			auto FoundScope = Scopes.find(Scope);
			if (FoundScope == Scopes.end()) {
				if (Scopes.size() >= Limits.MaximumScopes) {
					MarkGlobalFailureLocked();
					return;
				}
				FoundScope = Scopes.emplace(Scope, ScopeState{}).first;
			}
			auto &State = FoundScope->second;
			auto Found = State.Entries.find(Object);
			if (Found == State.Entries.end()) {
				if (State.Entries.size() >= Limits.MaximumDistinctObjects) {
					MarkFailure(State, "Render dirty distinct-object limit exceeded; partial publication was suppressed");
					return;
				}
				Found = State.Entries.emplace(Object, Entry{}).first;
			}
			auto &EntryValue = Found->second;
			const auto PreviousBytes = EntryValue.EstimatedDeformableBytes;
			EntryValue.Domains = EntryValue.Domains | Domains;
			if (HasRenderUpdateDomain(Domains, RenderUpdateDomain::DeformableVertices))
				EntryValue.EstimatedDeformableBytes = EstimatedDeformableBytes;
			const auto RemainingBytes = State.EstimatedDeformableBytes - PreviousBytes;
			if (EntryValue.EstimatedDeformableBytes > std::numeric_limits<std::size_t>::max() - RemainingBytes)
				State.EstimatedDeformableBytes = std::numeric_limits<std::size_t>::max();
			else
				State.EstimatedDeformableBytes = RemainingBytes + EntryValue.EstimatedDeformableBytes;
			EntryValue.Version = AdvanceVersion(State);
			if (State.EstimatedDeformableBytes > Limits.MaximumDeformableBytes)
				MarkFailure(State, "Render dirty deformable-byte limit exceeded; partial publication was suppressed");
		} catch (...) { MarkGlobalFailure(); }
	}

	void RenderDirtyAccumulator::MarkUi(ObjectId Scope, std::size_t EstimatedUiBytes) noexcept {
		if (!Scope.IsValid()) return;
		try {
			std::scoped_lock Lock(Mutex);
			auto Found = Scopes.find(Scope);
			if (Found == Scopes.end()) {
				if (Scopes.size() >= Limits.MaximumScopes) {
					MarkGlobalFailureLocked();
					return;
				}
				Found = Scopes.emplace(Scope, ScopeState{}).first;
			}
			auto &State = Found->second;
			State.EstimatedUiBytes = EstimatedUiBytes;
			State.UiVersion = AdvanceVersion(State);
			if (EstimatedUiBytes > Limits.MaximumUiBytes)
				MarkFailure(State, "Render dirty UI-byte limit exceeded; partial publication was suppressed");
		} catch (...) { MarkGlobalFailure(); }
	}

	void RenderDirtyAccumulator::RequestFullResync(ObjectId Scope, std::string Diagnostic) noexcept {
		if (!Scope.IsValid()) return;
		try {
			std::scoped_lock Lock(Mutex);
			auto Found = Scopes.find(Scope);
			if (Found == Scopes.end()) {
				if (Scopes.size() >= Limits.MaximumScopes) {
					MarkGlobalFailureLocked();
					return;
				}
				Found = Scopes.emplace(Scope, ScopeState{}).first;
			}
			MarkFailure(Found->second, std::move(Diagnostic));
		} catch (...) { MarkGlobalFailure(); }
	}

	RenderDirtyConsumerId RenderDirtyAccumulator::CreateConsumer() {
		auto Result = NextConsumer.load(std::memory_order_relaxed);
		for (;;) {
			if (Result == InvalidRenderDirtyConsumerId || Result == std::numeric_limits<RenderDirtyConsumerId>::max())
				throw std::overflow_error("Render dirty consumer identity exhausted");
			if (NextConsumer.compare_exchange_weak(Result, Result + 1, std::memory_order_relaxed)) break;
		}
		std::scoped_lock Lock(Mutex);
		if (ConsumerGlobalVersions.size() >= Limits.MaximumConsumers)
			throw std::length_error("Render dirty consumer limit exceeded");
		ConsumerGlobalVersions.emplace(Result, 0);
		return Result;
	}

	void RenderDirtyAccumulator::ReleaseConsumer(RenderDirtyConsumerId Consumer, ObjectId Scope) {
		if (Consumer == InvalidRenderDirtyConsumerId) return;
		std::scoped_lock Lock(Mutex);
		if (!Scope.IsValid()) ConsumerGlobalVersions.erase(Consumer);
		for (auto Position = Scopes.begin(); Position != Scopes.end();) {
			if (Scope.IsValid() && Position->first != Scope) {
				++Position;
				continue;
			}
			Position->second.ConsumerVersions.erase(Consumer);
			if (Position->second.ConsumerVersions.empty()) Position = Scopes.erase(Position);
			else ++Position;
		}
	}

	RenderDirtyBatch RenderDirtyAccumulator::Capture(ObjectId Scope, RenderDirtyConsumerId Consumer) {
		if (!Scope.IsValid()) throw std::invalid_argument("Render dirty capture requires a valid scope");
		if (Consumer == InvalidRenderDirtyConsumerId)
			throw std::invalid_argument("Render dirty capture requires a valid consumer");
		std::scoped_lock Lock(Mutex);
		auto GlobalConsumer = ConsumerGlobalVersions.find(Consumer);
		if (GlobalConsumer == ConsumerGlobalVersions.end())
			throw std::invalid_argument("Render dirty capture consumer is not registered");
		auto Found = Scopes.find(Scope);
		if (Found == Scopes.end()) {
			if (Scopes.size() >= Limits.MaximumScopes) {
				MarkGlobalFailureLocked();
				RenderDirtyBatch Batch{
					.Scope = Scope,
					.Consumer = Consumer,
					.GlobalCaptureVersion = GlobalFailureVersion,
					.FullResyncRequired = true,
				};
				Batch.Diagnostics.emplace_back("Render dirty scope limit exceeded; a full resync is required");
				return Batch;
			}
			Found = Scopes.emplace(Scope, ScopeState{}).first;
		}
		auto &State = Found->second;
		State.ConsumerVersions.try_emplace(Consumer, 0);
		RenderDirtyBatch Batch{
			.Scope = Scope,
			.Consumer = Consumer,
			.GlobalCaptureVersion = GlobalFailureVersion,
			.FullResyncRequired = GlobalFailureVersion > GlobalConsumer->second,
		};
		Batch.CaptureVersion = State.NextVersion == std::numeric_limits<std::uint64_t>::max()
			? State.NextVersion : State.NextVersion - 1;
		const auto ConsumerVersion = State.ConsumerVersions.at(Consumer);
		Batch.FullResyncRequired = Batch.FullResyncRequired || State.FullResyncVersion > ConsumerVersion;
		Batch.UiDirty = State.UiVersion > ConsumerVersion;
		Batch.EstimatedDeformableBytes = State.EstimatedDeformableBytes;
		Batch.EstimatedUiBytes = State.EstimatedUiBytes;
		Batch.Diagnostics = State.Diagnostics;
		Batch.Records.reserve(State.Entries.size());
		for (const auto &[Object, EntryValue] : State.Entries)
			if (EntryValue.Version > ConsumerVersion)
				Batch.Records.push_back({Object, EntryValue.Domains, EntryValue.EstimatedDeformableBytes, EntryValue.Version});
		std::ranges::sort(Batch.Records, {}, &RenderDirtyRecord::Object);
		return Batch;
	}

	void RenderDirtyAccumulator::Acknowledge(const RenderDirtyBatch &Batch) {
		if (!Batch.Scope.IsValid() || Batch.Consumer == InvalidRenderDirtyConsumerId)
			throw std::invalid_argument("Render dirty acknowledgement requires a valid scope and consumer");
		std::scoped_lock Lock(Mutex);
		auto GlobalConsumer = ConsumerGlobalVersions.find(Batch.Consumer);
		if (GlobalConsumer == ConsumerGlobalVersions.end()) return;
		GlobalConsumer->second = std::max(GlobalConsumer->second, Batch.GlobalCaptureVersion);
		auto Found = Scopes.find(Batch.Scope);
		if (Found == Scopes.end()) return;
		auto &State = Found->second;
		auto Consumer = State.ConsumerVersions.find(Batch.Consumer);
		if (Consumer == State.ConsumerVersions.end()) return;
		Consumer->second = std::max(Consumer->second, Batch.CaptureVersion);
		std::uint64_t MinimumAcknowledged = std::numeric_limits<std::uint64_t>::max();
		for (const auto &[ConsumerId, Version] : State.ConsumerVersions) {
			(void)ConsumerId;
			MinimumAcknowledged = std::min(MinimumAcknowledged, Version);
		}
		for (auto EntryPosition = State.Entries.begin(); EntryPosition != State.Entries.end();) {
			if (EntryPosition->second.Version > MinimumAcknowledged) {
				++EntryPosition;
				continue;
			}
			State.EstimatedDeformableBytes -= EntryPosition->second.EstimatedDeformableBytes;
			EntryPosition = State.Entries.erase(EntryPosition);
		}
		if (State.UiVersion != 0 && State.UiVersion <= MinimumAcknowledged) {
			State.UiVersion = 0;
			State.EstimatedUiBytes = 0;
		}
		if (State.FullResyncVersion != 0 && State.FullResyncVersion <= MinimumAcknowledged) {
			State.FullResyncVersion = 0;
			State.Diagnostics.clear();
		}
	}

	void RenderDirtyAccumulator::Clear() {
		std::scoped_lock Lock(Mutex);
		Scopes.clear();
		GlobalFailureVersion = 0;
		NextGlobalVersion = 1;
		for (auto &[Consumer, Version] : ConsumerGlobalVersions) {
			(void)Consumer;
			Version = 0;
		}
	}

	std::size_t RenderDirtyAccumulator::GetPendingObjectCount(ObjectId Scope) const {
		std::scoped_lock Lock(Mutex);
		const auto Found = Scopes.find(Scope);
		return Found == Scopes.end() ? 0 : Found->second.Entries.size();
	}

	void RenderDirtyAccumulator::ResetProfile() {
		ProfileClassificationCalls.store(0, std::memory_order_relaxed);
		ProfileAccumulationCalls.store(0, std::memory_order_relaxed);
		ProfileClassificationNanoseconds.store(0, std::memory_order_relaxed);
		ProfileAccumulationNanoseconds.store(0, std::memory_order_relaxed);
	}

	RenderDirtyProfile RenderDirtyAccumulator::GetProfile() const {
		return {
			ProfileClassificationCalls.load(std::memory_order_relaxed),
			ProfileAccumulationCalls.load(std::memory_order_relaxed),
			ProfileClassificationNanoseconds.load(std::memory_order_relaxed),
			ProfileAccumulationNanoseconds.load(std::memory_order_relaxed),
		};
	}
}
