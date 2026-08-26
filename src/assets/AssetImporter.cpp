// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "AssetImporter.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>

#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		constexpr std::array<std::uint8_t, 8> ArtifactMagic{'G', 'A', 'R', 'G', 'A', 'S', '0', '1'};
		constexpr std::uint32_t ArtifactVersion1 = 1;
		constexpr std::uint32_t ArtifactVersion2 = 2;

		AssetDiagnostic Error(std::string Code, std::string Message) {
			if (Message.size() > AssetLimits::MaximumDiagnosticBytes) Message.resize(AssetLimits::MaximumDiagnosticBytes);
			return {std::move(Code), std::move(Message)};
		}

		bool Cancelled(const AssetImportContext &Context) {
			return Context.Cancellation.IsCancelled() || std::chrono::steady_clock::now() > Context.Deadline;
		}

		void AppendU32(std::vector<std::uint8_t> &Output, std::uint32_t Value) {
			for (std::size_t Index = 0; Index < 4; ++Index) Output.push_back(static_cast<std::uint8_t>(Value >> (Index * 8)));
		}

		void AppendU16(std::vector<std::uint8_t> &Output, std::uint16_t Value) {
			Output.push_back(static_cast<std::uint8_t>(Value));
			Output.push_back(static_cast<std::uint8_t>(Value >> 8));
		}

		void AppendU64(std::vector<std::uint8_t> &Output, std::uint64_t Value) {
			for (std::size_t Index = 0; Index < 8; ++Index)
				Output.push_back(static_cast<std::uint8_t>(Value >> (Index * 8)));
		}

		void AppendFloat(std::vector<std::uint8_t> &Output, float Value) { AppendU32(Output, std::bit_cast<std::uint32_t>(Value)); }

		std::optional<std::uint32_t> ReadU32(std::span<const std::uint8_t> Bytes, std::size_t &Offset) {
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return std::nullopt;
			std::uint32_t Value = 0;
			for (std::size_t Index = 0; Index < 4; ++Index) Value |= static_cast<std::uint32_t>(Bytes[Offset++]) << (Index * 8);
			return Value;
		}

		std::optional<std::uint16_t> ReadU16(std::span<const std::uint8_t> Bytes, std::size_t &Offset) {
			if (Offset > Bytes.size() || Bytes.size() - Offset < 2) return std::nullopt;
			const auto Value = static_cast<std::uint16_t>(Bytes[Offset]) |
				(static_cast<std::uint16_t>(Bytes[Offset + 1]) << 8);
			Offset += 2;
			return Value;
		}

		std::optional<std::uint64_t> ReadU64(std::span<const std::uint8_t> Bytes, std::size_t &Offset) {
			if (Offset > Bytes.size() || Bytes.size() - Offset < 8) return std::nullopt;
			std::uint64_t Value = 0;
			for (std::size_t Index = 0; Index < 8; ++Index)
				Value |= static_cast<std::uint64_t>(Bytes[Offset++]) << (Index * 8);
			return Value;
		}

		std::optional<float> ReadFloat(std::span<const std::uint8_t> Bytes, std::size_t &Offset) {
			auto Value = ReadU32(Bytes, Offset);
			if (!Value) return std::nullopt;
			const auto Result = std::bit_cast<float>(*Value);
			return std::isfinite(Result) ? std::optional(Result) : std::nullopt;
		}

		std::vector<std::uint8_t> BeginArtifact(AssetKind Kind, std::uint32_t Version = ArtifactVersion1) {
			std::vector<std::uint8_t> Result(ArtifactMagic.begin(), ArtifactMagic.end());
			AppendU32(Result, Version);
			Result.push_back(static_cast<std::uint8_t>(Kind));
			return Result;
		}

		std::expected<std::pair<std::uint32_t, std::uint32_t>, AssetDiagnostic> ReadImageDimensions(
			std::span<const std::uint8_t> Source,
			std::string_view Extension
		) {
			if (Extension == ".png") {
				static constexpr std::array<std::uint8_t, 8> Signature{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
				if (Source.size() < 24 || !std::equal(Signature.begin(), Signature.end(), Source.begin()) ||
					std::memcmp(Source.data() + 12, "IHDR", 4) != 0)
					return std::unexpected(Error("MalformedImage", "PNG signature or IHDR is invalid"));
				const auto ReadBig = [&](std::size_t Offset) {
					return (static_cast<std::uint32_t>(Source[Offset]) << 24) |
						(static_cast<std::uint32_t>(Source[Offset + 1]) << 16) |
						(static_cast<std::uint32_t>(Source[Offset + 2]) << 8) | Source[Offset + 3];
				};
				return std::pair(ReadBig(16), ReadBig(20));
			}
			if (Extension == ".bmp") {
				if (Source.size() < 26 || Source[0] != 'B' || Source[1] != 'M')
					return std::unexpected(Error("MalformedImage", "BMP header is invalid"));
				const auto ReadLittle = [&](std::size_t Offset) {
					return static_cast<std::uint32_t>(Source[Offset]) |
						(static_cast<std::uint32_t>(Source[Offset + 1]) << 8) |
						(static_cast<std::uint32_t>(Source[Offset + 2]) << 16) |
						(static_cast<std::uint32_t>(Source[Offset + 3]) << 24);
				};
				const auto Width = ReadLittle(18);
				const auto SignedHeight = std::bit_cast<std::int32_t>(ReadLittle(22));
				if (SignedHeight == std::numeric_limits<std::int32_t>::min())
					return std::unexpected(Error("MalformedImage", "BMP height is invalid"));
				return std::pair(Width, static_cast<std::uint32_t>(std::abs(SignedHeight)));
			}
			return std::unexpected(Error("UnsupportedFormat", "Foundation 1 image importer supports PNG and BMP"));
		}

		class ImageImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override { return AssetKind::Image; }
			bool SupportsExtension(std::string_view Extension) const override {
				return Extension == ".png" || Extension == ".bmp";
			}
			std::expected<AssetImportCandidate, AssetDiagnostic> Import(
				std::span<const std::uint8_t> Source,
				const AssetImportContext &Context
			) const override {
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Image import was cancelled or exceeded its deadline"));
				auto Dimensions = ReadImageDimensions(Source, Context.SourceExtension);
				if (!Dimensions) return std::unexpected(Dimensions.error());
				const auto [Width, Height] = *Dimensions;
				if (Width == 0 || Height == 0 || Width > AssetLimits::MaximumImageDimension ||
					Height > AssetLimits::MaximumImageDimension ||
					static_cast<std::uint64_t>(Width) * Height * 4 > AssetLimits::MaximumImageBytes)
					return std::unexpected(Error("ImageLimit", "Decoded image dimensions exceed the asset limit"));

				auto *Stream = SDL_IOFromConstMem(Source.data(), Source.size());
				if (!Stream) return std::unexpected(Error("ImageDecode", SDL_GetError()));
				auto *Surface = IMG_LoadTyped_IO(Stream, true, Context.SourceExtension == ".png" ? "PNG" : "BMP");
				if (!Surface) return std::unexpected(Error("ImageDecode", SDL_GetError()));
				if (Surface->w != static_cast<int>(Width) || std::abs(Surface->h) != static_cast<int>(Height)) {
					SDL_DestroySurface(Surface);
					return std::unexpected(Error("MalformedImage", "Decoded image dimensions disagree with the bounded header"));
				}
				auto *Converted = SDL_ConvertSurface(Surface, SDL_PIXELFORMAT_RGBA32);
				SDL_DestroySurface(Surface);
				if (!Converted) return std::unexpected(Error("ImageDecode", SDL_GetError()));
				auto Pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Width) * Height * 4);
				for (std::uint32_t Row = 0; Row < Height; ++Row)
					std::memcpy(Pixels->data() + static_cast<std::size_t>(Row) * Width * 4,
						static_cast<const std::uint8_t *>(Converted->pixels) + static_cast<std::size_t>(Row) * Converted->pitch,
						static_cast<std::size_t>(Width) * 4);
				SDL_DestroySurface(Converted);
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Image import was cancelled before commit"));

				auto Artifact = BeginArtifact(AssetKind::Image);
				AppendU32(Artifact, Width);
				AppendU32(Artifact, Height);
				Artifact.insert(Artifact.end(), Pixels->begin(), Pixels->end());
				auto ImmutableArtifact = std::make_shared<const std::vector<std::uint8_t>>(std::move(Artifact));
				return AssetImportCandidate{
					ImportedImage{Width, Height, Pixels}, ImmutableArtifact, AssetContentId::Hash(*ImmutableArtifact)
				};
			}
		};

		struct ObjVertexKey {
			int Position = 0;
			int TextureCoordinate = 0;
			int Normal = 0;
			bool operator==(const ObjVertexKey &) const = default;
		};

		struct ObjVertexKeyHash {
			std::size_t operator()(const ObjVertexKey &Value) const noexcept {
				return std::hash<int>{}(Value.Position) ^ (std::hash<int>{}(Value.TextureCoordinate) << 1) ^
					(std::hash<int>{}(Value.Normal) << 2);
			}
		};

		std::optional<int> ResolveObjIndex(int Value, std::size_t Size) {
			if (Value > 0 && static_cast<std::size_t>(Value) <= Size) return Value - 1;
			if (Value < 0 && static_cast<std::size_t>(-Value) <= Size) return static_cast<int>(Size) + Value;
			return std::nullopt;
		}

		std::optional<ObjVertexKey> ParseObjVertex(std::string_view Token) {
			ObjVertexKey Result;
			std::array<int *, 3> Fields{&Result.Position, &Result.TextureCoordinate, &Result.Normal};
			std::size_t Begin = 0;
			for (std::size_t Field = 0; Field < Fields.size(); ++Field) {
				const auto End = Token.find('/', Begin);
				const auto Slice = Token.substr(Begin, End == std::string_view::npos ? Token.size() - Begin : End - Begin);
				if (!Slice.empty()) {
					const auto [Position, Failure] = std::from_chars(Slice.data(), Slice.data() + Slice.size(), *Fields[Field]);
					if (Failure != std::errc{} || Position != Slice.data() + Slice.size() || *Fields[Field] == 0) return std::nullopt;
				}
				if (End == std::string_view::npos) break;
				Begin = End + 1;
			}
			return Result.Position != 0 ? std::optional(Result) : std::nullopt;
		}

		class MeshImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override { return AssetKind::Mesh; }
			bool SupportsExtension(std::string_view Extension) const override { return Extension == ".obj"; }
			std::expected<AssetImportCandidate, AssetDiagnostic> Import(
				std::span<const std::uint8_t> Source,
				const AssetImportContext &Context
			) const override {
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Mesh import was cancelled or exceeded its deadline"));
				std::string Text(reinterpret_cast<const char *>(Source.data()), Source.size());
				std::istringstream Input(Text);
				std::vector<glm::vec3> Positions;
				std::vector<glm::vec3> Normals;
				std::vector<glm::vec2> TextureCoordinates;
				auto Vertices = std::make_shared<std::vector<RenderVertex>>();
				auto Indices = std::make_shared<std::vector<std::uint32_t>>();
				std::unordered_map<ObjVertexKey, std::uint32_t, ObjVertexKeyHash> VertexMap;
				std::vector<bool> HasNormal;
				std::string Line;
				std::size_t LineNumber = 0;
				auto ParseFloat = [](std::string_view Value) -> std::optional<float> {
					float Result = 0;
					const auto [End, Failure] = std::from_chars(Value.data(), Value.data() + Value.size(), Result);
					if (Failure != std::errc{} || End != Value.data() + Value.size() || !std::isfinite(Result)) return std::nullopt;
					return Result;
				};
				while (std::getline(Input, Line)) {
					++LineNumber;
					if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Mesh import was cancelled before commit"));
					if (Line.size() > 4096) return std::unexpected(Error("MeshLimit", "OBJ line exceeds 4096 bytes"));
					std::istringstream Tokens(Line);
					std::string Command;
					Tokens >> Command;
					if (Command.empty() || Command[0] == '#') continue;
					if (Command == "v" || Command == "vn" || Command == "vt") {
						std::vector<float> Values;
						std::string Token;
						while (Tokens >> Token) {
							auto Value = ParseFloat(Token);
							if (!Value) return std::unexpected(Error("MalformedMesh", "OBJ contains a non-finite or invalid number"));
							Values.push_back(*Value);
						}
						if ((Command == "vt" && Values.size() < 2) || (Command != "vt" && Values.size() < 3))
							return std::unexpected(Error("MalformedMesh", "OBJ vertex attribute has too few components"));
						if (Command == "v") Positions.emplace_back(Values[0], Values[1], Values[2]);
						else if (Command == "vn") Normals.emplace_back(Values[0], Values[1], Values[2]);
						else TextureCoordinates.emplace_back(Values[0], 1.0f - Values[1]);
						if (Positions.size() > AssetLimits::MaximumMeshVertices || Normals.size() > AssetLimits::MaximumMeshVertices ||
							TextureCoordinates.size() > AssetLimits::MaximumMeshVertices)
							return std::unexpected(Error("MeshLimit", "OBJ attribute count exceeds the asset limit"));
						continue;
					}
					if (Command != "f") continue;
					std::vector<std::uint32_t> Face;
					std::string Token;
					while (Tokens >> Token) {
						if (Face.size() >= 256) return std::unexpected(Error("MeshLimit", "OBJ polygon exceeds 256 vertices"));
						auto Key = ParseObjVertex(Token);
						if (!Key) return std::unexpected(Error("MalformedMesh", "OBJ face index is invalid"));
						auto Existing = VertexMap.find(*Key);
						if (Existing != VertexMap.end()) { Face.push_back(Existing->second); continue; }
						auto Position = ResolveObjIndex(Key->Position, Positions.size());
						auto TextureCoordinate = Key->TextureCoordinate == 0 ? std::optional<int>{} : ResolveObjIndex(Key->TextureCoordinate, TextureCoordinates.size());
						auto Normal = Key->Normal == 0 ? std::optional<int>{} : ResolveObjIndex(Key->Normal, Normals.size());
						if (!Position || (Key->TextureCoordinate != 0 && !TextureCoordinate) || (Key->Normal != 0 && !Normal))
							return std::unexpected(Error("InvalidMeshIndex", "OBJ face references an out-of-range attribute"));
						RenderVertex Vertex;
						Vertex.Position = Positions[*Position];
						if (TextureCoordinate) Vertex.TextureCoordinate = TextureCoordinates[*TextureCoordinate];
						if (Normal) Vertex.Normal = Normals[*Normal]; else Vertex.Normal = {0.0f, 0.0f, 0.0f};
						if (Vertices->size() >= AssetLimits::MaximumMeshVertices)
							return std::unexpected(Error("MeshLimit", "Canonical mesh vertex count exceeds the asset limit"));
						const auto Index = static_cast<std::uint32_t>(Vertices->size());
						Vertices->push_back(Vertex);
						HasNormal.push_back(Normal.has_value());
						VertexMap.emplace(*Key, Index);
						Face.push_back(Index);
					}
					if (Face.size() < 3) return std::unexpected(Error("MalformedTopology", "OBJ face has fewer than three vertices"));
					if (Indices->size() + (Face.size() - 2) * 3 > AssetLimits::MaximumMeshIndices)
						return std::unexpected(Error("MeshLimit", "Canonical mesh index count exceeds the asset limit"));
					for (std::size_t Index = 1; Index + 1 < Face.size(); ++Index) {
						Indices->push_back(Face[0]); Indices->push_back(Face[Index]); Indices->push_back(Face[Index + 1]);
					}
				}
				if (Vertices->empty() || Indices->empty()) return std::unexpected(Error("MalformedTopology", "OBJ contains no renderable triangles"));
				for (std::size_t Index = 0; Index < Indices->size(); Index += 3) {
					const auto A = (*Indices)[Index], B = (*Indices)[Index + 1], C = (*Indices)[Index + 2];
					if (A >= Vertices->size() || B >= Vertices->size() || C >= Vertices->size())
						return std::unexpected(Error("InvalidMeshIndex", "Canonical mesh contains an invalid index"));
					const auto Cross = glm::cross((*Vertices)[B].Position - (*Vertices)[A].Position, (*Vertices)[C].Position - (*Vertices)[A].Position);
					if (!std::isfinite(Cross.x) || !std::isfinite(Cross.y) || !std::isfinite(Cross.z) || glm::length(Cross) < 1e-8f)
						return std::unexpected(Error("MalformedTopology", "OBJ contains a degenerate or non-finite triangle"));
					for (const auto Vertex : {A, B, C}) if (!HasNormal[Vertex]) (*Vertices)[Vertex].Normal += Cross;
				}
				for (std::size_t Index = 0; Index < Vertices->size(); ++Index) {
					auto &Vertex = (*Vertices)[Index];
					if (glm::length(Vertex.Normal) < 1e-8f) return std::unexpected(Error("MalformedTopology", "OBJ generated a zero normal"));
					Vertex.Normal = glm::normalize(Vertex.Normal);
				}
				RenderBounds Bounds{Vertices->front().Position, Vertices->front().Position};
				for (const auto &Vertex : *Vertices) {
					Bounds.Minimum = glm::min(Bounds.Minimum, Vertex.Position);
					Bounds.Maximum = glm::max(Bounds.Maximum, Vertex.Position);
				}

				auto Artifact = BeginArtifact(AssetKind::Mesh);
				AppendU32(Artifact, static_cast<std::uint32_t>(Vertices->size()));
				AppendU32(Artifact, static_cast<std::uint32_t>(Indices->size()));
				AppendU32(Artifact, 1);
				for (const auto Component : {Bounds.Minimum.x, Bounds.Minimum.y, Bounds.Minimum.z, Bounds.Maximum.x, Bounds.Maximum.y, Bounds.Maximum.z})
					AppendFloat(Artifact, Component);
				for (const auto &Vertex : *Vertices) {
					for (const auto Component : {Vertex.Position.x, Vertex.Position.y, Vertex.Position.z, Vertex.Normal.x, Vertex.Normal.y,
						Vertex.Normal.z, Vertex.Tangent.x, Vertex.Tangent.y, Vertex.Tangent.z, Vertex.Tangent.w,
						Vertex.TextureCoordinate.x, Vertex.TextureCoordinate.y}) AppendFloat(Artifact, Component);
				}
				for (const auto Index : *Indices) AppendU32(Artifact, Index);
				auto ImmutableArtifact = std::make_shared<const std::vector<std::uint8_t>>(std::move(Artifact));
				return AssetImportCandidate{
					ImportedMesh{Vertices, Indices, Bounds, 1}, ImmutableArtifact, AssetContentId::Hash(*ImmutableArtifact)
				};
			}
		};

		std::mutex FontValidationMutex;

		class FontImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override { return AssetKind::Font; }
			bool SupportsExtension(std::string_view Extension) const override { return Extension == ".ttf" || Extension == ".otf"; }
			std::expected<AssetImportCandidate, AssetDiagnostic> Import(
				std::span<const std::uint8_t> Source,
				const AssetImportContext &Context
			) const override {
				if (Source.empty() || Source.size() > AssetLimits::MaximumFontBytes)
					return std::unexpected(Error("FontLimit", "Font byte count is outside the asset limit"));
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Font import was cancelled or exceeded its deadline"));
				std::uint32_t FaceCount = 0;
				{
					std::scoped_lock Lock(FontValidationMutex);
					if (!TTF_Init()) return std::unexpected(Error("FontValidation", SDL_GetError()));
					auto *Stream = SDL_IOFromConstMem(Source.data(), Source.size());
					auto *Font = Stream ? TTF_OpenFontIO(Stream, true, 16.0f) : nullptr;
					if (!Font) {
						const auto Message = std::string(SDL_GetError());
						TTF_Quit();
						return std::unexpected(Error("FontValidation", Message));
					}
					const auto Faces = TTF_GetNumFontFaces(Font);
					TTF_CloseFont(Font);
					TTF_Quit();
					if (Faces <= 0 || Faces > 64) return std::unexpected(Error("FontLimit", "Font face count is outside the asset limit"));
					FaceCount = static_cast<std::uint32_t>(Faces);
				}
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "Font import was cancelled before commit"));
				auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Source.begin(), Source.end());
				auto Artifact = BeginArtifact(AssetKind::Font);
				AppendU32(Artifact, FaceCount);
				AppendU32(Artifact, static_cast<std::uint32_t>(Bytes->size()));
				Artifact.insert(Artifact.end(), Bytes->begin(), Bytes->end());
				auto ImmutableArtifact = std::make_shared<const std::vector<std::uint8_t>>(std::move(Artifact));
				return AssetImportCandidate{
					ImportedFont{Bytes, FaceCount}, ImmutableArtifact, AssetContentId::Hash(*ImmutableArtifact)
				};
			}
		};

		class WaveImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override {
				return AssetKind::Audio;
			}
			bool SupportsExtension(std::string_view Extension) const override {
				return Extension == ".wav";
			}

			std::expected<AssetImportCandidate, AssetDiagnostic>
			Import(std::span<const std::uint8_t> Source, const AssetImportContext &Context) const override {
				if (Cancelled(Context))
					return std::unexpected(Error("Cancelled", "Audio import was cancelled or exceeded its deadline"));
				if (Source.size() < 44 || std::memcmp(Source.data(), "RIFF", 4) != 0 ||
					std::memcmp(Source.data() + 8, "WAVE", 4) != 0)
					return std::unexpected(Error("MalformedAudio", "WAV RIFF/WAVE header is invalid"));

				std::size_t HeaderOffset = 4;
				auto RiffBytes = ReadU32(Source, HeaderOffset);
				if (!RiffBytes || static_cast<std::uint64_t>(*RiffBytes) + 8 != Source.size())
					return std::unexpected(Error("MalformedAudio", "WAV RIFF size does not match the source"));

				std::optional<std::uint16_t> Channels;
				std::optional<std::uint32_t> SampleRate;
				std::optional<std::span<const std::uint8_t>> Data;
				std::size_t Offset = 12;
				std::size_t ChunkCount = 0;
				while (Offset < Source.size()) {
					if (++ChunkCount > AssetLimits::MaximumWaveChunks || Source.size() - Offset < 8)
						return std::unexpected(
							Error("MalformedAudio", "WAV chunk table is invalid or exceeds its limit")
						);
					const auto *ChunkId = Source.data() + Offset;
					Offset += 4;
					auto ChunkBytes = ReadU32(Source, Offset);
					if (!ChunkBytes || Offset > Source.size() || *ChunkBytes > Source.size() - Offset)
						return std::unexpected(Error("MalformedAudio", "WAV chunk length exceeds the source"));
					const auto Chunk = Source.subspan(Offset, *ChunkBytes);

					if (std::memcmp(ChunkId, "fmt ", 4) == 0) {
						if (Channels || Chunk.size() < 16)
							return std::unexpected(
								Error("MalformedAudio", "WAV must contain one complete format chunk")
							);
						std::size_t FormatOffset = 0;
						auto Format = ReadU16(Chunk, FormatOffset);
						Channels = ReadU16(Chunk, FormatOffset);
						SampleRate = ReadU32(Chunk, FormatOffset);
						auto ByteRate = ReadU32(Chunk, FormatOffset);
						auto BlockAlign = ReadU16(Chunk, FormatOffset);
						auto BitsPerSample = ReadU16(Chunk, FormatOffset);
						if (!Format || !Channels || !SampleRate || !ByteRate || !BlockAlign || !BitsPerSample ||
							*Format != 1 || *BitsPerSample != 16 || *Channels == 0 ||
							*Channels > AssetLimits::MaximumAudioChannels ||
							*SampleRate < AssetLimits::MinimumAudioSampleRate ||
							*SampleRate > AssetLimits::MaximumAudioSampleRate ||
							*BlockAlign != static_cast<std::uint16_t>(*Channels * sizeof(std::int16_t)) ||
							static_cast<std::uint64_t>(*SampleRate) * *BlockAlign != *ByteRate)
							return std::unexpected(Error(
								"UnsupportedAudio",
								"Foundation 1 WAV requires mono/stereo little-endian PCM16 at 8-48 kHz"
							));
					} else if (std::memcmp(ChunkId, "data", 4) == 0) {
						if (Data) return std::unexpected(Error("MalformedAudio", "WAV contains multiple data chunks"));
						Data = Chunk;
					}

					Offset += *ChunkBytes;
					if ((*ChunkBytes & 1u) != 0) {
						if (Offset >= Source.size())
							return std::unexpected(Error("MalformedAudio", "WAV chunk padding is missing"));
						++Offset;
					}
				}
				if (!Channels || !SampleRate || !Data || Data->empty())
					return std::unexpected(Error("MalformedAudio", "WAV format or sample data is missing"));

				const auto BlockAlign = static_cast<std::size_t>(*Channels) * sizeof(std::int16_t);
				if (Data->size() % BlockAlign != 0)
					return std::unexpected(Error("MalformedAudio", "WAV sample data is not frame-aligned"));
				const auto FrameCount64 = Data->size() / BlockAlign;
				const auto DurationFrameLimit = static_cast<std::uint64_t>(*SampleRate) *
												AssetLimits::MaximumAudioDurationSeconds;
				if (FrameCount64 == 0 || FrameCount64 > AssetLimits::MaximumAudioFrames ||
					FrameCount64 > DurationFrameLimit || Data->size() > AssetLimits::MaximumAudioPcmBytes)
					return std::unexpected(
						Error("AudioLimit", "Decoded audio exceeds the 30-second resident PCM limit")
					);

				auto Pcm = std::make_shared<std::vector<std::int16_t>>();
				Pcm->reserve(Data->size() / sizeof(std::int16_t));
				for (std::size_t SampleOffset = 0; SampleOffset < Data->size(); SampleOffset += 2) {
					const auto Bits = static_cast<std::uint16_t>(
						static_cast<std::uint16_t>((*Data)[SampleOffset]) |
						(static_cast<std::uint16_t>((*Data)[SampleOffset + 1]) << 8)
					);
					Pcm->push_back(std::bit_cast<std::int16_t>(Bits));
				}
				if (Cancelled(Context))
					return std::unexpected(Error("Cancelled", "Audio import was cancelled before commit"));

				auto Artifact = BeginArtifact(AssetKind::Audio, ArtifactVersion2);
				AppendU32(Artifact, *SampleRate);
				Artifact.push_back(static_cast<std::uint8_t>(*Channels));
				AppendU32(Artifact, static_cast<std::uint32_t>(FrameCount64));
				for (const auto Sample : *Pcm)
					AppendU16(Artifact, std::bit_cast<std::uint16_t>(Sample));
				auto ImmutablePcm = std::shared_ptr<const std::vector<std::int16_t>>(std::move(Pcm));
				auto ImmutableArtifact = std::make_shared<const std::vector<std::uint8_t>>(std::move(Artifact));
				return AssetImportCandidate{
					ImportedAudio{
						*SampleRate,
						static_cast<std::uint8_t>(*Channels),
						static_cast<std::uint32_t>(FrameCount64),
						ImmutablePcm
					},
					ImmutableArtifact,
					AssetContentId::Hash(*ImmutableArtifact),
				};
			}
		};
	}

	std::expected<std::vector<std::string>, AssetDiagnostic> IAssetImporter::DiscoverExternalResources(
		std::span<const std::uint8_t>,
		const AssetImportContext &
	) const {
		return std::vector<std::string>{};
	}

	std::expected<AssetImportGraphCandidate, AssetDiagnostic> IAssetImporter::ImportGraph(
		std::span<const std::uint8_t> Source,
		const AssetImportContext &Context
	) const {
		auto Candidate = Import(Source, Context);
		if (!Candidate) return std::unexpected(Candidate.error());
		AssetImportNodeCandidate Node;
		Node.LogicalKey = "asset";
		Node.Kind = GetKind();
		Node.Asset = std::move(Candidate->Asset);
		Node.Artifact = std::move(Candidate->Artifact);
		Node.ContentId = Candidate->ContentId;
		return AssetImportGraphCandidate{{std::move(Node)}, "asset"};
	}

	std::vector<std::unique_ptr<IAssetImporter>> CreateFoundationAssetImporters() {
		std::vector<std::unique_ptr<IAssetImporter>> Result;
		Result.push_back(std::make_unique<ImageImporter>());
		Result.push_back(std::make_unique<MeshImporter>());
		Result.push_back(std::make_unique<FontImporter>());
		Result.push_back(std::make_unique<WaveImporter>());
		Result.push_back(CreateGltfImporter());
		return Result;
	}

	std::expected<std::shared_ptr<const std::vector<std::uint8_t>>, AssetDiagnostic> EncodeAssetArtifact(
		const ImportedAsset &Asset,
		AssetKind Kind
	) {
		if ((Kind == AssetKind::Image && !std::holds_alternative<ImportedImage>(Asset)) ||
			(Kind == AssetKind::Mesh && !std::holds_alternative<ImportedMesh>(Asset)) ||
			(Kind == AssetKind::Font && !std::holds_alternative<ImportedFont>(Asset)) ||
			(Kind == AssetKind::Material && !std::holds_alternative<ImportedMaterial>(Asset)) ||
			(Kind == AssetKind::Audio && !std::holds_alternative<ImportedAudio>(Asset)))
			return std::unexpected(Error("ArtifactKindMismatch", "Canonical value does not match its asset kind"));
		auto Artifact = BeginArtifact(Kind, ArtifactVersion2);
		if (const auto *Image = std::get_if<ImportedImage>(&Asset)) {
			if (!Image->Rgba8 || Image->Width == 0 || Image->Height == 0 ||
				Image->Rgba8->size() != static_cast<std::size_t>(Image->Width) * Image->Height * 4)
				return std::unexpected(Error("MalformedImage", "Canonical image value is invalid"));
			AppendU32(Artifact, Image->Width);
			AppendU32(Artifact, Image->Height);
			Artifact.insert(Artifact.end(), Image->Rgba8->begin(), Image->Rgba8->end());
		} else if (const auto *Font = std::get_if<ImportedFont>(&Asset)) {
			if (!Font->Bytes || Font->Bytes->empty() || Font->Bytes->size() > AssetLimits::MaximumFontBytes ||
				Font->FaceCount == 0 || Font->FaceCount > 64)
				return std::unexpected(Error("MalformedFont", "Canonical font value is invalid"));
			AppendU32(Artifact, Font->FaceCount);
			AppendU32(Artifact, static_cast<std::uint32_t>(Font->Bytes->size()));
			Artifact.insert(Artifact.end(), Font->Bytes->begin(), Font->Bytes->end());
		} else if (const auto *Mesh = std::get_if<ImportedMesh>(&Asset)) {
			if (!Mesh->Vertices || !Mesh->Indices || Mesh->Vertices->empty() || Mesh->Indices->empty() ||
				Mesh->Vertices->size() > AssetLimits::MaximumMeshVertices ||
				Mesh->Indices->size() > AssetLimits::MaximumMeshIndices)
				return std::unexpected(Error("MalformedMesh", "Canonical mesh value is invalid"));
			auto Primitives = Mesh->Primitives;
			if (!Primitives) Primitives = std::make_shared<const std::vector<ImportedMeshPrimitive>>(
				std::vector<ImportedMeshPrimitive>{{0, static_cast<std::uint32_t>(Mesh->Indices->size()), std::nullopt}}
			);
			if (Primitives->empty() || Primitives->size() > AssetLimits::MaximumMeshSubmeshes)
				return std::unexpected(Error("MalformedTopology", "Canonical mesh primitive count is invalid"));
			AppendU32(Artifact, static_cast<std::uint32_t>(Mesh->Vertices->size()));
			AppendU32(Artifact, static_cast<std::uint32_t>(Mesh->Indices->size()));
			AppendU32(Artifact, static_cast<std::uint32_t>(Primitives->size()));
			for (const auto Component : {Mesh->Bounds.Minimum.x, Mesh->Bounds.Minimum.y, Mesh->Bounds.Minimum.z,
				Mesh->Bounds.Maximum.x, Mesh->Bounds.Maximum.y, Mesh->Bounds.Maximum.z}) AppendFloat(Artifact, Component);
			for (const auto &Vertex : *Mesh->Vertices)
				for (const auto Component : {Vertex.Position.x, Vertex.Position.y, Vertex.Position.z, Vertex.Normal.x,
					Vertex.Normal.y, Vertex.Normal.z, Vertex.Tangent.x, Vertex.Tangent.y, Vertex.Tangent.z,
					Vertex.Tangent.w, Vertex.TextureCoordinate.x, Vertex.TextureCoordinate.y}) AppendFloat(Artifact, Component);
			for (const auto Index : *Mesh->Indices) {
				if (Index >= Mesh->Vertices->size()) return std::unexpected(Error("InvalidMeshIndex", "Canonical mesh index is invalid"));
				AppendU32(Artifact, Index);
			}
			for (const auto &Primitive : *Primitives) {
				if (Primitive.IndexCount == 0 || Primitive.IndexCount % 3 != 0 ||
					Primitive.FirstIndex > Mesh->Indices->size() ||
					Primitive.IndexCount > Mesh->Indices->size() - Primitive.FirstIndex)
					return std::unexpected(Error("MalformedTopology", "Canonical mesh primitive range is invalid"));
				AppendU32(Artifact, Primitive.FirstIndex);
				AppendU32(Artifact, Primitive.IndexCount);
				Artifact.push_back(Primitive.Material.has_value() ? 1 : 0);
				if (Primitive.Material) {
					if (!Primitive.Material->IsValid()) return std::unexpected(Error("InvalidDependency", "Mesh material dependency is invalid"));
					AppendU64(Artifact, Primitive.Material->High);
					AppendU64(Artifact, Primitive.Material->Low);
				}
			}
		} else if (const auto *Material = std::get_if<ImportedMaterial>(&Asset)) {
			for (const auto Component : {Material->BaseColorFactor.x, Material->BaseColorFactor.y,
				Material->BaseColorFactor.z, Material->BaseColorFactor.w, Material->MetallicFactor,
				Material->RoughnessFactor, Material->AlphaCutoff})
				if (!std::isfinite(Component)) return std::unexpected(Error("MalformedMaterial", "Material factor is non-finite"));
			for (const auto Component : {Material->BaseColorFactor.x, Material->BaseColorFactor.y,
				Material->BaseColorFactor.z, Material->BaseColorFactor.w, Material->MetallicFactor,
				Material->RoughnessFactor}) AppendFloat(Artifact, Component);
			auto AppendReference = [&](const std::optional<AssetId> &Reference) {
				Artifact.push_back(Reference.has_value() ? 1 : 0);
				if (Reference) { AppendU64(Artifact, Reference->High); AppendU64(Artifact, Reference->Low); }
			};
			AppendReference(Material->BaseColorTexture);
			AppendReference(Material->NormalTexture);
			Artifact.push_back(static_cast<std::uint8_t>(Material->AlphaMode));
			AppendFloat(Artifact, Material->AlphaCutoff);
			Artifact.push_back(Material->DoubleSided ? 1 : 0);
		} else if (const auto *Audio = std::get_if<ImportedAudio>(&Asset)) {
			const auto SampleCount = static_cast<std::uint64_t>(Audio->FrameCount) * Audio->Channels;
			if (!Audio->Pcm16 || Audio->Channels == 0 || Audio->Channels > AssetLimits::MaximumAudioChannels ||
				Audio->SampleRate < AssetLimits::MinimumAudioSampleRate ||
				Audio->SampleRate > AssetLimits::MaximumAudioSampleRate || Audio->FrameCount == 0 ||
				Audio->FrameCount > AssetLimits::MaximumAudioFrames ||
				Audio->FrameCount > Audio->SampleRate * AssetLimits::MaximumAudioDurationSeconds ||
				SampleCount != Audio->Pcm16->size() ||
				SampleCount * sizeof(std::int16_t) > AssetLimits::MaximumAudioPcmBytes)
				return std::unexpected(Error("MalformedAudio", "Canonical audio value is invalid"));
			AppendU32(Artifact, Audio->SampleRate);
			Artifact.push_back(Audio->Channels);
			AppendU32(Artifact, Audio->FrameCount);
			for (const auto Sample : *Audio->Pcm16)
				AppendU16(Artifact, std::bit_cast<std::uint16_t>(Sample));
		}
		if (Artifact.size() > AssetLimits::MaximumArtifactBytes)
			return std::unexpected(Error("ArtifactLimit", "Canonical artifact exceeds its byte limit"));
		return std::make_shared<const std::vector<std::uint8_t>>(std::move(Artifact));
	}

	std::expected<AssetImportCandidate, AssetDiagnostic> DecodeAssetArtifact(
		std::span<const std::uint8_t> Artifact,
		AssetKind ExpectedKind,
		const AssetContentId &ExpectedContentId
	) {
		if (Artifact.size() < 13 || Artifact.size() > AssetLimits::MaximumArtifactBytes ||
			!std::equal(ArtifactMagic.begin(), ArtifactMagic.end(), Artifact.begin()))
			return std::unexpected(Error("MalformedArtifact", "Asset artifact header is invalid"));
		if (AssetContentId::Hash(Artifact) != ExpectedContentId)
			return std::unexpected(Error("IntegrityFailure", "Asset artifact content hash does not match the catalog"));
		std::size_t Offset = ArtifactMagic.size();
		auto Version = ReadU32(Artifact, Offset);
		if (!Version || (*Version != ArtifactVersion1 && *Version != ArtifactVersion2) || Offset >= Artifact.size() ||
			Artifact[Offset++] != static_cast<std::uint8_t>(ExpectedKind))
			return std::unexpected(Error("UnsupportedArtifact", "Asset artifact version or kind is unsupported"));

		if (ExpectedKind == AssetKind::Image) {
			auto Width = ReadU32(Artifact, Offset), Height = ReadU32(Artifact, Offset);
			if (!Width || !Height || *Width == 0 || *Height == 0 || *Width > AssetLimits::MaximumImageDimension ||
				*Height > AssetLimits::MaximumImageDimension)
				return std::unexpected(Error("MalformedArtifact", "Image artifact dimensions are invalid"));
			const auto Bytes = static_cast<std::size_t>(*Width) * *Height * 4;
			if (Bytes > AssetLimits::MaximumImageBytes || Offset > Artifact.size() || Artifact.size() - Offset != Bytes)
				return std::unexpected(Error("MalformedArtifact", "Image artifact payload length is invalid"));
			auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin() + Offset, Artifact.end());
			return AssetImportCandidate{ImportedImage{*Width, *Height, Pixels},
				std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin(), Artifact.end()), ExpectedContentId};
		}

		if (ExpectedKind == AssetKind::Font) {
			auto Faces = ReadU32(Artifact, Offset), ByteCount = ReadU32(Artifact, Offset);
			if (!Faces || !ByteCount || *Faces == 0 || *Faces > 64 || *ByteCount == 0 ||
				*ByteCount > AssetLimits::MaximumFontBytes || Offset > Artifact.size() || Artifact.size() - Offset != *ByteCount)
				return std::unexpected(Error("MalformedArtifact", "Font artifact metadata is invalid"));
			auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin() + Offset, Artifact.end());
			return AssetImportCandidate{ImportedFont{Bytes, *Faces},
				std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin(), Artifact.end()), ExpectedContentId};
		}

		if (ExpectedKind == AssetKind::Material) {
			if (*Version != ArtifactVersion2)
				return std::unexpected(Error("UnsupportedArtifact", "Material assets require artifact version 2"));
			ImportedMaterial Material;
			std::array<float *, 6> Factors{&Material.BaseColorFactor.x, &Material.BaseColorFactor.y,
				&Material.BaseColorFactor.z, &Material.BaseColorFactor.w, &Material.MetallicFactor,
				&Material.RoughnessFactor};
			for (auto *Destination : Factors) {
				auto Value = ReadFloat(Artifact, Offset);
				if (!Value) return std::unexpected(Error("MalformedArtifact", "Material artifact factor is invalid"));
				*Destination = *Value;
			}
			auto ReadReference = [&]() -> std::expected<std::optional<AssetId>, AssetDiagnostic> {
				if (Offset >= Artifact.size() || Artifact[Offset] > 1)
					return std::unexpected(Error("MalformedArtifact", "Material artifact dependency flag is invalid"));
				if (Artifact[Offset++] == 0) return std::optional<AssetId>{};
				auto High = ReadU64(Artifact, Offset), Low = ReadU64(Artifact, Offset);
				if (!High || !Low || !AssetId{*High, *Low}.IsValid())
					return std::unexpected(Error("MalformedArtifact", "Material artifact dependency is invalid"));
				return std::optional(AssetId{*High, *Low});
			};
			auto BaseColorTexture = ReadReference();
			if (!BaseColorTexture) return std::unexpected(BaseColorTexture.error());
			auto NormalTexture = ReadReference();
			if (!NormalTexture) return std::unexpected(NormalTexture.error());
			Material.BaseColorTexture = *BaseColorTexture;
			Material.NormalTexture = *NormalTexture;
			if (Offset >= Artifact.size() || Artifact[Offset] > static_cast<std::uint8_t>(AssetMaterialAlphaMode::Blend))
				return std::unexpected(Error("MalformedArtifact", "Material artifact alpha mode is invalid"));
			Material.AlphaMode = static_cast<AssetMaterialAlphaMode>(Artifact[Offset++]);
			auto AlphaCutoff = ReadFloat(Artifact, Offset);
			if (!AlphaCutoff || Offset >= Artifact.size() || Artifact[Offset] > 1)
				return std::unexpected(Error("MalformedArtifact", "Material artifact alpha state is invalid"));
			Material.AlphaCutoff = *AlphaCutoff;
			Material.DoubleSided = Artifact[Offset++] != 0;
			if (Offset != Artifact.size())
				return std::unexpected(Error("MalformedArtifact", "Material artifact has trailing bytes"));
			return AssetImportCandidate{Material,
				std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin(), Artifact.end()), ExpectedContentId};
		}

		if (ExpectedKind == AssetKind::Audio) {
			if (*Version != ArtifactVersion2)
				return std::unexpected(Error("UnsupportedArtifact", "Audio assets require artifact version 2"));
			auto SampleRate = ReadU32(Artifact, Offset);
			if (!SampleRate || Offset >= Artifact.size())
				return std::unexpected(Error("MalformedArtifact", "Audio artifact metadata is incomplete"));
			const auto Channels = Artifact[Offset++];
			auto FrameCount = ReadU32(Artifact, Offset);
			if (!FrameCount || Channels == 0 || Channels > AssetLimits::MaximumAudioChannels ||
				*SampleRate < AssetLimits::MinimumAudioSampleRate ||
				*SampleRate > AssetLimits::MaximumAudioSampleRate || *FrameCount == 0 ||
				*FrameCount > AssetLimits::MaximumAudioFrames ||
				*FrameCount > *SampleRate * AssetLimits::MaximumAudioDurationSeconds)
				return std::unexpected(Error("MalformedArtifact", "Audio artifact metadata is outside its limits"));
			const auto SampleCount = static_cast<std::uint64_t>(*FrameCount) * Channels;
			const auto PcmBytes = SampleCount * sizeof(std::int16_t);
			if (PcmBytes > AssetLimits::MaximumAudioPcmBytes || Offset > Artifact.size() ||
				Artifact.size() - Offset != PcmBytes)
				return std::unexpected(Error("MalformedArtifact", "Audio artifact sample payload is invalid"));
			auto Pcm = std::make_shared<std::vector<std::int16_t>>();
			Pcm->reserve(static_cast<std::size_t>(SampleCount));
			while (Offset < Artifact.size()) {
				auto Sample = ReadU16(Artifact, Offset);
				if (!Sample) return std::unexpected(Error("MalformedArtifact", "Audio artifact sample is truncated"));
				Pcm->push_back(std::bit_cast<std::int16_t>(*Sample));
			}
			return AssetImportCandidate{
				ImportedAudio{
					*SampleRate, Channels, *FrameCount, std::shared_ptr<const std::vector<std::int16_t>>(std::move(Pcm))
				},
				std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin(), Artifact.end()),
				ExpectedContentId,
			};
		}

		auto VertexCount = ReadU32(Artifact, Offset), IndexCount = ReadU32(Artifact, Offset), SubmeshCount = ReadU32(Artifact, Offset);
		if (!VertexCount || !IndexCount || !SubmeshCount || *VertexCount == 0 || *IndexCount == 0 ||
			*VertexCount > AssetLimits::MaximumMeshVertices || *IndexCount > AssetLimits::MaximumMeshIndices ||
			*SubmeshCount == 0 || *SubmeshCount > AssetLimits::MaximumMeshSubmeshes)
			return std::unexpected(Error("MalformedArtifact", "Mesh artifact counts are invalid"));
		std::array<float, 6> BoundsValues{};
		for (std::size_t Index = 0; Index < 6; ++Index) {
			auto Value = ReadFloat(Artifact, Offset);
			if (!Value) return std::unexpected(Error("MalformedArtifact", "Mesh artifact bounds are invalid"));
			BoundsValues[Index] = *Value;
		}
		RenderBounds Bounds{{BoundsValues[0], BoundsValues[1], BoundsValues[2]},
			{BoundsValues[3], BoundsValues[4], BoundsValues[5]}};
		auto Vertices = std::make_shared<std::vector<RenderVertex>>(*VertexCount);
		for (auto &Vertex : *Vertices) {
			std::array<float *, 12> Values{&Vertex.Position.x, &Vertex.Position.y, &Vertex.Position.z,
				&Vertex.Normal.x, &Vertex.Normal.y, &Vertex.Normal.z, &Vertex.Tangent.x, &Vertex.Tangent.y,
				&Vertex.Tangent.z, &Vertex.Tangent.w, &Vertex.TextureCoordinate.x, &Vertex.TextureCoordinate.y};
			for (auto *Destination : Values) {
				auto Value = ReadFloat(Artifact, Offset);
				if (!Value) return std::unexpected(Error("MalformedArtifact", "Mesh artifact vertex is invalid"));
				*Destination = *Value;
			}
			if (glm::length(Vertex.Normal) < 1e-8f)
				return std::unexpected(Error("MalformedArtifact", "Mesh artifact contains a zero normal"));
		}
		auto Indices = std::make_shared<std::vector<std::uint32_t>>();
		Indices->reserve(*IndexCount);
		for (std::uint32_t Index = 0; Index < *IndexCount; ++Index) {
			auto Value = ReadU32(Artifact, Offset);
			if (!Value || *Value >= *VertexCount) return std::unexpected(Error("MalformedArtifact", "Mesh artifact index is invalid"));
			Indices->push_back(*Value);
		}
		auto Primitives = std::make_shared<std::vector<ImportedMeshPrimitive>>();
		if (*Version == ArtifactVersion1) {
			if (*SubmeshCount != 1)
				return std::unexpected(Error("UnsupportedArtifact", "Version 1 mesh artifacts support exactly one primitive"));
			Primitives->push_back({0, *IndexCount, std::nullopt});
		} else {
			Primitives->reserve(*SubmeshCount);
			for (std::uint32_t PrimitiveIndex = 0; PrimitiveIndex < *SubmeshCount; ++PrimitiveIndex) {
				auto FirstIndex = ReadU32(Artifact, Offset), PrimitiveIndexCount = ReadU32(Artifact, Offset);
				if (!FirstIndex || !PrimitiveIndexCount || *PrimitiveIndexCount == 0 || *PrimitiveIndexCount % 3 != 0 ||
					*FirstIndex > *IndexCount || *PrimitiveIndexCount > *IndexCount - *FirstIndex ||
					Offset >= Artifact.size() || Artifact[Offset] > 1)
					return std::unexpected(Error("MalformedArtifact", "Mesh artifact primitive range is invalid"));
				std::optional<AssetId> Material;
				if (Artifact[Offset++] != 0) {
					auto High = ReadU64(Artifact, Offset), Low = ReadU64(Artifact, Offset);
					if (!High || !Low || !AssetId{*High, *Low}.IsValid())
						return std::unexpected(Error("MalformedArtifact", "Mesh artifact material dependency is invalid"));
					Material = AssetId{*High, *Low};
				}
				Primitives->push_back({*FirstIndex, *PrimitiveIndexCount, Material});
			}
		}
		if (Offset != Artifact.size()) return std::unexpected(Error("MalformedArtifact", "Mesh artifact has trailing bytes"));
		return AssetImportCandidate{ImportedMesh{Vertices, Indices, Bounds, *SubmeshCount, Primitives},
			std::make_shared<const std::vector<std::uint8_t>>(Artifact.begin(), Artifact.end()), ExpectedContentId};
	}
}
