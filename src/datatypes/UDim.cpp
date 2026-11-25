#include "gargantuan/datatypes/UDim.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"
#include <common.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace gargantuan {
	G_USERDATA_IMPL(
		UDim,
		.Tag = UserdataTag::UDim,
		.Type = "UDim",
		.Properties =
			{
				{"Scale", Property::fromReadonlyMember<&UDim::Scale>()},
				{"Offset", Property::fromReadonlyMember<&UDim::Offset>()},
			},
		.Methods = {
			{"Lerp", Method::fromMember<&UDim::Lerp>()},
			{"__add", Method::fromMember<&UDim::Add>()},
			{"__sub", Method::fromMember<&UDim::Sub>()},
			{"__unm", Method::fromMember<&UDim::Unm>()},
			{"__eq", Method::fromMember<&UDim::Eq>()},
			{"__tostring", Method::fromMember<&UDim::Tostring>()},
		}
	);

	UDim::UDim(float scale, int offset) : Scale(scale), Offset(offset) {};

	UDim UDim::Lerp(const UDim &goal, float alpha) const {
		if (!std::isfinite(alpha)) throw std::invalid_argument("UDim:Lerp alpha must be finite");
		const double Rounded = std::round(std::lerp(static_cast<double>(Offset), static_cast<double>(goal.Offset), alpha));
		const double Bounded = std::clamp(Rounded, static_cast<double>(std::numeric_limits<int>::min()),
			static_cast<double>(std::numeric_limits<int>::max()));
		return UDim(std::lerp(Scale, goal.Scale, alpha), static_cast<int>(Bounded));
	}

	UDim UDim::Add(const UDim &other) const {
		return UDim(Scale + other.Scale, Offset + other.Offset);
	}

	UDim UDim::Sub(const UDim &other) const {
		return UDim(Scale - other.Scale, Offset - other.Offset);
	}

	UDim UDim::Unm() const {
		return UDim(-Scale, -Offset);
	}

	bool UDim::Eq(const UDim &other) const {
		return Scale == other.Scale && Offset == other.Offset;
	}

	std::string UDim::Tostring() const {
		std::ostringstream ss;
		ss << Scale << ", " << Offset;
		return ss.str();
	}
} // namespace gargantuan
