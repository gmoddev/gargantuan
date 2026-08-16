#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "serialization/GlazePrototype.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
	int Failures = 0;

	void Check(bool Condition, std::string_view Message) {
		if (Condition) return;
		++Failures;
		std::cerr << "[Serialization:GlazePrototype] " << Message << '\n';
	}

	std::string Fixture(std::string_view Name) {
		auto Path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "serialization" / Name;
		std::ifstream Input(Path, std::ios::binary);
		std::string Contents{std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
		while (!Contents.empty() && (Contents.back() == '\n' || Contents.back() == '\r')) Contents.pop_back();
		return Contents;
	}
}

int main() {
	using namespace gargantuan;
	BootstrapNativeRuntimeSchema();
	for (const auto Name : {"snapshot_v6_minimal.json"}) {
		auto Text = Fixture(Name);
		auto Decoded = GlazePrototype::DecodeSnapshot(Text);
		auto Encoded = Decoded ? GlazePrototype::EncodeSnapshot(*Decoded) : SerializationResult<std::string>(std::unexpected(Decoded.error()));
		Check(Decoded.has_value(), "Snapshot fixture decodes");
		Check(Encoded.has_value() && *Encoded == Text, "Snapshot fixture re-encodes byte-for-byte");
		if (!Decoded) std::cerr << Decoded.error().Format() << '\n';
		else if (!Encoded) std::cerr << Encoded.error().Format() << '\n';
		else if (*Encoded != Text) std::cerr << "Expected: " << Text << "\nActual:   " << *Encoded << '\n';
	}
	std::vector<WireJournalRecord> Journal{{
		.Sequence = 1, .Scope = {1, 1}, .Operation = WireJournalOperation::AttributeUpdate,
		.Object = {2, 1}, .AttributeName = "Health", .Value = 75,
	}};
	auto JournalText = GlazePrototype::EncodeJournal(Journal);
	auto JournalDecoded = JournalText ? GlazePrototype::DecodeJournal(*JournalText) : SerializationResult<std::vector<WireJournalRecord>>(std::unexpected(JournalText.error()));
	Check(JournalDecoded.has_value() && JournalDecoded->size() == 1 &&
		JournalDecoded->front().Operation == WireJournalOperation::AttributeUpdate &&
		JournalDecoded->front().Value == std::optional<WireValue>(75), "Native journal record round-trips");
	std::vector<WireValue> WireValues{
		std::monostate{}, true, -12, 3.25, WireFloat{1.5f}, std::string("text"),
		WireVector2{1, 2}, WireVector3{1, 2, 3}, WireColor3{0.1f, 0.2f, 0.3f},
		WireUDim{0.5f, -4}, WireUDim2{{0.5f, -4}, {1.0f, 8}},
		WireCFrame{{1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1}},
		WireEnumItem{"Material", "Plastic"},
		WireSchemaEnumValue{SchemaId::FromEnumName("Game", "Prototype"), 1, -2},
		WireObjectReference{{9, 2}},
	};
	std::vector<WireJournalRecord> WireRecords;
	for (std::size_t Index = 0; Index < WireValues.size(); ++Index) WireRecords.push_back({
		.Sequence = Index + 1, .Scope = {1, 1}, .Operation = WireJournalOperation::PropertyUpdate,
		.Object = {2, 1}, .PropertyName = "Value", .Value = WireValues[Index],
	});
	auto AllText = GlazePrototype::EncodeJournal(WireRecords);
	const auto CanonicalWireText = SerializeWireJournalRecords(WireRecords);
	Check(AllText.has_value() && *AllText == CanonicalWireText, "All WireValue encodings match canonical nlohmann bytes");
	if (AllText && *AllText != CanonicalWireText) std::cerr << "Expected: " << CanonicalWireText << "\nActual:   " << *AllText << '\n';
	auto AllDecoded = AllText ? GlazePrototype::DecodeJournal(*AllText) : SerializationResult<std::vector<WireJournalRecord>>(std::unexpected(AllText.error()));
	bool AllEqual = AllDecoded && AllDecoded->size() == WireValues.size();
	for (std::size_t Index = 0; AllEqual && Index < WireValues.size(); ++Index) AllEqual = (*AllDecoded)[Index].Value == WireValues[Index];
	Check(AllEqual, "Every WireValue alternative round-trips through the Glaze journal prototype");
	Check(GlazePrototype::DecodeJournal(R"({"Version":5,"Version":6,"Records":[]})").has_value(), "Duplicate fields preserve last-value-wins compatibility");
	auto DuplicateSnapshotVersion = Fixture("snapshot_v6_minimal.json");
	DuplicateSnapshotVersion.replace(0, std::string_view("{\"Version\":6").size(), "{\"Version\":5,\"Version\":6");
	Check(GlazePrototype::DecodeSnapshot(DuplicateSnapshotVersion).has_value(), "Snapshot duplicate fields preserve last-value-wins compatibility");
	auto DuplicateWireValue = GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":1,"Generation":1},"Operation":"PropertyUpdate","ObjectId":{"Slot":2,"Generation":1},"PropertyName":"Value","Value":{"Type":"Int","Value":1,"Value":2}}]})");
	Check(DuplicateWireValue && DuplicateWireValue->front().Value == std::optional<WireValue>(2), "WireValue duplicate fields preserve last-value-wins compatibility");
	Check(!GlazePrototype::DecodeSnapshot(R"({"Version":6)"), "Malformed Snapshot JSON is rejected");
	Check(!GlazePrototype::DecodeSnapshot(R"({"Version":6,"Cursor":{"Scope":{"Slot":1,"Generation":1},"NextSequence":1},"Objects":[],"Unknown":true})"), "Unknown Snapshot fields are rejected");
	Check(!GlazePrototype::DecodeSnapshot(R"({"Version":6,"Cursor":{"Scope":{"Slot":1,"Generation":1},"NextSequence":1}})"), "Missing Snapshot fields are rejected");
	auto MissingParent = Fixture("snapshot_v6_minimal.json");
	MissingParent.erase(MissingParent.find(",\"Parent\":null"), std::string_view(",\"Parent\":null").size());
	Check(!GlazePrototype::DecodeSnapshot(MissingParent), "Missing required nullable Snapshot Parent is rejected");
	Check(!GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":1,"Generation":1},"Operation":"Destroy","ObjectId":{"Slot":2,"Generation":1},"Unknown":true}]})"), "Unknown journal fields are rejected");
	Check(!GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":1,"Generation":1},"Operation":"Reparent","ObjectId":{"Slot":2,"Generation":1}}]})"), "Missing required nullable Reparent ParentId is rejected");
	Check(!GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":1,"Generation":1},"Operation":"AttributeUpdate","ObjectId":{"Slot":2,"Generation":1},"AttributeName":"Health","Value":{"Type":"Int","Value":1.5}}]})"), "Invalid typed WireValue is rejected");
	Check(!GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":0,"Generation":1},"Operation":"Destroy","ObjectId":{"Slot":2,"Generation":1}}]})"), "Invalid ObjectIds are rejected");
	Check(!GlazePrototype::DecodeJournal(R"({"Version":6,"Records":[{"Version":6,"Sequence":1,"Scope":{"Slot":1,"Generation":1},"Operation":"PropertyUpdate","ObjectId":{"Slot":2,"Generation":1},"PropertyName":"Value","Value":{"Type":"Double","Value":1e999}}]})"), "Non-finite numeric input is rejected");
	std::string ExcessiveNesting(70, '['); ExcessiveNesting.append(70, ']');
	Check(!GlazePrototype::DecodeJournal(ExcessiveNesting), "Excessive JSON nesting is rejected before typed decode");
	Check(!GlazePrototype::DecodeJournal(std::string(gargantuan::MaximumProtocolDocumentBytes + 1, 'x')), "Oversized documents are rejected before parsing");
	Check(!GlazePrototype::DecodeJournal(std::string("{\"Version\":6,\"Records\":[],\"X\":\"") + char(0xff) + "\"}"), "Invalid UTF-8 is rejected");
	return Failures == 0 ? 0 : 1;
}
