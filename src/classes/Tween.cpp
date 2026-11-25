#include "gargantuan/classes/Tween.hpp"

#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/services/TweenService.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		void RequireTweenControl() {
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main ||
				!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("[Tween:Playback] Current context cannot control tweens");
		}
	}

	G_USERDATA_IMPL(
		Tween,
		.Tag = UserdataTag::Tween,
		.Type = "Tween",
		.Properties = {
			{"PlaybackState", Property::fromRead([](Tween *Value) { return Value->GetPlaybackState(); })},
			{"TweenInfo", Property::fromRead([](Tween *Value) { return TweenInfo(Value->GetInfo()); })},
			{"Completed", Property::fromRead([](Tween *Value) { return Value->GetCompleted(); })},
		},
		.Methods = {
			{"Play", Method::fromMember<&Tween::Play>()},
			{"Pause", Method::fromMember<&Tween::Pause>()},
			{"Resume", Method::fromMember<&Tween::Resume>()},
			{"Cancel", Method::fromMember<&Tween::Cancel>()},
		}
	);

	Tween::Tween(std::weak_ptr<TweenRuntime> RuntimeValue, ObjectId TargetValue, TweenInfo InfoValue,
		std::vector<Goal> GoalsValue, std::weak_ptr<Script> OwnerValue)
		: Runtime(std::move(RuntimeValue)), Target(TargetValue), Info(std::move(InfoValue)),
		  Goals(std::move(GoalsValue)), Owner(std::move(OwnerValue)), HasOwner(!Owner.expired()) {}

	void Tween::Play() {
		RequireTweenControl();
		auto Scheduler = Runtime.lock();
		if (!Scheduler) throw std::runtime_error("[Tween:Playback] Runtime has stopped");
		Scheduler->Play(*this);
	}

	void Tween::Pause() {
		RequireTweenControl();
		if (Sequence && Context.Domain != GetCurrentScriptSecurityContext().Domain)
			throw std::runtime_error("[Tween:Playback] Current domain does not own playback");
		if (State == Enums::PlaybackState::Playing || State == Enums::PlaybackState::Delayed) {
			State = Enums::PlaybackState::Paused;
			SampleInterrupted = true;
		}
	}

	void Tween::Resume() {
		RequireTweenControl();
		if (State == Enums::PlaybackState::Paused) {
			const auto ResumeContext = GetCurrentScriptSecurityContext();
			auto Scheduler = Runtime.lock();
			if (!Scheduler || ResumeContext.Domain != Context.Domain)
				throw std::runtime_error("[Tween:Playback] Current domain does not own playback");
			Scheduler->Reauthorize(*this, ResumeContext);
			State = Elapsed < Info.DelayTime ? Enums::PlaybackState::Delayed : Enums::PlaybackState::Playing;
		}
	}

	void Tween::Cancel() {
		RequireTweenControl();
		if (Sequence && Context.Domain != GetCurrentScriptSecurityContext().Domain)
			throw std::runtime_error("[Tween:Playback] Current domain does not own playback");
		if (auto Scheduler = Runtime.lock()) Scheduler->Finish(*this, Enums::PlaybackState::Cancelled);
	}
}
