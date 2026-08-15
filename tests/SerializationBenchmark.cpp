#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireJournal.hpp"

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

	bool BenchmarkSnapshot(std::size_t ObjectCount) {
		const auto ConstructBegin = Clock::now();
		auto Value = MakeSnapshot(ObjectCount);
		const auto ConstructEnd = Clock::now();
		const auto EncodeBegin = Clock::now();
		auto Encoded = gargantuan::SerializeSnapshot(Value);
		const auto EncodeEnd = Clock::now();
		const auto DecodeBegin = Clock::now();
		auto Decoded = gargantuan::DeserializeSnapshot(Encoded);
		const auto DecodeEnd = Clock::now();
		const bool ExpectedDocumentLimit = !Decoded.Succeeded() && Encoded.size() > gargantuan::MaximumProtocolDocumentBytes;
		std::cout << "Snapshot," << ObjectCount << ',' << Encoded.size() << ',' <<
			Milliseconds(ConstructBegin, ConstructEnd) << ',' << Milliseconds(EncodeBegin, EncodeEnd) << ',' <<
			Milliseconds(DecodeBegin, DecodeEnd) << ',' << Milliseconds(ConstructBegin, DecodeEnd) << ',' <<
			(ExpectedDocumentLimit ? "Expected8MiBLimit" : Decoded.Succeeded() ? "Ok" : "Failed") << '\n';
		return ExpectedDocumentLimit || (Decoded.Succeeded() && Decoded.Value->Objects.size() == ObjectCount);
	}

	bool BenchmarkJournal(std::size_t RecordCount) {
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
		std::vector<std::string> Encoded;
		std::size_t EncodedBytes = 0;
		const auto EncodeBegin = Clock::now();
		for (const auto &Batch : Batches) {
			Encoded.push_back(SerializeWireJournalRecords(Batch));
			EncodedBytes += Encoded.back().size();
		}
		const auto EncodeEnd = Clock::now();
		const auto DecodeBegin = Clock::now();
		std::size_t DecodedRecords = 0;
		for (const auto &Batch : Encoded) {
			auto Decoded = DeserializeWireJournalRecords(Batch);
			if (!Decoded.Succeeded()) return false;
			DecodedRecords += Decoded.Value->size();
		}
		const auto DecodeEnd = Clock::now();
		std::cout << "Journal," << RecordCount << ',' << EncodedBytes << ',' <<
			Milliseconds(ConstructBegin, ConstructEnd) << ',' << Milliseconds(EncodeBegin, EncodeEnd) << ',' <<
			Milliseconds(DecodeBegin, DecodeEnd) << ',' << Milliseconds(ConstructBegin, DecodeEnd) << ",Ok\n";
		return DecodedRecords == RecordCount;
	}

	bool BenchmarkEditorHost(std::size_t RequestCount) {
		gargantuan::EditorHost Host("benchmark-token");
		const std::string Request =
			R"({"Version":1,"RequestId":"benchmark","SessionToken":"wrong-token","Method":"Handshake","Params":{}})";
		std::size_t Bytes = 0;
		const auto Begin = Clock::now();
		for (std::size_t Index = 0; Index < RequestCount; ++Index) Bytes += Host.HandleRequest(Request).size();
		const auto End = Clock::now();
		std::cout << "EditorHost," << RequestCount << ',' << Bytes << ",N/A,N/A,N/A," << Milliseconds(Begin, End) << ",Ok\n";
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
		std::cout << "Project," << ObjectCount << ',' << Encoded.size() << ',' <<
			Milliseconds(ConstructBegin, ConstructEnd) << ',' << Milliseconds(EncodeBegin, EncodeEnd) << ',' <<
			Milliseconds(DecodeBegin, DecodeEnd) << ',' << Milliseconds(ConstructBegin, DecodeEnd) << ",Ok\n";
		return Decoded.Ok;
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		std::cout << "Workload,Items,Bytes,ConstructMs,EncodeMs,DecodeMs,TotalMs,Status\n";
		bool Ok = true;
		for (const auto Count : Full ? std::vector<std::size_t>{1000, 10000, 50000} : std::vector<std::size_t>{1000})
			Ok = BenchmarkSnapshot(Count) && Ok;
		for (const auto Count : Full ? std::vector<std::size_t>{100, 1000, 10000} : std::vector<std::size_t>{100})
			Ok = BenchmarkJournal(Count) && Ok;
		Ok = BenchmarkEditorHost(Full ? 10000 : 100) && Ok;
		Ok = BenchmarkProject(Full ? 1000 : 100) && Ok;
		return Ok ? 0 : 1;
	} catch (const std::exception &Error) {
		std::cerr << "Serialization benchmark failed: " << Error.what() << '\n';
		return 1;
	}
}
