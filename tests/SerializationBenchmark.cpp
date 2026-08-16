#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
#include "serialization/GlazePrototype.hpp"
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
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		const std::size_t RunCount = Full ? 5 : 1;
		std::cout << "Backend,Workload,Items,Bytes,ConstructMs,EncodeMs,DecodeMs,CodecTotalMs,Status\n";
		bool Ok = true;
		for (const auto Count : Full ? std::vector<std::size_t>{1000, 10000, 20000} : std::vector<std::size_t>{1000})
			Ok = BenchmarkSnapshot(Count, RunCount) && Ok;
		for (const auto Count : Full ? std::vector<std::size_t>{100, 1000, 10000} : std::vector<std::size_t>{100})
			Ok = BenchmarkJournal(Count, RunCount) && Ok;
		Ok = BenchmarkEditorHost(Full ? 10000 : 100) && Ok;
		Ok = BenchmarkProject(Full ? 1000 : 100) && Ok;
		return Ok ? 0 : 1;
	} catch (const std::exception &Error) {
		std::cerr << "Serialization benchmark failed: " << Error.what() << '\n';
		return 1;
	}
}
