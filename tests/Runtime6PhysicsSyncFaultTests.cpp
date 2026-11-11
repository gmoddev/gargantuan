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

namespace gargantuan {
	struct WorldRootTestAccess {
		static bool CompleteControlScanNeeded(const WorldRoot &World) { return World.FullControlScanNeeded; }
		static std::size_t PendingControls(const WorldRoot &World) { return World.PendingBodyControls.size(); }
		static void ClearPendingControls(WorldRoot &World) { World.PendingBodyControls.clear(); }
		static void QueueControl(WorldRoot &World, const BasePart &Part) { World.QueuePendingBodyControl(Part); }
		static std::size_t DynamicBodies(const WorldRoot &World) { return World.DynamicBodies.size(); }
		static bool HasDynamicObject(const WorldRoot &World, ObjectId Object) {
			return World.DynamicPositions.contains(Object);
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
		PartValue->SetCanCollide(false);
		PartValue->SetCanTouch(false);
		PartValue->SetParent(World);
		const auto OldId = PartValue->GetObjectId();
		if (WorldRootTestAccess::DynamicBodies(*World) != 1 ||
			WorldRootTestAccess::PendingControls(*World) != 0)
			throw std::runtime_error("initial compact body/control state mismatch");
		PartValue->SetLinearVelocity({0.5f, 0.0f, 0.0f});
		if (WorldRootTestAccess::PendingControls(*World) != 1)
			throw std::runtime_error("authored velocity did not queue one pending control");
		WorldRootTestAccess::ClearPendingControls(*World);
		FailAllocation = 0;
		WorldRootTestAccess::QueueControl(*World, *PartValue);
		FailAllocation = -1;
		if (!WorldRootTestAccess::CompleteControlScanNeeded(*World) ||
			WorldRootTestAccess::PendingControls(*World) != 0)
			throw std::runtime_error("allocation fault did not select complete control scan");
		World->StepPhysics(1.0 / 60.0, std::nullopt);
		const auto State = WorldRootTestAccess::State(*World, OldId);
		if (!State || State->LinearVelocity.x < 0.4f || PartValue->GetCFrame().Position.x <= 0.0f ||
			WorldRootTestAccess::CompleteControlScanNeeded(*World) ||
			WorldRootTestAccess::PendingControls(*World) != 0)
			throw std::runtime_error("complete-scan fallback lost an authored body control");
		PartValue->Destroy();
		if (WorldRootTestAccess::HasDynamicObject(*World, OldId) ||
			WorldRootTestAccess::DynamicBodies(*World) != 0)
			throw std::runtime_error("destroyed generation remained in dynamic index");
		auto Replacement = std::make_shared<Part>();
		Replacement->SetParent(World);
		if (Replacement->GetObjectId() == OldId ||
			WorldRootTestAccess::DynamicBodies(*World) != 1 ||
			WorldRootTestAccess::HasDynamicObject(*World, OldId))
			throw std::runtime_error("replacement aliased retired compact identity");
		std::cout << "[Runtime6:PhysicsSyncFault] PASS\n";
		return 0;
	} catch (const std::exception &Failure) {
		FailAllocation = -1;
		std::cerr << "[Runtime6:PhysicsSyncFault] " << Failure.what() << '\n';
		return 1;
	}
}
