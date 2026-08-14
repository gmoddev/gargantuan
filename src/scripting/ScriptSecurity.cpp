// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		thread_local ScriptSecurityContext CurrentContext = ScriptSecurityContext::CoreTrusted();

		constexpr std::uint64_t CapabilityBit(ScriptCapability capability) {
			return static_cast<std::uint64_t>(capability);
		}
	}

	ScriptDomainSet::ScriptDomainSet(std::initializer_list<ScriptExecutionDomain> domains) {
		for (const auto domain : domains) Mask |= static_cast<std::uint8_t>(1u << static_cast<unsigned>(domain));
	}

	bool ScriptDomainSet::Contains(ScriptExecutionDomain domain) const {
		return (Mask & static_cast<std::uint8_t>(1u << static_cast<unsigned>(domain))) != 0;
	}

	ScriptDomainSet ScriptDomainSet::All() {
		return {ScriptExecutionDomain::Core, ScriptExecutionDomain::Studio,
			ScriptExecutionDomain::Server, ScriptExecutionDomain::Client};
	}

	ScriptCapabilitySet::ScriptCapabilitySet(std::initializer_list<ScriptCapability> capabilities) {
		for (const auto capability : capabilities) Add(capability);
	}

	bool ScriptCapabilitySet::Contains(ScriptCapability capability) const {
		return capability == ScriptCapability::None || (Mask & CapabilityBit(capability)) != 0;
	}

	void ScriptCapabilitySet::Add(ScriptCapability capability) { Mask |= CapabilityBit(capability); }

	ScriptCapabilitySet ScriptCapabilitySet::AllDefined() {
		return {
			ScriptCapability::ReadDataModel,
			ScriptCapability::MutateDataModel,
			ScriptCapability::EditorCommands,
			ScriptCapability::SelectionAccess,
			ScriptCapability::ViewportControl,
			ScriptCapability::FilesystemRead,
			ScriptCapability::FilesystemWrite,
			ScriptCapability::ProcessControl,
			ScriptCapability::NetworkSend,
			ScriptCapability::NetworkReceive,
		};
	}

	bool ScriptSecurityContext::HasCapability(ScriptCapability capability) const {
		return Capabilities.Contains(capability);
	}

	ScriptSecurityContext ScriptSecurityContext::CoreTrusted() {
		return {ScriptExecutionDomain::Core, ScriptCapabilitySet::AllDefined()};
	}

	ScriptSecurityContext ScriptSecurityContext::StudioCoreUi() {
		return {
			ScriptExecutionDomain::Studio,
			{
				ScriptCapability::ReadDataModel,
				ScriptCapability::MutateDataModel,
				ScriptCapability::EditorCommands,
				ScriptCapability::SelectionAccess,
				ScriptCapability::ViewportControl,
			},
		};
	}

	ScriptSecurityScope::ScriptSecurityScope(ScriptSecurityContext context) : Previous(CurrentContext) {
		CurrentContext = std::move(context);
	}

	ScriptSecurityScope::~ScriptSecurityScope() { CurrentContext = std::move(Previous); }

	const ScriptSecurityContext &GetCurrentScriptSecurityContext() { return CurrentContext; }

	std::string_view GetScriptExecutionDomainName(ScriptExecutionDomain domain) {
		switch (domain) {
			case ScriptExecutionDomain::Core: return "Core";
			case ScriptExecutionDomain::Studio: return "Studio";
			case ScriptExecutionDomain::Server: return "Server";
			case ScriptExecutionDomain::Client: return "Client";
		}
		throw std::invalid_argument("Unknown script execution domain");
	}

	std::string_view GetScriptCapabilityName(ScriptCapability capability) {
		switch (capability) {
			case ScriptCapability::None: return "None";
			case ScriptCapability::ReadDataModel: return "ReadDataModel";
			case ScriptCapability::MutateDataModel: return "MutateDataModel";
			case ScriptCapability::EditorCommands: return "EditorCommands";
			case ScriptCapability::SelectionAccess: return "SelectionAccess";
			case ScriptCapability::ViewportControl: return "ViewportControl";
			case ScriptCapability::FilesystemRead: return "FilesystemRead";
			case ScriptCapability::FilesystemWrite: return "FilesystemWrite";
			case ScriptCapability::ProcessControl: return "ProcessControl";
			case ScriptCapability::NetworkSend: return "NetworkSend";
			case ScriptCapability::NetworkReceive: return "NetworkReceive";
		}
		throw std::invalid_argument("Unknown script capability");
	}
}
