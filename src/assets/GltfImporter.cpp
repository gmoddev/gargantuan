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
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <glm/geometric.hpp>

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
			if ((Parsed->contains("skins") && !(*Parsed)["skins"].empty()) ||
				(Parsed->contains("animations") && !(*Parsed)["animations"].empty()))
				return std::unexpected(Error("UnsupportedGltfFeature", "Skins and animations are outside Asset Foundation 2A"));
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
			if ((Root.contains("skins") && (!Root["skins"].is_array() || !Root["skins"].empty())) ||
				(Root.contains("animations") && (!Root["animations"].is_array() || !Root["animations"].empty())))
				return std::unexpected(Error("UnsupportedGltfFeature", "Skins and animations are not supported by Foundation 2A"));
			if (Root.contains("textures")) for (const auto &Texture : Root["textures"]) {
				if (!Texture.is_object() || !Texture.contains("source"))
					return std::unexpected(Error("MalformedGltf", "glTF texture must contain an image source"));
				if (Texture.contains("sampler") || (Texture.contains("extensions") && !Texture["extensions"].empty()))
					return std::unexpected(Error("UnsupportedGltfFeature", "Explicit texture samplers and texture extensions are not represented in Foundation 2A"));
			}
			auto Buffers = LoadBuffers(Document, Context);
			if (!Buffers) return std::unexpected(Buffers.error());

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
			Graph.Nodes.reserve(ImageCount + MaterialCount + Root["meshes"].size());
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
						if (Semantic != "POSITION" && Semantic != "NORMAL" && Semantic != "TANGENT" && Semantic != "TEXCOORD_0")
							return std::unexpected(Error("UnsupportedGltfFeature", "glTF vertex semantic " + Semantic + " is not represented in Foundation 2A"));
					}
					if (!Attributes.contains("POSITION")) return std::unexpected(Error("MalformedTopology", "glTF primitive has no POSITION accessor"));
					auto PositionAccessor = Unsigned(Attributes["POSITION"]);
					if (!PositionAccessor) return std::unexpected(Error("InvalidAccessor", "glTF POSITION accessor is invalid"));
					auto Positions = ReadAccessor(Root, *Buffers, *PositionAccessor, "VEC3", 5126);
					if (!Positions) return std::unexpected(Positions.error());
					const auto VertexCount = Positions->size() / 3;
					if (VertexCount == 0 || VertexCount > AssetLimits::MaximumMeshVertices - Vertices->size())
						return std::unexpected(Error("MeshLimit", "glTF canonical vertex count exceeds its limit"));
					std::optional<std::vector<double>> Normals, Tangents, TextureCoordinates;
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
				ImportedMesh Mesh{Vertices, Indices, Bounds, static_cast<std::uint32_t>(Primitives->size()), Primitives};
				Graph.Nodes.push_back({MeshKeys[MeshIndex], AssetKind::Mesh,
					BoundedName(SourceMesh, "Mesh " + std::to_string(MeshIndex)), Mesh, {}, {}, std::move(Bindings)});
			}
			if (Graph.Nodes.size() > AssetLimits::MaximumGeneratedAssets)
				return std::unexpected(Error("GeneratedAssetLimit", "glTF generated asset count exceeds its limit"));
			return Graph;
		}

		class GltfImporter final : public IAssetImporter {
		  public:
			AssetKind GetKind() const override { return AssetKind::Mesh; }
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
				return ImportDocument(*Document, Context);
			}
		};
	}

	std::unique_ptr<IAssetImporter> CreateGltfImporter() { return std::make_unique<GltfImporter>(); }
}
