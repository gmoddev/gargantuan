#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace gargantuan {
	struct ObjectCreatedChange { std::string ClassName; };
	struct PropertyUpdatedChange { std::string PropertyName; };
	struct ObjectReparentedChange { std::optional<ObjectId> Parent; };
	struct ObjectDestroyedChange {};
	using ChangePayload = std::variant<ObjectCreatedChange, PropertyUpdatedChange, ObjectReparentedChange, ObjectDestroyedChange>;

	struct ChangeRecord {
		std::uint64_t Sequence;
		ObjectId Object;
		ChangePayload Payload;
	};

	class ChangeJournal {
	  public:
		static ChangeJournal &Get();
		std::uint64_t Commit(ObjectId object, ChangePayload payload);
		[[nodiscard]] std::vector<ChangeRecord> ReadSince(std::uint64_t sequence) const;
		void Clear();

	  private:
		mutable std::mutex Mutex;
		std::uint64_t NextSequence = 1;
		std::vector<ChangeRecord> Records;
	};
}
