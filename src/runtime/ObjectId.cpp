#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/classes/Instance.hpp"

#include <mutex>
#include <vector>

namespace gargantuan {
	struct ObjectRegistry::Impl {
		struct Entry {
			std::weak_ptr<Instance> Object;
			std::uint32_t Generation = 1;
		};

		mutable std::mutex Mutex;
		std::vector<Entry> Entries{{{}, 0}};
		std::vector<std::uint32_t> FreeSlots;
	};

	ObjectRegistry::ObjectRegistry() : State(std::make_unique<Impl>()) {}
	ObjectRegistry::~ObjectRegistry() = default;

	ObjectRegistry &ObjectRegistry::Get() {
		static ObjectRegistry Registry;
		return Registry;
	}

	ObjectId ObjectRegistry::Register(const std::shared_ptr<Instance> &instance) {
		std::scoped_lock lock(State->Mutex);
		std::uint32_t slot;
		if (State->FreeSlots.empty()) {
			slot = static_cast<std::uint32_t>(State->Entries.size());
			State->Entries.push_back({instance, 1});
		} else {
			slot = State->FreeSlots.back();
			State->FreeSlots.pop_back();
			State->Entries[slot].Object = instance;
		}
		return {slot, State->Entries[slot].Generation};
	}

	void ObjectRegistry::Invalidate(ObjectId id) {
		std::scoped_lock lock(State->Mutex);
		if (!id.IsValid() || id.Slot >= State->Entries.size()) return;
		auto &entry = State->Entries[id.Slot];
		if (entry.Generation != id.Generation) return;
		entry.Object.reset();
		if (++entry.Generation == 0) entry.Generation = 1;
		State->FreeSlots.push_back(id.Slot);
	}

	std::shared_ptr<Instance> ObjectRegistry::Lookup(ObjectId id) const {
		std::scoped_lock lock(State->Mutex);
		if (!id.IsValid() || id.Slot >= State->Entries.size()) return nullptr;
		const auto &entry = State->Entries[id.Slot];
		if (entry.Generation != id.Generation) return nullptr;
		return entry.Object.lock();
	}
}
