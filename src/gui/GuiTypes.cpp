// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/gui/GuiTypes.hpp"

#include <algorithm>
#include <cmath>

namespace gargantuan {
	bool GuiRect::Contains(float PointX, float PointY) const {
		return PointX >= X && PointY >= Y && PointX <= X + Width && PointY <= Y + Height;
	}

	std::optional<GuiRect> GuiRect::Intersect(const GuiRect &Left, const GuiRect &Right) {
		const float X = std::max(Left.X, Right.X);
		const float Y = std::max(Left.Y, Right.Y);
		const float RightEdge = std::min(Left.X + Left.Width, Right.X + Right.Width);
		const float BottomEdge = std::min(Left.Y + Left.Height, Right.Y + Right.Height);
		if (RightEdge <= X || BottomEdge <= Y) return std::nullopt;
		return GuiRect{X, Y, RightEdge - X, BottomEdge - Y};
	}

	Vector2 GuiTransform::Apply(Vector2 Point) const {
		return Vector2(
			M00 * Point.GetX() + M01 * Point.GetY() + Tx,
			M10 * Point.GetX() + M11 * Point.GetY() + Ty
		);
	}

	std::optional<GuiTransform> GuiTransform::Inverse() const {
		const float Determinant = M00 * M11 - M01 * M10;
		if (!std::isfinite(Determinant) || std::abs(Determinant) < 1e-7f) return std::nullopt;
		const float InverseDeterminant = 1.0f / Determinant;
		GuiTransform Result;
		Result.M00 = M11 * InverseDeterminant;
		Result.M01 = -M01 * InverseDeterminant;
		Result.M10 = -M10 * InverseDeterminant;
		Result.M11 = M00 * InverseDeterminant;
		Result.Tx = -(Result.M00 * Tx + Result.M01 * Ty);
		Result.Ty = -(Result.M10 * Tx + Result.M11 * Ty);
		return Result;
	}

	GuiTransform GuiTransform::Then(const GuiTransform &Child) const {
		return {
			M00 * Child.M00 + M01 * Child.M10,
			M00 * Child.M01 + M01 * Child.M11,
			M10 * Child.M00 + M11 * Child.M10,
			M10 * Child.M01 + M11 * Child.M11,
			M00 * Child.Tx + M01 * Child.Ty + Tx,
			M10 * Child.Tx + M11 * Child.Ty + Ty,
		};
	}

	GuiTransform GuiTransform::RotationAbout(float Degrees, float CenterX, float CenterY) {
		const float Radians = Degrees * 0.01745329251994329577f;
		const float Cosine = std::cos(Radians);
		const float Sine = std::sin(Radians);
		return {Cosine, -Sine, Sine, Cosine,
			CenterX - Cosine * CenterX + Sine * CenterY,
			CenterY - Sine * CenterX - Cosine * CenterY};
	}

	bool GuiViewportConfiguration::IsValid() const {
		return PhysicalWidth > 0 && PhysicalHeight > 0 && std::isfinite(DpiScale) && DpiScale > 0.0f &&
			std::isfinite(SafeArea.Left) && std::isfinite(SafeArea.Top) && std::isfinite(SafeArea.Right) &&
			std::isfinite(SafeArea.Bottom) && SafeArea.Left >= 0.0f && SafeArea.Top >= 0.0f &&
			SafeArea.Right >= 0.0f && SafeArea.Bottom >= 0.0f &&
			SafeArea.Left + SafeArea.Right < LogicalWidth() && SafeArea.Top + SafeArea.Bottom < LogicalHeight();
	}
}
