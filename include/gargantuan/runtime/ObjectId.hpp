#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <memory>

namespace gargantuan {
	class Instance;

	struct ObjectId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
		auto operator<=>(const ObjectId &) const = default;
	};

	class ObjectRegistry {
	  public:
		static ObjectRegistry &Get();

		ObjectId Register(const std::shared_ptr<Instance> &instance);
		void Invalidate(ObjectId id);
		[[nodiscard]] std::shared_ptr<Instance> Lookup(ObjectId id) const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;

		ObjectRegistry();
		~ObjectRegistry();
	};
}

template <> struct std::hash<gargantuan::ObjectId> {
	std::size_t operator()(const gargantuan::ObjectId &id) const noexcept {
		return (static_cast<std::size_t>(id.Generation) << 32) ^ id.Slot;
	}
};
