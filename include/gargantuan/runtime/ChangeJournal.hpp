#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "gargantuan/runtime/WireValue.hpp"

namespace gargantuan {
	class InProcessReplicationSession;

	struct ObjectCreatedChange { std::string ClassName; };
	struct PropertyUpdatedChange {
		std::string PropertyName;
		WireValue Value;
		bool Replicated = false;
	};
	struct AttributeUpdatedChange {
		std::string AttributeName;
		std::optional<WireValue> Value;
	};
	struct ExtensionPropertyUpdatedChange {
		SchemaId ExtensionSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Value;
	};
	struct TagAddedChange { std::string TagName; };
	struct TagRemovedChange { std::string TagName; };
	struct ObjectReparentedChange { std::optional<ObjectId> Parent; };
	struct ObjectDestroyedChange {};
	using ChangePayload = std::variant<
		ObjectCreatedChange,
		PropertyUpdatedChange,
		AttributeUpdatedChange,
		ExtensionPropertyUpdatedChange,
		TagAddedChange,
		TagRemovedChange,
		ObjectReparentedChange,
		ObjectDestroyedChange
	>;

	struct ChangeRecord {
		std::uint64_t Sequence;
		ObjectId Scope;
		ObjectId Object;
		ChangePayload Payload;
	};

	struct ChangeCursor {
		ObjectId Scope;
		std::uint64_t NextSequence = 1;
	};
	enum class ChangeReadStatus { Available, ResnapshotRequired };
	struct ChangeReadResult {
		ChangeReadStatus Status = ChangeReadStatus::Available;
		ChangeCursor Cursor;
		std::vector<ChangeRecord> Records;
	};

	class ChangeJournal {
	  public:
		static ChangeJournal &Get();
		std::uint64_t Commit(ObjectId object, ChangePayload payload);
		std::uint64_t Commit(ObjectId scope, ObjectId object, ChangePayload payload);
		void CommitBatch(ObjectId scope, std::vector<std::pair<ObjectId, ChangePayload>> changes);
		[[nodiscard]] std::vector<ChangeRecord> ReadSince(std::uint64_t sequence) const;
		[[nodiscard]] ChangeCursor CreateCursor(ObjectId scope = {}) const;
		[[nodiscard]] ChangeReadResult Read(ChangeCursor cursor, std::size_t maximumRecords = std::numeric_limits<std::size_t>::max()) const;
		void SetCapacity(std::size_t capacity);
		[[nodiscard]] std::size_t GetCapacity() const;
		void Clear();

	  private:
		struct Stream {
			std::uint64_t NextSequence = 1;
			std::list<ChangeRecord> Records;
		};
		mutable std::mutex Mutex;
		std::size_t Capacity = 4096;
		std::unordered_map<ObjectId, Stream> Streams;
	};

	class ScopedChangeJournalSuppression {
	  private:
		friend class EditorHost;
		friend class InProcessReplicationSession;
		ScopedChangeJournalSuppression();
		~ScopedChangeJournalSuppression();
		ScopedChangeJournalSuppression(const ScopedChangeJournalSuppression &) = delete;
		ScopedChangeJournalSuppression &operator=(const ScopedChangeJournalSuppression &) = delete;
	};
}
