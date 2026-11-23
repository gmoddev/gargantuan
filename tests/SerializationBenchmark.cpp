#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
#include "serialization/GlazePrototype.hpp"
#endif
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
#include "BenchmarkAllocationProbe.hpp"
#include "GasBenchmarkAllocations.hpp"
#include <charconv>
#else
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"

#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	double Milliseconds(Clock::time_point Begin, Clock::time_point End) {
		return std::chrono::duration<double, std::milli>(End - Begin).count();
	}

	gargantuan::Snapshot MakeSnapshot(std::size_t ObjectCount) {
		using namespace gargantuan;
		Snapshot Value;
		Value.Cursor = {ObjectId{1, 1}, 1};
		Value.Objects.reserve(ObjectCount);
		Value.Objects.push_back({
			.Id = {1, 1},
			.ClassSchemaId = SchemaId::FromNativeName("Engine", "DataModel"),
			.ClassDefinitionVersion = 1,
			.ClassName = "DataModel",
			.Name = "BenchmarkWorld",
		});
		for (std::size_t Index = 1; Index < ObjectCount; ++Index) {
			Value.Objects.push_back({
				.Id = {static_cast<std::uint32_t>(Index + 1), 1},
				.ClassSchemaId = SchemaId::FromNativeName("Engine", "Folder"),
				.ClassDefinitionVersion = 1,
				.ClassName = "Folder",
				.Name = "Object" + std::to_string(Index),
				.Parent = WireObjectId{1, 1},
				.Attributes = {{"Enabled", true}, {"Index", static_cast<int>(Index)}},
				.Tags = {"Benchmark"},
			});
		}
		return Value;
	}

	bool BenchmarkSnapshot(std::size_t ObjectCount, std::size_t RunCount) {
		const auto ConstructBegin = Clock::now();
		auto Value = MakeSnapshot(ObjectCount);
		const auto ConstructEnd = Clock::now();
		auto Run = [&](std::string_view Backend, auto Encode, auto Decode) {
			auto Warm = Encode(Value); if (!Warm) return false; auto WarmDecoded = Decode(*Warm); if (!WarmDecoded) return false;
			double EncodeMs = 0; double DecodeMs = 0; std::size_t Bytes = 0;
			for (std::size_t RunIndex = 0; RunIndex < RunCount; ++RunIndex) {
				const auto EncodeBegin = Clock::now(); auto Encoded = Encode(Value); const auto EncodeEnd = Clock::now();
				if (!Encoded) return false; Bytes = Encoded->size();
				const auto DecodeBegin = Clock::now(); auto Decoded = Decode(*Encoded); const auto DecodeEnd = Clock::now();
				if (!Decoded || Decoded->Objects.size() != ObjectCount) return false;
				EncodeMs += Milliseconds(EncodeBegin, EncodeEnd); DecodeMs += Milliseconds(DecodeBegin, DecodeEnd);
			}
			EncodeMs /= RunCount; DecodeMs /= RunCount;
			std::cout << Backend << ",Snapshot," << ObjectCount << ',' << Bytes << ',' <<
				Milliseconds(ConstructBegin, ConstructEnd) << ',' << EncodeMs << ',' << DecodeMs << ',' <<
				(EncodeMs + DecodeMs) << ",Ok\n";
			return true;
		};
		auto NlohmannEncode = [](const auto &SnapshotValue) -> std::optional<std::string> { return gargantuan::SerializeSnapshot(SnapshotValue); };
		auto NlohmannDecode = [](std::string_view Text) -> std::optional<gargantuan::Snapshot> { auto Result = gargantuan::DeserializeSnapshot(Text); return Result.Value; };
		#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
		auto GlazeEncode = [](const auto &SnapshotValue) -> std::optional<std::string> { auto Result = gargantuan::GlazePrototype::EncodeSnapshot(SnapshotValue); return Result ? std::optional<std::string>(std::move(*Result)) : std::nullopt; };
		auto GlazeDecode = [](std::string_view Text) -> std::optional<gargantuan::Snapshot> { auto Result = gargantuan::GlazePrototype::DecodeSnapshot(Text); return Result ? std::optional<gargantuan::Snapshot>(std::move(*Result)) : std::nullopt; };
		return Run("nlohmann", NlohmannEncode, NlohmannDecode) && Run("Glaze", GlazeEncode, GlazeDecode);
		#else
		return Run("nlohmann", NlohmannEncode, NlohmannDecode);
		#endif
	}

	bool BenchmarkJournal(std::size_t RecordCount, std::size_t RunCount) {
		using namespace gargantuan;
		const auto ConstructBegin = Clock::now();
		std::vector<std::vector<WireJournalRecord>> Batches;
		for (std::size_t Offset = 0; Offset < RecordCount;) {
			const auto Count = std::min<std::size_t>(MaximumWireJournalRecords, RecordCount - Offset);
			auto &Batch = Batches.emplace_back();
			Batch.reserve(Count);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				Batch.push_back({
					.Sequence = Offset + Index + 1,
					.Scope = {1, 1},
					.Operation = WireJournalOperation::AttributeUpdate,
					.Object = {static_cast<std::uint32_t>((Offset + Index) % 60000 + 2), 1},
					.AttributeName = "Health",
					.Value = static_cast<int>((Offset + Index) % 100),
				});
			}
			Offset += Count;
		}
		const auto ConstructEnd = Clock::now();
		auto Run = [&](std::string_view Backend, auto Encode, auto Decode) {
			std::vector<std::string> Warm; for (const auto &Batch : Batches) { auto E = Encode(Batch); if (!E) return false; Warm.push_back(std::move(*E)); }
			for (const auto &Text : Warm) if (!Decode(Text)) return false;
			double EncodeMs = 0; double DecodeMs = 0; std::size_t EncodedBytes = 0;
			for (std::size_t RunIndex = 0; RunIndex < RunCount; ++RunIndex) {
				std::vector<std::string> Encoded; EncodedBytes = 0; const auto EncodeBegin = Clock::now();
				for (const auto &Batch : Batches) { auto E = Encode(Batch); if (!E) return false; EncodedBytes += E->size(); Encoded.push_back(std::move(*E)); }
				const auto EncodeEnd = Clock::now(); const auto DecodeBegin = Clock::now(); std::size_t Count = 0;
				for (const auto &Text : Encoded) { auto D = Decode(Text); if (!D) return false; Count += *D; }
				const auto DecodeEnd = Clock::now(); if (Count != RecordCount) return false;
				EncodeMs += Milliseconds(EncodeBegin, EncodeEnd); DecodeMs += Milliseconds(DecodeBegin, DecodeEnd);
			}
			EncodeMs /= RunCount; DecodeMs /= RunCount;
			std::cout << Backend << ",Journal," << RecordCount << ',' << EncodedBytes << ',' << Milliseconds(ConstructBegin, ConstructEnd) << ',' << EncodeMs << ',' << DecodeMs << ',' << (EncodeMs + DecodeMs) << ",Ok\n";
			return true;
		};
		auto NlohmannEncode = [](const auto &Batch) -> std::optional<std::string> { return SerializeWireJournalRecords(Batch); };
		auto NlohmannDecode = [](std::string_view Text) -> std::optional<std::size_t> { auto D = DeserializeWireJournalRecords(Text); return D.Value ? std::optional<std::size_t>(D.Value->size()) : std::nullopt; };
		#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
		auto GlazeEncode = [](const auto &Batch) -> std::optional<std::string> { auto E = GlazePrototype::EncodeJournal(Batch); return E ? std::optional<std::string>(std::move(*E)) : std::nullopt; };
		auto GlazeDecode = [](std::string_view Text) -> std::optional<std::size_t> { auto D = GlazePrototype::DecodeJournal(Text); return D ? std::optional<std::size_t>(D->size()) : std::nullopt; };
		return Run("nlohmann", NlohmannEncode, NlohmannDecode) && Run("Glaze", GlazeEncode, GlazeDecode);
		#else
		return Run("nlohmann", NlohmannEncode, NlohmannDecode);
		#endif
	}

	bool BenchmarkEditorHost(std::size_t RequestCount) {
		gargantuan::EditorHost Host("benchmark-token");
		const std::string Request =
			R"({"Version":1,"RequestId":"benchmark","SessionToken":"wrong-token","Method":"Handshake","Params":{}})";
		std::size_t Bytes = 0;
		const auto Begin = Clock::now();
		for (std::size_t Index = 0; Index < RequestCount; ++Index) Bytes += Host.HandleRequest(Request).size();
		const auto End = Clock::now();
		std::cout << "nlohmann,EditorHost," << RequestCount << ',' << Bytes << ",N/A,N/A,N/A," << Milliseconds(Begin, End) << ",Ok\n";
		return Bytes != 0;
	}

	bool BenchmarkProject(std::size_t ObjectCount) {
		using namespace gargantuan;
		const auto ConstructBegin = Clock::now();
		auto Root = std::make_shared<DataModel>();
		Root->SetArchivable(true);
		Root->SetName("BenchmarkProject");
		for (std::size_t Index = 1; Index < ObjectCount; ++Index) {
			auto Child = std::make_shared<Folder>();
			Child->SetName("Object" + std::to_string(Index));
			Child->SetParent(Root);
		}
		std::shared_ptr<Instance> Serializable = Root;
		const auto ConstructEnd = Clock::now();
		const auto EncodeBegin = Clock::now();
		auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Serializable);
		const auto EncodeEnd = Clock::now();
		std::istringstream Input(Encoded);
		const auto DecodeBegin = Clock::now();
		auto Decoded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		const auto DecodeEnd = Clock::now();
		std::cout << "nlohmann,Project," << ObjectCount << ',' << Encoded.size() << ',' <<
			Milliseconds(ConstructBegin, ConstructEnd) << ',' << Milliseconds(EncodeBegin, EncodeEnd) << ',' <<
			Milliseconds(DecodeBegin, DecodeEnd) << ',' << Milliseconds(ConstructBegin, DecodeEnd) << ",Ok\n";
		return Decoded.Ok;
	}

	gargantuan::ProjectId BenchmarkIdentity(std::uint64_t Index) {
		gargantuan::ProjectId Id;
		Id.Bytes.front() = 0x47;
		for (std::size_t Byte = 0; Byte < sizeof(Index); ++Byte)
			Id.Bytes[8 + Byte] = static_cast<std::uint8_t>(Index >> (Byte * 8));
		return Id;
	}

	std::shared_ptr<gargantuan::Instance> MakeGasBenchmarkProject(std::size_t ObjectCount) {
		using namespace gargantuan;
		constexpr std::size_t Branching = 8;
		const auto LastParent = (ObjectCount - 2) / Branching;
		const auto Security = ScriptSecurityContext::CoreTrusted();
		auto Root = std::make_shared<DataModel>();
		Root->SetName("BenchmarkProject");
		Root->SetArchivable(true);
		std::vector<std::shared_ptr<Instance>> Nodes{Root};
		Nodes.reserve(ObjectCount);
		for (std::size_t Index = 1; Index < ObjectCount; ++Index) {
			std::shared_ptr<Instance> Node;
			std::shared_ptr<WeldConstraint> Weld;
			if (Index <= LastParent) {
				Node = std::make_shared<Folder>();
			} else if (Index % 16 == 4) {
				auto ScriptValue = std::make_shared<Script>();
				ScriptValue->SetEnabled(false);
				ScriptValue->SetSourceId(BenchmarkIdentity(Index).ToString());
				ScriptValue->SetSource("local Index = " + std::to_string(Index) + "\nreturn Index\n");
				Node = std::move(ScriptValue);
			} else if (Index % 16 == 7 && Index - 2 > LastParent) {
				Weld = std::make_shared<WeldConstraint>();
				Node = Weld;
			} else {
				auto PartValue = std::make_shared<Part>();
				PartValue->SetAnchored(true);
				PartValue->SetSize({1.0f + static_cast<float>(Index % 8), 1.5f, 2.0f});
				Node = std::move(PartValue);
			}
			Node->SetName("Object" + std::to_string(Index));
			Node->SetArchivable(true);
			Node->SetParent(Nodes[(Index - 1) / Branching]);
			if (Weld) {
				Weld->SetPart0(std::static_pointer_cast<Part>(Nodes[Index - 2]));
				Weld->SetPart1(std::static_pointer_cast<Part>(Nodes[Index - 1]));
			}
			if (Node->ApplyAttributeMutation("Index", WireValue(static_cast<int>(Index)), Security) !=
				MutationStatus::Success)
				throw std::runtime_error("Could not set the GAS benchmark attribute");
			if (Index % 7 == 0 && !Root->Tags.Add(Root->GetObjectId(), Node->GetObjectId(), "Benchmark", Security))
				throw std::runtime_error("Could not set the GAS benchmark tag");
			Nodes.push_back(std::move(Node));
		}
		return Root;
	}

#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
	template <typename Operation>
	auto CountGasBenchmarkAllocations(bool Measured, BenchmarkAllocationProbe::Counts &Counts, Operation &&Execute) {
		if (!Measured) return Execute();
		BenchmarkAllocationProbe::Scope AllocationScope(Counts);
		return Execute();
	}
#endif

	void BenchmarkGasProject(std::size_t ObjectCount, std::size_t RunCount,
		[[maybe_unused]] std::string_view ProfileBackend = {}) {
		using namespace gargantuan;
		const auto ConstructBegin = Clock::now();
		auto Root = MakeGasBenchmarkProject(ObjectCount);
		const auto ConstructEnd = Clock::now();
		const Gas::PlaceMetadata Metadata{BenchmarkIdentity(0), std::nullopt};
		auto Capture = [&](const std::shared_ptr<Instance> &ProjectRoot) {
			auto Document = InstanceSerialization::CaptureGasDocument(std::span(&ProjectRoot, 1),
				Gas::DocumentKind::Place, Metadata);
			if (!Document) throw std::runtime_error(Document.error().Format());
			return std::move(*Document);
		};
#ifndef GARGANTUAN_GAS_BENCHMARK_PROFILE
		const auto Expected = Capture(Root);
		if (Expected.Nodes.size() != ObjectCount) throw std::runtime_error("Unexpected GAS benchmark graph size");
#endif

		auto Run = [&](std::string_view Backend, auto Encode, auto Decode) {
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
			if (Backend != ProfileBackend) return;
			const auto FixturePeak = GasBenchmarkAllocations::ReadPeakResidentBytes();
			std::uint64_t MeasuredPeak = 0;
			BenchmarkAllocationProbe::Counts EncodeAllocations;
			BenchmarkAllocationProbe::Counts DecodeAllocations;
#endif
			std::optional<decltype(Encode())> FirstBytes;
			double EncodeMs = 0;
			double DecodeMs = 0;
			for (std::size_t RunIndex = 0; RunIndex <= RunCount; ++RunIndex) {
				const auto EncodeBegin = Clock::now();
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
				auto Bytes = CountGasBenchmarkAllocations(RunIndex > 0, EncodeAllocations, Encode);
#else
				auto Bytes = Encode();
#endif
				const auto EncodeEnd = Clock::now();
				if (RunIndex == 0) FirstBytes = Bytes;
				else if (Bytes != *FirstBytes) throw std::runtime_error("Benchmark export bytes changed");
				const auto DecodeBegin = Clock::now();
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
				auto Restored = CountGasBenchmarkAllocations(RunIndex > 0, DecodeAllocations,
					[&] { return Decode(std::move(Bytes)); });
#else
				auto Restored = Decode(std::move(Bytes));
#endif
				const auto DecodeEnd = Clock::now();
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
				if (RunIndex == RunCount) {
					// Use a single backend per process and sample before semantic recapture.
					MeasuredPeak = GasBenchmarkAllocations::ReadPeakResidentBytes();
					const auto Expected = Capture(Root);
					if (Expected.Nodes.size() != ObjectCount || Capture(Restored) != Expected)
						throw std::runtime_error("Benchmark round-trip changed persistent state");
				}
#else
				auto Actual = Capture(Restored);
#endif
				Restored->Destroy();
#ifndef GARGANTUAN_GAS_BENCHMARK_PROFILE
				if (Actual != Expected) throw std::runtime_error("Benchmark round-trip changed persistent state");
#endif
				if (RunIndex > 0) {
					EncodeMs += Milliseconds(EncodeBegin, EncodeEnd);
					DecodeMs += Milliseconds(DecodeBegin, DecodeEnd);
				}
			}
			EncodeMs /= RunCount;
			DecodeMs /= RunCount;
			std::cout << Backend << ",PlaceGraph," << ObjectCount << ',' << FirstBytes->size() << ','
					  << Milliseconds(ConstructBegin, ConstructEnd) << ',' << EncodeMs << ',' << DecodeMs << ','
					  << EncodeMs + DecodeMs;
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
			std::cout << ',' << RunCount << ',' << EncodeAllocations.Calls << ',' << EncodeAllocations.Bytes << ','
					  << DecodeAllocations.Calls << ',' << DecodeAllocations.Bytes << ',' << FixturePeak << ','
					  << MeasuredPeak;
#endif
			std::cout << ",Ok\n";
		};

		Run("JSON", [&] { return InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root); },
			[](std::string Text) {
				std::istringstream Input(std::move(Text));
				auto Result = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
				if (!Result.Ok) throw std::runtime_error("Benchmark JSON project could not be deserialized");
				Result.Instance->MarkPersistenceSubtreeArchivable();
				return std::move(Result.Instance);
			});
		Run("GAS", [&] {
			auto Bytes = Gas::Encode(Capture(Root));
			if (!Bytes) throw std::runtime_error(Bytes.error().Format());
			return std::move(*Bytes);
		}, [](Gas::Binary Bytes) {
			auto Document = Gas::Decode(Bytes);
			if (!Document) throw std::runtime_error(Document.error().Format());
			auto Result = InstanceSerialization::MaterializeGasDocument(*Document, true);
			if (!Result.Succeeded() || Result.Roots.size() != 1)
				throw std::runtime_error(Result.Errors.empty() ? "Benchmark GAS place could not be materialized" :
					Result.Errors.front());
			return std::move(Result.Roots.front());
		});
		Root->Destroy();
	}

#ifndef GARGANTUAN_GAS_BENCHMARK_PROFILE
	void BenchmarkProjectLoad(const std::filesystem::path &ProjectPath, const std::filesystem::path &PlacePath) {
		using namespace gargantuan;
		constexpr std::size_t RunCount = 5;
		if (!SDL_Init(0)) throw std::runtime_error("Could not initialize platform file support");
		struct SdlLifetime {
			~SdlLifetime() { SDL_Quit(); }
		} Sdl;

		DiskFilesystem Filesystem(std::filesystem::absolute(ProjectPath).lexically_normal());
		const auto Origin = Project::FromGasOrigin(&Filesystem);
		const auto JsonProject = Project::fromExisting(&Filesystem);
		const auto AbsolutePlace = std::filesystem::absolute(PlacePath).lexically_normal();
		const auto FileSize = [](const std::filesystem::path &Path, std::size_t Maximum) {
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(Path)))
				throw std::runtime_error("Benchmark input must be a regular file");
			const auto Size = std::filesystem::file_size(Path);
			if (Size > Maximum) throw std::length_error("Benchmark input exceeds its document limit");
			return Size;
		};
		if (JsonProject.InstanceFileFormat != InstanceSerialization::InstanceFormat::Json)
			throw std::runtime_error("The project load comparison requires the existing JSON project format");
		const auto JsonBytes = FileSize(JsonProject.InstanceFilePath, MaximumProtocolDocumentBytes);
		const auto GasBytes = FileSize(AbsolutePlace, Gas::MaximumDocumentBytes);
		DiskFilesystem PlaceFilesystem(AbsolutePlace.parent_path());
		const SourceMount PlaceMount(PlaceFilesystem);
		const auto ReadPlace = [&] {
			SourceMountBudget Budget;
			const auto Bytes = PlaceMount.ReadFile(AbsolutePlace.filename(), Gas::MaximumDocumentBytes, Budget);
			if (!Bytes) throw std::runtime_error(Bytes.error().Format());
			auto Place = Gas::Decode(std::span(reinterpret_cast<const std::uint8_t *>(Bytes->data()), Bytes->size()));
			if (!Place) throw std::runtime_error(Place.error().Format());
			return std::move(*Place);
		};
		const auto Document = ReadPlace();
		const auto TrustedPreRun = ReadProjectPreRunSource(Filesystem.Root);
		if (Document.Kind != Gas::DocumentKind::Place || !Document.Place ||
			Document.Place->Identity != Origin.Identity || Document.Place->PreRunSource != TrustedPreRun)
			throw std::runtime_error("Benchmark GASP does not match the trusted project identity and PreRun source");
		BootstrapPackagedRuntimeSchema(TrustedPreRun);
		auto Baseline = Origin.LoadGasPlace(Document);
		Baseline->InitializeLoadedProjectRevision();
		const auto Expected = Origin.CaptureGasPlace(Baseline);
		Baseline->Destroy();
		Baseline.reset();
		if (!Expected) throw std::runtime_error(Expected.error().Format());

		std::cout << "Backend,Workload,Items,DocumentBytes,MeanLoadMs,Runs,Status\n";
		const auto Run = [&](std::string_view Backend, std::uintmax_t Bytes, auto Load) {
			double LoadMs = 0;
			for (std::size_t Index = 0; Index <= RunCount; ++Index) {
				const auto Begin = Clock::now();
				auto World = Load();
				World->InitializeLoadedProjectRevision();
				const auto End = Clock::now();
				const auto Restored = Origin.CaptureGasPlace(World);
				World->Destroy();
				if (!Restored) throw std::runtime_error(Restored.error().Format());
				if (*Restored != *Expected)
					throw std::runtime_error("Benchmark inputs must describe the same persistent project");
				if (Index > 0) LoadMs += Milliseconds(Begin, End);
			}
			std::cout << Backend << ",DiskProjectLoad," << Expected->Nodes.size() << ',' << Bytes << ','
					  << LoadMs / RunCount << ',' << RunCount << ",Ok\n";
		};
		Run("JSON", JsonBytes, [&] {
			auto ProjectValue = Project::fromExisting(&Filesystem);
			return ProjectValue.DeserializeGame();
		});
		Run("GAS", GasBytes, [&] {
			auto Place = ReadPlace();
			const auto ProjectValue = Project::FromGasOrigin(&Filesystem);
			return ProjectValue.LoadGasPlace(Place);
		});
	}
#endif
}

int main(int ArgumentCount, char **Arguments) {
	try {
#ifndef GARGANTUAN_GAS_BENCHMARK_PROFILE
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--gas-project-load") {
			if (ArgumentCount != 4) {
				std::cerr << "Usage: gargantuan_serialization_benchmark --gas-project-load <origin> <gasp-file>\n";
				return 2;
			}
			BenchmarkProjectLoad(Arguments[2], Arguments[3]);
			return 0;
		}
#endif
		gargantuan::BootstrapNativeRuntimeSchema();
#ifdef GARGANTUAN_GAS_BENCHMARK_PROFILE
		constexpr std::string_view Usage = "Usage: gargantuan_gas_benchmark_profile --gas-profile JSON|GAS 100|1000|5000\n";
		if (ArgumentCount != 4 || std::string_view(Arguments[1]) != "--gas-profile") {
			std::cerr << Usage;
			return 2;
		}
		const std::string_view Backend(Arguments[2]);
		const std::string_view CountArgument(Arguments[3]);
		std::size_t ObjectCount = 0;
		const auto [CountEnd, Error] =
			std::from_chars(CountArgument.data(), CountArgument.data() + CountArgument.size(), ObjectCount);
		if ((Backend != "JSON" && Backend != "GAS") || Error != std::errc{} ||
			CountEnd != CountArgument.data() + CountArgument.size() ||
			(ObjectCount != 100 && ObjectCount != 1000 && ObjectCount != 5000)) {
			std::cerr << Usage;
			return 2;
		}
		std::cout << "Backend,Workload,Items,Bytes,ConstructMs,EncodeMs,DecodeMs,CodecTotalMs,Runs,"
					 "EncodeCppNewCallsTotal,EncodeCppNewBytesTotal,DecodeCppNewCallsTotal,DecodeCppNewBytesTotal,"
					 "FixturePeakResidentBytes,MeasuredPeakResidentBytes,Status\n";
		BenchmarkGasProject(ObjectCount, 5, Backend);
		return 0;
#else
		const bool Gas = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--gas";
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		const std::size_t RunCount = Full ? 5 : 1;
		std::cout << "Backend,Workload,Items,Bytes,ConstructMs,EncodeMs,DecodeMs,CodecTotalMs,Status\n";
		if (Gas) {
			for (const auto Count : {100, 1000, 5000}) BenchmarkGasProject(Count, 5);
			return 0;
		}
		bool Ok = true;
		for (const auto Count : Full ? std::vector<std::size_t>{1000, 10000, 20000} : std::vector<std::size_t>{1000})
			Ok = BenchmarkSnapshot(Count, RunCount) && Ok;
		for (const auto Count : Full ? std::vector<std::size_t>{100, 1000, 10000} : std::vector<std::size_t>{100})
			Ok = BenchmarkJournal(Count, RunCount) && Ok;
		Ok = BenchmarkEditorHost(Full ? 10000 : 100) && Ok;
		Ok = BenchmarkProject(Full ? 1000 : 100) && Ok;
		return Ok ? 0 : 1;
#endif
	} catch (const std::exception &Error) {
		std::cerr << "Serialization benchmark failed: " << Error.what() << '\n';
		return 1;
	}
}
