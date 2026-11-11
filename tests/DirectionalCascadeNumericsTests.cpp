#include "render/sdl/SDLDirectionalCascadePlan.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void CheckCorners(const gargantuan::RenderCameraSnapshot &Camera,
		const gargantuan::SDLDirectionalCascadePlan &Plan) {
		const double Tangent = std::tan(glm::radians(double(Camera.VerticalFieldOfView)) * 0.5);
		for (const auto &Cascade : Plan.Cascades) {
			for (const float Depth : {Cascade.Near, Cascade.Far})
				for (const float X : {-1.0f, 1.0f}) for (const float Y : {-1.0f, 1.0f}) {
					const glm::dvec3 World = glm::dvec3(Camera.Position) +
						glm::dvec3(Camera.LookDirection) * double(Depth) +
						glm::dvec3(Camera.RightDirection) * (X * Depth * Tangent * 16.0 / 9.0) +
						glm::dvec3(Camera.UpDirection) * (Y * Depth * Tangent);
					const auto Clip = glm::dmat4(Cascade.Matrix) *
						glm::dvec4(World - glm::dvec3(Cascade.Origin), 1.0);
					Require(std::abs(Clip.x) <= 1.01 && std::abs(Clip.y) <= 1.01,
						"frustum corner escaped snapped cascade");
				}
		}
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
	Require(!MakeSDLDirectionalCascadePlan(Camera, {0, 0, 0}, 1.0f, Settings),
		"degenerate sun direction admitted");
	std::cout << "[Render:ShadowCascade] Numerical geometry passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:ShadowCascade] " << Error.what() << '\n';
	return 1;
}
