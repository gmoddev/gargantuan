// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/reflection/RuntimeSchema.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <typeindex>

namespace gargantuan {
	using RuntimeSchemaGeneration = std::uint64_t;
	inline constexpr RuntimeSchemaGeneration InvalidRuntimeSchemaGeneration = 0;

	enum class RuntimeSchemaLifecyclePhase : std::uint8_t {
		Bootstrap,
		NativeRegistration,
		CoreRegistration,
		ExternalRegistration,
		Validation,
		Frozen,
		Runtime,
	};

	class RuntimeSchemaBootstrapAuthority {
	  private:
		RuntimeSchemaBootstrapAuthority() = default;
		friend const RuntimeSchemaBootstrapAuthority &GetRuntimeSchemaBootstrapAuthority();
	};

	class RuntimeSchemaLifecycle {
	  public:
		RuntimeSchemaLifecycle() = default;

		void BeginCandidate(const RuntimeSchemaBootstrapAuthority &authority);
		void RegisterNative(
			const RuntimeSchemaBootstrapAuthority &authority,
			std::type_index nativeType,
			SchemaDefinition definition
		);

		template <typename T>
		void RegisterNative(const RuntimeSchemaBootstrapAuthority &authority, SchemaDefinition definition) {
			RegisterNative(authority, std::type_index(typeid(T)), std::move(definition));
		}

		void AdvanceRegistrationPhase(
			const RuntimeSchemaBootstrapAuthority &authority,
			RuntimeSchemaLifecyclePhase nextPhase
		);
		void ValidateCandidate(const RuntimeSchemaBootstrapAuthority &authority);
		void FreezeCandidate(const RuntimeSchemaBootstrapAuthority &authority);
		void PublishCandidate(const RuntimeSchemaBootstrapAuthority &authority);
		void AbortCandidate(const RuntimeSchemaBootstrapAuthority &authority);

		[[nodiscard]] RuntimeSchemaLifecyclePhase GetPhase() const { return Phase; }
		[[nodiscard]] RuntimeSchemaGeneration GetActiveGeneration() const { return ActiveGeneration; }
		[[nodiscard]] bool HasActiveRegistry() const { return static_cast<bool>(ActiveRegistry); }
		[[nodiscard]] bool HasCandidate() const { return static_cast<bool>(CandidateRegistry); }
		[[nodiscard]] std::shared_ptr<const RuntimeSchemaRegistry> GetActiveRegistry() const;
		void RequireRuntimeReady(std::string_view operation) const;

	  private:
		RuntimeSchemaLifecyclePhase Phase = RuntimeSchemaLifecyclePhase::Bootstrap;
		RuntimeSchemaGeneration ActiveGeneration = InvalidRuntimeSchemaGeneration;
		std::shared_ptr<RuntimeSchemaRegistry> CandidateRegistry;
		std::shared_ptr<const RuntimeSchemaRegistry> ActiveRegistry;

		void RequirePhase(RuntimeSchemaLifecyclePhase expected, std::string_view operation) const;
		void RejectCandidate(std::string_view operation, std::string_view reason);
	};

	void RegisterNativeSchemaSeed(std::type_index nativeType, SchemaDefinition definition);
	void BootstrapNativeRuntimeSchema();
	void RequireFrozenRuntimeSchema(std::string_view operation);

	[[nodiscard]] const RuntimeSchemaBootstrapAuthority &GetRuntimeSchemaBootstrapAuthority();
	[[nodiscard]] const RuntimeSchemaLifecycle &GetRuntimeSchemaLifecycle();
	[[nodiscard]] const RuntimeSchemaRegistry &GetActiveRuntimeSchemaRegistry();
	[[nodiscard]] std::string_view GetRuntimeSchemaLifecyclePhaseName(RuntimeSchemaLifecyclePhase phase);
}
