#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "../src/host/server/FrozenRecoveryQuote.hpp"
#include "../src/network/GameSessionTestAccess.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace gargantuan::host::detail {
struct FrozenRecoveryQuoteTestAccess {
	static bool WaitingAtCapacity(FrozenRecoveryQuote &Value) {
		std::scoped_lock Lock(Value.Mutex);
		return Value.WaitingForSpace && Value.Count == Value.QueueCapacity;
	}
};
}

namespace {
using namespace gargantuan;
using namespace gargantuan::network;
using host::detail::FrozenRecoveryQuote;
using host::detail::FrozenRecoveryQuoteTestAccess;

void Require(bool Value, const char *Message) {
	if (!Value) throw std::runtime_error(Message);
}
struct Fixture {
	std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
	std::shared_ptr<Folder> Object = std::make_shared<Folder>();
	std::unique_ptr<ReplicationCoordinator> Source;
	std::map<ConnectionId, std::size_t> Limits;
	explicit Fixture(std::uint32_t Peers) {
		Object->SetParent(World);
		Source = std::make_unique<ReplicationCoordinator>(World);
		for (std::uint32_t Slot = 1; Slot <= Peers; ++Slot) {
			const ConnectionId Connection{Slot, 1};
			const auto Baseline = Source->AddPeer(Connection, ReplicationEpoch(1));
			Require(Baseline.Frame.has_value(), "oracle fixture baseline must be accepted");
			Limits.emplace(Connection, MaximumReliableServiceGroupBytes - ReliableServiceEnvelopeBytes);
		}
		Object->SetName("oracle-captured-name");
	}
	~Fixture() { Source.reset(); Object->Destroy(); World->Destroy(); }
	std::unique_ptr<ReplicationCoordinator> Capture() {
		std::string Error;
		auto Result = Source->CaptureFrozenQuote(Error);
		Require(Result && Error.empty(), "oracle fixture captures owned cessation projection");
		return Result;
	}
};

template<class Predicate> void Await(Predicate &&Ready, const char *Message) {
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (!Ready()) {
		Require(std::chrono::steady_clock::now() < Deadline, Message);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void Same(const FrozenJournalQuoteStep &Left, const FrozenJournalQuoteStep &Right) {
	Require(Left.Complete == Right.Complete && Left.Error == Right.Error && Left.Frame.has_value() == Right.Frame.has_value(),
		"serial and worker oracle dispositions match exactly");
	if (!Left.Frame) return;
	const auto &A = *Left.Frame;
	const auto &B = *Right.Frame;
	Require(A.Connection == B.Connection && A.Sequence == B.Sequence && A.CompleteBytes == B.CompleteBytes &&
		A.Fingerprint == B.Fingerprint && A.CursorBefore == B.CursorBefore && A.CursorAfter == B.CursorAfter,
		"serial and worker oracle frame identity, bytes and coverage match exactly");
}

void TestExactReplayAndIsolation() {
	Fixture Input(4);
	auto Serial = Input.Capture();
	auto Threaded = Input.Capture();
	// Neither oracle may see a live mutation after its captured source fence.
	Input.Object->SetName("later-live-name-outside-the-captured-reference");
	std::vector<FrozenJournalQuoteStep> Expected;
	for (std::size_t Step = 0; Step < 100; ++Step) {
		auto Value = Serial->AdvanceFrozenJournalQuote(Input.Limits);
		Require(Value.Error.empty(), "serial oracle has no replay error");
		const bool Complete = Value.Complete;
		Expected.push_back(std::move(Value));
		if (Complete) break;
	}
	Require(!Expected.empty() && Expected.back().Complete, "serial reference terminates");
	struct MainObservation {
		std::thread::id Main = std::this_thread::get_id();
		std::atomic<unsigned> Calls = 0, WrongThread = 0;
		network::detail::StructuralCausalEvidenceSink Sink{this, [](void *Context, const network::detail::StructuralCausalEvent &) noexcept {
			auto &Self = *static_cast<MainObservation *>(Context);
			++Self.Calls;
			if (std::this_thread::get_id() != Self.Main) ++Self.WrongThread;
		}};
		network::detail::StructuralCausalEvidenceSink *Previous = network::detail::ActiveStructuralCausalEvidence;
		MainObservation() { network::detail::ActiveStructuralCausalEvidence = &Sink; }
		~MainObservation() { network::detail::ActiveStructuralCausalEvidence = Previous; }
	} Observation;
	runtime_detail::WorkSample MainWork{};
	runtime_detail::WorkCapture MainCapture(&MainWork);
	FrozenRecoveryQuote Oracle(std::move(Threaded), Input.Limits);
	network::detail::RecordStructuralCausal({});
	std::size_t Index = 0;
	std::uint64_t PreviousLag = std::numeric_limits<std::uint64_t>::max();
	Await([&] {
		while (auto Value = Oracle.TryPop()) {
			Require(Index < Expected.size(), "worker oracle cannot add an unowned result");
			Same(Expected[Index++], Value->Step);
			Require(Value->JournalLagRecords <= PreviousLag, "worker quote lag is local and monotonic");
			PreviousLag = Value->JournalLagRecords;
		}
		return Index == Expected.size();
	}, "worker oracle completes without Main waiting on replay");
	Require(PreviousLag == 0 && Observation.Calls == 1 && Observation.WrongThread == 0 &&
		network::detail::ActiveStructuralCausalEvidence == &Observation.Sink,
		"worker cannot borrow or replace Main causal observation");
	for (const auto &Duration : MainWork)
		Require(Duration.Nanoseconds == 0 && Duration.ExclusiveNanoseconds == 0,
			"worker timing capture does not charge Main instrumentation");
	Require(!Oracle.TryPop(), "terminal result is published once");
}

void TestCancellationAtFullQueue() {
	Fixture Input(static_cast<std::uint32_t>(FrozenRecoveryQuote::QueueCapacity + 8));
	auto Oracle = std::make_unique<FrozenRecoveryQuote>(Input.Capture(), Input.Limits);
	Await([&] { return FrozenRecoveryQuoteTestAccess::WaitingAtCapacity(*Oracle); },
		"bounded oracle queue reaches stop-aware producer wait");
	// No consumer frees space: destruction must wake the blocked publication
	// and join rather than requiring an extra pop or abandoning the thread.
	const auto Started = std::chrono::steady_clock::now();
	Oracle.reset();
	Require(std::chrono::steady_clock::now() - Started < std::chrono::seconds(5),
		"cancelled full oracle queue joins promptly without a consumer");
}

void TestImmediateCancellationAndError() {
	Fixture Input(2);
	{ FrozenRecoveryQuote Cancelled(Input.Capture(), Input.Limits); }
	auto Limits = Input.Limits;
	Limits.erase(Limits.begin());
	FrozenRecoveryQuote Invalid(Input.Capture(), std::move(Limits));
	std::optional<FrozenRecoveryQuote::Result> Result;
	Await([&] { Result = Invalid.TryPop(); return Result.has_value(); }, "invalid replay produces a terminal result");
	Require(!Result->Step.Error.empty() && !Result->Step.Complete && !Result->Step.Frame,
		"worker returns source validation error instead of successful completion");
	bool Rejected = false;
	try { FrozenRecoveryQuote Null(nullptr, Input.Limits); } catch (const std::invalid_argument &) { Rejected = true; }
	Require(Rejected, "invalid constructor cannot launch a worker");
}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestExactReplayAndIsolation();
		TestCancellationAtFullQueue();
		TestImmediateCancellationAndError();
		std::cout << "[Qualification:FrozenQuote] 3 focused cases passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Qualification:FrozenQuote] " << Error.what() << '\n';
		return 1;
	}
}
