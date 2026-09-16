#include "../src/runtime/PreparedPropertyCommit.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include <chrono>
#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <new>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#endif

namespace gargantuan {
	struct PreparedPropertyTestAccess {
		static void SetRevision(DataModel &World, std::uint64_t Value) { World.AuthoritativeRevision = Value; }
		static void DropStream(ObjectId Scope) { ChangeJournal::Get().ReleaseScope(Scope); }
		static bool HasStream(ObjectId Scope) {
			auto &Journal = ChangeJournal::Get(); std::scoped_lock Lock(Journal.Mutex);
			return Journal.Streams.contains(Scope);
		}
		static void SetSequence(ObjectId Scope, std::uint64_t Sequence) {
			auto &Journal = ChangeJournal::Get(); std::scoped_lock Lock(Journal.Mutex);
			Journal.Streams.at(Scope).NextSequence = Sequence;
		}
	};
}

namespace {
	thread_local bool RejectAllocations = false;
	thread_local long long FailAllocation = -1;
	thread_local std::size_t AllocationCount = 0;
	thread_local std::size_t AllocationBytes = 0;
	long long SweepIndex = -1;
	std::size_t BoundaryCount = 0;
	std::chrono::steady_clock::time_point BoundaryStart;
	std::chrono::nanoseconds CommitTime{};
	void *Allocate(std::size_t Bytes, std::size_t Alignment) {
		if (RejectAllocations) {
			std::fputs("[Prepared:AllocationAfterBoundary] unexpected operator new\n", stderr);
			throw std::bad_alloc();
		}
		if (FailAllocation == 0) throw std::bad_alloc();
		if (FailAllocation > 0) --FailAllocation;
		++AllocationCount;
		AllocationBytes += Bytes;
		Alignment = std::max(Alignment, alignof(void *));
		void *Raw = std::malloc(Bytes + Alignment + sizeof(void *));
		if (!Raw) throw std::bad_alloc();
		const auto Start = reinterpret_cast<std::uintptr_t>(Raw) + sizeof(void *);
		const auto Address = (Start + Alignment - 1) & ~(Alignment - 1);
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

namespace {
	using namespace gargantuan;
	void Require(bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); }
	const auto Security = ScriptSecurityContext::CoreTrusted();
	void Boundary(bool Enter) noexcept {
		RejectAllocations = Enter;
		if (Enter) { ++BoundaryCount; BoundaryStart = std::chrono::steady_clock::now(); }
		else CommitTime = std::chrono::steady_clock::now() - BoundaryStart;
	}
	PreparedPropertyWrite Write(const std::shared_ptr<Instance> &Object, std::string Name, WireValue Value) {
		const auto *Property = Object->FindProperty(Name);
		return {Object->GetObjectId(), Property->DeclaringSchemaId, Property->DeclaringDefinitionVersion,
			std::move(Name), std::move(Value)};
	}
	struct Fixture {
		std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
		std::shared_ptr<Part> A = std::make_shared<Part>();
		std::shared_ptr<Part> B = std::make_shared<Part>();
		std::vector<PreparedPropertyWrite> Writes;
		ChangeCursor Cursor;
		std::size_t Fires = 0;
		Fixture() {
			A->SetParent(World); B->SetParent(World);
			A->SetName(std::string(96, 'a')); B->SetName(std::string(96, 'b'));
			Writes = {Write(A, "Name", std::string(128, 'x')), Write(B, "Name", std::string(128, 'y')),
				Write(A, "Transparency", WireFloat{0.25f}), Write(B, "Size", WireVector3{2, 3, 4}),
				Write(A, "Shape", WireEnumItem{"PartType", "Ball"})};
			for (const auto &Entry : Writes) {
				auto Target = ObjectRegistry::Get().Lookup(Entry.Object);
				Target->GetPropertyChangedSignal(Entry.PropertyName)->Connect([this](std::monostate) { ++Fires; });
			}
			ChangeJournal::Get().Clear();
			Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
		}
		~Fixture() {
			PreparedPropertyCommit::SetQualificationHooks({});
			for (auto Object : {A, B}) for (auto &[Name, Signal] : Object->PropertyChangedSignals) Signal->DisconnectAll();
			World->Destroy();
			ChangeJournal::Get().SetCapacity(DefaultChangeJournalCapacity);
		}
		PreparedPropertyResult Apply() {
			return PreparedPropertyCommit::Apply(*World, World->GetAuthoritativeRevision(), Writes, Security);
		}
	};
	struct Snapshot {
		std::vector<WireValue> Values;
		std::uint64_t Revision;
		TransactionHistoryStatus History;
		std::deque<std::shared_ptr<const CommittedTransaction>> Actions;
		std::vector<ChangeRecord> Records;
		std::uint64_t NextSequence;
		std::size_t Fires;
		bool HadStream;
		explicit Snapshot(Fixture &F) : Revision(F.World->GetAuthoritativeRevision()), History(F.World->Transactions.GetStatus()),
			Actions(F.World->Transactions.GetCommitted()), Records(ChangeJournal::Get().Read(F.Cursor).Records),
			NextSequence(ChangeJournal::Get().CreateCursor(F.World->GetObjectId()).NextSequence), Fires(F.Fires),
			HadStream(PreparedPropertyTestAccess::HasStream(F.World->GetObjectId())) {
			for (const auto &Entry : F.Writes)
				Values.push_back(*ObjectRegistry::Get().Lookup(Entry.Object)->ReadPropertyWireValue(Entry.PropertyName));
		}
		void Unchanged(Fixture &F) const {
			Snapshot After(F);
			Require(Values == After.Values, "failure changed live values");
			Require(Revision == After.Revision && Fires == After.Fires, "failure changed revision or notified");
			Require(Actions == After.Actions && History.Cursor == After.History.Cursor && History.SemanticBytes == After.History.SemanticBytes,
				"failure changed history/cursor/retention");
			Require(NextSequence == After.NextSequence && Records.size() == After.Records.size() && HadStream == After.HadStream,
				"failure published journal or leaked empty stream");
			for (std::size_t Index = 0; Index < Records.size(); ++Index) {
				const auto &Before = Records[Index]; const auto &Next = After.Records[Index];
				Require(Before.Sequence == Next.Sequence && Before.Object == Next.Object && Before.Scope == Next.Scope,
					"failure changed journal identities");
				Require(std::get<PropertyUpdatedChange>(Before.Payload).Value == std::get<PropertyUpdatedChange>(Next.Payload).Value,
					"failure changed journal payload");
			}
		}
	};

	void TestCommitReplay() {
		Fixture F;
		Snapshot Before(F);
		const auto Revision = Before.Revision;
		bool SawComplete = false;
		F.A->GetPropertyChangedSignal("Name")->Connect([&](std::monostate) {
			SawComplete = true;
			for (const auto &Entry : F.Writes)
				Require(ObjectRegistry::Get().Lookup(Entry.Object)->ReadPropertyWireValue(Entry.PropertyName) == Entry.Value,
					"observer saw a prefix");
			Require(F.World->GetAuthoritativeRevision() == Revision + 1, "observer saw old revision");
			Require(F.World->Transactions.GetCommitted().size() == 1 && ChangeJournal::Get().Read(F.Cursor).Records.size() == F.Writes.size(),
				"observer saw incomplete history/journal");
		});
		PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
		const auto Result = F.Apply();
		Require(Result.Status == MutationStatus::Success && Result.ChangedWrites == F.Writes.size() && SawComplete && Result.NotificationFailures == 0,
			"prepared commit/observer failed");
		Require(F.World->Transactions.GetCommitted().size() == 1 && F.World->GetAuthoritativeRevision() == Revision + 1,
			"batch did not commit one action/revision");
		F.A->GetPropertyChangedSignal("Name")->DisconnectAll();
		bool UndoVisible = true;
		std::size_t ReplayObservers = 0;
		F.B->GetPropertyChangedSignal("Name")->Connect([&](std::monostate) {
			for (std::size_t Index = 0; Index < F.Writes.size(); ++Index) {
				const auto &Entry = F.Writes[Index];
				Require(ObjectRegistry::Get().Lookup(Entry.Object)->ReadPropertyWireValue(Entry.PropertyName) ==
					(UndoVisible ? Before.Values[Index] : Entry.Value), "replay observer saw a prefix");
			}
			++ReplayObservers;
		});
		MutationGateway Gateway;
		const auto UndoResult = Gateway.Undo(*F.World, Security);
		Require(UndoResult.Succeeded() && UndoResult.NotificationFailures == 0, "prepared undo failed");
		for (std::size_t Index = 0; Index < F.Writes.size(); ++Index)
			Require(ObjectRegistry::Get().Lookup(F.Writes[Index].Object)->ReadPropertyWireValue(F.Writes[Index].PropertyName) == Before.Values[Index],
				"undo did not restore complete state");
		UndoVisible = false;
		const auto RedoResult = Gateway.Redo(*F.World, Security);
		Require(RedoResult.Succeeded() && RedoResult.NotificationFailures == 0 && ReplayObservers == 2, "prepared redo failed");
		Require(F.World->GetAuthoritativeRevision() == Revision + 3 && F.World->Transactions.GetStatus().Cursor == 1 &&
			ChangeJournal::Get().Read(F.Cursor).Records.size() == 3 * F.Writes.size(), "replay publication is incoherent");
		Snapshot NoOp(F);
		Require(F.Apply().Status == MutationStatus::Success, "no-op failed");
		NoOp.Unchanged(F);
	}

	PreparedPropertyStage FailureStage;
	std::size_t FailureIndex = 0;
	void Inject(PreparedPropertyStage Stage, std::size_t Index) { if (Stage == FailureStage && Index == FailureIndex) throw std::bad_alloc(); }
	void TestStageFailures() {
		for (bool Replay : {false, true}) for (bool Redo : {false, true}) {
			for (int Stage = 0; Stage <= static_cast<int>(PreparedPropertyStage::JournalReservation); ++Stage) {
				const bool PerWrite = Stage >= 1 && Stage <= 3;
				for (std::size_t Index = 0; Index < (PerWrite ? 5u : 1u); ++Index) {
					Fixture F;
					MutationGateway Gateway;
					if (Replay) {
						Require(F.Apply().Status == MutationStatus::Success, "failure fixture commit");
						if (Redo) Require(Gateway.Undo(*F.World, Security).Succeeded(), "failure fixture undo");
					}
					Snapshot Before(F);
					FailureStage = static_cast<PreparedPropertyStage>(Stage); FailureIndex = Index;
					PreparedPropertyCommit::SetQualificationHooks({Inject, Boundary});
					if (Replay) Require(!(Redo ? Gateway.Redo(*F.World, Security) : Gateway.Undo(*F.World, Security)).Succeeded(), "injection did not reject replay");
					else Require(F.Apply().Status != MutationStatus::Success, "injection did not reject forward");
					PreparedPropertyCommit::SetQualificationHooks({});
					Before.Unchanged(F);
				}
			}
		}
	}

	void TestEveryAllocation() {
		for (int Mode = 0; Mode < 5; ++Mode) {
			std::size_t Rejections = 0;
			for (long long Allocation = 0; Allocation < 20000; ++Allocation) {
				Fixture F; MutationGateway Gateway;
				if (Mode > 0 && Mode < 4) {
					Require(F.Apply().Status == MutationStatus::Success, "allocation fixture commit");
					if (Mode >= 2) Require(Gateway.Undo(*F.World, Security).Succeeded(), "allocation fixture undo");
					F.World->Transactions.RemapIdentity(F.A->GetObjectId(), F.A->GetObjectId());
				}
				if (Mode == 4) {
					PreparedPropertyTestAccess::DropStream(F.World->GetObjectId());
					F.Cursor = ChangeJournal::Get().CreateCursor(F.World->GetObjectId());
				}
				Snapshot Before(F);
				PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
				FailAllocation = Allocation;
				SweepIndex = Allocation;
				const bool Success = Mode == 0 || Mode >= 3 ? F.Apply().Status == MutationStatus::Success :
					(Mode == 1 ? Gateway.Undo(*F.World, Security) : Gateway.Redo(*F.World, Security)).Succeeded();
				FailAllocation = -1;
				Require(!RejectAllocations, "boundary instrumentation leaked");
				if (Success) break;
				Before.Unchanged(F);
				++Rejections;
			}
			Require(Rejections > 10 && Rejections < 19999, "allocation sweep failed to reach successful commit");
			std::cout << "[Prepared:Allocation] mode=" << Mode << " rejected=" << Rejections << '\n';
		}
	}

	void TestDenialsAndNotifications() {
		Fixture F;
		auto Denied = [&](auto Change) {
			auto Saved = F.Writes;
			Snapshot Before(F);
			Change();
			Require(F.Apply().Status != MutationStatus::Success, "invalid request accepted");
			F.Writes = std::move(Saved);
			Before.Unchanged(F);
		};
		Denied([&] { F.Writes.push_back(F.Writes[0]); });
		Denied([&] { F.Writes.resize(257, F.Writes[0]); });
		Denied([&] { F.Writes[0].DefinitionVersion++; });
		Denied([&] { F.Writes[0].Object.Generation++; });
		Denied([&] { F.Writes[0].Value = std::string(65537, 'z'); });
		Denied([&] { F.Writes[0].Value = 42; });
		Denied([&] { F.Writes[0].PropertyName = std::string(257, 'p'); });
		Denied([&] { F.Writes[2].Value = WireFloat{-1.0f}; });
		Denied([&] { F.Writes[2].Value = WireFloat{std::numeric_limits<float>::infinity()}; });
		Denied([&] { F.Writes[4].Value = WireEnumItem{"PartType", "Missing"}; });
		Denied([&] { F.Writes[0] = Write(F.A, "Position", WireVector3{1, 2, 3}); });
		Denied([&] { F.Writes[0] = Write(F.A, "Parent", WireObjectReference{WireObjectId::FromObjectId(F.B->GetObjectId())}); });
		ChangeJournal::Get().SetCapacity(4);
		Denied([] {});
		ChangeJournal::Get().SetCapacity(DefaultChangeJournalCapacity);
		Snapshot Before(F);
		Require(PreparedPropertyCommit::Apply(*F.World, Before.Revision - 1, F.Writes, Security).Status == MutationStatus::Conflict,
			"stale revision accepted");
		Before.Unchanged(F);
		F.A->GetPropertyChangedSignal("Name")->Connect([](std::monostate) { throw std::runtime_error("observer failure"); });
		const auto Result = F.Apply();
		Require(Result.Status == MutationStatus::Success && Result.NotificationFailures == 1 && F.Fires == F.Writes.size(),
			"observer exception lost authoritative success or later delivery");
		MutationGateway Gateway;
		F.B->SetSize({9, 9, 9});
		Snapshot Diverged(F);
		Require(!Gateway.Undo(*F.World, Security).Succeeded(), "divergent replay accepted");
		Diverged.Unchanged(F);
	}

	void TestBoundsAndCosts() {
		for (std::size_t Count : {1u, 32u, 256u}) {
			Fixture F;
			std::vector<std::shared_ptr<Part>> Objects;
			F.Writes.clear();
			for (std::size_t Index = 0; Index < Count; ++Index) {
				auto Object = std::make_shared<Part>(); Object->SetParent(F.World);
				Objects.push_back(Object); F.Writes.push_back(Write(Object, "Name", std::string(96, 'n')));
			}
			ChangeJournal::Get().Clear();
			PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
			const auto Allocations = AllocationCount; const auto Bytes = AllocationBytes;
			const auto Start = std::chrono::steady_clock::now();
			const auto Result = F.Apply();
			const auto Elapsed = std::chrono::steady_clock::now() - Start;
			Require(Result.Status == MutationStatus::Success && Result.ChangedWrites == Count, "bounded batch failed");
			std::cout << "[Prepared:Cost] writes=" << Count << " allocations=" << AllocationCount - Allocations
				<< " allocatedBytes=" << AllocationBytes - Bytes << " valueBytes=" << Result.ValueBytes
				<< " journalBytes=" << Result.JournalBytes << " historyBytes=" << Result.HistoryBytes
				<< " totalNs=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Elapsed).count()
				<< " commitNs=" << CommitTime.count() << '\n';
			MutationGateway Gateway;
			Require(Gateway.Undo(*F.World, Security).Succeeded() && Gateway.Redo(*F.World, Security).Succeeded(), "bounded replay failed");
		}
	}

	Fixture *InvalidationFixture = nullptr;
	bool InvalidateIdentity = false;
	void Invalidate(PreparedPropertyStage Stage, std::size_t) {
		if (Stage != PreparedPropertyStage::FinalValidation) return;
		if (InvalidateIdentity) ObjectRegistry::Get().Invalidate(InvalidationFixture->A->GetObjectId());
		else InvalidationFixture->World->AdvanceAuthoritativeRevision();
	}
	void TestFinalValidationAndRetention() {
		{
			Fixture F;
			PreparedPropertyTestAccess::SetSequence(F.World->GetObjectId(), std::numeric_limits<std::uint64_t>::max() - 2);
			F.Cursor = ChangeJournal::Get().CreateCursor(F.World->GetObjectId());
			Snapshot Before(F);
			Require(F.Apply().Status == MutationStatus::RevisionExhausted, "journal sequence exhaustion accepted");
			Before.Unchanged(F);
			PreparedPropertyTestAccess::SetSequence(F.World->GetObjectId(), 1);
		}
		for (bool Identity : {false, true}) {
			Fixture F; Snapshot Before(F);
			InvalidationFixture = &F; InvalidateIdentity = Identity;
			PreparedPropertyCommit::SetQualificationHooks({Invalidate, Boundary});
			Require(F.Apply().Status == MutationStatus::Conflict, "final invalidation accepted");
			PreparedPropertyCommit::SetQualificationHooks({});
			Require(F.A->ReadPropertyWireValue("Name") == Before.Values[0] && F.B->ReadPropertyWireValue("Name") == Before.Values[1] &&
				F.A->GetTransparency() == 0 && F.B->GetSize() == glm::vec3(1) && F.Fires == 0,
				"invalidated batch wrote a prefix");
			Require(F.World->GetAuthoritativeRevision() == Before.Revision + (Identity ? 0 : 1) &&
				F.World->Transactions.GetCommitted().empty() && ChangeJournal::Get().Read(F.Cursor).Records.empty(),
				"invalidated batch contributed revision/history/journal");
		}
		{
			Fixture F; Snapshot Before(F);
			PreparedPropertyTestAccess::SetRevision(*F.World, std::numeric_limits<std::uint64_t>::max());
			Require(F.Apply().Status == MutationStatus::RevisionExhausted, "revision exhaustion accepted");
			PreparedPropertyTestAccess::SetRevision(*F.World, Before.Revision);
			Before.Unchanged(F);
			auto Foreign = std::make_shared<DataModel>();
			Require(PreparedPropertyCommit::Apply(*Foreign, Foreign->GetAuthoritativeRevision(), F.Writes, Security).Status == MutationStatus::Unauthorized,
				"wrong world accepted");
			Before.Unchanged(F);
			{ ExecutionDomainScope Worker(ExecutionDomain::Worker);
				Require(F.Apply().Status == MutationStatus::WrongExecutionDomain, "worker domain accepted"); }
			Before.Unchanged(F);
			auto Group = F.World->Transactions.Begin(*F.World, 42, "ordinary group");
			Require(F.Apply().Status != MutationStatus::Success, "prepared batch entered ordinary transaction");
			Require(F.World->Transactions.Commit(*F.World, Group.Id, 42).Status == TransactionStatus::NoChanges, "group was modified");
			Before.Unchanged(F);
			bool Changed = false;
			{ ScopedAuthoritativeRevisionDeferral Deferral(*F.World, Changed);
				Require(F.Apply().Status != MutationStatus::Success, "prepared batch entered revision deferral"); }
			Before.Unchanged(F);
			F.World->Transactions = AuthoritativeTransactionHistory(MaximumRetainedTransactions, 1);
			Require(F.Apply().Status == MutationStatus::ResourceLimit, "history byte limit accepted oversized action");
			Before.Unchanged(F);
		}
		{
			Fixture F; MutationGateway Gateway;
			F.World->Transactions = AuthoritativeTransactionHistory(2, MaximumRetainedTransactionBytes);
			ChangeJournal::Get().SetCapacity(5);
			Require(F.Apply().Status == MutationStatus::Success, "capacity-exact batch rejected");
			const auto First = F.World->Transactions.GetUndoTransaction()->Id;
			F.Writes = {Write(F.A, "Name", std::string(90, '2'))}; Require(F.Apply().Status == MutationStatus::Success, "second retention action");
			Require(Gateway.Undo(*F.World, Security).Succeeded(), "retention undo");
			F.Writes = {Write(F.A, "Name", std::string(90, '3'))}; Require(F.Apply().Status == MutationStatus::Success, "redo suffix replacement");
			Require(!F.World->Transactions.GetStatus().CanRedo && F.World->Transactions.GetCommitted().front()->Id == First,
				"redo suffix pruning changed prefix");
			F.Writes = {Write(F.A, "Name", std::string(90, '4'))}; Require(F.Apply().Status == MutationStatus::Success, "front eviction action");
			Require(F.World->Transactions.GetCommitted().size() == 2 && F.World->Transactions.GetCommitted().front()->Id != First &&
				ChangeJournal::Get().Read(F.Cursor).Status == ChangeReadStatus::ResnapshotRequired, "retention/eviction failed");
		}
		{
			Fixture F;
			F.Writes = {Write(F.A, "CFrame", *EncodeNativeWireValue(CFrame(glm::vec3(1, 2, 3))))};
			PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
			Require(F.Apply().Status == MutationStatus::Success, "CFrame raw store unsupported");
			MutationGateway Gateway;
			Require(Gateway.Undo(*F.World, Security).Succeeded() && Gateway.Redo(*F.World, Security).Succeeded(), "CFrame prepared replay");
		}
	}

	std::atomic<bool> ReaderGo = false;
	std::atomic<bool> ReaderAttempt = false;
	void ReaderBoundary(bool Enter) noexcept {
		Boundary(Enter);
		if (Enter) {
			ReaderGo.store(true, std::memory_order_release);
			while (!ReaderAttempt.load(std::memory_order_acquire)) std::this_thread::yield();
		}
	}
	void TestReaderFenceAndLargeValues() {
		{
			Fixture F;
			const auto Revision = F.World->GetAuthoritativeRevision();
			std::uint64_t ObservedRevision = 0;
			std::size_t ObservedRecords = 0;
			std::thread Reader([&] {
				while (!ReaderGo.load(std::memory_order_acquire)) std::this_thread::yield();
				ReaderAttempt.store(true, std::memory_order_release);
				ObservedRecords = ChangeJournal::Get().Read(F.Cursor).Records.size();
				ObservedRevision = F.World->GetAuthoritativeRevision();
			});
			PreparedPropertyCommit::SetQualificationHooks({nullptr, ReaderBoundary});
			const auto Result = F.Apply();
			// Always release/join the test reader even if preparation unexpectedly rejects.
			ReaderGo.store(true, std::memory_order_release);
			Reader.join();
			Require(Result.Status == MutationStatus::Success && ObservedRecords == F.Writes.size() && ObservedRevision == Revision + 1,
				"journal reader saw publication with old revision");
		}
		{
			Fixture F;
			std::vector<std::shared_ptr<Part>> Objects;
			F.Writes.clear();
			for (std::size_t Index = 0; Index < 65; ++Index) {
				auto Object = std::make_shared<Part>(); Object->SetParent(F.World); Object->SetName(std::string(65536, 'o'));
				Objects.push_back(Object); F.Writes.push_back(Write(Object, "Name", std::string("new")));
			}
			ChangeJournal::Get().Clear();
			F.Cursor = ChangeJournal::Get().CreateCursor(F.World->GetObjectId());
			Snapshot Before(F);
			Require(F.Apply().Status == MutationStatus::ResourceLimit, "aggregate old/new bound not enforced");
			Before.Unchanged(F);
			for (auto &Entry : F.Writes) Entry.Value = std::string(65536, 'n');
			Snapshot Envelope(F);
			Require(F.Apply().Status == MutationStatus::ResourceLimit, "request envelope bound not enforced");
			Envelope.Unchanged(F);
			// A small forward request can capture large old values. Replay must
			// still reserve its own journal budget before restoring any of them.
			F.Writes.resize(33);
			for (auto &Entry : F.Writes) Entry.Value = std::string("new");
			Require(F.Apply().Status == MutationStatus::Success, "large captured old values rejected within value budget");
			Snapshot ReplayBudget(F);
			MutationGateway Gateway;
			Require(!Gateway.Undo(*F.World, Security).Succeeded(), "replay journal payload limit not enforced");
			ReplayBudget.Unchanged(F);
		}
		{
			Fixture F; MutationGateway Gateway;
			F.A->SetArchivable(true);
			F.Writes = {Write(F.A, "Name", std::string("restored property"))};
			Require(F.Apply().Status == MutationStatus::Success, "alias fixture commit");
			const auto Historical = F.A->GetObjectId();
			const auto Authority = MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), F.World->GetObjectId());
			Require(Gateway.Apply(DestroyObjectCommand{Historical}, Authority).Succeeded(), "legacy destroy after prepared action");
			Require(Gateway.Undo(*F.World, Security).Succeeded(), "legacy restore before prepared undo");
			const auto Current = F.World->Transactions.ResolveIdentity(Historical);
			Require(Current != Historical && ObjectRegistry::Get().Lookup(Current), "legacy restore did not remap generation");
			PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
			Require(Gateway.Undo(*F.World, Security).Succeeded() && Gateway.Redo(*F.World, Security).Succeeded(),
				"prepared replay failed after legacy identity remap");
		}
	}

	void TestAdditionalPropertyTypes() {
		Fixture F;
		auto Gui = std::make_shared<Frame>(); Gui->SetParent(F.World);
		ChangeJournal::Get().Clear();
		F.Cursor = ChangeJournal::Get().CreateCursor(F.World->GetObjectId());
		F.Writes = {Write(Gui, "AnchorPoint", WireVector2{0.25f, 0.5f}),
			Write(Gui, "BackgroundColor3", WireColor3{0.2f, 0.3f, 0.4f}),
			Write(Gui, "Position", WireUDim2{{0.25f, 10}, {0.5f, 20}}),
			Write(F.A, "Anchored", true)};
		Gui->PropertyChangedSignals.clear();
		const auto SignalCount = Gui->PropertyChangedSignals.size();
		PreparedPropertyCommit::SetQualificationHooks({nullptr, Boundary});
		Require(F.Apply().Status == MutationStatus::Success && Gui->PropertyChangedSignals.size() == SignalCount,
			"compound raw stores failed or preparation created signals");
		MutationGateway Gateway;
		Require(Gateway.Undo(*F.World, Security).Succeeded() && Gateway.Redo(*F.World, Security).Succeeded(), "compound prepared replay");
		Snapshot Before(F);
		Require(PreparedPropertyCommit::Apply(*F.World, Before.Revision, F.Writes, ScriptSecurityContext{}).Status == MutationStatus::Unauthorized,
			"missing capability accepted");
		Before.Unchanged(F);
	}
}

int main() {
	std::set_terminate([] {
		std::fprintf(stderr, "[Prepared:Terminate] boundary=%d sweep=%lld remaining=%lld\n", RejectAllocations, SweepIndex, FailAllocation);
		std::_Exit(3);
	});
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	try {
		std::cerr << "[Prepared:Test] bootstrap\n";
		gargantuan::BootstrapNativeRuntimeSchema();
		std::cerr << "[Prepared:Test] commit/replay\n"; TestCommitReplay();
		std::cerr << "[Prepared:Test] stages\n"; TestStageFailures();
		std::cerr << "[Prepared:Test] allocations\n"; TestEveryAllocation();
		std::cerr << "[Prepared:Test] denials\n"; TestDenialsAndNotifications();
		std::cerr << "[Prepared:Test] bounds/costs\n"; TestBoundsAndCosts();
		TestFinalValidationAndRetention();
		TestReaderFenceAndLargeValues();
		TestAdditionalPropertyTypes();
		std::cout << "[Prepared:Qualification] passed boundaries=" << BoundaryCount << '\n';
		return 0;
	} catch (const std::exception &Error) {
		RejectAllocations = false; FailAllocation = -1;
		std::cerr << "[Prepared:Failure] " << Error.what() << '\n';
		return 1;
	}
}
