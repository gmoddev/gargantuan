#include "gargantuan/persistence/PersistenceRuntime.hpp"

#include "../runtime/RuntimeWorkDiagnostics.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace gargantuan {
	struct PersistenceResultBudget final {
		std::atomic_size_t Reserved = 0;
	};

	namespace {
		struct WorkerResult final {
			PersistenceResult Result;
			std::atomic_bool Ready = false;
		};

		void RequireMain() {
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main)
				throw std::logic_error("[Persistence:Runtime] Main execution domain required");
		}

		bool SameOwner(const std::weak_ptr<Script>& Left, const std::weak_ptr<Script>& Right) {
			return !Left.owner_before(Right) && !Right.owner_before(Left);
		}

		std::size_t ReserveResultBytes(const PersistenceRequest& Request) {
			const auto RecordBytes = sizeof(PersistenceRecord) + PersistenceMaxKeyBytes + PersistenceMaxValueBytes;
			const auto DocumentBytes = RecordBytes + PersistenceMaxDocumentNodes * sizeof(PersistenceDocumentField);
			if (Request.Operation == PersistenceOperation::Query)
				return Request.Query.Limit * DocumentBytes + PersistenceMaxCursorBytes;
			if (Request.Operation == PersistenceOperation::Get)
				return Request.Kind == PersistenceStoreKind::Document ? DocumentBytes : RecordBytes;
			return 0;
		}

		PersistenceResult ExecuteRequest(IPersistenceProvider& Provider, const PersistenceRequest& Request,
			std::stop_token Stop) {
			if (Stop.stop_requested()) return {.Status = PersistenceStatus::Cancelled};
			if (PersistenceClock::now() >= Request.Deadline) return {.Status = PersistenceStatus::Deadline};
			// Native exceptions or malformed success responses cannot prove that
			// a dispatched mutation did not commit.
			try {
				auto Result = Provider.Execute(Request, Stop);
				if (IsPersistenceResultValid(Request, Result)) return Result;
			} catch (...) {
			}
			return {.Status = IsPersistenceMutation(Request.Operation) ?
				PersistenceStatus::UnknownOutcome : PersistenceStatus::Unavailable};
		}
	}

	struct PersistenceRuntime::State final {
		struct Entry final {
			std::weak_ptr<Script> Owner;
			std::shared_ptr<PersistenceHandle> Handle;
			std::shared_ptr<WorkerResult> Work;
		};
		std::shared_ptr<IPersistenceProvider> Provider;
		std::shared_ptr<PersistenceResultBudget> ResultBudget = std::make_shared<PersistenceResultBudget>();
		std::weak_ptr<DataModel> World;
		JobSystem Workers{2, JobSystem::StartPolicy::OnFirstSubmission};
		std::vector<Entry> Entries;
		std::string RequestPrefix;
		std::uint64_t Sequence = 0;
		bool Stopping = false;
	};

	bool PersistenceHandle::Cancel() {
		RequireMain();
		return Result.Status == PersistenceStatus::Pending && Stop.request_stop();
	}

	PersistenceHandle::~PersistenceHandle() { ReleaseReservation(); }

	void PersistenceHandle::ReleaseReservation() noexcept {
		if (ReservedBytes != 0) {
			Budget->Reserved.fetch_sub(ReservedBytes, std::memory_order_relaxed);
			ReservedBytes = 0;
			Budget.reset();
		}
	}

	void PersistenceHandle::ConsumePayload() {
		RequireMain();
		if (Result.Status != PersistenceStatus::Ok || Result.Revision != 0) return;
		Result = {.Status = PersistenceStatus::Consumed};
		ReleaseReservation();
	}

	PersistenceRuntime::PersistenceRuntime(std::shared_ptr<IPersistenceProvider> Provider, std::weak_ptr<DataModel> World)
		: Runtime(std::make_unique<State>()) {
		Runtime->Provider = std::move(Provider);
		Runtime->World = std::move(World);
		Runtime->Entries.reserve(MaximumInFlight);
		if (Runtime->Provider) Runtime->RequestPrefix = ProjectId::New().ToString();
	}

	PersistenceRuntime::~PersistenceRuntime() { Shutdown(); }

	PersistenceCapabilities PersistenceRuntime::Capabilities() const {
		return Runtime->Provider ? Runtime->Provider->Capabilities() : PersistenceCapabilities{};
	}

	std::shared_ptr<PersistenceHandle> PersistenceRuntime::Submit(PersistenceRequest Request, std::weak_ptr<Script> Owner,
		std::chrono::milliseconds Timeout) {
		RequireMain();
		auto Handle = std::make_shared<PersistenceHandle>();
		const auto Reject = [&](PersistenceStatus Status) {
			Handle->Result.Status = Status;
			return Handle;
		};
		if (Timeout <= std::chrono::milliseconds::zero() || Timeout > MaximumDeadline ||
			!IsPersistenceRequestValid(Request)) return Reject(PersistenceStatus::Invalid);
		if (Runtime->Stopping) return Reject(PersistenceStatus::Cancelled);
		if (!Runtime->Provider) return Reject(PersistenceStatus::Unavailable);
		const auto Features = Capabilities();
		if ((Request.Kind == PersistenceStoreKind::KeyValue && !Features.KeyValue) ||
			(Request.Kind == PersistenceStoreKind::Document && !Features.Documents) ||
			(Request.Operation == PersistenceOperation::Query && !Features.DocumentQueries))
			return Reject(PersistenceStatus::Unsupported);
		if (Runtime->Entries.size() >= MaximumInFlight) return Reject(PersistenceStatus::Busy);
		const bool HasOwner = !SameOwner(Owner, {});
		if (HasOwner) {
			const auto ScriptValue = Owner.lock();
			if (!ScriptValue || !ScriptValue->GetEnabled() || ScriptValue->GetDestroyed() ||
				ScriptValue->IsDestroying() || ScriptValue->GetDataModel() != Runtime->World.lock())
				return Reject(PersistenceStatus::Cancelled);
		}
		const auto Count = std::ranges::count_if(Runtime->Entries, [&](const State::Entry& Entry) {
			return SameOwner(Entry.Owner, Owner);
		});
		if (Count >= (HasOwner ? MaximumPerOwner : MaximumUnattributed)) return Reject(PersistenceStatus::Busy);

		const auto Bytes = ReserveResultBytes(Request);
		const auto& Budget = Runtime->ResultBudget;
		if (Bytes > MaximumReservedResultBytes - Budget->Reserved.load(std::memory_order_relaxed))
			return Reject(PersistenceStatus::Busy);
		if (Bytes != 0) {
			Budget->Reserved.fetch_add(Bytes, std::memory_order_relaxed);
			Handle->Budget = Budget;
			Handle->ReservedBytes = Bytes;
		}

		Request.Deadline = PersistenceClock::now() + Timeout;
		Request.RequestId = Runtime->RequestPrefix + "-" + std::to_string(++Runtime->Sequence);
		auto Work = std::make_shared<WorkerResult>();
		try {
			Runtime->Workers.Submit([Provider = Runtime->Provider, Work, Request = std::move(Request),
				Stop = Handle->Stop.get_token()] {
				Work->Result = ExecuteRequest(*Provider, Request, Stop);
				Work->Ready.store(true, std::memory_order_release);
			});
		} catch (...) {
			Handle->ReleaseReservation();
			return Reject(PersistenceStatus::Unavailable);
		}
		Runtime->Entries.push_back({std::move(Owner), Handle, std::move(Work)});
		return Handle;
	}

	void PersistenceRuntime::Pump() {
		RequireMain();
		if (Runtime->Entries.empty()) return;
		runtime_detail::WorkScope Measure(runtime_detail::WorkPhase::PersistencePump);
		const auto World = Runtime->World.lock();
		for (auto& Entry : Runtime->Entries) {
			if (SameOwner(Entry.Owner, {})) continue;
			const auto Owner = Entry.Owner.lock();
			if (!Owner || Owner->GetDestroyed() || Owner->IsDestroying() ||
				!Owner->GetEnabled() || Owner->GetDataModel() != World)
				(void)Entry.Handle->Cancel();
		}
		std::size_t Published = 0;
		std::erase_if(Runtime->Entries, [&](const State::Entry& Entry) {
			if (Published >= MaximumCompletionsPerStep || !Entry.Work->Ready.load(std::memory_order_acquire))
				return false;
			Entry.Handle->Result = std::move(Entry.Work->Result);
			if (Entry.Handle->Result.Status != PersistenceStatus::Ok) Entry.Handle->ReleaseReservation();
			++Published;
			return true;
		});
	}

	void PersistenceRuntime::Shutdown() {
		if (Runtime->Stopping) return;
		Runtime->Stopping = true;
		for (const auto& Entry : Runtime->Entries) Entry.Handle->Stop.request_stop();
		// Drop jobs that never began. Only the two executing jobs can delay
		// shutdown, each governed by the provider cancellation/deadline contract.
		Runtime->Workers.Shutdown(false);
		for (auto& Entry : Runtime->Entries) {
			Entry.Handle->Result = Entry.Work->Ready.load(std::memory_order_acquire) ?
				std::move(Entry.Work->Result) : PersistenceResult{.Status = PersistenceStatus::Cancelled};
			if (Entry.Handle->Result.Status != PersistenceStatus::Ok) Entry.Handle->ReleaseReservation();
		}
		Runtime->Entries.clear();
		Runtime->Provider.reset();
	}
}
