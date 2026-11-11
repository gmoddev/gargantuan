// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <variant>

namespace gargantuan {
	struct WorldRootTestAccess {
		static bool FullPartScanNeeded(const WorldRoot &World) { return World.FullPartScanNeeded; }
		static std::size_t DirtyCount(const WorldRoot &World) { return World.DirtyBodies.size(); }
		static std::uint64_t DirtyEpoch(const WorldRoot &World) { return World.DirtyEpoch; }
		static bool BindingDirty(const WorldRoot &World, ObjectId Object) {
			auto Found = World.TrackedParts.find(Object);
			return Found != World.TrackedParts.end() && Found->second &&
				Found->second->LastDirtyEpoch == World.DirtyEpoch;
		}
		static void ClearDerivedDirtyState(WorldRoot &World, ObjectId Object) {
			World.DirtyBodies.clear();
			auto Found = World.TrackedParts.find(Object);
			if (Found != World.TrackedParts.end() && Found->second) Found->second->LastDirtyEpoch = 0;
		}
		static void ApplyPending(WorldRoot &World) { World.ApplyPendingPhysicsChanges(); }
		static SignalConnection::Pointer CFrameConnection(const WorldRoot &World, ObjectId Object) {
			auto Found = World.PhysicsConnections.find(Object);
			return Found == World.PhysicsConnections.end() || Found->second.empty() ? nullptr : Found->second.front();
		}
		static std::optional<PhysicsBodyState> State(const WorldRoot &World, ObjectId Object) {
			auto Found = World.PartBodies.find(Object);
			return Found == World.PartBodies.end() ? std::nullopt : World.Physics.GetBodyState(Found->second);
		}
	};
}

namespace {
	thread_local long long FailAllocation = -1;
	void *Allocate(std::size_t Bytes, std::size_t Alignment) {
		if (FailAllocation == 0) throw std::bad_alloc();
		if (FailAllocation > 0) --FailAllocation;
		Alignment = std::max(Alignment, alignof(void *));
		if (Bytes > std::numeric_limits<std::size_t>::max() - Alignment - sizeof(void *)) throw std::bad_alloc();
		void *Raw = std::malloc(Bytes + Alignment + sizeof(void *));
		if (!Raw) throw std::bad_alloc();
		const auto Start = reinterpret_cast<std::uintptr_t>(Raw) + sizeof(void *);
		const auto Address = (Start + Alignment - 1) & ~(static_cast<std::uintptr_t>(Alignment) - 1);
		auto Result = reinterpret_cast<void **>(Address);
		Result[-1] = Raw;
		return Result;
	}
	void Free(void *Value) noexcept { if (Value) std::free(static_cast<void **>(Value)[-1]); }
}

void *operator new(std::size_t Bytes) { return Allocate(Bytes, alignof(std::max_align_t)); }
void *operator new[](std::size_t Bytes) { return Allocate(Bytes, alignof(std::max_align_t)); }
void *operator new(std::size_t Bytes, std::align_val_t Align) { return Allocate(Bytes, static_cast<std::size_t>(Align)); }
void *operator new[](std::size_t Bytes, std::align_val_t Align) { return Allocate(Bytes, static_cast<std::size_t>(Align)); }
void *operator new(std::size_t Bytes, const std::nothrow_t &) noexcept { try { return ::operator new(Bytes); } catch (...) { return nullptr; } }
void *operator new[](std::size_t Bytes, const std::nothrow_t &) noexcept { try { return ::operator new[](Bytes); } catch (...) { return nullptr; } }
void *operator new(std::size_t Bytes, std::align_val_t Align, const std::nothrow_t &) noexcept { try { return ::operator new(Bytes, Align); } catch (...) { return nullptr; } }
void *operator new[](std::size_t Bytes, std::align_val_t Align, const std::nothrow_t &) noexcept { try { return ::operator new[](Bytes, Align); } catch (...) { return nullptr; } }
void operator delete(void *Value) noexcept { Free(Value); }
void operator delete[](void *Value) noexcept { Free(Value); }
void operator delete(void *Value, std::size_t) noexcept { Free(Value); }
void operator delete[](void *Value, std::size_t) noexcept { Free(Value); }
void operator delete(void *Value, std::align_val_t) noexcept { Free(Value); }
void operator delete[](void *Value, std::align_val_t) noexcept { Free(Value); }
void operator delete(void *Value, std::size_t, std::align_val_t) noexcept { Free(Value); }
void operator delete[](void *Value, std::size_t, std::align_val_t) noexcept { Free(Value); }
void operator delete(void *Value, const std::nothrow_t &) noexcept { Free(Value); }
void operator delete[](void *Value, const std::nothrow_t &) noexcept { Free(Value); }
void operator delete(void *Value, std::align_val_t, const std::nothrow_t &) noexcept { Free(Value); }
void operator delete[](void *Value, std::align_val_t, const std::nothrow_t &) noexcept { Free(Value); }

int main() {
	using namespace gargantuan;
	try {
		BootstrapNativeRuntimeSchema();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		if (!World) throw std::runtime_error("Workspace unavailable");
		World->SetGravity(0.0f);
		auto PartValue = std::make_shared<Part>();
		PartValue->SetAnchored(true);
		PartValue->SetCanCollide(false);
		PartValue->SetCanTouch(false);
		PartValue->SetCFrame(CFrame(0.0f, 5.0f, 0.0f));
		PartValue->SetParent(World);
		const auto Object = PartValue->GetObjectId();
		const auto Connection = WorldRootTestAccess::CFrameConnection(*World, Object);
		const auto Initial = WorldRootTestAccess::State(*World, Object);
		if (!Connection || !Initial || Initial->Description.Transform.Position.x != 0.0f)
			throw std::runtime_error("initial anchored body or physics listener unavailable");
		PartValue->SetCFrame(CFrame(3.0f, 5.0f, 0.0f));
		if (WorldRootTestAccess::DirtyCount(*World) != 1 ||
			!WorldRootTestAccess::BindingDirty(*World, Object))
			throw std::runtime_error("authored CFrame did not queue first dirty body update");
		// Test-only fault injection models a consumed derived hint just before a new first mark.
		// The authored Part state stays authoritative and must survive insertion failure.
		WorldRootTestAccess::ClearDerivedDirtyState(*World, Object);
		bool Threw = false;
		FailAllocation = 0;
		try { Connection->Callback(std::monostate{}); }
		catch (const std::bad_alloc &) { Threw = true; }
		FailAllocation = -1;
		if (!Threw || !WorldRootTestAccess::FullPartScanNeeded(*World) ||
			WorldRootTestAccess::DirtyCount(*World) != 0 ||
			WorldRootTestAccess::BindingDirty(*World, Object))
			throw std::runtime_error("failed first dirty insertion did not retain conservative scan");
		World->StepPhysics(0.0, std::nullopt);
		const auto State = WorldRootTestAccess::State(*World, Object);
		if (!State || State->Description.Transform.Position.x != 3.0f ||
			WorldRootTestAccess::FullPartScanNeeded(*World) ||
			WorldRootTestAccess::DirtyCount(*World) != 0)
			throw std::runtime_error("conservative scan lost the committed authored CFrame");
		PartValue->SetCFrame(CFrame(4.0f, 5.0f, 0.0f));
		World->StepPhysics(0.0, std::nullopt);
		const auto Recovered = WorldRootTestAccess::State(*World, Object);
		if (!Recovered || Recovered->Description.Transform.Position.x != 4.0f ||
			WorldRootTestAccess::DirtyCount(*World) != 0)
			throw std::runtime_error("dirty binding did not resume after allocation recovery");
		PartValue->SetCFrame(CFrame(5.0f, 5.0f, 0.0f));
		const auto BeforeCaptureEpoch = WorldRootTestAccess::DirtyEpoch(*World);
		Threw = false;
		// AssetService is already initialized by the earlier safe points. The
		// next allocation copies DirtyBodies into the sorted update capture.
		FailAllocation = 0;
		try { WorldRootTestAccess::ApplyPending(*World); }
		catch (const std::bad_alloc &) { Threw = true; }
		FailAllocation = -1;
		const auto BeforeRetry = WorldRootTestAccess::State(*World, Object);
		if (!Threw || WorldRootTestAccess::DirtyEpoch(*World) != BeforeCaptureEpoch ||
			WorldRootTestAccess::DirtyCount(*World) != 1 ||
			!WorldRootTestAccess::BindingDirty(*World, Object) || !BeforeRetry ||
			BeforeRetry->Description.Transform.Position.x != 4.0f)
			throw std::runtime_error("failed sorted update capture consumed the dirty epoch");
		WorldRootTestAccess::ApplyPending(*World);
		const auto AfterRetry = WorldRootTestAccess::State(*World, Object);
		if (!AfterRetry || AfterRetry->Description.Transform.Position.x != 5.0f ||
			WorldRootTestAccess::DirtyCount(*World) != 0 ||
			WorldRootTestAccess::BindingDirty(*World, Object))
			throw std::runtime_error("retry after sorted capture fault lost authored control");
		std::cout << "[Runtime6:PhysicsDirtyBindingFault] PASS\n";
		return 0;
	} catch (const std::exception &Failure) {
		FailAllocation = -1;
		std::cerr << "[Runtime6:PhysicsDirtyBindingFault] " << Failure.what() << '\n';
		return 1;
	}
}
