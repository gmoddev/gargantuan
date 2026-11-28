// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
// Included only by a separately linked diagnostic EditorHost, never by a shipping target.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "BenchmarkAllocationProbe.hpp"
#include "SpatialOrdinaryCallProfile.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace gargantuan::ordinary_benchmark {
	using namespace runtime_detail;
	inline constexpr std::array Selected{WorkPhase::EngineStep,WorkPhase::PreSimulation,WorkPhase::Physics,
		WorkPhase::PhysicsUpdate,WorkPhase::PhysicsRigidStep,WorkPhase::PhysicsEvents,WorkPhase::PhysicsSoftColliders,
		WorkPhase::PhysicsSoftStep,WorkPhase::PhysicsBackendStep,WorkPhase::Animation,WorkPhase::RenderPublication,
		WorkPhase::RootMotion,WorkPhase::ContentStep,WorkPhase::InstanceWrap,WorkPhase::GameplayMotion,
		WorkPhase::GameplayPoseCommit,WorkPhase::AudioReconcile,WorkPhase::TweenStep};

	struct ThreadReading {
		PERFORMANCE_DATA Data{};
		DWORD Error = ERROR_NOT_SUPPORTED;
		ULONG64 FallbackCycles = 0;
	};
	struct VmCounts {
		std::uint64_t Events=0,NewEvents=0,ResizeEvents=0,RequestedBytes=0,GrowthBytes=0,ShrinkBytes=0;
		std::uint64_t BeforeBytes=0,AfterBytes=0;
	};
	class VmCapture {
	public:
		explicit VmCapture(const std::shared_ptr<ScriptEngine> &Script) : Owner(Script),Previous(Active) {
			if (!Owner || !Owner->L) return;
			State=Owner->L;
			Saved=lua_callbacks(State)->onallocate;
			Counts.BeforeBytes=lua_totalbytes(State,-1);
			Active=this;
			lua_callbacks(State)->onallocate=Observe;
		}
		~VmCapture() { Finish(); }
		void Finish() {
			if (!State) return;
			if (Owner->L==State) {
				Counts.AfterBytes=lua_totalbytes(State,-1);
				if (lua_callbacks(State)->onallocate==Observe) lua_callbacks(State)->onallocate=Saved;
			}
			Active=Previous;
			State=nullptr;
		}
		VmCounts Counts{};
	private:
		static void Observe(lua_State *L,std::size_t Before,std::size_t After) {
			auto *Scope=Active;
			if (!Scope) return;
			auto &C=Scope->Counts;
			++C.Events;
			if (Before==0) ++C.NewEvents; else ++C.ResizeEvents;
			C.RequestedBytes+=After;
			if (After>=Before) C.GrowthBytes+=After-Before; else C.ShrinkBytes+=Before-After;
			if (Scope->Saved && Scope->Saved!=Observe) Scope->Saved(L,Before,After);
		}
		inline static thread_local VmCapture *Active=nullptr;
		std::shared_ptr<ScriptEngine> Owner;
		VmCapture *Previous;
		lua_State *State=nullptr;
		void (*Saved)(lua_State*,std::size_t,std::size_t)=nullptr;
	};

	struct TickRecord {
		std::uint64_t Tick = 0, Cycles = 0, Switches = 0, Reasons = 0;
		DWORD ReadError = 0;
		std::array<std::uint64_t,MAX_HW_COUNTERS> Hardware{};
		unsigned HardwareCount = 0;
		SignalDispatchProfile Signals{};
		Functions FunctionsCalled{};
		MotionSample Motion{};
		VmCounts Vm{};
		std::array<WorkDuration,Selected.size()> Phases{};
	};

	class Profile final {
	public:
		Profile() {
			const char *Destination = std::getenv("GARGANTUAN_ORDINARY_PROFILE_CSV");
			if (!Destination || !*Destination) return;
			Path=Destination;
			EnableError=EnableThreadProfiling(GetCurrentThread(),THREAD_PROFILING_FLAG_DISPATCH,0xffff,&Handle);
			Records.reserve(4096);
		}
		~Profile() {
			if (Handle) DisableThreadProfiling(Handle);
			if (Path.empty()) return;
			std::ofstream File(Path);
			if (!File) { std::fprintf(stderr,"[Spatial:StepProfile] unable to write raw native rows\n"); return; }
			File<<"Tick,Cycles,ContextSwitches,WaitReasons,EnableError,ReadError,HardwareCount,Dropped,SignalCalls,DeferredCalls,ConnectionVisits,NativeCallbacks,ScriptCallbacks,SignalSnapshotNs,SignalCallbackNs,SignalCleanupNs,SignalTotalNs";
			for (std::size_t Index=0;Index<MAX_HW_COUNTERS;++Index) File<<",Hardware"<<Index;
			File<<",LuauAllocEvents,LuauNewEvents,LuauResizeEvents,LuauRequestedBytes,LuauGrowthBytes,LuauShrinkBytes,LuauHeapBeforeBytes,LuauHeapAfterBytes";
			File<<",MotionCalls,NegativeMotionVelocity,MotionHasFloor,MotionCollided,LastRequestedYBits,LastInputVelocityYBits,LastOutputVelocityYBits,LastAppliedYBits,LastPositionYBits";
			for (const char *Name : FunctionNames)
				for (const char *Field : {"Ns","Calls","AllocationCalls","AllocationBytes"}) File<<','<<Name<<Field;
			for (const auto Phase : Selected) {
				const auto Name=WorkPhaseNames[static_cast<std::size_t>(Phase)];
				for (const char *Field : {"Ns","ExclusiveNs","Calls","Units","AllocationCalls","AllocationBytes"}) File<<','<<Name<<Field;
			}
			File<<'\n';
			for (const auto &Row : Records) {
				const auto &S=Row.Signals;
				File<<Row.Tick<<','<<Row.Cycles<<','<<Row.Switches<<','<<Row.Reasons<<','<<EnableError<<','<<Row.ReadError
					<<','<<Row.HardwareCount<<','<<Dropped<<','<<S.Calls<<','<<S.DeferredCalls<<','<<S.ConnectionVisits
					<<','<<S.NativeCallbacks<<','<<S.ScriptCallbacks<<','<<S.SnapshotNanoseconds<<','<<S.CallbackNanoseconds
					<<','<<S.CleanupNanoseconds<<','<<S.TotalNanoseconds;
				for (auto Value : Row.Hardware) File<<','<<Value;
				const auto &Vm=Row.Vm;
				File<<','<<Vm.Events<<','<<Vm.NewEvents<<','<<Vm.ResizeEvents<<','<<Vm.RequestedBytes<<','<<Vm.GrowthBytes
					<<','<<Vm.ShrinkBytes<<','<<Vm.BeforeBytes<<','<<Vm.AfterBytes;
				const auto &M=Row.Motion;
				File<<','<<M.Calls<<','<<M.NegativeVelocity<<','<<M.HasFloor<<','<<M.Collided<<','<<M.LastRequestedYBits
					<<','<<M.LastInputVelocityYBits<<','<<M.LastOutputVelocityYBits<<','<<M.LastAppliedYBits<<','<<M.LastPositionYBits;
				for (const auto &Function : Row.FunctionsCalled) File<<','<<Function.Nanoseconds<<','<<Function.Calls
					<<','<<Function.Allocations<<','<<Function.Bytes;
				for (const auto &Phase : Row.Phases) File<<','<<Phase.Nanoseconds<<','<<Phase.ExclusiveNanoseconds
					<<','<<Phase.Calls<<','<<Phase.Units<<','<<Phase.Allocations<<','<<Phase.AllocatedBytes;
				File<<'\n';
			}
		}
		ThreadReading Read() const {
			ThreadReading Snapshot;
			Snapshot.Data.Size=sizeof(PERFORMANCE_DATA);
			Snapshot.Data.Version=PERFORMANCE_DATA_VERSION;
			if (Handle) Snapshot.Error=ReadThreadProfilingData(Handle,
				READ_THREAD_PROFILING_FLAG_DISPATCHING|READ_THREAD_PROFILING_FLAG_HARDWARE_COUNTERS,&Snapshot.Data);
			if (Snapshot.Error!=ERROR_SUCCESS) QueryThreadCycleTime(GetCurrentThread(),&Snapshot.FallbackCycles);
			return Snapshot;
		}
		void Step(Engine &Runtime, WorkSample &Sample) {
			if (Path.empty()) { const WorkCapture Capture(&Sample); Runtime.Step(); return; }
			BenchmarkAllocationProbe::Counts Allocation;
			const BenchmarkAllocationProbe::Scope AllocationScope(Allocation);
			Functions Called{};
			MotionSample Motion{};
			const FunctionCapture Calls(Called,Motion);
			VmCapture Vm(Runtime.Script);
			BaseSignal::ResetDispatchProfile();
			BaseSignal::SetDispatchProfilingEnabled(true);
			struct SignalGuard { ~SignalGuard(){ BaseSignal::SetDispatchProfilingEnabled(false); } } SignalLifetime;
			const auto Before=Read();
			{
				const WorkCapture Capture(&Sample,BenchmarkAllocationProbe::ReadCalls,BenchmarkAllocationProbe::ReadBytes);
				Runtime.Step();
			}
			const auto After=Read();
			Vm.Finish();
			if (Records.size()==Records.capacity()) { ++Dropped; return; }
			auto &Row=Records.emplace_back();
			Row.Tick=Runtime.GetSimulationTick();
			Row.ReadError=Before.Error?Before.Error:After.Error;
			if (Row.ReadError==ERROR_SUCCESS) {
				Row.Cycles=After.Data.CycleTime-Before.Data.CycleTime;
				Row.Switches=After.Data.ContextSwitchCount-Before.Data.ContextSwitchCount;
				Row.Reasons=After.Data.WaitReasonBitMap;
				Row.HardwareCount=After.Data.HwCountersCount;
				for (std::size_t Index=0;Index<MAX_HW_COUNTERS;++Index)
					Row.Hardware[Index]=After.Data.HwCounters[Index].Value-Before.Data.HwCounters[Index].Value;
			} else if (Before.Error && After.Error) Row.Cycles=After.FallbackCycles-Before.FallbackCycles;
			Row.Signals=BaseSignal::GetDispatchProfile();
			Row.FunctionsCalled=Called;
			Row.Motion=Motion;
			Row.Vm=Vm.Counts;
			for (std::size_t Index=0;Index<Selected.size();++Index)
				Row.Phases[Index]=Sample[static_cast<std::size_t>(Selected[Index])];
		}
	private:
		std::string Path;
		std::vector<TickRecord> Records;
		HANDLE Handle=nullptr;
		DWORD EnableError=ERROR_NOT_SUPPORTED;
		std::size_t Dropped=0;
	};

	inline void Step(Engine &Runtime, WorkSample &Sample) {
		static thread_local Profile Collector;
		Collector.Step(Runtime,Sample);
	}
}
