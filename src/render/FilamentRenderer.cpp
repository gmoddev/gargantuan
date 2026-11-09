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
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <math/mat4.h>
#include <math/vec2.h>
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
#include <span>
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
		static constexpr std::uint8_t MASKED_MATERIAL_PACKAGE[] = {
#include "GargantuanMaskedMaterial.h"
		};
		static constexpr std::uint8_t TRANSPARENT_MATERIAL_PACKAGE[] = {
#include "GargantuanTransparentMaterial.h"
		};

		constexpr std::uint32_t MAXIMUM_VIEWPORT_DIMENSION = 4096;
		constexpr std::size_t MAXIMUM_PENDING_UPLOAD_BYTES = 256u * 1024u * 1024u;

		struct FilamentVertex {
			filament::math::float3 Position;
			filament::math::float4 TangentFrame;
			filament::math::float2 UV;
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

		filament::math::float4 TangentFrame(const glm::vec3 &NormalValue, const glm::vec4 &TangentValue) {
			const auto NormalLength = glm::length(NormalValue);
			const auto Normal = NormalLength > 1.0e-8f && std::isfinite(NormalLength)
				? NormalValue / NormalLength : glm::vec3{0.0f, 1.0f, 0.0f};
			const auto Reference = std::abs(Normal.z) < 0.999f ? glm::vec3{0.0f, 0.0f, 1.0f}
				: glm::vec3{0.0f, 1.0f, 0.0f};
			const auto Orthogonal = glm::vec3(TangentValue) - Normal * glm::dot(glm::vec3(TangentValue), Normal);
			const auto TangentLength = glm::length(Orthogonal);
			const auto Tangent = TangentLength > 1.0e-8f && std::isfinite(TangentLength)
				? Orthogonal / TangentLength : glm::normalize(glm::cross(Reference, Normal));
			const auto Bitangent = glm::cross(Normal, Tangent);
			auto Orientation = glm::quat_cast(glm::mat3(Tangent, Bitangent, Normal));
			if (Orientation.w < 0.0f) Orientation = -Orientation;
			// Filament stores tangent handedness in the sign of the quaternion's W.
			Orientation.w = std::max(Orientation.w, 1.0e-7f);
			if (TangentValue.w < 0.0f) Orientation = -Orientation;
			return {Orientation.x, Orientation.y, Orientation.z, Orientation.w};
		}

		FilamentVertex MakeVertex(const glm::vec3 &Position, const glm::vec3 &Normal,
			const glm::vec4 &Tangent, const glm::vec2 &UV) {
			return {ToFilament(Position), TangentFrame(Normal, Tangent), {UV.x, UV.y}};
		}

		void AddVertex(CpuMesh &MeshValue, const glm::vec3 &Position, const glm::vec3 &Normal,
			const glm::vec2 &UV = glm::vec2(0.0f)) {
			MeshValue.Vertices.push_back(MakeVertex(Position, Normal, glm::vec4(0.0f), UV));
		}

		CpuMesh BuildPrimitiveMesh(Mesh Source) {
			CpuMesh Result;
			Result.Vertices.reserve(Source.Vertices.size());
			Result.Indices = std::move(Source.Indices);
			for (const auto &VertexValue : Source.Vertices)
				Result.Vertices.push_back(MakeVertex(VertexValue.Position, VertexValue.Normal, VertexValue.Tangent, VertexValue.UV));
			return Result;
		}

		std::shared_ptr<const std::vector<FilamentVertex>> ConvertVertices(std::span<const RenderVertex> Source) {
			auto Result = std::make_shared<std::vector<FilamentVertex>>();
			Result->reserve(Source.size());
			for (const auto &Vertex : Source)
				Result->push_back(MakeVertex(Vertex.Position, Vertex.Normal, Vertex.Tangent, Vertex.TextureCoordinate));
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
					AddVertex(Result, Normal * 0.5f, Normal, {U, V});
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

		filament::Box ToFilament(const RenderBounds &Bounds) {
			return {ToFilament(Bounds.Minimum * 0.5f + Bounds.Maximum * 0.5f),
				ToFilament(Bounds.Maximum * 0.5f - Bounds.Minimum * 0.5f)};
		}

		struct MaterialKey {
			std::array<std::uint32_t, 7> Factors{};
			std::optional<RenderTextureIdentity> BaseColor;
			std::optional<RenderTextureIdentity> Normal;
			std::optional<RenderTextureIdentity> MetallicRoughness;
			RenderOpacityMode Opacity = RenderOpacityMode::Opaque;
			bool DoubleSided = false;
			friend bool operator==(const MaterialKey &, const MaterialKey &) = default;
		};

		struct MaterialKeyHash {
			std::size_t operator()(const MaterialKey &Key) const noexcept {
				std::size_t Result = 0;
				const auto Append = [&](std::size_t Value) {
					Result ^= static_cast<std::size_t>(Value) + 0x9e3779b9u + (Result << 6u) + (Result >> 2u);
				};
				for (const auto Value : Key.Factors) Append(Value);
				Append(Key.BaseColor ? RenderTextureIdentityHash{}(*Key.BaseColor) : 0u);
				Append(Key.Normal ? RenderTextureIdentityHash{}(*Key.Normal) : 0u);
				Append(Key.MetallicRoughness ? RenderTextureIdentityHash{}(*Key.MetallicRoughness) : 0u);
				Append(static_cast<std::size_t>(Key.Opacity));
				Append(Key.DoubleSided);
				return Result;
			}
		};

		MaterialKey MakeMaterialKey(const RenderMaterialState &Material) {
			const auto &Color = Material.BaseColorFactor;
			const bool PhysicallyBased = Material.ShadingModel == RenderShadingModel::MetallicRoughness;
			return {{
				std::bit_cast<std::uint32_t>(Color.r),
				std::bit_cast<std::uint32_t>(Color.g),
				std::bit_cast<std::uint32_t>(Color.b),
				std::bit_cast<std::uint32_t>(Color.a),
				std::bit_cast<std::uint32_t>(PhysicallyBased ? Material.Metallic : 0.0f),
				std::bit_cast<std::uint32_t>(PhysicallyBased ? Material.Roughness : 1.0f),
				std::bit_cast<std::uint32_t>(Material.AlphaCutoff),
			}, Material.BaseColorTexture,
				PhysicallyBased ? Material.NormalTexture : std::nullopt,
				PhysicallyBased ? Material.MetallicRoughnessTexture : std::nullopt,
				Material.OpacityMode, Material.DoubleSided};
		}

		std::uint32_t CheckedViewportDimension(float Value) {
			if (!std::isfinite(Value) || Value < 1.0f || Value > MAXIMUM_VIEWPORT_DIMENSION)
				throw std::invalid_argument("Filament viewport dimension is outside the qualification limit");
			return static_cast<std::uint32_t>(Value);
		}
	} // namespace

	struct FilamentRenderer::Impl {
		struct MeshResource {
			filament::VertexBuffer *Vertices = nullptr;
			filament::IndexBuffer *Indices = nullptr;
			std::uint32_t IndexCount = 0;
			RenderBounds Bounds;
			std::unordered_set<ObjectId> Users;
		};

		struct TextureResource {
			filament::Texture *Linear = nullptr;
			filament::Texture *Color = nullptr;
		};

		struct UploadBudget {
			std::size_t Bytes = 0;
		};

		struct UploadLifetime {
			std::shared_ptr<UploadBudget> Budget;
			std::shared_ptr<const void> Data;
			std::size_t Bytes;
			~UploadLifetime() { Budget->Bytes -= Bytes; }
			static void Release(void *, std::size_t, void *Value) noexcept {
				delete static_cast<UploadLifetime *>(Value);
			}
		};

		struct MaterialRecord {
			filament::MaterialInstance *Instance = nullptr;
			std::size_t References = 0;
		};

		struct PrimitiveBinding {
			MaterialKey Material;
			std::uint32_t FirstIndex;
			std::uint32_t IndexCount;
		};

		struct Entry {
			RenderItem Item;
			std::optional<RenderMeshIdentity> Mesh;
			utils::Entity Entity;
			std::vector<PrimitiveBinding> Primitives;
		};

		struct ReadbackState {
			std::vector<std::uint8_t> Pixels;
			bool Complete = false;
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
		std::array<filament::Material *, 3> MaterialPackages{};
		filament::IndirectLight *AmbientLight = nullptr;
		std::optional<glm::vec3> AmbientColor;
		TextureResource WhiteTexture;
		filament::TextureSampler Sampler{
			filament::TextureSampler::MinFilter::LINEAR,
			filament::TextureSampler::MagFilter::LINEAR,
			filament::TextureSampler::WrapMode::REPEAT
		};
		utils::Entity CameraEntity;
		utils::Entity LightEntity;
		std::array<MeshResource, 4> Primitives;
		std::unordered_map<RenderMeshIdentity, MeshResource, RenderMeshIdentityHash> Meshes;
		std::unordered_map<RenderTextureIdentity, TextureResource, RenderTextureIdentityHash> Textures;
		std::unordered_map<MaterialKey, MaterialRecord, MaterialKeyHash> Materials;
		std::unordered_map<ObjectId, Entry> Entries;
		RenderProjection Projection;
		std::shared_ptr<UploadBudget> Uploads = std::make_shared<UploadBudget>();
		FilamentFrameMetrics LastMetrics;
		std::uint64_t SubmittedFrames = 0;
		std::uint64_t SkippedFrames = 0;
		std::uint64_t FullResyncs = 0;
		bool RequiresFullResync = true;

		Impl(const Vector2 &ViewportSize, bool HeadlessValue, bool ShadowsEnabledValue)
			: Width(CheckedViewportDimension(ViewportSize.GetX())),
			  Height(CheckedViewportDimension(ViewportSize.GetY())), Headless(HeadlessValue),
			  ShadowsEnabled(ShadowsEnabledValue) {
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
				SwapChain = Engine->createSwapChain(Width, Height, filament::SwapChain::CONFIG_READABLE);
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

			const std::array<std::span<const std::uint8_t>, 3> Packages{
				BENCHMARK_MATERIAL_PACKAGE, MASKED_MATERIAL_PACKAGE, TRANSPARENT_MATERIAL_PACKAGE};
			for (std::size_t Index = 0; Index < Packages.size(); ++Index) {
				MaterialPackages[Index] = filament::Material::Builder()
					.package(Packages[Index].data(), Packages[Index].size()).build(*Engine);
				if (!MaterialPackages[Index]) throw std::runtime_error("Filament rejected a qualification material package");
			}

			CreateTexture(WhiteTexture, 1, 1,
				std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255, 255, 255, 255}));
			CreatePrimitive(Primitives[0], BuildPrimitiveMesh(PrimitiveMeshes::Block()));
			CreatePrimitive(Primitives[1], BuildSphereMesh());
			CreatePrimitive(Primitives[2], BuildCylinderMesh());
			CreatePrimitive(Primitives[3], BuildPrimitiveMesh(PrimitiveMeshes::Wedge()));
			Engine->flushAndWait();
		}

		template <typename T>
		UploadLifetime *PrepareUpload(const std::shared_ptr<const std::vector<T>> &Data) {
			const auto Bytes = Data->size() * sizeof(T);
			if (Bytes > MAXIMUM_PENDING_UPLOAD_BYTES - Uploads->Bytes) Engine->flushAndWait();
			if (Bytes > MAXIMUM_PENDING_UPLOAD_BYTES - Uploads->Bytes)
				throw std::runtime_error("Filament pending upload budget is exhausted");
			auto Lifetime = std::make_unique<UploadLifetime>(Uploads, Data, Bytes);
			Uploads->Bytes += Bytes;
			return Lifetime.release();
		}

		template <typename T>
		filament::VertexBuffer::BufferDescriptor UploadBuffer(
			const std::shared_ptr<const std::vector<T>> &Data
		) {
			return {Data->data(), Data->size() * sizeof(T), UploadLifetime::Release, PrepareUpload(Data)};
		}

		void DestroyMesh(MeshResource &Resource) {
			if (Resource.Vertices) Engine->destroy(Resource.Vertices);
			if (Resource.Indices) Engine->destroy(Resource.Indices);
			Resource = {};
		}

		void CreateMesh(MeshResource &Target, const std::shared_ptr<const std::vector<FilamentVertex>> &Vertices,
			const std::shared_ptr<const std::vector<std::uint32_t>> &Indices, const RenderBounds &Bounds) {
			Target.Bounds = Bounds;
			Target.IndexCount = static_cast<std::uint32_t>(Indices->size());
			Target.Vertices = filament::VertexBuffer::Builder()
				.vertexCount(static_cast<std::uint32_t>(Vertices->size()))
				.bufferCount(1)
				.attribute(filament::VertexAttribute::POSITION, 0, filament::VertexBuffer::AttributeType::FLOAT3,
					offsetof(FilamentVertex, Position), sizeof(FilamentVertex))
				.attribute(filament::VertexAttribute::TANGENTS, 0, filament::VertexBuffer::AttributeType::FLOAT4,
					offsetof(FilamentVertex, TangentFrame), sizeof(FilamentVertex))
				.attribute(filament::VertexAttribute::UV0, 0, filament::VertexBuffer::AttributeType::FLOAT2,
					offsetof(FilamentVertex, UV), sizeof(FilamentVertex))
				.build(*Engine);
			Target.Indices = filament::IndexBuffer::Builder()
				.indexCount(Target.IndexCount).bufferType(filament::IndexBuffer::IndexType::UINT).build(*Engine);
			if (!Target.Vertices || !Target.Indices) throw std::runtime_error("Filament could not create mesh buffers");
			Target.Vertices->setBufferAt(*Engine, 0, UploadBuffer(Vertices));
			Target.Indices->setBuffer(*Engine, UploadBuffer(Indices));
		}

		void CreatePrimitive(MeshResource &Target, CpuMesh Source) {
			CreateMesh(Target, std::make_shared<const std::vector<FilamentVertex>>(std::move(Source.Vertices)),
				std::make_shared<const std::vector<std::uint32_t>>(std::move(Source.Indices)),
				{glm::vec3(-0.5f), glm::vec3(0.5f)});
		}

		MeshResource &GetMesh(const RenderProjectedObject &Object) {
			if (Object.Mesh) return Meshes.at(*Object.Mesh);
			switch (Object.Item.Geometry) {
				case RenderGeometry::Block: return Primitives[0];
				case RenderGeometry::Ball: return Primitives[1];
				case RenderGeometry::Cylinder: return Primitives[2];
				case RenderGeometry::Wedge: return Primitives[3];
				case RenderGeometry::CornerWedge: break;
			}
			throw std::invalid_argument("Filament qualification requires a canonical mesh for CornerWedge");
		}

		void UploadTexture(TextureResource &Resource, std::uint32_t X, std::uint32_t Y,
			std::uint32_t WidthValue, std::uint32_t HeightValue,
			const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels) {
			for (auto *Texture : {Resource.Linear, Resource.Color}) {
				Texture->setImage(*Engine, 0, X, Y, WidthValue, HeightValue,
					filament::Texture::PixelBufferDescriptor(Pixels->data(), Pixels->size(),
						filament::Texture::Format::RGBA, filament::Texture::Type::UBYTE,
						UploadLifetime::Release, PrepareUpload(Pixels)));
				LastMetrics.NativeTextureUploadBytes += Pixels->size();
			}
		}

		void CreateTexture(TextureResource &Target, std::uint32_t WidthValue, std::uint32_t HeightValue,
			const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels) {
			using Texture = filament::Texture;
			const auto Build = [&](Texture::InternalFormat Format) {
				return Texture::Builder().width(WidthValue).height(HeightValue).levels(1)
					.sampler(Texture::Sampler::SAMPLER_2D).format(Format).build(*Engine);
			};
			// The same publication identity may be used as either color or data.
			// The color view must decode sRGB before hardware linear filtering.
			Target.Linear = Build(Texture::InternalFormat::RGBA8);
			Target.Color = Build(Texture::InternalFormat::SRGB8_A8);
			if (!Target.Linear || !Target.Color) throw std::runtime_error("Filament could not create texture views");
			UploadTexture(Target, 0, 0, WidthValue, HeightValue, Pixels);
		}

		void DestroyTexture(TextureResource &Resource) {
			if (Resource.Linear) Engine->destroy(Resource.Linear);
			if (Resource.Color) Engine->destroy(Resource.Color);
			Resource = {};
		}

		filament::MaterialInstance *AcquireMaterial(const RenderMaterialState &Value, const MaterialKey &Key) {
			if (auto Existing = Materials.find(Key); Existing != Materials.end()) {
				++Existing->second.References;
				return Existing->second.Instance;
			}

			const auto PackageIndex = static_cast<std::size_t>(Value.OpacityMode);
			auto *Instance = MaterialPackages.at(PackageIndex)->createInstance();
			if (!Instance) throw std::runtime_error("Filament could not create a material instance");
			try {
				const auto *Color = Value.BaseColorTexture ? Textures.at(*Value.BaseColorTexture).Color : WhiteTexture.Color;
				const auto *Normal = Key.Normal ? Textures.at(*Key.Normal).Linear : WhiteTexture.Linear;
				const auto *Packed = Key.MetallicRoughness ? Textures.at(*Key.MetallicRoughness).Linear : WhiteTexture.Linear;
				const bool PhysicallyBased = Value.ShadingModel == RenderShadingModel::MetallicRoughness;
				Instance->setParameter("baseColorFactor", ToFilament(Value.BaseColorFactor));
				Instance->setParameter("metallicFactor", PhysicallyBased ? Value.Metallic : 0.0f);
				Instance->setParameter("roughnessFactor", PhysicallyBased ? Value.Roughness : 1.0f);
				Instance->setParameter("baseColorMap", Color, Sampler);
				Instance->setParameter("normalMap", Normal, Sampler);
				Instance->setParameter("metallicRoughnessMap", Packed, Sampler);
				Instance->setParameter("hasNormalMap", Key.Normal.has_value());
				Instance->setParameter("hasMetallicRoughnessMap", Key.MetallicRoughness.has_value());
				Instance->setDoubleSided(Value.DoubleSided);
				Instance->setCullingMode(Value.DoubleSided ? filament::MaterialInstance::CullingMode::NONE
					: filament::MaterialInstance::CullingMode::BACK);
				if (Value.OpacityMode == RenderOpacityMode::Masked) Instance->setMaskThreshold(Value.AlphaCutoff);
				Materials.emplace(Key, MaterialRecord{Instance, 1});
			} catch (...) {
				Engine->destroy(Instance);
				throw;
			}
			return Instance;
		}

		void ReleaseMaterial(const MaterialKey &Key) {
			const auto Existing = Materials.find(Key);
			if (Existing == Materials.end()) return;
			if (--Existing->second.References != 0) return;
			Engine->destroy(Existing->second.Instance);
			Materials.erase(Existing);
		}

		void CreateEntry(const RenderProjectedObject &Object) {
			const auto &[Position, Inserted] = Entries.try_emplace(Object.Item.Object);
			if (!Inserted) throw std::logic_error("Filament attempted duplicate native object creation");
			auto &EntryValue = Position->second;
			EntryValue.Item = Object.Item;
			EntryValue.Mesh = Object.Mesh;
			EntryValue.Entity = utils::EntityManager::get().create();
			if (EntryValue.Entity.isNull()) throw std::runtime_error("Filament entity capacity was exhausted");

			auto &Mesh = GetMesh(Object);
			const auto Count = Object.Primitives ? Object.Primitives->size() : 1u;
			EntryValue.Primitives.reserve(Count);
			filament::RenderableManager::Builder Builder(Count);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				const auto Primitive = Object.Primitives ? (*Object.Primitives)[Index]
					: RenderPrimitiveMaterialState{0, Mesh.IndexCount, Object.Material};
				const auto Key = MakeMaterialKey(Primitive.Material);
				auto *Instance = AcquireMaterial(Primitive.Material, Key);
				EntryValue.Primitives.push_back({Key, Primitive.FirstIndex, Primitive.IndexCount});
				Builder.material(Index, Instance).geometry(Index, filament::RenderableManager::PrimitiveType::TRIANGLES,
					Mesh.Vertices, Mesh.Indices, Primitive.FirstIndex, Primitive.IndexCount);
			}
			const auto Result = Builder.boundingBox(ToFilament(Mesh.Bounds))
				.culling(true).castShadows(ShadowsEnabled && Object.Item.CastShadow).receiveShadows(ShadowsEnabled)
				.build(*Engine, EntryValue.Entity);
			if (Result != filament::RenderableManager::Builder::Success)
				throw std::runtime_error("Filament could not build a renderable component");

			auto &Transforms = Engine->getTransformManager();
			const auto Transform = Transforms.getInstance(EntryValue.Entity);
			if (!Transform) throw std::runtime_error("Filament did not create a renderable transform component");
			Transforms.setTransform(Transform, ToFilament(Object.Item.ModelMatrix));
			Scene->addEntity(EntryValue.Entity);
			if (Object.Mesh) Mesh.Users.insert(Object.Item.Object);
		}

		bool BindingMatches(const Entry &Existing, const RenderProjectedObject &Object) {
			if (Existing.Mesh != Object.Mesh || Existing.Item.Geometry != Object.Item.Geometry) return false;
			if (!Object.Primitives)
				return Existing.Primitives.size() == 1 && Existing.Primitives[0].FirstIndex == 0 &&
					Existing.Primitives[0].IndexCount == GetMesh(Object).IndexCount &&
					Existing.Primitives[0].Material == MakeMaterialKey(Object.Material);
			if (Existing.Primitives.size() != Object.Primitives->size()) return false;
			for (std::size_t Index = 0; Index < Existing.Primitives.size(); ++Index) {
				const auto &Current = Existing.Primitives[Index];
				const auto &Next = (*Object.Primitives)[Index];
				if (Current.FirstIndex != Next.FirstIndex || Current.IndexCount != Next.IndexCount ||
					Current.Material != MakeMaterialKey(Next.Material)) return false;
			}
			return true;
		}

		void UpdateEntry(const RenderProjectedObject &Object) {
			const auto Existing = Entries.find(Object.Item.Object);
			if (Existing == Entries.end()) {
				if (Object.Visible) CreateEntry(Object);
				return;
			}
			if (!Object.Visible || !BindingMatches(Existing->second, Object)) {
				RemoveEntry(Existing->second);
				Entries.erase(Existing);
				if (Object.Visible) CreateEntry(Object);
				return;
			}
			auto &Value = Existing->second;
			if (Value.Item.ModelMatrix != Object.Item.ModelMatrix) {
				auto &Transforms = Engine->getTransformManager();
				Transforms.setTransform(Transforms.getInstance(Value.Entity), ToFilament(Object.Item.ModelMatrix));
			}
			if (Value.Item.CastShadow != Object.Item.CastShadow) {
				auto &Renderables = Engine->getRenderableManager();
				Renderables.setCastShadows(Renderables.getInstance(Value.Entity), ShadowsEnabled && Object.Item.CastShadow);
			}
			Value.Item = Object.Item;
		}

		void RemoveEntry(Entry &Existing) {
			if (Existing.Entity) {
				Scene->remove(Existing.Entity);
				Engine->destroy(Existing.Entity);
				utils::EntityManager::get().destroy(Existing.Entity);
			}
			if (Existing.Mesh) Meshes.at(*Existing.Mesh).Users.erase(Existing.Item.Object);
			for (const auto &Primitive : Existing.Primitives) ReleaseMaterial(Primitive.Material);
		}

		void ClearNativeResources() {
			for (auto &[Object, Existing] : Entries) {
				(void)Object;
				RemoveEntry(Existing);
			}
			Entries.clear();
			for (auto &[Identity, Mesh] : Meshes) {
				(void)Identity;
				DestroyMesh(Mesh);
			}
			Meshes.clear();
			for (auto &[Identity, Texture] : Textures) {
				(void)Identity;
				DestroyTexture(Texture);
			}
			Textures.clear();
		}

		void UpdateCameraAndLight(const RenderFrameState &Frame) {
			const auto &CameraValue = Frame.Camera;
			filament::math::mat4 ProjectionMatrix{ToFilament(CameraValue.ProjectionMatrix)};
			// Gargantuan publishes ZO clip depth. Filament accepts OpenGL NDC here and
			// applies its own reversed depth conversion before rendering.
			for (std::size_t Column = 0; Column < 4; ++Column)
				ProjectionMatrix[Column][2] = 2.0 * ProjectionMatrix[Column][2] - ProjectionMatrix[Column][3];
			Camera->setCustomProjection(ProjectionMatrix, CameraValue.NearPlane, CameraValue.FarPlane);
			Camera->setModelMatrix(inverse(filament::math::mat4{ToFilament(CameraValue.ViewMatrix)}));

			const auto &Environment = Frame.Environment;
			auto &Lights = Engine->getLightManager();
			const auto Light = Lights.getInstance(LightEntity);
			Lights.setDirection(Light, ToFilament(-Environment.SunDirection));
			Lights.setColor(Light, ToFilament(Environment.SunColor));
			Lights.setIntensity(Light, 100000.0f * Environment.SunIntensity * Environment.ExposureMultiplier);

			if (!AmbientColor || *AmbientColor != Environment.AmbientColor) {
				const auto Irradiance = ToFilament(Environment.AmbientColor);
				auto *Next = filament::IndirectLight::Builder().irradiance(1, &Irradiance).build(*Engine);
				if (!Next) throw std::runtime_error("Filament could not set diffuse ambient illumination");
				Scene->setIndirectLight(Next);
				if (AmbientLight) Engine->destroy(AmbientLight);
				AmbientLight = Next;
				AmbientColor = Environment.AmbientColor;
			}
			AmbientLight->setIntensity(100000.0f * Environment.ExposureMultiplier);

			const auto &Background = Environment.EnvironmentColor;
			filament::Renderer::ClearOptions Clear;
			Clear.clearColor = {Background.r, Background.g, Background.b, 1.0};
			Clear.clear = true;
			Renderer->setClearOptions(Clear);
		}

		void ValidatePublication(const RenderPublication &Publication, bool CapturePixels) const {
			if (!Engine || !Renderer || !SwapChain || !View || !Scene)
				throw std::logic_error("FilamentRenderer is not initialized");
			if (Publication.Frame.ViewportWidth != Width || Publication.Frame.ViewportHeight != Height)
				throw std::invalid_argument("Filament publication viewport does not match its target");
			if (RequiresFullResync && !Publication.FullResync)
				throw std::invalid_argument("Filament requires a full publication to reconstruct residency");
			if (CapturePixels && !Headless)
				throw std::invalid_argument("Filament qualification readback requires a headless target");
			const auto &ViewMatrix = Publication.Frame.Camera.ViewMatrix;
			const glm::dmat3 CameraBasis{ViewMatrix};
			const auto GramMatrix = glm::transpose(CameraBasis) * CameraBasis;
			const glm::dmat3 Identity(1.0);
			for (glm::length_t Column = 0; Column < 3; ++Column)
				if (ViewMatrix[Column][3] != 0.0f || !(glm::length(GramMatrix[Column] - Identity[Column]) < 1e-3))
					throw std::invalid_argument("Filament requires a rigid camera view matrix");
			if (ViewMatrix[3][3] != 1.0f || !(glm::determinant(CameraBasis) > 0.0) ||
				glm::determinant(glm::dmat4(Publication.Frame.Camera.ProjectionMatrix)) == 0.0)
				throw std::invalid_argument("Filament requires a right-handed view and invertible camera projection");
			if (Publication.Frame.Environment.Sky || Publication.Frame.Environment.Fog.Enabled ||
				!Publication.GetUi().Batches.empty())
				throw std::invalid_argument("Filament qualification does not support Sky, fog or GUI frames");
			for (const auto &Pose : Publication.AnimationPoseUpdates)
				if (Pose.Mode != RenderAnimationSkinningMode::CpuFallback)
					throw std::invalid_argument("Filament qualification requires CPU fallback meshes for animation");
			for (const auto &Mesh : Publication.MeshCreates)
				if (Mesh.Vertices && Mesh.Indices && (Mesh.Indices->size() > MAXIMUM_PENDING_UPLOAD_BYTES / sizeof(std::uint32_t) ||
					Mesh.Indices->size() % 3 != 0 ||
					Mesh.Vertices->size() > MAXIMUM_PENDING_UPLOAD_BYTES / sizeof(FilamentVertex)))
					throw std::invalid_argument("Filament publication mesh exceeds native buffer limits");
			for (const auto &Texture : Publication.TextureCreates)
				if (Texture.Width > MAXIMUM_VIEWPORT_DIMENSION || Texture.Height > MAXIMUM_VIEWPORT_DIMENSION)
					throw std::invalid_argument("Filament publication texture exceeds the qualification dimension limit");
			const auto CheckGeometry = [](const auto &Object) {
				if (Object.Visible && !Object.Mesh && Object.Item.Geometry == RenderGeometry::CornerWedge)
					throw std::invalid_argument("Filament qualification requires a canonical mesh for CornerWedge");
			};
			for (const auto &Object : Publication.Creates) CheckGeometry(Object);
			for (const auto &Object : Publication.Updates) CheckGeometry(Object);
		}

		void ApplyNativeResources(const RenderPublication &Publication) {
			if (Publication.FullResync) ClearNativeResources();
			for (const auto &Remove : Publication.Removes) {
				if (const auto Existing = Entries.find(Remove.Object); Existing != Entries.end()) {
					RemoveEntry(Existing->second);
					Entries.erase(Existing);
				}
			}
			Meshes.reserve(Meshes.size() + Publication.MeshCreates.size());
			Textures.reserve(Textures.size() + Publication.TextureCreates.size());
			Entries.reserve(Entries.size() + Publication.Creates.size());
			for (const auto &Create : Publication.MeshCreates) {
				const auto Vertices = ConvertVertices(*Create.Vertices);
				CreateMesh(Meshes[Create.Mesh], Vertices, Create.Indices, Create.Bounds);
				LastMetrics.NativeVertexUploadBytes += Vertices->size() * sizeof(FilamentVertex)
					+ Create.Indices->size() * sizeof(std::uint32_t);
			}
			for (const auto &Update : Publication.MeshVertexUpdates) {
				auto &Mesh = Meshes.at(Update.Mesh);
				const auto Vertices = ConvertVertices(*Update.Vertices);
				Mesh.Vertices->setBufferAt(*Engine, 0, UploadBuffer(Vertices),
					static_cast<std::uint32_t>(Update.FirstVertex * sizeof(FilamentVertex)));
				Mesh.Bounds = Update.Bounds;
				LastMetrics.NativeVertexUploadBytes += Vertices->size() * sizeof(FilamentVertex);
				auto &Renderables = Engine->getRenderableManager();
				for (const auto User : Mesh.Users)
					Renderables.setAxisAlignedBoundingBox(Renderables.getInstance(Entries.at(User).Entity), ToFilament(Mesh.Bounds));
			}
			for (const auto &Create : Publication.TextureCreates)
				CreateTexture(Textures[Create.Texture], Create.Width, Create.Height, Create.Pixels);
			for (const auto &Update : Publication.TextureUpdates)
				UploadTexture(Textures.at(Update.Texture), Update.X, Update.Y, Update.Width, Update.Height, Update.Pixels);
			for (const auto &Create : Publication.Creates) {
				const auto &Object = Projection.GetObjects().at(Create.Item.Object);
				if (Object.Visible) CreateEntry(Object);
			}
			for (const auto &Update : Publication.Updates) UpdateEntry(Projection.GetObjects().at(Update.Object));
			for (const auto &Remove : Publication.MeshRemoves) {
				DestroyMesh(Meshes.at(Remove.Mesh));
				Meshes.erase(Remove.Mesh);
			}
			for (const auto &Remove : Publication.TextureRemoves) {
				DestroyTexture(Textures.at(Remove.Texture));
				Textures.erase(Remove.Texture);
			}
			UpdateCameraAndLight(Projection.GetFrame());
		}

		void Submit(std::vector<std::uint8_t> *Pixels) {
			std::shared_ptr<ReadbackState> Readback;
			std::unique_ptr<std::shared_ptr<ReadbackState>> ReadbackLifetime;
			if (Pixels) {
				Readback = std::make_shared<ReadbackState>();
				Readback->Pixels.resize(static_cast<std::size_t>(Width) * Height * 4u);
				ReadbackLifetime = std::make_unique<std::shared_ptr<ReadbackState>>(Readback);
				if (!Engine->flushAndWait(5'000'000'000ull))
					throw std::runtime_error("Filament could not drain the qualification target before capture");
			}
			const auto SubmissionStart = Clock::now();
			if (Renderer->beginFrame(SwapChain)) {
				Renderer->render(View);
				if (Readback) {
					Renderer->readPixels(0, 0, Width, Height, filament::Texture::PixelBufferDescriptor(
						Readback->Pixels.data(), Readback->Pixels.size(), filament::Texture::Format::RGBA,
						filament::Texture::Type::UBYTE, [](void *, std::size_t, void *Context) {
							std::unique_ptr<std::shared_ptr<ReadbackState>> Owner(
								static_cast<std::shared_ptr<ReadbackState> *>(Context));
							(*Owner)->Complete = true;
						}, ReadbackLifetime.release()));
				}
				Renderer->endFrame();
				++SubmittedFrames;
			} else {
				++SkippedFrames;
				if (Pixels) throw std::runtime_error("Filament skipped the requested capture frame");
			}
			LastMetrics.RendererSubmissionMilliseconds = Milliseconds(Clock::now() - SubmissionStart);
			if (Pixels) {
				if (!Engine->flushAndWait(5'000'000'000ull) || !Readback->Complete)
					throw std::runtime_error("Filament did not complete the qualification readback");
				// Filament's Vulkan driver copies top-first native image rows.
				const auto RowBytes = static_cast<std::size_t>(Width) * 4u;
				for (std::uint32_t Row = 0; Row < Height / 2; ++Row) {
					auto Top = Readback->Pixels.begin() + Row * RowBytes;
					auto Bottom = Readback->Pixels.begin() + (Height - 1 - Row) * RowBytes;
					std::swap_ranges(Top, Top + RowBytes, Bottom);
				}
				*Pixels = std::move(Readback->Pixels);
			}
			const auto History = Renderer->getFrameInfoHistory(1);
			if (!History.empty() && History.back().gpuFrameDuration >= 0)
				LastMetrics.GpuFrameMilliseconds = static_cast<double>(History.back().gpuFrameDuration) / 1'000'000.0;
		}

		void Resize(int WidthValue, int HeightValue) {
			if (WidthValue < 1 || HeightValue < 1) return;
			if (!Engine || !SwapChain || !View) throw std::logic_error("Cannot resize an uninitialized FilamentRenderer");
			if (WidthValue > MAXIMUM_VIEWPORT_DIMENSION || HeightValue > MAXIMUM_VIEWPORT_DIMENSION)
				throw std::invalid_argument("Filament resize exceeds the qualification dimension limit");
			const auto NewWidth = static_cast<std::uint32_t>(WidthValue);
			const auto NewHeight = static_cast<std::uint32_t>(HeightValue);
			if (NewWidth == Width && NewHeight == Height) return;
			if (Headless) {
				auto *Replacement = Engine->createSwapChain(NewWidth, NewHeight, filament::SwapChain::CONFIG_READABLE);
				if (!Replacement) throw std::runtime_error("Filament could not replace its headless swapchain");
				Engine->destroy(SwapChain);
				SwapChain = Replacement;
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
			ClearNativeResources();
			if (Engine && View) Engine->destroy(View);
			View = nullptr;
			if (Scene && LightEntity) Scene->remove(LightEntity);
			if (Scene && AmbientLight) Scene->setIndirectLight(nullptr);
			if (Engine && AmbientLight) Engine->destroy(AmbientLight);
			AmbientLight = nullptr;
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
			if (Engine) {
				for (auto *&Package : MaterialPackages) {
					if (Package) Engine->destroy(Package);
					Package = nullptr;
				}
				for (auto &Mesh : Primitives) DestroyMesh(Mesh);
				DestroyTexture(WhiteTexture);
			}
			if (Engine && Scene) Engine->destroy(Scene);
			if (Engine && Renderer) Engine->destroy(Renderer);
			if (Engine && SwapChain) Engine->destroy(SwapChain);
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

	void FilamentRenderer::Draw(RenderPublicationPtr Publication) {
		DrawPublication(std::move(Publication), nullptr);
	}

	std::vector<std::uint8_t> FilamentRenderer::Capture(RenderPublicationPtr Publication) {
		std::vector<std::uint8_t> Pixels;
		DrawPublication(std::move(Publication), &Pixels);
		return Pixels;
	}

	void FilamentRenderer::DrawPublication(RenderPublicationPtr Publication, std::vector<std::uint8_t> *Pixels) {
		if (!Publication) throw std::invalid_argument("FilamentRenderer requires an immutable RenderPublication");
		if (!Implementation) throw std::logic_error("FilamentRenderer is destroyed");
		auto &State = *Implementation;
		const auto ProjectionStart = Clock::now();
		State.ValidatePublication(*Publication, Pixels != nullptr);
		RenderProjectionChanges Changes;
		const auto Invalidate = [&] {
			State.RequiresFullResync = true;
			State.ClearNativeResources();
			State.Projection.Clear();
		};
		try {
			Changes = State.Projection.Apply(*Publication);
		} catch (const std::invalid_argument &) {
			throw; // Validation rejects before mutating the last usable projection.
		} catch (...) {
			Invalidate();
			throw;
		}
		State.LastMetrics = {};
		State.LastMetrics.Changes = Changes;
		State.LastMetrics.ProjectionScanMilliseconds = Milliseconds(Clock::now() - ProjectionStart);
		const auto ApplyStart = Clock::now();
		try {
			State.ApplyNativeResources(*Publication);
			State.RequiresFullResync = false;
			if (Publication->FullResync) ++State.FullResyncs;
			State.LastMetrics.ChangedObjectApplyMilliseconds = Milliseconds(Clock::now() - ApplyStart);
			State.Submit(Pixels);
		} catch (...) {
			Invalidate();
			throw;
		}
		State.LastMetrics.ProjectedObjects = State.Projection.GetSize();
		State.LastMetrics.RenderableObjects = State.Entries.size();
		State.LastMetrics.ResidentMeshes = State.Meshes.size();
		State.LastMetrics.ResidentTextures = State.Textures.size();
		State.LastMetrics.SubmittedFrames = State.SubmittedFrames;
		State.LastMetrics.SkippedFrames = State.SkippedFrames;
		State.LastMetrics.FullResyncs = State.FullResyncs;
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
