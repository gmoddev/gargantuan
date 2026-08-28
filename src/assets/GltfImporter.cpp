// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "AssetImporter.hpp"

#include "gargantuan/filesystem/SourceMount.hpp"
#include "serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace gargantuan {
	namespace {
		using Json = JsonCodec::Json;

		struct ParsedGltf {
			Json Root;
			std::shared_ptr<const std::vector<std::uint8_t>> BinaryChunk;
		};

		struct BufferViewValue {
			std::span<const std::uint8_t> Bytes;
			std::size_t Stride = 0;
		};

		AssetDiagnostic Error(std::string Code, std::string Message) {
			if (Message.size() > AssetLimits::MaximumDiagnosticBytes) Message.resize(AssetLimits::MaximumDiagnosticBytes);
			return {std::move(Code), std::move(Message)};
		}

		bool Cancelled(const AssetImportContext &Context) {
			return Context.Cancellation.IsCancelled() || std::chrono::steady_clock::now() > Context.Deadline;
		}

		std::optional<std::uint32_t> ReadU32(std::span<const std::uint8_t> Bytes, std::size_t Offset) {
			if (Offset > Bytes.size() || Bytes.size() - Offset < 4) return std::nullopt;
			return static_cast<std::uint32_t>(Bytes[Offset]) |
				(static_cast<std::uint32_t>(Bytes[Offset + 1]) << 8) |
				(static_cast<std::uint32_t>(Bytes[Offset + 2]) << 16) |
				(static_cast<std::uint32_t>(Bytes[Offset + 3]) << 24);
		}

		std::optional<std::size_t> Unsigned(const Json &Value) {
			if (!Value.is_number_unsigned()) return std::nullopt;
			const auto Number = Value.get<std::uint64_t>();
			return Number <= std::numeric_limits<std::size_t>::max() ? std::optional(static_cast<std::size_t>(Number)) : std::nullopt;
		}

		std::expected<ParsedGltf, AssetDiagnostic> ParseGltf(
			std::span<const std::uint8_t> Source,
			std::string_view Extension
		) {
			if (Source.empty() || Source.size() > AssetLimits::MaximumSourceBytes)
				return std::unexpected(Error("GltfLimit", "glTF source byte count is outside the asset limit"));
			std::string JsonText;
			std::shared_ptr<const std::vector<std::uint8_t>> Binary;
			if (Extension == ".glb") {
				if (Source.size() < 20 || ReadU32(Source, 0) != 0x46546c67u || ReadU32(Source, 4) != 2u ||
					ReadU32(Source, 8) != Source.size())
					return std::unexpected(Error("MalformedGlb", "GLB header, version, or declared length is invalid"));
				std::size_t Offset = 12;
				std::size_t ChunkCount = 0;
				bool SawJson = false;
				while (Offset < Source.size()) {
					if (++ChunkCount > AssetLimits::MaximumGltfChunks || Source.size() - Offset < 8)
						return std::unexpected(Error("MalformedGlb", "GLB chunk table is invalid or exceeds its limit"));
					const auto Length = ReadU32(Source, Offset);
					const auto Type = ReadU32(Source, Offset + 4);
					Offset += 8;
					if (!Length || !Type || *Length > Source.size() - Offset || *Length % 4 != 0)
						return std::unexpected(Error("MalformedGlb", "GLB chunk length is invalid"));
					if (!SawJson) {
						if (*Type != 0x4e4f534au || *Length > AssetLimits::MaximumGltfJsonBytes)
							return std::unexpected(Error("MalformedGlb", "GLB first chunk must be bounded JSON"));
						JsonText.assign(reinterpret_cast<const char *>(Source.data() + Offset), *Length);
						while (!JsonText.empty() && (JsonText.back() == ' ' || JsonText.back() == '\0')) JsonText.pop_back();
						SawJson = true;
					} else if (*Type == 0x004e4942u) {
						if (Binary) return std::unexpected(Error("MalformedGlb", "GLB contains more than one BIN chunk"));
						Binary = std::make_shared<const std::vector<std::uint8_t>>(Source.begin() + Offset, Source.begin() + Offset + *Length);
					}
					Offset += *Length;
				}
				if (!SawJson || Offset != Source.size()) return std::unexpected(Error("MalformedGlb", "GLB has no complete JSON chunk"));
			} else if (Extension == ".gltf") {
				if (Source.size() > AssetLimits::MaximumGltfJsonBytes)
					return std::unexpected(Error("GltfLimit", "glTF JSON exceeds its byte limit"));
				JsonText.assign(reinterpret_cast<const char *>(Source.data()), Source.size());
			} else return std::unexpected(Error("UnsupportedFormat", "glTF importer supports only .gltf and .glb"));

			auto Parsed = JsonCodec::Parse(JsonText, AssetLimits::MaximumGltfJsonBytes, "glTF");
			if (!Parsed || !Parsed->is_object())
				return std::unexpected(Error("MalformedGltf", Parsed ? "glTF root must be an object" : Parsed.error().Message));
			if (!Parsed->contains("asset") || !(*Parsed)["asset"].is_object() ||
				!(*Parsed)["asset"].contains("version") || !(*Parsed)["asset"]["version"].is_string() ||
				!(*Parsed)["asset"]["version"].get_ref<const std::string &>().starts_with("2."))
				return std::unexpected(Error("UnsupportedGltf", "glTF asset.version must be 2.x"));
			if (Parsed->contains("extensionsRequired") &&
				(!(*Parsed)["extensionsRequired"].is_array() || !(*Parsed)["extensionsRequired"].empty()))
				return std::unexpected(Error("UnsupportedGltfFeature", "Required glTF extensions are not supported"));
			return ParsedGltf{std::move(*Parsed), std::move(Binary)};
		}

		std::expected<std::string, AssetDiagnostic> ValidateExternalUri(std::string_view Uri) {
			if (Uri.empty() || Uri.size() > AssetLimits::MaximumSourcePathBytes || Uri.find('\0') != std::string_view::npos)
				return std::unexpected(Error("InvalidGltfUri", "glTF external URI is empty or oversized"));
			if (Uri.starts_with("data:")) return std::string(Uri);
			if (Uri.starts_with('/') || Uri.starts_with('\\') || Uri.find(':') != std::string_view::npos ||
				Uri.find('?') != std::string_view::npos || Uri.find('#') != std::string_view::npos ||
				Uri.find('%') != std::string_view::npos || Uri.find('\\') != std::string_view::npos)
				return std::unexpected(Error("ExternalUriRejected", "glTF URI must be a plain project-relative path"));
			std::size_t Depth = 0;
			std::size_t Begin = 0;
			while (Begin <= Uri.size()) {
				const auto End = Uri.find('/', Begin);
				const auto Component = Uri.substr(Begin, End == std::string_view::npos ? Uri.size() - Begin : End - Begin);
				if (Component.empty() || Component == ".")
					return std::unexpected(Error("InvalidGltfUri", "glTF URI contains an empty or dot path component"));
				if (Component == "..") return std::unexpected(Error("PathEscape", "glTF URI traversal is not allowed"));
				if (++Depth > MaximumSourceMountTraversalDepth)
					return std::unexpected(Error("TraversalDepthLimit", "glTF URI exceeds the traversal depth limit"));
				if (End == std::string_view::npos) break;
				Begin = End + 1;
			}
			return std::string(Uri);
		}

		std::expected<std::vector<std::uint8_t>, AssetDiagnostic> DecodeBase64(std::string_view Encoded) {
			static constexpr std::string_view Alphabet =
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			if (Encoded.size() > (AssetLimits::MaximumGltfDataUriBytes * 4 / 3 + 4))
				return std::unexpected(Error("DataUriLimit", "glTF data URI exceeds its encoded byte limit"));
			std::vector<std::uint8_t> Result;
			Result.reserve(Encoded.size() * 3 / 4);
			std::uint32_t Accumulator = 0;
			int Bits = 0;
			bool Padding = false;
			for (const char Character : Encoded) {
				if (Character == '=') { Padding = true; continue; }
				if (Padding) return std::unexpected(Error("MalformedDataUri", "glTF data URI padding is invalid"));
				const auto Position = Alphabet.find(Character);
				if (Position == std::string_view::npos)
					return std::unexpected(Error("MalformedDataUri", "glTF data URI is not canonical base64"));
				Accumulator = (Accumulator << 6) | static_cast<std::uint32_t>(Position);
				Bits += 6;
				if (Bits >= 8) {
					Bits -= 8;
					Result.push_back(static_cast<std::uint8_t>(Accumulator >> Bits));
					Accumulator &= Bits == 0 ? 0 : (1u << Bits) - 1u;
					if (Result.size() > AssetLimits::MaximumGltfDataUriBytes)
						return std::unexpected(Error("DataUriLimit", "glTF data URI exceeds its decoded byte limit"));
				}
			}
			return Result;
		}

		std::expected<std::vector<std::uint8_t>, AssetDiagnostic> ResolveUriBytes(
			std::string_view Uri,
			const AssetImportContext &Context
		) {
			auto Validated = ValidateExternalUri(Uri);
			if (!Validated) return std::unexpected(Validated.error());
			if (Uri.starts_with("data:")) {
				const auto Separator = Uri.find(',');
				if (Separator == std::string_view::npos || Uri.substr(0, Separator).find(";base64") == std::string_view::npos)
					return std::unexpected(Error("UnsupportedDataUri", "Only bounded base64 glTF data URIs are supported"));
				return DecodeBase64(Uri.substr(Separator + 1));
			}
			if (!Context.ExternalResources)
				return std::unexpected(Error("MissingExternalResource", "glTF external resource capture is unavailable"));
			const auto Found = Context.ExternalResources->find(std::string(Uri));
			if (Found == Context.ExternalResources->end() || !Found->second)
				return std::unexpected(Error("MissingExternalResource", "glTF external resource was not captured through SourceMount"));
			return std::vector<std::uint8_t>(Found->second->begin(), Found->second->end());
		}

		std::string BoundedName(const Json &Object, std::string Fallback) {
			if (!Object.is_object() || !Object.contains("name") || !Object["name"].is_string()) return Fallback;
			auto Result = Object["name"].get<std::string>();
			if (Result.empty()) return Fallback;
			if (Result.size() > AssetLimits::MaximumNameBytes) Result.resize(AssetLimits::MaximumNameBytes);
			return Result;
		}

		std::string KeyHash(std::string_view Prefix, std::string_view Identity) {
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Identity.data()), Identity.size());
			return std::string(Prefix) + AssetContentId::Hash(Bytes).ToString().substr(0, 24);
		}

		std::expected<std::vector<std::vector<std::uint8_t>>, AssetDiagnostic> LoadBuffers(
			const ParsedGltf &Document,
			const AssetImportContext &Context
		) {
			if (!Document.Root.contains("buffers") || !Document.Root["buffers"].is_array() ||
				Document.Root["buffers"].empty() || Document.Root["buffers"].size() > AssetLimits::MaximumGltfBuffers)
				return std::unexpected(Error("MalformedGltf", "glTF buffers array is missing or exceeds its limit"));
			std::vector<std::vector<std::uint8_t>> Result;
			Result.reserve(Document.Root["buffers"].size());
			for (std::size_t Index = 0; Index < Document.Root["buffers"].size(); ++Index) {
				const auto &Buffer = Document.Root["buffers"][Index];
				if (!Buffer.is_object() || !Buffer.contains("byteLength"))
					return std::unexpected(Error("MalformedGltf", "glTF buffer has no byteLength"));
				auto ByteLength = Unsigned(Buffer["byteLength"]);
				if (!ByteLength || *ByteLength == 0 || *ByteLength > AssetLimits::MaximumSourceBytes)
					return std::unexpected(Error("GltfLimit", "glTF buffer byteLength is invalid or oversized"));
				std::vector<std::uint8_t> Bytes;
				if (Buffer.contains("uri")) {
					if (!Buffer["uri"].is_string()) return std::unexpected(Error("MalformedGltf", "glTF buffer URI must be a string"));
					auto Resolved = ResolveUriBytes(Buffer["uri"].get_ref<const std::string &>(), Context);
					if (!Resolved) return std::unexpected(Resolved.error());
					Bytes = std::move(*Resolved);
				} else if (Index == 0 && Document.BinaryChunk) Bytes.assign(Document.BinaryChunk->begin(), Document.BinaryChunk->end());
				else return std::unexpected(Error("MissingExternalResource", "glTF buffer has neither URI nor GLB BIN content"));
				if (Bytes.size() < *ByteLength)
					return std::unexpected(Error("MalformedGltf", "glTF buffer is shorter than its declared byteLength"));
				Bytes.resize(*ByteLength);
				Result.push_back(std::move(Bytes));
			}
			return Result;
		}

		std::size_t ComponentSize(std::uint32_t Type) {
			switch (Type) {
				case 5120: case 5121: return 1;
				case 5122: case 5123: return 2;
				case 5125: case 5126: return 4;
			}
			return 0;
		}

		std::size_t ComponentCount(std::string_view Type) {
			if (Type == "SCALAR") return 1;
			if (Type == "VEC2") return 2;
			if (Type == "VEC3") return 3;
			if (Type == "VEC4") return 4;
			if (Type == "MAT4") return 16;
			return 0;
		}

		std::expected<double, AssetDiagnostic> ReadComponent(
			std::span<const std::uint8_t> Bytes,
			std::size_t Offset,
			std::uint32_t Type,
			bool Normalized
		) {
			if (Offset > Bytes.size() || ComponentSize(Type) > Bytes.size() - Offset)
				return std::unexpected(Error("AccessorOutOfRange", "glTF accessor component is out of range"));
			switch (Type) {
				case 5120: {
					const auto Value = std::bit_cast<std::int8_t>(Bytes[Offset]);
					return Normalized ? std::max(-1.0, static_cast<double>(Value) / 127.0) : Value;
				}
				case 5121: return Normalized ? static_cast<double>(Bytes[Offset]) / 255.0 : Bytes[Offset];
				case 5122: {
					const auto Raw = static_cast<std::uint16_t>(
						static_cast<std::uint16_t>(Bytes[Offset]) |
						static_cast<std::uint16_t>(static_cast<std::uint16_t>(Bytes[Offset + 1]) << 8)
					);
					const auto Value = std::bit_cast<std::int16_t>(Raw);
					return Normalized ? std::max(-1.0, static_cast<double>(Value) / 32767.0) : Value;
				}
				case 5123: {
					const auto Value = static_cast<std::uint16_t>(Bytes[Offset]) | static_cast<std::uint16_t>(Bytes[Offset + 1]) << 8;
					return Normalized ? static_cast<double>(Value) / 65535.0 : Value;
				}
				case 5125: {
					const auto Value = ReadU32(Bytes, Offset);
					return Value ? std::expected<double, AssetDiagnostic>(*Value) :
						std::unexpected(Error("AccessorOutOfRange", "glTF uint accessor is out of range"));
				}
				case 5126: {
					const auto Raw = ReadU32(Bytes, Offset);
					if (!Raw) return std::unexpected(Error("AccessorOutOfRange", "glTF float accessor is out of range"));
					const auto Value = std::bit_cast<float>(*Raw);
					return std::isfinite(Value) ? std::expected<double, AssetDiagnostic>(Value) :
						std::unexpected(Error("NonFiniteGltf", "glTF accessor contains NaN or infinity"));
				}
			}
			return std::unexpected(Error("UnsupportedAccessor", "glTF accessor component type is unsupported"));
		}

		std::expected<std::vector<double>, AssetDiagnostic> ReadAccessor(
			const Json &Root,
			const std::vector<std::vector<std::uint8_t>> &Buffers,
			std::size_t AccessorIndex,
			std::string_view ExpectedType,
			std::optional<std::uint32_t> RequiredComponent = std::nullopt
		) {
			if (!Root.contains("accessors") || !Root["accessors"].is_array() ||
				AccessorIndex >= Root["accessors"].size() || Root["accessors"].size() > AssetLimits::MaximumGltfAccessors)
				return std::unexpected(Error("InvalidAccessor", "glTF accessor index is invalid"));
			const auto &Accessor = Root["accessors"][AccessorIndex];
			if (!Accessor.is_object() || Accessor.contains("sparse") || !Accessor.contains("bufferView") ||
				!Accessor.contains("componentType") || !Accessor.contains("count") || !Accessor.contains("type") ||
				!Accessor["type"].is_string() || Accessor["type"].get<std::string>() != ExpectedType)
				return std::unexpected(Error("UnsupportedAccessor", "glTF accessor shape or sparse storage is unsupported"));
			auto ViewIndex = Unsigned(Accessor["bufferView"]), Count = Unsigned(Accessor["count"]),
				Component = Unsigned(Accessor["componentType"]);
			if (!ViewIndex || !Count || !Component || *Count == 0 || *Count > AssetLimits::MaximumMeshIndices ||
				*Component > std::numeric_limits<std::uint32_t>::max())
				return std::unexpected(Error("InvalidAccessor", "glTF accessor metadata is invalid"));
			const auto ComponentType = static_cast<std::uint32_t>(*Component);
			if (RequiredComponent && ComponentType != *RequiredComponent)
				return std::unexpected(Error("UnsupportedAccessor", "glTF accessor component type does not match the supported semantic"));
			if (RequiredComponent && Accessor.contains("normalized") &&
				(!Accessor["normalized"].is_boolean() || Accessor["normalized"].get<bool>()))
				return std::unexpected(Error("InvalidAccessor", "glTF float geometry accessors cannot be normalized"));
			const auto ComponentBytes = ComponentSize(ComponentType), Components = ComponentCount(ExpectedType);
			if (ComponentBytes == 0 || Components == 0)
				return std::unexpected(Error("UnsupportedAccessor", "glTF accessor component or vector type is unsupported"));
			if (!Root.contains("bufferViews") || !Root["bufferViews"].is_array() ||
				Root["bufferViews"].size() > AssetLimits::MaximumGltfBufferViews || *ViewIndex >= Root["bufferViews"].size())
				return std::unexpected(Error("InvalidBufferView", "glTF bufferView index is invalid"));
			const auto &View = Root["bufferViews"][*ViewIndex];
			if (!View.is_object() || !View.contains("buffer") || !View.contains("byteLength"))
				return std::unexpected(Error("InvalidBufferView", "glTF bufferView metadata is incomplete"));
			auto BufferIndex = Unsigned(View["buffer"]), ViewLength = Unsigned(View["byteLength"]);
			const auto ViewOffset = View.contains("byteOffset") ? Unsigned(View["byteOffset"]) : std::optional<std::size_t>(0);
			const auto AccessorOffset = Accessor.contains("byteOffset") ? Unsigned(Accessor["byteOffset"]) : std::optional<std::size_t>(0);
			if (!BufferIndex || !ViewLength || !ViewOffset || !AccessorOffset || *BufferIndex >= Buffers.size() ||
				*ViewOffset > Buffers[*BufferIndex].size() || *ViewLength > Buffers[*BufferIndex].size() - *ViewOffset)
				return std::unexpected(Error("InvalidBufferView", "glTF bufferView range is invalid"));
			const auto ElementBytes = ComponentBytes * Components;
			const auto Stride = View.contains("byteStride") ? Unsigned(View["byteStride"]) : std::optional<std::size_t>(ElementBytes);
			if (!Stride || *Stride < ElementBytes || *Stride > 252 || *AccessorOffset > *ViewLength)
				return std::unexpected(Error("InvalidAccessor", "glTF accessor stride or offset is invalid"));
			if (*Count - 1 > (std::numeric_limits<std::size_t>::max() - ElementBytes) / *Stride)
				return std::unexpected(Error("GltfOverflow", "glTF accessor range overflows"));
			const auto RequiredBytes = (*Count - 1) * *Stride + ElementBytes;
			if (RequiredBytes > *ViewLength - *AccessorOffset)
				return std::unexpected(Error("AccessorOutOfRange", "glTF accessor exceeds its bufferView"));
			if (Accessor.contains("normalized") && !Accessor["normalized"].is_boolean())
				return std::unexpected(Error("InvalidAccessor", "glTF accessor normalized flag must be boolean"));
			const bool Normalized = Accessor.value("normalized", false);
			std::vector<double> Result;
			if (*Count > std::numeric_limits<std::size_t>::max() / Components)
				return std::unexpected(Error("GltfOverflow", "glTF accessor element count overflows"));
			Result.reserve(*Count * Components);
			const auto &Buffer = Buffers[*BufferIndex];
			const auto Start = *ViewOffset + *AccessorOffset;
			for (std::size_t Element = 0; Element < *Count; ++Element)
				for (std::size_t ComponentIndex = 0; ComponentIndex < Components; ++ComponentIndex) {
					auto Value = ReadComponent(Buffer, Start + Element * *Stride + ComponentIndex * ComponentBytes,
						ComponentType, Normalized);
					if (!Value) return std::unexpected(Value.error());
					Result.push_back(*Value);
				}
			return Result;
		}

		std::optional<std::uint32_t> AccessorComponentType(const Json &Root, std::size_t AccessorIndex) {
			if (!Root.contains("accessors") || !Root["accessors"].is_array() ||
				AccessorIndex >= Root["accessors"].size() || !Root["accessors"][AccessorIndex].is_object() ||
				!Root["accessors"][AccessorIndex].contains("componentType")) return std::nullopt;
			auto Value = Unsigned(Root["accessors"][AccessorIndex]["componentType"]);
			return Value && *Value <= std::numeric_limits<std::uint32_t>::max() ?
				std::optional(static_cast<std::uint32_t>(*Value)) : std::nullopt;
		}

		bool AccessorIsNormalized(const Json &Root, std::size_t AccessorIndex) {
			const auto &Accessor = Root["accessors"][AccessorIndex];
			return Accessor.contains("normalized") && Accessor["normalized"].is_boolean() &&
				Accessor["normalized"].get<bool>();
		}

		std::expected<ImportedImage, AssetDiagnostic> DecodeImage(
			std::span<const std::uint8_t> Bytes,
			std::string_view Type
		) {
			if (Bytes.empty() || Bytes.size() > AssetLimits::MaximumSourceBytes)
				return std::unexpected(Error("ImageLimit", "glTF image byte count is outside the asset limit"));
			auto *Stream = SDL_IOFromConstMem(Bytes.data(), Bytes.size());
			if (!Stream) return std::unexpected(Error("ImageDecode", SDL_GetError()));
			auto *Surface = IMG_LoadTyped_IO(Stream, true, std::string(Type).c_str());
			if (!Surface) return std::unexpected(Error("ImageDecode", SDL_GetError()));
			if (Surface->w <= 0 || Surface->h <= 0 || Surface->w > static_cast<int>(AssetLimits::MaximumImageDimension) ||
				Surface->h > static_cast<int>(AssetLimits::MaximumImageDimension) ||
				static_cast<std::uint64_t>(Surface->w) * Surface->h * 4 > AssetLimits::MaximumImageBytes) {
				SDL_DestroySurface(Surface);
				return std::unexpected(Error("ImageLimit", "Decoded glTF image dimensions exceed the asset limit"));
			}
			auto *Converted = SDL_ConvertSurface(Surface, SDL_PIXELFORMAT_RGBA32);
			SDL_DestroySurface(Surface);
			if (!Converted) return std::unexpected(Error("ImageDecode", SDL_GetError()));
			const auto Width = static_cast<std::uint32_t>(Converted->w), Height = static_cast<std::uint32_t>(Converted->h);
			auto Pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Width) * Height * 4);
			for (std::uint32_t Row = 0; Row < Height; ++Row)
				std::memcpy(Pixels->data() + static_cast<std::size_t>(Row) * Width * 4,
					static_cast<const std::uint8_t *>(Converted->pixels) + static_cast<std::size_t>(Row) * Converted->pitch,
					static_cast<std::size_t>(Width) * 4);
			SDL_DestroySurface(Converted);
			return ImportedImage{Width, Height, Pixels};
		}

		std::string ImageType(std::string_view Mime, std::string_view Uri) {
			if (Mime == "image/png" || Uri.ends_with(".png")) return "PNG";
			if (Mime == "image/jpeg" || Uri.ends_with(".jpg") || Uri.ends_with(".jpeg")) return "JPG";
			if (Mime == "image/bmp" || Uri.ends_with(".bmp")) return "BMP";
			return {};
		}

		std::expected<std::vector<std::uint8_t>, AssetDiagnostic> BufferViewBytes(
			const Json &Root,
			const std::vector<std::vector<std::uint8_t>> &Buffers,
			std::size_t ViewIndex
		) {
			if (!Root.contains("bufferViews") || !Root["bufferViews"].is_array() || ViewIndex >= Root["bufferViews"].size())
				return std::unexpected(Error("InvalidBufferView", "glTF image bufferView index is invalid"));
			const auto &View = Root["bufferViews"][ViewIndex];
			if (!View.is_object() || !View.contains("buffer") || !View.contains("byteLength"))
				return std::unexpected(Error("InvalidBufferView", "glTF image bufferView is incomplete"));
			auto BufferIndex = Unsigned(View["buffer"]), Length = Unsigned(View["byteLength"]);
			const auto Offset = View.contains("byteOffset") ? Unsigned(View["byteOffset"]) : std::optional<std::size_t>(0);
			if (!BufferIndex || !Length || !Offset || *BufferIndex >= Buffers.size() ||
				*Offset > Buffers[*BufferIndex].size() || *Length > Buffers[*BufferIndex].size() - *Offset)
				return std::unexpected(Error("InvalidBufferView", "glTF image bufferView range is invalid"));
			return std::vector<std::uint8_t>(Buffers[*BufferIndex].begin() + *Offset,
				Buffers[*BufferIndex].begin() + *Offset + *Length);
		}

		std::optional<std::size_t> TextureImageIndex(const Json &Root, std::size_t TextureIndex) {
			if (!Root.contains("textures") || !Root["textures"].is_array() ||
				TextureIndex >= Root["textures"].size() || !Root["textures"][TextureIndex].is_object() ||
				!Root["textures"][TextureIndex].contains("source")) return std::nullopt;
			return Unsigned(Root["textures"][TextureIndex]["source"]);
		}

		std::expected<AssetImportGraphCandidate, AssetDiagnostic> ImportDocument(
			const ParsedGltf &Document,
			const AssetImportContext &Context
		) {
			const auto &Root = Document.Root;
			if (!Root.contains("meshes") || !Root["meshes"].is_array() || Root["meshes"].empty())
				return std::unexpected(Error("MalformedGltf", "glTF must contain a nonempty meshes array"));
			if (Root["meshes"].size() > AssetLimits::MaximumGltfMeshes)
				return std::unexpected(Error("GltfLimit", "glTF mesh count exceeds its limit"));
			if (Root.contains("materials") && (!Root["materials"].is_array() || Root["materials"].size() > AssetLimits::MaximumGltfMaterials))
				return std::unexpected(Error("GltfLimit", "glTF materials array exceeds its limit"));
			if (Root.contains("images") && (!Root["images"].is_array() || Root["images"].size() > AssetLimits::MaximumGltfImages))
				return std::unexpected(Error("GltfLimit", "glTF images array exceeds its limit"));
			if (Root.contains("textures") && (!Root["textures"].is_array() || Root["textures"].size() > AssetLimits::MaximumGltfTextures))
				return std::unexpected(Error("GltfLimit", "glTF textures array exceeds its limit"));
			if (Root.contains("nodes") && (!Root["nodes"].is_array() || Root["nodes"].size() > AssetLimits::MaximumGltfNodes))
				return std::unexpected(Error("GltfLimit", "glTF nodes array is invalid or exceeds its limit"));
			if (Root.contains("skins") && (!Root["skins"].is_array() || Root["skins"].size() > AssetLimits::MaximumGltfSkins))
				return std::unexpected(Error("GltfLimit", "glTF skins array is invalid or exceeds its limit"));
			if (Root.contains("animations") && (!Root["animations"].is_array() ||
				Root["animations"].size() > AssetLimits::MaximumGltfAnimations))
				return std::unexpected(Error("GltfLimit", "glTF animations array is invalid or exceeds its limit"));
			if (Root.contains("textures")) for (const auto &Texture : Root["textures"]) {
				if (!Texture.is_object() || !Texture.contains("source"))
					return std::unexpected(Error("MalformedGltf", "glTF texture must contain an image source"));
				if (Texture.contains("sampler") || (Texture.contains("extensions") && !Texture["extensions"].empty()))
					return std::unexpected(Error("UnsupportedGltfFeature", "Explicit texture samplers and texture extensions are not represented in Foundation 2A"));
			}
			auto Buffers = LoadBuffers(Document, Context);
			if (!Buffers) return std::unexpected(Buffers.error());

			const auto NodeCount = Root.contains("nodes") ? Root["nodes"].size() : 0;
			std::vector<std::int32_t> NodeParents(NodeCount, -1);
			if (Root.contains("nodes")) {
				for (std::size_t NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex) {
					const auto &Node = Root["nodes"][NodeIndex];
					if (!Node.is_object()) return std::unexpected(Error("MalformedSkeleton", "glTF node must be an object"));
					if (!Node.contains("children")) continue;
					if (!Node["children"].is_array() || Node["children"].size() > AssetLimits::MaximumGltfNodes)
						return std::unexpected(Error("MalformedSkeleton", "glTF node children are invalid or oversized"));
					std::unordered_set<std::size_t> LocalChildren;
					for (const auto &ChildValue : Node["children"]) {
						auto Child = Unsigned(ChildValue);
						if (!Child || *Child >= NodeCount || *Child == NodeIndex || !LocalChildren.insert(*Child).second ||
							NodeParents[*Child] != -1)
							return std::unexpected(Error("MalformedSkeleton", "glTF node hierarchy has an invalid or multiply-parented child"));
						NodeParents[*Child] = static_cast<std::int32_t>(NodeIndex);
					}
				}
				std::vector<std::uint8_t> NodeVisit(NodeCount);
				std::function<bool(std::size_t)> VisitNode = [&](std::size_t NodeIndex) {
					if (NodeVisit[NodeIndex] == 1) return false;
					if (NodeVisit[NodeIndex] == 2) return true;
					NodeVisit[NodeIndex] = 1;
					const auto Parent = NodeParents[NodeIndex];
					if (Parent >= 0 && !VisitNode(static_cast<std::size_t>(Parent))) return false;
					NodeVisit[NodeIndex] = 2;
					return true;
				};
				for (std::size_t NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
					if (!VisitNode(NodeIndex)) return std::unexpected(Error("MalformedSkeleton", "glTF node hierarchy is cyclic"));
			}

			struct NodeTransform {
				glm::vec3 Translation{0.0f};
				glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
				glm::vec3 Scale{1.0f};
			};
			const glm::mat4 Handedness = [] {
				glm::mat4 Value(1.0f);
				Value[2][2] = -1.0f;
				return Value;
			}();
			auto ReadNodeTransform = [&](std::size_t NodeIndex) -> std::expected<NodeTransform, AssetDiagnostic> {
				if (NodeIndex >= NodeCount) return std::unexpected(Error("MalformedSkeleton", "glTF joint node index is invalid"));
				const auto &Node = Root["nodes"][NodeIndex];
				NodeTransform Result;
				auto ReadArray = [&](std::string_view Name, std::size_t Count) -> std::expected<std::vector<float>, AssetDiagnostic> {
					if (!Node.contains(Name)) return std::vector<float>{};
					if (!Node[Name].is_array() || Node[Name].size() != Count)
						return std::unexpected(Error("MalformedSkeleton", "glTF node transform component has invalid length"));
					std::vector<float> Values;
					Values.reserve(Count);
					for (const auto &Value : Node[Name]) {
						if (!Value.is_number()) return std::unexpected(Error("MalformedSkeleton", "glTF node transform component is not numeric"));
						const auto Component = Value.get<float>();
						if (!std::isfinite(Component)) return std::unexpected(Error("MalformedSkeleton", "glTF node transform component is non-finite"));
						Values.push_back(Component);
					}
					return Values;
				};
				if (Node.contains("matrix")) {
					if (Node.contains("translation") || Node.contains("rotation") || Node.contains("scale"))
						return std::unexpected(Error("MalformedSkeleton", "glTF node cannot mix matrix and TRS transforms"));
					auto Values = ReadArray("matrix", 16);
					if (!Values) return std::unexpected(Values.error());
					glm::mat4 Source(1.0f);
					for (glm::length_t Column = 0; Column < 4; ++Column)
						for (glm::length_t Row = 0; Row < 4; ++Row) Source[Column][Row] = (*Values)[Column * 4 + Row];
					const auto Converted = Handedness * Source * Handedness;
					if (std::abs(Converted[0][3]) > 1.0e-6f || std::abs(Converted[1][3]) > 1.0e-6f ||
						std::abs(Converted[2][3]) > 1.0e-6f || std::abs(Converted[3][3] - 1.0f) > 1.0e-6f)
						return std::unexpected(Error("UnsupportedGltfFeature", "Joint matrices with shear or perspective are unsupported"));
					Result.Translation = glm::vec3(Converted[3]);
					glm::vec3 BasisX(Converted[0]), BasisY(Converted[1]), BasisZ(Converted[2]);
					Result.Scale = {glm::length(BasisX), glm::length(BasisY), glm::length(BasisZ)};
					if (Result.Scale.x < 1.0e-8f || Result.Scale.y < 1.0e-8f || Result.Scale.z < 1.0e-8f)
						return std::unexpected(Error("MalformedSkeleton", "glTF joint matrix is singular"));
					BasisX /= Result.Scale.x; BasisY /= Result.Scale.y; BasisZ /= Result.Scale.z;
					if (std::abs(glm::dot(BasisX, BasisY)) > 1.0e-5f ||
						std::abs(glm::dot(BasisX, BasisZ)) > 1.0e-5f || std::abs(glm::dot(BasisY, BasisZ)) > 1.0e-5f)
						return std::unexpected(Error("UnsupportedGltfFeature", "Joint matrices with shear or perspective are unsupported"));
					if (glm::dot(glm::cross(BasisX, BasisY), BasisZ) < 0.0f) {
						Result.Scale.x = -Result.Scale.x;
						BasisX = -BasisX;
					}
					const glm::mat3 RotationMatrix(BasisX, BasisY, BasisZ);
					if (std::abs(glm::determinant(RotationMatrix) - 1.0f) > 1.0e-4f)
						return std::unexpected(Error("MalformedSkeleton", "glTF joint matrix has an invalid rotation basis"));
					Result.Rotation = glm::quat_cast(RotationMatrix);
				} else {
					auto Translation = ReadArray("translation", 3), Rotation = ReadArray("rotation", 4), Scale = ReadArray("scale", 3);
					if (!Translation) return std::unexpected(Translation.error());
					if (!Rotation) return std::unexpected(Rotation.error());
					if (!Scale) return std::unexpected(Scale.error());
					if (!Translation->empty()) Result.Translation = {(*Translation)[0], (*Translation)[1], -(*Translation)[2]};
					if (!Rotation->empty()) Result.Rotation = {(*Rotation)[3], -(*Rotation)[0], -(*Rotation)[1], (*Rotation)[2]};
					if (!Scale->empty()) Result.Scale = {(*Scale)[0], (*Scale)[1], (*Scale)[2]};
				}
				if (glm::length(Result.Rotation) < 1.0e-6f || std::abs(Result.Scale.x) < 1.0e-8f ||
					std::abs(Result.Scale.y) < 1.0e-8f || std::abs(Result.Scale.z) < 1.0e-8f)
					return std::unexpected(Error("MalformedSkeleton", "glTF joint transform is singular"));
				Result.Rotation = glm::normalize(Result.Rotation);
				return Result;
			};

			struct SkinData {
				ImportedSkeleton Skeleton;
				std::vector<std::size_t> JointNodes;
				std::vector<std::uint16_t> SourceJointToCanonical;
				std::unordered_map<std::size_t, std::uint16_t> NodeToCanonical;
			};
			std::vector<SkinData> Skins;
			const auto SkinCount = Root.contains("skins") ? Root["skins"].size() : 0;
			Skins.reserve(SkinCount);
			for (std::size_t SkinIndex = 0; SkinIndex < SkinCount; ++SkinIndex) {
				const auto &Skin = Root["skins"][SkinIndex];
				if (!Skin.is_object() || !Skin.contains("joints") || !Skin["joints"].is_array() || Skin["joints"].empty() ||
					Skin["joints"].size() > AssetLimits::MaximumSkeletonBones || NodeCount == 0)
					return std::unexpected(Error("MalformedSkeleton", "glTF skin joints are missing or exceed the bone limit"));
				SkinData Data;
				std::unordered_map<std::size_t, std::size_t> SourceJointOrder;
				for (std::size_t SourceIndex = 0; SourceIndex < Skin["joints"].size(); ++SourceIndex) {
					auto Node = Unsigned(Skin["joints"][SourceIndex]);
					if (!Node || *Node >= NodeCount || !SourceJointOrder.emplace(*Node, SourceIndex).second)
						return std::unexpected(Error("MalformedSkeleton", "glTF skin contains an invalid or duplicate joint"));
					Data.JointNodes.push_back(*Node);
				}
				for (const auto Node : Data.JointNodes) {
					const auto Parent = NodeParents[Node];
					if (Parent >= 0 && !SourceJointOrder.contains(static_cast<std::size_t>(Parent)))
						return std::unexpected(Error("UnsupportedGltfFeature", "A glTF joint parent must also be a joint in Foundation 1"));
				}
				std::vector<std::size_t> OrderedNodes;
				std::function<void(std::size_t)> AppendSubtree = [&](std::size_t Node) {
					OrderedNodes.push_back(Node);
					std::vector<std::size_t> Children;
					for (const auto Candidate : Data.JointNodes)
						if (NodeParents[Candidate] == static_cast<std::int32_t>(Node)) Children.push_back(Candidate);
					std::ranges::sort(Children);
					for (const auto Child : Children) AppendSubtree(Child);
				};
				std::vector<std::size_t> Roots;
				for (const auto Node : Data.JointNodes) if (NodeParents[Node] < 0) Roots.push_back(Node);
				std::ranges::sort(Roots);
				for (const auto RootNode : Roots) AppendSubtree(RootNode);
				if (OrderedNodes.size() != Data.JointNodes.size())
					return std::unexpected(Error("MalformedSkeleton", "glTF skin hierarchy could not be ordered"));
				for (std::size_t Canonical = 0; Canonical < OrderedNodes.size(); ++Canonical)
					Data.NodeToCanonical.emplace(OrderedNodes[Canonical], static_cast<std::uint16_t>(Canonical));
				Data.SourceJointToCanonical.resize(Data.JointNodes.size());
				for (std::size_t SourceIndex = 0; SourceIndex < Data.JointNodes.size(); ++SourceIndex)
					Data.SourceJointToCanonical[SourceIndex] = Data.NodeToCanonical.at(Data.JointNodes[SourceIndex]);

				std::vector<glm::mat4> InverseBinds(Data.JointNodes.size(), glm::mat4(1.0f));
				if (Skin.contains("inverseBindMatrices")) {
					auto Accessor = Unsigned(Skin["inverseBindMatrices"]);
					if (!Accessor) return std::unexpected(Error("InvalidAccessor", "glTF inverse bind accessor is invalid"));
					auto Values = ReadAccessor(Root, *Buffers, *Accessor, "MAT4", 5126);
					if (!Values) return std::unexpected(Values.error());
					if (Values->size() != Data.JointNodes.size() * 16)
						return std::unexpected(Error("MalformedSkeleton", "glTF inverse bind matrix count differs from joints"));
					for (std::size_t SourceIndex = 0; SourceIndex < Data.JointNodes.size(); ++SourceIndex) {
						glm::mat4 Matrix(1.0f);
						for (glm::length_t Column = 0; Column < 4; ++Column)
							for (glm::length_t Row = 0; Row < 4; ++Row)
								Matrix[Column][Row] = static_cast<float>((*Values)[SourceIndex * 16 + Column * 4 + Row]);
						Matrix = Handedness * Matrix * Handedness;
						if (std::abs(glm::determinant(Matrix)) < 1.0e-12f)
							return std::unexpected(Error("MalformedSkeleton", "glTF inverse bind matrix is singular"));
						InverseBinds[SourceIndex] = Matrix;
					}
				}

				auto Joints = std::make_shared<std::vector<ImportedSkeletonJoint>>();
				Joints->reserve(OrderedNodes.size());
				std::unordered_set<std::string> Paths;
				std::vector<std::uint8_t> CompatibilityBytes;
				auto AppendCompatibilityU32 = [&](std::uint32_t Value) {
					for (std::size_t Byte = 0; Byte < 4; ++Byte)
						CompatibilityBytes.push_back(static_cast<std::uint8_t>(Value >> (Byte * 8)));
				};
				for (std::size_t Canonical = 0; Canonical < OrderedNodes.size(); ++Canonical) {
					const auto NodeIndex = OrderedNodes[Canonical];
					const auto &Node = Root["nodes"][NodeIndex];
					if (!Node.contains("name") || !Node["name"].is_string())
						return std::unexpected(Error("MalformedSkeleton", "Every glTF joint requires a stable name"));
					const auto Name = Node["name"].get<std::string>();
					if (Name.empty() || Name.size() > AssetLimits::MaximumJointPathBytes ||
						Name.find('/') != std::string::npos || Name.find('\\') != std::string::npos)
						return std::unexpected(Error("MalformedSkeleton", "glTF joint name is empty, oversized, or contains a path separator"));
					const auto ParentNode = NodeParents[NodeIndex];
					const auto Parent = ParentNode < 0 ? -1 : static_cast<std::int32_t>(Data.NodeToCanonical.at(ParentNode));
					const auto Path = Parent < 0 ? Name : (*Joints)[Parent].Path + "/" + Name;
					if (Path.size() > AssetLimits::MaximumJointPathBytes || !Paths.insert(Path).second)
						return std::unexpected(Error("MalformedSkeleton", "glTF joint paths are duplicate or oversized"));
					auto Transform = ReadNodeTransform(NodeIndex);
					if (!Transform) return std::unexpected(Transform.error());
					const auto SourceIndex = SourceJointOrder.at(NodeIndex);
					Joints->push_back({Path, Parent, Transform->Translation, Transform->Rotation, Transform->Scale,
						InverseBinds[SourceIndex]});
					CompatibilityBytes.insert(CompatibilityBytes.end(), Path.begin(), Path.end());
					CompatibilityBytes.push_back(0);
					AppendCompatibilityU32(std::bit_cast<std::uint32_t>(Parent));
					for (const auto Component : {Transform->Translation.x, Transform->Translation.y, Transform->Translation.z,
						Transform->Rotation.x, Transform->Rotation.y, Transform->Rotation.z, Transform->Rotation.w,
						Transform->Scale.x, Transform->Scale.y, Transform->Scale.z})
						AppendCompatibilityU32(std::bit_cast<std::uint32_t>(Component));
					for (glm::length_t Column = 0; Column < 4; ++Column)
						for (glm::length_t Row = 0; Row < 4; ++Row)
							AppendCompatibilityU32(std::bit_cast<std::uint32_t>(InverseBinds[SourceIndex][Column][Row]));
				}
				Data.Skeleton = {AssetContentId::Hash(CompatibilityBytes),
					std::shared_ptr<const std::vector<ImportedSkeletonJoint>>(std::move(Joints))};
				Skins.push_back(std::move(Data));
			}

			std::vector<std::optional<std::size_t>> MeshSkins(Root["meshes"].size());
			if (Root.contains("nodes")) for (const auto &Node : Root["nodes"]) {
				if (Node.contains("skin") && !Node.contains("mesh"))
					return std::unexpected(Error("MalformedSkeleton", "glTF node with a skin must also reference a mesh"));
				if (!Node.contains("mesh")) continue;
				auto MeshIndex = Unsigned(Node["mesh"]);
				if (!MeshIndex || *MeshIndex >= Root["meshes"].size())
					return std::unexpected(Error("MalformedGltf", "glTF node mesh index is invalid"));
				if (!Node.contains("skin")) continue;
				auto SkinIndex = Unsigned(Node["skin"]);
				if (!SkinIndex || *SkinIndex >= Skins.size())
					return std::unexpected(Error("MalformedSkeleton", "glTF node skin index is invalid"));
				if (MeshSkins[*MeshIndex] && *MeshSkins[*MeshIndex] != *SkinIndex)
					return std::unexpected(Error("UnsupportedGltfFeature", "One glTF mesh cannot bind to multiple distinct skins"));
				MeshSkins[*MeshIndex] = *SkinIndex;
			}

			std::vector<std::string> MeshKeys(Root["meshes"].size());
			std::unordered_map<std::string, std::size_t> DuplicateMeshKeys;
			for (std::size_t MeshIndex = 0; MeshIndex < Root["meshes"].size(); ++MeshIndex) {
				const auto &Mesh = Root["meshes"][MeshIndex];
				if (!Mesh.is_object() || !Mesh.contains("primitives") || !Mesh["primitives"].is_array() || Mesh["primitives"].empty())
					return std::unexpected(Error("MalformedGltf", "glTF mesh has no primitives"));
				std::string Signature;
				for (const auto &Primitive : Mesh["primitives"]) {
					if (!Primitive.is_object() || !Primitive.contains("attributes") || !Primitive["attributes"].is_object())
						return std::unexpected(Error("MalformedGltf", "glTF primitive attributes are missing"));
					for (const auto &[Semantic, Accessor] : Primitive["attributes"].items())
						Signature += Semantic + ":" + Accessor.dump() + ";";
					Signature += "i:" + (Primitive.contains("indices") ? Primitive["indices"].dump() : std::string("none")) + ";";
				}
				auto Base = KeyHash("mesh/", Signature);
				const auto Rank = DuplicateMeshKeys[Base]++;
				MeshKeys[MeshIndex] = Rank == 0 ? Base : Base + "/" + std::to_string(Rank);
			}

			struct AnimationData {
				std::string LogicalKey;
				std::string Name;
				ImportedAnimation Animation;
				std::string SkeletonMeshKey;
			};
			std::vector<AnimationData> Animations;
			std::size_t SourceAnimationChannels = 0;
			std::size_t SourceAnimationKeys = 0;
			std::unordered_map<std::string, std::size_t> DuplicateAnimationKeys;
			if (Root.contains("animations")) {
				Animations.reserve(Root["animations"].size());
				for (std::size_t AnimationIndex = 0; AnimationIndex < Root["animations"].size(); ++AnimationIndex) {
					const auto &SourceAnimation = Root["animations"][AnimationIndex];
					if (!SourceAnimation.is_object() || !SourceAnimation.contains("channels") ||
						!SourceAnimation["channels"].is_array() || SourceAnimation["channels"].empty() ||
						!SourceAnimation.contains("samplers") || !SourceAnimation["samplers"].is_array() ||
						SourceAnimation["samplers"].empty())
						return std::unexpected(Error("MalformedAnimation", "glTF animation channels or samplers are missing"));
					if (SourceAnimation.contains("extensions") && !SourceAnimation["extensions"].empty())
						return std::unexpected(Error("UnsupportedGltfFeature", "glTF animation extensions are unsupported"));
					if (SourceAnimation["channels"].size() > AssetLimits::MaximumGltfAnimationChannels -
						std::min(SourceAnimationChannels, AssetLimits::MaximumGltfAnimationChannels))
						return std::unexpected(Error("AnimationLimit", "glTF animation channel count exceeds its limit"));
					SourceAnimationChannels += SourceAnimation["channels"].size();

					std::vector<std::size_t> TargetNodes;
					TargetNodes.reserve(SourceAnimation["channels"].size());
					for (const auto &Channel : SourceAnimation["channels"]) {
						if (!Channel.is_object() || !Channel.contains("sampler") || !Channel.contains("target") ||
							!Channel["target"].is_object() || !Channel["target"].contains("node") ||
							!Channel["target"].contains("path") || !Channel["target"]["path"].is_string() ||
							(Channel.contains("extensions") && !Channel["extensions"].empty()) ||
							(Channel["target"].contains("extensions") && !Channel["target"]["extensions"].empty()))
							return std::unexpected(Error("MalformedAnimation", "glTF animation channel metadata is invalid"));
						auto TargetNode = Unsigned(Channel["target"]["node"]);
						if (!TargetNode || *TargetNode >= NodeCount)
							return std::unexpected(Error("MalformedAnimation", "glTF animation targets an invalid node"));
						const auto &Path = Channel["target"]["path"].get_ref<const std::string &>();
						if (Path != "translation" && Path != "rotation" && Path != "scale")
							return std::unexpected(Error("UnsupportedGltfFeature", "Only translation, rotation, and scale animation channels are supported"));
						TargetNodes.push_back(*TargetNode);
					}

					std::optional<std::size_t> CompatibleSkin;
					for (std::size_t SkinIndex = 0; SkinIndex < Skins.size(); ++SkinIndex) {
						const bool ContainsAll = std::ranges::all_of(TargetNodes, [&](const auto Node) {
							return Skins[SkinIndex].NodeToCanonical.contains(Node);
						});
						if (!ContainsAll) continue;
						if (CompatibleSkin && Skins[*CompatibleSkin].Skeleton.CompatibilityId !=
							Skins[SkinIndex].Skeleton.CompatibilityId)
							return std::unexpected(Error("IncompatibleSkeleton", "glTF animation channels ambiguously match distinct skeletons"));
						if (!CompatibleSkin) CompatibleSkin = SkinIndex;
					}
					if (!CompatibleSkin)
						return std::unexpected(Error("IncompatibleSkeleton", "glTF animation channels do not target one imported skeleton"));

					std::optional<std::size_t> SkeletonMesh;
					for (std::size_t MeshIndex = 0; MeshIndex < MeshSkins.size(); ++MeshIndex)
						if (MeshSkins[MeshIndex] && Skins[*MeshSkins[MeshIndex]].Skeleton.CompatibilityId ==
							Skins[*CompatibleSkin].Skeleton.CompatibilityId) {
							SkeletonMesh = MeshIndex;
							break;
						}
					if (!SkeletonMesh)
						return std::unexpected(Error("IncompatibleSkeleton", "glTF animation has no skinned mesh dependency"));

					std::vector<std::optional<ImportedAnimationTrack>> TrackSlots(
						Skins[*CompatibleSkin].Skeleton.Joints->size());
					float Duration = 0.0f;
					for (const auto &Channel : SourceAnimation["channels"]) {
						auto SamplerIndex = Unsigned(Channel["sampler"]);
						if (!SamplerIndex || *SamplerIndex >= SourceAnimation["samplers"].size())
							return std::unexpected(Error("MalformedAnimation", "glTF animation sampler index is invalid"));
						const auto &Sampler = SourceAnimation["samplers"][*SamplerIndex];
						if (!Sampler.is_object() || !Sampler.contains("input") || !Sampler.contains("output") ||
							(Sampler.contains("extensions") && !Sampler["extensions"].empty()))
							return std::unexpected(Error("MalformedAnimation", "glTF animation sampler metadata is invalid"));
						auto InputAccessor = Unsigned(Sampler["input"]), OutputAccessor = Unsigned(Sampler["output"]);
						if (!InputAccessor || !OutputAccessor)
							return std::unexpected(Error("MalformedAnimation", "glTF animation accessor index is invalid"));
						const auto InterpolationName = Sampler.value("interpolation", std::string("LINEAR"));
						AssetAnimationInterpolation Interpolation;
						if (InterpolationName == "LINEAR") Interpolation = AssetAnimationInterpolation::Linear;
						else if (InterpolationName == "STEP") Interpolation = AssetAnimationInterpolation::Step;
						else if (InterpolationName == "CUBICSPLINE")
							return std::unexpected(Error("UnsupportedInterpolation", "glTF CUBICSPLINE animation is deferred in Foundation 1"));
						else return std::unexpected(Error("MalformedAnimation", "glTF animation interpolation is invalid"));
						auto Times = ReadAccessor(Root, *Buffers, *InputAccessor, "SCALAR", 5126);
						if (!Times) return std::unexpected(Times.error());
						if (Times->empty() || Times->size() > AssetLimits::MaximumAnimationKeyframesPerTrack ||
							Times->size() > AssetLimits::MaximumAnimationKeyframes -
							std::min(SourceAnimationKeys, AssetLimits::MaximumAnimationKeyframes))
							return std::unexpected(Error("AnimationLimit", "glTF animation key count exceeds its limit"));
						SourceAnimationKeys += Times->size();
						float PreviousTime = -1.0f;
						for (const auto TimeValue : *Times) {
							const auto Time = static_cast<float>(TimeValue);
							if (Time < 0.0f || Time <= PreviousTime || Time > AssetLimits::MaximumAnimationDurationSeconds)
								return std::unexpected(Error("MalformedAnimation", "glTF animation times must be finite, increasing, and bounded"));
							PreviousTime = Time;
							Duration = std::max(Duration, Time);
						}

						const auto TargetNode = *Unsigned(Channel["target"]["node"]);
						const auto JointIndex = Skins[*CompatibleSkin].NodeToCanonical.at(TargetNode);
						auto &TrackSlot = TrackSlots[JointIndex];
						if (!TrackSlot) TrackSlot = ImportedAnimationTrack{
							.JointPath = (*Skins[*CompatibleSkin].Skeleton.Joints)[JointIndex].Path};
						auto &Track = *TrackSlot;
						const auto &Path = Channel["target"]["path"].get_ref<const std::string &>();
						if (Path == "rotation") {
							if (Track.RotationKeys)
								return std::unexpected(Error("MalformedAnimation", "glTF animation duplicates a joint rotation channel"));
							auto Values = ReadAccessor(Root, *Buffers, *OutputAccessor, "VEC4", 5126);
							if (!Values) return std::unexpected(Values.error());
							if (Values->size() != Times->size() * 4)
								return std::unexpected(Error("MalformedAnimation", "glTF animation rotation key count differs from input"));
							auto Keys = std::make_shared<std::vector<ImportedAnimationRotationKey>>();
							Keys->reserve(Times->size());
							for (std::size_t KeyIndex = 0; KeyIndex < Times->size(); ++KeyIndex) {
								glm::quat Rotation(static_cast<float>((*Values)[KeyIndex * 4 + 3]),
									-static_cast<float>((*Values)[KeyIndex * 4]),
									-static_cast<float>((*Values)[KeyIndex * 4 + 1]),
									static_cast<float>((*Values)[KeyIndex * 4 + 2]));
								if (glm::length(Rotation) < 1.0e-6f)
									return std::unexpected(Error("MalformedAnimation", "glTF animation contains a near-zero quaternion"));
								Keys->push_back({static_cast<float>((*Times)[KeyIndex]), glm::normalize(Rotation)});
							}
							Track.RotationInterpolation = Interpolation;
							Track.RotationKeys = std::move(Keys);
						} else {
							auto Values = ReadAccessor(Root, *Buffers, *OutputAccessor, "VEC3", 5126);
							if (!Values) return std::unexpected(Values.error());
							if (Values->size() != Times->size() * 3)
								return std::unexpected(Error("MalformedAnimation", "glTF animation vector key count differs from input"));
							auto Keys = std::make_shared<std::vector<ImportedAnimationVectorKey>>();
							Keys->reserve(Times->size());
							for (std::size_t KeyIndex = 0; KeyIndex < Times->size(); ++KeyIndex) {
								glm::vec3 Value(static_cast<float>((*Values)[KeyIndex * 3]),
									static_cast<float>((*Values)[KeyIndex * 3 + 1]),
									static_cast<float>((*Values)[KeyIndex * 3 + 2]));
								if (Path == "translation") Value.z = -Value.z;
								Keys->push_back({static_cast<float>((*Times)[KeyIndex]), Value});
							}
							if (Path == "translation") {
								if (Track.TranslationKeys)
									return std::unexpected(Error("MalformedAnimation", "glTF animation duplicates a joint translation channel"));
								Track.TranslationInterpolation = Interpolation;
								Track.TranslationKeys = std::move(Keys);
							} else {
								if (Track.ScaleKeys)
									return std::unexpected(Error("MalformedAnimation", "glTF animation duplicates a joint scale channel"));
								Track.ScaleInterpolation = Interpolation;
								Track.ScaleKeys = std::move(Keys);
							}
						}
					}
					if (Duration <= 0.0f)
						return std::unexpected(Error("MalformedAnimation", "glTF animation duration must be greater than zero"));
					auto Tracks = std::make_shared<std::vector<ImportedAnimationTrack>>();
					for (auto &Track : TrackSlots) if (Track) Tracks->push_back(std::move(*Track));
					if (Tracks->empty() || Tracks->size() > AssetLimits::MaximumAnimationTracks)
						return std::unexpected(Error("AnimationLimit", "glTF animation track count is invalid or oversized"));
					const auto Name = BoundedName(SourceAnimation, "Animation " + std::to_string(AnimationIndex));
					std::string Identity = SourceAnimation.contains("name") && SourceAnimation["name"].is_string()
						? SourceAnimation["name"].get<std::string>() : "clip/" + std::to_string(AnimationIndex);
					for (const auto &Track : *Tracks) Identity += ";" + Track.JointPath;
					auto Base = KeyHash("animation/", Identity);
					const auto Rank = DuplicateAnimationKeys[Base]++;
					Animations.push_back({Rank == 0 ? Base : Base + "/" + std::to_string(Rank), Name,
						ImportedAnimation{Duration, Skins[*CompatibleSkin].Skeleton.CompatibilityId, std::nullopt,
							std::shared_ptr<const std::vector<ImportedAnimationTrack>>(std::move(Tracks))},
						MeshKeys[*SkeletonMesh]});
				}
			}

			const auto MaterialCount = Root.contains("materials") ? Root["materials"].size() : 0;
			std::vector<std::vector<std::string>> MaterialUses(MaterialCount);
			std::size_t PrimitiveTotal = 0;
			for (std::size_t MeshIndex = 0; MeshIndex < Root["meshes"].size(); ++MeshIndex) {
				const auto &Primitives = Root["meshes"][MeshIndex]["primitives"];
				PrimitiveTotal += Primitives.size();
				if (PrimitiveTotal > AssetLimits::MaximumGltfPrimitives)
					return std::unexpected(Error("GltfLimit", "glTF primitive count exceeds its limit"));
				for (std::size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives.size(); ++PrimitiveIndex)
					if (Primitives[PrimitiveIndex].contains("material")) {
						auto MaterialIndex = Unsigned(Primitives[PrimitiveIndex]["material"]);
						if (!MaterialIndex || *MaterialIndex >= MaterialCount)
							return std::unexpected(Error("InvalidMaterial", "glTF primitive material index is invalid"));
						MaterialUses[*MaterialIndex].push_back(MeshKeys[MeshIndex] + "/primitive/" + std::to_string(PrimitiveIndex));
					}
			}
			std::vector<std::string> MaterialKeys(MaterialCount);
			std::unordered_map<std::string, std::size_t> DuplicateMaterialKeys;
			for (std::size_t Index = 0; Index < MaterialCount; ++Index) {
				std::ranges::sort(MaterialUses[Index]);
				const auto Identity = MaterialUses[Index].empty() ? std::string("orphan/") + std::to_string(Index) : MaterialUses[Index].front();
				auto Base = KeyHash("material/", Identity);
				const auto Rank = DuplicateMaterialKeys[Base]++;
				MaterialKeys[Index] = Rank == 0 ? Base : Base + "/" + std::to_string(Rank);
			}

			const auto ImageCount = Root.contains("images") ? Root["images"].size() : 0;
			std::vector<std::vector<std::string>> ImageUses(ImageCount);
			auto AddTextureUse = [&](const Json &TextureInfo, std::string Use) -> std::expected<void, AssetDiagnostic> {
				if (!TextureInfo.is_object() || !TextureInfo.contains("index") ||
					(TextureInfo.contains("texCoord") && TextureInfo["texCoord"] != 0) ||
					(TextureInfo.contains("extensions") && !TextureInfo["extensions"].empty()))
					return std::unexpected(Error("UnsupportedGltfFeature", "Only TEXCOORD_0 material textures are supported"));
				auto TextureIndex = Unsigned(TextureInfo["index"]);
				if (!TextureIndex || *TextureIndex >= (Root.contains("textures") ? Root["textures"].size() : 0))
					return std::unexpected(Error("InvalidTexture", "glTF material texture index is invalid"));
				auto ImageIndex = TextureImageIndex(Root, *TextureIndex);
				if (!ImageIndex || *ImageIndex >= ImageCount)
					return std::unexpected(Error("InvalidTexture", "glTF texture source image is invalid"));
				ImageUses[*ImageIndex].push_back(std::move(Use));
				return {};
			};
			for (std::size_t Index = 0; Index < MaterialCount; ++Index) {
				const auto &Material = Root["materials"][Index];
				if (!Material.is_object()) return std::unexpected(Error("MalformedMaterial", "glTF material must be an object"));
				if (Material.contains("extensions") && !Material["extensions"].empty())
					return std::unexpected(Error("UnsupportedMaterialFeature", "glTF material extensions are not represented in Foundation 2A"));
				if (Material.contains("pbrMetallicRoughness") && Material["pbrMetallicRoughness"].is_object()) {
					const auto &Pbr = Material["pbrMetallicRoughness"];
					if (Pbr.contains("baseColorTexture")) {
						auto Result = AddTextureUse(Pbr["baseColorTexture"], MaterialKeys[Index] + "/base-color");
						if (!Result) return std::unexpected(Result.error());
					}
				}
				if (Material.contains("normalTexture")) {
					auto Result = AddTextureUse(Material["normalTexture"], MaterialKeys[Index] + "/normal");
					if (!Result) return std::unexpected(Result.error());
				}
			}
			std::vector<std::string> ImageKeys(ImageCount);
			std::unordered_map<std::string, std::size_t> DuplicateImageKeys;
			for (std::size_t Index = 0; Index < ImageCount; ++Index) {
				const auto &Image = Root["images"][Index];
				if (!Image.is_object()) return std::unexpected(Error("MalformedImage", "glTF image must be an object"));
				if (Image.contains("uri") == Image.contains("bufferView"))
					return std::unexpected(Error("MalformedImage", "glTF image must contain exactly one of URI or bufferView"));
				std::string Identity;
				if (Image.contains("uri") && Image["uri"].is_string() && !Image["uri"].get_ref<const std::string &>().starts_with("data:"))
					Identity = "uri/" + Image["uri"].get<std::string>();
				else {
					std::ranges::sort(ImageUses[Index]);
					Identity = ImageUses[Index].empty() ? std::string("embedded/") + std::to_string(Index) : ImageUses[Index].front();
				}
				auto Base = KeyHash("image/", Identity);
				const auto Rank = DuplicateImageKeys[Base]++;
				ImageKeys[Index] = Rank == 0 ? Base : Base + "/" + std::to_string(Rank);
			}

			AssetImportGraphCandidate Graph;
			Graph.PrimaryLogicalKey = MeshKeys.front();
			Graph.Nodes.reserve(ImageCount + MaterialCount + Root["meshes"].size() + Animations.size());
			for (std::size_t Index = 0; Index < ImageCount; ++Index) {
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "glTF import was cancelled"));
				const auto &Image = Root["images"][Index];
				std::vector<std::uint8_t> Bytes;
				std::string Uri;
				const auto Mime = Image.value("mimeType", std::string{});
				if (Image.contains("uri") && Image["uri"].is_string()) {
					Uri = Image["uri"].get<std::string>();
					auto Resolved = ResolveUriBytes(Uri, Context);
					if (!Resolved) return std::unexpected(Resolved.error());
					Bytes = std::move(*Resolved);
				} else if (Image.contains("bufferView")) {
					auto View = Unsigned(Image["bufferView"]);
					if (!View) return std::unexpected(Error("InvalidBufferView", "glTF image bufferView is invalid"));
					auto Resolved = BufferViewBytes(Root, *Buffers, *View);
					if (!Resolved) return std::unexpected(Resolved.error());
					Bytes = std::move(*Resolved);
				} else return std::unexpected(Error("MalformedImage", "glTF image has neither URI nor bufferView"));
				const auto Type = ImageType(Mime, Uri);
				if (Type.empty()) return std::unexpected(Error("UnsupportedImageFormat", "glTF image must be PNG, JPEG, or BMP"));
				auto Decoded = DecodeImage(Bytes, Type);
				if (!Decoded) return std::unexpected(Decoded.error());
				auto Artifact = EncodeAssetArtifact(ImportedAsset(*Decoded), AssetKind::Image);
				if (!Artifact) return std::unexpected(Artifact.error());
				Graph.Nodes.push_back({ImageKeys[Index], AssetKind::Image,
					BoundedName(Image, "Image " + std::to_string(Index)), *Decoded, *Artifact, AssetContentId::Hash(**Artifact), {}});
			}

			for (std::size_t Index = 0; Index < MaterialCount; ++Index) {
				const auto &SourceMaterial = Root["materials"][Index];
				ImportedMaterial Material;
				std::vector<AssetImportBinding> Bindings;
				const auto &Pbr = SourceMaterial.contains("pbrMetallicRoughness") ? SourceMaterial["pbrMetallicRoughness"] : Json::object();
				if (!Pbr.is_object()) return std::unexpected(Error("MalformedMaterial", "glTF pbrMetallicRoughness must be an object"));
				if (Pbr.contains("baseColorFactor")) {
					if (!Pbr["baseColorFactor"].is_array() || Pbr["baseColorFactor"].size() != 4)
						return std::unexpected(Error("MalformedMaterial", "glTF baseColorFactor must contain four values"));
					std::array<float *, 4> Factors{&Material.BaseColorFactor.x, &Material.BaseColorFactor.y,
						&Material.BaseColorFactor.z, &Material.BaseColorFactor.w};
					for (std::size_t Component = 0; Component < 4; ++Component) {
						if (!Pbr["baseColorFactor"][Component].is_number()) return std::unexpected(Error("MalformedMaterial", "glTF base color factor is invalid"));
						*Factors[Component] = Pbr["baseColorFactor"][Component].get<float>();
						if (!std::isfinite(*Factors[Component]) || *Factors[Component] < 0.0f || *Factors[Component] > 1.0f)
							return std::unexpected(Error("MalformedMaterial", "glTF base color factor is out of range"));
					}
				}
			auto ReadUnitFactor = [&](std::string_view Name, float Default) -> std::expected<float, AssetDiagnostic> {
					if (!Pbr.contains(Name)) return Default;
					if (!Pbr[Name].is_number()) return std::unexpected(Error("MalformedMaterial", "glTF material factor must be numeric"));
					const auto Value = Pbr[Name].get<float>();
					if (!std::isfinite(Value) || Value < 0.0f || Value > 1.0f)
						return std::unexpected(Error("MalformedMaterial", "glTF material factor is out of range"));
					return Value;
				};
				auto Metallic = ReadUnitFactor("metallicFactor", 1.0f), Roughness = ReadUnitFactor("roughnessFactor", 1.0f);
				if (!Metallic) return std::unexpected(Metallic.error());
				if (!Roughness) return std::unexpected(Roughness.error());
				Material.MetallicFactor = *Metallic;
				Material.RoughnessFactor = *Roughness;
				if (Pbr.contains("metallicRoughnessTexture") || SourceMaterial.contains("occlusionTexture") ||
					SourceMaterial.contains("emissiveTexture") || SourceMaterial.contains("emissiveFactor"))
					return std::unexpected(Error("UnsupportedMaterialFeature", "Metallic-roughness textures, occlusion, and emissive inputs are not represented in Foundation 2A"));
				auto BindTexture = [&](const Json &Info, AssetImportBindingKind Kind) -> std::expected<void, AssetDiagnostic> {
					auto TextureIndex = Unsigned(Info["index"]);
					auto ImageIndex = TextureIndex ? TextureImageIndex(Root, *TextureIndex) : std::nullopt;
					if (!ImageIndex || *ImageIndex >= ImageKeys.size()) return std::unexpected(Error("InvalidTexture", "glTF texture source is invalid"));
					Bindings.push_back({Kind, 0, ImageKeys[*ImageIndex]});
					return {};
				};
				if (Pbr.contains("baseColorTexture")) {
					auto Bound = BindTexture(Pbr["baseColorTexture"], AssetImportBindingKind::MaterialBaseColorTexture);
					if (!Bound) return std::unexpected(Bound.error());
				}
				if (SourceMaterial.contains("normalTexture")) {
					if (!SourceMaterial["normalTexture"].is_object())
						return std::unexpected(Error("MalformedMaterial", "glTF normalTexture must be an object"));
					if (SourceMaterial["normalTexture"].contains("scale")) {
						if (!SourceMaterial["normalTexture"]["scale"].is_number() ||
							!std::isfinite(SourceMaterial["normalTexture"]["scale"].get<float>()) ||
							SourceMaterial["normalTexture"]["scale"].get<float>() != 1.0f)
							return std::unexpected(Error("UnsupportedMaterialFeature", "Scaled normal textures are not represented in Foundation 2A"));
					}
					auto Bound = BindTexture(SourceMaterial["normalTexture"], AssetImportBindingKind::MaterialNormalTexture);
					if (!Bound) return std::unexpected(Bound.error());
				}
				if (SourceMaterial.contains("alphaMode") && !SourceMaterial["alphaMode"].is_string())
					return std::unexpected(Error("MalformedMaterial", "glTF alphaMode must be a string"));
				const auto AlphaMode = SourceMaterial.value("alphaMode", std::string("OPAQUE"));
				if (AlphaMode == "OPAQUE") Material.AlphaMode = AssetMaterialAlphaMode::Opaque;
				else if (AlphaMode == "MASK") Material.AlphaMode = AssetMaterialAlphaMode::Mask;
				else if (AlphaMode == "BLEND") Material.AlphaMode = AssetMaterialAlphaMode::Blend;
				else return std::unexpected(Error("MalformedMaterial", "glTF alphaMode is invalid"));
				if (SourceMaterial.contains("alphaCutoff") && !SourceMaterial["alphaCutoff"].is_number())
					return std::unexpected(Error("MalformedMaterial", "glTF alphaCutoff must be numeric"));
				if (SourceMaterial.contains("doubleSided") && !SourceMaterial["doubleSided"].is_boolean())
					return std::unexpected(Error("MalformedMaterial", "glTF doubleSided must be boolean"));
				Material.AlphaCutoff = SourceMaterial.value("alphaCutoff", 0.5f);
				Material.DoubleSided = SourceMaterial.value("doubleSided", false);
				if (!std::isfinite(Material.AlphaCutoff) || Material.AlphaCutoff < 0.0f || Material.AlphaCutoff > 1.0f)
					return std::unexpected(Error("MalformedMaterial", "glTF alphaCutoff is outside the canonical range"));
				Graph.Nodes.push_back({MaterialKeys[Index], AssetKind::Material,
					BoundedName(SourceMaterial, "Material " + std::to_string(Index)), Material, {}, {}, std::move(Bindings)});
			}

			for (std::size_t MeshIndex = 0; MeshIndex < Root["meshes"].size(); ++MeshIndex) {
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "glTF import was cancelled"));
				const auto &SourceMesh = Root["meshes"][MeshIndex];
				if (SourceMesh.contains("weights"))
					return std::unexpected(Error("UnsupportedGltfFeature", "Morph target weights are not supported by Foundation 2A"));
				auto Vertices = std::make_shared<std::vector<RenderVertex>>();
				auto Indices = std::make_shared<std::vector<std::uint32_t>>();
				auto Primitives = std::make_shared<std::vector<ImportedMeshPrimitive>>();
				auto SkinInfluences = MeshSkins[MeshIndex]
					? std::make_shared<std::vector<ImportedSkinInfluence>>() : nullptr;
				const auto *Skin = MeshSkins[MeshIndex] ? &Skins[*MeshSkins[MeshIndex]] : nullptr;
				std::vector<AssetImportBinding> Bindings;
				for (std::size_t PrimitiveIndex = 0; PrimitiveIndex < SourceMesh["primitives"].size(); ++PrimitiveIndex) {
					const auto &Primitive = SourceMesh["primitives"][PrimitiveIndex];
					const auto Mode = Primitive.contains("mode") ? Unsigned(Primitive["mode"]) : std::optional<std::size_t>(4);
					if (!Mode) return std::unexpected(Error("MalformedTopology", "glTF primitive mode is invalid"));
					if (*Mode != 4 || Primitive.contains("targets") ||
						(Primitive.contains("extensions") && !Primitive["extensions"].empty()))
						return std::unexpected(Error("UnsupportedGltfFeature", "Only uncompressed static triangle primitives are supported"));
					const auto &Attributes = Primitive["attributes"];
					for (const auto &[Semantic, Accessor] : Attributes.items()) {
						(void)Accessor;
						if (Semantic != "POSITION" && Semantic != "NORMAL" && Semantic != "TANGENT" &&
							Semantic != "TEXCOORD_0" && Semantic != "JOINTS_0" && Semantic != "WEIGHTS_0")
							return std::unexpected(Error("UnsupportedGltfFeature", "glTF vertex semantic " + Semantic + " is not represented in Foundation 2A"));
					}
					if (Attributes.contains("JOINTS_0") != Attributes.contains("WEIGHTS_0") ||
						(Skin && !Attributes.contains("JOINTS_0")) || (!Skin && Attributes.contains("JOINTS_0")))
						return std::unexpected(Error("MalformedSkinWeights", "glTF skinned primitives require matching JOINTS_0, WEIGHTS_0, and skin binding"));
					if (!Attributes.contains("POSITION")) return std::unexpected(Error("MalformedTopology", "glTF primitive has no POSITION accessor"));
					auto PositionAccessor = Unsigned(Attributes["POSITION"]);
					if (!PositionAccessor) return std::unexpected(Error("InvalidAccessor", "glTF POSITION accessor is invalid"));
					auto Positions = ReadAccessor(Root, *Buffers, *PositionAccessor, "VEC3", 5126);
					if (!Positions) return std::unexpected(Positions.error());
					const auto VertexCount = Positions->size() / 3;
					if (VertexCount == 0 || VertexCount > AssetLimits::MaximumMeshVertices - Vertices->size())
						return std::unexpected(Error("MeshLimit", "glTF canonical vertex count exceeds its limit"));
					std::optional<std::vector<double>> Normals, Tangents, TextureCoordinates, JointValues, WeightValues;
					if (Attributes.contains("NORMAL")) {
						auto Accessor = Unsigned(Attributes["NORMAL"]);
						if (!Accessor) return std::unexpected(Error("InvalidAccessor", "glTF NORMAL accessor is invalid"));
						auto Values = ReadAccessor(Root, *Buffers, *Accessor, "VEC3", 5126);
						if (!Values) return std::unexpected(Values.error());
						Normals = std::move(*Values);
						if (Normals->size() / 3 != VertexCount) return std::unexpected(Error("MalformedTopology", "glTF NORMAL count differs from POSITION"));
					}
					if (Attributes.contains("TANGENT")) {
						auto Accessor = Unsigned(Attributes["TANGENT"]);
						if (!Accessor) return std::unexpected(Error("InvalidAccessor", "glTF TANGENT accessor is invalid"));
						auto Values = ReadAccessor(Root, *Buffers, *Accessor, "VEC4", 5126);
						if (!Values) return std::unexpected(Values.error());
						Tangents = std::move(*Values);
						if (Tangents->size() / 4 != VertexCount) return std::unexpected(Error("MalformedTopology", "glTF TANGENT count differs from POSITION"));
						for (std::size_t Index = 3; Index < Tangents->size(); Index += 4)
							if ((*Tangents)[Index] != -1.0 && (*Tangents)[Index] != 1.0)
								return std::unexpected(Error("MalformedTangentSpace", "glTF tangent handedness must be -1 or 1"));
					}
					if (Attributes.contains("TEXCOORD_0")) {
						auto Accessor = Unsigned(Attributes["TEXCOORD_0"]);
						if (!Accessor) return std::unexpected(Error("InvalidAccessor", "glTF TEXCOORD_0 accessor is invalid"));
						const auto ComponentType = AccessorComponentType(Root, *Accessor);
						if (!ComponentType || (*ComponentType != 5126 &&
							((*ComponentType != 5121 && *ComponentType != 5123) || !AccessorIsNormalized(Root, *Accessor))))
							return std::unexpected(Error("UnsupportedAccessor", "glTF TEXCOORD_0 must be float or normalized unsigned byte/short"));
						auto Values = ReadAccessor(Root, *Buffers, *Accessor, "VEC2");
						if (!Values) return std::unexpected(Values.error());
						TextureCoordinates = std::move(*Values);
						if (TextureCoordinates->size() / 2 != VertexCount) return std::unexpected(Error("MalformedTopology", "glTF TEXCOORD_0 count differs from POSITION"));
					}
					if (Skin) {
						auto JointAccessor = Unsigned(Attributes["JOINTS_0"]), WeightAccessor = Unsigned(Attributes["WEIGHTS_0"]);
						if (!JointAccessor || !WeightAccessor)
							return std::unexpected(Error("InvalidAccessor", "glTF skin influence accessor is invalid"));
						const auto JointComponent = AccessorComponentType(Root, *JointAccessor);
						const auto WeightComponent = AccessorComponentType(Root, *WeightAccessor);
						if (!JointComponent || (*JointComponent != 5121 && *JointComponent != 5123) ||
							AccessorIsNormalized(Root, *JointAccessor))
							return std::unexpected(Error("UnsupportedAccessor", "glTF JOINTS_0 must use non-normalized unsigned byte or short"));
						if (!WeightComponent || (*WeightComponent != 5126 &&
							((*WeightComponent != 5121 && *WeightComponent != 5123) || !AccessorIsNormalized(Root, *WeightAccessor))))
							return std::unexpected(Error("UnsupportedAccessor", "glTF WEIGHTS_0 must use float or normalized unsigned byte or short"));
						auto ReadJoints = ReadAccessor(Root, *Buffers, *JointAccessor, "VEC4");
						auto ReadWeights = ReadAccessor(Root, *Buffers, *WeightAccessor, "VEC4");
						if (!ReadJoints) return std::unexpected(ReadJoints.error());
						if (!ReadWeights) return std::unexpected(ReadWeights.error());
						JointValues = std::move(*ReadJoints);
						WeightValues = std::move(*ReadWeights);
						if (JointValues->size() / 4 != VertexCount || WeightValues->size() / 4 != VertexCount)
							return std::unexpected(Error("MalformedSkinWeights", "glTF skin influence count differs from POSITION"));
					}
					const auto VertexBase = static_cast<std::uint32_t>(Vertices->size());
					for (std::size_t VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
						RenderVertex Vertex;
						Vertex.Position = {static_cast<float>((*Positions)[VertexIndex * 3]),
							static_cast<float>((*Positions)[VertexIndex * 3 + 1]), -static_cast<float>((*Positions)[VertexIndex * 3 + 2])};
						if (Normals) Vertex.Normal = {static_cast<float>((*Normals)[VertexIndex * 3]),
							static_cast<float>((*Normals)[VertexIndex * 3 + 1]), -static_cast<float>((*Normals)[VertexIndex * 3 + 2])};
						else Vertex.Normal = {};
						if (Tangents) Vertex.Tangent = {static_cast<float>((*Tangents)[VertexIndex * 4]),
							static_cast<float>((*Tangents)[VertexIndex * 4 + 1]), -static_cast<float>((*Tangents)[VertexIndex * 4 + 2]),
							-static_cast<float>((*Tangents)[VertexIndex * 4 + 3])};
						if (TextureCoordinates) Vertex.TextureCoordinate = {static_cast<float>((*TextureCoordinates)[VertexIndex * 2]),
							static_cast<float>((*TextureCoordinates)[VertexIndex * 2 + 1])};
						Vertices->push_back(Vertex);
						if (Skin) {
							ImportedSkinInfluence Influence;
							float WeightSum = 0.0f;
							for (std::size_t InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex) {
								const auto Joint = (*JointValues)[VertexIndex * 4 + InfluenceIndex];
								const auto Weight = (*WeightValues)[VertexIndex * 4 + InfluenceIndex];
								if (!std::isfinite(Joint) || Joint < 0.0 || std::floor(Joint) != Joint ||
									Joint >= Skin->SourceJointToCanonical.size() || !std::isfinite(Weight) || Weight < 0.0)
									return std::unexpected(Error("MalformedSkinWeights", "glTF skin influence contains an invalid joint or weight"));
								Influence.Joints[InfluenceIndex] = Skin->SourceJointToCanonical[static_cast<std::size_t>(Joint)];
								Influence.Weights[InfluenceIndex] = static_cast<float>(Weight);
								WeightSum += static_cast<float>(Weight);
							}
							if (!std::isfinite(WeightSum) || WeightSum <= 1.0e-8f)
								return std::unexpected(Error("MalformedSkinWeights", "glTF skin weights have a zero or non-finite sum"));
							Influence.Weights /= WeightSum;
							SkinInfluences->push_back(Influence);
						}
					}
					std::vector<std::uint32_t> LocalIndices;
					if (Primitive.contains("indices")) {
						auto Accessor = Unsigned(Primitive["indices"]);
						if (!Accessor) return std::unexpected(Error("InvalidAccessor", "glTF index accessor is invalid"));
						const auto ComponentType = AccessorComponentType(Root, *Accessor);
						if (!ComponentType || (*ComponentType != 5121 && *ComponentType != 5123 && *ComponentType != 5125) ||
							AccessorIsNormalized(Root, *Accessor))
							return std::unexpected(Error("UnsupportedAccessor", "glTF indices must use non-normalized unsigned byte, short, or int components"));
						auto Values = ReadAccessor(Root, *Buffers, *Accessor, "SCALAR");
						if (!Values) return std::unexpected(Values.error());
						LocalIndices.reserve(Values->size());
						for (const auto Value : *Values) {
							if (Value < 0 || Value > std::numeric_limits<std::uint32_t>::max() || std::floor(Value) != Value)
								return std::unexpected(Error("InvalidMeshIndex", "glTF index accessor contains an invalid value"));
							LocalIndices.push_back(static_cast<std::uint32_t>(Value));
						}
					} else {
						LocalIndices.resize(VertexCount);
						for (std::size_t Index = 0; Index < VertexCount; ++Index) LocalIndices[Index] = static_cast<std::uint32_t>(Index);
					}
					if (LocalIndices.empty() || LocalIndices.size() % 3 != 0 ||
						LocalIndices.size() > AssetLimits::MaximumMeshIndices - Indices->size())
						return std::unexpected(Error("MalformedTopology", "glTF triangle index count is invalid or oversized"));
					bool RequiresTextureCoordinates = false;
					bool RequiresTangentSpace = false;
					if (Primitive.contains("material")) {
						auto MaterialIndex = Unsigned(Primitive["material"]);
						if (!MaterialIndex || *MaterialIndex >= MaterialCount)
							return std::unexpected(Error("InvalidMaterial", "glTF primitive material index is invalid"));
						const auto &SourceMaterial = Root["materials"][*MaterialIndex];
						const auto &Pbr = SourceMaterial.contains("pbrMetallicRoughness") ?
							SourceMaterial["pbrMetallicRoughness"] : Json::object();
						RequiresTextureCoordinates = Pbr.contains("baseColorTexture") || SourceMaterial.contains("normalTexture");
						RequiresTangentSpace = SourceMaterial.contains("normalTexture");
					}
					if (RequiresTextureCoordinates && !TextureCoordinates)
						return std::unexpected(Error("MissingTextureCoordinates", "A textured glTF primitive has no TEXCOORD_0 accessor"));
					std::vector<glm::vec3> TangentSums;
					std::vector<glm::vec3> BitangentSums;
					if (!Tangents && TextureCoordinates) {
						TangentSums.resize(VertexCount);
						BitangentSums.resize(VertexCount);
					}
					const auto FirstIndex = static_cast<std::uint32_t>(Indices->size());
					for (std::size_t Index = 0; Index < LocalIndices.size(); Index += 3) {
						const auto A = LocalIndices[Index], B = LocalIndices[Index + 1], C = LocalIndices[Index + 2];
						if (A >= VertexCount || B >= VertexCount || C >= VertexCount)
							return std::unexpected(Error("InvalidMeshIndex", "glTF primitive index is out of range"));
						Indices->push_back(VertexBase + A); Indices->push_back(VertexBase + C); Indices->push_back(VertexBase + B);
						const auto Cross = glm::cross((*Vertices)[VertexBase + C].Position - (*Vertices)[VertexBase + A].Position,
							(*Vertices)[VertexBase + B].Position - (*Vertices)[VertexBase + A].Position);
						if (!std::isfinite(Cross.x) || !std::isfinite(Cross.y) || !std::isfinite(Cross.z) || glm::length(Cross) < 1e-8f)
							return std::unexpected(Error("MalformedTopology", "glTF contains a degenerate triangle"));
						if (!Normals) for (const auto Local : {A, B, C}) (*Vertices)[VertexBase + Local].Normal += Cross;
						if (!TangentSums.empty()) {
							const auto &VertexA = (*Vertices)[VertexBase + A];
							const auto &VertexB = (*Vertices)[VertexBase + B];
							const auto &VertexC = (*Vertices)[VertexBase + C];
							const auto EdgeOne = VertexC.Position - VertexA.Position;
							const auto EdgeTwo = VertexB.Position - VertexA.Position;
							const auto UvOne = VertexC.TextureCoordinate - VertexA.TextureCoordinate;
							const auto UvTwo = VertexB.TextureCoordinate - VertexA.TextureCoordinate;
							const auto Determinant = UvOne.x * UvTwo.y - UvOne.y * UvTwo.x;
							if (std::abs(Determinant) <= 1e-12f) {
								if (RequiresTangentSpace)
									return std::unexpected(Error("MalformedTangentSpace", "A normal-mapped glTF primitive has degenerate texture coordinates"));
							} else {
								const auto Reciprocal = 1.0f / Determinant;
								const auto GeneratedTangent = (EdgeOne * UvTwo.y - EdgeTwo * UvOne.y) * Reciprocal;
								const auto GeneratedBitangent = (EdgeTwo * UvOne.x - EdgeOne * UvTwo.x) * Reciprocal;
								for (const auto Local : {A, B, C}) {
									TangentSums[Local] += GeneratedTangent;
									BitangentSums[Local] += GeneratedBitangent;
								}
							}
						}
					}
					for (std::size_t Index = 0; Index < VertexCount; ++Index) {
						auto &Vertex = (*Vertices)[VertexBase + Index];
						if (glm::length(Vertex.Normal) < 1e-8f) return std::unexpected(Error("MalformedTopology", "glTF generated a zero normal"));
						Vertex.Normal = glm::normalize(Vertex.Normal);
						if (Tangents) {
							const glm::vec3 Tangent(Vertex.Tangent);
							if (glm::length(Tangent) < 1e-8f) return std::unexpected(Error("MalformedTopology", "glTF contains a zero tangent"));
							const auto Orthonormal = glm::normalize(Tangent - Vertex.Normal * glm::dot(Vertex.Normal, Tangent));
							Vertex.Tangent = {Orthonormal, Vertex.Tangent.w < 0.0f ? -1.0f : 1.0f};
						} else if (!TangentSums.empty() && glm::length(TangentSums[Index]) >= 1e-8f) {
							const auto Orthonormal = glm::normalize(TangentSums[Index] -
								Vertex.Normal * glm::dot(Vertex.Normal, TangentSums[Index]));
							if (!std::isfinite(Orthonormal.x) || !std::isfinite(Orthonormal.y) || !std::isfinite(Orthonormal.z))
								return std::unexpected(Error("MalformedTangentSpace", "glTF tangent generation produced non-finite data"));
							const auto Handedness = glm::dot(glm::cross(Vertex.Normal, Orthonormal), BitangentSums[Index]) < 0.0f ? -1.0f : 1.0f;
							Vertex.Tangent = {Orthonormal, Handedness};
						} else {
							if (RequiresTangentSpace)
								return std::unexpected(Error("MalformedTangentSpace", "A normal-mapped glTF primitive cannot produce durable tangents"));
							const auto Axis = std::abs(Vertex.Normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
							Vertex.Tangent = {glm::normalize(glm::cross(Axis, Vertex.Normal)), 1.0f};
						}
					}
					Primitives->push_back({FirstIndex, static_cast<std::uint32_t>(LocalIndices.size()), std::nullopt});
					if (Primitive.contains("material")) {
						auto MaterialIndex = Unsigned(Primitive["material"]);
						Bindings.push_back({AssetImportBindingKind::MeshPrimitiveMaterial, PrimitiveIndex, MaterialKeys[*MaterialIndex]});
					}
				}
				RenderBounds Bounds{Vertices->front().Position, Vertices->front().Position};
				for (const auto &Vertex : *Vertices) {
					Bounds.Minimum = glm::min(Bounds.Minimum, Vertex.Position);
					Bounds.Maximum = glm::max(Bounds.Maximum, Vertex.Position);
				}
				ImportedMesh Mesh{Vertices, Indices, Bounds, static_cast<std::uint32_t>(Primitives->size()), Primitives,
					Skin ? std::make_shared<const ImportedSkeleton>(Skin->Skeleton) : nullptr,
					Skin ? std::shared_ptr<const std::vector<ImportedSkinInfluence>>(std::move(SkinInfluences)) : nullptr};
				Graph.Nodes.push_back({MeshKeys[MeshIndex], AssetKind::Mesh,
					BoundedName(SourceMesh, "Mesh " + std::to_string(MeshIndex)), Mesh, {}, {}, std::move(Bindings)});
			}
			for (auto &Animation : Animations) {
				std::vector<AssetImportBinding> Bindings{{AssetImportBindingKind::AnimationSkeletonMesh, 0,
					Animation.SkeletonMeshKey}};
				Graph.Nodes.push_back({std::move(Animation.LogicalKey), AssetKind::Animation,
					std::move(Animation.Name), std::move(Animation.Animation), {}, {}, std::move(Bindings)});
			}
			if (Graph.Nodes.size() > AssetLimits::MaximumGeneratedAssets)
				return std::unexpected(Error("GeneratedAssetLimit", "glTF generated asset count exceeds its limit"));
			return Graph;
		}

		class GltfImporter final : public IAssetImporter {
		  public:
			explicit GltfImporter(AssetKind Kind) : Kind(Kind) {}

			AssetKind GetKind() const override { return Kind; }
			bool SupportsExtension(std::string_view Extension) const override { return Extension == ".gltf" || Extension == ".glb"; }
			bool IsCompound() const override { return true; }

			std::expected<std::vector<std::string>, AssetDiagnostic> DiscoverExternalResources(
				std::span<const std::uint8_t> Source,
				const AssetImportContext &Context
			) const override {
				auto Document = ParseGltf(Source, Context.SourceExtension);
				if (!Document) return std::unexpected(Document.error());
				std::set<std::string> Unique;
				for (const auto Name : {"buffers", "images"}) if (Document->Root.contains(Name)) {
					if (!Document->Root[Name].is_array()) return std::unexpected(Error("MalformedGltf", "glTF resource table must be an array"));
					for (const auto &Entry : Document->Root[Name]) if (Entry.is_object() && Entry.contains("uri")) {
						if (!Entry["uri"].is_string()) return std::unexpected(Error("MalformedGltf", "glTF resource URI must be a string"));
						const auto &Uri = Entry["uri"].get_ref<const std::string &>();
						auto Validated = ValidateExternalUri(Uri);
						if (!Validated) return std::unexpected(Validated.error());
						if (!Uri.starts_with("data:")) Unique.insert(Uri);
					}
				}
				if (Unique.size() > AssetLimits::MaximumExternalResources)
					return std::unexpected(Error("ExternalResourceLimit", "glTF external resource count exceeds its limit"));
				return std::vector<std::string>(Unique.begin(), Unique.end());
			}

			std::expected<AssetImportCandidate, AssetDiagnostic> Import(
				std::span<const std::uint8_t>,
				const AssetImportContext &
			) const override {
				return std::unexpected(Error("CompoundImportRequired", "glTF sources require graph import"));
			}

			std::expected<AssetImportGraphCandidate, AssetDiagnostic> ImportGraph(
				std::span<const std::uint8_t> Source,
				const AssetImportContext &Context
			) const override {
				if (Cancelled(Context)) return std::unexpected(Error("Cancelled", "glTF import was cancelled or exceeded its deadline"));
				auto Document = ParseGltf(Source, Context.SourceExtension);
				if (!Document) return std::unexpected(Document.error());
				auto Graph = ImportDocument(*Document, Context);
				if (!Graph) return std::unexpected(Graph.error());
				if (Kind == AssetKind::Animation) {
					auto Primary = std::ranges::find_if(Graph->Nodes, [](const auto &Node) {
						return Node.Kind == AssetKind::Animation;
					});
					if (Primary == Graph->Nodes.end())
						return std::unexpected(Error("MissingAnimation", "glTF source contains no animation clips"));
					Graph->PrimaryLogicalKey = Primary->LogicalKey;
				}
				return Graph;
			}

		  private:
			AssetKind Kind;
		};
	}

	std::unique_ptr<IAssetImporter> CreateGltfImporter(AssetKind Kind) { return std::make_unique<GltfImporter>(Kind); }
}
