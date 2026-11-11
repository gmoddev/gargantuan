#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace gargantuan;

namespace {

struct Box { glm::dvec3 Center; glm::dvec3 Size; };
const std::array<Box, 2> Scene{{{{-50.125, 0, 74.25}, {205.75, 1, 341}},
                               {{-10.5, 24, -28.75}, {30.5, 59, 135}}}};

double Intersect(const Box &Value, glm::dvec3 Eye, glm::dvec3 Ray) {
	double Near = 0, Far = std::numeric_limits<double>::infinity();
	for (int Axis = 0; Axis < 3; ++Axis) {
		const double Minimum = Value.Center[Axis] - Value.Size[Axis] * .5;
		const double Maximum = Value.Center[Axis] + Value.Size[Axis] * .5;
		if (std::abs(Ray[Axis]) < 1e-12) {
			if (Eye[Axis] < Minimum || Eye[Axis] > Maximum) return std::numeric_limits<double>::infinity();
			continue;
		}
		const double A = (Minimum - Eye[Axis]) / Ray[Axis], B = (Maximum - Eye[Axis]) / Ray[Axis];
		Near = std::max(Near, std::min(A, B)); Far = std::min(Far, std::max(A, B));
	}
	return Near <= Far ? Near : std::numeric_limits<double>::infinity();
}

struct Coverage { std::size_t Expected = 0, Missing = 0; };
Coverage Measure(const EditorViewportFrame &Frame, const RenderCameraSnapshot &Camera, double Scale) {
	const double Tangent = std::tan(glm::radians(static_cast<double>(Camera.VerticalFieldOfView)) * .5);
	std::vector<bool> Expected(Frame.Width * Frame.Height);
	for (unsigned Y = 0; Y < Frame.Height; ++Y) for (unsigned X = 0; X < Frame.Width; ++X) {
		const auto Ray = glm::dvec3(Camera.LookDirection) + glm::dvec3(Camera.RightDirection) *
			((2.0 * (X + .5) / Frame.Width - 1) * Frame.Width / Frame.Height * Tangent) +
			glm::dvec3(Camera.UpDirection) * ((1 - 2.0 * (Y + .5) / Frame.Height) * Tangent);
		const auto Floor = Intersect({Scene[0].Center * Scale, Scene[0].Size * Scale}, Camera.Position, Ray);
		const auto Block = Intersect({Scene[1].Center * Scale, Scene[1].Size * Scale}, Camera.Position, Ray);
		Expected[Y * Frame.Width + X] = Block < Floor && Block > Camera.NearPlane && Block < Camera.FarPlane;
	}
	Coverage Result;
	for (unsigned Y = 1; Y + 1 < Frame.Height; ++Y) for (unsigned X = 1; X + 1 < Frame.Width; ++X) {
		const auto Index = Y * Frame.Width + X;
		if (!Expected[Index] || !Expected[Index-1] || !Expected[Index+1] ||
			!Expected[Index-Frame.Width] || !Expected[Index+Frame.Width]) continue;
		++Result.Expected;
		const auto Pixel = Index * 3;
		if (Frame.RgbPixels[Pixel] != Frame.RgbPixels[Pixel+1] || Frame.RgbPixels[Pixel+1] != Frame.RgbPixels[Pixel+2])
			++Result.Missing;
	}
	return Result;
}

void WriteFrame(const std::filesystem::path &Path, const EditorViewportFrame &Frame) {
	std::ofstream File(Path, std::ios::binary);
	File << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
	File.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()), Frame.RgbPixels.size());
	if (!File) throw std::runtime_error("Could not write raw frame");
}

constexpr std::uint32_t ORDER_WIDTH = 640;
constexpr std::uint32_t ORDER_HEIGHT = 480;
constexpr RenderMeshIdentity ORDER_MESH{1, 1};

RenderPublication MakeOrderingScene(RenderPublicationId Id, const glm::mat4 &First, const glm::mat4 &Second,
	bool Reverse, bool Masked, bool Shadows) {
	RenderPublication Value{.Id = Id, .FullResync = true,
		.Frame = {.ViewportWidth = ORDER_WIDTH, .ViewportHeight = ORDER_HEIGHT}};
	auto &Camera = Value.Frame.Camera;
	Camera.Position = {0, 0, 3};
	Camera.VerticalFieldOfView = 45;
	Camera.FarPlane = 10;
	Camera.ViewMatrix = glm::lookAtRH(Camera.Position, glm::vec3(0), Camera.UpDirection);
	Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
		static_cast<float>(ORDER_WIDTH) / ORDER_HEIGHT, Camera.NearPlane, Camera.FarPlane);
	Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
	Value.Frame.Environment.SunDirection = {0, 0, 1};
	Value.Frame.Environment.SunIntensity = Shadows ? 1.0f : 0.0f;
	Value.Frame.Environment.AmbientColor = {0.7f, 0.7f, 0.7f};
	Value.MeshCreates.push_back({ORDER_MESH, 1, 1,
		std::make_shared<const std::vector<RenderVertex>>(std::initializer_list<RenderVertex>{
			{{-.01f, -.01f, 0}, {0, 0, 1}}, {{.01f, -.01f, 0}, {0, 0, 1}},
			{{.01f, .01f, 0}, {0, 0, 1}}, {{-.01f, .01f, 0}, {0, 0, 1}},
		}),
		std::make_shared<const std::vector<std::uint32_t>>(std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3}),
		{{-.01f, -.01f, 0}, {.01f, .01f, 0}},
	});
	const auto Append = [&](ObjectId Id, const glm::mat4 &Matrix, const glm::vec4 &Color) {
		RenderMaterialState FirstMaterial{.BaseColorFactor = Color, .DoubleSided = true};
		RenderMaterialState SecondMaterial{.BaseColorFactor = {0, 0, 1, 1}, .DoubleSided = true};
		constexpr RenderTextureIdentity Texture{1, 1};
		SecondMaterial.BaseColorTexture = Texture;
		if (Value.TextureCreates.empty()) Value.TextureCreates.push_back({Texture, 1, 1, 1,
			RenderTextureFormat::Rgba8Unorm,
			std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255, 255, 255, 255})});
		if (Masked) FirstMaterial.OpacityMode = RenderOpacityMode::Masked;
		Value.Creates.push_back({.Item = {.Object = Id, .ModelMatrix = Matrix, .InverseModelMatrix = glm::inverse(Matrix),
			.CastShadow = Shadows}, .Mesh = ORDER_MESH,
			.Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
				std::initializer_list<RenderPrimitiveMaterialState>{{0, 3, FirstMaterial}, {3, 3, SecondMaterial}})});
	};
	Append({1, 1}, First, {1, 0, 0, 1});
	Append({2, 1}, Second, {0, 1, 0, 1});
	if (Reverse) std::swap(Value.Creates[0], Value.Creates[1]);
	return Value;
}

RenderPublication WithoutSharedMesh(RenderPublication Value) {
	auto Separate = Value.MeshCreates.front();
	Separate.Mesh = {2, 1};
	Value.MeshCreates.push_back(Separate);
	Value.Creates[1].Mesh = Separate.Mesh;
	return Value;
}

void CheckOpaqueOrdering(const std::filesystem::path &Output) {
	EditorViewportRenderer Candidate(ORDER_WIDTH, ORDER_HEIGHT);
	EditorViewportRenderer Reference(ORDER_WIDTH, ORDER_HEIGHT);
	SDLRenderer MetricsRenderer({ORDER_WIDTH, ORDER_HEIGHT}, {
		.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
	});
	const bool Vulkan = MetricsRenderer.GetDriverName() == "vulkan";
	const auto Draw = [](auto &Renderer, const RenderPublication &Publication) {
		return Renderer.Capture(std::make_shared<const RenderPublication>(Publication));
	};
	const auto CheckPixels = [&](std::string_view Name, const RenderPublication &Shared, const RenderPublication &Control) {
		const auto Actual = Draw(Candidate, Shared);
		if (!Output.empty()) WriteFrame(Output / (std::string(Name) + "-candidate.ppm"), Actual);
		const auto Expected = Draw(Reference, Control);
		if (!Output.empty()) WriteFrame(Output / (std::string(Name) + "-legacy-order.ppm"), Expected);
		if (Actual.RgbPixels != Expected.RgbPixels)
			throw std::runtime_error(std::format("Opaque ordering changed visible pixels for {}", Name));
		if (!std::ranges::any_of(Actual.RgbPixels, [](std::uint8_t Channel) { return Channel > 150; }))
			throw std::runtime_error(std::format("Opaque ordering fixture {} produced no bright material pixels", Name));
	};
	const glm::mat4 Identity(1.0f);
	const auto Translate = [&](float X, float Z) { return glm::translate(Identity, glm::vec3(X, 0, Z)); };
	const auto First = Translate(-.045f, 0);
	const auto Second = Translate(.045f, 0);
	const auto Rotation = glm::rotate(Identity, glm::radians(45.0f), glm::vec3(0, 1, 0));
	const auto Large = glm::scale(Identity, glm::vec3(12));
	RenderPublicationId Next = 1;
	const auto Check = [&](std::string_view Name, const glm::mat4 &A, const glm::mat4 &B, bool Grouped,
		bool Conflict = false, bool Depth = false, bool Budget = false, bool Reverse = false,
		bool Mask = false, bool Shadow = false) {
		const auto Value = MakeOrderingScene(Next++, A, B, Reverse, Mask, Shadow);
		CheckPixels(Name, Value, WithoutSharedMesh(Value));
		const auto Before = MetricsRenderer.GetMetrics();
		MetricsRenderer.Draw(std::make_shared<const RenderPublication>(Value));
		const auto After = MetricsRenderer.GetMetrics();
		const auto GroupedCount = After.OpaqueGroupedObjects - Before.OpaqueGroupedObjects;
		const auto Conflicts = After.OpaqueGroupingConflictBarriers - Before.OpaqueGroupingConflictBarriers;
		const auto DepthProofs = After.OpaqueGroupingDepthProofs - Before.OpaqueGroupingDepthProofs;
		const auto Budgets = After.OpaqueGroupingBudgetBarriers - Before.OpaqueGroupingBudgetBarriers;
		const auto Binds = After.WorldMaterialBindings - Before.WorldMaterialBindings;
		std::cout << "[Render:OpaqueOrdering] " << Name << " grouped=" << GroupedCount << " conflicts=" << Conflicts
			<< " depthProofs=" << DepthProofs << " budgets=" << Budgets << " binds=" << Binds << '\n';
		if (GroupedCount != (Vulkan && Grouped ? 2 : 0) || (Conflicts > 0) != (Vulkan && Conflict) ||
			(DepthProofs > 0) != (Vulkan && Depth) || (Budgets > 0) != (Vulkan && Budget) ||
			Binds != (Vulkan && Grouped ? 2 : 4))
			throw std::runtime_error(std::format("Unexpected grouping or material bindings for {}", Name));
	};
	Check("screen-separated", First, Second, true);
	Check("depth-separated", Identity, Translate(0, -1.5f), true, false, true);
	Check("coplanar", Identity, Identity, false, true);
	Check("coplanar-reversed", Identity, Identity, false, true, false, false, true);
	Check("near-equal", Identity, Translate(0, 1e-7f), false, true);
	Check("intersecting", Identity, Rotation, false, true);
	Check("coverage-budget", Large, Large, false, false, false, true);
	Check("alpha-mask", First, Second, false, false, false, false, false, true);
	Check("shadow-casters", First, Second, true, false, false, false, false, false, true);

	const auto Initial = MakeOrderingScene(Next++, First, Second, false, false, false);
	const auto Control = WithoutSharedMesh(Initial);
	CheckPixels("before-moving-overlap", Initial, Control);
	RenderPublication Move{.Id = Next++, .BaseId = Initial.Id, .Frame = Initial.Frame};
	const auto &Object = Initial.Creates[1];
	Move.Updates.push_back({.Object = Object.Item.Object, .Domains = RenderUpdateDomain::Transform,
		.Item = Object.Item, .Mesh = Object.Mesh, .Primitives = Object.Primitives});
	Move.Updates[0].Item.ModelMatrix = First;
	Move.Updates[0].Item.InverseModelMatrix = glm::inverse(First);
	auto ControlMove = Move;
	ControlMove.Updates[0].Mesh = Control.Creates[1].Mesh;
	CheckPixels("moving-overlap", Move, ControlMove);

	EditorViewportRenderer Restarted(ORDER_WIDTH, ORDER_HEIGHT);
	const auto Resync = MakeOrderingScene(Next, First, First, false, false, false);
	if (Draw(Restarted, Resync).RgbPixels != Draw(Reference, WithoutSharedMesh(Resync)).RgbPixels)
		throw std::runtime_error("Opaque ordering changed visible pixels after renderer recreation");
}

RenderPublication MakeInstanceScene(RenderPublicationId Id, const glm::mat4 &First,
	const glm::mat4 &Second, bool Reverse = false, bool Shadows = false) {
	auto Value = MakeOrderingScene(Id, First, Second, Reverse, false, Shadows);
	Value.TextureCreates.clear();
	for (auto &Create : Value.Creates) {
		auto Material = Create.Primitives->front().Material;
		Create.Material = Material;
		Create.Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
			std::initializer_list<RenderPrimitiveMaterialState>{{0, 6, Material}});
	}
	return Value;
}

void CheckInstancingOrdering(const std::filesystem::path &Output) {
	EditorViewportRenderer Candidate(ORDER_WIDTH, ORDER_HEIGHT, true, true);
	EditorViewportRenderer Reference(ORDER_WIDTH, ORDER_HEIGHT, true, false);
	const auto Compare = [&](std::string_view Name, const RenderPublication &Value) {
		const auto Publication = std::make_shared<const RenderPublication>(Value);
		const auto Actual = Candidate.Capture(Publication);
		const auto Expected = Reference.Capture(Publication);
		if (!Output.empty()) {
			WriteFrame(Output / (std::string(Name) + "-instanced.ppm"), Actual);
			WriteFrame(Output / (std::string(Name) + "-individual.ppm"), Expected);
		}
		if (Actual.RgbPixels != Expected.RgbPixels)
			throw std::runtime_error(std::format("Instancing changed visible pixels for {}", Name));
		if (!std::ranges::any_of(Actual.RgbPixels, [](std::uint8_t Channel) { return Channel > 150; }))
			throw std::runtime_error(std::format("Instancing fixture {} produced no bright material pixels", Name));
		const auto CandidatePick = Candidate.Pick(ORDER_WIDTH * 0.5f, ORDER_HEIGHT * 0.5f);
		const auto ReferencePick = Reference.Pick(ORDER_WIDTH * 0.5f, ORDER_HEIGHT * 0.5f);
		if (CandidatePick.has_value() != ReferencePick.has_value() ||
			(CandidatePick && CandidatePick->Object != ReferencePick->Object))
			throw std::runtime_error(std::format("Instancing changed picking identity for {}", Name));
	};
	const glm::mat4 Identity(1.0f);
	const auto Translate = [&](float X, float Z) { return glm::translate(Identity, glm::vec3(X, 0, Z)); };
	const auto First = Translate(-.045f, 0);
	const auto Second = Translate(.045f, 0);
	const auto Rotation = glm::rotate(Identity, glm::radians(45.0f), glm::vec3(0, 1, 0));
	RenderPublicationId Next = 1;
	const auto ScreenSeparated = MakeInstanceScene(Next++, First, Second);
	Compare("screen-separated", ScreenSeparated);
	SDLRenderer MetricsRenderer({ORDER_WIDTH, ORDER_HEIGHT}, {
		.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		.EnableInstancing = true,
	});
	MetricsRenderer.Draw(std::make_shared<const RenderPublication>(ScreenSeparated));
	const auto Metrics = MetricsRenderer.GetMetrics();
	const bool Vulkan = MetricsRenderer.GetDriverName() == "vulkan";
	if (Vulkan && (Metrics.InstancedDrawCalls == 0 || Metrics.InstancedObjects < 2))
		throw std::runtime_error("Screen-separated fixture did not issue a native instanced draw");
	std::cout << "[Render:Instancing] driver=" << MetricsRenderer.GetDriverName()
		<< " nativeDraws=" << Metrics.InstancedDrawCalls << " objects=" << Metrics.InstancedObjects << '\n';
	Compare("depth-separated", MakeInstanceScene(Next++, Identity, Translate(0, -1.5f)));
	Compare("coplanar", MakeInstanceScene(Next++, Identity, Identity));
	Compare("coplanar-reversed", MakeInstanceScene(Next++, Identity, Identity, true));
	Compare("near-equal", MakeInstanceScene(Next++, Identity, Translate(0, 1e-7f)));
	Compare("intersecting", MakeInstanceScene(Next++, Identity, Rotation));
	Compare("multiple-ranges", MakeOrderingScene(Next++, First, Second, false, false, false));
	Compare("masked-material", MakeOrderingScene(Next++, First, Second, false, true, false));
	Compare("separate-meshes", WithoutSharedMesh(MakeInstanceScene(Next++, First, Second)));
	Compare("shadow-casters", MakeInstanceScene(Next++, First, Second, false, true));
	auto Frustum = MakeInstanceScene(Next++, Translate(0, 6), Second);
	Compare("frustum-culled", Frustum);
	const auto BeforeCulled = MetricsRenderer.GetMetrics();
	MetricsRenderer.Draw(std::make_shared<const RenderPublication>(Frustum));
	const auto AfterCulled = MetricsRenderer.GetMetrics();
	if (AfterCulled.CameraCulledObjects != BeforeCulled.CameraCulledObjects + 1)
		throw std::runtime_error("Instancing counted a preclassified camera cull more than once");

	const auto Initial = MakeInstanceScene(Next++, First, Second);
	Compare("before-rehash", Initial);
	RenderPublication VisibilityOff{.Id = Next++, .BaseId = Initial.Id, .Frame = Initial.Frame};
	const auto &FirstCreate = Initial.Creates.front();
	VisibilityOff.Updates.push_back({.Object = FirstCreate.Item.Object,
		.Domains = RenderUpdateDomain::Visibility, .Item = FirstCreate.Item,
		.Mesh = FirstCreate.Mesh, .Material = FirstCreate.Material, .Visible = false,
		.Primitives = FirstCreate.Primitives});
	Compare("visibility-off", VisibilityOff);
	RenderPublication VisibilityOn{.Id = Next++, .BaseId = VisibilityOff.Id, .Frame = Initial.Frame};
	VisibilityOn.Updates = VisibilityOff.Updates;
	VisibilityOn.Updates.front().Visible = true;
	Compare("visibility-on", VisibilityOn);
	RenderPublication Insert{.Id = Next++, .BaseId = VisibilityOn.Id, .Frame = Initial.Frame};
	for (std::uint32_t Slot = 3; Slot <= 82; ++Slot) {
		auto Create = Initial.Creates.front();
		Create.Item.Object = {Slot, 1};
		Create.Item.ModelMatrix = Translate(100.0f + Slot, 0);
		Create.Item.InverseModelMatrix = glm::inverse(Create.Item.ModelMatrix);
		Insert.Creates.push_back(Create);
	}
	Compare("late-create-rehash", Insert);
	RenderPublication Sparse{.Id = Next++, .BaseId = Insert.Id, .Frame = Initial.Frame};
	Sparse.Removes.push_back({{3, 1}});
	auto Replacement = Initial.Creates.front();
	Replacement.Item.Object = {3, 2};
	Replacement.Item.ModelMatrix = Translate(103, 0);
	Replacement.Item.InverseModelMatrix = glm::inverse(Replacement.Item.ModelMatrix);
	Sparse.Creates.push_back(Replacement);
	Compare("remove-recreate-generation", Sparse);
	RenderPublication MaterialChange{.Id = Next++, .BaseId = Sparse.Id, .Frame = Initial.Frame};
	RenderObjectUpdate Masked{.Object = Initial.Creates.front().Item.Object,
		.Domains = RenderUpdateDomain::Material, .Item = Initial.Creates.front().Item,
		.Mesh = ORDER_MESH, .Material = Initial.Creates.front().Material};
	Masked.Material.OpacityMode = RenderOpacityMode::Masked;
	Masked.Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
		std::initializer_list<RenderPrimitiveMaterialState>{{0, 6, Masked.Material}});
	MaterialChange.Updates.push_back(Masked);
	Compare("material-eligibility-transition", MaterialChange);
	RenderPublication Dense{.Id = Next++, .BaseId = MaterialChange.Id, .Frame = Initial.Frame};
	for (std::uint32_t Slot = 4; Slot <= 82; ++Slot) {
		RenderObjectUpdate Update{.Object = {Slot, 1}, .Domains = RenderUpdateDomain::Transform,
			.Item = Insert.Creates[Slot - 3].Item, .Mesh = ORDER_MESH,
			.Material = Insert.Creates[Slot - 3].Material,
			.Primitives = Insert.Creates[Slot - 3].Primitives};
		Update.Item.ModelMatrix[3].x += 1;
		Update.Item.InverseModelMatrix = glm::inverse(Update.Item.ModelMatrix);
		Dense.Updates.push_back(Update);
	}
	Compare("dense-update", Dense);
	Compare("full-resync", MakeInstanceScene(Next++, First, Second));
	EditorViewportRenderer Restarted(ORDER_WIDTH, ORDER_HEIGHT, true, true);
	EditorViewportRenderer RestartReference(ORDER_WIDTH, ORDER_HEIGHT, true, false);
	const auto Restart = std::make_shared<const RenderPublication>(MakeInstanceScene(Next, First, Second));
	if (Restarted.Capture(Restart).RgbPixels != RestartReference.Capture(Restart).RgbPixels)
		throw std::runtime_error("Instancing changed visible pixels after renderer recreation");
}

void CheckSceneEnvelopePixels(const std::filesystem::path &Output) {
	EditorViewportRenderer Candidate(ORDER_WIDTH, ORDER_HEIGHT, true, false, true);
	EditorViewportRenderer Reference(ORDER_WIDTH, ORDER_HEIGHT);
	SDLRenderer MetricsRenderer({ORDER_WIDTH, ORDER_HEIGHT}, {
		.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		.EnableSceneEnvelope = true});
	const auto CaptureEqual = [&](std::string_view Name, const RenderPublication &Value) {
		const auto Publication = std::make_shared<const RenderPublication>(Value);
		const auto Actual = Candidate.Capture(Publication);
		const auto Expected = Reference.Capture(Publication);
		MetricsRenderer.Draw(Publication);
		if (!Output.empty()) {
			WriteFrame(Output / (std::string(Name) + "-envelope.ppm"), Actual);
			WriteFrame(Output / (std::string(Name) + "-linear.ppm"), Expected);
		}
		if (Actual.RgbPixels != Expected.RgbPixels)
			throw std::runtime_error(std::format("Scene envelope changed visible pixels for {}", Name));
	};
	const glm::mat4 Identity(1.0f);
	const auto Translate = [&](float X, float Z) {
		return glm::translate(Identity, glm::vec3(X, 0, Z));
	};
	RenderPublicationId Id = 1;
	auto Initial = MakeInstanceScene(Id++, Translate(1'000'000.0f, 0), Translate(1'000'002.0f, 0));
	CaptureEqual("all-offscreen-initial", Initial);
	for (int Index = 0; Index < 3; ++Index) {
		RenderPublication Quiet{.Id = Id, .BaseId = Id - 1, .Frame = Initial.Frame};
		++Id;
		CaptureEqual("all-offscreen-quiet-" + std::to_string(Index), Quiet);
	}
	const auto Rejected = MetricsRenderer.GetMetrics();
	if (Rejected.EnvelopeRejections == 0 || Rejected.EnvelopeRebuilds == 0 ||
		Rejected.CameraCulledObjects < 2 * Rejected.EnvelopeRejections)
		throw std::runtime_error("Scene envelope did not reject the static offscreen camera scene");
	RenderPublication Move{.Id = Id, .BaseId = Id - 1, .Frame = Initial.Frame};
	++Id;
	const auto &Object = Initial.Creates[0];
	Move.Updates.push_back({.Object = Object.Item.Object, .Domains = RenderUpdateDomain::Transform,
		.Item = Object.Item, .Mesh = Object.Mesh, .Material = Object.Material,
		.Primitives = Object.Primitives});
	Move.Updates[0].Item.ModelMatrix = Translate(0, 0);
	Move.Updates[0].Item.InverseModelMatrix = glm::inverse(Move.Updates[0].Item.ModelMatrix);
	CaptureEqual("move-into-view", Move);
	RenderPublication Near{.Id = Id, .BaseId = Id - 1, .Frame = Initial.Frame};
	++Id;
	Near.Updates = Move.Updates;
	Near.Updates[0].Item.ModelMatrix = Translate(0, 2.9f);
	Near.Updates[0].Item.InverseModelMatrix = glm::inverse(Near.Updates[0].Item.ModelMatrix);
	CaptureEqual("near-plane", Near);
	for (int Index = 0; Index < 3; ++Index) {
		RenderPublication Quiet{.Id = Id, .BaseId = Id - 1, .Frame = Initial.Frame};
		++Id;
		CaptureEqual("near-plane-cached-" + std::to_string(Index), Quiet);
	}
	auto Resync = MakeInstanceScene(Id, Translate(1'000'000.0f, 0), Translate(1'000'002.0f, 0));
	CaptureEqual("full-resync", Resync);
	std::cout << "[Render:SceneEnvelope] rejections=" << Rejected.EnvelopeRejections
		<< " rebuilds=" << Rejected.EnvelopeRebuilds << " culled=" << Rejected.CameraCulledObjects << '\n';
}

void CheckContiguousInstancing(const std::filesystem::path &Output) {
	EditorViewportRenderer Candidate(ORDER_WIDTH, ORDER_HEIGHT, true, true, false, true);
	EditorViewportRenderer Reference(ORDER_WIDTH, ORDER_HEIGHT);
	SDLRenderer MetricsRenderer({ORDER_WIDTH, ORDER_HEIGHT}, {
		.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		.EnableInstancing = true, .UseContiguousInstancing = true});
	const bool Vulkan = MetricsRenderer.GetDriverName() == "vulkan";
	RenderPublicationId Id = 1;
	const glm::mat4 Identity(1.0f);
	const auto Translate = [&](float X, float Z) {
		return glm::translate(Identity, glm::vec3(X, 0, Z));
	};
	const auto Compare = [&](std::string_view Name, const RenderPublication &Value, bool Eligible,
		std::uint64_t ExpectedObjects = 2, std::uint64_t ExpectedBatches = 1) {
		const auto Publication = std::make_shared<const RenderPublication>(Value);
		const auto Actual = Candidate.Capture(Publication);
		const auto Expected = Reference.Capture(Publication);
		if (!Output.empty()) {
			WriteFrame(Output / (std::string(Name) + "-contiguous.ppm"), Actual);
			WriteFrame(Output / (std::string(Name) + "-individual.ppm"), Expected);
		}
		if (Actual.RgbPixels != Expected.RgbPixels)
			throw std::runtime_error(std::format("Contiguous instancing changed visible pixels for {}", Name));
		const auto Before = MetricsRenderer.GetMetrics();
		MetricsRenderer.Draw(Publication);
		const auto After = MetricsRenderer.GetMetrics();
		const auto Batches = After.ContiguousInstancedDrawCalls - Before.ContiguousInstancedDrawCalls;
		const auto Objects = After.InstancedObjects - Before.InstancedObjects;
		if (Vulkan && (Batches != (Eligible ? ExpectedBatches : 0u) ||
			Objects != (Eligible ? ExpectedObjects : 0u)))
			throw std::runtime_error(std::format("Contiguous fixture {} issued {} batches for {} objects", Name, Batches, Objects));
		std::cout << "[Render:ContiguousInstances] " << Name << " batches=" << Batches
			<< " objects=" << Objects << '\n';
		return Expected;
	};
	// Build the order-dependent offset fixture first, while all renderer maps
	// have the same fresh bucket policy as this independent projection.
	auto Offset = MakeInstanceScene(Id++, Identity, Identity);
	for (std::uint32_t Slot = 3; Slot <= 4; ++Slot) {
		auto Create = Offset.Creates.front();
		Create.Item.Object = {Slot, 1};
		Offset.Creates.push_back(Create);
	}
	RenderProjection Order;
	(void)Order.Apply(Offset);
	const auto &Traversal = Order.GetCompactTraversal();
	for (std::size_t Position = 2; Position < Traversal.size(); ++Position) {
		const auto Object = Traversal[Position].Object;
		auto Create = std::ranges::find_if(Offset.Creates,
			[&](const RenderObjectCreate &Entry) { return Entry.Item.Object == Object; });
		Create->Material.DoubleSided = false;
		Create->Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
			std::initializer_list<RenderPrimitiveMaterialState>{{0, 6, Create->Material}});
	}
	Compare("nonzero-first-instance", Offset, true, 4, 2);
	Compare("screen-separated", MakeInstanceScene(Id++, Translate(-.045f, 0), Translate(.045f, 0)), true);
	Compare("depth-separated", MakeInstanceScene(Id++, Identity, Translate(0, -1.5f)), true);
	Compare("coplanar", MakeInstanceScene(Id++, Identity, Identity), true);
	Compare("coplanar-reversed", MakeInstanceScene(Id++, Identity, Identity, true), true);
	Compare("near-equal-depth", MakeInstanceScene(Id++, Identity, Translate(0, 1e-7f)), true);
	Compare("intersecting", MakeInstanceScene(Id++, Identity,
		glm::rotate(Identity, glm::radians(45.0f), glm::vec3(0, 1, 0))), true);
	Compare("shadow-casters", MakeInstanceScene(Id++, Identity, Translate(0, -1.5f), false, true), true);
	auto Nonunit = MakeInstanceScene(Id++, Identity, Translate(0, -1.5f));
	Nonunit.Frame.Environment.SunIntensity = 3.0f;
	Nonunit.Frame.Environment.SunDirection = glm::normalize(glm::vec3(0.5f, 0.4f, 1.0f));
	Nonunit.Frame.Environment.AmbientColor = {0.02f, 0.02f, 0.02f};
	auto Vertices = std::make_shared<std::vector<RenderVertex>>(*Nonunit.MeshCreates.front().Vertices);
	const std::array<glm::vec3, 4> NonunitNormals{{{4, 0, 1}, {0, 3, 1}, {-4, 0, 1}, {0, -3, 1}}};
	for (std::size_t Index = 0; Index < Vertices->size(); ++Index) {
		auto &Vertex = Vertices->at(Index);
		Vertex.Position *= 30.0f;
		Vertex.Normal = NonunitNormals[Index];
		Vertex.Tangent.x *= 2.3f;
		Vertex.Tangent.y *= 2.3f;
		Vertex.Tangent.z *= 2.3f;
	}
	auto Flat = Nonunit;
	auto FlatVertices = std::make_shared<std::vector<RenderVertex>>(*Vertices);
	for (auto &Vertex : *FlatVertices) Vertex.Normal = {0, 0, 1};
	Flat.MeshCreates.front().Vertices = FlatVertices;
	Nonunit.MeshCreates.front().Vertices = Vertices;
	Nonunit.MeshCreates.front().Bounds = {{-.3f, -.3f, 0}, {.3f, .3f, 0}};
	Flat.MeshCreates.front().Bounds = Nonunit.MeshCreates.front().Bounds;
	const auto NonunitPixels = Compare("nonunit-normal-tangent", Nonunit, true);
	EditorViewportRenderer FlatRenderer(ORDER_WIDTH, ORDER_HEIGHT);
	const auto FlatPixels = FlatRenderer.Capture(std::make_shared<const RenderPublication>(Flat));
	if (FlatPixels.RgbPixels == NonunitPixels.RgbPixels)
		throw std::runtime_error("Nonunit normal fixture did not exercise sun-lit shading");
	auto Dense = MakeInstanceScene(Id++, Identity, Identity);
	for (std::uint32_t Slot = 3; Slot <= 64; ++Slot) {
		auto Create = Dense.Creates.front();
		Create.Item.Object = {Slot, 1};
		Dense.Creates.push_back(Create);
	}
	Compare("dense-overlap-64", Dense, true, 64);
	Compare("multiple-ranges", MakeOrderingScene(Id++, Identity, Identity, false, false, false), false);
	Compare("masked-material", MakeOrderingScene(Id++, Identity, Identity, false, true, false), false);
	Compare("separate-meshes", WithoutSharedMesh(MakeInstanceScene(Id++, Identity, Identity)), false);
}
}

int main(int Argc, char **Argv) {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	try {
		bool Observe = false, Extended = false, OpaqueOrdering = false, InstancingOrdering = false,
			SceneEnvelopePixels = false, ContiguousInstancing = false;
		float Near = .1f, Far = 100000;
		std::string Projection = "production";
		std::filesystem::path Output;
		for (int Index = 1; Index < Argc; ++Index) {
			const std::string Argument = Argv[Index];
			if (Argument == "--observe") Observe = true;
			else if (Argument == "--extended") Extended = true;
			else if (Argument == "--opaque-ordering") OpaqueOrdering = true;
			else if (Argument == "--instancing-ordering") InstancingOrdering = true;
			else if (Argument == "--scene-envelope-pixels") SceneEnvelopePixels = true;
			else if (Argument == "--contiguous-instancing") ContiguousInstancing = true;
			else if (Argument.starts_with("--near=")) Near = std::stof(Argument.substr(7));
			else if (Argument.starts_with("--far=")) Far = std::stof(Argument.substr(6));
			else if (Argument.starts_with("--projection=")) Projection = Argument.substr(13);
			else if (Argument.starts_with("--output=")) Output = Argument.substr(9);
			else throw std::invalid_argument("Unknown depth test argument");
		}
		if (!Output.empty()) std::filesystem::create_directories(Output);
		if (OpaqueOrdering) {
			CheckOpaqueOrdering(Output);
			return 0;
		}
		if (InstancingOrdering) {
			CheckInstancingOrdering(Output);
			return 0;
		}
		if (SceneEnvelopePixels) {
			CheckSceneEnvelopePixels(Output);
			return 0;
		}
		if (ContiguousInstancing) {
			CheckContiguousInstancing(Output);
			return 0;
		}
		BootstrapNativeRuntimeSchema();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		std::array<std::shared_ptr<Part>, 2> Parts;
		for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
			Parts[Index] = std::make_shared<Part>();
			Parts[Index]->SetColor(Index == 0 ? Color3(.5f, .5f, 1) : Color3(1, 1, 1));
			Parts[Index]->SetParent(World);
		}
		RenderPublisher Publisher;
		EditorViewportRenderer Renderer(800, 600);
		auto *Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
		if (!Device) throw std::runtime_error(SDL_GetError());
		std::cout << "[Render:Depth] Driver=" << SDL_GetGPUDeviceDriver(Device)
			<< " D16=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
			<< " D24=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
			<< " D32=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) << '\n';
		SDL_DestroyGPUDevice(Device);
		auto Builder = SDLPipelineBuilder().SetDepthEnabled(true).SetDepthFormat(SDLSceneDepthFormat);
		const auto PipelineInfo = Builder.BuildInfo();
		if (SDLSceneDepthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
			PipelineInfo.depth_stencil_state.compare_op != SDL_GPU_COMPAREOP_LESS ||
			!PipelineInfo.depth_stencil_state.enable_depth_test || !PipelineInfo.depth_stencil_state.enable_depth_write)
			throw std::runtime_error("Unexpected scene depth state");
		std::size_t MissingTotal = 0, Cases = 0;
		std::cout << "[Render:Depth] Near=" << Near << " Far=" << Far << " Projection=" << Projection << '\n';
		for (unsigned Variant = 0; Variant < 3; ++Variant) {
			const double Scale = Variant == 2 ? .1 : 1;
			for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
				Parts[Index]->SetSize(glm::vec3(Scene[Index].Size * Scale));
				Parts[Index]->SetCFrame(CFrame(glm::vec3(Scene[Index].Center * Scale)));
				Parts[Index]->SetCastShadow(Variant == 0);
			}
			for (int Distance : {200, 500, 1000, 2000}) for (int Angle : {0, 45, 90}) {
				const double Radians = glm::radians(static_cast<double>(Angle));
				const auto Target = glm::vec3(glm::dvec3(-10.5, 15, -28.75) * Scale);
				const auto Position = Target + glm::vec3(glm::dvec3(std::cos(Radians), .55, std::sin(Radians)) * (Distance * Scale));
				auto Input = MakeLookAtRenderCameraInput(Position, Target, {0, 1, 0}, 60);
				Input.NearPlane = Near; Input.FarPlane = Far;
				auto Value = std::make_shared<RenderPublication>(*Publisher.Publish(*World, Input, 800, 600));
				auto &Camera = Value->Frame.Camera;
				if (Projection == "zo") Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(60.0f), 800.0f/600, Near, Far);
				else if (Projection == "no") Camera.ProjectionMatrix = glm::perspectiveRH_NO(glm::radians(60.0f), 800.0f/600, Near, Far);
				else if (Projection != "production") throw std::invalid_argument("Unknown projection");
				Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
				if (Cases == 0) std::cout << "[Render:Depth] Matrix22=" << Camera.ProjectionMatrix[2][2]
					<< " Matrix32=" << Camera.ProjectionMatrix[3][2] << '\n';
				const auto Frame = Renderer.Capture(Value);
				const auto Result = Measure(Frame, Camera, Scale);
				const auto Name = std::to_string(Variant) + "-" + std::to_string(Distance) + "-" + std::to_string(Angle);
				std::cout << "[Render:Depth] " << Name << " missing=" << Result.Missing << " expected=" << Result.Expected << '\n';
				if (!Output.empty()) WriteFrame(Output / (Name + ".ppm"), Frame);
				if (!Result.Expected) throw std::runtime_error("Empty coverage oracle");
				MissingTotal += Result.Missing; ++Cases;
			}
		}
		if (Extended) {
			// Independent occluders with known, nonzero surface separation.
			// Near/far probes also distinguish API clipping from depth rejection.
			for (const auto &[Depth, Gap, Visible] : std::array<std::tuple<float, float, bool>, 7>{{
				{.15f, .001f, true}, {.05f, .001f, false}, {1, .001f, true},
				{500, .5f, true}, {2000, 8, true}, {10000, 256, true}, {110000, 256, false}}}) {
				Parts[0]->SetSize({Depth, Depth, .001f});
				Parts[0]->SetCFrame(CFrame(glm::vec3(0, 0, -Depth - Gap)));
				Parts[1]->SetSize({Depth * .5f, Depth * .5f, .001f});
				Parts[1]->SetCFrame(CFrame(glm::vec3(0, 0, -Depth)));
				for (auto &PartValue : Parts) PartValue->SetCastShadow(false);
				const auto Input = MakeLookAtRenderCameraInput({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 60);
				const auto Frame = Renderer.Capture(Publisher.Publish(*World, Input, 800, 600));
				std::size_t Wrong = 0;
				for (unsigned Y = 294; Y <= 306; ++Y) for (unsigned X = 394; X <= 406; ++X) {
					const auto Pixel = (Y * Frame.Width + X) * 3;
					const bool White = Frame.RgbPixels[Pixel] > 20 && Frame.RgbPixels[Pixel] == Frame.RgbPixels[Pixel+1] && Frame.RgbPixels[Pixel+1] == Frame.RgbPixels[Pixel+2];
					Wrong += White != Visible;
				}
				std::cout << "[Render:Depth] separation depth=" << Depth << " gap=" << Gap << " visible=" << Visible << " wrong=" << Wrong << '\n';
				MissingTotal += Wrong;
			}
			for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
				Parts[Index]->SetSize(glm::vec3(Scene[Index].Size));
				Parts[Index]->SetCFrame(CFrame(glm::vec3(Scene[Index].Center)));
				Parts[Index]->SetCastShadow(true);
			}
			SDLRenderer Runtime({800, 600}, {.Offscreen = true, .WaitForGpuCompletion = true});
			RenderPublisher RuntimePublisher;
			for (const auto &[Width, Height] : std::array<std::pair<unsigned, unsigned>, 5>{{{1920,1080},{2560,1440},{3840,2160},{320,240},{800,600}}}) {
				const auto Begin = std::chrono::steady_clock::now();
				Renderer.Resize(Width, Height);
				Publisher.RequestFullResync();
				const auto ResizeUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count();
				const auto Input = MakeLookAtRenderCameraInput({343.0534f,290,-28.75f + 353.5534f}, {-10.5f,15,-28.75f}, {0,1,0}, 60);
				std::vector<long long> Times;
				for (int Index = 0; Index < 24; ++Index) {
					const auto View = Renderer.CaptureBgra(Publisher.Publish(*World, Input, Width, Height));
					if (Index >= 4) Times.push_back(View.Total.count());
				}
				std::sort(Times.begin(), Times.end());
				const auto Value = Publisher.Publish(*World, Input, Width, Height);
				const auto Frame = Renderer.Capture(Value);
				const auto Result = Measure(Frame, Value->Frame.Camera, 1);
				MissingTotal += Result.Missing;
				Runtime.Resize(Width, Height);
				Runtime.Draw(RuntimePublisher.Publish(*World, Input, Width, Height));
				Runtime.WaitForIdle();
				std::cout << "[Render:Depth] resize=" << Width << 'x' << Height << " resizeUs=" << ResizeUs
					<< " captureMedianUs=" << Times[Times.size()/2] << " captureMaxUs=" << Times.back() << " missing=" << Result.Missing << '\n';
			}
			if (Runtime.GetMetrics().FramesSubmitted != 5) throw std::runtime_error("Runtime resize path did not submit all frames");
		}
		Game->Destroy();
		std::cout << "[Render:Depth] Cases=" << Cases << " MissingTotal=" << MissingTotal << '\n';
		return Observe || MissingTotal == 0 ? 0 : 1;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Depth] " << Error.what() << '\n';
		return 1;
	}
}
