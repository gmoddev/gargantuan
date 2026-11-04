#include "gargantuan/classes/LuaSourceContainer.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <limits>
#include "gargantuan/classes/Instance.hpp"

#include <cstdlib>
#include <format>
#include <optional>
#include <string>

namespace gargantuan {
	std::string LuaSourceContainer::GetSource() const {
		return Source;
	}

	int LuaSourceContainer::GetSourceVersion() const {
		return SourceVersion;
	}

	void LuaSourceContainer::SetSourceVersion(int Value) {
		AssertCanMutate();
		if (Value <= 0) throw std::invalid_argument("Script source version must be positive");
		if (SourceVersion == Value) return;
		SourceVersion = Value;
		NotifyPropertyCommitted("SourceVersion");
	}

	void LuaSourceContainer::SetSource(std::string Value) {
		AssignSource(std::move(Value), true);
	}

	void LuaSourceContainer::ApplyReplicatedSource(std::string Value) {
		// SourceVersion is a separate exact server-owned wire value. Applying
		// projected source must not advance it again or depend on property order.
		AssignSource(std::move(Value), false);
	}

	void LuaSourceContainer::AssignSource(std::string Value, bool Authoritative) {
		AssertCanMutate();
		ValidateProtocolString(Value, MaximumScriptSourceBytes, "Script source");
		ValidatePropertyMutation("Source", Value);
		if (Source == Value) return;
		if (Authoritative && SourceVersion == std::numeric_limits<int>::max())
			throw std::overflow_error("Script source version is exhausted");

		Source = std::move(Value);
		if (Authoritative) ++SourceVersion;
		Bytecode.clear();
		BytecodeSize = 0;
		BytecodeCompileStatus = BytecodeCompileStatus::Uncompiled;
		BytecodeCompileError.reset();
		if (Authoritative) {
			NotifyPropertyCommitted("SourceVersion");
			if (auto DataModelValue = GetDataModel()) DataModelValue->AdvanceAuthoritativeRevision();
		}
		GetPropertyChangedSignal("Source")->Fire({});
	}

	void LuaSourceContainer::CompileBytecode(lua_CompileOptions *options) {
		if (BytecodeCompileStatus != BytecodeCompileStatus::Uncompiled) return;

		char *rawBytecode = luau_compile(Source.c_str(), Source.length(), options, &BytecodeSize);

		if (!rawBytecode && BytecodeSize == 0) {
			BytecodeCompileStatus = BytecodeCompileStatus::Error;
			BytecodeCompileError = std::format("Failed to compile: {}", std::string(rawBytecode, BytecodeSize));
			return;
		}

		BytecodeCompileStatus = BytecodeCompileStatus::Success;
		Bytecode.assign(rawBytecode, rawBytecode + BytecodeSize);
		std::free(rawBytecode);
	};

	std::optional<std::string> LuaSourceContainer::LoadIntoState(lua_State *L) {
		if (BytecodeCompileStatus != BytecodeCompileStatus::Success) {
			return "Bytecode must be successfully compiled prior to LuaSourceContainer::LoadIntoState";
		};

		// Scripts can be named and parented after construction. Capture the live
		// Instance path so require() can restore the correct navigation context.
		ChunkName = GetFullName();
		StackValue<std::shared_ptr<Instance>>::Push(L, shared_from_this());
		lua_setglobal(L, "script");

		luaL_sandboxthread(L);

		if (luau_load(L, ChunkName.c_str(), Bytecode.data(), BytecodeSize, 0) != LUA_OK) {
			return std::format("Failed to load {}: {}", ChunkName, lua_tostring(L, -1));
		};

		return std::nullopt;
	}
}
