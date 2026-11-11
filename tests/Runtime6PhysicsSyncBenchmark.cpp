// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Psapi.lib")
#endif
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t BodyCount(const WorldRoot &World) { return World.PartBodies.size(); }
		static std::size_t PartCount(const WorldRoot &World) { return World.Parts.size(); }
		static std::size_t DirtyBodyCount(const WorldRoot &World) { return World.DirtyBodies.size(); }
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
		std::size_t StaticCount = 900;
		std::size_t DynamicCount = 100;
		std::size_t Frames = 12;
		std::size_t Warmup = 3;
		std::size_t LifecyclePerFrame = 0;
		double ControlDensity = 0.01;
		bool DisableContacts = true;
		bool Publication = false;
	};

	std::size_t ParseCount(std::string_view Text, std::string_view Name) {
		std::size_t Value = 0;
		const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
		if (Error != std::errc{} || End != Text.data() + Text.size())
			throw std::invalid_argument(std::string("[Runtime6:PhysicsSync] Invalid ") + std::string(Name));
		return Value;
	}

	double ParseDensity(const std::string &Text) {
		std::size_t Consumed = 0;
		const double Value = std::stod(Text, &Consumed);
		if (Consumed != Text.size() || !std::isfinite(Value) || Value < 0.0 || Value > 1.0)
			throw std::invalid_argument("[Runtime6:PhysicsSync] Density must be a finite fraction in [0, 1]");
		return Value;
	}

	Options ParseOptions(int ArgumentCount, char **Arguments) {
		Options Result;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Flag = Arguments[Index];
			if (Flag == "--publication") Result.Publication = true;
			else if (Flag == "--disable-contacts") Result.DisableContacts = true;
			else if (Flag == "--enable-contacts") Result.DisableContacts = false;
			else {
				if (++Index >= ArgumentCount) throw std::invalid_argument("[Runtime6:PhysicsSync] Missing option value");
				const std::string Value = Arguments[Index];
				if (Flag == "--static-count") Result.StaticCount = ParseCount(Value, Flag);
				else if (Flag == "--dynamic-count") Result.DynamicCount = ParseCount(Value, Flag);
				else if (Flag == "--frames") Result.Frames = ParseCount(Value, Flag);
				else if (Flag == "--warmup") Result.Warmup = ParseCount(Value, Flag);
				else if (Flag == "--lifecycle-per-frame") Result.LifecyclePerFrame = ParseCount(Value, Flag);
				else if (Flag == "--control-density") Result.ControlDensity = ParseDensity(Value);
				else throw std::invalid_argument("[Runtime6:PhysicsSync] Unknown option: " + std::string(Flag));
			}
		}
		if (Result.StaticCount + Result.DynamicCount == 0 ||
			Result.StaticCount > 50'000 || Result.DynamicCount > 50'000 ||
			Result.StaticCount + Result.DynamicCount > 50'000 ||
			Result.Frames == 0 || Result.Frames > 120 || Result.Warmup > 16 ||
			Result.LifecyclePerFrame > 16 ||
			Result.LifecyclePerFrame > Result.StaticCount + Result.DynamicCount)
			throw std::invalid_argument("[Runtime6:PhysicsSync] Workload exceeds bounded fixture limits");
		return Result;
	}

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	std::uint64_t ResidentBytes() {
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS Counters{};
		if (GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters)))
			return static_cast<std::uint64_t>(Counters.WorkingSetSize);
#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t VirtualPages = 0, ResidentPages = 0;
		if (Input >> VirtualPages >> ResidentPages) {
			const long PageSize = sysconf(_SC_PAGESIZE);
			if (PageSize > 0) return ResidentPages * static_cast<std::uint64_t>(PageSize);
		}
#endif
		return 0;
	}
	std::string MemoryMetric(std::uint64_t Bytes) {
		return Bytes == 0 ? "NA" : std::to_string(Bytes);
	}

	struct Fixture {
		Options Configuration;
		std::size_t CFrameEvents = 0;
		std::shared_ptr<DataModel> Game;
		std::shared_ptr<Workspace> World;
		std::vector<std::shared_ptr<Part>> Parts;
		std::vector<SignalConnection::Pointer> Observers;
		std::unique_ptr<RenderPublisher> Publisher;
		std::unique_ptr<RenderProjection> Projection;
		RenderCameraInput Camera;
		std::uint64_t NextCreation = 0;
		double ConstructionMs = 0.0;
		double AdoptionAndBodyMs = 0.0;
		double ObservationMs = 0.0;
		double RenderInitMs = 0.0;

		explicit Fixture(Options Value) : Configuration(Value) {
			const auto Start = Clock::now();
			Game = std::make_shared<DataModel>();
			World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
			if (!World) throw std::runtime_error("[Runtime6:PhysicsSync] Workspace unavailable");
			World->SetGravity(0.0f);
			Parts.resize(Configuration.StaticCount + Configuration.DynamicCount);
			Observers.resize(Parts.size());
			ConstructionMs = Milliseconds(Clock::now() - Start);
			for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
				const auto CreateStart = Clock::now();
				Parts[Index] = MakePart(Index);
				ConstructionMs += Milliseconds(Clock::now() - CreateStart);
				const auto AdoptStart = Clock::now();
				Parts[Index]->SetParent(World);
				AdoptionAndBodyMs += Milliseconds(Clock::now() - AdoptStart);
				const auto ObserveStart = Clock::now();
				Observe(Index);
				ObservationMs += Milliseconds(Clock::now() - ObserveStart);
			}
			if (WorldRootTestAccess::BodyCount(*World) != Parts.size() ||
				WorldRootTestAccess::PartCount(*World) != Parts.size())
				throw std::runtime_error("[Runtime6:PhysicsSync] Initial body/Part count mismatch");
			if (Configuration.Publication) {
				const auto RenderStart = Clock::now();
				Publisher = std::make_unique<RenderPublisher>();
				Projection = std::make_unique<RenderProjection>();
				Camera = MakeLookAtRenderCameraInput(
					glm::vec3(0.0f, 0.0f, 1600.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				auto Initial = Publisher->Publish(*World, Camera, 1280, 720);
				(void)Projection->Apply(*Initial);
				if (Initial->Creates.size() != Parts.size() || Projection->GetObjects().size() != Parts.size())
					throw std::runtime_error("[Runtime6:PhysicsSync] Initial render count mismatch");
				RenderInitMs = Milliseconds(Clock::now() - RenderStart);
			}
		}

		std::shared_ptr<Part> MakePart(std::size_t Index) {
			auto Value = std::make_shared<Part>();
			const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Parts.size()))));
			const auto X = static_cast<float>(Index % Side) * 6.0f;
			const auto Y = static_cast<float>(Index / Side) * 6.0f;
			Value->SetCFrame(CFrame(glm::vec3(X, Y, 0.0f)));
			Value->SetAnchored(Index < Configuration.StaticCount);
			if (Configuration.DisableContacts) {
				Value->SetCanCollide(false);
				Value->SetCanTouch(false);
			}
			++NextCreation;
			return Value;
		}

		void Observe(std::size_t Index) {
			Observers[Index] = Parts[Index]->GetPropertyChangedSignal("CFrame")->Connect(
				[this](std::monostate) { ++CFrameEvents; });
		}
	};

	void Run(const Options &Configuration) {
		Fixture Test(Configuration);
		const auto Total = Test.Parts.size();
		const auto ControlCount = Configuration.DynamicCount == 0 ? 0 :
			std::min(Configuration.DynamicCount, static_cast<std::size_t>(
				std::ceil(Configuration.ControlDensity * static_cast<double>(Configuration.DynamicCount))));
		std::uint64_t PeakResident = ResidentBytes();
		std::cout << "kind,phase,frame,static_count,dynamic_count,control_density,control_parts,control_writes,"
			"lifecycle_count,total_created,total_destroyed,contacts_disabled,publication_enabled,"
			"construction_ms,adoption_body_ms,observation_ms,render_init_ms,"
			"create_ms,control_ms,step_ms,publish_ms,"
			"projection_ms,validation_ms,frame_ms,body_count,part_count,dirty_body_count,render_object_count,"
			"render_created,render_updated,render_removed,cframe_events,resident_bytes,peak_resident_bytes,"
			"position_checksum,velocity_checksum\n";
		std::cout << "setup,setup,0," << Configuration.StaticCount << ',' << Configuration.DynamicCount << ','
			<< Configuration.ControlDensity << ",0,0,0," << Test.NextCreation << ",0,"
			<< Configuration.DisableContacts << ','
			<< Configuration.Publication << ',' << std::fixed << std::setprecision(6)
			<< Test.ConstructionMs << ',' << Test.AdoptionAndBodyMs << ','
			<< Test.ObservationMs << ',' << Test.RenderInitMs
			<< ",0,0,0,0,0,0,0," << WorldRootTestAccess::BodyCount(*Test.World) << ','
			<< WorldRootTestAccess::PartCount(*Test.World) << ','
			<< WorldRootTestAccess::DirtyBodyCount(*Test.World) << ','
			<< (Test.Projection ? Test.Projection->GetObjects().size() : 0)
			<< ",0,0,0,0," << MemoryMetric(PeakResident) << ',' << MemoryMetric(PeakResident) << ",0,0\n" << std::flush;
		for (std::size_t Iteration = 0; Iteration < Configuration.Warmup + Configuration.Frames; ++Iteration) {
			const bool Warmup = Iteration < Configuration.Warmup;
			const auto Frame = Warmup ? Iteration : Iteration - Configuration.Warmup;
			const auto FrameStart = Clock::now();
			std::size_t Creates = 0, Destroys = 0;
			const auto CreateStart = Clock::now();
			for (std::size_t Item = 0; Item < Configuration.LifecyclePerFrame; ++Item) {
				const auto Index = (Iteration * Configuration.LifecyclePerFrame + Item) % Total;
				const auto OldId = Test.Parts[Index]->GetObjectId();
				Test.Observers[Index]->Disconnect();
				Test.Parts[Index]->Destroy();
				++Destroys;
				Test.Parts[Index] = Test.MakePart(Index);
				Test.Parts[Index]->SetParent(Test.World);
				Test.Observe(Index);
				if (Test.Parts[Index]->GetObjectId() == OldId)
					throw std::runtime_error("[Runtime6:PhysicsSync] Reused stale ObjectId");
				++Creates;
			}
			const auto CreateMs = Milliseconds(Clock::now() - CreateStart);
			const auto ControlStart = Clock::now();
			for (std::size_t Item = 0; Item < ControlCount; ++Item) {
				const auto Relative = (Iteration * ControlCount + Item) % Configuration.DynamicCount;
				auto &Part = Test.Parts[Configuration.StaticCount + Relative];
				const float Velocity = 0.10f + static_cast<float>((Relative + Iteration) % 3) * 0.05f;
				Part->SetLinearVelocity({Velocity, 0.0f, 0.0f});
				Part->ApplyImpulse({0.001f, 0.0f, 0.0f});
				Part->SetAwake(true);
			}
			const auto ControlMs = Milliseconds(Clock::now() - ControlStart);
			const auto EventBefore = Test.CFrameEvents;
			const auto StepStart = Clock::now();
			Test.World->StepPhysics(1.0 / 60.0, std::nullopt);
			const auto StepMs = Milliseconds(Clock::now() - StepStart);
			double PublishMs = 0.0, ProjectionMs = 0.0;
			std::size_t RenderCreated = 0, RenderUpdated = 0, RenderRemoved = 0;
			if (Test.Publisher) {
				const auto PublishStart = Clock::now();
				auto Publication = Test.Publisher->Publish(*Test.World, Test.Camera, 1280, 720);
				PublishMs = Milliseconds(Clock::now() - PublishStart);
				const auto ProjectionStart = Clock::now();
				const auto Change = Test.Projection->Apply(*Publication);
				ProjectionMs = Milliseconds(Clock::now() - ProjectionStart);
				RenderCreated = Change.Created;
				RenderUpdated = Change.Updated;
				RenderRemoved = Change.Removed;
				if (Publication->FullResync || Test.Projection->GetObjects().size() != Total ||
					RenderCreated != Creates || RenderRemoved != Destroys)
					throw std::runtime_error("[Runtime6:PhysicsSync] Render lifecycle/count mismatch");
			}
			const auto ValidateStart = Clock::now();
			if (WorldRootTestAccess::BodyCount(*Test.World) != Total ||
				WorldRootTestAccess::PartCount(*Test.World) != Total || Creates != Destroys)
				throw std::runtime_error("[Runtime6:PhysicsSync] Body/Part lifecycle mismatch");
			if (ControlCount != 0 && (Test.CFrameEvents == EventBefore ||
				(Test.Publisher && RenderUpdated == 0)))
				throw std::runtime_error("[Runtime6:PhysicsSync] Nonzero controls produced no observed motion");
			double PositionChecksum = 0.0, VelocityChecksum = 0.0;
			for (std::size_t Index = 0; Index < Total; ++Index) {
				const auto &Part = Test.Parts[Index];
				const auto State = WorldRootTestAccess::State(*Test.World, *Part);
				if (!State || !State->Transform.FuzzyEq(Part->GetCFrame()) ||
					State->Description.Anchored != (Index < Configuration.StaticCount))
					throw std::runtime_error("[Runtime6:PhysicsSync] Authoritative/backend state divergence");
				PositionChecksum += static_cast<double>(Part->GetCFrame().Position.x) * (Index + 1);
				VelocityChecksum += static_cast<double>(State->LinearVelocity.x) * (Index + 1);
			}
			const auto ValidateMs = Milliseconds(Clock::now() - ValidateStart);
			PeakResident = std::max(PeakResident, ResidentBytes());
			const auto FrameMs = Milliseconds(Clock::now() - FrameStart);
			std::cout << "frame," << (Warmup ? "warmup" : "sample") << ',' << Frame << ','
				<< Configuration.StaticCount << ',' << Configuration.DynamicCount << ','
				<< Configuration.ControlDensity << ',' << ControlCount << ',' << ControlCount * 3 << ','
				<< Configuration.LifecyclePerFrame << ',' << Test.NextCreation << ','
				<< Test.NextCreation - Total << ',' << Configuration.DisableContacts << ','
				<< Configuration.Publication << ",0,0,0,0," << std::fixed << std::setprecision(6)
				<< CreateMs << ',' << ControlMs << ',' << StepMs << ',' << PublishMs << ','
				<< ProjectionMs << ',' << ValidateMs << ',' << FrameMs << ','
				<< WorldRootTestAccess::BodyCount(*Test.World) << ','
				<< WorldRootTestAccess::PartCount(*Test.World) << ','
				<< WorldRootTestAccess::DirtyBodyCount(*Test.World) << ','
				<< (Test.Projection ? Test.Projection->GetObjects().size() : 0) << ','
				<< RenderCreated << ',' << RenderUpdated << ',' << RenderRemoved << ','
				<< Test.CFrameEvents - EventBefore << ',' << MemoryMetric(ResidentBytes()) << ',' << MemoryMetric(PeakResident) << ','
				<< PositionChecksum << ',' << VelocityChecksum << '\n' << std::flush;
		}
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		Run(ParseOptions(ArgumentCount, Arguments));
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Runtime6:PhysicsSync] " << Failure.what() << '\n';
		return 1;
	}
}
