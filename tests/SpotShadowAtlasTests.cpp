#include "render/sdl/SDLSpotShadowAtlas.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>
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
		Require(Entry.Dirty && !Entry.Sampleable && Entry.Layer < MaximumShadowedSpotlights,
			"first admission requires a bounded layer render");
		Atlas.BeginRecording(Entry.Layer);
		if (Entry.Light == ObjectId{1, 1}) Atlas.RecordPose(Entry.Layer, {100, 1}, 5);
		Atlas.FinishRecording(Entry.Layer, true, Entry.Light == ObjectId{1, 1});
	}
	for (const auto &Entry : Atlas.GetAssignments())
		Require(Entry.Sampleable, "a complete first-frame map is sampleable after its depth pass");
	Require(Atlas.OnFrameSubmitted() == 8, "all eight submitted maps commit");
	Atlas.Prepare(Frame, nullptr);
	for (const auto &Entry : Atlas.GetAssignments())
		Require(!Entry.Dirty && Entry.Sampleable, "unchanged lights reuse their depth layers");
	const auto RetainedLayer = Atlas.GetAssignments()[0].Layer;
	SDLShadowPoseRevisions CachedPoses;
	Atlas.MergeCommittedPoses(RetainedLayer, CachedPoses);
	Require(CachedPoses.size() == 1 && CachedPoses.at(ObjectId{100, 1}) == 5,
		"warm spotlight restores the exact submitted GPU skin pose revision");
	auto ChangedLights = std::make_shared<std::vector<RenderLocalLightState>>(*Lights);
	(*ChangedLights)[0].Intensity = 1.5f;
	(*ChangedLights)[0].Color = {0.8f, 0.7f, 0.6f};
	Frame.Environment.LocalLights = ChangedLights;
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments()[0].Layer == RetainedLayer &&
		!Atlas.GetAssignments()[0].Dirty, "color and intensity edits preserve a static spotlight map");
	auto DirectedLights = std::make_shared<std::vector<RenderLocalLightState>>(*ChangedLights);
	(*DirectedLights)[0].Direction = glm::normalize(glm::vec3(0.2f, 0.0f, -1.0f));
	Frame.Environment.LocalLights = DirectedLights;
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments()[0].Dirty, "light direction invalidates its own map");
	CachedPoses.clear();
	Atlas.MergeCommittedPoses(RetainedLayer, CachedPoses);
	Require(CachedPoses.empty(), "invalid spotlight depth cannot restore stale poses");
	Require(Atlas.OnFrameSubmitted() == 0, "unsubmitted depth never commits");
	Atlas.Invalidate();
	Atlas.Prepare(Frame, nullptr);
	for (const auto &Entry : Atlas.GetAssignments())
		Require(Entry.Dirty, "device recreation invalidates every selected map");
	const auto CanceledLayer = Atlas.GetAssignments().front().Layer;
	Atlas.BeginRecording(CanceledLayer);
	Atlas.RecordPose(CanceledLayer, {100, 1}, 7);
	Atlas.FinishRecording(CanceledLayer, true, true);
	const auto IncompleteCanceledLayer = Atlas.GetAssignments()[1].Layer;
	Atlas.BeginRecording(IncompleteCanceledLayer);
	Atlas.RecordPose(IncompleteCanceledLayer, {101, 1}, 7);
	Atlas.FinishRecording(IncompleteCanceledLayer, false, true);
	Atlas.ApplyPublication(RenderProjection{}, RenderPublication{});
	Atlas.Invalidate(); // A later render pass canceled this command buffer.
	Require(Atlas.OnFrameSubmitted() == 0,
		"a successful empty frame cannot commit any partially canceled spotlight candidate");
	CachedPoses.clear();
	Atlas.MergeCommittedPoses(CanceledLayer, CachedPoses);
	Atlas.MergeCommittedPoses(IncompleteCanceledLayer, CachedPoses);
	Require(CachedPoses.empty(), "canceled spotlight depth cannot restore a skin receipt");
	Atlas.Prepare(Frame, nullptr);
	Require(Atlas.GetAssignments().front().Dirty,
		"resumed spotlight must rebuild after a canceled recording");
	const auto IncompleteLayer = Atlas.GetAssignments().front().Layer;
	Atlas.BeginRecording(IncompleteLayer);
	Atlas.FinishRecording(IncompleteLayer, false, true);
	Require(!Atlas.GetAssignments().front().Sampleable && Atlas.OnFrameSubmitted() == 0,
		"missing caster residency cannot publish a sampleable atlas layer");
	auto PointLights = std::make_shared<std::vector<RenderLocalLightState>>(*DirectedLights);
	(*PointLights)[0].Kind = RenderLocalLightKind::Point;
	Frame.Environment.LocalLights = PointLights;
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
	// Projection accepts every positive outer cosine through one. Direct
	// publications wider than the atlas quality limit must be entirely
	// unshadowed, while supported cones stay finite and depth ordered.
	for (const float Range : {0.101f, 6.0f, 1024.0f}) {
		for (const float Outer : {0.000001f, 0.01f, 0.8f, 0.9999f, 1.0f}) {
			for (const glm::vec3 Direction : {glm::vec3(0, 1, 0),
				glm::vec3(std::sqrt(1.0f - 0.9899f * 0.9899f), 0.9899f, 0),
				glm::vec3(std::sqrt(1.0f - 0.9901f * 0.9901f), 0.9901f, 0),
				glm::vec3(0, -1, 0), glm::vec3(0, 0, -1)}) {
				RenderLocalLightState Extreme = (*Lights)[0];
				Extreme.Range = Range;
				Extreme.Direction = Direction;
				Extreme.OuterConeCosine = Outer;
				Extreme.InnerConeCosine = 1.0f;
				Frame.Environment.LocalLights =
					std::make_shared<const std::vector<RenderLocalLightState>>(
						std::initializer_list<RenderLocalLightState>{Extreme});
				Frame.Camera.Position = {0, 0, 0};
				Frame.Camera.ViewMatrix = glm::lookAt(glm::vec3(0), glm::vec3(0, 0, -1),
					glm::vec3(0, 1, 0));
				Frame.Camera.ViewProjectionMatrix = Frame.Camera.ProjectionMatrix * Frame.Camera.ViewMatrix;
				SDLSpotShadowAtlas ExtremeAtlas;
				ExtremeAtlas.Prepare(Frame, nullptr);
				if (Outer < 0.01f) {
					Require(ExtremeAtlas.GetAssignments().empty(),
						"unsupported wide spotlight admitted a partially covered shadow layer");
					continue;
				}
				Require(ExtremeAtlas.GetAssignments().size() == 1,
					"supported spotlight cone and range lost atlas eligibility");
				const auto &Matrix = ExtremeAtlas.GetAssignments().front().Matrix;
				for (int Column = 0; Column < 4; ++Column)
					for (int Row = 0; Row < 4; ++Row)
						Require(std::isfinite(Matrix[Column][Row]),
							"legal spotlight cone made a nonfinite depth matrix");
				const auto Near = Matrix * glm::vec4(Direction * (Range * 0.01f), 1.0f);
				const auto Far = Matrix * glm::vec4(Direction * Range, 1.0f);
				Require(Near.w > 0.0f && Far.w > 0.0f &&
					Near.z / Near.w < Far.z / Far.w,
					"legal spotlight cone reversed forward depth ordering");
			}
		}
	}
	const auto LightPlaneX = [&Frame, &Lights](glm::vec3 Direction) {
		RenderLocalLightState Light = (*Lights)[0];
		Light.Direction = glm::normalize(Direction);
		Frame.Environment.LocalLights = std::make_shared<const std::vector<RenderLocalLightState>>(
			std::initializer_list<RenderLocalLightState>{Light});
		SDLSpotShadowAtlas PoleAtlas;
		PoleAtlas.Prepare(Frame, nullptr);
		Require(PoleAtlas.GetAssignments().size() == 1,
			"spotlight pole continuity fixture lost its atlas layer");
		const auto &Matrix = PoleAtlas.GetAssignments().front().Matrix;
		return glm::normalize(glm::vec3(Matrix[0][0], Matrix[1][0], Matrix[2][0]));
	};
	const auto BeforeSwitch = LightPlaneX({std::sqrt(1.0f - 0.9899f * 0.9899f), 0.9899f, 0});
	const auto AfterSwitch = LightPlaneX({std::sqrt(1.0f - 0.9901f * 0.9901f), 0.9901f, 0});
	Require(glm::dot(BeforeSwitch, AfterSwitch) > 0.999f,
		"spotlight light plane jumped while crossing the old up-axis threshold");
	const auto BeforePole = LightPlaneX({0.000001f, 1, 0});
	const auto AfterPole = LightPlaneX({-0.000001f, 1, 0});
	Require(glm::dot(BeforePole, AfterPole) > 0.999f,
		"spotlight light plane jumped at the positive Y pole");
	const auto BeforeNegativePole = LightPlaneX({0.000001f, -1, 0});
	const auto AfterNegativePole = LightPlaneX({-0.000001f, -1, 0});
	Require(glm::dot(BeforeNegativePole, AfterNegativePole) > 0.999f,
		"spotlight light plane jumped at the negative Y pole");
	// Internal direct-frame adversary: this light lies beyond the neutral
	// MaximumLocalLightCoordinate (1e6). Its VP matches P * V bit for bit, but
	// it cannot be published. This checks conservative adapter behavior.
	RenderFrameState LargeFrame;
	LargeFrame.Camera.Position = {8970941.0f, 8221246.5f, 9602025.0f};
	LargeFrame.Camera.ProjectionMatrix = glm::mat4(
		glm::vec4(0.8033333f, 0, 0, 0), glm::vec4(0, 1.428148f, 0, 0),
		glm::vec4(0, 0, -1.001001f, -1), glm::vec4(0, 0, -0.1001001f, 0));
	LargeFrame.Camera.ViewMatrix = glm::mat4(
		glm::vec4(-0.72518361f, -0.56743336f, 0.39003608f, 0),
		glm::vec4(0, 0.56645548f, 0.82409233f, 0),
		glm::vec4(-0.68855554f, 0.59761822f, -0.41078421f, 0),
		glm::vec4(13117107.0f, -5304904.0f, -6329696.5f, 1));
	LargeFrame.Camera.ViewProjectionMatrix = glm::mat4(
		glm::vec4(-0.58256412f, -0.81037885f, -0.39042652f, -0.39003608f),
		glm::vec4(0, 0.80898231f, -0.82491726f, -0.82409233f),
		glm::vec4(-0.55313957f, 0.85348731f, 0.41119540f, 0.41078421f),
		glm::vec4(10537409.0f, -7576188.0f, 6336032.5f, 6329696.5f));
	RenderLocalLightState EdgeLight = (*Lights)[0];
	EdgeLight.Position = {8970932.0f, 8221241.0f, 9602022.0f};
	EdgeLight.Range = 0.101f;
	Require(glm::any(glm::greaterThan(glm::abs(EdgeLight.Position),
		glm::vec3(MaximumLocalLightCoordinate))) &&
		LargeFrame.Camera.ViewProjectionMatrix ==
			LargeFrame.Camera.ProjectionMatrix * LargeFrame.Camera.ViewMatrix,
		"direct-frame adversary unexpectedly became an in-range or noncanonical fixture");
	LargeFrame.Environment.LocalLights = std::make_shared<const std::vector<RenderLocalLightState>>(
		std::initializer_list<RenderLocalLightState>{EdgeLight});
	const glm::vec4 Staged = LargeFrame.Camera.ProjectionMatrix *
		(LargeFrame.Camera.ViewMatrix * glm::vec4(EdgeLight.Position, 1.0f));
	const auto Rows = glm::transpose(glm::dmat4(LargeFrame.Camera.ViewProjectionMatrix));
	const auto RightPlane = Rows[3] - Rows[0];
	const double AggregatedDistance = glm::dot(glm::dvec3(RightPlane),
		glm::dvec3(EdgeLight.Position)) + RightPlane.w;
	const double SphereRadius = glm::length(glm::dvec3(RightPlane)) * EdgeLight.Range;
	Require(Staged.w - Staged.x > 0.5f && AggregatedDistance < -SphereRadius - 0.25,
		"out-of-range direct-frame fixture did not separate its clip calculations");
	SDLSpotShadowAtlas EdgeAtlas;
	EdgeAtlas.Prepare(LargeFrame, nullptr);
	Require(EdgeAtlas.GetAssignments().size() == 1,
		"out-of-range direct-frame spotlight lost conservative atlas admission");
	std::cout << "[Render:SpotShadowAtlas] Budget, stable assignment and lifetime passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:SpotShadowAtlas] " << Error.what() << '\n';
	return 1;
}
