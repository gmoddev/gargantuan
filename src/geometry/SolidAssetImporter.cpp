// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/geometry/SolidGeometryCompiler.hpp"
#include "../assets/AssetImporter.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace gargantuan {
	namespace {
		using Json = nlohmann::json;

		AssetDiagnostic Error(std::string Code, std::string Message) {
			return {std::move(Code), std::move(Message)};
		}

		bool BoundedJson(std::span<const std::uint8_t> Source) {
			if (Source.empty() || Source.size() > 1024 * 1024) return false;
			std::size_t Depth = 0;
			bool String = false;
			bool Escape = false;
			for (const auto Byte : Source) {
				if (String) {
					if (Escape) Escape = false;
					else if (Byte == '\\') Escape = true;
					else if (Byte == '"') String = false;
				} else if (Byte == '"') String = true;
				else if (Byte == '{' || Byte == '[') {
					if (++Depth > 32) return false;
				} else if (Byte == '}' || Byte == ']') {
					if (Depth == 0) return false;
					--Depth;
				}
			}
			return !String && Depth == 0;
		}

		std::expected<glm::vec3, AssetDiagnostic> Vector3(const Json &Value) {
			if (!Value.is_array() || Value.size() != 3 ||
				!Value[0].is_number() || !Value[1].is_number() || !Value[2].is_number())
				return std::unexpected(Error("InvalidSolidGraph", "Expected a three-number vector"));
			return glm::vec3(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
		}

		std::expected<SolidGraph, AssetDiagnostic> ParseGraph(std::span<const std::uint8_t> Source) {
			if (!BoundedJson(Source))
				return std::unexpected(Error("SolidSourceLimit", "Solid source exceeds size or nesting limit"));
			const auto Document = Json::parse(Source.begin(), Source.end(), nullptr, false);
			if (Document.is_discarded() || !Document.is_object() || !Document.contains("Version") ||
				!Document["Version"].is_number_unsigned() ||
				Document["Version"].get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max() ||
				!Document.contains("Root") ||
				!Document["Root"].is_string() || !Document.contains("Nodes") || !Document["Nodes"].is_array())
				return std::unexpected(Error("InvalidSolidGraph", "Solid source requires Version, Root, and Nodes"));
			SolidGraph Graph;
			Graph.Version = Document["Version"].get<std::uint32_t>();
			Graph.Root = Document["Root"].get<std::string>();
			if (Document["Nodes"].size() > 128)
				return std::unexpected(Error("SolidSourceLimit", "Solid source has too many nodes"));
			for (const auto &Value : Document["Nodes"]) {
				if (!Value.is_object() || !Value.contains("Id") || !Value["Id"].is_string() ||
					!Value.contains("Kind") || !Value["Kind"].is_string())
					return std::unexpected(Error("InvalidSolidGraph", "Solid node requires Id and Kind"));
				SolidNode Node;
				Node.Id = Value["Id"].get<std::string>();
				const auto Kind = Value["Kind"].get<std::string>();
				if (Kind == "Box") Node.Kind = SolidNodeKind::Box;
				else if (Kind == "Sphere") Node.Kind = SolidNodeKind::Sphere;
				else if (Kind == "ImportedMesh") Node.Kind = SolidNodeKind::ImportedMesh;
				else if (Kind == "Transform") Node.Kind = SolidNodeKind::Transform;
				else if (Kind == "Union") Node.Kind = SolidNodeKind::Union;
				else if (Kind == "Intersection") Node.Kind = SolidNodeKind::Intersection;
				else if (Kind == "Subtraction") Node.Kind = SolidNodeKind::Subtraction;
				else return std::unexpected(Error("InvalidSolidGraph", "Unknown solid node kind"));
				if (Value.contains("Inputs")) {
					if (!Value["Inputs"].is_array() || Value["Inputs"].size() > 16)
						return std::unexpected(Error("InvalidSolidGraph", "Solid inputs are invalid"));
					for (const auto &Input : Value["Inputs"]) {
						if (!Input.is_string()) return std::unexpected(Error("InvalidSolidGraph", "Solid input ID is invalid"));
						Node.Inputs.push_back(Input.get<std::string>());
					}
				}
				if (Node.Kind == SolidNodeKind::Box) {
					if (!Value.contains("Size")) return std::unexpected(Error("InvalidSolidGraph", "Box requires Size"));
					auto Size = Vector3(Value["Size"]);
					if (!Size) return std::unexpected(Size.error());
					Node.Size = *Size;
				} else if (Node.Kind == SolidNodeKind::Sphere) {
					if (!Value.contains("Radius") || !Value["Radius"].is_number())
						return std::unexpected(Error("InvalidSolidGraph", "Sphere requires Radius"));
					Node.Radius = Value["Radius"].get<float>();
				} else if (Node.Kind == SolidNodeKind::Transform) {
					if (!Value.contains("Matrix") || !Value["Matrix"].is_array() || Value["Matrix"].size() != 16)
						return std::unexpected(Error("InvalidSolidGraph", "Transform requires 16-number Matrix"));
					for (std::size_t Index = 0; Index < 16; ++Index) {
						if (!Value["Matrix"][Index].is_number())
							return std::unexpected(Error("InvalidSolidGraph", "Transform matrix value is invalid"));
						Node.Matrix[Index / 4][Index % 4] = Value["Matrix"][Index].get<float>();
					}
				} else if (Node.Kind == SolidNodeKind::ImportedMesh) {
					if (!Value.contains("Positions") || !Value["Positions"].is_array() ||
						!Value.contains("Indices") || !Value["Indices"].is_array() ||
						Value["Positions"].size() > 8192 || Value["Indices"].size() > 8192 * 3)
						return std::unexpected(Error("InvalidSolidMesh", "Imported operand arrays are invalid"));
					std::vector<RenderVertex> Vertices;
					std::vector<std::uint32_t> Indices;
					Vertices.reserve(Value["Positions"].size());
					Indices.reserve(Value["Indices"].size());
					for (const auto &Position : Value["Positions"]) {
						auto Parsed = Vector3(Position);
						if (!Parsed) return std::unexpected(Parsed.error());
						RenderVertex Vertex;
						Vertex.Position = *Parsed;
						Vertices.push_back(Vertex);
					}
					for (const auto &Index : Value["Indices"]) {
						if (!Index.is_number_unsigned() ||
							Index.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
							return std::unexpected(Error("InvalidSolidMesh", "Imported operand index is invalid"));
						Indices.push_back(Index.get<std::uint32_t>());
					}
					ImportedMesh Mesh;
					Mesh.Vertices = std::make_shared<const std::vector<RenderVertex>>(std::move(Vertices));
					Mesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(Indices));
					Node.Mesh = std::make_shared<const ImportedMesh>(std::move(Mesh));
				}
			Graph.Nodes.push_back(std::move(Node));
			}
			return Graph;
		}

		class SolidAssetImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override { return AssetKind::Mesh; }
			bool SupportsExtension(std::string_view Extension) const override { return Extension == ".solid"; }
			bool IsCompound() const override { return true; }
			std::expected<AssetImportCandidate, AssetDiagnostic> Import(
				std::span<const std::uint8_t>, const AssetImportContext &
			) const override {
				return std::unexpected(Error("CompoundRequired", "Solid source publishes a mesh and collision together"));
			}
			std::expected<AssetImportGraphCandidate, AssetDiagnostic> ImportGraph(
				std::span<const std::uint8_t> Source, const AssetImportContext &Context
			) const override {
				try {
					auto Graph = ParseGraph(Source);
					if (!Graph) return std::unexpected(Graph.error());
					auto Compiled = CompileSolidGeometry(*Graph, Context.Cancellation, Context.Deadline);
					if (!Compiled) return std::unexpected(Compiled.error());
					AssetImportGraphCandidate Candidate;
					Candidate.PrimaryLogicalKey = "asset";
					AssetImportNodeCandidate Mesh;
					Mesh.LogicalKey = "asset";
					Mesh.Kind = AssetKind::Mesh;
					Mesh.Name = "Solid Mesh";
					Mesh.Asset = std::move(Compiled->Mesh);
					Mesh.Bindings.push_back({AssetImportBindingKind::MeshCollision, 0, "collision"});
					Candidate.Nodes.push_back(std::move(Mesh));
					AssetImportNodeCandidate Collision;
					Collision.LogicalKey = "collision";
					Collision.Kind = AssetKind::Collision;
					Collision.Name = "Solid Collision";
					Collision.Asset = std::move(Compiled->Collision);
					Candidate.Nodes.push_back(std::move(Collision));
					return Candidate;
				} catch (const std::exception &) {
					return std::unexpected(Error("InvalidSolidGraph", "Solid source could not be decoded"));
				}
			}
		};
	}

	std::unique_ptr<IAssetImporter> CreateSolidImporter() {
		return std::make_unique<SolidAssetImporter>();
	}
}
