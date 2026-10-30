// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gargantuan {
	class RuntimeSchemaLifecycle;
	class RuntimeSchemaBootstrapAuthority;

	inline constexpr std::size_t MaximumPreRunSourceBytes = 256 * 1024;
	inline constexpr std::size_t MaximumPreRunMemoryBytes = 16 * 1024 * 1024;
	inline constexpr auto MaximumPreRunExecutionTime = std::chrono::milliseconds(250);

	enum class PreRunDiagnosticCode {
		SourceTooLarge,
		CompileError,
		ExecutionBudgetExceeded,
		MemoryBudgetExceeded,
		SchemaRegistrationError,
		RuntimeError,
	};

	struct PreRunDiagnostic {
		PreRunDiagnosticCode Code = PreRunDiagnosticCode::RuntimeError;
		std::string Source;
		std::string Definition;
		std::string Message;
	};

	class PreRunRegistrationError : public std::runtime_error {
	  public:
		explicit PreRunRegistrationError(PreRunDiagnostic diagnostic);
		[[nodiscard]] const PreRunDiagnostic &GetDiagnostic() const { return Diagnostic; }

	  private:
		PreRunDiagnostic Diagnostic;
	};

	class PreRunSourceConflict : public std::runtime_error {
	  public:
		using std::runtime_error::runtime_error;
	};

	void ExecutePreRunRegistration(
		RuntimeSchemaLifecycle &lifecycle,
		const RuntimeSchemaBootstrapAuthority &authority,
		std::string_view source,
		std::string sourceName,
		ScriptSecurityContext securityContext = ScriptSecurityContext::PreRunRegistration()
	);

	[[nodiscard]] std::optional<std::string> ReadProjectPreRunSource(const std::filesystem::path &projectRoot);
	// Builds, validates and freezes a disposable candidate. Never publishes it.
	[[nodiscard]] std::optional<PreRunDiagnostic> ValidatePreRunSource(std::string_view Source);
	[[nodiscard]] std::filesystem::path GetProjectPreRunPath(const std::filesystem::path &projectRoot);
	// Fixed canonical resource only; rejects linked/reparse path components.
	[[nodiscard]] std::filesystem::path ResolveProjectPreRunPath(const std::filesystem::path &ProjectRoot);
	[[nodiscard]] std::string_view GetPreRunDiagnosticCodeName(PreRunDiagnosticCode code);
}
