#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace gargantuan {
	enum class PersistenceStoreKind { KeyValue, Document };
	enum class PersistenceOperation { Get, Put, Delete, Query };
	enum class PersistenceStatus {
		Pending, Ok, Missing, Conflict, Invalid, Denied, Unavailable,
		Unsupported, Deadline, Cancelled, UnknownOutcome, Busy, StaleCursor, Consumed,
	};

	struct PersistenceCapabilities final {
		bool KeyValue = false;
		bool Documents = false;
		bool DocumentQueries = false;
	};

	// The only numeric document type is a finite IEEE 754 binary64 value.
	// An object has unique UTF-8 field names. The outer document is an object.
	struct PersistenceDocumentField;
	struct PersistenceDocumentValue final {
		using Array = std::vector<PersistenceDocumentValue>;
		using Object = std::vector<PersistenceDocumentField>;
		std::variant<std::monostate, bool, double, std::string, Array, Object> Data;
		bool operator==(const PersistenceDocumentValue &) const = default;
	};

	struct PersistenceDocumentField final {
		std::string Name;
		PersistenceDocumentValue Value;
		bool operator==(const PersistenceDocumentField &) const = default;
	};

	using PersistenceBytes = std::vector<std::byte>;
	using PersistencePayload = std::variant<PersistenceBytes, PersistenceDocumentValue>;
	using PersistenceClock = std::chrono::steady_clock;

	// V1 exposes a bounded conjunction of comparisons and optional field order.
	enum class PersistenceComparison { Equal, NotEqual, Less, LessOrEqual, Greater, GreaterOrEqual, Exists };
	struct PersistenceFilter final {
		std::vector<std::string> Path;
		PersistenceComparison Operator = PersistenceComparison::Equal;
		PersistenceDocumentValue Value;
	};
	struct PersistenceOrder final {
		std::vector<std::string> Path;
		bool Descending = false;
	};
	struct PersistenceQuery final {
		std::vector<PersistenceFilter> Filters;
		std::vector<PersistenceOrder> Order;
		std::uint32_t Limit = 10;
		std::string Cursor;
	};

	struct PersistenceRequest final {
		PersistenceStoreKind Kind = PersistenceStoreKind::KeyValue;
		PersistenceOperation Operation = PersistenceOperation::Get;
		std::string RequestId;
		std::string Store;
		std::string Key;
		PersistencePayload Payload;
		// Absent is unconditional. Zero is create-only and is valid only for Put.
		std::optional<std::uint64_t> ExpectedRevision;
		PersistenceQuery Query;
		PersistenceClock::time_point Deadline;
	};

	struct PersistenceRecord final {
		std::string Key;
		PersistencePayload Payload;
		std::uint64_t Revision = 0;
	};

	struct PersistenceResult final {
		PersistenceStatus Status = PersistenceStatus::Unavailable;
		std::uint64_t Revision = 0;
		std::optional<PersistenceRecord> Record;
		std::vector<PersistenceRecord> Records;
		std::string Cursor;
	};

	inline constexpr std::size_t PersistenceMaxStoreBytes = 96;
	inline constexpr std::size_t PersistenceMaxKeyBytes = 256;
	inline constexpr std::size_t PersistenceMaxValueBytes = 64 * 1024;
	inline constexpr std::size_t PersistenceMaxDocumentNodes = 2048;
	inline constexpr std::size_t PersistenceMaxDocumentDepth = 12;
	inline constexpr std::size_t PersistenceMaxPageRecords = 10;
	inline constexpr std::size_t PersistenceMaxPageBytes = 1024 * 1024;
	inline constexpr std::size_t PersistenceMaxCursorBytes = 512;
	inline constexpr std::size_t PersistenceMaxFieldBytes = 128;
	inline constexpr std::size_t PersistenceMaxFilters = 8;
	inline constexpr std::size_t PersistenceMaxOrder = 4;
	inline constexpr std::size_t PersistenceMaxPathDepth = 8;

	// Optional native capability of an exclusively owned local world store.
	// A receipt identifies the original transaction even after subsequent writes.
	class ILocalMutationReceipts {
	public:
		virtual ~ILocalMutationReceipts() = default;
		virtual PersistenceStatus ReadCommitReceipt(const std::string& RequestId, PersistenceClock::time_point Deadline) = 0;
	};

	class IPersistenceProvider {
	public:
		virtual ~IPersistenceProvider() = default;
		virtual PersistenceCapabilities Capabilities() const noexcept = 0;
		// Runs on native workers, potentially concurrently. Providers must honor
		// cancellation and the steady deadline, including time spent acquiring
		// internal locks, and classify uncertain mutations as UnknownOutcome.
		// Namespace and credentials come only from trusted host composition.
		virtual PersistenceResult Execute(const PersistenceRequest& Request, std::stop_token Stop) = 0;
	};

	const char* GetPersistenceStatusName(PersistenceStatus Status) noexcept;
	bool IsPersistenceMutation(PersistenceOperation Operation) noexcept;
	bool IsPersistenceStoreName(const std::string& Name) noexcept;
	bool IsPersistenceKey(const std::string& Key) noexcept;
	bool IsPersistenceUtf8(const std::string& Text) noexcept;
	bool IsPersistenceField(const std::string& Field) noexcept;
	bool MeasurePersistenceDocument(const PersistenceDocumentValue& Value, std::size_t& Bytes) noexcept;
	bool IsPersistenceRequestValid(const PersistenceRequest& Request) noexcept;
	bool IsPersistenceResultValid(const PersistenceRequest& Request, const PersistenceResult& Result) noexcept;
}
