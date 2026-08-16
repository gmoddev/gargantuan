// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace gargantuan {
	enum class ScriptExecutionDomain : std::uint8_t { Core, PreRun, Studio, Server, Client };

	enum class ScriptCapability : std::uint64_t {
		None = 0,
		ReadDataModel = 1ull << 0,
		MutateDataModel = 1ull << 1,
		EditorCommands = 1ull << 2,
		SelectionAccess = 1ull << 3,
		ViewportControl = 1ull << 4,
		FilesystemRead = 1ull << 5,
		FilesystemWrite = 1ull << 6,
		ProcessControl = 1ull << 7,
		NetworkSend = 1ull << 8,
		NetworkReceive = 1ull << 9,
		DefineSchema = 1ull << 10,
	};

	class ScriptDomainSet {
	  public:
		ScriptDomainSet() = default;
		ScriptDomainSet(std::initializer_list<ScriptExecutionDomain> domains);

		[[nodiscard]] bool Contains(ScriptExecutionDomain domain) const;
		[[nodiscard]] static ScriptDomainSet All();

	  private:
		std::uint8_t Mask = 0;
	};

	class ScriptCapabilitySet {
	  public:
		ScriptCapabilitySet() = default;
		ScriptCapabilitySet(std::initializer_list<ScriptCapability> capabilities);

		[[nodiscard]] bool Contains(ScriptCapability capability) const;
		void Add(ScriptCapability capability);
		[[nodiscard]] static ScriptCapabilitySet AllDefined();

	  private:
		std::uint64_t Mask = 0;
	};

	struct ScriptSecurityContext {
		ScriptExecutionDomain Domain = ScriptExecutionDomain::Core;
		ScriptCapabilitySet Capabilities;
		bool AllowsTaskSchedulerYield = true;

		[[nodiscard]] bool HasCapability(ScriptCapability capability) const;
		[[nodiscard]] static ScriptSecurityContext CoreTrusted();
		[[nodiscard]] static ScriptSecurityContext PreRunRegistration();
		[[nodiscard]] static ScriptSecurityContext StudioCoreUi();
	};

	class ScriptSecurityScope {
	  public:
		explicit ScriptSecurityScope(ScriptSecurityContext context);
		~ScriptSecurityScope();

		ScriptSecurityScope(const ScriptSecurityScope &) = delete;
		ScriptSecurityScope &operator=(const ScriptSecurityScope &) = delete;

	  private:
		ScriptSecurityContext Previous;
	};

	[[nodiscard]] const ScriptSecurityContext &GetCurrentScriptSecurityContext();
	[[nodiscard]] std::string_view GetScriptExecutionDomainName(ScriptExecutionDomain domain);
	[[nodiscard]] std::string_view GetScriptCapabilityName(ScriptCapability capability);
}
