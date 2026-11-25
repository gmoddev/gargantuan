#include "host/server/NodePersistenceProvider.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::host;
	using namespace std::chrono_literals;
	using Status = PersistenceStatus;
	using Operation = PersistenceOperation;
	using Kind = PersistenceStoreKind;

	std::string GetEnvironment(const char *Name) {
		if (const char *Value = std::getenv(Name)) return Value;
		return {};
	}

	void Require(bool Condition, std::string_view Stage) {
		if (!Condition) throw std::runtime_error(std::string(Stage));
	}

	NodePersistenceProviderConfiguration GetConfiguration(const char *Token) {
		return {GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_ENDPOINT"),
			GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_CERTIFICATE"), GetEnvironment(Token)};
	}

	PersistenceResult Execute(NodePersistenceProvider &Provider, PersistenceRequest Request, Status Expected,
		std::string_view Stage, std::chrono::milliseconds Budget = 2s, std::stop_token Stop = {}) {
		Request.Deadline = PersistenceClock::now() + Budget;
		auto Result = Provider.Execute(Request, Stop);
		if (Result.Status != Expected) throw std::runtime_error(std::string(Stage) + " expected " +
			GetPersistenceStatusName(Expected) + " received " + GetPersistenceStatusName(Result.Status));
		Require(IsPersistenceResultValid(Request, Result), "invalid provider-neutral result shape");
		return Result;
	}

	PersistenceRequest MakeRequest(Kind StoreKind, Operation Op, std::string Key) {
		static std::uint32_t Sequence = 0;
		static const auto Store = GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_STORE");
		return {.Kind = StoreKind, .Operation = Op, .RequestId = "f11-engine-" + std::to_string(++Sequence),
			.Store = Store, .Key = std::move(Key)};
	}

	void WaitFor(std::string_view Signal) {
		std::string Line;
		Require(static_cast<bool>(std::getline(std::cin, Line)) && Line == Signal, "qualification barrier failed");
	}

	void CheckInvalidConfiguration() {
		for (const auto &Configuration : {NodePersistenceProviderConfiguration{},
			NodePersistenceProviderConfiguration{"127.0.0.1:443", "missing-certificate.pem", "missing-token.txt"}}) {
			bool Rejected = false;
			try { NodePersistenceProvider Provider(Configuration); }
			catch (const std::invalid_argument &) { Rejected = true; }
			Require(Rejected, "invalid trusted adapter configuration accepted");
		}
	}

	PersistenceDocumentValue MakeDocument(double Score) {
		using Value = PersistenceDocumentValue;
		return Value{Value::Object{{"stats", Value{Value::Object{{"score", Value{Score}}}}},
			{"inventory", Value{Value::Array{Value{}, Value{true}, Value{std::string("caf\xc3\xa9")}}}}}};
	}

	void CheckDocument(const PersistenceResult &Result) {
		const auto &Document = std::get<PersistenceDocumentValue>(Result.Record->Payload);
		const auto &Fields = std::get<PersistenceDocumentValue::Object>(Document.Data);
		Require(Fields.size() == 2 && Fields[0].Name == "inventory" && Fields[1].Name == "stats", "document object fields");
		const auto &Inventory = std::get<PersistenceDocumentValue::Array>(Fields[0].Value.Data);
		Require(Inventory.size() == 3 && std::holds_alternative<std::monostate>(Inventory[0].Data) &&
			std::get<bool>(Inventory[1].Data) && std::get<std::string>(Inventory[2].Data) == "caf\xc3\xa9",
			"structured value roundtrip");
	}

	void CheckFaultResponses() {
		NodePersistenceProvider Provider(GetConfiguration("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_A"));
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Get, "correlation"), Status::Unavailable, "wrong request id on read");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "correlation"), Status::UnknownOutcome, "wrong request id on write");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "connection"), Status::UnknownOutcome, "lost write acknowledgement");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "unavailable"), Status::Unavailable, "confirmed backend unavailability");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "uncertain"), Status::UnknownOutcome, "confirmed unknown mutation outcome");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "zero-revision"), Status::UnknownOutcome, "missing write revision");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Get, "oversize"), Status::Unavailable, "oversize stored value");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Get, "oversize-frame"), Status::Busy, "wire receive limit");
		Execute(Provider, MakeRequest(Kind::Document, Operation::Get, "wrong-key"), Status::Unavailable, "wrong document key");
		Execute(Provider, MakeRequest(Kind::Document, Operation::Get, "nonfinite"), Status::Unavailable, "invalid document number");
		auto Query = MakeRequest(Kind::Document, Operation::Query, "");
		Query.Query.Limit = 2;
		Execute(Provider, Query, Status::Unavailable, "duplicate query results");
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Get, "block"), Status::Deadline, "in-flight read deadline", 120ms);
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "block"), Status::UnknownOutcome, "in-flight write deadline", 120ms);
		std::stop_source Stop;
		std::jthread Canceller([&Stop] { std::this_thread::sleep_for(200ms); Stop.request_stop(); });
		Execute(Provider, MakeRequest(Kind::KeyValue, Operation::Put, "block"), Status::UnknownOutcome,
			"in-flight write cancellation", 2s, Stop.get_token());
		std::cout << "PERSISTENCE_WIRE_OK" << std::endl;
	}

	void CheckNodePostgres() {
		NodePersistenceProvider A(GetConfiguration("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_A"));
		NodePersistenceProvider B(GetConfiguration("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_B"));
		NodePersistenceProvider Denied(GetConfiguration("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_LIMITED"));
		const auto Capabilities = A.Capabilities();
		Require(Capabilities.KeyValue && Capabilities.Documents && Capabilities.DocumentQueries, "Node provider capabilities");
		auto Key = MakeRequest(Kind::KeyValue, Operation::Get, "player");
		Execute(A, Key, Status::Missing, "initial key absence");
		const PersistenceBytes Binary{std::byte{0}, std::byte{0xf1}, std::byte{0}, std::byte{0x35}};
		auto Put = MakeRequest(Kind::KeyValue, Operation::Put, Key.Key);
		Put.Payload = Binary;
		Put.ExpectedRevision = 1;
		Execute(A, Put, Status::Conflict, "conditional key put on an absent record");
		auto Delete = MakeRequest(Kind::KeyValue, Operation::Delete, Key.Key);
		Execute(A, Delete, Status::Missing, "unconditional deletion of an absent key");
		Delete.ExpectedRevision = 1;
		Execute(A, Delete, Status::Conflict, "conditional deletion of an absent key");
		Put.ExpectedRevision = 0;
		const auto Created = Execute(A, Put, Status::Ok, "key create").Revision;
		Execute(A, Put, Status::Conflict, "key create-only conflict");
		const auto Read = Execute(A, Key, Status::Ok, "key read");
		Require(std::get<PersistenceBytes>(Read.Record->Payload) == Binary && Read.Record->Revision == Created, "binary key roundtrip");
		Execute(B, Key, Status::Missing, "tenant B cannot see tenant A key");
		const auto OtherRevision = Execute(B, Put, Status::Ok, "tenant B same key create").Revision;
		Require(OtherRevision != Created, "distinct key revisions across tenants");
		Execute(Denied, Put, Status::Denied, "workload capability denial");
		std::stop_source Stop;
		Stop.request_stop();
		Execute(A, Put, Status::Cancelled, "cancel before calling Node", 2s, Stop.get_token());
		Execute(A, Put, Status::Deadline, "deadline before calling Node", -1ms);
		Put.ExpectedRevision = Created;
		const auto Updated = Execute(A, Put, Status::Ok, "conditional key update").Revision;
		Require(Updated > Created, "conditional revision did not advance");
		Execute(A, Put, Status::Conflict, "stale key revision");

		for (int Score = 1; Score <= 3; ++Score) {
			auto Document = MakeRequest(Kind::Document, Operation::Put, "player-" + std::to_string(Score));
			Document.Payload = MakeDocument(Score);
			Document.ExpectedRevision = 0;
			Execute(A, Document, Status::Ok, "document create");
		}
		auto Document = MakeRequest(Kind::Document, Operation::Get, "player-2");
		const auto Stored = Execute(A, Document, Status::Ok, "document get");
		CheckDocument(Stored);
		Execute(B, Document, Status::Missing, "tenant B cannot see tenant A document");
		auto Query = MakeRequest(Kind::Document, Operation::Query, "");
		Query.Query.Limit = 1;
		Query.Query.Filters.push_back({{"stats", "score"}, PersistenceComparison::GreaterOrEqual, {2.0}});
		Query.Query.Filters.push_back({{"inventory"}, PersistenceComparison::Exists, {}});
		Query.Query.Order.push_back({{"stats", "score"}, true});
		const auto First = Execute(A, Query, Status::Ok, "filtered first page");
		Require(First.Records.size() == 1 && First.Records[0].Key == "player-3" && !First.Cursor.empty(), "query first page");
		Query.Query.Cursor = First.Cursor;
		const auto Second = Execute(A, Query, Status::Ok, "filtered second page");
		Require(Second.Records.size() == 1 && Second.Records[0].Key == "player-2" && Second.Cursor.empty(), "query second page");
		Execute(B, Query, Status::Invalid, "cursor cannot cross tenants");
		Document.Operation = Operation::Delete;
		Document.ExpectedRevision = Stored.Record->Revision;
		Execute(A, Document, Status::Ok, "conditional document delete");
		Execute(A, Query, Status::StaleCursor, "cursor invalidated by collection mutation");
		Document.Operation = Operation::Get;
		Document.ExpectedRevision.reset();
		Execute(A, Document, Status::Missing, "document deleted");
		Document.Operation = Operation::Delete;
		Execute(A, Document, Status::Missing, "unconditional deletion of an absent document");
		Document.ExpectedRevision = Stored.Record->Revision;
		Execute(A, Document, Status::Conflict, "conditional deletion of an absent document");

		auto BadCertificate = GetConfiguration("GARGANTUAN_ENGINE_PERSISTENCE_TOKEN_A");
		BadCertificate.RootCertificateFile = GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_WRONG_CERTIFICATE");
		NodePersistenceProvider MismatchedRoot(std::move(BadCertificate));
		Key.Deadline = PersistenceClock::now() + 400ms;
		const auto Mismatch = MismatchedRoot.Execute(Key, {}).Status;
		Require(Mismatch == Status::Deadline || Mismatch == Status::Unavailable, "TLS root mismatch");
		std::cout << "PERSISTENCE_READY_FOR_RESTART" << std::endl;
		WaitFor("down");
		Key.Deadline = PersistenceClock::now() + 400ms;
		const auto OfflineRead = A.Execute(Key, {}).Status;
		Require(OfflineRead == Status::Deadline || OfflineRead == Status::Unavailable, "Node outage read");
		Execute(A, Put, Status::UnknownOutcome, "Node outage write");
		std::cout << "PERSISTENCE_DOWN_CONFIRMED" << std::endl;
		WaitFor("up");
		const auto End = PersistenceClock::now() + 8s;
		for (;;) {
			Key.Deadline = PersistenceClock::now() + 400ms;
			const auto Reply = A.Execute(Key, {});
			if (Reply.Status == Status::Denied) break;
			if ((Reply.Status != Status::Unavailable && Reply.Status != Status::Deadline) || PersistenceClock::now() >= End)
				throw std::runtime_error(std::string("old credential after Node restart returned ") +
					GetPersistenceStatusName(Reply.Status));
			std::this_thread::sleep_for(100ms);
		}
		std::cout << "PERSISTENCE_CREDENTIAL_REJECTED" << std::endl;
		WaitFor("rotated");
		const auto Recovered = Execute(A, Key, Status::Ok, "recovered channel and workload credential");
		Require(std::get<PersistenceBytes>(Recovered.Record->Payload) == Binary && Recovered.Record->Revision == Updated,
			"PostgreSQL recovery preserved committed key and revision");
		if (GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_RESTART_DB") == "1") {
			std::cout << "PERSISTENCE_READY_FOR_DB_RESTART" << std::endl;
			WaitFor("db-down");
			Key.Deadline = PersistenceClock::now() + 1s;
			const auto DatabaseDown = A.Execute(Key, {}).Status;
			Require(DatabaseDown == Status::Unavailable || DatabaseDown == Status::Deadline,
				"database outage must not be reported as missing data");
			std::cout << "PERSISTENCE_DB_DOWN_CONFIRMED" << std::endl;
			WaitFor("db-up");
			const auto Restarted = Execute(A, Key, Status::Ok, "recovered after PostgreSQL process restart");
			Require(Restarted.Record->Revision == Updated && std::get<PersistenceBytes>(Restarted.Record->Payload) == Binary,
				"PostgreSQL process restart changed committed data");
		}
		Delete.ExpectedRevision = Updated;
		Execute(A, Delete, Status::Ok, "conditional key delete");
		Execute(A, Key, Status::Missing, "deleted tenant A key");
		Require(Execute(B, Key, Status::Ok, "tenant B key unaffected").Record->Revision == OtherRevision, "tenant B key changed");
		Delete.ExpectedRevision = OtherRevision;
		Execute(B, Delete, Status::Ok, "clean tenant B fixture key");
		Execute(A, MakeRequest(Kind::Document, Operation::Delete, "player-1"), Status::Ok, "clean first fixture document");
		Execute(A, MakeRequest(Kind::Document, Operation::Delete, "player-3"), Status::Ok, "clean final fixture document");
		std::cout << "PERSISTENCE_POSTGRES_OK" << std::endl;
	}
} // namespace

int main() {
	try {
		CheckInvalidConfiguration();
		const auto Mode = GetEnvironment("GARGANTUAN_ENGINE_PERSISTENCE_TEST_MODE");
		if (Mode == "wire") CheckFaultResponses();
		if (Mode == "postgres") CheckNodePostgres();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Persistence:NodeQualification] " << Error.what() << '\n';
		return 1;
	}
}
