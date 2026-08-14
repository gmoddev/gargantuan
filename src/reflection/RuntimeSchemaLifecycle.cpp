// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		struct NativeSchemaSeed {
			std::type_index NativeType;
			SchemaClassDefinition Definition;
		};

		std::vector<NativeSchemaSeed> &GetNativeSchemaSeeds() {
			static std::vector<NativeSchemaSeed> seeds;
			return seeds;
		}

		std::mutex &GetNativeSchemaSeedMutex() {
			static std::mutex mutex;
			return mutex;
		}

		RuntimeSchemaLifecycle &GetMutableRuntimeSchemaLifecycle() {
			static RuntimeSchemaLifecycle lifecycle;
			return lifecycle;
		}

		std::string CanonicalName(const SchemaClassDefinition &definition) {
			return definition.Namespace + "." + definition.ClassName;
		}
	}

	void RuntimeSchemaLifecycle::RequirePhase(
		RuntimeSchemaLifecyclePhase expected,
		std::string_view operation
	) const {
		if (Phase != expected)
			throw std::logic_error(
				"Runtime schema lifecycle cannot " + std::string(operation) + " during " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(Phase)) + "; expected " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(expected))
			);
	}

	void RuntimeSchemaLifecycle::BeginCandidate(const RuntimeSchemaBootstrapAuthority &) {
		if (Phase != RuntimeSchemaLifecyclePhase::Bootstrap && Phase != RuntimeSchemaLifecyclePhase::Runtime)
			throw std::logic_error(
				"Runtime schema lifecycle cannot begin a candidate during " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(Phase))
			);
		if (CandidateRegistry) throw std::logic_error("Runtime schema lifecycle already has a candidate");
		CandidateRegistry = std::make_shared<RuntimeSchemaRegistry>();
		Phase = RuntimeSchemaLifecyclePhase::NativeRegistration;
	}

	void RuntimeSchemaLifecycle::RejectCandidate(std::string_view operation, std::string_view reason) {
		const auto rejectedPhase = Phase;
		CandidateRegistry.reset();
		Phase = ActiveRegistry ? RuntimeSchemaLifecyclePhase::Runtime : RuntimeSchemaLifecyclePhase::Bootstrap;
		throw std::runtime_error(
			"Runtime schema candidate " + std::string(operation) + " failed during " +
			std::string(GetRuntimeSchemaLifecyclePhaseName(rejectedPhase)) + ": " + std::string(reason) +
			"; candidate publication aborted"
		);
	}

	void RuntimeSchemaLifecycle::RegisterNative(
		const RuntimeSchemaBootstrapAuthority &,
		std::type_index nativeType,
		SchemaClassDefinition definition
	) {
		RequirePhase(RuntimeSchemaLifecyclePhase::NativeRegistration, "register a native definition");
		try {
			CandidateRegistry->RegisterNative(nativeType, std::move(definition));
		} catch (const std::exception &exception) {
			RejectCandidate("registration", exception.what());
		}
	}

	void RuntimeSchemaLifecycle::RegisterEnum(
		const RuntimeSchemaBootstrapAuthority &,
		SchemaEnumDefinition definition,
		const ScriptSecurityContext &securityContext
	) {
		RequirePhase(RuntimeSchemaLifecyclePhase::PreRunRegistration, "register an enum definition");
		if (!securityContext.HasCapability(ScriptCapability::DefineSchema))
			RejectCandidate("registration", "enum definition requires DefineSchema capability");
		try {
			CandidateRegistry->RegisterEnum(std::move(definition));
		} catch (const std::exception &exception) {
			RejectCandidate("registration", exception.what());
		}
	}

	void RuntimeSchemaLifecycle::AdvanceRegistrationPhase(
		const RuntimeSchemaBootstrapAuthority &,
		RuntimeSchemaLifecyclePhase nextPhase
	) {
		RuntimeSchemaLifecyclePhase expected;
		switch (Phase) {
			case RuntimeSchemaLifecyclePhase::NativeRegistration:
				expected = RuntimeSchemaLifecyclePhase::CoreRegistration;
				break;
			case RuntimeSchemaLifecyclePhase::CoreRegistration:
				expected = RuntimeSchemaLifecyclePhase::PreRunRegistration;
				break;
			case RuntimeSchemaLifecyclePhase::PreRunRegistration:
				expected = RuntimeSchemaLifecyclePhase::Validation;
				break;
			default:
				throw std::logic_error(
					"Runtime schema lifecycle cannot advance registration during " +
					std::string(GetRuntimeSchemaLifecyclePhaseName(Phase))
				);
		}
		if (nextPhase != expected)
			throw std::logic_error(
				"Invalid runtime schema lifecycle transition from " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(Phase)) + " to " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(nextPhase)) + "; expected " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(expected))
			);
		Phase = nextPhase;
	}

	void RuntimeSchemaLifecycle::ValidateCandidate(const RuntimeSchemaBootstrapAuthority &) {
		RequirePhase(RuntimeSchemaLifecyclePhase::Validation, "validate a candidate");
		try {
			CandidateRegistry->Validate();
		} catch (const std::exception &exception) {
			RejectCandidate("validation", exception.what());
		}
	}

	void RuntimeSchemaLifecycle::FreezeCandidate(const RuntimeSchemaBootstrapAuthority &) {
		RequirePhase(RuntimeSchemaLifecyclePhase::Validation, "freeze a candidate");
		try {
			CandidateRegistry->Freeze();
		} catch (const std::exception &exception) {
			RejectCandidate("freeze", exception.what());
		}
		Phase = RuntimeSchemaLifecyclePhase::Frozen;
	}

	void RuntimeSchemaLifecycle::PublishCandidate(const RuntimeSchemaBootstrapAuthority &) {
		RequirePhase(RuntimeSchemaLifecyclePhase::Frozen, "publish a candidate");
		if (!CandidateRegistry || !CandidateRegistry->IsFrozen())
			RejectCandidate("publication", "candidate is not frozen");
		if (ActiveGeneration == std::numeric_limits<RuntimeSchemaGeneration>::max())
			RejectCandidate("publication", "registry generation exhausted and will not roll over");

		const auto nextGeneration = ActiveGeneration + 1;
		std::shared_ptr<const RuntimeSchemaRegistry> published = CandidateRegistry;
		ActiveRegistry = std::move(published);
		ActiveGeneration = nextGeneration;
		CandidateRegistry.reset();
		Phase = RuntimeSchemaLifecyclePhase::Runtime;
	}

	void RuntimeSchemaLifecycle::AbortCandidate(const RuntimeSchemaBootstrapAuthority &) {
		if (!CandidateRegistry)
			throw std::logic_error("Runtime schema lifecycle cannot abort without a candidate");
		CandidateRegistry.reset();
		Phase = ActiveRegistry ? RuntimeSchemaLifecyclePhase::Runtime : RuntimeSchemaLifecyclePhase::Bootstrap;
	}

	std::shared_ptr<const RuntimeSchemaRegistry> RuntimeSchemaLifecycle::GetActiveRegistry() const {
		if (!ActiveRegistry)
			throw std::logic_error(
				"Runtime schema lifecycle has no active registry during " +
				std::string(GetRuntimeSchemaLifecyclePhaseName(Phase))
			);
		return ActiveRegistry;
	}

	void RuntimeSchemaLifecycle::RequireRuntimeReady(std::string_view operation) const {
		if (Phase != RuntimeSchemaLifecyclePhase::Runtime || !ActiveRegistry || !ActiveRegistry->IsFrozen())
			throw std::logic_error(
				"Runtime schema must be frozen and published before " + std::string(operation) +
				"; current phase is " + std::string(GetRuntimeSchemaLifecyclePhaseName(Phase))
			);
	}

	void RegisterNativeSchemaSeed(std::type_index nativeType, SchemaClassDefinition definition) {
		std::scoped_lock lock(GetNativeSchemaSeedMutex());
		if (GetMutableRuntimeSchemaLifecycle().HasActiveRegistry())
			throw std::logic_error("Native schema seed registration occurred after runtime schema publication");
		GetNativeSchemaSeeds().push_back({nativeType, std::move(definition)});
	}

	void BootstrapNativeRuntimeSchema() {
		auto &lifecycle = GetMutableRuntimeSchemaLifecycle();
		const auto &authority = GetRuntimeSchemaBootstrapAuthority();
		if (lifecycle.HasActiveRegistry())
			throw std::logic_error("Native runtime schema bootstrap has already completed");

		std::vector<NativeSchemaSeed> seeds;
		{
			std::scoped_lock lock(GetNativeSchemaSeedMutex());
			seeds = GetNativeSchemaSeeds();
		}
		std::sort(seeds.begin(), seeds.end(), [](const auto &left, const auto &right) {
			const auto leftName = CanonicalName(left.Definition);
			const auto rightName = CanonicalName(right.Definition);
			if (leftName != rightName) return leftName < rightName;
			if (left.Definition.Id != right.Definition.Id)
				return left.Definition.Id.ToString() < right.Definition.Id.ToString();
			return std::string(left.NativeType.name()) < std::string(right.NativeType.name());
		});

		lifecycle.BeginCandidate(authority);
		try {
			for (auto &seed : seeds)
				lifecycle.RegisterNative(authority, seed.NativeType, std::move(seed.Definition));
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
			lifecycle.ValidateCandidate(authority);
			lifecycle.FreezeCandidate(authority);
			lifecycle.PublishCandidate(authority);
		} catch (...) {
			if (lifecycle.HasCandidate()) lifecycle.AbortCandidate(authority);
			throw;
		}
	}

	void BootstrapProjectRuntimeSchema(const std::filesystem::path &projectRoot) {
		auto &lifecycle = GetMutableRuntimeSchemaLifecycle();
		const auto &authority = GetRuntimeSchemaBootstrapAuthority();

		std::vector<NativeSchemaSeed> nativeDefinitions;
		{
			std::scoped_lock lock(GetNativeSchemaSeedMutex());
			nativeDefinitions = GetNativeSchemaSeeds();
		}
		if (nativeDefinitions.empty()) throw std::logic_error("Native schema seeds are unavailable for project registration");
		std::sort(nativeDefinitions.begin(), nativeDefinitions.end(), [](const auto &left, const auto &right) {
			return CanonicalName(left.Definition) < CanonicalName(right.Definition);
		});

		lifecycle.BeginCandidate(authority);
		try {
			for (auto &seed : nativeDefinitions)
				lifecycle.RegisterNative(authority, seed.NativeType, std::move(seed.Definition));
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
			if (auto source = ReadProjectPreRunSource(projectRoot))
				ExecutePreRunRegistration(lifecycle, authority, *source, GetProjectPreRunPath(projectRoot).string());
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
			lifecycle.ValidateCandidate(authority);
			lifecycle.FreezeCandidate(authority);
			lifecycle.PublishCandidate(authority);
		} catch (...) {
			if (lifecycle.HasCandidate()) lifecycle.AbortCandidate(authority);
			throw;
		}
	}

	void RequireFrozenRuntimeSchema(std::string_view operation) {
		GetMutableRuntimeSchemaLifecycle().RequireRuntimeReady(operation);
	}

	const RuntimeSchemaBootstrapAuthority &GetRuntimeSchemaBootstrapAuthority() {
		static RuntimeSchemaBootstrapAuthority authority;
		return authority;
	}

	const RuntimeSchemaLifecycle &GetRuntimeSchemaLifecycle() { return GetMutableRuntimeSchemaLifecycle(); }

	const RuntimeSchemaRegistry &GetActiveRuntimeSchemaRegistry() {
		return *GetMutableRuntimeSchemaLifecycle().GetActiveRegistry();
	}

	std::string_view GetRuntimeSchemaLifecyclePhaseName(RuntimeSchemaLifecyclePhase phase) {
		switch (phase) {
			case RuntimeSchemaLifecyclePhase::Bootstrap: return "Bootstrap";
			case RuntimeSchemaLifecyclePhase::NativeRegistration: return "NativeRegistration";
			case RuntimeSchemaLifecyclePhase::CoreRegistration: return "CoreRegistration";
			case RuntimeSchemaLifecyclePhase::PreRunRegistration: return "PreRunRegistration";
			case RuntimeSchemaLifecyclePhase::Validation: return "Validation";
			case RuntimeSchemaLifecyclePhase::Frozen: return "Frozen";
			case RuntimeSchemaLifecyclePhase::Runtime: return "Runtime";
		}
		throw std::invalid_argument("Unknown runtime schema lifecycle phase");
	}
}
