#pragma once

#include "gargantuan/classes/generated/LuaSourceContainer.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/scripting/RuntimeScriptCode.hpp"

#include <SDL3/SDL.h>
#include <lua.h>
#include <luacode.h>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace gargantuan {
	enum class BytecodeCompileStatus : int { Uncompiled, Success, Error };

	class LuaSourceContainer : public Instance {
		I_LuaSourceContainer;

	  private:
		std::string SourceId = ProjectId::New().ToString();
		bool SourceIdLoaded = false;
		std::string GuiTemplateSourceId;
		std::string GuiTemplateSourceDigest;
		int GuiTemplateSourceVersion = 0;
		std::string Source;
		int SourceVersion = 1;
		std::string Bytecode;
		std::shared_ptr<const RuntimeScriptCode> RuntimeCode;
		std::optional<RuntimeScriptDiagnostics> RuntimeSourceDiagnosticPolicy;
		void AssignSource(std::string Value, bool Authoritative);

	  protected:
		void ApplyReplicatedSource(std::string Value);

	  public:
		[[nodiscard]] bool WasSourceIdLoaded() const { return SourceIdLoaded; }
		[[nodiscard]] bool HasSource() const { return !Source.empty(); }
		[[nodiscard]] bool HasRuntimeScriptCode() const { return static_cast<bool>(RuntimeCode); }
		[[nodiscard]] bool HasExecutableCode() const { return HasRuntimeScriptCode() || HasSource(); }
		[[nodiscard]] std::string GetRuntimeScriptChunkName() const;
		[[nodiscard]] std::string GetScriptDiagnosticName();
		void BindRuntimeScriptCode(std::shared_ptr<const RuntimeScriptCode> Code);
		void SetRuntimeSourceDiagnostics(RuntimeScriptDiagnostics Diagnostics);
		void CopyRuntimeScriptCodeTo(LuaSourceContainer &Target) const;
		[[nodiscard]] std::string GetSourceDigest() const;
		bool HydrateGuiTemplateSource(std::string Value);
		std::string ChunkName = GetFullName();

		BytecodeCompileStatus BytecodeCompileStatus = BytecodeCompileStatus::Uncompiled;
		std::optional<std::string> BytecodeCompileError;

		void CompileBytecode(lua_CompileOptions *Options = nullptr);
		[[nodiscard]] std::optional<std::string> LoadBytecodeIntoState(lua_State *L, const char *SourceChunkName);
		[[nodiscard]] std::optional<std::string> LoadIntoState(lua_State *L);
	};

	template <typename T>
		requires std::is_base_of_v<LuaSourceContainer, T> && (!std::is_same_v<LuaSourceContainer, T>)
	[[nodiscard]] static std::shared_ptr<T> ScriptFromFile(const char *filepath) {
		size_t fileSize;
		void *code = SDL_LoadFile(filepath, &fileSize);

		if (code == nullptr) {
			throw std::runtime_error(
				std::format(
					"Failed to load {} from file {}: {}", T::CLASS_DEFINITION.ClassName, filepath, SDL_GetError()
				)
			);
		};

		std::string source((char *)code, fileSize);
		SDL_free(code);

		auto self = std::make_shared<T>();
		self->ChunkName = std::string(filepath);
		self->SetSource(source);
		return self;
	};
}
