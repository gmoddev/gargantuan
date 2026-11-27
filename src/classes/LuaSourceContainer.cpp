#include "gargantuan/classes/LuaSourceContainer.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "../scripting/DiagnosticText.hpp"

#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <string>

namespace gargantuan {
	std::string LuaSourceContainer::GetSourceId() const { return SourceId; }
	void LuaSourceContainer::SetSourceId(std::string Value) {
		AssertCanMutate();
		if (!ProjectId::Parse(Value)) throw std::invalid_argument("Script SourceId must be canonical nonzero 128-bit hex");
		// Only hydration of an unadopted native Instance may restore identity.
		// Public Luau and EditorHost property writes also fail Engine permission.
		if (GetDataModel() || SourceIdLoaded) throw std::invalid_argument("Script SourceId is immutable after hydration");
		SourceId = std::move(Value);
		SourceIdLoaded = true;
	}

	std::string LuaSourceContainer::GetGuiTemplateSourceId() const { return GuiTemplateSourceId; }

	void LuaSourceContainer::SetGuiTemplateSourceId(std::string Value) {
		AssertCanMutate();
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core)
			throw std::invalid_argument("GUI template source provenance is engine-owned");
		if (Value == GuiTemplateSourceId) return;
		if (!GuiTemplateSourceId.empty() || !ProjectId::Parse(Value))
			throw std::invalid_argument("GUI template source provenance is immutable and requires a canonical SourceId");
		GuiTemplateSourceId = std::move(Value);
		NotifyPropertyCommitted("GuiTemplateSourceId");
	}

	bool LuaSourceContainer::HydrateGuiTemplateSource(std::string Value) {
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core ||
			RuntimeCode ||
			GuiTemplateSourceId.empty() || GuiTemplateSourceDigest.empty() || GuiTemplateSourceVersion != SourceVersion ||
			(!Source.empty() && Source != Value)) return false;
		AssignSource(std::move(Value), false);
		return true;
	}

	std::string LuaSourceContainer::GetSourceDigest() const {
		return AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Source.data()), Source.size()
		)).ToString();
	}

	std::string LuaSourceContainer::GetGuiTemplateSourceDigest() const { return GuiTemplateSourceDigest; }

	void LuaSourceContainer::SetGuiTemplateSourceDigest(std::string Value) {
		AssertCanMutate();
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core)
			throw std::invalid_argument("GUI template source digest is engine-owned");
		if (Value == GuiTemplateSourceDigest) return;
		if (!GuiTemplateSourceDigest.empty() || !AssetContentId::Parse(Value))
			throw std::invalid_argument("GUI template source digest is immutable and requires a canonical digest");
		GuiTemplateSourceDigest = std::move(Value);
		NotifyPropertyCommitted("GuiTemplateSourceDigest");
	}

	int LuaSourceContainer::GetGuiTemplateSourceVersion() const { return GuiTemplateSourceVersion; }

	void LuaSourceContainer::SetGuiTemplateSourceVersion(int Value) {
		AssertCanMutate();
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core)
			throw std::invalid_argument("GUI template source version is engine-owned");
		if (Value == GuiTemplateSourceVersion) return;
		if (GuiTemplateSourceVersion != 0 || Value <= 0)
			throw std::invalid_argument("GUI template source version is immutable and must be positive");
		GuiTemplateSourceVersion = Value;
		NotifyPropertyCommitted("GuiTemplateSourceVersion");
	}

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
		if (RuntimeCode) throw std::invalid_argument("Packaged script source version is immutable");
		SourceVersion = Value;
		if (IsReplicaMaterialization() && GuiTemplateSourceVersion != 0 && GuiTemplateSourceVersion != Value)
			AssignSource({}, false);
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
		if (RuntimeCode) throw std::invalid_argument("Packaged script cannot accept replacement source");
		if (Authoritative && SourceVersion == std::numeric_limits<int>::max())
			throw std::overflow_error("Script source version is exhausted");

		Source = std::move(Value);
		if (Authoritative) ++SourceVersion;
		Bytecode.clear();
		BytecodeCompileStatus = BytecodeCompileStatus::Uncompiled;
		BytecodeCompileError.reset();
		if (Authoritative) {
			NotifyPropertyCommitted("SourceVersion");
			if (auto DataModelValue = GetDataModel()) DataModelValue->AdvanceAuthoritativeRevision();
		}
		GetPropertyChangedSignal("Source")->Fire({});
	}

	void LuaSourceContainer::BindRuntimeScriptCode(std::shared_ptr<const RuntimeScriptCode> Code) {
		AssertCanMutate();
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core || !Code)
			throw std::invalid_argument("Runtime script code requires a trusted binding");
		if (RuntimeCode == Code) return;
		if (RuntimeCode ||
			RuntimeSourceDiagnosticPolicy || HasSource() || BytecodeCompileStatus != BytecodeCompileStatus::Uncompiled)
			throw std::invalid_argument("Runtime script code requires a trusted binding to an unstarted source-free script");
		RuntimeCode = std::move(Code);
		BytecodeCompileStatus = BytecodeCompileStatus::Success;
	}

	void LuaSourceContainer::SetRuntimeSourceDiagnostics(RuntimeScriptDiagnostics Diagnostics) {
		AssertCanMutate();
		if (GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Core || !GetRuntimeScriptDebugLevel(Diagnostics))
			throw std::invalid_argument("Packaged source diagnostics require a trusted policy");
		if (RuntimeSourceDiagnosticPolicy == Diagnostics) return;
		if (RuntimeCode || RuntimeSourceDiagnosticPolicy ||
			BytecodeCompileStatus != BytecodeCompileStatus::Uncompiled)
			throw std::invalid_argument("Packaged source diagnostics require a trusted unstarted source script");
		RuntimeSourceDiagnosticPolicy = Diagnostics;
	}

	void LuaSourceContainer::CopyRuntimeScriptCodeTo(LuaSourceContainer &Target) const {
		if (!RuntimeCode && !RuntimeSourceDiagnosticPolicy) return;
		if (GetClassName() != Target.GetClassName())
			throw std::invalid_argument("Packaged script clone class differs from its source");
		if (RuntimeCode) {
			Target.BindRuntimeScriptCode(RuntimeCode);
		} else {
			Target.SetRuntimeSourceDiagnostics(*RuntimeSourceDiagnosticPolicy);
		}
	}

	std::string LuaSourceContainer::GetRuntimeScriptChunkName() const {
		if (!RuntimeCode) return {};
		const auto Id = GetObjectId();
		return std::format("{}{}/{}/{}", RuntimeScriptChunkPrefix, RuntimeCode->PublicScriptId, Id.Slot, Id.Generation);
	}

	std::string LuaSourceContainer::GetScriptDiagnosticName() {
		return RuntimeCode ? std::string(RuntimeScriptChunkPrefix.substr(1)) + RuntimeCode->PublicScriptId : GetFullName();
	}

	void LuaSourceContainer::CompileBytecode(lua_CompileOptions *Options) {
		if (BytecodeCompileStatus != BytecodeCompileStatus::Uncompiled) return;

		lua_CompileOptions SelectedOptions;
		if (RuntimeSourceDiagnosticPolicy) {
			// Match the C wrapper's stock defaults when no Engine options were supplied.
			SelectedOptions = Options ? *Options : lua_CompileOptions{.optimizationLevel = 1};
			SelectedOptions.debugLevel = GetRuntimeScriptDebugLevel(*RuntimeSourceDiagnosticPolicy).value();
			Options = &SelectedOptions;
		}
		std::size_t BytecodeSize = 0;
		const std::unique_ptr<char, decltype(&std::free)> Compiled(
			luau_compile(Source.data(), Source.size(), Options, &BytecodeSize), std::free
		);
		if (!Compiled || BytecodeSize == 0 || BytecodeSize > MaximumRuntimeScriptBytecodeBytes) {
			BytecodeCompileError = "Luau compilation failed or exceeded the bytecode limit";
		} else if (Compiled.get()[0] == '\0') {
			BytecodeCompileError = BoundedDiagnosticText({Compiled.get() + 1, BytecodeSize - 1}, 2048);
		} else {
			Bytecode.assign(Compiled.get(), BytecodeSize);
			BytecodeCompileStatus = BytecodeCompileStatus::Success;
			return;
		}
		BytecodeCompileStatus = BytecodeCompileStatus::Error;
	}

	std::optional<std::string> LuaSourceContainer::LoadBytecodeIntoState(lua_State *L, const char *SourceChunkName) {
		if (BytecodeCompileStatus != BytecodeCompileStatus::Success)
			return BytecodeCompileError.value_or("Script bytecode is not ready");
		const auto RuntimeChunkName = GetRuntimeScriptChunkName();
		const char *LoadName = RuntimeCode ? RuntimeChunkName.c_str() : SourceChunkName;
		const auto &Code = RuntimeCode ? RuntimeCode->Bytecode : Bytecode;
		if (Code.empty()) return "Script bytecode is not ready";
		if (luau_load(L, LoadName, Code.data(), Code.size(), 0) == LUA_OK) return std::nullopt;

		std::size_t MessageLength = 0;
		const auto *Message = lua_tolstring(L, -1, &MessageLength);
		auto Result = Message ? BoundedDiagnosticText({Message, MessageLength}, 2048) : "Luau bytecode could not be loaded";
		lua_pop(L, 1);
		return Result;
	}

	std::optional<std::string> LuaSourceContainer::LoadIntoState(lua_State *L) {
		if (BytecodeCompileStatus != BytecodeCompileStatus::Success)
			return BytecodeCompileError.value_or("Script bytecode is not ready");

		// Scripts can be named and parented after construction. Capture the live
		// Instance path so require() can restore the correct navigation context.
		ChunkName = GetFullName();
		// Give this Script its own global table before assigning `script`.
		// Otherwise later Scripts replace the shared fallback global and an
		// older yielded coroutine can resolve the wrong Script on resume.
		luaL_sandboxthread(L);
		StackValue<std::shared_ptr<Instance>>::Push(L, shared_from_this());
		lua_setglobal(L, "script");

		return LoadBytecodeIntoState(L, ChunkName.c_str());
	}
}
