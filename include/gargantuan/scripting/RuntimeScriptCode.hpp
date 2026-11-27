#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan {

	inline constexpr std::size_t MaximumRuntimeScriptBytecodeBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumScriptSourceBytes = 64 * 1024;
	inline constexpr std::string_view RuntimeScriptChunkPrefix = "=GScript/";

	enum class RuntimeScriptDiagnostics { Stripped, LineInfo, Full };

	struct RuntimeScriptCompilation {
		std::string Bytecode;
		std::string Error;
	};

	[[nodiscard]] std::string_view GetRuntimeScriptCompilerIdentity() noexcept;
	[[nodiscard]] std::optional<int> GetRuntimeScriptDebugLevel(RuntimeScriptDiagnostics Diagnostics) noexcept;
	[[nodiscard]] RuntimeScriptCompilation CompileRuntimeScript(std::string_view Source, RuntimeScriptDiagnostics Diagnostics);

	class LuaSourceContainer;
	class RuntimeScriptCode final {
	  public:
		~RuntimeScriptCode();
		RuntimeScriptCode(const RuntimeScriptCode &) = delete;
		RuntimeScriptCode &operator=(const RuntimeScriptCode &) = delete;

	  private:
		RuntimeScriptCode(std::string Bytecode, std::string PublicScriptId);

		std::string Bytecode;
		std::string PublicScriptId;

		friend class LuaSourceContainer;
		friend std::shared_ptr<const RuntimeScriptCode> AdmitTrustedRuntimeScriptCode(
			std::string Bytecode, std::string_view CompilerIdentity, std::string PublicScriptId
		);
	};

	// For native callers after exact compiler provenance is established by
	// package admission. Hashes from an unsigned manifest do not establish it.
	// Header checks below that trust boundary do not validate arbitrary bytecode.
	[[nodiscard]] std::shared_ptr<const RuntimeScriptCode> AdmitTrustedRuntimeScriptCode(
		std::string Bytecode, std::string_view CompilerIdentity, std::string PublicScriptId
	);

}
