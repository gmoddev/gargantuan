#include "gargantuan/persistence/LocalPersistenceProvider.hpp"

#include <array>
#include <barrier>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Passed, std::string_view Label) {
		if (Passed) return;
		std::cerr << "[Persistence:LocalTests] " << Label << '\n';
		++Failures;
	}

	PersistenceRequest RequestFor(PersistenceOperation Operation, std::string Key,
		PersistenceStoreKind Kind = PersistenceStoreKind::KeyValue, std::string Store = "progress") {
		PersistenceRequest Request;
		Request.Operation = Operation;
		Request.Kind = Kind;
		Request.Store = std::move(Store);
		Request.Key = std::move(Key);
		Request.Deadline = PersistenceClock::now() + 5s;
		return Request;
	}

	PersistenceResult Expect(IPersistenceProvider& Provider, const PersistenceRequest& Request, PersistenceStatus Expected,
		std::string_view Label, std::stop_token Stop = {}) {
		auto Result = Provider.Execute(Request, Stop);
		if (Result.Status != Expected) {
			std::cerr << "[Persistence:LocalTests] " << Label << " expected " << GetPersistenceStatusName(Expected)
				<< ", received " << GetPersistenceStatusName(Result.Status) << '\n';
			++Failures;
		}
		Check(IsPersistenceResultValid(Request, Result), Label);
		return Result;
	}

	void CheckByteRecord(const PersistenceResult& Result, const PersistenceBytes& Expected, std::uint64_t Revision) {
		const auto* Bytes = Result.Record ? std::get_if<PersistenceBytes>(&Result.Record->Payload) : nullptr;
		Check(Bytes && *Bytes == Expected && Result.Record->Revision == Revision, "returned the requested bytes and revision");
	}

	PersistenceDocumentValue FixtureDocument() {
		return PersistenceDocumentValue{PersistenceDocumentValue::Object{
			{"nested", PersistenceDocumentValue{PersistenceDocumentValue::Array{
				PersistenceDocumentValue{},
				PersistenceDocumentValue{false},
				PersistenceDocumentValue{true},
				PersistenceDocumentValue{-0.0},
				PersistenceDocumentValue{std::numeric_limits<double>::min()},
				PersistenceDocumentValue{std::string("caf\xc3\xa9")},
				PersistenceDocumentValue{PersistenceDocumentValue::Array{}},
				PersistenceDocumentValue{PersistenceDocumentValue::Object{}},
			}}},
		}};
	}

	void CheckDocumentRecord(const PersistenceResult& Result, std::uint64_t Revision) {
		const auto* Value = Result.Record ? std::get_if<PersistenceDocumentValue>(&Result.Record->Payload) : nullptr;
		const auto* Object = Value ? std::get_if<PersistenceDocumentValue::Object>(&Value->Data) : nullptr;
		Check(Object && Object->size() == 1 && (*Object)[0].Name == "nested" && Result.Record->Revision == Revision,
			"returned the document object and revision");
		if (!Object || Object->size() != 1) return;
		const auto* Array = std::get_if<PersistenceDocumentValue::Array>(&(*Object)[0].Value.Data);
		Check(Array && Array->size() == 8, "preserved the nested array");
		if (!Array || Array->size() != 8) return;
		const auto& Items = *Array;
		const auto* False = std::get_if<bool>(&Items[1].Data);
		const auto* True = std::get_if<bool>(&Items[2].Data);
		const auto* NegativeZero = std::get_if<double>(&Items[3].Data);
		const auto* Minimum = std::get_if<double>(&Items[4].Data);
		const auto* Text = std::get_if<std::string>(&Items[5].Data);
		const auto* EmptyArray = std::get_if<PersistenceDocumentValue::Array>(&Items[6].Data);
		const auto* EmptyObject = std::get_if<PersistenceDocumentValue::Object>(&Items[7].Data);
		Check(std::holds_alternative<std::monostate>(Items[0].Data) && False && !*False && True && *True
			&& NegativeZero && std::bit_cast<std::uint64_t>(*NegativeZero) == std::bit_cast<std::uint64_t>(-0.0)
			&& Minimum && *Minimum == std::numeric_limits<double>::min() && Text && *Text == "caf\xc3\xa9"
			&& EmptyArray && EmptyArray->empty() && EmptyObject && EmptyObject->empty(),
			"preserved document tags, finite number bits, UTF-8, and empty container identities");
	}

	std::uint64_t CheckConformance(IPersistenceProvider& Provider) {
		const auto Capabilities = Provider.Capabilities();
		Check(Capabilities.KeyValue && Capabilities.Documents && !Capabilities.DocumentQueries,
			"declared local key/value and document CRUD capabilities");

		auto Get = RequestFor(PersistenceOperation::Get, "player");
		Expect(Provider, Get, PersistenceStatus::Missing, "get missing key");
		auto Put = RequestFor(PersistenceOperation::Put, "player");
		const PersistenceBytes Original{std::byte{0}, std::byte{255}, std::byte{17}, std::byte{0}};
		Put.Payload = Original;
		Put.ExpectedRevision = 0;
		const auto Created = Expect(Provider, Put, PersistenceStatus::Ok, "create only");
		CheckByteRecord(Expect(Provider, Get, PersistenceStatus::Ok, "get original"), Original, Created.Revision);
		Expect(Provider, Put, PersistenceStatus::Conflict, "duplicate create only");
		Expect(Provider, RequestFor(PersistenceOperation::Get, "player", PersistenceStoreKind::KeyValue, "other"),
			PersistenceStatus::Missing, "distinct store name");

		auto DocumentPut = RequestFor(PersistenceOperation::Put, "player", PersistenceStoreKind::Document);
		DocumentPut.Payload = FixtureDocument();
		const auto Document = Expect(Provider, DocumentPut, PersistenceStatus::Ok, "put document using same store and key");
		const auto DocumentGet = RequestFor(PersistenceOperation::Get, "player", PersistenceStoreKind::Document);
		CheckDocumentRecord(Expect(Provider, DocumentGet, PersistenceStatus::Ok, "get document with nested values"),
			Document.Revision);
		auto InvalidDocument = DocumentPut;
		InvalidDocument.Payload = PersistenceDocumentValue{PersistenceDocumentValue::Array{}};
		Expect(Provider, InvalidDocument, PersistenceStatus::Invalid, "reject a document whose root is an array");

		auto Query = RequestFor(PersistenceOperation::Query, "", PersistenceStoreKind::Document);
		Expect(Provider, Query, PersistenceStatus::Unsupported, "query capability remains explicit");
		Put.ExpectedRevision = Created.Revision;
		Put.Payload = PersistenceBytes{};
		const auto Empty = Expect(Provider, Put, PersistenceStatus::Ok, "compare-and-swap to empty buffer");
		Check(Empty.Revision > Created.Revision && Empty.Revision > Document.Revision, "mutation revisions advance");
		CheckByteRecord(Expect(Provider, Get, PersistenceStatus::Ok, "get empty buffer"), {}, Empty.Revision);
		Expect(Provider, Put, PersistenceStatus::Conflict, "reject stale compare-and-swap");

		auto Delete = RequestFor(PersistenceOperation::Delete, "player");
		Delete.ExpectedRevision = Created.Revision;
		Expect(Provider, Delete, PersistenceStatus::Conflict, "reject stale conditional deletion");
		Delete.ExpectedRevision = 0;
		Expect(Provider, Delete, PersistenceStatus::Invalid, "reject create-only condition on deletion");
		Delete.ExpectedRevision = Empty.Revision;
		const auto Removed = Expect(Provider, Delete, PersistenceStatus::Ok, "delete by expected revision");
		Check(Removed.Revision > Empty.Revision, "deletion advances revision");
		Expect(Provider, Delete, PersistenceStatus::Conflict, "missing with expected revision conflicts");
		Delete.ExpectedRevision.reset();
		Expect(Provider, Delete, PersistenceStatus::Missing, "unconditional missing delete reports missing");
		Expect(Provider, Get, PersistenceStatus::Missing, "deleted buffer is absent");
		CheckDocumentRecord(Expect(Provider, DocumentGet, PersistenceStatus::Ok, "deletion preserves document domain"),
			Document.Revision);

		Put.ExpectedRevision = 0;
		Put.Payload = PersistenceBytes(PersistenceMaxValueBytes + 1);
		Expect(Provider, Put, PersistenceStatus::Invalid, "reject oversize buffer");
		Put.Payload = Original;
		std::stop_source Cancel;
		Cancel.request_stop();
		Expect(Provider, Put, PersistenceStatus::Cancelled, "pre-cancelled write", Cancel.get_token());
		Put.Deadline = PersistenceClock::now() - 1ms;
		Expect(Provider, Put, PersistenceStatus::Deadline, "expired write");
		Expect(Provider, Get, PersistenceStatus::Missing, "expired and cancelled writes leave record absent");
		return Removed.Revision;
	}

	void CheckLimits(IPersistenceProvider& Provider) {
		auto Put = RequestFor(PersistenceOperation::Put, "a");
		Put.Payload = PersistenceBytes(4, std::byte{1});
		const auto First = Expect(Provider, Put, PersistenceStatus::Ok, "bounded payload admitted");
		Put.Key = "b";
		Put.Payload = PersistenceBytes(1, std::byte{2});
		Expect(Provider, Put, PersistenceStatus::Busy, "aggregate payload quota");
		Put.Payload = PersistenceBytes{};
		const auto Second = Expect(Provider, Put, PersistenceStatus::Ok, "zero-byte record fits payload quota");
		Put.Key = "c";
		Expect(Provider, Put, PersistenceStatus::Busy, "empty payload cannot bypass the record quota");
		auto Delete = RequestFor(PersistenceOperation::Delete, "a");
		Delete.ExpectedRevision = First.Revision;
		const auto Removed = Expect(Provider, Delete, PersistenceStatus::Ok, "delete reclaims capacity");
		Put.Payload = PersistenceBytes(4, std::byte{3});
		const auto Third = Expect(Provider, Put, PersistenceStatus::Ok, "admit write after reclaim");
		Check(Second.Revision == First.Revision + 1 && Removed.Revision == Second.Revision + 1
			&& Third.Revision == Removed.Revision + 1, "rejected writes do not allocate revisions");
	}

	void CheckConcurrentCas(IPersistenceProvider& First, IPersistenceProvider& Second) {
		std::barrier Go(2);
		std::array<PersistenceResult, 2> Results;
		const auto Race = [&](std::size_t Index, IPersistenceProvider& Provider) {
			auto Put = RequestFor(PersistenceOperation::Put, "race");
			Put.ExpectedRevision = 0;
			Put.Payload = PersistenceBytes{static_cast<std::byte>(Index)};
			Go.arrive_and_wait();
			Results[Index] = Provider.Execute(Put, {});
		};
		{
			std::jthread Left([&] { Race(0, First); });
			std::jthread Right([&] { Race(1, Second); });
		}
		const std::size_t Winner = Results[0].Status == PersistenceStatus::Ok ? 0 : 1;
		Check(Results[Winner].Status == PersistenceStatus::Ok && Results[1 - Winner].Status == PersistenceStatus::Conflict,
			"competing create operations commit exactly once");
		const auto Get = RequestFor(PersistenceOperation::Get, "race");
		const auto Observed = Expect(Second, Get, PersistenceStatus::Ok, "independent caller observes winning commit");
		CheckByteRecord(Observed, {static_cast<std::byte>(Winner)}, Results[Winner].Revision);
	}

	class TemporaryDirectory final {
	public:
		TemporaryDirectory() {
			const auto Parent = std::filesystem::temp_directory_path();
			std::random_device Random;
			for (int Attempt = 0; Attempt < 8; ++Attempt) {
				auto Candidate = Parent / ("gargantuan-local-persistence-" + std::to_string(Random()));
				if (std::filesystem::create_directory(Candidate)) {
					Path = std::move(Candidate);
					break;
				}
			}
		}
		~TemporaryDirectory() {
			if (Path.empty()) return;
			std::error_code Ignored;
			std::filesystem::remove_all(Path, Ignored);
		}
		TemporaryDirectory(const TemporaryDirectory&) = delete;
		TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
		std::filesystem::path Path;
	};

	void CheckDurable() {
		const TemporaryDirectory Directory;
		Check(!Directory.Path.empty(), "created an isolated temp directory");
		if (Directory.Path.empty()) return;
		const auto Path = Directory.Path / "development.sqlite";
		auto Configuration = DurableLocalPersistenceConfiguration{Path, "project-alpha"};
		Configuration.CreateIfMissing = false;
		auto Inspector = MakeDurableLocalPersistenceProvider(Configuration);
		Check(static_cast<bool>(Inspector), "accept native inspection configuration");
		if (!Inspector) return;
		Expect(*Inspector, RequestFor(PersistenceOperation::Get, "player"), PersistenceStatus::Unavailable,
			"inspection of a missing profile");
		Check(!std::filesystem::exists(Path), "inspection did not create a missing database");
		Inspector.reset();
		Configuration.CreateIfMissing = true;

		std::uint64_t BeforeRestart = 0;
		{
			auto Durable = MakeDurableLocalPersistenceProvider(Configuration);
			Check(static_cast<bool>(Durable), "create durable adapter");
			if (!Durable) return;
			BeforeRestart = CheckConformance(*Durable);
			auto Second = MakeDurableLocalPersistenceProvider(Configuration);
			CheckConcurrentCas(*Durable, *Second);
			const auto Get = RequestFor(PersistenceOperation::Get, "race");
			const auto Winner = Expect(*Durable, Get, PersistenceStatus::Ok, "first handle sees competing writes");
			if (Winner.Record) BeforeRestart = Winner.Record->Revision;
		}

		Configuration.CreateIfMissing = false;
		auto Reopened = MakeDurableLocalPersistenceProvider(Configuration);
		const auto Get = RequestFor(PersistenceOperation::Get, "player", PersistenceStoreKind::Document);
		CheckDocumentRecord(Expect(*Reopened, Get, PersistenceStatus::Ok, "document survived provider restart"), 2);
		const auto Later = Expect(*Reopened, RequestFor(PersistenceOperation::Put, "new"), PersistenceStatus::Ok,
			"write after restart");
		Check(Later.Revision > BeforeRestart, "revision is durable across reopen, including deleted records");

		auto Foreign = Configuration;
		Foreign.ProfileId = "project-beta";
		Expect(*MakeDurableLocalPersistenceProvider(Foreign), Get, PersistenceStatus::Unavailable,
			"existing file rejects a foreign project identity");
		Foreign = Configuration;
		Foreign.Limits.MaximumRecords = 2;
		Expect(*MakeDurableLocalPersistenceProvider(Foreign), Get, PersistenceStatus::Unavailable,
			"shared file rejects incompatible native bounds");
		CheckDocumentRecord(Expect(*Reopened, Get, PersistenceStatus::Ok, "failed foreign open preserves original"), 2);

		const auto Bounded = DurableLocalPersistenceConfiguration{Directory.Path / "bounded.sqlite", "bounded", {2, 4}};
		CheckLimits(*MakeDurableLocalPersistenceProvider(Bounded));

		const auto CorruptPath = Directory.Path / "corrupt.sqlite";
		{
			std::ofstream File(CorruptPath, std::ios::binary);
			File << "damaged database";
		}
		const auto Corrupt = DurableLocalPersistenceConfiguration{CorruptPath, "project-alpha"};
		Expect(*MakeDurableLocalPersistenceProvider(Corrupt), Get, PersistenceStatus::Unavailable,
			"reject existing corrupt storage");
		Check(std::filesystem::file_size(CorruptPath) == 16, "corrupt file was preserved");

		const auto EmptyPath = Directory.Path / "empty.sqlite";
		std::ofstream(EmptyPath, std::ios::binary).close();
		const auto Empty = DurableLocalPersistenceConfiguration{EmptyPath, "project-alpha"};
		Expect(*MakeDurableLocalPersistenceProvider(Empty), Get, PersistenceStatus::Unavailable,
			"reject an existing truncated storage file");
		Check(std::filesystem::file_size(EmptyPath) == 0, "truncated file was preserved");

		const auto Initial = DurableLocalPersistenceConfiguration{Directory.Path / "initial.sqlite", "initial"};
		auto Incomplete = Initial.Path;
		Incomplete += ".initializing";
		std::ofstream(Incomplete, std::ios::binary).close();
		auto FirstInitializer = MakeDurableLocalPersistenceProvider(Initial);
		auto SecondInitializer = MakeDurableLocalPersistenceProvider(Initial);
		CheckConcurrentCas(*FirstInitializer, *SecondInitializer);
		Check(std::filesystem::exists(Initial.Path), "interrupted empty initialization can be resumed on next use");
	}
}

int main() {
	auto Memory = MakeMemoryPersistenceProvider();
	auto Isolated = MakeMemoryPersistenceProvider();
	CheckConformance(*Memory);
	CheckConcurrentCas(*Memory, *Memory);
	Expect(*Isolated, RequestFor(PersistenceOperation::Get, "player", PersistenceStoreKind::Document),
		PersistenceStatus::Missing, "separate memory sessions never share data");
	CheckLimits(*MakeMemoryPersistenceProvider({2, 4}));
	Check(!MakeMemoryPersistenceProvider({0, 4}), "reject invalid memory limits");
	Check(!MakeDurableLocalPersistenceProvider({"relative.sqlite", "project"}), "reject a relative durable path");
	if (SupportsDurableLocalPersistence()) CheckDurable();
	return Failures == 0 ? 0 : 1;
}
