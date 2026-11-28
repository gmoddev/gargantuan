#include "gargantuan/persistence/PersistenceProvider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace gargantuan {
	namespace {
		bool IsName(const std::string& Name, std::size_t Maximum) {
			return !Name.empty() && Name.size() <= Maximum && IsPersistenceUtf8(Name) &&
				Name.find_first_not_of(" \t\v\r\n\f") != std::string::npos &&
				std::ranges::none_of(Name, [](unsigned char Byte) { return Byte < 32 || Byte == 127; });
		}

		bool Charge(std::size_t Amount, std::size_t& Bytes) {
			if (Amount > PersistenceMaxValueBytes - Bytes) return false;
			Bytes += Amount;
			return true;
		}

		using FieldScratch = std::array<std::string_view, PersistenceMaxDocumentNodes>;
		bool Measure(const PersistenceDocumentValue& Value, std::size_t Depth, std::size_t& Nodes,
			std::size_t& Bytes, FieldScratch& Fields) {
			if (Depth > PersistenceMaxDocumentDepth || ++Nodes > PersistenceMaxDocumentNodes || !Charge(8, Bytes))
				return false;
			if (const auto* Number = std::get_if<double>(&Value.Data)) return std::isfinite(*Number);
			if (const auto* String = std::get_if<std::string>(&Value.Data))
				return Charge(String->size(), Bytes) && String->find('\0') == std::string::npos && IsPersistenceUtf8(*String);
			if (const auto* Array = std::get_if<PersistenceDocumentValue::Array>(&Value.Data)) {
				if (Array->size() > PersistenceMaxDocumentNodes - Nodes) return false;
				for (const auto& Item : *Array) if (!Measure(Item, Depth + 1, Nodes, Bytes, Fields)) return false;
			}
			if (const auto* Object = std::get_if<PersistenceDocumentValue::Object>(&Value.Data)) {
				if (Object->size() > PersistenceMaxDocumentNodes - Nodes) return false;
				// Bounded stack storage keeps validation allocation-free. Sorting views
				// catches duplicate names without quadratic scans of wide objects.
				std::size_t Count = 0;
				for (const auto& Field : *Object) {
					if (!IsPersistenceField(Field.Name) || !Charge(Field.Name.size() + 4, Bytes)) return false;
					Fields[Count++] = Field.Name;
				}
				const auto End = Fields.begin() + Count;
				std::sort(Fields.begin(), End);
				if (std::adjacent_find(Fields.begin(), End) != End) return false;
				for (const auto& Field : *Object)
					if (!Measure(Field.Value, Depth + 1, Nodes, Bytes, Fields)) return false;
			}
			return true;
		}

		bool MeasurePayload(PersistenceStoreKind Kind, const PersistencePayload& Payload, std::size_t& Bytes) {
			if (Kind == PersistenceStoreKind::KeyValue) {
				const auto* Binary = std::get_if<PersistenceBytes>(&Payload);
				if (!Binary || Binary->size() > PersistenceMaxValueBytes) return false;
				Bytes = Binary->size();
				return true;
			}
			const auto* Document = std::get_if<PersistenceDocumentValue>(&Payload);
			return Document && std::holds_alternative<PersistenceDocumentValue::Object>(Document->Data) &&
				MeasurePersistenceDocument(*Document, Bytes);
		}

		bool IsPath(const std::vector<std::string>& Path) {
			return !Path.empty() && Path.size() <= PersistenceMaxPathDepth &&
				std::ranges::all_of(Path, IsPersistenceField);
		}

		bool IsQuery(const PersistenceQuery& Query) {
			if (Query.Limit == 0 || Query.Limit > PersistenceMaxPageRecords ||
				Query.Cursor.size() > PersistenceMaxCursorBytes || Query.Filters.size() > PersistenceMaxFilters ||
				Query.Order.size() > PersistenceMaxOrder) return false;
			std::size_t Bytes = 0;
			for (const auto& Filter : Query.Filters) {
				if (Filter.Operator < PersistenceComparison::Equal || Filter.Operator > PersistenceComparison::Exists ||
					!IsPath(Filter.Path) || Filter.Value.Data.index() > 3 ||
					(Filter.Operator == PersistenceComparison::Exists && Filter.Value.Data.index() != 0)) return false;
				std::size_t OperandBytes = 0;
				if (!MeasurePersistenceDocument(Filter.Value, OperandBytes) || !Charge(OperandBytes, Bytes)) return false;
			}
			return std::ranges::all_of(Query.Order, [](const auto& Order) { return IsPath(Order.Path); });
		}
	}

	const char* GetPersistenceStatusName(PersistenceStatus Status) noexcept {
		switch (Status) {
			case PersistenceStatus::Pending: return "Pending";
			case PersistenceStatus::Ok: return "Ok";
			case PersistenceStatus::Missing: return "Missing";
			case PersistenceStatus::Conflict: return "Conflict";
			case PersistenceStatus::Invalid: return "Invalid";
			case PersistenceStatus::Denied: return "Denied";
			case PersistenceStatus::Unavailable: return "Unavailable";
			case PersistenceStatus::Unsupported: return "Unsupported";
			case PersistenceStatus::Deadline: return "Deadline";
			case PersistenceStatus::Cancelled: return "Cancelled";
			case PersistenceStatus::UnknownOutcome: return "UnknownOutcome";
			case PersistenceStatus::Busy: return "Busy";
			case PersistenceStatus::StaleCursor: return "StaleCursor";
			case PersistenceStatus::Consumed: return "Consumed";
			case PersistenceStatus::StorageFull: return "StorageFull";
		}
		return "Unavailable";
	}

	bool IsPersistenceMutation(PersistenceOperation Operation) noexcept {
		return Operation == PersistenceOperation::Put || Operation == PersistenceOperation::Delete;
	}

	bool IsPersistenceStoreName(const std::string& Name) noexcept { return IsName(Name, PersistenceMaxStoreBytes); }
	bool IsPersistenceKey(const std::string& Key) noexcept { return IsName(Key, PersistenceMaxKeyBytes); }
	bool IsPersistenceField(const std::string& Field) noexcept { return IsName(Field, PersistenceMaxFieldBytes); }

	bool IsPersistenceUtf8(const std::string& Text) noexcept {
		std::size_t Index = 0;
		while (Index < Text.size()) {
			const auto Lead = static_cast<unsigned char>(Text[Index++]);
			if (Lead < 0x80) continue;
			std::size_t Remaining = 0;
			std::uint32_t Code = 0;
			std::uint32_t Minimum = 0;
			if (Lead >= 0xc2 && Lead <= 0xdf) { Remaining = 1; Code = Lead & 0x1f; Minimum = 0x80; }
			else if (Lead >= 0xe0 && Lead <= 0xef) { Remaining = 2; Code = Lead & 0x0f; Minimum = 0x800; }
			else if (Lead >= 0xf0 && Lead <= 0xf4) { Remaining = 3; Code = Lead & 7; Minimum = 0x10000; }
			else return false;
			if (Remaining > Text.size() - Index) return false;
			for (; Remaining > 0; --Remaining) {
				const auto Byte = static_cast<unsigned char>(Text[Index++]);
				if ((Byte & 0xc0) != 0x80) return false;
				Code = (Code << 6) | (Byte & 0x3f);
			}
			if (Code < Minimum || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)) return false;
		}
		return true;
	}

	bool MeasurePersistenceDocument(const PersistenceDocumentValue& Value, std::size_t& Bytes) noexcept {
		std::size_t Nodes = 0;
		Bytes = 0;
		FieldScratch Fields;
		return Measure(Value, 1, Nodes, Bytes, Fields);
	}

	bool IsPersistenceRequestValid(const PersistenceRequest& Request) noexcept {
		if ((Request.Kind != PersistenceStoreKind::KeyValue && Request.Kind != PersistenceStoreKind::Document) ||
			!IsPersistenceStoreName(Request.Store)) return false;
		const auto* Bytes = std::get_if<PersistenceBytes>(&Request.Payload);
		const bool EmptyPayload = Bytes && Bytes->empty();
		if (Request.Operation == PersistenceOperation::Query)
			return Request.Kind == PersistenceStoreKind::Document && Request.Key.empty() && EmptyPayload &&
				!Request.ExpectedRevision && IsQuery(Request.Query);
		const auto& Query = Request.Query;
		if (!Query.Filters.empty() || !Query.Order.empty() || !Query.Cursor.empty() ||
			Query.Limit != PersistenceQuery{}.Limit) return false;
		if (!IsPersistenceKey(Request.Key)) return false;
		if (Request.Operation == PersistenceOperation::Put) {
			std::size_t PayloadBytes = 0;
			return MeasurePayload(Request.Kind, Request.Payload, PayloadBytes);
		}
		if (!EmptyPayload) return false;
		if (Request.Operation == PersistenceOperation::Delete) return Request.ExpectedRevision != 0;
		return Request.Operation == PersistenceOperation::Get && !Request.ExpectedRevision;
	}

	bool IsPersistenceResultValid(const PersistenceRequest& Request, const PersistenceResult& Result) noexcept {
		if (Result.Status < PersistenceStatus::Ok || Result.Status > PersistenceStatus::StaleCursor) return false;
		if (Result.Status != PersistenceStatus::Ok)
			return Result.Revision == 0 && !Result.Record && Result.Records.empty() && Result.Cursor.empty();
		if (Request.Operation == PersistenceOperation::Query) {
			if (Result.Record || Result.Revision != 0 || Result.Records.size() > Request.Query.Limit ||
				Result.Cursor.size() > PersistenceMaxCursorBytes) return false;
			std::size_t PageBytes = 0;
			for (const auto& Record : Result.Records) {
				std::size_t Bytes = 0;
				if (Record.Revision == 0 || !IsPersistenceKey(Record.Key) ||
					!MeasurePayload(Request.Kind, Record.Payload, Bytes)) return false;
				PageBytes += Bytes + Record.Key.size();
			}
			return PageBytes <= PersistenceMaxPageBytes;
		}
		if (!Result.Records.empty() || !Result.Cursor.empty()) return false;
		if (IsPersistenceMutation(Request.Operation)) return Result.Revision != 0 && !Result.Record;
		std::size_t Bytes = 0;
		return Result.Revision == 0 && Result.Record && Result.Record->Key == Request.Key &&
			Result.Record->Revision != 0 && MeasurePayload(Request.Kind, Result.Record->Payload, Bytes);
	}
}
