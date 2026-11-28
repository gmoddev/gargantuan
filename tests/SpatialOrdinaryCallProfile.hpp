// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "BenchmarkAllocationProbe.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>

namespace gargantuan::ordinary_benchmark {
	enum class Function { ShortClassLookup, QualifiedClassLookup, ScriptCharacterMove, CharacterAdmitMotion,
		CharacterCommit, CharacterRootSync, PendingImpulses, DynamicReadback, KinematicResolve, EffectiveWorld,
		Publish, ProjectionApply, CameraBuild, VisualAssetRead, DirtyCapture, DirtyExpansion, FinalExtraction,
		PublicationBuild, PublisherReconcile, ProjectionValidation, ProjectionMutation, Count };
	inline constexpr std::array FunctionNames{"ShortClassLookup","QualifiedClassLookup","ScriptCharacterMove",
		"CharacterAdmitMotion","CharacterCommit","CharacterRootSync","PendingImpulses","DynamicReadback",
		"KinematicResolve","EffectiveWorld","Publish","ProjectionApply","CameraBuild","VisualAssetRead","DirtyCapture",
		"DirtyExpansion","FinalExtraction","PublicationBuild","PublisherReconcile","ProjectionValidation","ProjectionMutation"};
	static_assert(FunctionNames.size() == static_cast<std::size_t>(Function::Count));
	struct FunctionValue { std::uint64_t Calls=0,Nanoseconds=0,Allocations=0,Bytes=0; };
	struct MotionSample {
		std::uint32_t Calls=0,NegativeVelocity=0,HasFloor=0,Collided=0;
		std::uint32_t LastRequestedYBits=0,LastInputVelocityYBits=0,LastOutputVelocityYBits=0,LastAppliedYBits=0,LastPositionYBits=0;
	};
	using Functions=std::array<FunctionValue,static_cast<std::size_t>(Function::Count)>;
	inline thread_local Functions *ActiveFunctions=nullptr;
	inline thread_local MotionSample *ActiveMotion=nullptr;
	inline void RecordPhase(Function Id, std::uint64_t Nanoseconds) {
		if (!ActiveFunctions) return;
		auto &Value = (*ActiveFunctions)[static_cast<std::size_t>(Id)];
		++Value.Calls;
		Value.Nanoseconds += Nanoseconds;
	}
	inline void RecordMotion(float RequestedY,float InputY,float OutputY,float AppliedY,float PositionY,bool Floor,bool Collided) {
		if (!ActiveMotion) return;
		++ActiveMotion->Calls;
		ActiveMotion->NegativeVelocity+=OutputY<0;
		ActiveMotion->HasFloor+=Floor;
		ActiveMotion->Collided+=Collided;
		ActiveMotion->LastRequestedYBits=std::bit_cast<std::uint32_t>(RequestedY);
		ActiveMotion->LastInputVelocityYBits=std::bit_cast<std::uint32_t>(InputY);
		ActiveMotion->LastOutputVelocityYBits=std::bit_cast<std::uint32_t>(OutputY);
		ActiveMotion->LastAppliedYBits=std::bit_cast<std::uint32_t>(AppliedY);
		ActiveMotion->LastPositionYBits=std::bit_cast<std::uint32_t>(PositionY);
	}
	class FunctionCapture {
	public:
		FunctionCapture(Functions &Destination,MotionSample &Motion) : Previous(ActiveFunctions),PreviousMotion(ActiveMotion) {
			ActiveFunctions=&Destination; ActiveMotion=&Motion;
		}
		~FunctionCapture() { ActiveFunctions=Previous; ActiveMotion=PreviousMotion; }
	private:
		Functions *Previous;
		MotionSample *PreviousMotion;
	};
	class FunctionScope {
	public:
		explicit FunctionScope(Function Id) : Target(ActiveFunctions?&(*ActiveFunctions)[static_cast<std::size_t>(Id)]:nullptr) {
			if (!Target) return;
			Begin=std::chrono::steady_clock::now();
			Calls=BenchmarkAllocationProbe::ReadCalls(); Bytes=BenchmarkAllocationProbe::ReadBytes();
		}
		~FunctionScope() {
			if (!Target) return;
			const auto Elapsed=std::chrono::steady_clock::now()-Begin;
			++Target->Calls;
			Target->Nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Elapsed).count());
			Target->Allocations+=BenchmarkAllocationProbe::ReadCalls()-Calls;
			Target->Bytes+=BenchmarkAllocationProbe::ReadBytes()-Bytes;
		}
	private:
		FunctionValue *Target;
		std::chrono::steady_clock::time_point Begin;
		std::uint64_t Calls=0,Bytes=0;
	};
}
