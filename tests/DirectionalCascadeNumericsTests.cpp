#include "render/sdl/SDLDirectionalCascadePlan.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <random>
#include <stdexcept>

namespace {
	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void CheckCorners(const gargantuan::RenderCameraSnapshot &Camera,
		const gargantuan::SDLDirectionalCascadePlan &Plan, double Aspect = 16.0 / 9.0) {
		const double Tangent = std::tan(glm::radians(double(Camera.VerticalFieldOfView)) * 0.5);
		for (std::uint32_t Index = 0; Index < Plan.Count; ++Index) {
			const auto &Cascade = Plan.Cascades[Index];
			const float CoverageNear = Index == 0 ? Cascade.Near : Plan.Cascades[Index - 1].TransitionStart;
			for (const float Depth : {CoverageNear, Cascade.Far})
				for (const float X : {-1.0f, 1.0f}) for (const float Y : {-1.0f, 1.0f}) {
					const glm::dvec3 World = glm::dvec3(Camera.Position) +
						glm::dvec3(Camera.LookDirection) * double(Depth) +
						glm::dvec3(Camera.RightDirection) * (X * Depth * Tangent * Aspect) +
						glm::dvec3(Camera.UpDirection) * (Y * Depth * Tangent);
					const auto Clip = glm::dmat4(Cascade.Matrix) *
						glm::dvec4(World - glm::dvec3(Cascade.Origin), 1.0);
					Require(std::abs(Clip.x) <= 1.01 && std::abs(Clip.y) <= 1.01,
						"frustum corner escaped snapped cascade");
				}
		}
	}

	void CheckLightBasis(const glm::dvec3 &Direction, double Tolerance = 1.0e-10) {
		const auto Basis = gargantuan::MakeSDLDirectionalLightBasis(Direction);
		Require(std::abs(glm::length(Basis.X) - 1.0) < Tolerance &&
			std::abs(glm::length(Basis.Y) - 1.0) < Tolerance &&
			std::abs(glm::dot(Basis.X, Basis.Y)) < Tolerance &&
			std::abs(glm::dot(Basis.X, Direction)) < Tolerance &&
			std::abs(glm::dot(Basis.Y, Direction)) < Tolerance &&
			glm::dot(glm::cross(Basis.X, Basis.Y), Direction) > 1.0 - Tolerance,
			"directional light basis lost right-handed orthonormality");
	}

	glm::vec3 CanonicalSun(float ClockTime) {
		const float Angle = (ClockTime - 6.0f) * (2.0f * std::numbers::pi_v<float> / 24.0f);
		return glm::normalize(glm::vec3(std::cos(Angle), std::sin(Angle), 0.0f));
	}
}

int main() try {
	using namespace gargantuan;
	RenderCameraSnapshot Camera;
	Camera.Position = {0.0f, 3.0f, 0.0f};
	const SDLDirectionalCascadeSettings Settings{.Count = 4, .Resolution = 1024,
		.MaximumDistance = 250.0f, .SplitLambda = 0.6f, .TransitionFraction = 0.1f};
	const auto Sun = glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f));
	const auto Plan = MakeSDLDirectionalCascadePlan(Camera, Sun, 16.0f / 9.0f, Settings);
	Require(Plan && Plan->Count == 4, "cascade plan failed");
	float Previous = Camera.NearPlane;
	for (const auto &Cascade : Plan->Cascades) {
		Require(Cascade.Near >= Previous && Cascade.Far > Cascade.Near &&
			Cascade.TransitionStart > Cascade.Near && Cascade.TransitionStart < Cascade.Far,
			"split or transition interval invalid");
		Require(Cascade.WorldUnitsPerTexel > 0 && Cascade.DepthSpan * 0.0005f < 0.6f,
			"texel or bias units invalid");
		Previous = Cascade.Far;
	}
	Require(std::abs(Previous - 250.0f) < 1e-4f, "far coverage cap failed");
	CheckCorners(Camera, *Plan);
	auto SmallMove = Camera;
	SmallMove.Position.x += Plan->Cascades[0].WorldUnitsPerTexel * 0.1f;
	const auto Stable = MakeSDLDirectionalCascadePlan(SmallMove, Sun, 16.0f / 9.0f, Settings);
	Require(Stable && Stable->Cascades[0].Matrix == Plan->Cascades[0].Matrix,
		"subtexel movement changed the matrix");
	std::uint32_t MatrixChanges = 0;
	auto LastMatrix = Plan->Cascades[0].Matrix;
	for (std::uint32_t Step = 1; Step <= 200; ++Step) {
		auto Moving = Camera;
		Moving.Position.x += Step * Plan->Cascades[0].WorldUnitsPerTexel * 0.02f;
		const auto Next = MakeSDLDirectionalCascadePlan(Moving, Sun, 16.0f / 9.0f, Settings);
		Require(Next.has_value(), "slow motion lost a cascade plan");
		if (Next->Cascades[0].Matrix != LastMatrix) ++MatrixChanges;
		LastMatrix = Next->Cascades[0].Matrix;
	}
	Require(MatrixChanges < 20, "slow subtexel movement rebuilt the cascade too often");
	Camera.Position = {900000.0f, 2000.0f, -900000.0f};
	const auto Distant = MakeSDLDirectionalCascadePlan(Camera, Sun, 16.0f / 9.0f, Settings);
	Require(Distant.has_value(), "large offset failed");
	CheckCorners(Camera, *Distant);
	Camera.LookDirection = glm::normalize(glm::vec3(0.4f, -0.1f, -1.0f));
	Camera.RightDirection = glm::normalize(glm::cross(Camera.LookDirection, glm::vec3(0, 1, 0)));
	Camera.UpDirection = glm::normalize(glm::cross(Camera.RightDirection, Camera.LookDirection));
	const auto Rotated = MakeSDLDirectionalCascadePlan(Camera, Sun, 16.0f / 9.0f, Settings);
	Require(Rotated.has_value(), "rotated camera failed");
	CheckCorners(Camera, *Rotated);
	const auto LightPlaneX = [](const glm::mat4 &Matrix) {
		return glm::normalize(glm::vec3(Matrix[0][0], Matrix[1][0], Matrix[2][0]));
	};
	for (const auto Hours : {std::array{11.99f, 12.0f, 12.01f},
		std::array{23.99f, 0.0f, 0.01f}}) {
		std::optional<SDLDirectionalCascadePlan> PreviousPlan;
		for (const float Hour : Hours) {
			const auto SunAtHour = CanonicalSun(Hour);
			CheckLightBasis(glm::normalize(glm::dvec3(SunAtHour)));
			const auto CurrentPlan = MakeSDLDirectionalCascadePlan(Camera, SunAtHour,
				16.0f / 9.0f, Settings);
			Require(CurrentPlan.has_value(), "canonical zenith lost a cascade plan");
			CheckCorners(Camera, *CurrentPlan);
			if (PreviousPlan)
				Require(glm::dot(LightPlaneX(PreviousPlan->Cascades[0].Matrix),
					LightPlaneX(CurrentPlan->Cascades[0].Matrix)) > 0.9999f,
					"canonical zenith rotated the cascade light plane abruptly");
			PreviousPlan = CurrentPlan;
		}
	}
	std::optional<SDLDirectionalLightBasis> PreviousBasis;
	for (std::uint32_t Minute = 0; Minute <= 24 * 60; ++Minute) {
		const auto Direction = glm::normalize(glm::dvec3(CanonicalSun((Minute % (24 * 60)) / 60.0f)));
		CheckLightBasis(Direction);
		const auto CurrentBasis = MakeSDLDirectionalLightBasis(Direction);
		if (PreviousBasis)
			Require(glm::dot(PreviousBasis->X, CurrentBasis.X) > 0.9999,
				"canonical 24-hour sun arc contains a light-plane basis jump");
		PreviousBasis = CurrentBasis;
	}
	for (const glm::dvec3 Direction : {glm::dvec3(1e-7, 1, 0), glm::dvec3(-1e-7, 1, 0),
		glm::dvec3(1e-7, -1, 0), glm::dvec3(-1e-7, -1, 0),
		glm::dvec3(1e-7, 2e-7, -1), glm::dvec3(0, 0, -1)})
		CheckLightBasis(glm::normalize(Direction));
	for (const glm::vec3 Pole : {glm::vec3(1e-7f, 2e-7f, -1), glm::vec3(0, 0, -1)}) {
		const auto PolePlan = MakeSDLDirectionalCascadePlan(Camera, glm::normalize(Pole),
			16.0f / 9.0f, Settings);
		Require(PolePlan.has_value(), "opposite-pole sun lost a cascade plan");
		CheckCorners(Camera, *PolePlan);
	}
	std::mt19937 Random(0xC45CADEu);
	std::uniform_real_distribution<float> Unit(-1.0f, 1.0f);
	std::uniform_real_distribution<float> Coordinate(-900000.0f, 900000.0f);
	std::uniform_real_distribution<float> FieldOfView(25.0f, 120.0f);
	std::uniform_real_distribution<float> AspectRatio(0.7f, 2.5f);
	for (std::uint32_t Trial = 0; Trial < 1024; ++Trial) {
		RenderCameraSnapshot Fuzz;
		Fuzz.Position = {Coordinate(Random), Coordinate(Random), Coordinate(Random)};
		Fuzz.VerticalFieldOfView = FieldOfView(Random);
		Fuzz.LookDirection = glm::normalize(glm::vec3(Unit(Random), Unit(Random) * 0.7f, -1.0f));
		Fuzz.RightDirection = glm::normalize(glm::cross(Fuzz.LookDirection, glm::vec3(0, 1, 0)));
		Fuzz.UpDirection = glm::normalize(glm::cross(Fuzz.RightDirection, Fuzz.LookDirection));
		const auto FuzzSun = glm::normalize(glm::vec3(Unit(Random), Unit(Random), Unit(Random) + 0.01f));
		CheckLightBasis(glm::normalize(glm::dvec3(FuzzSun)));
		const auto FuzzAspect = AspectRatio(Random);
		const auto FuzzPlan = MakeSDLDirectionalCascadePlan(Fuzz, FuzzSun, FuzzAspect, Settings);
		Require(FuzzPlan.has_value(), "finite randomized camera lost cascade plan");
		CheckCorners(Fuzz, *FuzzPlan, FuzzAspect);
	}
	Require(!MakeSDLDirectionalCascadePlan(Camera, {0, 0, 0}, 1.0f, Settings),
		"degenerate sun direction admitted");
	std::cout << "[Render:ShadowCascade] Numerical geometry passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:ShadowCascade] " << Error.what() << '\n';
	return 1;
}
