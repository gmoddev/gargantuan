#include "gargantuan/persistence/LocalPersistenceProvider.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

#if defined(GARGANTUAN_WITH_LOCAL_SQLITE) && GARGANTUAN_WITH_LOCAL_SQLITE
#include <sqlite3.h>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#endif

namespace gargantuan {
	namespace {
		using namespace std::chrono_literals;

		constexpr auto LocalCapabilities = PersistenceCapabilities{true, true, false};
		constexpr std::uint64_t MaximumRevision = std::numeric_limits<std::int64_t>::max();
		// A binary64 needs one extra tag byte beyond the shared per-node charge.
		constexpr std::size_t MaximumEncodedDocument = PersistenceMaxValueBytes + PersistenceMaxDocumentNodes;

		PersistenceResult Status(PersistenceStatus Value) {
			PersistenceResult Result;
			Result.Status = Value;
			return Result;
		}

		bool ValidLimits(const LocalPersistenceLimits& Limits) {
			return Limits.MaximumRecords > 0 && Limits.MaximumRecords <= LocalPersistenceLimits::RecordCeiling
				&& Limits.MaximumPayloadBytes > 0
				&& Limits.MaximumPayloadBytes <= LocalPersistenceLimits::PayloadByteCeiling;
		}

		struct Execution final {
			std::stop_token Stop;
			PersistenceClock::time_point Deadline;

			std::optional<PersistenceStatus> Interruption() const {
				if (Stop.stop_requested()) return PersistenceStatus::Cancelled;
				if (PersistenceClock::now() >= Deadline) return PersistenceStatus::Deadline;
				return std::nullopt;
			}

			std::unique_lock<std::timed_mutex> Lock(std::timed_mutex& Mutex) const {
				std::unique_lock Guard(Mutex, std::defer_lock);
				while (!Interruption()) {
					if (Guard.try_lock_until(std::min(Deadline, PersistenceClock::now() + 5ms))) break;
				}
				return Guard;
			}
		};

		enum class DocumentTag : std::uint8_t {
			Null = 0, False = 1, True = 2, Number = 3, Text = 4, Array = 5, Object = 6,
		};

		template <typename Unsigned> void AppendUnsigned(PersistenceBytes& Output, Unsigned Value) {
			for (std::size_t Index = 0; Index < sizeof(Value); ++Index) {
				Output.push_back(static_cast<std::byte>(Value & 0xff));
				Value >>= 8;
			}
		}

		void AppendText(PersistenceBytes& Output, std::string_view Value) {
			AppendUnsigned(Output, static_cast<std::uint32_t>(Value.size()));
			const auto Bytes = std::as_bytes(std::span(Value.data(), Value.size()));
			Output.insert(Output.end(), Bytes.begin(), Bytes.end());
		}

		// The format is private to version 1 of the local database. Tags and
		// little-endian lengths keep empty containers and numbers unambiguous.
		void EncodeDocument(PersistenceBytes& Output, const PersistenceDocumentValue& Value) {
			std::visit([&](const auto& Data) {
				using Type = std::decay_t<decltype(Data)>;
				if constexpr (std::is_same_v<Type, std::monostate>) {
					Output.push_back(static_cast<std::byte>(DocumentTag::Null));
				} else if constexpr (std::is_same_v<Type, bool>) {
					Output.push_back(static_cast<std::byte>(Data ? DocumentTag::True : DocumentTag::False));
				} else if constexpr (std::is_same_v<Type, double>) {
					Output.push_back(static_cast<std::byte>(DocumentTag::Number));
					AppendUnsigned(Output, std::bit_cast<std::uint64_t>(Data));
				} else if constexpr (std::is_same_v<Type, std::string>) {
					Output.push_back(static_cast<std::byte>(DocumentTag::Text));
					AppendText(Output, Data);
				} else if constexpr (std::is_same_v<Type, PersistenceDocumentValue::Array>) {
					Output.push_back(static_cast<std::byte>(DocumentTag::Array));
					AppendUnsigned(Output, static_cast<std::uint32_t>(Data.size()));
					for (const auto& Item : Data) EncodeDocument(Output, Item);
				} else {
					static_assert(std::is_same_v<Type, PersistenceDocumentValue::Object>);
					Output.push_back(static_cast<std::byte>(DocumentTag::Object));
					AppendUnsigned(Output, static_cast<std::uint32_t>(Data.size()));
					for (const auto& Field : Data) {
						AppendText(Output, Field.Name);
						EncodeDocument(Output, Field.Value);
					}
				}
			}, Value.Data);
		}

		class DocumentDecoder final {
		public:
			explicit DocumentDecoder(std::span<const std::byte> Bytes) : Remaining(Bytes) {}

			std::optional<PersistenceDocumentValue> Decode() {
				if (Remaining.size() > MaximumEncodedDocument) return std::nullopt;
				auto Value = ReadValue(1);
				std::size_t DocumentBytes = 0;
				if (!Value || !Remaining.empty() || !std::holds_alternative<PersistenceDocumentValue::Object>(Value->Data)
					|| !MeasurePersistenceDocument(*Value, DocumentBytes))
					return std::nullopt;
				return Value;
			}

		private:
			template <typename Unsigned> std::optional<Unsigned> ReadUnsigned() {
				if (Remaining.size() < sizeof(Unsigned)) return std::nullopt;
				Unsigned Value = 0;
				for (std::size_t Index = 0; Index < sizeof(Unsigned); ++Index) {
					Value |= static_cast<Unsigned>(std::to_integer<unsigned>(Remaining[Index])) << (8 * Index);
				}
				Remaining = Remaining.subspan(sizeof(Unsigned));
				return Value;
			}

			std::optional<std::string> ReadText() {
				const auto Length = ReadUnsigned<std::uint32_t>();
				if (!Length || *Length > Remaining.size()) return std::nullopt;
				std::string Text(reinterpret_cast<const char*>(Remaining.data()), *Length);
				Remaining = Remaining.subspan(*Length);
				return Text;
			}

			std::optional<std::uint32_t> ReadCount() {
				const auto Count = ReadUnsigned<std::uint32_t>();
				if (!Count || *Count > NodesRemaining || *Count > Remaining.size()) return std::nullopt;
				return Count;
			}

			std::optional<PersistenceDocumentValue> ReadValue(std::size_t Depth) {
				if (Remaining.empty() || NodesRemaining == 0 || Depth > PersistenceMaxDocumentDepth) return std::nullopt;
				--NodesRemaining;
				const auto Tag = static_cast<DocumentTag>(Remaining.front());
				Remaining = Remaining.subspan(1);
				switch (Tag) {
				case DocumentTag::Null: return PersistenceDocumentValue{};
				case DocumentTag::False: return PersistenceDocumentValue{false};
				case DocumentTag::True: return PersistenceDocumentValue{true};
				case DocumentTag::Number: {
					const auto Bits = ReadUnsigned<std::uint64_t>();
					if (Bits) return PersistenceDocumentValue{std::bit_cast<double>(*Bits)};
					break;
				}
				case DocumentTag::Text: {
					auto Text = ReadText();
					if (Text) return PersistenceDocumentValue{std::move(*Text)};
					break;
				}
				case DocumentTag::Array: {
					const auto Count = ReadCount();
					if (!Count) break;
					PersistenceDocumentValue::Array Array;
					Array.reserve(*Count);
					for (std::uint32_t Index = 0; Index < *Count; ++Index) {
						auto Value = ReadValue(Depth + 1);
						if (!Value) return std::nullopt;
						Array.push_back(std::move(*Value));
					}
					return PersistenceDocumentValue{std::move(Array)};
				}
				case DocumentTag::Object: {
					const auto Count = ReadCount();
					if (!Count) break;
					PersistenceDocumentValue::Object Object;
					Object.reserve(*Count);
					for (std::uint32_t Index = 0; Index < *Count; ++Index) {
						auto Name = ReadText();
						if (!Name) return std::nullopt;
						auto Value = ReadValue(Depth + 1);
						if (!Value) return std::nullopt;
						Object.push_back({std::move(*Name), std::move(*Value)});
					}
					return PersistenceDocumentValue{std::move(Object)};
				}
				}
				return std::nullopt;
			}

			std::span<const std::byte> Remaining;
			std::size_t NodesRemaining = PersistenceMaxDocumentNodes;
		};

		std::optional<PersistenceStatus> ValidateRequest(const PersistenceRequest& Request, PersistenceBytes& Encoded) {
			if (Request.Kind != PersistenceStoreKind::KeyValue && Request.Kind != PersistenceStoreKind::Document)
				return PersistenceStatus::Invalid;
			if (!IsPersistenceRequestValid(Request)) return PersistenceStatus::Invalid;
			if (Request.Operation == PersistenceOperation::Query) return PersistenceStatus::Unsupported;
			if (Request.Operation != PersistenceOperation::Put) return std::nullopt;
			if (Request.Kind == PersistenceStoreKind::KeyValue) {
				Encoded = std::get<PersistenceBytes>(Request.Payload);
			} else {
				EncodeDocument(Encoded, std::get<PersistenceDocumentValue>(Request.Payload));
				if (Encoded.size() > MaximumEncodedDocument) return PersistenceStatus::Invalid;
			}
			return std::nullopt;
		}

		PersistenceResult ReadResult(const PersistenceRequest& Request, std::uint64_t Revision,
			std::span<const std::byte> Encoded) {
			PersistencePayload Payload;
			if (Request.Kind == PersistenceStoreKind::KeyValue) {
				if (Encoded.size() > PersistenceMaxValueBytes) return Status(PersistenceStatus::Unavailable);
				if (!Encoded.empty()) Payload = PersistenceBytes(Encoded.begin(), Encoded.end());
			} else {
				auto Document = DocumentDecoder(Encoded).Decode();
				if (!Document) return Status(PersistenceStatus::Unavailable);
				Payload = std::move(*Document);
			}
			auto Result = Status(PersistenceStatus::Ok);
			Result.Record.emplace(PersistenceRecord{Request.Key, std::move(Payload), Revision});
			return Result;
		}

		struct StoreUsage final {
			std::uint64_t Revision = 0;
			std::size_t Records = 0;
			std::size_t PayloadBytes = 0;
		};

		struct RecordMetadata final {
			std::uint64_t Revision;
			std::size_t PayloadBytes;
		};

		std::optional<PersistenceStatus> PrepareMutation(const PersistenceRequest& Request, const LocalPersistenceLimits& Limits,
			const std::optional<RecordMetadata>& Existing, std::size_t PayloadBytes, StoreUsage& Usage) {
			const bool IsDelete = Request.Operation == PersistenceOperation::Delete;
			if (Request.ExpectedRevision && *Request.ExpectedRevision != (Existing ? Existing->Revision : 0))
				return PersistenceStatus::Conflict;
			if (IsDelete && !Existing) return PersistenceStatus::Missing;
			if (Usage.Revision >= MaximumRevision) return PersistenceStatus::Unavailable;
			if (Existing) {
				if (Usage.Records == 0 || Existing->PayloadBytes > Usage.PayloadBytes) return PersistenceStatus::Unavailable;
				Usage.PayloadBytes -= Existing->PayloadBytes;
			}
			if (IsDelete) {
				--Usage.Records;
			} else {
				if (!Existing) ++Usage.Records;
				Usage.PayloadBytes += PayloadBytes;
				if (Usage.Records > Limits.MaximumRecords || Usage.PayloadBytes > Limits.MaximumPayloadBytes)
					return PersistenceStatus::Busy;
			}
			++Usage.Revision;
			return std::nullopt;
		}

		PersistenceResult MutationResult(std::uint64_t Revision) {
			auto Result = Status(PersistenceStatus::Ok);
			Result.Revision = Revision;
			return Result;
		}

		class MemoryPersistenceProvider final : public IPersistenceProvider {
		public:
			explicit MemoryPersistenceProvider(LocalPersistenceLimits Limits) : Limits(Limits) {}

			PersistenceCapabilities Capabilities() const noexcept override { return LocalCapabilities; }

			PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
				const Execution Context{Stop, Request.Deadline};
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
				PersistenceBytes Encoded;
				if (const auto Invalid = ValidateRequest(Request, Encoded)) return Status(*Invalid);
				auto Guard = Context.Lock(Mutex);
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);

				const auto Address = std::tuple(Request.Kind, Request.Store, Request.Key);
				const auto Found = Records.find(Address);
				if (Request.Operation == PersistenceOperation::Get) {
					if (Found == Records.end()) return Status(PersistenceStatus::Missing);
					return ReadResult(Request, Found->second.Revision, Found->second.Payload);
				}

				std::optional<RecordMetadata> Existing;
				if (Found != Records.end()) Existing = RecordMetadata{Found->second.Revision, Found->second.Payload.size()};
				auto NewUsage = Usage;
				if (const auto Rejected = PrepareMutation(Request, Limits, Existing, Encoded.size(), NewUsage))
					return Status(*Rejected);
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
				if (Request.Operation == PersistenceOperation::Delete) Records.erase(Found);
				else Records.insert_or_assign(Address, MemoryRecord{std::move(Encoded), NewUsage.Revision});
				Usage = NewUsage;
				return MutationResult(NewUsage.Revision);
			}

		private:
			struct MemoryRecord {
				PersistenceBytes Payload;
				std::uint64_t Revision;
			};

			const LocalPersistenceLimits Limits;
			std::timed_mutex Mutex;
			std::map<std::tuple<PersistenceStoreKind, std::string, std::string>, MemoryRecord> Records;
			StoreUsage Usage;
		};

#if defined(GARGANTUAN_WITH_LOCAL_SQLITE) && GARGANTUAN_WITH_LOCAL_SQLITE
		constexpr int LocalApplicationId = 0x47445331; // GDS1
		constexpr char CreateReceipts[] =
			"CREATE TABLE receipts (slot INTEGER PRIMARY KEY CHECK (slot BETWEEN 0 AND 63), request TEXT NOT NULL) STRICT";
		constexpr int FilePageSize = 4096;
		constexpr int FilePageCeiling = 32'768;
		constexpr auto FileByteCeiling = static_cast<std::uintmax_t>(FilePageSize) * FilePageCeiling;
		constexpr char CreateProfile[] =
			"CREATE TABLE profile (id INTEGER PRIMARY KEY CHECK (id = 1), identity BLOB NOT NULL, "
			"revision INTEGER NOT NULL CHECK (revision >= 0), records INTEGER NOT NULL CHECK (records >= 0), "
			"payload_bytes INTEGER NOT NULL CHECK (payload_bytes >= 0), "
			"maximum_records INTEGER NOT NULL CHECK (maximum_records > 0), "
			"maximum_payload_bytes INTEGER NOT NULL CHECK (maximum_payload_bytes > 0)) STRICT";
		constexpr char CreateRecords[] =
			"CREATE TABLE records (kind INTEGER NOT NULL CHECK (kind IN (0, 1)), store TEXT NOT NULL, "
			"key TEXT NOT NULL, payload BLOB NOT NULL, revision INTEGER NOT NULL CHECK (revision > 0), "
			"PRIMARY KEY (kind, store, key)) STRICT, WITHOUT ROWID";

		int SqlExec(sqlite3* Database, const char* Sql) {
			return sqlite3_exec(Database, Sql, nullptr, nullptr, nullptr);
		}

		class SqlStatement final {
		public:
			SqlStatement(sqlite3* Database, const char* Sql)
				: Code(sqlite3_prepare_v2(Database, Sql, -1, &Statement, nullptr)) {}
			~SqlStatement() { sqlite3_finalize(Statement); }
			SqlStatement(const SqlStatement&) = delete;
			SqlStatement& operator=(const SqlStatement&) = delete;

			void BindInteger(int Index, std::uint64_t Value) {
				if (Code == SQLITE_OK) Code = sqlite3_bind_int64(Statement, Index, static_cast<sqlite3_int64>(Value));
			}

			void BindText(int Index, std::string_view Value) {
				if (Code == SQLITE_OK)
					Code = sqlite3_bind_text(Statement, Index, Value.data(), static_cast<int>(Value.size()), SQLITE_TRANSIENT);
			}

			void BindBlob(int Index, std::span<const std::byte> Value) {
				if (Code != SQLITE_OK) return;
				if (Value.empty()) Code = sqlite3_bind_zeroblob(Statement, Index, 0);
				else Code = sqlite3_bind_blob(Statement, Index, Value.data(), static_cast<int>(Value.size()), SQLITE_TRANSIENT);
			}

			void BindAddress(const PersistenceRequest& Request) {
				BindInteger(1, Request.Kind == PersistenceStoreKind::KeyValue ? 0 : 1);
				BindText(2, Request.Store);
				BindText(3, Request.Key);
			}

			int Step() const { return Code == SQLITE_OK ? sqlite3_step(Statement) : Code; }
			sqlite3_int64 Integer(int Column) const { return sqlite3_column_int64(Statement, Column); }
			std::span<const std::byte> Blob(int Column) const {
				const auto* Data = static_cast<const std::byte*>(sqlite3_column_blob(Statement, Column));
				const auto Bytes = static_cast<std::size_t>(sqlite3_column_bytes(Statement, Column));
				if (!Data) return {};
				return std::span(Data, Bytes);
			}
			std::string_view Text(int Column) const {
				const auto* Data = reinterpret_cast<const char*>(sqlite3_column_text(Statement, Column));
				const auto Bytes = static_cast<std::size_t>(sqlite3_column_bytes(Statement, Column));
				if (!Data) return {};
				return std::string_view(Data, Bytes);
			}

		private:
			sqlite3_stmt* Statement = nullptr;
			int Code;
		};

		class SqlCancellation final {
		public:
			SqlCancellation(sqlite3* Database, const Execution& Context) : Database(Database) {
				sqlite3_busy_handler(Database, [](void* State, int) {
					const auto& Current = *static_cast<const Execution*>(State);
					if (Current.Interruption()) return 0;
					std::this_thread::sleep_for(2ms);
					return Current.Interruption() ? 0 : 1;
				}, const_cast<Execution*>(&Context));
				sqlite3_progress_handler(Database, 500, [](void* State) {
					return static_cast<const Execution*>(State)->Interruption() ? 1 : 0;
				}, const_cast<Execution*>(&Context));
			}
			~SqlCancellation() {
				sqlite3_busy_handler(Database, nullptr, nullptr);
				sqlite3_progress_handler(Database, 0, nullptr, nullptr);
			}
			SqlCancellation(const SqlCancellation&) = delete;
			SqlCancellation& operator=(const SqlCancellation&) = delete;
		private:
			sqlite3* Database;
		};

		class SqlTransaction final {
		public:
			explicit SqlTransaction(sqlite3* Database)
				: Database(Database), BeginResult(SqlExec(Database, "BEGIN IMMEDIATE")) {}
			~SqlTransaction() {
				if (sqlite3_get_autocommit(Database)) return;
				sqlite3_progress_handler(Database, 0, nullptr, nullptr);
				SqlExec(Database, "ROLLBACK");
			}
			SqlTransaction(const SqlTransaction&) = delete;
			SqlTransaction& operator=(const SqlTransaction&) = delete;

			int BeginCode() const { return BeginResult; }
			int Commit() const { return SqlExec(Database, "COMMIT"); }

		private:
			sqlite3* Database;
			int BeginResult;
		};

		PersistenceStatus SqlFailure(int Code, const Execution& Context) {
			if (const auto Interrupted = Context.Interruption()) return *Interrupted;
			switch (Code & 0xff) {
			case SQLITE_BUSY:
			case SQLITE_LOCKED:
			case SQLITE_FULL: return PersistenceStatus::Busy;
			default: return PersistenceStatus::Unavailable;
			}
		}

		bool SafePath(const std::filesystem::path& Path, bool Directory, bool& Exists) {
			std::error_code Error;
			const auto File = std::filesystem::symlink_status(Path, Error);
			Exists = std::filesystem::exists(File);
			if (!Exists) {
				return !Directory && (File.type() == std::filesystem::file_type::not_found)
					&& (!Error || Error == std::errc::no_such_file_or_directory);
			}
			if (Error || (Directory ? !std::filesystem::is_directory(File) : !std::filesystem::is_regular_file(File)))
				return false;
#if defined(_WIN32)
			const DWORD Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES) {
				// A writer can remove its journal while a second handle opens.
				const DWORD NativeError = GetLastError();
				Exists = false;
				return !Directory && (NativeError == ERROR_FILE_NOT_FOUND || NativeError == ERROR_PATH_NOT_FOUND);
			}
			if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
#endif
			return true;
		}

		bool ValidatePath(const std::filesystem::path& Path, bool& Exists) {
			const auto Root = Path.root_path();
#if defined(_WIN32)
			if (GetDriveTypeW(Root.c_str()) != DRIVE_FIXED) return false;
#endif
			bool EntryExists = false;
			for (auto Directory = Path.parent_path(); Directory != Root; Directory = Directory.parent_path()) {
				if (!SafePath(Directory, true, EntryExists)) return false;
			}
			if (!SafePath(Path, false, Exists)) return false;
			if (Exists) {
				std::error_code Error;
				if (std::filesystem::file_size(Path, Error) > FileByteCeiling || Error) return false;
			}
			auto Journal = Path;
			Journal += "-journal";
			if (!SafePath(Journal, false, EntryExists)) return false;
			if (EntryExists) {
				std::error_code Error;
				const auto Bytes = std::filesystem::file_size(Journal, Error);
				if (Error != std::errc::no_such_file_or_directory && (Error || Bytes > 2 * FileByteCeiling)) return false;
			}
			// This format uses the rollback journal exclusively.
			auto Wal = Path;
			Wal += "-wal";
			if (!SafePath(Wal, false, EntryExists) || EntryExists) return false;
			auto SharedMemory = Path;
			SharedMemory += "-shm";
			return SafePath(SharedMemory, false, EntryExists) && !EntryExists;
		}

		std::optional<sqlite3_int64> SqlInteger(sqlite3* Database, const char* Sql) {
			const SqlStatement Statement(Database, Sql);
			if (Statement.Step() != SQLITE_ROW) return std::nullopt;
			return Statement.Integer(0);
		}

		bool InitializeDatabase(sqlite3* Database, const DurableLocalPersistenceConfiguration& Configuration) {
			if (SqlInteger(Database, "PRAGMA user_version") != 0
				|| SqlInteger(Database, "SELECT count(*) FROM sqlite_schema") != 0) return false;
			if (SqlExec(Database, CreateProfile) != SQLITE_OK || SqlExec(Database, CreateRecords) != SQLITE_OK
				|| SqlExec(Database, "PRAGMA application_id=1195660081; PRAGMA user_version=1") != SQLITE_OK) return false;
			SqlStatement Insert(Database,
				"INSERT INTO profile VALUES (1, ?1, 0, 0, 0, ?2, ?3)");
			Insert.BindBlob(1, std::as_bytes(std::span(Configuration.ProfileId.data(), Configuration.ProfileId.size())));
			Insert.BindInteger(2, Configuration.Limits.MaximumRecords);
			Insert.BindInteger(3, Configuration.Limits.MaximumPayloadBytes);
			return Insert.Step() == SQLITE_DONE;
		}

		std::optional<StoreUsage> ReadUsage(sqlite3* Database, const LocalPersistenceLimits& Limits) {
			const SqlStatement Profile(Database, "SELECT revision, records, payload_bytes FROM profile WHERE id=1");
			if (Profile.Step() != SQLITE_ROW) return std::nullopt;
			const auto Revision = Profile.Integer(0);
			const auto Records = Profile.Integer(1);
			const auto PayloadBytes = Profile.Integer(2);
			if (Revision < 0 || Records < 0 || PayloadBytes < 0
				|| static_cast<std::uint64_t>(Records) > Limits.MaximumRecords
				|| static_cast<std::uint64_t>(PayloadBytes) > Limits.MaximumPayloadBytes) return std::nullopt;
			return StoreUsage{static_cast<std::uint64_t>(Revision), static_cast<std::size_t>(Records),
				static_cast<std::size_t>(PayloadBytes)};
		}

		bool VerifyDatabase(sqlite3* Database, const DurableLocalPersistenceConfiguration& Configuration) {
			const auto Version = SqlInteger(Database, "PRAGMA user_version");
			if (Version != 1 && Version != 2) return false;
			const SqlStatement Schema(Database, "SELECT sql FROM sqlite_schema ORDER BY name");
			if (Schema.Step() != SQLITE_ROW || Schema.Text(0) != CreateProfile
				|| (Version == 2 && (Schema.Step() != SQLITE_ROW || Schema.Text(0) != CreateReceipts))
				|| Schema.Step() != SQLITE_ROW || Schema.Text(0) != CreateRecords || Schema.Step() != SQLITE_DONE)
				return false;
			const SqlStatement Profile(Database, "SELECT identity, maximum_records, maximum_payload_bytes FROM profile WHERE id=1");
			if (Profile.Step() != SQLITE_ROW) return false;
			const auto Identity = std::as_bytes(std::span(Configuration.ProfileId.data(), Configuration.ProfileId.size()));
			if (!std::ranges::equal(Profile.Blob(0), Identity)
				|| Profile.Integer(1) != static_cast<sqlite3_int64>(Configuration.Limits.MaximumRecords)
				|| Profile.Integer(2) != static_cast<sqlite3_int64>(Configuration.Limits.MaximumPayloadBytes)) return false;
			const SqlStatement Integrity(Database, "PRAGMA quick_check(1)");
			if (Integrity.Step() != SQLITE_ROW || Integrity.Text(0) != "ok") return false;
			const auto Usage = ReadUsage(Database, Configuration.Limits);
			const SqlStatement Records(Database,
				"SELECT count(*), coalesce(sum(length(payload)),0), coalesce(max(revision),0) FROM records");
			return Usage && Records.Step() == SQLITE_ROW
				&& Records.Integer(0) == static_cast<sqlite3_int64>(Usage->Records)
				&& Records.Integer(1) == static_cast<sqlite3_int64>(Usage->PayloadBytes)
				&& Records.Integer(2) <= static_cast<sqlite3_int64>(Usage->Revision);
		}

		struct SqlCloser final {
			void operator()(sqlite3* Database) const noexcept { sqlite3_close(Database); }
		};
		using SqlDatabase = std::unique_ptr<sqlite3, SqlCloser>;

		PersistenceStatus OpenCheckedDatabase(SqlDatabase& Opened, const DurableLocalPersistenceConfiguration& Configuration,
			const std::filesystem::path& Location, bool MayInitialize, const Execution& Context) {
			if (const auto Interrupted = Context.Interruption()) return *Interrupted;
			const auto Path = Location.u8string();
			sqlite3* RawDatabase = nullptr;
			const int Flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW
				| (MayInitialize ? SQLITE_OPEN_CREATE : 0);
			const int OpenCode = sqlite3_open_v2(reinterpret_cast<const char*>(Path.c_str()), &RawDatabase, Flags, nullptr);
			SqlDatabase Candidate(RawDatabase);
			if (OpenCode != SQLITE_OK) return SqlFailure(OpenCode, Context);
			const SqlCancellation Cancellation(RawDatabase, Context);
			sqlite3_limit(RawDatabase, SQLITE_LIMIT_LENGTH, static_cast<int>(MaximumEncodedDocument + 16 * 1024));
			if (sqlite3_db_config(RawDatabase, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) != SQLITE_OK
				|| sqlite3_db_config(RawDatabase, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK)
				return PersistenceStatus::Unavailable;
			if (MayInitialize && SqlExec(RawDatabase, "PRAGMA page_size=4096") != SQLITE_OK)
				return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			if (SqlExec(RawDatabase, "PRAGMA synchronous=EXTRA; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-1024") != SQLITE_OK)
				return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			if (SqlInteger(RawDatabase, "PRAGMA page_size") != FilePageSize
				|| SqlInteger(RawDatabase, "PRAGMA max_page_count=32768") != FilePageCeiling)
				return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			{
				const SqlStatement Journal(RawDatabase, "PRAGMA journal_mode");
				if (Journal.Step() != SQLITE_ROW || Journal.Text(0) != "delete")
					return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			}
			const SqlTransaction Initialize(RawDatabase);
			if (Initialize.BeginCode() != SQLITE_OK) return SqlFailure(Initialize.BeginCode(), Context);
			const auto AppId = SqlInteger(RawDatabase, "PRAGMA application_id");
			if (AppId == 0 && MayInitialize) {
				if (!InitializeDatabase(RawDatabase, Configuration))
					return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			} else if (AppId != LocalApplicationId) {
				return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			}
			if (!VerifyDatabase(RawDatabase, Configuration)) return PersistenceStatus::Unavailable;
			if (Configuration.RecordWorldWrites && SqlInteger(RawDatabase, "PRAGMA user_version") == 1) {
				// Upgrade a verified v1 slot and its receipt schema in this opening transaction.
				if (SqlExec(RawDatabase, CreateReceipts) != SQLITE_OK ||
					SqlExec(RawDatabase, "PRAGMA user_version=2") != SQLITE_OK)
					return SqlFailure(sqlite3_errcode(RawDatabase), Context);
			}
			if (const auto Interrupted = Context.Interruption()) return *Interrupted;
			const int Commit = Initialize.Commit();
			if (Commit != SQLITE_OK) return SqlFailure(Commit, Context);
			Opened = std::move(Candidate);
			return PersistenceStatus::Ok;
		}

		enum class DatabasePublication { Published, Locked, Failed };

		DatabasePublication PublishDatabase(const std::filesystem::path& Staging, const std::filesystem::path& Destination) {
#if defined(_WIN32)
			if (MoveFileExW(Staging.c_str(), Destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
				return DatabasePublication::Published;
			const auto Error = GetLastError();
			if (Error == ERROR_SHARING_VIOLATION || Error == ERROR_LOCK_VIOLATION) return DatabasePublication::Locked;
			return DatabasePublication::Failed;
#else
			std::error_code Error;
			std::filesystem::create_hard_link(Staging, Destination, Error);
			if (Error) return DatabasePublication::Failed;
			std::filesystem::remove(Staging, Error);
			return DatabasePublication::Published;
#endif
		}

		PersistenceStatus OpenDatabase(SqlDatabase& Opened, const DurableLocalPersistenceConfiguration& Configuration,
			const Execution& Context) {
			bool Existing = false;
			if (!ValidatePath(Configuration.Path, Existing) || (!Existing && !Configuration.CreateIfMissing))
				return PersistenceStatus::Unavailable;
			if (!Existing) {
				// This sibling is reserved for empty schemas. A crash or cancellation
				// here can be retried without treating a truncated final DB as empty.
				auto Staging = Configuration.Path;
				Staging += ".initializing";
				bool StagingExists = false;
				if (!ValidatePath(Staging, StagingExists)) return PersistenceStatus::Unavailable;
				SqlDatabase Prepared;
				const auto Preparation = OpenCheckedDatabase(Prepared, Configuration, Staging, true, Context);
				if (Preparation != PersistenceStatus::Ok) {
					// Another opener may have published the stage while this open was pending.
					if (!ValidatePath(Configuration.Path, Existing) || !Existing) return Preparation;
				}
				Prepared.reset();
				while (!Existing) {
					if (const auto Interrupted = Context.Interruption()) return *Interrupted;
					const auto Published = PublishDatabase(Staging, Configuration.Path);
					if (Published == DatabasePublication::Published) break;
					if (!ValidatePath(Configuration.Path, Existing)) return PersistenceStatus::Unavailable;
					if (Existing) break;
					if (Published == DatabasePublication::Failed) return PersistenceStatus::Unavailable;
					std::this_thread::sleep_for(2ms);
				}
			}
			return OpenCheckedDatabase(Opened, Configuration, Configuration.Path, false, Context);
		}

		std::optional<RecordMetadata> SqlRecordMetadata(const SqlStatement& Row) {
			const auto Revision = Row.Integer(0);
			const auto Bytes = Row.Integer(1);
			if (Revision <= 0 || Bytes < 0 || static_cast<std::uint64_t>(Bytes) > MaximumEncodedDocument)
				return std::nullopt;
			return RecordMetadata{static_cast<std::uint64_t>(Revision), static_cast<std::size_t>(Bytes)};
		}

		PersistenceResult DurableGet(sqlite3* Database, const PersistenceRequest& Request, const Execution& Context) {
			SqlStatement Lookup(Database, "SELECT revision, length(payload), payload FROM records WHERE kind=?1 AND store=?2 AND key=?3");
			Lookup.BindAddress(Request);
			const int Found = Lookup.Step();
			if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
			if (Found == SQLITE_DONE) return Status(PersistenceStatus::Missing);
			if (Found != SQLITE_ROW) return Status(SqlFailure(Found, Context));
			const auto Metadata = SqlRecordMetadata(Lookup);
			const auto Data = Lookup.Blob(2);
			if (!Metadata || Metadata->PayloadBytes != Data.size()) return Status(PersistenceStatus::Unavailable);
			return ReadResult(Request, Metadata->Revision, Data);
		}

		PersistenceResult DurableMutation(sqlite3* Database, const PersistenceRequest& Request, const Execution& Context,
			const LocalPersistenceLimits& Limits, const PersistenceBytes& Encoded, bool RecordReceipt) {
			const SqlTransaction Transaction(Database);
			if (Transaction.BeginCode() != SQLITE_OK) return Status(SqlFailure(Transaction.BeginCode(), Context));
			std::optional<RecordMetadata> Existing;
			{
				SqlStatement Lookup(Database, "SELECT revision, length(payload) FROM records WHERE kind=?1 AND store=?2 AND key=?3");
				Lookup.BindAddress(Request);
				const int Found = Lookup.Step();
				if (Found == SQLITE_ROW) {
					Existing = SqlRecordMetadata(Lookup);
					if (!Existing) return Status(PersistenceStatus::Unavailable);
				} else if (Found != SQLITE_DONE) {
					return Status(SqlFailure(Found, Context));
				}
			}
			auto Usage = ReadUsage(Database, Limits);
			if (!Usage) return Status(SqlFailure(sqlite3_errcode(Database), Context));
			if (const auto Rejected = PrepareMutation(Request, Limits, Existing, Encoded.size(), *Usage))
				return Status(*Rejected);
			{
				const bool Delete = Request.Operation == PersistenceOperation::Delete;
				SqlStatement Write(Database, Delete
					? "DELETE FROM records WHERE kind=?1 AND store=?2 AND key=?3"
					: "INSERT INTO records VALUES (?1, ?2, ?3, ?4, ?5) "
						"ON CONFLICT (kind, store, key) DO UPDATE SET payload=excluded.payload, revision=excluded.revision");
				Write.BindAddress(Request);
				if (!Delete) {
					Write.BindBlob(4, Encoded);
					Write.BindInteger(5, Usage->Revision);
				}
				const int Written = Write.Step();
				if (Written != SQLITE_DONE) return Status(SqlFailure(Written, Context));
			}
			{
				SqlStatement Update(Database, "UPDATE profile SET revision=?1, records=?2, payload_bytes=?3 WHERE id=1");
				Update.BindInteger(1, Usage->Revision);
				Update.BindInteger(2, Usage->Records);
				Update.BindInteger(3, Usage->PayloadBytes);
				const int Updated = Update.Step();
				if (Updated != SQLITE_DONE) return Status(SqlFailure(Updated, Context));
			}
			if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
			if (RecordReceipt && !Request.RequestId.empty()) {
				SqlStatement Receipt(Database, "INSERT OR REPLACE INTO receipts VALUES (?1, ?2)");
				Receipt.BindInteger(1, Usage->Revision % 64);
				Receipt.BindText(2, Request.RequestId);
				const int Written = Receipt.Step();
				if (Written != SQLITE_DONE) return Status(SqlFailure(Written, Context));
			}
			const int Committed = Transaction.Commit();
			if (Committed == SQLITE_OK) return MutationResult(Usage->Revision);
			if (((Committed & 0xff) == SQLITE_BUSY || (Committed & 0xff) == SQLITE_LOCKED) && !sqlite3_get_autocommit(Database))
				return Status(SqlFailure(Committed, Context));
			return Status(PersistenceStatus::UnknownOutcome);
		}

		class DurableLocalPersistenceProvider final : public IPersistenceProvider, public ILocalMutationReceipts {
		public:
			explicit DurableLocalPersistenceProvider(DurableLocalPersistenceConfiguration Configuration)
				: Configuration(std::move(Configuration)) {}

			PersistenceCapabilities Capabilities() const noexcept override { return LocalCapabilities; }

			PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) override {
				const Execution Context{Stop, Request.Deadline};
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
				PersistenceBytes Encoded;
				if (const auto Invalid = ValidateRequest(Request, Encoded)) return Status(*Invalid);
				auto Guard = Context.Lock(Mutex);
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
				if (!Database) {
					const auto Opened = OpenDatabase(Database, Configuration, Context);
					if (Opened != PersistenceStatus::Ok) return Status(Opened);
				}
				if (const auto Interrupted = Context.Interruption()) return Status(*Interrupted);
				const SqlCancellation Cancellation(Database.get(), Context);
				if (Request.Operation == PersistenceOperation::Get) return DurableGet(Database.get(), Request, Context);
				return DurableMutation(Database.get(), Request, Context, Configuration.Limits, Encoded, Configuration.RecordWorldWrites);
			}

			PersistenceStatus ReadCommitReceipt(const std::string& RequestId, PersistenceClock::time_point Deadline) override {
				const Execution Context{{}, Deadline};
				auto Guard = Context.Lock(Mutex);
				if (Context.Interruption() || !Configuration.RecordWorldWrites || !Database || RequestId.empty())
					return PersistenceStatus::UnknownOutcome;
				const SqlCancellation Cancellation(Database.get(), Context);
				SqlStatement Read(Database.get(), "SELECT 1 FROM receipts WHERE request=?1 LIMIT 1");
				Read.BindText(1, RequestId);
				return Read.Step() == SQLITE_ROW ? PersistenceStatus::Ok : PersistenceStatus::UnknownOutcome;
			}

		private:
			const DurableLocalPersistenceConfiguration Configuration;
			std::timed_mutex Mutex;
			SqlDatabase Database;
		};
#endif
	}

	std::shared_ptr<IPersistenceProvider> MakeMemoryPersistenceProvider(LocalPersistenceLimits Limits) {
		if (!ValidLimits(Limits)) return {};
		return std::make_shared<MemoryPersistenceProvider>(Limits);
	}

	bool SupportsDurableLocalPersistence() noexcept {
#if defined(GARGANTUAN_WITH_LOCAL_SQLITE) && GARGANTUAN_WITH_LOCAL_SQLITE
		return true;
#else
		return false;
#endif
	}

	std::shared_ptr<IPersistenceProvider> MakeDurableLocalPersistenceProvider(DurableLocalPersistenceConfiguration Configuration) {
#if defined(GARGANTUAN_WITH_LOCAL_SQLITE) && GARGANTUAN_WITH_LOCAL_SQLITE
		if (!ValidLimits(Configuration.Limits) || Configuration.ProfileId.empty()
			|| Configuration.ProfileId.size() > DurableLocalPersistenceConfiguration::MaximumProfileIdBytes
			|| Configuration.ProfileId.find('\0') != std::string::npos
			|| !Configuration.Path.is_absolute()
			|| Configuration.Path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
			return {};
		Configuration.Path = Configuration.Path.lexically_normal();
		if (Configuration.Path.filename().empty() || Configuration.Path.filename() == "."
			|| Configuration.Path.filename().u8string().ends_with(u8".initializing")) return {};
		return std::make_shared<DurableLocalPersistenceProvider>(std::move(Configuration));
#else
		static_cast<void>(Configuration);
		return {};
#endif
	}
}
