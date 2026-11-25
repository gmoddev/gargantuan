#pragma once

#include "gargantuan/classes/Tween.hpp"
#include "gargantuan/services/generated/TweenService.hpp"

#include <array>
#include <map>
#include <string_view>

namespace gargantuan {
	class DataModel;
	class ScriptEngine;

	class TweenService : public Instance {
		I_TweenService;
	};

	// There is one lazy scheduler per ScriptEngine. The service itself owns no
	// playback state, and a Tween value cannot retain a DataModel or Script.
	class TweenRuntime : public std::enable_shared_from_this<TweenRuntime> {
	  public:
		static constexpr std::size_t MaximumGoals = 16;
		static constexpr std::size_t MaximumPlaybacks = 1024;

		explicit TweenRuntime(ScriptEngine &Engine) : Engine(Engine) {}
		Tween::Pointer Create(lua_State *L, const std::shared_ptr<Instance> &Target, const TweenInfo &Info, int GoalsIndex);
		void Step(float DeltaTime);
		void Shutdown();

	  private:
		friend class Tween;
		using PlaybackState = Enums::PlaybackState;
		using PropertyKey = std::pair<ObjectId, std::string_view>;

		struct Completion {
			Tween::Pointer Value;
			PlaybackState State;
		};

		ScriptEngine &Engine;
		std::map<std::uint64_t, Tween::Pointer> Active;
		std::map<PropertyKey, std::uint64_t> PropertyOwners;
		std::array<Completion, MaximumPlaybacks> Pending{};
		std::size_t PendingHead = 0;
		std::size_t PendingCount = 0;
		std::uint64_t LastSequence = 0;
		bool Stepping = false;
		bool Stopped = false;

		[[nodiscard]] std::shared_ptr<Instance> ResolveTarget(ObjectId Target) const;
		[[nodiscard]] bool IsOwnerLive(const Tween &Value) const;
		void Play(Tween &Value);
		void Reauthorize(Tween &Value, const ScriptSecurityContext &Context);
		void Finish(Tween &Value, PlaybackState State, bool Notify = true);
		void Update(Tween &Value, float DeltaTime);
		void DispatchCompletions();
	};
}
