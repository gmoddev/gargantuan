// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace gargantuan::blockgrid {
	enum class Shape : std::uint8_t { Cube, Slab, Wedge, Stairs };
	enum class PieceKind : std::uint8_t { Box, Wedge };
	using Point = std::array<float,3>;
	using RotationBasis = std::array<Point,3>;
	using FacePair = std::array<unsigned,2>;
	// Face indices are +X,-X,+Y,-Y,+Z,-Z. Basis columns are right, up and positive Z.
	[[nodiscard]] FacePair OrientationFaces(std::uint8_t Orientation);
	[[nodiscard]] std::uint8_t OrientationIndex(unsigned Up,unsigned Front);
	[[nodiscard]] const RotationBasis& Basis(std::uint8_t Orientation);
	[[nodiscard]] unsigned RotateFace(unsigned LocalFace,std::uint8_t Orientation);
	struct Face {
		std::array<Point,4> Corners{};
		Point Normal{};
		unsigned Count{};
		int Boundary{-1};
	};
	struct Piece {
		PieceKind Kind{};
		Point Center{}, Size{};
		std::uint8_t Orientation{};
	};
	struct Geometry {
		std::span<const Face> Faces;
		std::uint8_t FullFaces{};
		std::span<const Piece> Collision;
	};
	// Immutable process-lifetime geometry in cell coordinates [0,1], rotated about its center.
	// Box3D's local wedge rises toward -Z. Piece.Orientation already includes this conversion.
	[[nodiscard]] const Geometry& GetGeometry(Shape Value,std::uint8_t Orientation);
	struct Definition {
		std::string Key;
		Shape Geometry{Shape::Cube};
		Point Color{1,1,1};
		bool CanCollide{true}, Queryable{true};
		std::uint8_t Ports{63};
		bool operator==(const Definition&) const = default;
	};
}
