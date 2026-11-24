#include "gargantuan/services/AudioMixService.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	namespace {
		void ValidateGain(double Gain) {
			if (!std::isfinite(Gain) || Gain < 0.0 || Gain > 1.0)
				throw std::invalid_argument("[Audio:Mix] Gain must be a finite number from zero through one");
		}

		double ReadGain(lua_State *State, int Index) {
			if (lua_type(State, Index) != LUA_TNUMBER)
				throw std::invalid_argument("[Audio:Mix] Expected a numeric gain");
			return lua_tonumber(State, Index);
		}

		bool ReadMuted(lua_State *State, int Index) {
			if (lua_type(State, Index) != LUA_TBOOLEAN)
				throw std::invalid_argument("[Audio:Mix] Expected a boolean mute state");
			return lua_toboolean(State, Index) != 0;
		}

		Enums::SoundMixCategory ReadCategory(lua_State *State) {
			if (!StackValue<EnumItem>::Is(State, 2))
				throw std::invalid_argument("[Audio:Mix] Expected a SoundMixCategory item");
			const auto Item = StackValue<EnumItem>::From(State, 2);
			if (!Item.EnumType || Item.EnumType->Name != "SoundMixCategory")
				throw std::invalid_argument("[Audio:Mix] Expected a SoundMixCategory item");
			return static_cast<Enums::SoundMixCategory>(Item.Value);
		}
	}

	std::size_t AudioMixService::CategoryIndex(Enums::SoundMixCategory Category) {
		const auto Index = static_cast<std::size_t>(Category);
		if (Index >= CategoryCount) throw std::invalid_argument("[Audio:Mix] Invalid SoundMixCategory item");
		return Index;
	}

	bool AudioMixService::HasAttachedRuntime() const {
		const auto Parent = GetParent();
		return RuntimeAttached && !GetDestroyed() && !IsDestroying() && Parent && *Parent == GetDataModel();
	}

	void AudioMixService::RequireAccess(ScriptCapability Capability) const {
		AssertAuthoritativeMutation("[Audio:Mix] Access local mix");
		const auto &Context = GetCurrentScriptSecurityContext();
		if ((Context.Domain != ScriptExecutionDomain::Client && Context.Domain != ScriptExecutionDomain::Core) ||
			!Context.HasCapability(Capability))
			throw std::runtime_error("[Audio:Mix] AudioMixService requires local presentation access");
		if (!HasAttachedRuntime()) throw std::runtime_error("[Audio:Mix] No local mix is attached to this service");
	}

	void AudioMixService::AttachRuntime() {
		if (RuntimeAttached) throw std::logic_error("[Audio:Mix] A local mix is already attached");
		RuntimeAttached = true;
	}

	void AudioMixService::DetachRuntime() noexcept {
		RuntimeAttached = false;
		Master = {};
		Categories = {};
	}

	AudioMixService::EffectiveGains AudioMixService::CaptureEffectiveGains() const {
		EffectiveGains Result;
		if (!HasAttachedRuntime()) {
			Result.fill(1.0f);
			return Result;
		}
		const auto MasterGain = Master.Muted ? 0.0 : Master.Gain;
		for (std::size_t Index = 0; Index < CategoryCount; ++Index) {
			const auto &Control = Categories[Index];
			Result[Index] = Control.Muted ? 0.0f : static_cast<float>(MasterGain * Control.Gain);
		}
		return Result;
	}

	double AudioMixService::GetMasterGain() const {
		RequireAccess(ScriptCapability::ReadDataModel);
		return Master.Gain;
	}

	void AudioMixService::SetMasterGain(double Gain) {
		RequireAccess(ScriptCapability::MutateDataModel);
		ValidateGain(Gain);
		Master.Gain = Gain;
	}

	bool AudioMixService::GetMasterMuted() const {
		RequireAccess(ScriptCapability::ReadDataModel);
		return Master.Muted;
	}

	void AudioMixService::SetMasterMuted(bool Muted) {
		RequireAccess(ScriptCapability::MutateDataModel);
		Master.Muted = Muted;
	}

	double AudioMixService::GetCategoryGain(Enums::SoundMixCategory Category) const {
		RequireAccess(ScriptCapability::ReadDataModel);
		return Categories[CategoryIndex(Category)].Gain;
	}

	void AudioMixService::SetCategoryGain(Enums::SoundMixCategory Category, double Gain) {
		RequireAccess(ScriptCapability::MutateDataModel);
		ValidateGain(Gain);
		Categories[CategoryIndex(Category)].Gain = Gain;
	}

	bool AudioMixService::GetCategoryMuted(Enums::SoundMixCategory Category) const {
		RequireAccess(ScriptCapability::ReadDataModel);
		return Categories[CategoryIndex(Category)].Muted;
	}

	void AudioMixService::SetCategoryMuted(Enums::SoundMixCategory Category, bool Muted) {
		RequireAccess(ScriptCapability::MutateDataModel);
		Categories[CategoryIndex(Category)].Muted = Muted;
	}

	int AudioMixService::GetMasterGain(lua_State *State, Instance *Receiver) {
		return StackValue<double>::Push(State, static_cast<AudioMixService *>(Receiver)->GetMasterGain());
	}

	int AudioMixService::SetMasterGain(lua_State *State, Instance *Receiver) {
		static_cast<AudioMixService *>(Receiver)->SetMasterGain(ReadGain(State, 2));
		return 0;
	}

	int AudioMixService::GetMasterMuted(lua_State *State, Instance *Receiver) {
		return StackValue<bool>::Push(State, static_cast<AudioMixService *>(Receiver)->GetMasterMuted());
	}

	int AudioMixService::SetMasterMuted(lua_State *State, Instance *Receiver) {
		static_cast<AudioMixService *>(Receiver)->SetMasterMuted(ReadMuted(State, 2));
		return 0;
	}

	int AudioMixService::GetCategoryGain(lua_State *State, Instance *Receiver) {
		return StackValue<double>::Push(State, static_cast<AudioMixService *>(Receiver)->GetCategoryGain(ReadCategory(State)));
	}

	int AudioMixService::SetCategoryGain(lua_State *State, Instance *Receiver) {
		static_cast<AudioMixService *>(Receiver)->SetCategoryGain(ReadCategory(State), ReadGain(State, 3));
		return 0;
	}

	int AudioMixService::GetCategoryMuted(lua_State *State, Instance *Receiver) {
		return StackValue<bool>::Push(State, static_cast<AudioMixService *>(Receiver)->GetCategoryMuted(ReadCategory(State)));
	}

	int AudioMixService::SetCategoryMuted(lua_State *State, Instance *Receiver) {
		static_cast<AudioMixService *>(Receiver)->SetCategoryMuted(ReadCategory(State), ReadMuted(State, 3));
		return 0;
	}
}
