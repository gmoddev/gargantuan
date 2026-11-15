// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>

namespace {
	constexpr double PI = std::numbers::pi;
	constexpr int AZIMUTH_STEPS = 256;
	constexpr int ELEVATION_STEPS = 128;
	int Failures = 0;

	void Check(bool Passed, const char *Message) {
		if (Passed) return;
		std::cerr << "[Render:IBLReference] FAIL: " << Message << '\n';
		++Failures;
	}

	struct FaceCoordinate {
		int Face;
		glm::dvec2 Uv;
	};

	FaceCoordinate SelectFace(glm::dvec3 Direction) {
		const auto Axis = glm::abs(Direction);
		glm::dvec2 Plane;
		int Face;
		if (Axis.x >= Axis.y && Axis.x >= Axis.z) {
			Face = Direction.x > 0.0 ? 0 : 1;
			Plane = glm::dvec2(Face == 0 ? -Direction.z : Direction.z, -Direction.y) / Axis.x;
		} else if (Axis.y >= Axis.z) {
			Face = Direction.y > 0.0 ? 2 : 3;
			Plane = glm::dvec2(Direction.x, Face == 2 ? Direction.z : -Direction.z) / Axis.y;
		} else {
			Face = Direction.z > 0.0 ? 4 : 5;
			Plane = glm::dvec2(Face == 4 ? Direction.x : -Direction.x, -Direction.y) / Axis.z;
		}
		return {Face, Plane * 0.5 + 0.5};
	}

	double DecodeSrgb(double Value) {
		return Value <= 0.04045 ? Value / 12.92 : std::pow((Value + 0.055) / 1.055, 2.4);
	}

	// Integrate in direction space with uniform solid-angle cells. The renderer
	// instead samples a cosine or GGX distribution, so these calculations can
	// distinguish a PDF, cosine, normalization, or visibility-weight error.
	template <typename Integrand> auto Hemisphere(Integrand Evaluate) {
		using Value = decltype(Evaluate(glm::dvec3{}));
		Value Sum(0.0);
		for (int Row = 0; Row < ELEVATION_STEPS; ++Row) {
			const auto Z = (Row + 0.5) / ELEVATION_STEPS;
			const auto Radius = std::sqrt(1.0 - Z * Z);
			for (int Column = 0; Column < AZIMUTH_STEPS; ++Column) {
				const auto Phi = (Column + 0.5) * (2.0 * PI / AZIMUTH_STEPS);
				Sum += Evaluate(glm::dvec3{Radius * std::cos(Phi), Radius * std::sin(Phi), Z});
			}
		}
		return Sum * (2.0 * PI / (AZIMUTH_STEPS * ELEVATION_STEPS));
	}

	double Distribution(double NoH, double AlphaSquared) {
		const auto Term = 1.0 + (AlphaSquared - 1.0) * NoH * NoH;
		return AlphaSquared / (PI * Term * Term);
	}

	template <typename Sky> double Lambert(Sky Radiance) {
		return Hemisphere([&](glm::dvec3 L) { return Radiance(L) * L.z / PI; });
	}

	template <typename Sky> double Prefilter(double Roughness, Sky Radiance) {
		constexpr glm::dvec3 Normal{0.0, 0.0, 1.0};
		if (Roughness == 0.0) return Radiance(Normal);
		const auto AlphaSquared = std::pow(Roughness, 4.0);
		const auto Integrated = Hemisphere([&](glm::dvec3 L) {
			const auto H = glm::normalize(Normal + L);
			// NoH and VoH cancel for this prefilter's N = V = R frame.
			const auto Weight = Distribution(H.z, AlphaSquared) * L.z * 0.25;
			return glm::dvec2(Radiance(L) * Weight, Weight);
		});
		return Integrated.x / Integrated.y;
	}

	glm::dvec2 IntegrateBrdf(double NoV, double Roughness) {
		const glm::dvec3 V{std::sqrt(1.0 - NoV * NoV), 0.0, NoV};
		const auto AlphaSquared = std::pow(Roughness, 4.0);
		return Hemisphere([&](glm::dvec3 L) {
			const auto H = glm::normalize(V + L);
			const auto SmithV = L.z * std::sqrt(NoV * NoV * (1.0 - AlphaSquared) + AlphaSquared);
			const auto SmithL = NoV * std::sqrt(L.z * L.z * (1.0 - AlphaSquared) + AlphaSquared);
			const auto Fresnel = std::pow(1.0 - glm::dot(V, H), 5.0);
			return glm::dvec2(1.0 - Fresnel, Fresnel) * (Distribution(H.z, AlphaSquared) * L.z * 0.5 / (SmithV + SmithL));
		});
	}

	void TestFaceOrientationAndSrgb() {
		const std::array<glm::dvec3, 6> Directions{{
			{1.0, 0.5, -0.25}, {-1.0, 0.5, 0.25}, {0.25, 1.0, -0.5},
			{0.25, -1.0, 0.5}, {0.25, 0.5, 1.0}, {-0.25, 0.5, -1.0},
		}};
		for (std::size_t Face = 0; Face < Directions.size(); ++Face) {
			const auto Coordinates = SelectFace(Directions[Face]);
			Check(Coordinates.Face == static_cast<int>(Face) && Coordinates.Uv == glm::dvec2(0.625, 0.25),
				"published face order and off-center UV orientation");
		}
		Check(std::abs(DecodeSrgb(0.5) - 0.21404114048223255) < 1e-12, "exact sRGB transfer at midgray");
		const auto LinearBlackWhiteMidpoint = (DecodeSrgb(0.0) + DecodeSrgb(1.0)) * 0.5;
		Check(LinearBlackWhiteMidpoint == 0.5 && LinearBlackWhiteMidpoint > DecodeSrgb(0.5),
			"filtering decoded black and white preserves linear energy");
	}

	void TestConvolutionAndBrdf() {
		const auto White = [](glm::dvec3) { return 1.0; };
		const auto PolarLight = [](glm::dvec3 L) { return L.z > 0.9 ? 1.0 : 0.0; };
		const auto OppositeLight = [](glm::dvec3 L) { return L.z < -0.9 ? 1.0 : 0.0; };
		Check(std::abs(Lambert(White) - 1.0) < 1e-12, "constant radiance stays constant after normalized Lambert convolution");
		Check(Lambert(PolarLight) > 0.17 && Lambert(PolarLight) < 0.20 && Lambert(OppositeLight) == 0.0,
			"Lambert irradiance conserves directional cosine weighting and excludes the opposite hemisphere");
		Check(Prefilter(0.0, PolarLight) == 1.0 && Prefilter(0.5, PolarLight) > Prefilter(1.0, PolarLight) &&
			Prefilter(1.0, PolarLight) < 0.21, "GGX roughness broadens and dims the central reflection lobe");
		Check(std::abs(Prefilter(1.0, White) - 1.0) < 1e-12, "GGX radiance preserves a constant white environment");
		const auto RoughNormal = IntegrateBrdf(1.0, 1.0);
		Check(std::abs(RoughNormal.x + RoughNormal.y - (1.0 - std::log(2.0))) < 3e-6,
			"fully rough normal-view white-furnace specular agrees with the analytic integral");
		const auto Grazing = IntegrateBrdf(0.2, 0.5);
		Check(Grazing.x >= 0.0 && Grazing.y > RoughNormal.y && Grazing.x + Grazing.y < 1.0,
			"the grazing split sum preserves Fresnel increase and bounded single-scatter energy");
	}
}

int main() {
	TestFaceOrientationAndSrgb();
	TestConvolutionAndBrdf();
	if (Failures != 0) return 1;
	std::cout << "[Render:IBLReference] Orientation, linear energy, GGX convolution and BRDF reference passed\n";
	return 0;
}
