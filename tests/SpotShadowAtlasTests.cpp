#include "render/sdl/SDLSpotShadowAtlas.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <iostream>
#include <stdexcept>

namespace {
	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}
}

int main() try {
	using namespace gargantuan;
	RenderFrameState Frame;
	Frame.ViewportWidth = 1280;
	Frame.ViewportHeight = 720;
	Frame.Camera.ViewMatrix = glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	Frame.Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(70.0f), 16.0f / 9.0f, 0.1f, 100.0f);
	Frame.Camera.ViewProjectionMatrix = Frame.Camera.ProjectionMatrix * Frame.Camera.ViewMatrix;
	auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
	for (std::uint32_t Index = 1; Index <= 12; ++Index) {
		RenderLocalLightState Light;
		Light.Object = {Index, 1};
		Light.Kind = RenderLocalLightKind::Spot;
		Light.Position = {0, 0, -4};
		Light.Direction = {0, 0, -1};
		Light.Intensity = static_cast<float>(Index);
		Light.ShadowEnabled = true;
		Light.ShadowPriority = Index == 1 ? 10 : 0;
		Lights->push_back(Light);
	}
	Frame.Environment.LocalLights = Lights;
	SDLSpotShadowAtlas Atlas;
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments().size() == MaximumShadowedSpotlights && Atlas.GetDroppedCount() == 4,
		"the spotlight budget selects exactly eight visible winners");
	Require(Atlas.GetAssignments()[0].Light == ObjectId{1, 1},
		"priority precedes contribution score");
	const auto OriginMatrix = Atlas.GetAssignments()[0].Matrix;
	for (const auto &Entry : Atlas.GetAssignments()) {
		Require(Entry.Dirty && Entry.Layer < MaximumShadowedSpotlights,
			"first admission requires a bounded layer render");
		Atlas.BeginRecording(Entry.Layer);
		Atlas.FinishRecording(Entry.Layer, true, false);
	}
	Require(Atlas.OnFrameSubmitted() == 8, "all eight submitted maps commit");
	Atlas.Prepare(Frame, nullptr);
	for (const auto &Entry : Atlas.GetAssignments())
		Require(!Entry.Dirty, "unchanged lights reuse their depth layers");
	const auto RetainedLayer = Atlas.GetAssignments()[0].Layer;
	auto ChangedLights = std::make_shared<std::vector<RenderLocalLightState>>(*Lights);
	(*ChangedLights)[0].Intensity = 1.5f;
	(*ChangedLights)[0].Color = {0.8f, 0.7f, 0.6f};
	Frame.Environment.LocalLights = ChangedLights;
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments()[0].Layer == RetainedLayer &&
		!Atlas.GetAssignments()[0].Dirty, "color and intensity edits preserve a static spotlight map");
	(*ChangedLights)[0].Direction = glm::normalize(glm::vec3(0.2f, 0.0f, -1.0f));
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments()[0].Dirty, "light direction invalidates its own map");
	Require(Atlas.OnFrameSubmitted() == 0, "unsubmitted depth never commits");
	Atlas.Invalidate();
	Atlas.Prepare(Frame, nullptr);
	for (const auto &Entry : Atlas.GetAssignments())
		Require(Entry.Dirty, "device recreation invalidates every selected map");
	(*ChangedLights)[0].Kind = RenderLocalLightKind::Point;
	Frame.Environment.LocalLights = ChangedLights;
	Atlas.Prepare(Frame, nullptr);
	for (const auto &Entry : Atlas.GetAssignments())
		Require(Entry.Light != ObjectId{1, 1}, "point lights cannot acquire spot atlas layers");
	auto DistantLights = std::make_shared<std::vector<RenderLocalLightState>>(*Lights);
	const glm::vec3 Offset(900000.0f, 2000.0f, -900000.0f);
	for (auto &Light : *DistantLights) Light.Position += Offset;
	Frame.Camera.Position = Offset;
	Frame.Camera.ViewMatrix = glm::lookAt(Offset, Offset + glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
	Frame.Camera.ViewProjectionMatrix = Frame.Camera.ProjectionMatrix * Frame.Camera.ViewMatrix;
	Frame.Environment.LocalLights = DistantLights;
	SDLSpotShadowAtlas DistantAtlas;
	DistantAtlas.Prepare(Frame, nullptr);
	Require(DistantAtlas.GetAssignments().size() == 8 &&
		DistantAtlas.GetAssignments()[0].Matrix == OriginMatrix &&
		DistantAtlas.GetAssignments()[0].Origin == (*DistantLights)[0].Position,
		"spot projection remains origin-relative at large coordinates");
	std::cout << "[Render:SpotShadowAtlas] Budget, stable assignment and lifetime passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:SpotShadowAtlas] " << Error.what() << '\n';
	return 1;
}
