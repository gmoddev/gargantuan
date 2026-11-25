#pragma once

#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/datatypes/TweenInfo.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <any>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	G_ENUM(PlaybackState, Begin, Delayed, Playing, Paused, Completed, Cancelled);

	class Instance;
	class Script;
	class TweenRuntime;

	G_SHARED_USERDATA_DECL(
		Tween,
		using Pointer = std::shared_ptr<Tween>;
		using Blend = std::any (*)(const std::any &, const std::any &, float);
		struct Goal {
			std::string Property;
			std::any End;
			Blend Interpolate;
		};

		Tween(std::weak_ptr<TweenRuntime> Runtime, ObjectId Target, TweenInfo Info, std::vector<Goal> Goals,
			std::weak_ptr<Script> Owner);

		[[nodiscard]] Enums::PlaybackState GetPlaybackState() const { return State; }
		[[nodiscard]] const TweenInfo &GetInfo() const { return Info; }
		[[nodiscard]] std::shared_ptr<BaseSignal> GetCompleted() const { return Completed; }

		void Play();
		void Pause();
		void Resume();
		void Cancel();

	  private:
		friend class TweenRuntime;
		std::weak_ptr<TweenRuntime> Runtime;
		ObjectId Target;
		TweenInfo Info;
		std::vector<Goal> Goals;
		std::weak_ptr<Script> Owner;
		bool HasOwner;
		std::vector<std::any> Initial;
		std::shared_ptr<Signal<Enums::PlaybackState>> Completed = std::make_shared<Signal<Enums::PlaybackState>>();
		ScriptSecurityContext Context;
		Enums::PlaybackState State = Enums::PlaybackState::Begin;
		std::uint64_t Sequence = 0;
		double Elapsed = 0;
		bool SampleInterrupted = false;
	);
}
