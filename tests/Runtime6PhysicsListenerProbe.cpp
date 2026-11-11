// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Diagnostic counterfactual: disconnected modes intentionally omit one physics
// observer and must never be used to claim game or simulation correctness.
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	struct PhysicsListenerProfileSnapshot {
		std::uint64_t Calls = 0;
		std::uint64_t NewDirty = 0;
		std::uint64_t Coalesced = 0;
		std::uint64_t Suppressed = 0;
		std::uint64_t Nanoseconds = 0;
	};

	struct WorldRootTestAccess {
		static PhysicsWorld DetachPhysics(WorldRoot &World) { return std::move(World.Physics); }
		static void AppendRendererPart(WorldRoot &World, const std::shared_ptr<BasePart> &Part) {
			World.Parts.push_back(Part);
		}
		static void DisconnectCFramePhysicsListener(WorldRoot &World, BasePart &Part) {
			auto Found = World.PhysicsConnections.find(Part.GetObjectId());
			if (Found == World.PhysicsConnections.end() || Found->second.empty() || !Found->second.front())
				throw std::runtime_error("[Runtime6:PhysicsListener] Missing native CFrame connection");
			Found->second.front()->Disconnect();
			Part.GetPropertyChangedSignal("CFrame")->DisconnectAll();
		}
		static void StartCFrameProfile(WorldRoot &World) {
			World.CFrameDirtyProfile = {};
			World.ProfileCFrameDirtyCallback = true;
		}
		static void StopCFrameProfile(WorldRoot &World) { World.ProfileCFrameDirtyCallback = false; }
		static PhysicsListenerProfileSnapshot Profile(const WorldRoot &World) {
			const auto &Value = World.CFrameDirtyProfile;
			return {Value.Calls, Value.NewDirty, Value.Coalesced, Value.Suppressed, Value.Nanoseconds};
		}
		static std::size_t DirtyCount(const WorldRoot &World) { return World.DirtyBodies.size(); }
		static std::size_t BodyCount(const WorldRoot &World) { return World.PartBodies.size(); }
		static std::optional<PhysicsBodyState> State(const WorldRoot &World, const BasePart &Part) {
			auto Found = World.PartBodies.find(Part.GetObjectId());
			return Found == World.PartBodies.end() ? std::nullopt : World.Physics.GetBodyState(Found->second);
		}
	};
}

namespace {
	using Clock = std::chrono::steady_clock;
	using namespace gargantuan;

	struct Options {
		std::string Mode = "native";
		std::size_t Count = 1000;
		std::size_t Changed = 1000;
		std::size_t Frames = 3;
		bool DrainPhysics = false;
		bool Profile = true;
	};

	class ScopedMeasuredProfiles {
		WorldRoot &World;
		bool Enabled;

	  public:
		explicit ScopedMeasuredProfiles(WorldRoot &Value, bool IsEnabled) : World(Value), Enabled(IsEnabled) {
			if (!Enabled) return;
			BaseSignal::SetDispatchProfilingEnabled(false);
			BaseSignal::ResetDispatchProfile();
			WorldRootTestAccess::StartCFrameProfile(World);
			BaseSignal::SetDispatchProfilingEnabled(true);
		}
		~ScopedMeasuredProfiles() {
			if (!Enabled) return;
			BaseSignal::SetDispatchProfilingEnabled(false);
			WorldRootTestAccess::StopCFrameProfile(World);
		}
		ScopedMeasuredProfiles(const ScopedMeasuredProfiles &) = delete;
		ScopedMeasuredProfiles &operator=(const ScopedMeasuredProfiles &) = delete;
	};

	std::size_t ParseBound(std::string_view Text, std::size_t Maximum) {
		if (Text.empty() || Text.find_first_not_of("0123456789") != std::string_view::npos)
			throw std::invalid_argument("[Runtime6:PhysicsListener] Expected bounded decimal option");
		const auto Value = std::stoull(std::string(Text));
		if (Value == 0 || Value > Maximum)
			throw std::invalid_argument("[Runtime6:PhysicsListener] Option exceeds bound");
		return static_cast<std::size_t>(Value);
	}

	Options ParseOptions(int ArgumentCount, char **Arguments) {
		Options Result;
		bool ChangedSpecified = false;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Option(Arguments[Index]);
			if (Option.starts_with("--mode=")) Result.Mode = std::string(Option.substr(7));
			else if (Option == "--drain-physics") Result.DrainPhysics = true;
			else if (Option == "--profile=on") Result.Profile = true;
			else if (Option == "--profile=off") Result.Profile = false;
			else if (Option.starts_with("--count=")) Result.Count = ParseBound(Option.substr(8), 50000);
			else if (Option.starts_with("--changed=")) {
				Result.Changed = ParseBound(Option.substr(10), 50000);
				ChangedSpecified = true;
			} else if (Option.starts_with("--frames=")) Result.Frames = ParseBound(Option.substr(9), 12);
			else throw std::invalid_argument("[Runtime6:PhysicsListener] Unknown option");
		}
		if (!ChangedSpecified) Result.Changed = Result.Count;
		if (Result.Changed > Result.Count ||
			(Result.Mode != "native" && Result.Mode != "native-no-listener" &&
				Result.Mode != "renderer-only" && Result.Mode != "renderer-only-no-listener") ||
			(Result.DrainPhysics && Result.Mode != "native"))
			throw std::invalid_argument("[Runtime6:PhysicsListener] Invalid mode or changed count");
		return Result;
	}

	void Run(const Options &Config) {
		const bool RendererOnly = Config.Mode.starts_with("renderer-only");
		const bool WithoutListener = Config.Mode.ends_with("no-listener");
		BaseSignal::SetDispatchProfilingEnabled(false);
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		if (!World || World->GetReplicationScopeId() != Game->GetObjectId())
			throw std::runtime_error("[Runtime6:PhysicsListener] Missing authoritative Workspace scope");
		World->SetGravity(0.0f);
		std::optional<PhysicsWorld> RetainedPhysics;
		if (RendererOnly) RetainedPhysics.emplace(WorldRootTestAccess::DetachPhysics(*World));
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Config.Count);
		for (std::size_t Index = 0; Index < Config.Count; ++Index) {
			auto PartValue = std::make_shared<Part>();
			PartValue->SetAnchored(true);
			PartValue->SetCanCollide(false);
			PartValue->SetCanTouch(false);
			PartValue->SetCFrame(CFrame(static_cast<float>(Index % 256) * 6.0f,
				static_cast<float>(Index / 256) * 6.0f, 0.0f));
			PartValue->SetParent(World);
			if (RendererOnly) WorldRootTestAccess::AppendRendererPart(*World, PartValue);
			if (WithoutListener) WorldRootTestAccess::DisconnectCFramePhysicsListener(*World, *PartValue);
			Parts.push_back(std::move(PartValue));
		}
		const auto BodyCount = WorldRootTestAccess::BodyCount(*World);
		if ((RendererOnly && BodyCount != 0) || (!RendererOnly && BodyCount != Config.Count) ||
			WorldRootTestAccess::DirtyCount(*World) != 0)
			throw std::runtime_error("[Runtime6:PhysicsListener] Fixture body or initial dirty count mismatch");
		if (Config.DrainPhysics) {
			// The first safe point may establish AssetService and author its
			// creation. Keep that setup outside the measured journal interval.
			World->StepPhysics(0.0, std::nullopt);
			if (WorldRootTestAccess::DirtyCount(*World) != 0)
				throw std::runtime_error("[Runtime6:PhysicsListener] Setup safe point left dirty marks");
		}
		std::cout << "Mode,Count,Changed,Frame,NativeBodies,ListenerEnabled,SetterNanoseconds,"
			"ListenerCalls,NewDirty,Coalesced,Suppressed,ListenerNanoseconds,DirtyCount,"
			"FireCalls,ConnectionVisits,NativeCallbacks,ScriptCallbacks,DeferredCalls,"
			"SnapshotNanoseconds,DispatchCallbackNanoseconds,CleanupNanoseconds,FireTotalNanoseconds,"
			"CallbackResidualNanoseconds,FireResidualNanoseconds,ProfileEnabled,DrainPhysics,"
			"DrainNanoseconds,JournalDelta,BackendMatches\n";
		for (std::size_t Frame = 0; Frame < Config.Frames; ++Frame) {
			const auto Scope = World->GetReplicationScopeId();
			const auto JournalBefore = Config.DrainPhysics ?
				ChangeJournal::Get().GetRetentionWindow(Scope).NextSequence : 0;
			std::int64_t SetterNanoseconds = 0;
			{
				ScopedMeasuredProfiles Profiles(*World, Config.Profile);
				const auto Started = Clock::now();
				for (std::size_t Index = 0; Index < Config.Changed; ++Index) {
					auto Transform = Parts[Index]->GetCFrame();
					Transform.Position.z = static_cast<float>(Frame + 1) * 0.01f;
					Parts[Index]->SetCFrame(Transform);
				}
				SetterNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
					Clock::now() - Started).count();
			}
			if (Config.DrainPhysics &&
				ChangeJournal::Get().GetRetentionWindow(Scope).NextSequence - JournalBefore != Config.Changed)
				throw std::runtime_error("[Runtime6:PhysicsListener] Authored CFrame setter journal delta mismatch");
			const auto After = Config.Profile ? WorldRootTestAccess::Profile(*World) : PhysicsListenerProfileSnapshot{};
			const auto Dispatch = Config.Profile ? BaseSignal::GetDispatchProfile() : SignalDispatchProfile{};
			const auto Calls = After.Calls;
			const auto NewDirty = After.NewDirty;
			const auto Coalesced = After.Coalesced;
			if (Config.Profile && Calls != (WithoutListener ? 0 : Config.Changed))
				throw std::runtime_error("[Runtime6:PhysicsListener] Listener call count mismatch");
			if (Config.Profile && (Dispatch.Calls != Config.Changed ||
				Dispatch.ConnectionVisits != (WithoutListener ? 0 : Config.Changed) ||
				Dispatch.NativeCallbacks != (WithoutListener ? 0 : Config.Changed) ||
				Dispatch.ScriptCallbacks != 0 || Dispatch.DeferredCalls != 0))
				throw std::runtime_error("[Runtime6:PhysicsListener] Signal dispatch count mismatch");
			if (Config.Profile && (Dispatch.CallbackNanoseconds < After.Nanoseconds ||
				Dispatch.TotalNanoseconds < After.Nanoseconds))
				throw std::runtime_error("[Runtime6:PhysicsListener] Callback timing nesting mismatch");
			if (Config.Profile && (NewDirty != (WithoutListener ? 0 :
				(Config.DrainPhysics || Frame == 0 ? Config.Changed : 0)) ||
				Coalesced != (WithoutListener || Config.DrainPhysics || Frame == 0 ? 0 : Config.Changed)))
				throw std::runtime_error("[Runtime6:PhysicsListener] New/coalesced callback count mismatch");
			const auto DirtyCount = WorldRootTestAccess::DirtyCount(*World);
			if (DirtyCount != (WithoutListener ? 0 : Config.Changed))
				throw std::runtime_error("[Runtime6:PhysicsListener] Dirty coalescing count mismatch");
			std::int64_t DrainNanoseconds = 0;
			std::uint64_t JournalDelta = 0;
			bool BackendMatches = true;
			if (Config.DrainPhysics) {
				const auto DrainStarted = Clock::now();
				World->StepPhysics(0.0, std::nullopt);
				DrainNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
					Clock::now() - DrainStarted).count();
				if (WorldRootTestAccess::DirtyCount(*World) != 0)
					throw std::runtime_error("[Runtime6:PhysicsListener] Native dirty marks did not drain");
				for (std::size_t Index = 0; Index < Config.Changed; ++Index) {
					const auto State = WorldRootTestAccess::State(*World, *Parts[Index]);
					if (!State || !State->Transform.FuzzyEq(Parts[Index]->GetCFrame())) {
						BackendMatches = false;
						break;
					}
				}
				if (!BackendMatches)
					throw std::runtime_error("[Runtime6:PhysicsListener] Native backend transform diverged");
				JournalDelta = ChangeJournal::Get().GetRetentionWindow(Scope).NextSequence - JournalBefore;
				if (JournalDelta != Config.Changed)
					throw std::runtime_error("[Runtime6:PhysicsListener] Authored CFrame journal delta mismatch");
			}
			std::cout << Config.Mode << ',' << Config.Count << ',' << Config.Changed << ',' << Frame << ','
				<< BodyCount << ',' << !WithoutListener << ',' << SetterNanoseconds << ','
				<< Calls << ',' << NewDirty << ','
				<< Coalesced << ',' << After.Suppressed << ','
				<< After.Nanoseconds << ',' << DirtyCount << ','
				<< Dispatch.Calls << ',' << Dispatch.ConnectionVisits << ',' << Dispatch.NativeCallbacks << ','
				<< Dispatch.ScriptCallbacks << ',' << Dispatch.DeferredCalls << ','
				<< Dispatch.SnapshotNanoseconds << ',' << Dispatch.CallbackNanoseconds << ','
				<< Dispatch.CleanupNanoseconds << ',' << Dispatch.TotalNanoseconds << ','
				<< Dispatch.CallbackNanoseconds - After.Nanoseconds << ','
				<< Dispatch.TotalNanoseconds - After.Nanoseconds << ','
				<< Config.Profile << ',' << Config.DrainPhysics << ',' << DrainNanoseconds << ','
				<< JournalDelta << ',' << BackendMatches << '\n';
		}
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		Run(ParseOptions(ArgumentCount, Arguments));
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << Failure.what() << '\n';
		return 1;
	}
}
