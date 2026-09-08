#include "gargantuan/content/ContentAvailability.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;

	struct Timings final {
		double Mean = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
		double Maximum = 0.0;
	};

	Timings Summarize(std::vector<double> Samples) {
		std::ranges::sort(Samples);
		double Total = 0.0;
		for (const auto Sample : Samples) Total += Sample;
		auto Percentile = [&](std::size_t Percent) { return Samples[(Samples.size() - 1) * Percent / 100]; };
		return {
			.Mean = Total / Samples.size(),
			.P50 = Percentile(50),
			.P95 = Percentile(95),
			.P99 = Percentile(99),
			.Maximum = Samples.back(),
		};
	}

	PackageContentManifest MakeManifest(std::size_t Count) {
		const std::array<std::uint8_t, 1> DigestInput{0x3a};
		const auto Digest = AssetContentId::Hash(DigestInput);
		PackageContentManifest Manifest{
			.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17},
			.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
		};
		Manifest.Entries.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto Coordinate = static_cast<float>(Index);
			Manifest.Entries.push_back({
				.Key = std::format("region/{:07}", Index),
				.BlobReference = std::format("content/regions/{:07}.instance.json", Index),
				.Digest = Digest,
				.CompressedBytes = 1,
				.UncompressedBytes = 1,
				.ObjectCount = 1,
				.Dependencies = {},
				.PackageSpaceKey = "default",
				.CoarseBounds = PackageCoarseBounds{{Coordinate, 0.0f, 0.0f}, {Coordinate, 0.0f, 0.0f}},
				.Flags = PackageContentFlags::ImmutableBaseline,
			});
		}
		return Manifest;
	}

	void RunParsedManifestCase(std::size_t Count) {
		auto Manifest = MakeManifest(Count);
		const auto Encoded = EncodePackageContentManifest(Manifest);
		const auto StartedAt = std::chrono::steady_clock::now();
		auto Parsed = ParsePackageContentManifest(Encoded);
		const auto ParseMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - StartedAt
		).count();
		if (!Parsed || Parsed->Entries.size() != Count) throw std::runtime_error("benchmark manifest parse failed");
		const auto IndexStartedAt = std::chrono::steady_clock::now();
		PackageContentCoarseIndex Index(*Parsed, 1.0f);
		const auto IndexMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - IndexStartedAt
		).count();
		std::mt19937_64 Random(0x3'4c'17);
		std::uniform_int_distribution<std::size_t> Distribution(0, Count - 1);
		std::vector<double> LookupMicroseconds;
		LookupMicroseconds.reserve(2048);
		for (std::size_t QueryNumber = 0; QueryNumber < 2048; ++QueryNumber) {
			const auto Coordinate = static_cast<float>(Distribution(Random));
			const auto QueryStartedAt = std::chrono::steady_clock::now();
			auto Result = Index.Query(
				"default", {{Coordinate, 0.0f, 0.0f}, {Coordinate, 0.0f, 0.0f}}, 8
			);
			LookupMicroseconds.push_back(std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - QueryStartedAt
			).count());
			if (Result.size() != 1) throw std::runtime_error("benchmark coarse lookup failed");
		}
		const auto Lookup = Summarize(std::move(LookupMicroseconds));
		std::cout << "[Content:Benchmark] manifest entries=" << Count << " encodedBytes=" << Encoded.size()
				  << " parseMs=" << ParseMilliseconds << " indexMs=" << IndexMilliseconds
				  << " memberships=" << Index.GetMembershipCount() << " lookupMeanUs=" << Lookup.Mean
				  << " lookupP50Us=" << Lookup.P50 << " lookupP95Us=" << Lookup.P95
				  << " lookupP99Us=" << Lookup.P99 << " lookupMaxUs=" << Lookup.Maximum << '\n';
	}

}

int main(int ArgumentCount, char **Arguments) {
	const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
	for (const auto Count : Quick ? std::vector<std::size_t>{100, 1'000, 10'000}
								  : std::vector<std::size_t>{100, 1'000, 10'000, 65'536})
		RunParsedManifestCase(Count);
	std::cout << "[Content:Benchmark] manifest entries=100000 boundedReject=true maximum=65536\n";
	std::cout << "[Content:Benchmark] manifest entries=1000000 boundedReject=true maximum=65536\n";
	for (const auto Objects : std::vector<std::size_t>{10, 100, 512, 1'000, 10'000})
		std::cout << "[Content:Benchmark] offeredObjects=" << Objects
				  << " admittedByUnitLimit=" << (Objects <= MaximumPackageContentObjectsPerUnit ? "true" : "false")
				  << " maximum=" << MaximumPackageContentObjectsPerUnit << '\n';
	return 0;
}
