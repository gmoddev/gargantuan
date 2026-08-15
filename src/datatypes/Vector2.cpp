#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/scripting/StackValue.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <cmath>
#include <lua.h>
#include <lualib.h>
#include <sstream>

namespace gargantuan {
	G_USERDATA_IMPL(
		Vector2,
		.Tag = UserdataTag::Vector2,
		.Type = "Vector2",
		.Properties =
			{
				{"X", Property::fromRead([](Vector2 *self) { return self->GetX(); })},
				{"Y", Property::fromRead([](Vector2 *self) { return self->GetY(); })},
				{"Unit", Property::fromRead([](Vector2 *self) { return self->GetUnit(); })},
				{"Magnitude", Property::fromRead([](Vector2 *self) { return self->GetMagnitude(); })},
			},
		.Methods = {
			{"Cross", Method::fromCheckedMember<&Vector2::Cross>()},
			{"Abs", Method::fromMember<&Vector2::Abs>()},
			{"Ceil", Method::fromMember<&Vector2::Ceil>()},
			{"Floor", Method::fromMember<&Vector2::Floor>()},
			{"Sign", Method::fromMember<&Vector2::Sign>()},
			{"Angle", Method{Vector2::LAngle}},
			{"Dot", Method::fromCheckedMember<&Vector2::Dot>()},
			{"FuzzyEq", Method{Vector2::LFuzzyEq}},
			{"Lerp", Method::fromCheckedMember<&Vector2::Lerp>()},
			{"Max", Method::fromCheckedMember<&Vector2::Max>()},
			{"Min", Method::fromCheckedMember<&Vector2::Min>()},
			{"__tostring", Method{Vector2::LTostring}},
			{"__add", Method{Vector2::LAdd}},
			{"__sub", Method{Vector2::LSub}},
			{"__mul", Method{.Call = Vector2::LMul, .AllowSecondArgumentReceiver = true}},
			{"__div", Method{Vector2::LDiv}},
			{"__unm", Method{Vector2::LUnm}},
			{"__eq", Method{Vector2::LEq}},
		}
	)

	Vector2::Vector2(float x, float y) : Value(x, y) {};
	Vector2::Vector2(glm::vec2 vec) : Value(vec) {};

	float Vector2::GetX() const {
		return Value.x;
	};

	float Vector2::GetY() const {
		return Value.y;
	};

	float Vector2::GetMagnitude() const {
		return glm::length(Value);
	};

	Vector2 Vector2::GetUnit() const {
		if (Value.x == 0 && Value.y == 0) return Vector2();
		return glm::normalize(Value);
	};

	float Vector2::Cross(const Vector2 &other) const {
		return (GetX() * other.GetY()) - (GetY() * other.GetX());
	};

	Vector2 Vector2::Abs() const {
		return glm::abs(Value);
	};

	Vector2 Vector2::Ceil() const {
		return glm::ceil(Value);
	};

	Vector2 Vector2::Floor() const {
		return glm::floor(Value);
	};

	Vector2 Vector2::Sign() const {
		return glm::sign(Value);
	};

	float Vector2::Angle(const Vector2 &other, bool isSigned) const {
		float angle = atan2(Cross(other), Dot(other));
		return isSigned ? angle : abs(angle);
	};

	float Vector2::Dot(const Vector2 &other) const {
		return GetX() * other.GetX() + GetY() * other.GetY();
	};

	Vector2 Vector2::Lerp(const Vector2 &goal, float alpha) const {
		return Value + (goal.Value - Value) * alpha;
	};

	Vector2 Vector2::Max(const Vector2 &other) const {
		return glm::max(Value, other.Value);
	};

	Vector2 Vector2::Min(const Vector2 &other) const {
		return glm::min(Value, other.Value);
	};

	bool Vector2::FuzzyEq(const Vector2 &other, float epsilon) const {
		return glm::abs(Value.x - other.Value.x) <= epsilon && glm::abs(Value.y - other.Value.y) <= epsilon;
	};

	Vector2 Vector2::Unm() const {
		return -Value;
	};

	int Vector2::LTostring(lua_State *L, Vector2 *self) {
		std::ostringstream ss;
		ss << self->GetX() << ", " << self->GetY();
		std::string str = ss.str();
		lua_pushlstring(L, str.c_str(), str.size());
		return 1;
	}

	int Vector2::LAdd(lua_State *L, Vector2 *self) {
		static_cast<void>(self);
		const Vector2 left = CheckStackValue<Vector2>(L, 1);
		const Vector2 right = CheckStackValue<Vector2>(L, 2);
		StackValue<Vector2>::Push(L, left + right);
		return 1;
	}

	int Vector2::LSub(lua_State *L, Vector2 *self) {
		static_cast<void>(self);
		const Vector2 left = CheckStackValue<Vector2>(L, 1);
		const Vector2 right = CheckStackValue<Vector2>(L, 2);
		StackValue<Vector2>::Push(L, left - right);
		return 1;
	}

	int Vector2::LMul(lua_State *L, Vector2 *self) {
		static_cast<void>(self);
		if (StackValue<Vector2>::Is(L, 1)) {
			const Vector2 left = StackValue<Vector2>::From(L, 1);
			if (StackValue<Vector2>::Is(L, 2))
				return StackValue<Vector2>::Push(L, left * StackValue<Vector2>::From(L, 2));
			if (lua_isnumber(L, 2))
				return StackValue<Vector2>::Push(L, left * static_cast<float>(lua_tonumber(L, 2)));
			luaL_typeerror(L, 2, "Vector2 or number");
			return 0;
		}
		if (lua_isnumber(L, 1) && StackValue<Vector2>::Is(L, 2))
			return StackValue<Vector2>::Push(
				L, StackValue<Vector2>::From(L, 2) * static_cast<float>(lua_tonumber(L, 1))
			);
		luaL_typeerror(L, 1, "Vector2 or number");
		return 0;
	}

	int Vector2::LDiv(lua_State *L, Vector2 *self) {
		static_cast<void>(self);
		const Vector2 left = CheckStackValue<Vector2>(L, 1);
		if (StackValue<Vector2>::Is(L, 2))
			return StackValue<Vector2>::Push(L, left / StackValue<Vector2>::From(L, 2));
		if (lua_isnumber(L, 2))
			return StackValue<Vector2>::Push(L, left / static_cast<float>(lua_tonumber(L, 2)));
		luaL_typeerror(L, 2, "Vector2 or number");
		return 0;
	}

	int Vector2::LUnm(lua_State *L, Vector2 *self) {
		static_cast<void>(self);
		return StackValue<Vector2>::Push(L, CheckStackValue<Vector2>(L, 1).Unm());
	}

	int Vector2::LAngle(lua_State *L, Vector2 *self) {
		const Vector2 other = CheckStackValue<Vector2>(L, 2);
		const bool isSigned = luaL_optboolean(L, 3, false);
		lua_pushnumber(L, self->Angle(other, isSigned));
		return 1;
	}

	int Vector2::LFuzzyEq(lua_State *L, Vector2 *self) {
		const Vector2 other = CheckStackValue<Vector2>(L, 2);
		const float epsilon = luaL_optnumber(L, 3, 1e-5f);
		if (!std::isfinite(epsilon) || epsilon < 0.0f) {
			luaL_argerror(L, 3, "epsilon must be finite and non-negative");
			return 0;
		}
		lua_pushboolean(L, self->FuzzyEq(other, epsilon));
		return 1;
	}

	int Vector2::LEq(lua_State *L, Vector2 *self) {
		if (StackValue<Vector2>::Is(L, 2)) {
			Vector2 other = StackValue<Vector2>::From(L, 2);
			// ISO C++20 considers use of overloaded operator '==' (with operand
			// types 'Vector2' and 'Vector2') to be ambiguous despite there
			// being a unique best viable function
			// ^ ???????????? fuck u mean
			// lua_pushboolean(L, *self == other);
			lua_pushboolean(L, self->Value.x == other.Value.x && self->Value.y == other.Value.y);
		} else {
			lua_pushboolean(L, false);
		}
		return 1;
	}

}
