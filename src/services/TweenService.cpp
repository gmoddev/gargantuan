#include "gargantuan/services/TweenService.hpp"
#include "../scripting/LuauTableStorage.hpp"

#include "gargantuan/classes/Script.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/datatypes/Color3.hpp"
#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/ServicePolicy.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gargantuan {
	namespace {
		void RequireAccess(const ScriptSecurityContext &Context) {
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main ||
				!Context.HasCapability(ScriptCapability::ReadDataModel) ||
				!Context.HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("[Tween:Playback] Tweening requires Main and gameplay read/write capabilities");
		}

		template <typename T> std::any Blend(const std::any &Start, const std::any &Goal, float Alpha) {
			if (Alpha == 0.0f) return Start;
			if (Alpha == 1.0f) return Goal;
			const auto &From = std::any_cast<const T &>(Start);
			const auto &To = std::any_cast<const T &>(Goal);
			if constexpr (std::is_same_v<T, int>) {
				const auto Value = std::round(std::lerp(static_cast<double>(From), static_cast<double>(To), Alpha));
				return static_cast<int>(std::clamp(Value, static_cast<double>(std::numeric_limits<int>::min()),
					static_cast<double>(std::numeric_limits<int>::max())));
			} else if constexpr (std::is_floating_point_v<T>) {
				const auto Value = std::lerp(static_cast<double>(From), static_cast<double>(To), Alpha);
				if constexpr (std::is_same_v<T, float>) {
					if (std::abs(Value) > std::numeric_limits<float>::max())
						throw std::invalid_argument("[Tween:Property] Interpolation exceeds the native float range");
				}
				return static_cast<T>(Value);
			} else if constexpr (std::is_same_v<T, glm::vec3>) {
				return glm::mix(From, To, Alpha);
			} else {
				return From.Lerp(To, Alpha);
			}
		}

		Tween::Blend FindBlend(InstanceProperty::DataType Type, const std::any &Value) {
			using TypeId = InstanceProperty::DataType;
			switch (Type) {
			case TypeId::Integer: return Value.type() == typeid(int) ? &Blend<int> : nullptr;
			case TypeId::Number:
				if (Value.type() == typeid(float)) return &Blend<float>;
				return Value.type() == typeid(double) ? &Blend<double> : nullptr;
			case TypeId::Vector2: return Value.type() == typeid(Vector2) ? &Blend<Vector2> : nullptr;
			case TypeId::Vector3: return Value.type() == typeid(glm::vec3) ? &Blend<glm::vec3> : nullptr;
			case TypeId::Color3: return Value.type() == typeid(Color3) ? &Blend<Color3> : nullptr;
			case TypeId::UDim: return Value.type() == typeid(UDim) ? &Blend<UDim> : nullptr;
			case TypeId::UDim2: return Value.type() == typeid(UDim2) ? &Blend<UDim2> : nullptr;
			case TypeId::CFrame: return Value.type() == typeid(CFrame) ? &Blend<CFrame> : nullptr;
			default: return nullptr;
			}
		}

		void ValidateValue(const InstanceProperty &Property, const std::any &Value) {
			const auto Wire = EncodeNativeWireValue(Value);
			if (!Wire || !Property.IsValueValid(Value))
				throw std::invalid_argument("[Tween:Property] A value fails the property's validation");
			ValidateProtocolWireValue(*Wire);
		}

		void RequireTargetAuthority(const Instance &Target, const ScriptSecurityContext &Context) {
			if (Context.Domain != ScriptExecutionDomain::Client) return;
			if (GetInstanceServicePolicy(Target) == SchemaServicePolicy::Replicated || IsStarterGuiTemplate(Target))
				throw std::invalid_argument("[Tween:Property] The target is server-authoritative");
			if (const auto Gui = FindPlayerGuiScope(Target); Gui && !IsLocalPlayerGui(*Gui))
				throw std::invalid_argument("[Tween:Property] The target belongs to another PlayerGui");
		}

		const InstanceProperty &RequireProperty(Instance &Target, const std::string &Name,
			const ScriptSecurityContext &Context) {
			auto *Property = Target.FindProperty(Name);
			if (!Property || !Property->Read || !Property->Write || !Property->FromStack ||
				Property->ReadPermission != Enums::Permission::None || Property->WritePermission != Enums::Permission::None ||
				!Property->CanRead(Context) || !Property->CanWrite(Context))
				throw std::invalid_argument("[Tween:Property] Goal must name an accessible read/write property");
			return *Property;
		}

		struct Sample {
			float Alpha;
			bool Complete;
		};

		Sample SampleTween(const TweenInfo &Info, double Elapsed) {
			const double Time = Info.Time;
			const double SinceDelay = Elapsed - Info.DelayTime;
			const double LegsPerCycle = Info.Reverses ? 2.0 : 1.0;
			const double CycleTime = Time * LegsPerCycle;
			if (Time == 0.0 || SinceDelay >= CycleTime * (static_cast<double>(Info.RepeatCount) + 1))
				return {Info.Reverses ? 0.0f : 1.0f, true};
			const double CyclePosition = std::fmod(SinceDelay, CycleTime);
			const bool Reverse = Info.Reverses && CyclePosition >= Time;
			const float Progress = static_cast<float>(Reverse ? (CyclePosition - Time) / Time : CyclePosition / Time);
			const float Alpha = EasingCurves::CalculateAlpha(Progress, Info.EasingStyle, Info.EasingDirection);
			return {Reverse ? 1.0f - Alpha : Alpha, false};
		}
	}

	int TweenService::Create(lua_State *L, Instance *Service) {
		auto *Engine = ScriptEngine::Get(L);
		if (Engine->IsStopping() || Service->GetDataModel() != Engine->DataModel)
			throw std::invalid_argument("[Tween:Service] TweenService requires the current live runtime");
		auto Target = CheckStackValue<std::shared_ptr<Instance>>(L, 2);
		const auto Info = CheckStackValue<TweenInfo>(L, 3);
		luaL_checktype(L, 4, LUA_TTABLE);
		return StackValue<Tween::Pointer>::Push(L, Engine->GetTweenRuntime().Create(L, Target, Info, 4));
	}

	std::shared_ptr<Instance> TweenRuntime::ResolveTarget(ObjectId Object) const {
		auto Target = ObjectRegistry::Get().Lookup(Object);
		if (!Target || Target->GetDestroyed() || Target->IsDestroying() ||
			Target->GetDataModel() != Engine.DataModel) return {};
		return Target;
	}

	bool TweenRuntime::IsOwnerLive(const Tween &Value) const {
		if (const auto *App = Engine.GetApplicationPresentation()) {
			const auto &Owner = Value.Sequence ? Value.Context : GetCurrentScriptSecurityContext();
			if (!App->IsOwnerCurrent(Owner.IssuingScript, Owner.ExecutionGeneration)) return false;
		}
		if (!Value.HasOwner) return true;
		const auto Owner = Value.Owner.lock();
		return Owner && !Owner->GetDestroyed() && !Owner->IsDestroying();
	}

	Tween::Pointer TweenRuntime::Create(lua_State *L, const std::shared_ptr<Instance> &Target,
		const TweenInfo &Info, int GoalsIndex) {
		const auto Context = GetCurrentScriptSecurityContext();
		RequireAccess(Context);
		Info.Validate();
		if (Stopped || !Target || Target->GetDestroyed() || Target->IsDestroying() ||
			Target->GetDataModel() != Engine.DataModel)
			throw std::invalid_argument("[Tween:Service] Expected a live target owned by this DataModel");
		RequireTargetAuthority(*Target, Context);
		if (lua_getmetatable(L, GoalsIndex)) {
			lua_pop(L, 1);
			throw std::invalid_argument("[Tween:Service] Goals must be a plain property table");
		}
		std::vector<Tween::Goal> Goals;
		constexpr std::size_t MaximumGoalTableSlots = 256;
		if (LuauTableStorageSlots(lua_topointer(L, GoalsIndex)) > MaximumGoalTableSlots)
			throw std::invalid_argument("[Tween:Service] Goal table exceeds 256 backing slots");
		lua_pushnil(L);
		while (lua_next(L, GoalsIndex)) {
			if (Goals.size() == MaximumGoals || lua_type(L, -2) != LUA_TSTRING)
				throw std::invalid_argument("[Tween:Service] Expected one through 16 named goal properties");
			size_t Length;
			const char *Bytes = lua_tolstring(L, -2, &Length);
			const std::string_view Name(Bytes, Length);
			ValidateProtocolString(Name, MaximumProtocolIdentifierBytes, "Tween property name");
			const auto &Property = RequireProperty(*Target, std::string(Name), Context);
			// Integer reflection consumes native int, so check the full Luau double
			// before the generic property conversion can truncate or narrow it.
			using Type = InstanceProperty::DataType;
			if (Property.SemanticType == Type::Integer || Property.SemanticType == Type::Number) {
				if (lua_type(L, -1) != LUA_TNUMBER)
					throw std::invalid_argument("[Tween:Property] Expected a numeric goal");
				const double Number = lua_tonumber(L, -1);
				if (!std::isfinite(Number))
					throw std::invalid_argument("[Tween:Property] Expected a finite numeric goal");
				if (Property.SemanticType == Type::Integer && (std::trunc(Number) != Number ||
					Number < std::numeric_limits<int>::min() || Number > std::numeric_limits<int>::max()))
					throw std::invalid_argument("[Tween:Property] Expected an in-range integer goal");
				if (Property.WireType == "Float" && std::abs(Number) > std::numeric_limits<float>::max())
					throw std::invalid_argument("[Tween:Property] Goal exceeds the native float range");
			}
			if (!Property.IsStack || !Property.IsStack(L, -1))
				throw std::invalid_argument("[Tween:Property] Goal type differs from the reflected property type");
			auto End = Property.FromStack(L, -1);
			const auto Interpolate = FindBlend(Property.SemanticType, End);
			if (!Interpolate) throw std::invalid_argument("[Tween:Property] Property type cannot be interpolated");
			ValidateValue(Property, End);
			Goals.push_back({std::string(Name), std::move(End), Interpolate});
			lua_pop(L, 1);
		}
		if (Goals.empty()) throw std::invalid_argument("[Tween:Service] Expected at least one goal property");
		std::ranges::sort(Goals, {}, &Tween::Goal::Property);
		return std::make_shared<Tween>(weak_from_this(), Target->GetObjectId(), Info, std::move(Goals),
			Engine.FindManagedScriptThread(L));
	}

	void TweenRuntime::Play(Tween &Value) {
		const auto Context = GetCurrentScriptSecurityContext();
		RequireAccess(Context);
		auto Target = ResolveTarget(Value.Target);
		if (Stopped || !Target || !IsOwnerLive(Value))
			throw std::runtime_error("[Tween:Playback] Target, Script or runtime has retired");
		RequireTargetAuthority(*Target, Context);
		if (Value.Sequence) {
			Value.Resume();
			return;
		}
		if (Active.size() + PendingCount >= MaximumPlaybacks || LastSequence == std::numeric_limits<std::uint64_t>::max())
			throw std::runtime_error("[Tween:Playback] Playback capacity exceeded");

		const auto Sequence = LastSequence + 1;
		std::array<std::uint64_t, MaximumGoals> Conflicts{};
		std::size_t Count = 0;
		std::vector<std::any> Initial;
		Initial.reserve(Value.Goals.size());
		std::map<PropertyKey, std::uint64_t> NewOwners;
		for (const auto &Goal : Value.Goals) {
			const auto &Property = RequireProperty(*Target, Goal.Property, Context);
			auto Start = Property.Read(Target.get());
			if (Start.type() != Goal.End.type())
				throw std::invalid_argument("[Tween:Property] Property type changed after creation");
			ValidateValue(Property, Start);
			Initial.push_back(std::move(Start));
			const PropertyKey Key(Value.Target, Goal.Property);
			if (const auto Found = PropertyOwners.find(Key); Found != PropertyOwners.end()) {
				if (Context.Domain == ScriptExecutionDomain::Client &&
					Active.at(Found->second)->Context.Domain != ScriptExecutionDomain::Client)
					throw std::runtime_error("[Tween:Playback] Client cannot supersede authoritative playback");
				if (std::find(Conflicts.begin(), Conflicts.begin() + Count, Found->second) == Conflicts.begin() + Count)
					Conflicts[Count++] = Found->second;
			}
			NewOwners.emplace(Key, Sequence);
		}
		std::sort(Conflicts.begin(), Conflicts.begin() + Count);
		Active.emplace(Sequence, Value.shared_from_this());
		for (std::size_t Index = 0; Index < Count; ++Index) Finish(*Active.at(Conflicts[Index]), PlaybackState::Cancelled);
		PropertyOwners.merge(NewOwners);
		Value.Initial = std::move(Initial);
		Value.Context = Context;
		Value.Sequence = Sequence;
		Value.Elapsed = 0.0;
		Value.State = Value.Info.DelayTime > 0.0f ? PlaybackState::Delayed : PlaybackState::Playing;
		LastSequence = Sequence;
	}

	void TweenRuntime::Reauthorize(Tween &Value, const ScriptSecurityContext &Context) {
		RequireAccess(Context);
		auto Target = ResolveTarget(Value.Target);
		if (Stopped || !Target || !IsOwnerLive(Value))
			throw std::runtime_error("[Tween:Playback] Target, Script or runtime has retired");
		RequireTargetAuthority(*Target, Context);
		for (const auto &Goal : Value.Goals) (void)RequireProperty(*Target, Goal.Property, Context);
		Value.Context = Context;
	}

	void TweenRuntime::Finish(Tween &Value, PlaybackState State, bool Notify) {
		if (!Value.Sequence) return;
		const auto Sequence = std::exchange(Value.Sequence, 0);
		Value.State = State;
		Value.Initial.clear();
		for (const auto &Goal : Value.Goals) PropertyOwners.erase(PropertyKey(Value.Target, Goal.Property));
		if (Notify) {
			Pending[(PendingHead + PendingCount) % MaximumPlaybacks] = {Active.at(Sequence), State};
			++PendingCount;
		}
		Active.erase(Sequence);
	}

	void TweenRuntime::Update(Tween &Value, float DeltaTime) {
		if (!IsOwnerLive(Value)) {
			Finish(Value, PlaybackState::Cancelled, false);
			return;
		}
		auto Target = ResolveTarget(Value.Target);
		if (!Target) {
			Finish(Value, PlaybackState::Cancelled);
			return;
		}
		if (Value.State == PlaybackState::Paused) return;
		Value.Elapsed += DeltaTime;
		if (Value.Elapsed < Value.Info.DelayTime) return;
		Value.State = PlaybackState::Playing;
		Value.SampleInterrupted = false;
		const auto [Alpha, Complete] = SampleTween(Value.Info, Value.Elapsed);
		const auto Sequence = Value.Sequence;
		const ScriptSecurityScope Scope(Value.Context);
		try {
			RequireTargetAuthority(*Target, Value.Context);
			std::array<std::any, MaximumGoals> Samples;
			for (std::size_t Index = 0; Index < Value.Goals.size(); ++Index) {
				const auto &Goal = Value.Goals[Index];
				const auto &Property = RequireProperty(*Target, Goal.Property, Value.Context);
				Samples[Index] = Goal.Interpolate(Value.Initial[Index], Goal.End, Alpha);
				ValidateValue(Property, Samples[Index]);
			}
			for (std::size_t Index = 0; Index < Value.Goals.size(); ++Index) {
				const auto &Goal = Value.Goals[Index];
				if (Target->ApplyPropertyMutation(Goal.Property, Samples[Index]) != MutationStatus::Success)
					throw std::runtime_error("[Tween:Playback] Property mutation rejected");
				// Property notifications can cancel, pause or replace this playback,
				// retire its owner or destroy the runtime before the next write.
				if (Stopped || Value.Sequence != Sequence || Value.SampleInterrupted) return;
				if (!IsOwnerLive(Value)) {
					Finish(Value, PlaybackState::Cancelled, false);
					return;
				}
				if (Target->GetDestroyed() || Target->IsDestroying()) {
					Finish(Value, PlaybackState::Cancelled);
					return;
				}
				if (Index + 1 < Value.Goals.size()) RequireTargetAuthority(*Target, Value.Context);
			}
		} catch (const std::exception &) {
			if (Value.Sequence == Sequence) {
				// The per-step trace counts failures without logging unbounded
				// script-supplied names, native exception text or property values.
				runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::TweenStep, false);
				Finish(Value, PlaybackState::Cancelled);
			}
			return;
		}
		if (Complete) Finish(Value, PlaybackState::Completed);
	}

	void TweenRuntime::DispatchCompletions() {
		const std::size_t Count = PendingCount;
		for (std::size_t Index = 0; Index < Count && !Stopped; ++Index) {
			auto CompletionValue = std::exchange(Pending[PendingHead], {});
			PendingHead = (PendingHead + 1) % MaximumPlaybacks;
			--PendingCount;
			if (IsOwnerLive(*CompletionValue.Value)) {
				try {
					CompletionValue.Value->Completed->Fire(CompletionValue.State);
				} catch (const std::exception &) {
					runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::TweenStep, false);
				}
			}
		}
	}

	void TweenRuntime::Step(float DeltaTime) {
		if (Stopped || Stepping || !std::isfinite(DeltaTime) || DeltaTime < 0.0f ||
			(Active.empty() && PendingCount == 0)) return;
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::TweenStep);
		struct ReentryGuard {
			bool &Stepping;
			~ReentryGuard() { Stepping = false; }
		} Guard{Stepping};
		Stepping = true;
		const auto Fence = LastSequence;
		std::uint64_t Prior = 0;
		while (!Stopped) {
			const auto Next = Active.upper_bound(Prior);
			if (Next == Active.end() || Next->first > Fence) break;
			Prior = Next->first;
			auto Value = Next->second;
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::TweenStep);
			Update(*Value, DeltaTime);
		}
		DispatchCompletions();
	}

	void TweenRuntime::Shutdown() {
		Stopped = true;
		PropertyOwners.clear();
		for (const auto &[Sequence, Value] : Active) {
			Value->Sequence = 0;
			Value->State = PlaybackState::Cancelled;
			Value->Initial.clear();
		}
		Active.clear();
		for (auto &CompletionValue : Pending) CompletionValue = {};
		PendingHead = 0;
		PendingCount = 0;
	}
}
