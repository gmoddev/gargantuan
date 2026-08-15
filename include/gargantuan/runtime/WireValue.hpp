#pragma once

#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/reflection/SchemaId.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace gargantuan {
	struct WireObjectId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;
		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
		[[nodiscard]] static WireObjectId FromObjectId(ObjectId id) { return {id.Slot, id.Generation}; }
		[[nodiscard]] ObjectId ToObjectId() const { return {Slot, Generation}; }
		auto operator<=>(const WireObjectId &) const = default;
	};

	struct WireFloat { float Value; auto operator<=>(const WireFloat &) const = default; };
	struct WireVector2 { float X; float Y; auto operator<=>(const WireVector2 &) const = default; };
	struct WireVector3 { float X; float Y; float Z; auto operator<=>(const WireVector3 &) const = default; };
	struct WireColor3 { float R; float G; float B; auto operator<=>(const WireColor3 &) const = default; };
	struct WireUDim { float Scale; int Offset; auto operator<=>(const WireUDim &) const = default; };
	struct WireUDim2 { WireUDim X; WireUDim Y; auto operator<=>(const WireUDim2 &) const = default; };
	struct WireCFrame { std::array<float, 12> Components; auto operator<=>(const WireCFrame &) const = default; };
	struct WireEnumItem { std::string EnumType; std::string Item; auto operator<=>(const WireEnumItem &) const = default; };
	struct WireSchemaEnumValue {
		SchemaId EnumSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::int32_t ItemValue = 0;
		auto operator<=>(const WireSchemaEnumValue &) const = default;
	};
	struct WireObjectReference { WireObjectId Object; auto operator<=>(const WireObjectReference &) const = default; };

	using WireValue = std::variant<
		std::monostate,
		bool,
		int,
		double,
		WireFloat,
		std::string,
		WireVector2,
		WireVector3,
		WireColor3,
		WireUDim,
		WireUDim2,
		WireCFrame,
		WireEnumItem,
		WireSchemaEnumValue,
		WireObjectReference
	>;
}

template <> struct std::hash<gargantuan::WireObjectId> {
	std::size_t operator()(const gargantuan::WireObjectId &id) const noexcept {
		const auto Combined = (static_cast<std::uint64_t>(id.Generation) << 32) | id.Slot;
		return std::hash<std::uint64_t>{}(Combined);
	}
};
