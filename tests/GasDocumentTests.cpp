// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/serialization/GasDocument.hpp"
#include "blockgrid/RegionalGridFixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>

namespace {
	using namespace gargantuan;

	constexpr std::size_t DigestBytes = 32;
	constexpr std::size_t DirectoryStart = 20;
	constexpr std::size_t DirectoryEntryBytes = 12;
	constexpr std::size_t ModelGraphStart = DirectoryStart + 2 * DirectoryEntryBytes;
	constexpr std::size_t FirstParent = ModelGraphStart + 4;
	constexpr std::size_t FirstNameLength = FirstParent + 4 + 16 + 4;
	constexpr std::size_t FirstName = FirstNameLength + 4;
	constexpr std::size_t FirstPropertyCount = FirstName + 1;
	constexpr SchemaId DataModelClass = SchemaId::FromNativeName("Engine", "DataModel");
	constexpr SchemaId FolderClass = SchemaId::FromNativeName("Engine", "Folder");
	constexpr SchemaId ScriptClass = SchemaId::FromNativeName("Engine", "Script");

	int Failures = 0;

	void Check(bool Condition, std::string_view Message) {
		if (!Condition) {
			std::cerr << "[GAS:Tests] " << Message << '\n';
			++Failures;
		}
	}

	Gas::Document Minimal() {
		Gas::Document Model;
		Model.Nodes.push_back({.Class = {1, 2}, .DefinitionVersion = 1, .Name = "R"});
		return Model;
	}

	Gas::Document ExampleModel() {
		Gas::Document Model;
		Model.Nodes.push_back({
			.Class = FolderClass,
			.DefinitionVersion = 1,
			.Name = "Assembly",
			.SavedProperties = {
				{"Binary", Gas::Binary{0x00, 0xff, 0x1a}},
				{"Bool", WireValue{true}},
				{"CFrame", WireValue{WireCFrame{{1.5f, -2.0f, 3.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}}}},
				{"Color3", WireValue{WireColor3{0.25f, 0.5f, 0.75f}}},
				{"Double", WireValue{0.0}},
				{"Enum", WireValue{WireEnumItem{"Material", "Plastic"}}},
				{"Float", WireValue{WireFloat{0.0f}}},
				{"Int", WireValue{std::numeric_limits<int>::min()}},
				{"Missing", Gas::LocalReference{0}},
				{"Nil", WireValue{std::monostate{}}},
				{"SchemaEnum", WireValue{WireSchemaEnumValue{{3, 4}, 2, -7}}},
				{"Target", Gas::LocalReference{3}},
				{"Text", WireValue{std::string("h\xc3\xa9")}},
				{"UDim", WireValue{WireUDim{0.5f, -12}}},
				{"UDim2", WireValue{WireUDim2{{0.0f, 5}, {1.0f, -5}}}},
				{"Vector2", WireValue{WireVector2{-1.0f, 2.0f}}},
				{"Vector3", WireValue{WireVector3{1.0f, 2.0f, -3.0f}}},
			},
			.Attributes = {{"Count", WireValue{3}}},
			.Extensions = {
				{{5, 6}, 1, {{"Weight", WireValue{WireFloat{0.5f}}}}},
				{{7, 8}, 1, {{"Flag", WireValue{true}}}},
			},
			.CustomProperties = {
				{{9, 10}, 1, {{"Value", WireValue{42}}}},
				{{11, 12}, 2, {{"Value", WireValue{43}}}},
			},
			.Tags = {"Assembly", "Portable"},
		});
		Model.Nodes.push_back({.Parent = 1, .Class = ScriptClass, .DefinitionVersion = 1,
			.Name = "Logic", .SavedProperties = {{"Source", WireValue{std::string("return 42\n")}}}});
		Model.Nodes.push_back({.Class = FolderClass, .DefinitionVersion = 1, .Name = "Target"});
		const std::array<std::uint8_t, 1> One{1};
		const std::array<std::uint8_t, 1> Two{2};
		Model.Dependencies = {{{1, 2}, AssetContentId::Hash(One)}, {{3, 4}, AssetContentId::Hash(Two)}};
		return Model;
	}

	void WriteUnsigned32(Gas::Binary &Bytes, std::size_t Offset, std::uint32_t Value) {
		for (std::size_t Index = 0; Index < 4; ++Index)
			Bytes[Offset + Index] = static_cast<std::uint8_t>(Value >> (8 * Index));
	}

	void Rehash(Gas::Binary &Bytes) {
		const auto Body = std::span(Bytes).first(Bytes.size() - DigestBytes);
		const auto Digest = AssetContentId::Hash(Body);
		std::copy(Digest.Bytes.begin(), Digest.Bytes.end(), Bytes.begin() + Body.size());
	}

	void RejectPatchedUnsigned32(const Gas::Binary &Valid, std::size_t Offset,
		std::uint32_t Value, std::string_view Message) {
		auto Patched = Valid;
		WriteUnsigned32(Patched, Offset, Value);
		Rehash(Patched);
		Check(!Gas::Decode(Patched), Message);
	}

	void TestGoldenDocument() {
		const auto Model = Minimal();
		const auto Encoded = Gas::Encode(Model);
		Check(Encoded.has_value(), "Encode minimal model");
		if (!Encoded) return;
		Check(Encoded->size() == 133, "Minimal v1 model matches the golden byte length");
		if (Encoded->size() < DigestBytes) return;
		const auto Body = std::span(*Encoded).first(Encoded->size() - DigestBytes);
		const auto Digest = AssetContentId::Hash(Body);
		// The expected body digest was generated independently using Python struct and hashlib.
		Check(Digest.ToString() == "9b8d96c208673652bb1face5f229c11b12c8ac99dcd1956d7178446bbcde9e1c",
			"Minimal v1 model matches the golden body digest");
		Check(std::ranges::equal(Digest.Bytes, std::span(*Encoded).last(DigestBytes)),
			"The trailer contains the body's SHA-256");
		const auto Decoded = Gas::Decode(*Encoded);
		Check(Decoded && *Decoded == Model, "Minimal model survives a semantic round trip");
		std::cout << "[GAS:Tests] Golden model SHA-256 " << Digest.ToString() << '\n';
	}

	void TestProfilesAndCanonicalValues() {
		const auto Model = ExampleModel();
		const auto Bytes = Gas::Encode(Model);
		Check(Bytes.has_value(), "Encode multiple roots with typed values, a forward reference, and dependencies");
		if (!Bytes) return;
		const auto Decoded = Gas::Decode(*Bytes);
		Check(Decoded && *Decoded == Model, "Model round trip preserves graph order, values, and local targets");

		auto Reordered = Model;
		std::ranges::reverse(Reordered.Nodes[0].Tags);
		std::ranges::reverse(Reordered.Nodes[0].Extensions);
		std::ranges::reverse(Reordered.Nodes[0].CustomProperties);
		std::ranges::reverse(Reordered.Dependencies);
		Reordered.Nodes[0].SavedProperties["Float"] = WireValue{WireFloat{-0.0f}};
		Reordered.Nodes[0].SavedProperties["Double"] = WireValue{-0.0};
		const auto Reencoded = Gas::Encode(Reordered);
		Check(Reencoded && *Reencoded == *Bytes,
			"Unordered records and signed zero yield the same canonical bytes");
		const auto Repeated = Gas::Encode(Model);
		Check(Repeated && *Repeated == *Bytes, "Repeated export yields identical bytes");
		std::cout << "[GAS:Tests] Typed model SHA-256 " << AssetContentId::Hash(*Bytes).ToString() << '\n';

		auto Place = Model;
		Place.Kind = Gas::DocumentKind::Place;
		Place.Nodes[0].Class = DataModelClass;
		Place.Nodes[2].Parent = 1;
		Place.Place = Gas::PlaceMetadata{.Identity = {{1}}, .PreRunSource = "return nil\n"};
		const auto PlaceBytes = Gas::Encode(Place);
		Check(PlaceBytes.has_value(), "Encode place with its project identity and PreRun source");
		if (PlaceBytes) {
			const auto DecodedPlace = Gas::Decode(*PlaceBytes);
			Check(DecodedPlace && *DecodedPlace == Place, "Place round trip preserves its profile and inert PreRun source");
			std::cout << "[GAS:Tests] Place SHA-256 " << AssetContentId::Hash(*PlaceBytes).ToString() << '\n';
		}

		auto Invalid = Place;
		Invalid.Nodes[2].Parent = 0;
		Check(!Gas::Encode(Invalid), "Place rejects multiple roots");
		Invalid = Place;
		Invalid.Nodes[0].Class = FolderClass;
		Check(!Gas::Encode(Invalid), "Place rejects a non-DataModel root");
		Invalid = Place;
		Invalid.Place.reset();
		Check(!Gas::Encode(Invalid), "Place requires place metadata");
		Invalid = Model;
		Invalid.Nodes[0].Class = DataModelClass;
		Check(!Gas::Encode(Invalid), "Model rejects a DataModel");
		Invalid = Model;
		Invalid.Place = Place.Place;
		Check(!Gas::Encode(Invalid), "Model rejects place metadata");
	}

	void TestEncodeRejections() {
		auto Invalid = ExampleModel();
		Invalid.Nodes[0].SavedProperties["Target"] = WireValue{WireObjectReference{{42, 1}}};
		Check(!Gas::Encode(Invalid), "Live slot/generation references are never persisted");
		Invalid.Nodes[0].SavedProperties["Target"] = Gas::LocalReference{4};
		Check(!Gas::Encode(Invalid), "Out-of-range local reference rejects");
		Invalid.Nodes[0].SavedProperties["Target"] = WireValue{std::numeric_limits<double>::infinity()};
		Check(!Gas::Encode(Invalid), "Non-finite double rejects");
		Invalid.Nodes[0].SavedProperties["Target"] = WireValue{WireFloat{std::numeric_limits<float>::quiet_NaN()}};
		Check(!Gas::Encode(Invalid), "NaN float rejects");
		Invalid.Nodes[0].SavedProperties["Target"] = Gas::Binary(Gas::MaximumBinaryBytes + 1);
		Check(!Gas::Encode(Invalid), "Oversized binary value rejects");

		Invalid = ExampleModel();
		Invalid.Nodes[0].Name = "\xc0\x80";
		Check(!Gas::Encode(Invalid), "Overlong UTF-8 encoding rejects");
		Invalid.Nodes[0].Name.assign(Gas::MaximumStringBytes + 1, 'x');
		Check(!Gas::Encode(Invalid), "Oversized name rejects");
		Invalid = ExampleModel();
		Invalid.Nodes[0].Tags.push_back(Invalid.Nodes[0].Tags.front());
		Check(!Gas::Encode(Invalid), "Duplicate tags reject");
		Invalid = ExampleModel();
		Invalid.Nodes[0].Extensions.push_back(Invalid.Nodes[0].Extensions.front());
		Check(!Gas::Encode(Invalid), "Duplicate extension owner rejects");
		Invalid = ExampleModel();
		Invalid.Dependencies.back().Id = Invalid.Dependencies.front().Id;
		Check(!Gas::Encode(Invalid), "Duplicate dependency identity rejects");
		Invalid = ExampleModel();
		Invalid.Nodes.push_back({.Parent = 1, .Class = FolderClass, .DefinitionVersion = 1, .Name = "Late"});
		Check(!Gas::Encode(Invalid), "Preorder cannot reopen a closed root");

		Invalid = Minimal();
		for (std::uint32_t Parent = 1; Parent < Gas::MaximumDepth; ++Parent)
			Invalid.Nodes.push_back({.Parent = Parent, .Class = FolderClass, .DefinitionVersion = 1, .Name = "C"});
		const auto MaximumDepthBytes = Gas::Encode(Invalid);
		Check(MaximumDepthBytes && Gas::Decode(*MaximumDepthBytes), "The hierarchy depth limit itself is accepted");
		Invalid.Nodes.push_back({.Parent = static_cast<std::uint32_t>(Gas::MaximumDepth),
			.Class = FolderClass, .DefinitionVersion = 1, .Name = "C"});
		Check(!Gas::Encode(Invalid), "Hierarchy deeper than the limit rejects");
	}

	void TestMalformedDocuments() {
		Check(!Gas::Decode(Gas::Binary(Gas::MaximumDocumentBytes + 1)),
			"Oversized document rejects");
		const auto Encoded = Gas::Encode(Minimal());
		if (!Encoded || Encoded->size() != 133) return;
		const auto &Bytes = *Encoded;
		for (std::size_t Size = 0; Size < Bytes.size(); ++Size)
			Check(!Gas::Decode(std::span(Bytes).first(Size)), "Truncated document rejects");
		auto Patched = Bytes;
		Patched[FirstName] ^= 1;
		Check(!Gas::Decode(Patched), "Payload damage without rehash rejects");
		Patched = Bytes;
		Patched[10] = static_cast<std::uint8_t>(Gas::DocumentKind::Place);
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Changing the header kind cannot turn a GASM into a GASP");
		Patched = Bytes;
		Patched[8] = 2;
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Newer format version rejects");
		Patched = Bytes;
		Patched[11] = 255;
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Unbounded section directory rejects");
		Patched = Bytes;
		Patched[FirstName] = 0xff;
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Invalid UTF-8 still rejects with a fresh digest");

		RejectPatchedUnsigned32(Bytes, 12, 2, "Unknown schema version rejects");
		RejectPatchedUnsigned32(Bytes, 16, 0xffffffffu, "Incompatible document length rejects");
		RejectPatchedUnsigned32(Bytes, DirectoryStart, 9, "Unknown required section rejects");
		RejectPatchedUnsigned32(Bytes, DirectoryStart + 4, 0xffffffffu, "Out-of-range section offset rejects");
		RejectPatchedUnsigned32(Bytes, DirectoryStart + 8, 0xffffffffu, "Oversized section rejects");
		RejectPatchedUnsigned32(Bytes, DirectoryStart + DirectoryEntryBytes, 1, "Duplicate section ID rejects");
		RejectPatchedUnsigned32(Bytes, DirectoryStart + DirectoryEntryBytes + 4,
			static_cast<std::uint32_t>(ModelGraphStart), "Overlapping sections reject");
		RejectPatchedUnsigned32(Bytes, ModelGraphStart, 0xffffffffu, "Unbounded graph count rejects");
		RejectPatchedUnsigned32(Bytes, FirstParent, 1, "Self-parent rejects");
		RejectPatchedUnsigned32(Bytes, FirstNameLength, 0xffffffffu, "Unbounded string rejects");
		RejectPatchedUnsigned32(Bytes, FirstPropertyCount, 0xffffffffu, "Unbounded property count rejects");
		RejectPatchedUnsigned32(Bytes, Bytes.size() - DigestBytes - 4, 0xffffffffu,
			"Unbounded dependency count rejects");

		auto ReferenceModel = Minimal();
		ReferenceModel.Nodes[0].SavedProperties = {{"A", Gas::LocalReference{0}}, {"B", Gas::LocalReference{0}}};
		const auto ReferenceBytes = Gas::Encode(ReferenceModel);
		Check(ReferenceBytes.has_value(), "Encode fixture with two named local references");
		if (!ReferenceBytes) return;
		constexpr std::size_t FirstKey = FirstPropertyCount + 4 + 4;
		constexpr std::size_t SecondKey = FirstKey + 1 + 1 + 4 + 4;
		RejectPatchedUnsigned32(*ReferenceBytes, SecondKey + 2, 2,
			"Out-of-range reference target still rejects with a fresh digest");
		Patched = *ReferenceBytes;
		Patched[SecondKey] = 'A';
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Duplicate property names reject");
		Patched = *ReferenceBytes;
		Patched[FirstKey] = 'C';
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Noncanonical property order rejects");
		Patched = *ReferenceBytes;
		Patched[FirstKey + 1] = 0xff;
		Rehash(Patched);
		Check(!Gas::Decode(Patched), "Unknown property type rejects");
	}

	void TestSegmentedNativeGrid() {
		using namespace blockgrid;
		// One more dense native leaf would exceed the native field limit. Sixty-five
		// occupied cells per leaf keep physical storage dense and collision scenes small.
		constexpr auto LeafBytes=13+3*LeafVolume;
		constexpr auto LeafCount=(MaximumEncodedBytes-9)/LeafBytes;
		Gas::Binary FieldBytes{'B','G','R','D',1};
		FieldBytes.reserve(9+LeafCount*LeafBytes);
		auto Put=[&](std::uint32_t Value,unsigned Width) {
			for (unsigned I=0;I<Width;++I) FieldBytes.push_back(static_cast<std::uint8_t>(Value>>(8*I)));
		};
		Put(static_cast<std::uint32_t>(LeafCount),4);
		for (unsigned Leaf=0;Leaf<LeafCount;++Leaf) {
			Put(Leaf*LeafSide,4);Put(0,4);Put(0,4);Put(2,1);
			for (unsigned I=0;I<LeafVolume;++I) {
				Put(I<=64?Stone:Air,2);Put(I<=64?(I+Leaf)%24:0,1);
			}
		}
		const auto Native=blockgrid::Decode(FieldBytes);
		Check(Native.GetOccupiedCount()==LeafCount*65 && blockgrid::Encode(Native)==FieldBytes,
			"Near-limit field is an actual canonical native field");
		SegmentCapture Grids;
		const auto Packed=Grids.Store(FieldBytes);
		const auto Manifest=DecodeSegmentManifest(Packed);
		Check(Manifest.Parts.size()==64 && Grids.Parts.size()==64,"Near-limit field occupies 64 distinct bounded segments");
		auto Model=Minimal();
		auto& Node=Model.Nodes.front();
		Node.Class=SchemaId::FromNativeName("Engine","BlockGrid");
		Node.SavedProperties.emplace(SegmentKey,Packed);
		Model.GridSegments=Grids.Parts;
		const auto Bytes=Gas::Encode(Model).value();
		Check(Bytes.size()>Gas::MaximumDocumentBytes && Gas::AdmitsFileLength(Bytes,Bytes.size()),
			"Only native data extends a segmented file past the ordinary graph cap");
		const auto Reopened=Gas::Decode(Bytes).value();
		Check(Reopened==Model && JoinSegments(Manifest,Reopened.GridSegments)==FieldBytes,"Near-limit GASM must preserve every byte");
		SegmentRequirements Requirements;
		for (unsigned I=0;I<4;++I) Requirements.Add(Manifest);
		bool Exhausted=false;
		try { Requirements.Add(Manifest); } catch (const std::length_error&) { Exhausted=true; }
		Check(Exhausted,"Repeated owner references must be charged against the logical document limit");
		Model.GridSegments.erase(Model.GridSegments.begin());
		Check(!Gas::Encode(Model),"Missing segment must be rejected");
		Model.GridSegments=Grids.Parts;
		Node.Class=FolderClass;
		Check(!Gas::Encode(Model),"Reserved segment data requires a native grid owner");
		Node.Class=SchemaId::FromNativeName("Engine","BlockGrid");
		auto& Record=std::get<Gas::Binary>(Node.SavedProperties.at(SegmentKey));
		Record[9]^=1;
		Check(!Gas::Encode(Model),"Wrong full-field digest must fail even when individual segments match");
		// Use the smaller real field for damaged-file tests to keep the corpus inexpensive.
		const auto Representative=blockgrid::Encode(blockgrid::test::LargePublicField());
		Grids={};
		Record=Grids.Store(Representative);
		Model.GridSegments=Grids.Parts;
		auto Damaged=Gas::Encode(Model).value();
		Damaged[Damaged.size()-DigestBytes-1]^=1;
		Rehash(Damaged);
		Check(!Gas::Decode(Damaged),"Independent segment digests must survive outer file-digest replacement");
		Damaged=Gas::Encode(Model).value();
		const auto At=std::search(Damaged.begin(),Damaged.end(),Record.begin(),Record.end());
		if (At==Damaged.end()) throw std::runtime_error("Could not locate encoded native owner manifest");
		At[9]^=1;
		Rehash(Damaged);
		Check(!Gas::Decode(Damaged),"A mismatched complete-field digest must be rejected on file decode");
		Model.Nodes.front().SavedProperties["Ordinary"]=Gas::Binary(Gas::MaximumBinaryBytes+1);
		Check(!Gas::Encode(Model),"Large native sections must preserve the generic per-value limit");
		std::cout<<"[BlockGrid:SegmentCodec] "<<FieldBytes.size()<<" native bytes, "<<Manifest.Parts.size()
			<<" segments, "<<Native.GetOccupiedCount()<<" occupied cells, "<<Bytes.size()<<" portable bytes\n";
	}

	void TestDeterministicMutationCorpus() {
		const auto Encoded = Gas::Encode(ExampleModel());
		if (!Encoded) return;
		std::uint32_t State = 0x7a2c970fu;
		std::size_t Rejected = 0;
		for (std::size_t Index = 0; Index < 512; ++Index) {
			auto Patched = *Encoded;
			State = 1664525u * State + 1013904223u;
			const auto Position = State % (Patched.size() - DigestBytes);
			Patched[Position] ^= static_cast<std::uint8_t>(1u << (State >> 29));
			Rehash(Patched);
			const auto Decoded = Gas::Decode(Patched);
			if (!Decoded) {
				++Rejected;
				continue;
			}
			const auto Canonical = Gas::Encode(*Decoded);
			Check(Canonical.has_value(), "Every accepted corpus case can be encoded");
			if (Canonical) {
				const auto DecodedAgain = Gas::Decode(*Canonical);
				Check(DecodedAgain && *DecodedAgain == *Decoded,
					"Every accepted corpus case survives a semantic round trip");
			}
		}
		Check(Rejected != 0 && Rejected != 512, "Mutation corpus exercises both acceptance and rejection");
	}
}

int main() {
	TestGoldenDocument();
	TestProfilesAndCanonicalValues();
	TestEncodeRejections();
	TestMalformedDocuments();
	TestSegmentedNativeGrid();
	TestDeterministicMutationCorpus();
	return Failures == 0 ? 0 : 1;
}
