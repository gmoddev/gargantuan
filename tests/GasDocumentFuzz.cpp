// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/serialization/GasDocument.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
	using namespace gargantuan;

	constexpr std::size_t DigestBytes = 32;
	constexpr std::size_t MaximumInputBytes = 8192;

	void WriteSeed(const std::filesystem::path &Path, const Gas::Document &Document) {
		const auto Bytes = Gas::Encode(Document);
		if (!Bytes) std::abort();
		std::ofstream Output(Path, std::ios::binary);
		Output.write(reinterpret_cast<const char *>(Bytes->data()), Bytes->size());
		Output.close();
		if (!Output) std::abort();
	}

	void CheckCanonical(std::span<const std::uint8_t> Bytes) {
		const auto Document = Gas::Decode(Bytes);
		if (!Document) return;
		const auto Reencoded = Gas::Encode(*Document);
		if (!Reencoded || !std::ranges::equal(*Reencoded, Bytes)) std::abort();
	}
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
	const auto *Corpus = std::getenv("GAS_FUZZ_CORPUS");
	if (!Corpus) return 0;

	Gas::Document Model;
	Model.Nodes.push_back({
		.Class = SchemaId::FromNativeName("Engine", "Folder"),
		.DefinitionVersion = 1,
		.Name = "Root",
		.SavedProperties = {
			{"Binary", Gas::Binary{0, 1, 255}},
			{"Frame", WireValue{WireCFrame{{1.0f, 2.0f, 3.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}}}},
			{"Target", Gas::LocalReference{2}},
		},
		.Attributes = {{"Count", WireValue{32}}},
		.Extensions = {{{1, 2}, 1, {{"Flag", WireValue{true}}}}},
		.CustomProperties = {{{3, 4}, 1, {{"Weight", WireValue{0.5}}}}},
		.Tags = {"Portable"},
	});
	Model.Nodes.push_back({
		.Parent = 1,
		.Class = SchemaId::FromNativeName("Engine", "Script"),
		.DefinitionVersion = 1,
		.Name = "Logic",
		.SavedProperties = {{"Source", WireValue{std::string("return 42")}}},
	});
	const Gas::Binary Asset{1, 4, 9};
	Model.Dependencies = {{{5, 6}, AssetContentId::Hash(Asset)}};
	const std::filesystem::path Path(Corpus);
	WriteSeed(Path / "model.gasm", Model);
	Model.Kind = Gas::DocumentKind::Place;
	Model.Nodes.front().Class = SchemaId::FromNativeName("Engine", "DataModel");
	Model.Place = Gas::PlaceMetadata{.Identity = {{1}}, .PreRunSource = "return nil"};
	WriteSeed(Path / "place.gasp", Model);
	return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *Data, std::size_t Size) {
	if (Size > MaximumInputBytes) return 0;
	const std::span Input(Data, Size);
	CheckCanonical(Input);
	if (Size < DigestBytes) return 0;

	// Keep a second path past the checksum so the fuzzer can reach section parsing.
	Gas::Binary Rehashed(Input.begin(), Input.end());
	const auto Body = std::span(Rehashed).first(Size - DigestBytes);
	const auto Digest = AssetContentId::Hash(Body);
	std::copy(Digest.Bytes.begin(), Digest.Bytes.end(), Rehashed.begin() + Body.size());
	CheckCanonical(Rehashed);
	return 0;
}
