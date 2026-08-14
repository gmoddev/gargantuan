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

	void ExecutePreRunRegistration(
		RuntimeSchemaLifecycle &lifecycle,
		const RuntimeSchemaBootstrapAuthority &authority,
		std::string_view source,
		std::string sourceName,
		ScriptSecurityContext securityContext = ScriptSecurityContext::PreRunRegistration()
	);

	[[nodiscard]] std::optional<std::string> ReadProjectPreRunSource(const std::filesystem::path &projectRoot);
	[[nodiscard]] std::filesystem::path GetProjectPreRunPath(const std::filesystem::path &projectRoot);
	[[nodiscard]] std::string_view GetPreRunDiagnosticCodeName(PreRunDiagnosticCode code);
}
