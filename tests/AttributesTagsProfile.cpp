#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireJournal.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	template <typename Callback> double Milliseconds(Callback callback) {
		const auto started = Clock::now();
		callback();
		return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
	}

	void Metric(std::string_view name, double value, std::string_view unit) {
		std::cout << "METRIC " << name << ' ' << value << ' ' << unit << '\n';
	}

	void Require(bool condition, std::string_view message) {
		if (!condition) throw std::runtime_error(std::string(message));
	}
}

int main() {
	using namespace gargantuan;
	try {
		BootstrapNativeRuntimeSchema();
		auto &journal = ChangeJournal::Get();
		journal.SetCapacity(32768);
		journal.Clear();
		MutationGateway mutations;
		auto game = std::make_shared<DataModel>();
		game->SetArchivable(true);
		std::vector<std::shared_ptr<Folder>> objects;
		objects.reserve(10000);

		Metric("construct_10000_instances", Milliseconds([&] {
			for (std::size_t index = 0; index < 10000; ++index) {
				auto object = std::make_shared<Folder>();
				object->SetName("Profile" + std::to_string(index));
				object->SetParent(game);
				objects.push_back(std::move(object));
			}
		}), "ms");

		Metric("set_attributes_1000x4", Milliseconds([&] {
			for (std::size_t index = 0; index < 1000; ++index) {
				const auto id = objects[index]->GetObjectId();
				Require(mutations.Apply(UpdateAttributeCommand{id, "Health", WireValue(100)}).Succeeded(), "Health rejected");
				Require(mutations.Apply(UpdateAttributeCommand{id, "Active", WireValue(true)}).Succeeded(), "Active rejected");
				Require(mutations.Apply(UpdateAttributeCommand{id, "Label", WireValue(std::string("Profile"))}).Succeeded(), "Label rejected");
				Require(mutations.Apply(UpdateAttributeCommand{id, "Weight", WireValue(1.25)}).Succeeded(), "Weight rejected");
			}
		}), "ms");

		Metric("set_sparse_attributes_10000", Milliseconds([&] {
			for (std::size_t index = 0; index < objects.size(); index += 10)
				Require(mutations.Apply(UpdateAttributeCommand{objects[index]->GetObjectId(), "Sparse", WireValue(static_cast<int>(index))}).Succeeded(), "Sparse rejected");
		}), "ms");

		const std::string nearLimit(4000, 'x');
		Metric("set_near_limit_attributes", Milliseconds([&] {
			for (int index = 0; index < 3; ++index)
				Require(mutations.Apply(UpdateAttributeCommand{
					objects.back()->GetObjectId(), "Payload" + std::to_string(index), WireValue(nearLimit)
				}).Succeeded(), "Near-limit attribute rejected");
		}), "ms");

		Metric("index_14000_tag_memberships", Milliseconds([&] {
			for (std::size_t index = 0; index < objects.size(); ++index) {
				const auto id = objects[index]->GetObjectId();
				if (index < 8000) Require(mutations.Apply(AddTagCommand{id, "Enemy"}).Succeeded(), "Enemy rejected");
				if (index % 2 == 0) Require(mutations.Apply(AddTagCommand{id, "Alive"}).Succeeded(), "Alive rejected");
				if (index % 10 == 0) Require(mutations.Apply(AddTagCommand{id, "Visible"}).Succeeded(), "Visible rejected");
			}
		}), "ms");

		const auto scope = game->GetObjectId();
		std::size_t queryResultCount = 0;
		Metric("query_enemy_x100", Milliseconds([&] {
			for (int repeat = 0; repeat < 100; ++repeat)
				queryResultCount += game->Tags.GetTagged(scope, "Enemy", ScriptSecurityContext::CoreTrusted()).size();
		}), "ms");
		Metric("query_enemy_alive_x100", Milliseconds([&] {
			for (int repeat = 0; repeat < 100; ++repeat)
				queryResultCount += game->Tags.GetTaggedAll(scope, {"Enemy", "Alive"}, ScriptSecurityContext::CoreTrusted()).size();
		}), "ms");
		Metric("query_enemy_alive_visible_x100", Milliseconds([&] {
			for (int repeat = 0; repeat < 100; ++repeat)
				queryResultCount += game->Tags.GetTaggedAll(scope, {"Enemy", "Alive", "Visible"}, ScriptSecurityContext::CoreTrusted()).size();
		}), "ms");
		Metric("query_result_objects", static_cast<double>(queryResultCount), "count");

		Metric("tag_add_remove_churn_10000", Milliseconds([&] {
			const auto id = objects.back()->GetObjectId();
			for (std::size_t index = 0; index < 10000; ++index) {
				const auto tag = "Churn" + std::to_string(index);
				Require(mutations.Apply(AddTagCommand{id, tag}).Succeeded(), "Churn add rejected");
				Require(mutations.Apply(RemoveTagCommand{id, tag}).Succeeded(), "Churn remove rejected");
			}
		}), "ms");

		Snapshot snapshot;
		Metric("capture_snapshot_10000", Milliseconds([&] { snapshot = CaptureSnapshot(game); }), "ms");
		std::string serializedSnapshot;
		Metric("serialize_snapshot_10000", Milliseconds([&] { serializedSnapshot = SerializeSnapshot(snapshot); }), "ms");
		Metric("snapshot_bytes", static_cast<double>(serializedSnapshot.size()), "bytes");
		SnapshotParseResult parsedSnapshot;
		Metric("parse_snapshot_10000", Milliseconds([&] { parsedSnapshot = DeserializeSnapshot(serializedSnapshot); }), "ms");
		Require(parsedSnapshot.Succeeded(), "Profile snapshot parse failed");
		SnapshotLoadResult loadedSnapshot;
		Metric("load_snapshot_10000", Milliseconds([&] { loadedSnapshot = LoadSnapshot(*parsedSnapshot.Value); }), "ms");
		Require(loadedSnapshot.Succeeded(), "Profile snapshot load failed");

		journal.Clear();
		const auto cursor = journal.CreateCursor(scope);
		Metric("attribute_updates_1000", Milliseconds([&] {
			for (std::size_t index = 0; index < 1000; ++index)
				Require(mutations.Apply(UpdateAttributeCommand{
					objects[index]->GetObjectId(), "Health", WireValue(static_cast<int>(index))
				}).Succeeded(), "Profile update rejected");
		}), "ms");
		auto records = journal.Read(cursor).Records;
		std::vector<WireJournalRecord> wireRecords;
		wireRecords.reserve(records.size());
		for (const auto &record : records) wireRecords.push_back(EncodeChangeRecord(record));
		const auto serializedJournal = SerializeWireJournalRecords(wireRecords);
		Metric("journal_records", static_cast<double>(wireRecords.size()), "count");
		Metric("journal_bytes", static_cast<double>(serializedJournal.size()), "bytes");

		auto churnParent = std::make_shared<Folder>();
		churnParent->SetParent(game);
		for (std::size_t index = 0; index < 1000; ++index) {
			auto child = std::make_shared<Folder>();
			child->SetParent(churnParent);
			Require(mutations.Apply(AddTagCommand{child->GetObjectId(), "Disposable"}).Succeeded(), "Disposable rejected");
		}
		Metric("destroy_1000_tagged_descendants", Milliseconds([&] { churnParent->Destroy(); }), "ms");
		Require(game->Tags.GetTagged(scope, "Disposable", ScriptSecurityContext::CoreTrusted()).empty(), "Destroy retained tags");

		journal.SetCapacity(4096);
		std::cout << "PROFILE AttributesTags complete\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "PROFILE FAILED: " << error.what() << '\n';
		return 1;
	}
}
