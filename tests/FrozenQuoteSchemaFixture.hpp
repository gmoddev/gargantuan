#pragma once

#include "ReliableEnvelopeContractFixture.hpp"
#include "../src/network/FrozenReplicationSchema.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"

namespace gargantuan::test {

inline void TestFrozenQuoteSchemaPin() {
	using namespace network;
	using namespace network::detail;
	const ConnectionId Connection{151, 3};
	const std::map<ConnectionId, std::size_t> Limits{{Connection,
		MaximumReliableServiceGroupBytes - ReliableServiceEnvelopeBytes}};
	auto NextFrame = [&](ReplicationCoordinator &Quote) {
		for (int Attempt = 0; Attempt < 2000; ++Attempt) {
			auto Result = Quote.AdvanceFrozenJournalQuote(Limits);
			EnvelopeRequire(Result.Error.empty(), "pinned frozen replay succeeds");
			if (Result.Frame || Result.Complete) return Result;
		}
		throw std::runtime_error("pinned frozen replay exceeded bounded test work");
	};
	std::unique_ptr<ReplicationCoordinator> Delayed;
	FrozenJournalQuoteStep Expected;
	std::weak_ptr<const RuntimeSchemaRegistry> CapturedSchema;
	const auto OriginalCompatibility = CaptureReplicationSchemaCompatibility();
	{
		auto World = std::make_shared<DataModel>();
		auto CharacterValue = std::make_shared<Character>();
		auto FirstRoot = std::make_shared<Part>();
		auto SecondRoot = std::make_shared<Part>();
		CharacterValue->SetParent(World);
		FirstRoot->SetParent(CharacterValue);
		SecondRoot->SetParent(CharacterValue);
		CharacterValue->SetRootPart(FirstRoot);
		ReplicationCoordinator Live(World);
		PeerRelevanceSelection Selection{.RequiredObjects = {World->GetObjectId()},
			.DesiredObjects = {World->GetObjectId(), CharacterValue->GetObjectId(),
				FirstRoot->GetObjectId(), SecondRoot->GetObjectId()}};
		std::ranges::sort(Selection.DesiredObjects);
		EnvelopeRequire(Live.AddPeer(Connection, ReplicationEpoch(1), Selection).Succeeded(),
			"schema pin fixture commits its native reference baseline");
		CharacterValue->SetRootPart(SecondRoot);
		std::string Error;
		auto Serial = Live.CaptureFrozenQuote(Error);
		Delayed = Live.CaptureFrozenQuote(Error);
		EnvelopeRequire(Serial && Delayed && Error.empty(), "independent quotes capture one immutable schema generation");
		CapturedSchema = GetRuntimeSchemaLifecycle().GetActiveRegistry();
		Expected = NextFrame(*Serial);
		EnvelopeRequire(Expected.Frame.has_value(), "serial quote encodes the native reference change");
		World->Destroy();
	}
	// Main legitimately publishes a different registry after capture. Detached
	// replay must neither borrow its lifetime nor silently adopt its definitions.
	BootstrapPackagedRuntimeSchema(std::string{R"(Schema:RegisterEnum({
		Namespace = "Game", Name = "FrozenQuoteReplacement", Version = 1,
		Items = { Ready = 1 },
	}))"});
	struct RestoreNativeSchema {
		~RestoreNativeSchema() { BootstrapPackagedRuntimeSchema(std::nullopt); }
	} Restore;
	auto CurrentSchema = GetRuntimeSchemaLifecycle().GetActiveRegistry();
	auto PreviousSchema = CapturedSchema.lock();
	EnvelopeRequire(PreviousSchema && PreviousSchema != CurrentSchema &&
		CurrentSchema->FindEnumByName("Game.FrozenQuoteReplacement") &&
		!PreviousSchema->FindEnumByName("Game.FrozenQuoteReplacement"),
		"quote retains the old immutable registry after live source destruction and schema replacement");
	EnvelopeRequire(!IsReplicationSchemaCompatible(OriginalCompatibility),
		"ordinary protocol compatibility observes the replacement registry");
	{
		FrozenReplicationSchemaScope Outer(*PreviousSchema);
		EnvelopeRequire(IsReplicationSchemaCompatible(OriginalCompatibility),
			"quote protocol compatibility uses the pinned schema rather than lifecycle publication");
		bool NestedLookupPassed = false;
		try {
			FrozenReplicationSchemaScope Inner(*CurrentSchema);
			EnvelopeRequire(&GetReplicationSchemaRegistry() == CurrentSchema.get(), "nested schema scope uses its own pin");
			NestedLookupPassed = true;
			throw std::runtime_error("exercise scope unwinding");
		} catch (const std::runtime_error &) {}
		EnvelopeRequire(NestedLookupPassed, "nested schema lookup completes before the intentional exception");
		EnvelopeRequire(&GetReplicationSchemaRegistry() == PreviousSchema.get(), "exception unwinding restores the outer pin");
	}
	EnvelopeRequire(&GetReplicationSchemaRegistry() == CurrentSchema.get(), "scope exit restores ordinary schema behavior");
	PreviousSchema.reset();
	{
		FrozenReplicationSchemaScope Outer(*CurrentSchema);
		const auto Actual = NextFrame(*Delayed);
		EnvelopeRequire(Actual.Frame && Expected.Frame && Actual.Frame->Sequence == Expected.Frame->Sequence &&
			Actual.Frame->CompleteBytes == Expected.Frame->CompleteBytes &&
			Actual.Frame->Fingerprint == Expected.Frame->Fingerprint &&
			Actual.Frame->CursorBefore == Expected.Frame->CursorBefore && Actual.Frame->CursorAfter == Expected.Frame->CursorAfter,
			"post-replacement pinned replay exactly matches serial bytes, sequence, fingerprint and source cursors");
		EnvelopeRequire(&GetReplicationSchemaRegistry() == CurrentSchema.get(), "Advance restores an existing outer schema pin");
		EnvelopeRequire(NextFrame(*Delayed).Complete, "pinned replay converges using only captured source");
	}
	Delayed.reset();
}

}
