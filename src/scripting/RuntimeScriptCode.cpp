#include "gargantuan/scripting/RuntimeScriptCode.hpp"

#include "DiagnosticText.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <Luau/Bytecode.h>
#include <Luau/Compiler.h>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		constexpr unsigned char RuntimeBytecodeVersion = 9;
		constexpr unsigned char RuntimeBytecodeTypeVersion = 3;
		static_assert(LBC_VERSION_TARGET == RuntimeBytecodeVersion);
		static_assert(LBC_TYPE_VERSION_TARGET == RuntimeBytecodeTypeVersion);

		bool HasCompatibleBytecodeHeader(std::string_view Bytecode) noexcept {
			return Bytecode.size() > 2 && Bytecode.size() <= MaximumRuntimeScriptBytecodeBytes &&
				static_cast<unsigned char>(Bytecode[0]) == RuntimeBytecodeVersion &&
				static_cast<unsigned char>(Bytecode[1]) == RuntimeBytecodeTypeVersion;
		}

		void ClearBytecode(std::string &Bytecode) noexcept {
			volatile char *Bytes = Bytecode.data();
			for (std::size_t Index = 0; Index < Bytecode.size(); ++Index) Bytes[Index] = 0;
		}
	}

	std::string_view GetRuntimeScriptCompilerIdentity() noexcept {
		return "Luau/3fc82b1071ab387531175869afc4fb528464afa4;Bytecode=9;Types=3;Profile=Runtime1-O0-T0-Vector3F32;Flags=LibraryDefaults";
	}

	std::optional<int> GetRuntimeScriptDebugLevel(RuntimeScriptDiagnostics Diagnostics) noexcept {
		switch (Diagnostics) {
		case RuntimeScriptDiagnostics::Stripped: return 0;
		case RuntimeScriptDiagnostics::LineInfo: return 1;
		case RuntimeScriptDiagnostics::Full: return 2;
		}
		return std::nullopt;
	}

	RuntimeScriptCompilation CompileRuntimeScript(std::string_view Source, RuntimeScriptDiagnostics Diagnostics) {
		try {
			ValidateProtocolString(Source, MaximumScriptSourceBytes, "Script source");
		} catch (const std::invalid_argument &Error) {
			return {{}, Error.what()};
		}

		Luau::CompileOptions Options;
		Options.optimizationLevel = 0;
		Options.typeInfoLevel = 0;
		Options.coverageLevel = 0;
		Options.vectorLib = "Vector3";
		Options.vectorCtor = "new";
		Options.vectorType = "Vector3";
		Options.vectorPrecision = 0;
		const auto DebugLevel = GetRuntimeScriptDebugLevel(Diagnostics);
		if (!DebugLevel) return {{}, "Unsupported script diagnostic policy"};
		Options.debugLevel = *DebugLevel;

		auto Bytecode = Luau::compile(std::string(Source), Options);
		if (Bytecode.empty()) return {{}, "Luau compilation produced no bytecode"};
		if (Bytecode.front() == '\0')
			return {{}, BoundedDiagnosticText(std::string_view(Bytecode).substr(1), 2048)};
		if (!HasCompatibleBytecodeHeader(Bytecode))
			return {{}, "Luau compiler output exceeds the script limit or uses an incompatible bytecode version"};
		return {std::move(Bytecode), {}};
	}

	RuntimeScriptCode::RuntimeScriptCode(std::string Bytecode, std::string PublicScriptId)
		: Bytecode(std::move(Bytecode)), PublicScriptId(std::move(PublicScriptId)) {}

	RuntimeScriptCode::~RuntimeScriptCode() {
		ClearBytecode(Bytecode);
	}

	std::shared_ptr<const RuntimeScriptCode> AdmitTrustedRuntimeScriptCode(
		std::string Bytecode, std::string_view CompilerIdentity, std::string PublicScriptId
	) {
		if (CompilerIdentity != GetRuntimeScriptCompilerIdentity() || !HasCompatibleBytecodeHeader(Bytecode) ||
			!ProjectId::Parse(PublicScriptId)) {
			ClearBytecode(Bytecode);
			throw std::invalid_argument("Incompatible compiler identity, bytecode or script diagnostic identity");
		}
		return std::shared_ptr<const RuntimeScriptCode>(new RuntimeScriptCode(std::move(Bytecode), std::move(PublicScriptId)));
	}
}
