#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <sstream>

namespace gargantuan {
	G_USERDATA_IMPL(
		UDim2,
		.Tag = UserdataTag::UDim2,
		.Type = "UDim2",
		.Properties = {
			{"X", Property::fromReadonlyMember<&UDim2::X>().SetSerializable()},
			{"Y", Property::fromReadonlyMember<&UDim2::Y>().SetSerializable()},
			{"Width", Property::fromReadonlyMember<&UDim2::X>()},
			{"Height", Property::fromReadonlyMember<&UDim2::Y>()},
		}
	);

	UDim2::UDim2(UDim x, UDim y) : X(x), Y(y) {};
	UDim2::UDim2(float xScale, int xOffset, float yScale, int yOffset) : X(xScale, xOffset), Y(yScale, yOffset) {};
	UDim2 UDim2::fromScale(float x, float y) { return {x, 0, y, 0}; }
	UDim2 UDim2::fromOffset(int x, int y) { return {0.0f, x, 0.0f, y}; }

	UDim2 UDim2::Lerp(const UDim2 &goal, float alpha) const {
		return UDim2(X.Lerp(goal.X, alpha), Y.Lerp(goal.Y, alpha));
	}

	UDim2 UDim2::Add(const UDim2 &other) const {
		return UDim2(X.Add(other.X), Y.Add(other.Y));
	}

	UDim2 UDim2::Sub(const UDim2 &other) const {
		return UDim2(X.Sub(other.X), Y.Sub(other.Y));
	}

	UDim2 UDim2::Unm() const {
		return UDim2(X.Unm(), Y.Unm());
	}

	bool UDim2::Eq(const UDim2 &other) const {
		return X.Eq(other.X) && Y.Eq(other.Y);
	}

	std::string UDim2::Tostring() const {
		std::ostringstream ss;
		ss << "{" << X.Tostring() << "}, {" << Y.Tostring() << "}";
		return ss.str();
	}
}
