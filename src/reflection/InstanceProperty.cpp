#include "gargantuan/InstanceProperty.hpp"

#include "gargantuan/runtime/WireCodec.hpp"

#include <cmath>
#include <type_traits>

namespace gargantuan {
	namespace {
		bool InRange(double Value, const InstanceProperty::NumericRange &Range) {
			return std::isfinite(Value) && (!Range.Minimum || Value >= *Range.Minimum) &&
				(!Range.Maximum || Value <= *Range.Maximum);
		}

		bool WireValueInRange(const WireValue &Value, const InstanceProperty::NumericRange &Range) {
			return std::visit([&](const auto &Typed) {
				using ValueType = std::decay_t<decltype(Typed)>;
				if constexpr (std::is_same_v<ValueType, int> || std::is_same_v<ValueType, double>)
					return InRange(static_cast<double>(Typed), Range);
				else if constexpr (std::is_same_v<ValueType, WireFloat>) return InRange(Typed.Value, Range);
				else if constexpr (std::is_same_v<ValueType, WireVector2>)
					return InRange(Typed.X, Range) && InRange(Typed.Y, Range);
				else if constexpr (std::is_same_v<ValueType, WireVector3>)
					return InRange(Typed.X, Range) && InRange(Typed.Y, Range) && InRange(Typed.Z, Range);
				else if constexpr (std::is_same_v<ValueType, WireColor3>)
					return InRange(Typed.R, Range) && InRange(Typed.G, Range) && InRange(Typed.B, Range);
				else if constexpr (std::is_same_v<ValueType, WireUDim>)
					return InRange(Typed.Scale, Range) && InRange(Typed.Offset, Range);
				else if constexpr (std::is_same_v<ValueType, WireUDim2>)
					return InRange(Typed.X.Scale, Range) && InRange(Typed.X.Offset, Range) &&
						InRange(Typed.Y.Scale, Range) && InRange(Typed.Y.Offset, Range);
				else if constexpr (std::is_same_v<ValueType, WireCFrame>) {
					for (const auto Component : Typed.Components)
						if (!InRange(Component, Range)) return false;
					return true;
				} else return false;
			}, Value);
		}
	}

	bool InstanceProperty::IsValueValid(const std::any &value) const {
		if (Validate && !Validate(value)) return false;
		if (!Range) return true;
		auto Encoded = EncodeNativeWireValue(value);
		return Encoded && WireValueInRange(*Encoded, *Range);
	}
}
