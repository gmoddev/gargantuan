#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/persistence/LocalPersistenceProvider.hpp"
#include "gargantuan/persistence/PersistenceRuntime.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include "BenchmarkAllocationProbe.hpp"

#if defined(GARGANTUAN_WITH_NODE_PERSISTENCE)
#include "host/server/NodePersistenceProvider.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace std::chrono_literals;
	using Status = PersistenceStatus;
	using Operation = PersistenceOperation;

	void Require(bool Condition, std::string_view Detail) {
		if (!Condition) throw std::runtime_error(std::string(Detail));
	}

	template <typename Predicate> void WaitFor(Predicate Done, std::string_view Detail,
		std::chrono::milliseconds Limit = 5s) {
		const auto Deadline = PersistenceClock::now() + Limit;
		while (!Done()) {
			Require(PersistenceClock::now() < Deadline, Detail);
			std::this_thread::sleep_for(1ms);
		}
	}

	class ControlledProvider final : public IPersistenceProvider {
	public:
		std::atomic_int Entered = 0;
		std::atomic_bool Hold = true;
		std::atomic_int ObservedCancellation = 0;

		PersistenceCapabilities Capabilities() const noexcept override { return {true, true, false}; }

		PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
			Require(GetCurrentExecutionDomain() == ExecutionDomain::Worker, "provider executed on Main");
			Entered.fetch_add(1);
			if (Request.Key == "bad") return {.Status = Status::Ok};
			if (Request.Key == "throw") throw std::runtime_error("simulated loss of acknowledgement");
			while (Hold.load() && !Stop.stop_requested() && PersistenceClock::now() < Request.Deadline)
				std::this_thread::sleep_for(1ms);
			if (Stop.stop_requested()) {
				ObservedCancellation.fetch_add(1);
				return {.Status = IsPersistenceMutation(Request.Operation) ? Status::UnknownOutcome : Status::Cancelled};
			}
			return {.Status = Hold.load() ? Status::Deadline : Status::Missing};
		}
	};

	class PageProvider final : public IPersistenceProvider {
	public:
		PersistenceCapabilities Capabilities() const noexcept override { return {true, true, true}; }

		PersistenceResult Execute(const PersistenceRequest&, std::stop_token) override {
			return {
				.Status = Status::Ok,
				.Records = {{"First", PersistenceDocumentValue{PersistenceDocumentValue::Object{}}, 1}},
			};
		}
	};

	class LostAcknowledgementProvider final : public IPersistenceProvider {
	public:
		PersistenceCapabilities Capabilities() const noexcept override { return Memory->Capabilities(); }

		PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
			auto Result = Memory->Execute(Request, Stop);
			if (Request.Operation == Operation::Put && Result.Status == Status::Ok && !Lost.exchange(true))
				return {.Status = Status::UnknownOutcome};
			return Result;
		}

	private:
		std::shared_ptr<IPersistenceProvider> Memory = MakeMemoryPersistenceProvider();
		std::atomic_bool Lost = false;
	};

	class DelayedProvider final : public IPersistenceProvider {
	public:
		PersistenceCapabilities Capabilities() const noexcept override { return Memory->Capabilities(); }

		PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
			const auto End = PersistenceClock::now() + 20ms;
			while (PersistenceClock::now() < End && !Stop.stop_requested())
				std::this_thread::sleep_for(1ms);
			return Memory->Execute(Request, Stop);
		}

	private:
		std::shared_ptr<IPersistenceProvider> Memory = MakeMemoryPersistenceProvider();
	};

#if defined(GARGANTUAN_WITH_NODE_PERSISTENCE)
	class SynchronizedProvider final : public IPersistenceProvider {
	public:
		SynchronizedProvider(std::shared_ptr<IPersistenceProvider> Provider, std::filesystem::path Gate)
			: Provider(std::move(Provider)), Gate(std::move(Gate)) {}

		PersistenceCapabilities Capabilities() const noexcept override { return Provider->Capabilities(); }

		PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
			if (Request.Operation != Operation::Put || Observed.exchange(true)) return Provider->Execute(Request, Stop);
			std::cout << "PERSISTENCE_LUAU_READY_TO_CAS" << std::endl;
			while (!std::filesystem::exists(Gate)) {
				if (Stop.stop_requested()) return {.Status = Status::Cancelled};
				if (PersistenceClock::now() >= Request.Deadline) return {.Status = Status::Deadline};
				std::this_thread::sleep_for(1ms);
			}
			auto Result = Provider->Execute(Request, Stop);
			std::cout << "PERSISTENCE_LUAU_CAS_" << GetPersistenceStatusName(Result.Status) << std::endl;
			return Result;
		}

	private:
		std::shared_ptr<IPersistenceProvider> Provider;
		std::filesystem::path Gate;
		std::atomic_bool Observed = false;
	};
#endif

	PersistenceRequest Request(std::string Key, Operation Op = Operation::Get) {
		return {.Operation = Op, .Store = "Runtime", .Key = std::move(Key)};
	}

	std::shared_ptr<Script> AddScript(ScriptEngine& Engine, std::string Source) {
		auto ScriptValue = std::make_shared<Script>();
		ScriptValue->SetRunContext(Enums::RunContext::Server);
		ScriptValue->SetSource(std::move(Source));
		ScriptValue->SetParent(Engine.DataModel);
		Engine.ScriptQueue.insert(ScriptValue);
		return ScriptValue;
	}

	void RunScript(ScriptEngine& Engine, std::string Source) {
		auto ScriptValue = AddScript(Engine, std::move(Source));
		WaitFor([&] {
			Engine.Step();
			Require(ScriptValue->Status != ScriptStatus::Error, ScriptValue->ErrorMessage);
			return ScriptValue->Status == ScriptStatus::Finished;
		}, "Luau persistence script did not finish");
		ScriptValue->Destroy();
	}

	void CheckRuntimeBounds() {
		auto Provider = std::make_shared<ControlledProvider>();
		auto World = std::make_shared<DataModel>();
		ScriptEngine Engine(World);
		Engine.ConfigurePersistence(Provider);
		auto Runtime = Engine.GetPersistenceRuntime();
		auto Owner = AddScript(Engine, "task.wait(60)");
		auto Read = Runtime->Submit(Request("active"), Owner);
		auto Write = Runtime->Submit(Request("active", Operation::Put));
		WaitFor([&] { return Provider->Entered == 2; }, "workers did not start");
		auto Expiry = Runtime->Submit(Request("expire"), {}, 2ms);
		auto Cancelled = Runtime->Submit(Request("cancel"), Owner);
		Require(Cancelled->Cancel(), "queued cancellation was not accepted");
		std::vector<std::shared_ptr<PersistenceHandle>> Pending;
		for (std::size_t Index = 2; Index < PersistenceRuntime::MaximumPerOwner; ++Index)
			Pending.push_back(Runtime->Submit(Request("owner"), Owner));
		Require(Runtime->Submit(Request("owner"), Owner)->Poll().Status == Status::Busy, "per-owner quota");
		for (std::size_t Index = 2; Index < PersistenceRuntime::MaximumUnattributed; ++Index)
			Pending.push_back(Runtime->Submit(Request("runtime")));
		Require(Runtime->Submit(Request("runtime"))->Poll().Status == Status::Busy, "unattributed runtime quota");
		std::vector<std::shared_ptr<Script>> OtherOwners;
		for (std::size_t Index = Pending.size() + 4; Index < PersistenceRuntime::MaximumInFlight; ++Index) {
			if (OtherOwners.empty() || Index % PersistenceRuntime::MaximumPerOwner == 0)
				OtherOwners.push_back(AddScript(Engine, "return"));
			Pending.push_back(Runtime->Submit(Request("runtime"), OtherOwners.back()));
		}
		auto ExtraOwner = AddScript(Engine, "return");
		Require(Runtime->Submit(Request("runtime"), ExtraOwner)->Poll().Status == Status::Busy, "runtime quota");
		std::this_thread::sleep_for(3ms);
		Require(Write->Cancel(), "in-flight mutation stop was not signaled");
		Owner->Destroy();
		WaitFor([&] {
			Runtime->Pump();
			return Read->Poll().Status != Status::Pending && Write->Poll().Status != Status::Pending;
		}, "in-flight cancellations were not published");
		Require(Read->Poll().Status == Status::Cancelled && Write->Poll().Status == Status::UnknownOutcome,
			"definite read cancellation and uncertain write classification");
		Provider->Hold = false;
		WaitFor([&] {
			Runtime->Pump();
			return Expiry->Poll().Status != Status::Pending && Cancelled->Poll().Status != Status::Pending;
		}, "queued cancellation or expiry was not published");
		Require(Expiry->Poll().Status == Status::Deadline && Cancelled->Poll().Status == Status::Cancelled,
			"expired and canceled queue entries must never enter provider");
		WaitFor([&] {
			Runtime->Pump();
			return std::ranges::none_of(Pending, [](const auto& Item) { return Item->Poll().Status == Status::Pending; });
		}, "bounded completions did not drain");
		auto Bad = Runtime->Submit(Request("bad"));
		auto MalformedWrite = Runtime->Submit(Request("bad", Operation::Put));
		auto ThrownWrite = Runtime->Submit(Request("throw", Operation::Put));
		WaitFor([&] {
			Runtime->Pump();
			return ThrownWrite->Poll().Status != Status::Pending && Bad->Poll().Status != Status::Pending &&
				MalformedWrite->Poll().Status != Status::Pending;
		}, "malformed response was not published");
		Require(Bad->Poll().Status == Status::Unavailable && MalformedWrite->Poll().Status == Status::UnknownOutcome &&
			ThrownWrite->Poll().Status == Status::UnknownOutcome, "provider failures must preserve mutation uncertainty");
		Provider->Hold = true;
		auto Shutdown = Runtime->Submit(Request("pending shutdown", Operation::Put));
		const auto Start = PersistenceClock::now();
		Engine.ShutdownRuntime();
		Require(PersistenceClock::now() - Start < 500ms, "bounded shutdown stalled");
		Require(Shutdown->Poll().Status == Status::Cancelled || Shutdown->Poll().Status == Status::UnknownOutcome,
			"shutdown must settle outstanding operation and retire provider");
	}

	void CheckResultRetention() {
		PersistenceRuntime Runtime(std::make_shared<PageProvider>(), {});
		PersistenceRequest Query{.Kind = PersistenceStoreKind::Document, .Operation = Operation::Query, .Store = "Pages"};
		std::vector<std::shared_ptr<PersistenceHandle>> Unobserved;
		for (std::size_t Index = 0; Index <= PersistenceRuntime::MaximumInFlight; ++Index) {
			auto Handle = Runtime.Submit(Query);
			if (Handle->Poll().Status == Status::Busy) break;
			WaitFor([&] {
				Runtime.Pump();
				return Handle->Poll().Status != Status::Pending;
			}, "page was not published");
			Require(Handle->Poll().Status == Status::Ok, "valid page response");
			Unobserved.push_back(std::move(Handle));
		}
		Require(Unobserved.size() > 1 && Unobserved.size() < PersistenceRuntime::MaximumInFlight,
			"completed pages must be charged after publication");
		Unobserved.front()->ConsumePayload();
		Require(Unobserved.front()->Poll().Status == Status::Consumed, "consumed status");
		Require(Runtime.Submit(Query)->Poll().Status == Status::Pending, "consumed payload capacity was not returned");
	}

	constexpr std::string_view Wait = R"(
local function Wait(Op)
	local Result = Op:Poll()
	while Result.Status == "Pending" do
		task.wait()
		Result = Op:Poll()
	end
	return Result
end
)";

	void CheckLuauContracts() {
		auto World = std::make_shared<DataModel>();
		ScriptEngine Engine(World);
		Engine.ConfigurePersistence(MakeMemoryPersistenceProvider());
		RunScript(Engine, std::string(Wait) + R"(
local DS = game:GetService("DataStoreService")
local Bytes = DS:GetKeyValueStore("CheckedBuffers")
local Value = buffer.fromstring("a\0b")
local Put = Bytes:PutAsync("one", Value, "0")
buffer.writeu8(Value, 0, 0)
local Written = Wait(Put)
assert(Written.Status == "Ok")
assert(Wait(Bytes:PutAsync("one", Value, "0")).Status == "Conflict")
local ReadOp = Bytes:GetAsync("one")
local Read = Wait(ReadOp)
assert(Read.Record.Revision == Written.Revision and buffer.tostring(Read.Record.Value) == "a\0b")
assert(ReadOp:Poll().Status == "Consumed")
assert(Wait(Bytes:DeleteAsync("one", Read.Record.Revision)).Status == "Ok")
assert(Wait(Bytes:GetAsync("one")).Status == "Missing")

local Docs = DS:GetDocumentStore("CheckedDocuments")
local Null = DS:GetNull()
local Document = {Nested = {Null, false, 1.25}, EmptyArray = DS:CreateArray(), EmptyObject = {}}
local Write = Docs:PutAsync("player", Document, "0")
Document.Nested[3] = 99
assert(Wait(Write).Status == "Ok")
local Stored = Wait(Docs:GetAsync("player")).Record.Value
assert(Stored.Nested[1] == Null and Stored.Nested[2] == false and Stored.Nested[3] == 1.25)
assert(getmetatable(Stored.EmptyArray) == "DataStoreArray" and getmetatable(Stored.EmptyObject) == nil)
assert(Docs:QueryAsync({}).Status == nil and Docs:QueryAsync({}):Poll().Status == "Unsupported")
local Cycle = {}
Cycle.Self = Cycle
assert(not pcall(function() Docs:PutAsync("cycle", Cycle) end))
assert(not pcall(function() Docs:PutAsync("instance", {Game = game}) end))
assert(not pcall(function() Docs:PutAsync("sparse", {Array = {[3] = true}}) end))
assert(not pcall(function() Docs:PutAsync("over", {Text = string.rep("a", 65537)}) end))
local Nested = {}
for Depth = 1, 13 do Nested = { Child = Nested } end
assert(not pcall(function() Docs:PutAsync("deep", Nested) end))
assert(not pcall(function() Bytes:DeleteAsync("badrev", "001") end))
)");
		{
			ScriptSecurityScope Scope(ScriptSecurityContext::ClientRuntime());
			bool Denied = false;
			try { (void)World->GetService("DataStoreService"); } catch (const std::runtime_error&) { Denied = true; }
			Require(Denied, "client can resolve server-only service");
		}
		ScriptEngine Offline(std::make_shared<DataModel>());
		Offline.ConfigurePersistence({});
		RunScript(Offline, R"(
local Store = game:GetService("DataStoreService"):GetDocumentStore("Unavailable")
assert(not Store:GetCapabilities().Documents)
assert(Store:GetAsync("key"):Poll().Status == "Unavailable")
)");
	}

	std::string ReadSample() {
		const auto Source = std::filesystem::path(GARGANTUAN_SOURCE_DIR) /
			"samples/Foundation11Persistence/Profile.luau";
		std::ifstream Input(Source, std::ios::binary);
		Require(Input.good(), "sample source is missing");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	void CheckSample(const std::shared_ptr<IPersistenceProvider>& Provider, std::string Key,
		bool LostAcknowledgement = false, bool Concurrent = false) {
		const auto Source = "local Profiles = (function()\n" + ReadSample() + "\nend)()\n";
		const auto PlayerKey = "local ProfileKey = \"" + Key + "\"\n";
		const std::string Expected = Concurrent ? "(Status == 'Granted' or Status == 'AlreadyGranted')" :
			(LostAcknowledgement ? "Status == 'Reconciled'" : "Status == 'Granted'");
		for (int Attempt = 0; Attempt < 2; ++Attempt) {
			ScriptEngine Engine(std::make_shared<DataModel>());
			Engine.ConfigurePersistence(Provider);
			RunScript(Engine, Source + PlayerKey + (Attempt == 0 ?
				"local Status, Profile = Profiles.GrantStarterReward(ProfileKey)\n"
				"assert(" + Expected + " and Profile.Currency == 25 and Profile.Inventory[1] == 'ExplorerBadge', Status)" :
				"local Status, Profile = Profiles.GrantStarterReward(ProfileKey)\n"
				"assert(Status == 'AlreadyGranted' and Profile.Currency == 25 and #Profile.Inventory == 1, Status)"));
		}
		std::cout << "[Persistence:LuauSample] reward, reconnect and no duplicate reward passed\n";
	}

	void MeasureRuntime(std::string_view Label, std::shared_ptr<IPersistenceProvider> Provider, unsigned Repetitions) {
		using namespace runtime_detail;
		struct RunningPhase {
			std::vector<std::uint64_t> Samples;
			std::uint64_t Allocations = 0;
			std::uint64_t Bytes = 0;
			std::uint64_t Calls = 0;
		};
		constexpr std::array Phases{WorkPhase::PersistenceSubmit, WorkPhase::PersistencePoll, WorkPhase::PersistencePump};
		std::array<RunningPhase, Phases.size()> Captures;
		std::vector<std::uint64_t> Steps;
		const auto Quantile = [](const auto& Samples, std::size_t P) { return Samples[(Samples.size() * P + 99) / 100 - 1]; };
		ScriptEngine Engine(std::make_shared<DataModel>());
		Engine.ConfigurePersistence(std::move(Provider));
		auto ScriptValue = AddScript(Engine, std::string(Wait) + "local Count = " + std::to_string(Repetitions) + R"(
local DS = game:GetService("DataStoreService")
local Docs = DS:GetDocumentStore("Timing")
local Value = { Text = string.rep("f", 4000), Items = { 1, 2, 3 }, Enabled = true }
for Index = 1, Count do
	local Write = Wait(Docs:PutAsync("Key", Value))
	assert(Write.Status == "Ok", Write.Status)
	local Read = Wait(Docs:GetAsync("Key"))
	assert(Read.Status == "Ok" and Read.Record.Revision == Write.Revision, Read.Status)
end
)");
		WaitFor([&] {
			WorkSample Sample;
			BenchmarkAllocationProbe::Counts Allocations;
			const auto Started = PersistenceClock::now();
			{
				BenchmarkAllocationProbe::Scope Probe(Allocations);
				WorkCapture Capture(&Sample, &BenchmarkAllocationProbe::ReadCalls, &BenchmarkAllocationProbe::ReadBytes);
				Engine.Step();
			}
			Steps.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
				PersistenceClock::now() - Started).count());
			for (std::size_t Index = 0; Index < Phases.size(); ++Index) {
				const auto& Phase = Sample[static_cast<std::size_t>(Phases[Index])];
				if (!Phase.Calls) continue;
				auto& Stats = Captures[Index];
				Stats.Samples.push_back(Phase.Nanoseconds);
				Stats.Allocations += Phase.Allocations;
				Stats.Bytes += Phase.AllocatedBytes;
				Stats.Calls += Phase.Calls;
			}
			Require(ScriptValue->Status != ScriptStatus::Error, ScriptValue->ErrorMessage);
			return ScriptValue->Status == ScriptStatus::Finished;
		}, "timing workload did not finish", 20s);
		const auto Print = [&](std::string_view Phase, std::vector<std::uint64_t>& Samples) {
			Require(!Samples.empty(), "missing runtime work samples");
			std::ranges::sort(Samples);
			std::cout << "[Persistence:Timing] Provider=" << Label << " Work=" << Phase << " Samples=" << Samples.size()
				<< " P50Ns=" << Quantile(Samples, 50) << " P95Ns=" << Quantile(Samples, 95)
				<< " P99Ns=" << Quantile(Samples, 99);
		};
		Print("ScriptStep", Steps);
		std::cout << '\n';
		for (std::size_t Index = 0; Index < Phases.size(); ++Index) {
			auto& Stats = Captures[Index];
			Print(WorkPhaseNames[static_cast<std::size_t>(Phases[Index])], Stats.Samples);
			std::cout << " Calls=" << Stats.Calls << " CxxAllocations=" << Stats.Allocations
				<< " CxxBytes=" << Stats.Bytes << '\n';
		}
	}
}

int main(int ArgumentCount, char** Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--measure") {
			MeasureRuntime("memory", MakeMemoryPersistenceProvider(), 128);
			MeasureRuntime("delayed-20ms", std::make_shared<DelayedProvider>(), 32);
			return 0;
		}
#if defined(GARGANTUAN_WITH_NODE_PERSISTENCE)
		if (const auto* Endpoint = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_ENDPOINT")) {
			const auto* Certificate = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_CERTIFICATE");
			const auto* Token = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_A");
			const auto* Key = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_STORE");
			Require(Certificate && Token && Key, "Node sample requires isolated qualification configuration");
			std::shared_ptr<IPersistenceProvider> Provider = std::make_shared<gargantuan::host::NodePersistenceProvider>(
				gargantuan::host::NodePersistenceProviderConfiguration{Endpoint, Certificate, Token});
			const auto* Gate = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_LUAU_GATE");
			if (Gate) Provider = std::make_shared<SynchronizedProvider>(std::move(Provider), Gate);
			const bool LostAcknowledgement = std::getenv("GARGANTUAN_ENGINE_PERSISTENCE_LUAU_LOST_ACK") != nullptr;
			CheckSample(Provider, Key, LostAcknowledgement, Gate != nullptr);
			return 0;
		}
#endif
		CheckRuntimeBounds();
		CheckResultRetention();
		CheckLuauContracts();
		CheckSample(gargantuan::MakeMemoryPersistenceProvider(), "test-explorer");
		CheckSample(std::make_shared<LostAcknowledgementProvider>(), "test-lost-acknowledgement", true);
		std::cout << "[Persistence:RuntimeTests] passed\n";
		return 0;
	} catch (const std::exception& Exception) {
		std::cerr << "[Persistence:RuntimeTests] " << Exception.what() << '\n';
		return 1;
	}
}
