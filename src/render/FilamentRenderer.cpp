// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/FilamentRenderer.hpp"

#include "gargantuan/render/PrimitiveMeshes.hpp"

#include <SDL3/SDL.h>

#include <backend/DriverEnums.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <math/mat4.h>
#include <math/vec3.h>
#include <math/vec4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;

		static constexpr std::uint8_t BENCHMARK_MATERIAL_PACKAGE[] = {
#include "GargantuanBenchmarkMaterial.h"
		};

		struct FilamentVertex {
			filament::math::float3 Position;
			filament::math::float4 TangentFrame;
		};

		struct CpuMesh {
			std::vector<FilamentVertex> Vertices;
			std::vector<std::uint32_t> Indices;
		};

		double Milliseconds(Clock::duration Duration) {
			return std::chrono::duration<double, std::milli>(Duration).count();
		}

		filament::math::float3 ToFilament(const glm::vec3 &Value) {
			return {Value.x, Value.y, Value.z};
		}

		filament::math::float4 ToFilament(const glm::vec4 &Value) {
			return {Value.x, Value.y, Value.z, Value.w};
		}

		filament::math::mat4f ToFilament(const glm::mat4 &Value) {
			return {
				filament::math::float4{Value[0].x, Value[0].y, Value[0].z, Value[0].w},
				filament::math::float4{Value[1].x, Value[1].y, Value[1].z, Value[1].w},
				filament::math::float4{Value[2].x, Value[2].y, Value[2].z, Value[2].w},
				filament::math::float4{Value[3].x, Value[3].y, Value[3].z, Value[3].w},
			};
		}

		filament::math::float4 TangentFrame(const glm::vec3 &NormalValue) {
			const auto Normal = glm::normalize(NormalValue);
			const auto Reference = std::abs(Normal.z) < 0.999f ? glm::vec3{0.0f, 0.0f, 1.0f}
				: glm::vec3{0.0f, 1.0f, 0.0f};
			const auto Tangent = glm::normalize(glm::cross(Reference, Normal));
			const auto Bitangent = glm::cross(Normal, Tangent);
			auto Orientation = glm::quat_cast(glm::mat3(Tangent, Bitangent, Normal));
			if (Orientation.w < 0.0f) Orientation = -Orientation;
			return {Orientation.x, Orientation.y, Orientation.z, Orientation.w};
		}

		void AddVertex(CpuMesh &MeshValue, const glm::vec3 &Position, const glm::vec3 &Normal) {
			MeshValue.Vertices.push_back({ToFilament(Position), TangentFrame(Normal)});
		}

		CpuMesh BuildBlockMesh() {
			const auto Source = PrimitiveMeshes::Block();
			CpuMesh Result;
			Result.Vertices.reserve(Source.Vertices.size());
			Result.Indices = Source.Indices;
			for (const auto &VertexValue : Source.Vertices)
				AddVertex(Result, VertexValue.Position, VertexValue.Normal);
			return Result;
		}

		CpuMesh BuildSphereMesh() {
			constexpr std::uint32_t SEGMENTS = 16;
			constexpr std::uint32_t RINGS = 8;
			constexpr float PI = 3.14159265358979323846f;
			CpuMesh Result;
			for (std::uint32_t Ring = 0; Ring <= RINGS; ++Ring) {
				const float V = static_cast<float>(Ring) / static_cast<float>(RINGS);
				const float Phi = V * PI;
				for (std::uint32_t Segment = 0; Segment <= SEGMENTS; ++Segment) {
					const float U = static_cast<float>(Segment) / static_cast<float>(SEGMENTS);
					const float Theta = U * 2.0f * PI;
					const glm::vec3 Normal{
						std::sin(Phi) * std::cos(Theta),
						std::cos(Phi),
						std::sin(Phi) * std::sin(Theta),
					};
					AddVertex(Result, Normal * 0.5f, Normal);
				}
			}
			for (std::uint32_t Ring = 0; Ring < RINGS; ++Ring) {
				for (std::uint32_t Segment = 0; Segment < SEGMENTS; ++Segment) {
					const auto First = Ring * (SEGMENTS + 1) + Segment;
					const auto Second = First + SEGMENTS + 1;
					Result.Indices.insert(Result.Indices.end(), {
						First, Second, First + 1, Second, Second + 1, First + 1,
					});
				}
			}
			return Result;
		}

		CpuMesh BuildCylinderMesh() {
			constexpr std::uint32_t SEGMENTS = 16;
			constexpr float PI = 3.14159265358979323846f;
			CpuMesh Result;
			for (std::uint32_t Segment = 0; Segment <= SEGMENTS; ++Segment) {
				const float Angle = static_cast<float>(Segment) / static_cast<float>(SEGMENTS) * 2.0f * PI;
				const glm::vec3 Normal{std::cos(Angle), 0.0f, std::sin(Angle)};
				AddVertex(Result, {Normal.x * 0.5f, -0.5f, Normal.z * 0.5f}, Normal);
				AddVertex(Result, {Normal.x * 0.5f, 0.5f, Normal.z * 0.5f}, Normal);
			}
			for (std::uint32_t Segment = 0; Segment < SEGMENTS; ++Segment) {
				const auto First = Segment * 2;
				Result.Indices.insert(Result.Indices.end(), {
					First, First + 1, First + 2, First + 1, First + 3, First + 2,
				});
			}

			const auto BottomCenter = static_cast<std::uint32_t>(Result.Vertices.size());
			AddVertex(Result, {0.0f, -0.5f, 0.0f}, {0.0f, -1.0f, 0.0f});
			const auto TopCenter = static_cast<std::uint32_t>(Result.Vertices.size());
			AddVertex(Result, {0.0f, 0.5f, 0.0f}, {0.0f, 1.0f, 0.0f});
			const auto CapStart = static_cast<std::uint32_t>(Result.Vertices.size());
			for (std::uint32_t Segment = 0; Segment <= SEGMENTS; ++Segment) {
				const float Angle = static_cast<float>(Segment) / static_cast<float>(SEGMENTS) * 2.0f * PI;
				const glm::vec3 Position{std::cos(Angle) * 0.5f, 0.0f, std::sin(Angle) * 0.5f};
				AddVertex(Result, {Position.x, -0.5f, Position.z}, {0.0f, -1.0f, 0.0f});
				AddVertex(Result, {Position.x, 0.5f, Position.z}, {0.0f, 1.0f, 0.0f});
			}
			for (std::uint32_t Segment = 0; Segment < SEGMENTS; ++Segment) {
				const auto Bottom = CapStart + Segment * 2;
				const auto Top = Bottom + 1;
				Result.Indices.insert(Result.Indices.end(), {
					BottomCenter, Bottom + 2, Bottom,
					TopCenter, Top, Top + 2,
				});
			}
			return Result;
		}

		bool ItemsEqual(const RenderItem &Left, const RenderItem &Right) {
			return Left.Object == Right.Object && Left.Geometry == Right.Geometry &&
				Left.ModelMatrix == Right.ModelMatrix && Left.InverseModelMatrix == Right.InverseModelMatrix &&
				Left.Color == Right.Color && Left.CastShadow == Right.CastShadow;
		}

		struct ColorKey {
			std::array<std::uint32_t, 4> Values{};

			friend bool operator==(const ColorKey &, const ColorKey &) = default;
		};

		struct ColorKeyHash {
			std::size_t operator()(const ColorKey &Key) const noexcept {
				std::size_t Result = 0;
				for (const auto Value : Key.Values)
					Result ^= static_cast<std::size_t>(Value) + 0x9e3779b9u + (Result << 6u) + (Result >> 2u);
				return Result;
			}
		};

		ColorKey MakeColorKey(const glm::vec4 &Color) {
			return {{
				std::bit_cast<std::uint32_t>(Color.r),
				std::bit_cast<std::uint32_t>(Color.g),
				std::bit_cast<std::uint32_t>(Color.b),
				std::bit_cast<std::uint32_t>(Color.a),
			}};
		}
	} // namespace

	struct FilamentRenderer::Impl {
		struct MeshResource {
			CpuMesh CpuData;
			filament::VertexBuffer *Vertices = nullptr;
			filament::IndexBuffer *Indices = nullptr;
		};

		struct MaterialRecord {
			filament::MaterialInstance *Instance = nullptr;
			std::size_t References = 0;
		};

		struct Entry {
			RenderItem Item;
			utils::Entity Entity;
			ColorKey MaterialColor;
		};

		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		bool Headless = false;
		bool ShadowsEnabled = false;
		SDL_Window *Window = nullptr;
		filament::Engine *Engine = nullptr;
		filament::Renderer *Renderer = nullptr;
		filament::SwapChain *SwapChain = nullptr;
		filament::View *View = nullptr;
		filament::Scene *Scene = nullptr;
		filament::Camera *Camera = nullptr;
		filament::Material *Material = nullptr;
		utils::Entity CameraEntity;
		utils::Entity LightEntity;
		std::array<MeshResource, 3> Meshes;
		std::unordered_map<ColorKey, MaterialRecord, ColorKeyHash> Materials;
		std::unordered_map<ObjectId, Entry> Entries;
		FilamentFrameMetrics LastMetrics;

		Impl(const Vector2 &ViewportSize, bool HeadlessValue, bool ShadowsEnabledValue)
			: Width(static_cast<std::uint32_t>(ViewportSize.GetX())),
			  Height(static_cast<std::uint32_t>(ViewportSize.GetY())), Headless(HeadlessValue),
			  ShadowsEnabled(ShadowsEnabledValue) {
			if (Width == 0 || Height == 0) throw std::invalid_argument("Filament viewport must be nonzero");
			try {
				InitializeWindow();
				InitializeEngine();
				InitializeScene();
			} catch (...) {
				Destroy();
				throw;
			}
		}

		~Impl() { Destroy(); }

		void InitializeWindow() {
			if (Headless) return;
			Window = SDL_CreateWindow(
				"Gargantuan Filament Benchmark",
				static_cast<int>(Width),
				static_cast<int>(Height),
				SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIGH_PIXEL_DENSITY
			);
			if (!Window) throw std::runtime_error(std::string("Failed to create Filament benchmark window: ") + SDL_GetError());
			int PixelWidth = 0;
			int PixelHeight = 0;
			if (!SDL_GetWindowSizeInPixels(Window, &PixelWidth, &PixelHeight) || PixelWidth < 1 || PixelHeight < 1)
				throw std::runtime_error(std::string("Failed to query Filament benchmark window size: ") + SDL_GetError());
			Width = static_cast<std::uint32_t>(PixelWidth);
			Height = static_cast<std::uint32_t>(PixelHeight);
		}

		void InitializeEngine() {
			Engine = filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
			if (!Engine) throw std::runtime_error("Filament could not initialize its Vulkan engine");
			Engine->setAutomaticInstancingEnabled(true);

			if (Headless) {
				SwapChain = Engine->createSwapChain(Width, Height);
			} else {
#if defined(_WIN32)
				const auto Properties = SDL_GetWindowProperties(Window);
				auto *NativeWindow = SDL_GetPointerProperty(Properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
				if (!NativeWindow) throw std::runtime_error("SDL did not expose a Win32 HWND for Filament");
				SwapChain = Engine->createSwapChain(NativeWindow);
#else
				throw std::runtime_error("The experimental Filament window adapter is implemented only for Win32");
#endif
			}
			if (!SwapChain) throw std::runtime_error("Filament could not create its swapchain");
			Renderer = Engine->createRenderer();
			View = Engine->createView();
			Scene = Engine->createScene();
			if (!Renderer || !View || !Scene) throw std::runtime_error("Filament could not construct its frame objects");
		}

		void InitializeScene() {
			CameraEntity = utils::EntityManager::get().create();
			if (CameraEntity.isNull()) throw std::runtime_error("Filament could not allocate a camera entity");
			Camera = Engine->createCamera(CameraEntity);
			if (!Camera) throw std::runtime_error("Filament could not create a camera component");
			View->setCamera(Camera);
			View->setScene(Scene);
			View->setViewport({0, 0, Width, Height});
			View->setPostProcessingEnabled(false);
			View->setAntiAliasing(filament::View::AntiAliasing::NONE);
			View->setFrustumCullingEnabled(true);
			View->setShadowingEnabled(ShadowsEnabled);

			LightEntity = utils::EntityManager::get().create();
			if (LightEntity.isNull()) throw std::runtime_error("Filament could not allocate a light entity");
			const auto LightResult = filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
				.color({1.0f, 1.0f, 1.0f})
				.intensity(100000.0f)
				.direction({-0.75f, -1.0f, -0.5f})
				.castShadows(ShadowsEnabled)
				.build(*Engine, LightEntity);
			if (LightResult != filament::LightManager::Builder::Success)
				throw std::runtime_error("Filament could not build its directional light");
			Scene->addEntity(LightEntity);

			Material = filament::Material::Builder()
				.package(BENCHMARK_MATERIAL_PACKAGE, sizeof(BENCHMARK_MATERIAL_PACKAGE))
				.build(*Engine);
			if (!Material) throw std::runtime_error("Filament rejected the benchmark material package");

			CreateMesh(Meshes[0], BuildBlockMesh());
			CreateMesh(Meshes[1], BuildSphereMesh());
			CreateMesh(Meshes[2], BuildCylinderMesh());
			Engine->flushAndWait();
		}

		void CreateMesh(MeshResource &Target, CpuMesh Source) {
			Target.CpuData = std::move(Source);
			Target.Vertices = filament::VertexBuffer::Builder()
				.vertexCount(static_cast<std::uint32_t>(Target.CpuData.Vertices.size()))
				.bufferCount(1)
				.attribute(
					filament::VertexAttribute::POSITION,
					0,
					filament::VertexBuffer::AttributeType::FLOAT3,
					offsetof(FilamentVertex, Position),
					sizeof(FilamentVertex)
				)
				.attribute(
					filament::VertexAttribute::TANGENTS,
					0,
					filament::VertexBuffer::AttributeType::FLOAT4,
					offsetof(FilamentVertex, TangentFrame),
					sizeof(FilamentVertex)
				)
				.build(*Engine);
			Target.Indices = filament::IndexBuffer::Builder()
				.indexCount(static_cast<std::uint32_t>(Target.CpuData.Indices.size()))
				.bufferType(filament::IndexBuffer::IndexType::UINT)
				.build(*Engine);
			if (!Target.Vertices || !Target.Indices) throw std::runtime_error("Filament could not create primitive buffers");
			Target.Vertices->setBufferAt(*Engine, 0, filament::VertexBuffer::BufferDescriptor(
				Target.CpuData.Vertices.data(), Target.CpuData.Vertices.size() * sizeof(FilamentVertex)
			));
			Target.Indices->setBuffer(*Engine, filament::IndexBuffer::BufferDescriptor(
				Target.CpuData.Indices.data(), Target.CpuData.Indices.size() * sizeof(std::uint32_t)
			));
		}

		MeshResource &GetMesh(RenderGeometry Geometry) {
			switch (Geometry) {
				case RenderGeometry::Ball: return Meshes[1];
				case RenderGeometry::Cylinder: return Meshes[2];
				case RenderGeometry::Block:
				case RenderGeometry::Wedge:
				case RenderGeometry::CornerWedge: return Meshes[0];
			}
			return Meshes[0];
		}

		filament::MaterialInstance *AcquireMaterial(const glm::vec4 &Color) {
			const auto Key = MakeColorKey(Color);
			auto Existing = Materials.find(Key);
			if (Existing != Materials.end()) {
				++Existing->second.References;
				return Existing->second.Instance;
			}

			auto *Instance = Material->createInstance();
			if (!Instance) throw std::runtime_error("Filament could not create a material instance");
			Instance->setParameter("baseColor", ToFilament(Color));
			Materials.emplace(Key, MaterialRecord{Instance, 1});
			return Instance;
		}

		void ReleaseMaterial(const ColorKey &Key) {
			auto Existing = Materials.find(Key);
			if (Existing == Materials.end()) return;
			if (--Existing->second.References != 0) return;
			if (Engine && Existing->second.Instance) Engine->destroy(Existing->second.Instance);
			Materials.erase(Existing);
		}

		Entry CreateEntry(const RenderItem &Item) {
			const auto MaterialKey = MakeColorKey(Item.Color);
			auto *MaterialInstance = AcquireMaterial(Item.Color);
			auto Entity = utils::EntityManager::get().create();
			if (Entity.isNull()) {
				ReleaseMaterial(MaterialKey);
				throw std::runtime_error("Filament entity capacity was exhausted");
			}

			try {
				auto &Mesh = GetMesh(Item.Geometry);
				const auto Result = filament::RenderableManager::Builder(1)
					.boundingBox({{0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}})
					.material(0, MaterialInstance)
					.geometry(
						0,
						filament::RenderableManager::PrimitiveType::TRIANGLES,
						Mesh.Vertices,
						Mesh.Indices,
						0,
						static_cast<std::uint32_t>(Mesh.CpuData.Indices.size())
					)
					.culling(true)
					.castShadows(ShadowsEnabled && Item.CastShadow)
					.receiveShadows(ShadowsEnabled)
					.build(*Engine, Entity);
				if (Result != filament::RenderableManager::Builder::Success)
					throw std::runtime_error("Filament could not build a renderable component");
				auto &Transforms = Engine->getTransformManager();
				const auto Transform = Transforms.getInstance(Entity);
				if (!Transform) throw std::runtime_error("Filament did not create a renderable transform component");
				Transforms.setTransform(Transform, ToFilament(Item.ModelMatrix));
				Scene->addEntity(Entity);
				return {Item, Entity, MaterialKey};
			} catch (...) {
				Engine->destroy(Entity);
				utils::EntityManager::get().destroy(Entity);
				ReleaseMaterial(MaterialKey);
				throw;
			}
		}

		void UpdateEntry(Entry &Existing, const RenderItem &Item) {
			auto &Renderables = Engine->getRenderableManager();
			auto &Transforms = Engine->getTransformManager();
			const auto Renderable = Renderables.getInstance(Existing.Entity);
			const auto Transform = Transforms.getInstance(Existing.Entity);
			if (!Renderable || !Transform) throw std::runtime_error("Filament projection lost an entity component");

			if (Existing.Item.ModelMatrix != Item.ModelMatrix)
				Transforms.setTransform(Transform, ToFilament(Item.ModelMatrix));
			if (Existing.Item.Geometry != Item.Geometry) {
				auto &Mesh = GetMesh(Item.Geometry);
				Renderables.setGeometryAt(
					Renderable,
					0,
					filament::RenderableManager::PrimitiveType::TRIANGLES,
					Mesh.Vertices,
					Mesh.Indices,
					0,
					static_cast<std::uint32_t>(Mesh.CpuData.Indices.size())
				);
			}
			if (Existing.Item.Color != Item.Color) {
				const auto ReplacementKey = MakeColorKey(Item.Color);
				auto *Replacement = AcquireMaterial(Item.Color);
				Renderables.setMaterialInstanceAt(Renderable, 0, Replacement);
				ReleaseMaterial(Existing.MaterialColor);
				Existing.MaterialColor = ReplacementKey;
			}
			if (Existing.Item.CastShadow != Item.CastShadow)
				Renderables.setCastShadows(Renderable, ShadowsEnabled && Item.CastShadow);
			Existing.Item = Item;
		}

		void RemoveEntry(Entry &Existing) {
			if (Scene) Scene->remove(Existing.Entity);
			if (Engine) Engine->destroy(Existing.Entity);
			utils::EntityManager::get().destroy(Existing.Entity);
			ReleaseMaterial(Existing.MaterialColor);
		}

		void ClearProjection() {
			for (auto &[Object, Existing] : Entries) {
				(void)Object;
				RemoveEntry(Existing);
			}
			Entries.clear();
		}

		void UpdateCameraAndLight(const RenderSnapshot &Snapshot) {
			const auto &CameraValue = Snapshot.Camera;
			const auto Aspect = static_cast<double>(Snapshot.ViewportWidth) /
				static_cast<double>(Snapshot.ViewportHeight);
			Camera->setProjection(
				static_cast<double>(CameraValue.VerticalFieldOfView),
				Aspect,
				static_cast<double>(CameraValue.NearPlane),
				static_cast<double>(CameraValue.FarPlane),
				filament::Camera::Fov::VERTICAL
			);
			const auto Eye = ToFilament(CameraValue.Position);
			const auto Target = ToFilament(CameraValue.Position + CameraValue.LookDirection);
			Camera->lookAt(Eye, Target, ToFilament(CameraValue.UpDirection));

			auto &Lights = Engine->getLightManager();
			const auto Light = Lights.getInstance(LightEntity);
			if (Light) Lights.setDirection(Light, ToFilament(-Snapshot.LightDirection));
		}

		void Draw(const RenderSnapshot &Snapshot) {
			if (!Engine || !Renderer || !SwapChain || !View || !Scene)
				throw std::logic_error("FilamentRenderer is not initialized");
			if (Snapshot.Id == InvalidRenderSnapshotId)
				throw std::invalid_argument("FilamentRenderer requires a valid RenderSnapshot identity");
			if (Snapshot.ViewportWidth != Width || Snapshot.ViewportHeight != Height)
				throw std::invalid_argument("Filament RenderSnapshot viewport does not match its render target");

			const auto ScanStart = Clock::now();
			std::unordered_set<ObjectId> Seen;
			Seen.reserve(Snapshot.Items.size());
			std::vector<const RenderItem *> Created;
			std::vector<const RenderItem *> Updated;
			std::vector<ObjectId> Removed;
			Created.reserve(Snapshot.Items.size());
			Updated.reserve(Snapshot.Items.size());
			for (const auto &Item : Snapshot.Items) {
				if (!Item.Object.IsValid()) throw std::invalid_argument("Filament projection requires valid ObjectId values");
				if (!Seen.insert(Item.Object).second)
					throw std::invalid_argument("Filament projection rejects duplicate ObjectId values");
				auto Existing = Entries.find(Item.Object);
				if (Existing == Entries.end()) Created.push_back(&Item);
				else if (!ItemsEqual(Existing->second.Item, Item)) Updated.push_back(&Item);
			}
			for (const auto &[Object, Existing] : Entries) {
				(void)Existing;
				if (!Seen.contains(Object)) Removed.push_back(Object);
			}
			LastMetrics = {};
			LastMetrics.Changes.Created = Created.size();
			LastMetrics.Changes.Updated = Updated.size();
			LastMetrics.Changes.Removed = Removed.size();
			LastMetrics.Changes.Unchanged = Snapshot.Items.size() - Created.size() - Updated.size();
			LastMetrics.ProjectionScanMilliseconds = Milliseconds(Clock::now() - ScanStart);

			const auto ApplyStart = Clock::now();
			try {
				Entries.reserve(Snapshot.Items.size());
				for (const auto *Item : Created) Entries.emplace(Item->Object, CreateEntry(*Item));
				for (const auto *Item : Updated) UpdateEntry(Entries.at(Item->Object), *Item);
				for (const auto Object : Removed) {
					auto Existing = Entries.find(Object);
					if (Existing == Entries.end()) continue;
					RemoveEntry(Existing->second);
					Entries.erase(Existing);
				}
				UpdateCameraAndLight(Snapshot);
			} catch (...) {
				ClearProjection();
				throw;
			}
			LastMetrics.ChangedObjectApplyMilliseconds = Milliseconds(Clock::now() - ApplyStart);
			LastMetrics.ProjectedObjects = Entries.size();

			const auto SubmissionStart = Clock::now();
			// The public API explicitly permits a caller to proceed when frame pacing recommends a skip.
			// A benchmark must submit every publication or it would compare different work.
			static_cast<void>(Renderer->beginFrame(SwapChain));
			Renderer->render(View);
			Renderer->endFrame();
			LastMetrics.RendererSubmissionMilliseconds = Milliseconds(Clock::now() - SubmissionStart);
			const auto History = Renderer->getFrameInfoHistory(1);
			if (!History.empty() && History.back().gpuFrameDuration >= 0)
				LastMetrics.GpuFrameMilliseconds = static_cast<double>(History.back().gpuFrameDuration) / 1'000'000.0;
		}

		void Resize(int WidthValue, int HeightValue) {
			if (WidthValue < 1 || HeightValue < 1) return;
			if (!Engine || !SwapChain || !View) throw std::logic_error("Cannot resize an uninitialized FilamentRenderer");
			const auto NewWidth = static_cast<std::uint32_t>(WidthValue);
			const auto NewHeight = static_cast<std::uint32_t>(HeightValue);
			if (NewWidth == Width && NewHeight == Height) return;
			if (Headless) {
				Engine->flushAndWait();
				Engine->destroy(SwapChain);
				SwapChain = Engine->createSwapChain(NewWidth, NewHeight);
				if (!SwapChain) throw std::runtime_error("Filament could not replace its headless swapchain");
			} else {
				if (!SDL_SetWindowSize(Window, WidthValue, HeightValue))
					throw std::runtime_error(std::string("Failed to resize Filament window: ") + SDL_GetError());
			}
			Width = NewWidth;
			Height = NewHeight;
			View->setViewport({0, 0, Width, Height});
		}

		void Destroy() {
			if (Engine) Engine->flushAndWait();
			ClearProjection();
			if (Engine && LightEntity) Engine->destroy(LightEntity);
			if (LightEntity) utils::EntityManager::get().destroy(LightEntity);
			LightEntity = {};
			if (Engine && CameraEntity) Engine->destroyCameraComponent(CameraEntity);
			if (CameraEntity) utils::EntityManager::get().destroy(CameraEntity);
			CameraEntity = {};
			Camera = nullptr;
			for (auto &[Key, Record] : Materials) {
				(void)Key;
				if (Engine && Record.Instance) Engine->destroy(Record.Instance);
			}
			Materials.clear();
			if (Engine && Material) Engine->destroy(Material);
			Material = nullptr;
			for (auto &Mesh : Meshes) {
				if (Engine && Mesh.Vertices) Engine->destroy(Mesh.Vertices);
				if (Engine && Mesh.Indices) Engine->destroy(Mesh.Indices);
				Mesh.Vertices = nullptr;
				Mesh.Indices = nullptr;
				Mesh.CpuData = {};
			}
			if (Engine && View) Engine->destroy(View);
			if (Engine && Scene) Engine->destroy(Scene);
			if (Engine && Renderer) Engine->destroy(Renderer);
			if (Engine && SwapChain) Engine->destroy(SwapChain);
			View = nullptr;
			Scene = nullptr;
			Renderer = nullptr;
			SwapChain = nullptr;
			if (Engine) filament::Engine::destroy(&Engine);
			if (Window) SDL_DestroyWindow(Window);
			Window = nullptr;
			Width = 0;
			Height = 0;
		}
	};

	FilamentRenderer::FilamentRenderer(const Vector2 &ViewportSize, bool Headless, bool ShadowsEnabled)
		: BaseRenderer(ViewportSize), Implementation(std::make_unique<Impl>(ViewportSize, Headless, ShadowsEnabled)) {}

	FilamentRenderer::~FilamentRenderer() { Destroy(); }

	void FilamentRenderer::Draw(RenderSnapshotPtr Snapshot) {
		if (!Snapshot) throw std::invalid_argument("FilamentRenderer requires an immutable RenderSnapshot");
		if (!Implementation) throw std::logic_error("FilamentRenderer is destroyed");
		Implementation->Draw(*Snapshot);
	}

	void FilamentRenderer::Resize(int WidthValue, int HeightValue) {
		if (!Implementation) throw std::logic_error("FilamentRenderer is destroyed");
		Implementation->Resize(WidthValue, HeightValue);
	}

	void FilamentRenderer::Destroy() { Implementation.reset(); }

	std::pair<std::uint32_t, std::uint32_t> FilamentRenderer::GetViewportSize() const {
		return Implementation ? std::pair{Implementation->Width, Implementation->Height} : std::pair{0u, 0u};
	}

	FilamentFrameMetrics FilamentRenderer::GetLastMetrics() const {
		return Implementation ? Implementation->LastMetrics : FilamentFrameMetrics{};
	}

	std::string FilamentRenderer::GetBackendName() const {
		if (!Implementation || !Implementation->Engine) return "destroyed";
		switch (Implementation->Engine->getBackend()) {
			case filament::Engine::Backend::OPENGL: return "OpenGL";
			case filament::Engine::Backend::VULKAN: return "Vulkan";
			case filament::Engine::Backend::METAL: return "Metal";
			case filament::Engine::Backend::NOOP: return "Noop";
			case filament::Engine::Backend::DEFAULT: return "Default";
			default: return "Other";
		}
	}

	std::size_t FilamentRenderer::GetMaxAutomaticInstances() const {
		return Implementation && Implementation->Engine ? Implementation->Engine->getMaxAutomaticInstances() : 0;
	}

	bool FilamentRenderer::IsAutomaticInstancingEnabled() const {
		return Implementation && Implementation->Engine && Implementation->Engine->isAutomaticInstancingEnabled();
	}

	std::optional<std::size_t> FilamentRenderer::GetRendererOwnedBytes() const { return std::nullopt; }

	std::vector<double> FilamentRenderer::GetGpuFrameHistoryMilliseconds(std::size_t MaximumFrames) {
		std::vector<double> Result;
		if (!Implementation || !Implementation->Engine || !Implementation->Renderer || MaximumFrames == 0) return Result;
		Implementation->Engine->flushAndWait();
		const auto History = Implementation->Renderer->getFrameInfoHistory(std::min(
			MaximumFrames,
			Implementation->Renderer->getMaxFrameHistorySize()
		));
		Result.reserve(History.size());
		for (const auto &Frame : History) {
			if (Frame.gpuFrameDuration >= 0)
				Result.push_back(static_cast<double>(Frame.gpuFrameDuration) / 1'000'000.0);
		}
		return Result;
	}

	void FilamentRenderer::FlushAndWait() {
		if (Implementation && Implementation->Engine) Implementation->Engine->flushAndWait();
	}
} // namespace gargantuan
