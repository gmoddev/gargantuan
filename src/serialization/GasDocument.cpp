// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/serialization/GasDocument.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <limits>
#include <type_traits>

namespace gargantuan::Gas {
	namespace {
		constexpr std::array<std::uint8_t, 8> Magic{'G', 'A', 'S', 0x1a, '\r', '\n', 0x02, '\n'};
		constexpr std::size_t HeaderBytes = 20;
		constexpr std::size_t DirectoryEntryBytes = 12;
		constexpr std::size_t DigestBytes = 32;
		constexpr std::size_t IdentifierBytes = 256;
		constexpr std::size_t MaximumAttributes = 64;
		constexpr std::size_t MaximumTags = 64;
		constexpr std::size_t MaximumGroups = 256;
		constexpr std::size_t MinimumNodeBytes = 48;
		constexpr auto DataModelClass = SchemaId::FromNativeName("Engine", "DataModel");
		static_assert(sizeof(int) == sizeof(std::int32_t));
		static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
		static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

		enum class ValueTag : std::uint8_t {
			Null = 0, Bool = 1, Int = 2, Double = 3, Float = 4, String = 5, Vector2 = 6,
			Vector3 = 7, Color3 = 8, UDim = 9, UDim2 = 10, CFrame = 11, Enum = 12,
			SchemaEnum = 13, Reference = 14, Binary = 15,
		};

		[[noreturn]] void Fail(SerializationErrorCode Code, const char *Message) {
			throw SerializationError{Code, "GAS", Message, {}};
		}

		void Require(bool Condition, const char *Message) {
			if (!Condition) Fail(SerializationErrorCode::InvalidValue, Message);
		}

		void CheckLimit(bool Condition, const char *Message) {
			if (!Condition) Fail(SerializationErrorCode::LimitExceeded, Message);
		}

		bool ValidText(std::string_view Text) {
			return Text.find('\0') == std::string_view::npos && IsValidProtocolUtf8(Text);
		}

		template <std::floating_point Float>
		using FloatBits = std::conditional_t<sizeof(Float) == 4, std::uint32_t, std::uint64_t>;

		class Writer {
		  public:
			Binary Bytes;

			template <std::unsigned_integral Integer>
			void Put(Integer Value) {
				CheckLimit(sizeof(Integer) <= MaximumDocumentBytes - Bytes.size(), "Document is too large");
				for (std::size_t Index = 0; Index < sizeof(Integer); ++Index)
					Bytes.push_back(static_cast<std::uint8_t>(Value >> (8 * Index)));
			}

			void Put(ValueTag Tag) { Put(static_cast<std::uint8_t>(Tag)); }
			void Put(SchemaId Id) { Put(Id.High); Put(Id.Low); }
			void PutInt(int Value) { Put(std::bit_cast<std::uint32_t>(Value)); }

			template <std::floating_point Float>
			void PutFloat(Float Value) {
				Require(std::isfinite(Value), "Non-finite floating-point value");
				Put(std::bit_cast<FloatBits<Float>>(Value == 0 ? Float{0} : Value));
			}

			void Raw(std::span<const std::uint8_t> Value) {
				CheckLimit(Value.size() <= MaximumDocumentBytes - Bytes.size(), "Document is too large");
				Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			}

			void Count(std::size_t Value, std::size_t Maximum) {
				CheckLimit(Value <= Maximum, "Collection exceeds its limit");
				Put(static_cast<std::uint32_t>(Value));
			}

			void Text(std::string_view Value, std::size_t Maximum = MaximumStringBytes, bool Nonempty = false) {
				Count(Value.size(), Maximum);
				Require((!Nonempty || !Value.empty()) && ValidText(Value), "Invalid UTF-8 string");
				Raw({reinterpret_cast<const std::uint8_t *>(Value.data()), Value.size()});
			}

			void Patch32(std::size_t Offset, std::size_t Value) {
				for (std::size_t Index = 0; Index < 4; ++Index)
					Bytes[Offset + Index] = static_cast<std::uint8_t>(Value >> (8 * Index));
			}
		};

		class Reader {
		  public:
			explicit Reader(std::span<const std::uint8_t> Input) : Bytes(Input) {}

			std::span<const std::uint8_t> Take(std::size_t Length) {
				if (Length > Bytes.size())
					Fail(SerializationErrorCode::TruncatedInput, "Section ends before its declared contents");
				const auto Result = Bytes.first(Length);
				Bytes = Bytes.subspan(Length);
				return Result;
			}

			template <std::unsigned_integral Integer>
			Integer Get() {
				std::uint64_t Value = 0;
				const auto Raw = Take(sizeof(Integer));
				for (std::size_t Index = 0; Index < sizeof(Integer); ++Index)
					Value |= static_cast<std::uint64_t>(Raw[Index]) << (8 * Index);
				return static_cast<Integer>(Value);
			}

			std::uint32_t U32() { return Get<std::uint32_t>(); }
			std::uint8_t U8() { return Get<std::uint8_t>(); }
			int Int() { return std::bit_cast<int>(U32()); }
			SchemaId Id() { return {Get<std::uint64_t>(), Get<std::uint64_t>()}; }

			template <std::floating_point Float = float>
			Float Number() {
				const auto Value = std::bit_cast<Float>(Get<FloatBits<Float>>());
				Require(std::isfinite(Value) && !(Value == 0 && std::signbit(Value)),
					"Invalid or noncanonical floating-point value");
				return Value;
			}

			std::uint32_t Count(std::size_t Maximum, std::size_t MinimumEntryBytes = 1) {
				const auto Value = U32();
				CheckLimit(Value <= Maximum, "Collection exceeds its limit");
				if (Value > Bytes.size() / MinimumEntryBytes)
					Fail(SerializationErrorCode::TruncatedInput, "Collection cannot fit in its section");
				return Value;
			}

			std::string Text(std::size_t Maximum = MaximumStringBytes, bool Nonempty = false) {
				const auto Raw = Take(Count(Maximum));
				const std::string_view Text(reinterpret_cast<const char *>(Raw.data()), Raw.size());
				Require((!Nonempty || !Text.empty()) && ValidText(Text), "Invalid UTF-8 string");
				return std::string(Text);
			}

			void Finish() const { Require(Bytes.empty(), "Unexpected trailing section data"); }

		  private:
			std::span<const std::uint8_t> Bytes;
		};

		struct ValueBudget {
			std::size_t Nodes;
			std::size_t Values = 0;
			std::size_t References = 0;

			void Add() { CheckLimit(++Values <= MaximumValues, "Document exceeds its value limit"); }
			void Reference(std::uint32_t Target, bool AllowReference) {
				CheckLimit(++References <= MaximumReferences, "Document exceeds its reference limit");
				Require(AllowReference && Target <= Nodes, "Invalid document-local reference");
			}
		};

		void WriteWireValue(Writer &Out, const WireValue &Property) {
			std::visit([&]<class Type>(const Type &Value) {
				if constexpr (std::same_as<Type, std::monostate>) Out.Put(ValueTag::Null);
				else if constexpr (std::same_as<Type, bool>) {
					Out.Put(ValueTag::Bool); Out.Put(static_cast<std::uint8_t>(Value));
				} else if constexpr (std::same_as<Type, int>) {
					Out.Put(ValueTag::Int); Out.PutInt(Value);
				} else if constexpr (std::same_as<Type, double>) {
					Out.Put(ValueTag::Double); Out.PutFloat(Value);
				} else if constexpr (std::same_as<Type, WireFloat>) {
					Out.Put(ValueTag::Float); Out.PutFloat(Value.Value);
				} else if constexpr (std::same_as<Type, std::string>) {
					Out.Put(ValueTag::String); Out.Text(Value);
				} else if constexpr (std::same_as<Type, WireVector2>) {
					Out.Put(ValueTag::Vector2); Out.PutFloat(Value.X); Out.PutFloat(Value.Y);
				} else if constexpr (std::same_as<Type, WireVector3>) {
					Out.Put(ValueTag::Vector3); Out.PutFloat(Value.X); Out.PutFloat(Value.Y); Out.PutFloat(Value.Z);
				} else if constexpr (std::same_as<Type, WireColor3>) {
					Out.Put(ValueTag::Color3); Out.PutFloat(Value.R); Out.PutFloat(Value.G); Out.PutFloat(Value.B);
				} else if constexpr (std::same_as<Type, WireUDim>) {
					Out.Put(ValueTag::UDim); Out.PutFloat(Value.Scale); Out.PutInt(Value.Offset);
				} else if constexpr (std::same_as<Type, WireUDim2>) {
					Out.Put(ValueTag::UDim2);
					Out.PutFloat(Value.X.Scale); Out.PutInt(Value.X.Offset);
					Out.PutFloat(Value.Y.Scale); Out.PutInt(Value.Y.Offset);
				} else if constexpr (std::same_as<Type, WireCFrame>) {
					Out.Put(ValueTag::CFrame);
					for (const auto Component : Value.Components) Out.PutFloat(Component);
				} else if constexpr (std::same_as<Type, WireEnumItem>) {
					Out.Put(ValueTag::Enum);
					Out.Text(Value.EnumType, IdentifierBytes, true); Out.Text(Value.Item, IdentifierBytes, true);
				} else if constexpr (std::same_as<Type, WireSchemaEnumValue>) {
					Require(Value.EnumSchemaId.IsValid() && Value.DefinitionVersion != 0, "Invalid schema enum identity");
					Out.Put(ValueTag::SchemaEnum);
					Out.Put(Value.EnumSchemaId); Out.Put(Value.DefinitionVersion); Out.PutInt(Value.ItemValue);
				} else {
					static_assert(std::same_as<Type, WireObjectReference>);
					Fail(SerializationErrorCode::InvalidType, "Runtime object references cannot be serialized");
				}
			}, Property);
		}

		Value ReadValue(Reader &In, ValueBudget &Budget, bool Native) {
			Budget.Add();
			switch (static_cast<ValueTag>(In.U8())) {
				case ValueTag::Null: return WireValue(std::monostate{});
				case ValueTag::Bool: {
					const auto Value = In.U8();
					Require(Value <= 1, "Invalid Boolean representation");
					return WireValue(Value != 0);
				}
				case ValueTag::Int: return WireValue(In.Int());
				case ValueTag::Double: return WireValue(In.Number<double>());
				case ValueTag::Float: return WireValue(WireFloat{In.Number()});
				case ValueTag::String: return WireValue(In.Text());
				case ValueTag::Vector2: return WireValue(WireVector2{In.Number(), In.Number()});
				case ValueTag::Vector3: return WireValue(WireVector3{In.Number(), In.Number(), In.Number()});
				case ValueTag::Color3: return WireValue(WireColor3{In.Number(), In.Number(), In.Number()});
				case ValueTag::UDim: return WireValue(WireUDim{In.Number(), In.Int()});
				case ValueTag::UDim2: return WireValue(WireUDim2{{In.Number(), In.Int()}, {In.Number(), In.Int()}});
				case ValueTag::CFrame: {
					WireCFrame Frame{};
					for (auto &Component : Frame.Components) Component = In.Number();
					return WireValue(Frame);
				}
				case ValueTag::Enum: return WireValue(WireEnumItem{
					In.Text(IdentifierBytes, true), In.Text(IdentifierBytes, true)});
				case ValueTag::SchemaEnum: {
					const WireSchemaEnumValue Enum{In.Id(), In.U32(), In.Int()};
					Require(Enum.EnumSchemaId.IsValid() && Enum.DefinitionVersion != 0, "Invalid schema enum identity");
					return WireValue(Enum);
				}
				case ValueTag::Reference: {
					const auto Target = In.U32();
					Budget.Reference(Target, Native);
					return LocalReference{Target};
				}
				case ValueTag::Binary: {
					Require(Native, "Binary values are only supported in native property records");
					const auto Raw = In.Take(In.Count(MaximumBinaryBytes));
					return Binary(Raw.begin(), Raw.end());
				}
			}
			Fail(SerializationErrorCode::InvalidType, "Unknown GAS value type");
		}

		void WriteProperties(Writer &Out, const Properties &Values, ValueBudget &Budget,
			std::size_t Maximum, bool Native = false) {
			Out.Count(Values.size(), Maximum);
			for (const auto &[Name, Property] : Values) {
				Budget.Add();
				Out.Text(Name, IdentifierBytes, true);
				if (const auto *Wire = std::get_if<WireValue>(&Property)) WriteWireValue(Out, *Wire);
				else if (const auto *Reference = std::get_if<LocalReference>(&Property)) {
					Budget.Reference(Reference->Target, Native);
					Out.Put(ValueTag::Reference);
					Out.Put(Reference->Target);
				} else {
					Require(Native, "Binary values are only supported in native property records");
					const auto &Bytes = std::get<Binary>(Property);
					Out.Put(ValueTag::Binary);
					Out.Count(Bytes.size(), MaximumBinaryBytes);
					Out.Raw(Bytes);
				}
			}
		}

		Properties ReadProperties(Reader &In, ValueBudget &Budget, std::size_t Maximum, bool Native = false) {
			Properties Result;
			const auto Count = In.Count(Maximum, 6);
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				auto Name = In.Text(IdentifierBytes, true);
				Require(Result.empty() || Result.rbegin()->first < Name, "Unsorted or duplicate property");
				auto Value = ReadValue(In, Budget, Native);
				Result.emplace_hint(Result.end(), std::move(Name), std::move(Value));
			}
			return Result;
		}

		void WriteGroups(Writer &Out, const std::vector<PropertyGroup> &Groups, ValueBudget &Budget) {
			Out.Count(Groups.size(), MaximumGroups);
			std::vector<const PropertyGroup *> Ordered;
			for (const auto &Group : Groups) Ordered.push_back(&Group);
			std::ranges::sort(Ordered, {}, [](const PropertyGroup *Group) { return Group->Owner; });
			SchemaId Previous;
			for (const auto *Group : Ordered) {
				Require(Group->Owner > Previous && Group->DefinitionVersion != 0 && !Group->Values.empty(),
					"Invalid or duplicate property group");
				Previous = Group->Owner;
				Out.Put(Group->Owner); Out.Put(Group->DefinitionVersion);
				WriteProperties(Out, Group->Values, Budget, MaximumProperties);
			}
		}

		std::vector<PropertyGroup> ReadGroups(Reader &In, ValueBudget &Budget) {
			std::vector<PropertyGroup> Result;
			const auto Count = In.Count(MaximumGroups, 24);
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				const auto Owner = In.Id();
				const auto Version = In.U32();
				Require(Owner.IsValid() && Version != 0 && (Result.empty() || Result.back().Owner < Owner),
					"Invalid, unsorted or duplicate property group");
				auto Values = ReadProperties(In, Budget, MaximumProperties);
				Require(!Values.empty(), "Empty property group");
				Result.push_back({Owner, Version, std::move(Values)});
			}
			return Result;
		}

		class Hierarchy {
		  public:
			explicit Hierarchy(DocumentKind KindValue) : Kind(KindValue) {}

			void Add(const Node &NodeValue, std::uint32_t Index) {
				Require(NodeValue.Class.IsValid() && NodeValue.DefinitionVersion != 0, "Invalid class identity");
				const bool PlaceRoot = Kind == DocumentKind::Place && Index == 1;
				Require((NodeValue.Class == DataModelClass) == PlaceRoot, "DataModel is only allowed as a place root");
				if (NodeValue.Parent == 0) {
					Require(Kind == DocumentKind::Model || PlaceRoot, "Place must have exactly one root");
					Depth = 0;
				} else {
					while (Depth > 0 && Ancestors[Depth - 1] != NodeValue.Parent) --Depth;
					Require(Depth != 0, "Parent must be an active ancestor in preorder");
				}
				CheckLimit(Depth < Ancestors.size(), "Hierarchy exceeds its depth limit");
				Ancestors[Depth++] = Index;
			}

		  private:
			DocumentKind Kind;
			std::array<std::uint32_t, MaximumDepth> Ancestors{};
			std::size_t Depth = 0;
		};

		void WriteGraph(Writer &Out, const Document &Doc) {
			Require(!Doc.Nodes.empty(), "Graph requires a root");
			Out.Count(Doc.Nodes.size(), MaximumInstances);
			ValueBudget Budget{Doc.Nodes.size()};
			Hierarchy Tree(Doc.Kind);
			std::uint32_t Index = 0;
			for (const auto &NodeValue : Doc.Nodes) {
				Tree.Add(NodeValue, ++Index);
				Out.Put(NodeValue.Parent); Out.Put(NodeValue.Class); Out.Put(NodeValue.DefinitionVersion);
				Out.Text(NodeValue.Name);
				WriteProperties(Out, NodeValue.SavedProperties, Budget, MaximumProperties, true);
				WriteProperties(Out, NodeValue.Attributes, Budget, MaximumAttributes);
				WriteGroups(Out, NodeValue.Extensions, Budget);
				WriteGroups(Out, NodeValue.CustomProperties, Budget);
				Out.Count(NodeValue.Tags.size(), MaximumTags);
				std::vector<std::string_view> Tags(NodeValue.Tags.begin(), NodeValue.Tags.end());
				std::ranges::sort(Tags);
				Require(std::ranges::adjacent_find(Tags) == Tags.end(), "Duplicate tag");
				for (const auto Tag : Tags) Out.Text(Tag, IdentifierBytes, true);
			}
		}

		std::vector<Node> ReadGraph(Reader In, DocumentKind Kind) {
			const auto Count = In.Count(MaximumInstances, MinimumNodeBytes);
			Require(Count != 0, "Graph requires a root");
			ValueBudget Budget{Count};
			Hierarchy Tree(Kind);
			std::vector<Node> Nodes;
			Nodes.reserve(Count);
			for (std::uint32_t Index = 1; Index <= Count; ++Index) {
				Node Current;
				Current.Parent = In.U32();
				Current.Class = In.Id();
				Current.DefinitionVersion = In.U32();
				Tree.Add(Current, Index);
				Current.Name = In.Text();
				Current.SavedProperties = ReadProperties(In, Budget, MaximumProperties, true);
				Current.Attributes = ReadProperties(In, Budget, MaximumAttributes);
				Current.Extensions = ReadGroups(In, Budget);
				Current.CustomProperties = ReadGroups(In, Budget);
				const auto Tags = In.Count(MaximumTags, 5);
				for (std::uint32_t Tag = 0; Tag < Tags; ++Tag) {
					auto Name = In.Text(IdentifierBytes, true);
					Require(Current.Tags.empty() || Current.Tags.back() < Name, "Unsorted or duplicate tag");
					Current.Tags.push_back(std::move(Name));
				}
				Nodes.push_back(std::move(Current));
			}
			In.Finish();
			return Nodes;
		}

		void CheckDependency(const Dependency &Entry, AssetId Previous) {
			Require(Entry.Id > Previous && Entry.Content.IsValid(), "Invalid, unsorted or duplicate asset dependency");
		}

		void WriteDependencies(Writer &Out, const std::vector<Dependency> &Dependencies) {
			Out.Count(Dependencies.size(), MaximumDependencies);
			auto Ordered = Dependencies;
			std::ranges::sort(Ordered, {}, &Dependency::Id);
			AssetId Previous;
			for (const auto &Entry : Ordered) {
				CheckDependency(Entry, Previous);
				Previous = Entry.Id;
				Out.Put(Entry.Id.High); Out.Put(Entry.Id.Low); Out.Raw(Entry.Content.Bytes);
			}
		}

		std::vector<Dependency> ReadDependencies(Reader In) {
			const auto Count = In.Count(MaximumDependencies, 48);
			std::vector<Dependency> Result;
			Result.reserve(Count);
			AssetId Previous;
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				Dependency Entry;
				Entry.Id = {In.Get<std::uint64_t>(), In.Get<std::uint64_t>()};
				std::ranges::copy(In.Take(Entry.Content.Bytes.size()), Entry.Content.Bytes.begin());
				CheckDependency(Entry, Previous);
				Previous = Entry.Id;
				Result.push_back(Entry);
			}
			In.Finish();
			return Result;
		}

		void WritePlace(Writer &Out, const PlaceMetadata &Place) {
			Require(Place.Identity.IsValid(), "Invalid place identity");
			Out.Raw(Place.Identity.Bytes);
			Out.Put(static_cast<std::uint8_t>(Place.PreRunSource.has_value()));
			if (Place.PreRunSource) Out.Text(*Place.PreRunSource, MaximumPreRunBytes);
			if (!Place.Domains.empty()) Out.Put(static_cast<std::uint8_t>(Place.TerrainManifest.has_value()));
			if (Place.TerrainManifest) {
				Require(Place.TerrainManifest->IsValid(), "Invalid terrain manifest identity");
				Out.Raw(Place.TerrainManifest->Bytes);
			}
			if (!Place.Domains.empty()) {
				Out.Count(Place.Domains.size(), terrain::MaximumDomainTerrainBindings);
				for (const auto &[Node, Digest] : Place.Domains) {
					Require(Node > 0 && Node <= MaximumInstances && Digest.IsValid(), "Invalid domain terrain binding");
					Out.Put(Node);
					Out.Raw(Digest.Bytes);
				}
			}
		}

		PlaceMetadata ReadPlace(Reader In, std::uint16_t Version) {
			PlaceMetadata Place;
			std::ranges::copy(In.Take(Place.Identity.Bytes.size()), Place.Identity.Bytes.begin());
			Require(Place.Identity.IsValid(), "Invalid place identity");
			const auto Present = In.U8();
			Require(Present <= 1, "Invalid place source flag");
			if (Present != 0) Place.PreRunSource = In.Text(MaximumPreRunBytes);
			const unsigned HasTerrain = Version == DomainTerrainPlaceFormatVersion ? In.U8() : Version == TerrainPlaceFormatVersion;
			Require(HasTerrain <= 1, "Invalid default terrain flag");
			if (HasTerrain) {
				AssetContentId Digest;
				std::ranges::copy(In.Take(Digest.Bytes.size()), Digest.Bytes.begin());
				Require(Digest.IsValid(), "Invalid terrain manifest identity");
				Place.TerrainManifest = Digest;
			}
			if (Version == DomainTerrainPlaceFormatVersion) {
				const auto Count = In.Count(terrain::MaximumDomainTerrainBindings, 36);
				Require(Count > 0, "Missing domain terrain bindings");
				std::uint32_t Prior = 0;
				for (std::uint32_t I = 0; I < Count; ++I) {
					const auto Node = In.U32();
					AssetContentId Digest;
					std::ranges::copy(In.Take(Digest.Bytes.size()), Digest.Bytes.begin());
					Require(Node > Prior && Node <= MaximumInstances && Digest.IsValid(), "Invalid domain terrain binding");
					Place.Domains.emplace(Node, Digest);
					Prior = Node;
				}
			}
			In.Finish();
			return Place;
		}
	}

	SerializationResult<Binary> Encode(const Document &DocumentValue) {
		try {
			const auto Kind = DocumentValue.Kind;
			const bool Place = Kind == DocumentKind::Place;
			Require(Kind == DocumentKind::Model || Place, "Unknown document kind");
			Require(Place == DocumentValue.Place.has_value(), "Place section does not match document kind");
			const std::uint8_t Sections = Place ? 3 : 2;
			Writer Out;
			Out.Raw(Magic);
			Out.Put(GetFormatVersion(DocumentValue));
			Out.Put(static_cast<std::uint8_t>(Kind));
			Out.Put(Sections);
			Out.Put(SchemaVersion);
			Out.Put(std::uint32_t{0});
			for (std::uint32_t Section = 1; Section <= Sections; ++Section) {
				Out.Put(Section); Out.Put(std::uint32_t{0}); Out.Put(std::uint32_t{0});
			}
			const auto WriteSection = [&](std::size_t Index, const auto &Write) {
				const auto Offset = Out.Bytes.size();
				Write();
				const auto Entry = HeaderBytes + Index * DirectoryEntryBytes;
				Out.Patch32(Entry + 4, Offset);
				Out.Patch32(Entry + 8, Out.Bytes.size() - Offset);
			};
			WriteSection(0, [&] { WriteGraph(Out, DocumentValue); });
			WriteSection(1, [&] { WriteDependencies(Out, DocumentValue.Dependencies); });
			if (Place) WriteSection(2, [&] { WritePlace(Out, *DocumentValue.Place); });
			CheckLimit(Out.Bytes.size() <= MaximumDocumentBytes - DigestBytes, "Document is too large");
			Out.Patch32(16, Out.Bytes.size() + DigestBytes);
			Out.Raw(AssetContentId::Hash(Out.Bytes).Bytes);
			return std::move(Out.Bytes);
		} catch (SerializationError &Error) {
			return std::unexpected(std::move(Error));
		} catch (const std::exception &) {
			return SerializationFailure(SerializationErrorCode::InternalFailure, "Could not encode GAS document");
		}
	}

	SerializationResult<Document> Decode(std::span<const std::uint8_t> Bytes) {
		try {
			CheckLimit(Bytes.size() <= MaximumDocumentBytes, "Document is too large");
			if (Bytes.size() < HeaderBytes + DigestBytes)
				Fail(SerializationErrorCode::TruncatedInput, "Incomplete GAS document");
			const auto HashedBytes = Bytes.first(Bytes.size() - DigestBytes);
			Reader Header(HashedBytes);
			if (!std::ranges::equal(Header.Take(Magic.size()), Magic))
				Fail(SerializationErrorCode::InvalidSyntax, "Invalid GAS magic");
			const auto Version = Header.Get<std::uint16_t>();
			if (Version != FormatVersion && Version != TerrainPlaceFormatVersion && Version != DomainTerrainPlaceFormatVersion)
				Fail(SerializationErrorCode::UnsupportedVersion, "Unsupported GAS format version");
			Document Result;
			Result.Kind = static_cast<DocumentKind>(Header.U8());
			const bool Place = Result.Kind == DocumentKind::Place;
			if (Version != FormatVersion && !Place)
				Fail(SerializationErrorCode::UnsupportedVersion, "Terrain format requires a GAS place");
			Require(Result.Kind == DocumentKind::Model || Place, "Unknown document kind");
			const auto Count = Header.U8();
			if (Header.U32() != SchemaVersion)
				Fail(SerializationErrorCode::UnsupportedVersion, "Unsupported GAS schema version");
			Require(Header.U32() == Bytes.size() && Count == (Place ? 3 : 2), "Invalid document length or section count");
			std::array<std::span<const std::uint8_t>, 3> Sections;
			std::size_t Offset = HeaderBytes + Count * DirectoryEntryBytes;
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				const auto Id = Header.U32();
				const auto Start = Header.U32();
				const auto Length = Header.U32();
				Require(Id == Index + 1 && Start == Offset && Start <= HashedBytes.size() &&
					Length != 0 && Length <= HashedBytes.size() - Start, "Invalid section directory");
				Sections[Index] = HashedBytes.subspan(Start, Length);
				Offset += Length;
			}
			Require(Offset == HashedBytes.size(), "Sections do not cover the document");
			Require(std::ranges::equal(AssetContentId::Hash(HashedBytes).Bytes, Bytes.last(DigestBytes)),
				"Document integrity digest does not match");
			Result.Nodes = ReadGraph(Reader(Sections[0]), Result.Kind);
			Result.Dependencies = ReadDependencies(Reader(Sections[1]));
			if (Place) Result.Place = ReadPlace(Reader(Sections[2]), Version);
			return Result;
		} catch (SerializationError &Error) {
			return std::unexpected(std::move(Error));
		} catch (const std::exception &) {
			return SerializationFailure(SerializationErrorCode::InternalFailure, "Could not decode GAS document");
		}
	}
}
