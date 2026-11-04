#include "gargantuan/content/ResidencyDemandPolicy.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace gargantuan;

namespace {
	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	PackageContentManifest ManifestWith(std::initializer_list<PackageContentEntry> Entries) {
		PackageContentManifest Manifest;
		Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 7};
		Manifest.Entries.assign(Entries.begin(), Entries.end());
		std::ranges::sort(Manifest.Entries, {}, &PackageContentEntry::Key);
		return Manifest;
	}

	PackageContentEntry Unit(std::string Key, float X, std::string Space = "default") {
		PackageContentEntry Entry;
		Entry.Key = std::move(Key);
		Entry.PackageSpaceKey = std::move(Space);
		Entry.CoarseBounds = PackageCoarseBounds{{X, 0.0f, 0.0f}, {X, 0.0f, 0.0f}};
		return Entry;
	}

	const ResidencyDemandSnapshot *Find(const std::vector<ResidencyDemandSnapshot> &Demands, std::string_view Key) {
		const auto Found = std::ranges::find_if(Demands, [Key](const auto &Demand) { return Demand.Key == Key; });
		return Found == Demands.end() ? nullptr : &*Found;
	}

	bool Desired(const ResidencyDemandPolicy &Policy, std::string_view Key) {
		const auto Demands = Policy.GetDemands();
		const auto *Demand = Find(Demands, Key);
		return Demand && Demand->Desired;
	}

	void StepThrough(ResidencyDemandPolicy &Policy, std::uint64_t First, std::uint64_t Last) {
		for (auto Tick = First; Tick <= Last; ++Tick) Policy.Step(nullptr, Tick);
	}

	void RequirementPriorityAndDeadline() {
		auto Manifest = ManifestWith({Unit("critical-late", 0.0f), Unit("critical-soon", 0.0f),
			Unit("gameplay", 0.0f), Unit("presentation", 0.0f)});
		ResidencyDemandPolicy Policy(Manifest);
		Check(Policy.SetRequirement("presentation", ResidencyDemandClass::Presentation),
			"presentation requirement rejected");
		Check(Policy.SetRequirement("critical-late", ResidencyDemandClass::Critical, 200),
			"critical requirement rejected");
		Check(Policy.SetRequirement("critical-soon", ResidencyDemandClass::Critical, 100),
			"earlier deadline rejected");
		Check(Policy.SetRequirement("gameplay", ResidencyDemandClass::Gameplay, 1),
			"gameplay requirement rejected");
		Policy.Step(nullptr, 1);
		const auto Demands = Policy.GetDemands();
		Check(Demands.size() == 4, "requirements did not produce four distinct demands");
		for (const auto &Demand : Demands) Check(Demand.Desired, "requirement lost desired state");
		Check(Demands[0].Key == "critical-soon" && Demands[1].Key == "critical-late" &&
			Demands[2].Key == "gameplay" && Demands[3].Key == "presentation",
			"semantic class and deadline did not determine stable demand order");
		const auto *Soon = Find(Demands, "critical-soon");
		const auto *Late = Find(Demands, "critical-late");
		Check(Soon && Late && Soon->Class == ResidencyDemandClass::Critical && Late->Class == ResidencyDemandClass::Critical &&
			Soon->DeadlineTick == 100 && Late->DeadlineTick == 200,
			"semantic class or deadline was lost from a demand");
		Check(!Policy.SetRequirement("unknown", ResidencyDemandClass::Critical),
			"unknown package key became trusted demand");
		Check(Policy.ClearRequirement("critical-soon"), "requirement clear rejected");
		Policy.Step(nullptr, 2);
		Check(!Desired(Policy, "critical-soon"), "cleared requirement remained desired");
	}

	void DefaultSpaceAndBoundary() {
		auto Manifest = ManifestWith({Unit("default-near", 10.0f), Unit("default-outside", 11.0f),
			Unit("other-near", 0.0f, "other")});
		ResidencyDemandPolicyLimits Limits;
		Limits.EnterRadius = 10.0f;
		Limits.LeaveRadius = 12.0f;
		Limits.LookaheadSeconds = 0.0f;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "default-space focus rejected");
		Policy.Step(nullptr, 1);
		Check(Desired(Policy, "default-near"), "entry boundary excluded a default-space unit");
		Check(!Desired(Policy, "default-outside"), "unit outside entry radius became desired");
		Check(!Desired(Policy, "other-near"), "focus crossed package-space boundary");
	}

	void StableDistanceAndKeyTie() {
		auto Manifest = ManifestWith({Unit("a", 2.0f), Unit("b", 2.0f),
			Unit("far", 10.0f), Unit("near", 1.0f)});
		ResidencyDemandPolicy Policy(Manifest);
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "distance focus rejected");
		Policy.Step(nullptr, 1);
		auto Demands = Policy.GetDemands();
		Check(Demands.size() == 4 && Demands[0].Key == "near" && Demands[1].Key == "a" &&
			Demands[2].Key == "b" && Demands[3].Key == "far",
			"distance and stable key did not break semantic ties");
		Check(Policy.SetRequirement("far", ResidencyDemandClass::Critical, 2),
			"critical override rejected");
		Policy.Step(nullptr, 2);
		Demands = Policy.GetDemands();
		Check(Demands[0].Key == "far" && Demands[0].Class == ResidencyDemandClass::Critical,
			"semantic requirement did not outrank nearer spatial demand");
		Check(Policy.ClearRequirement("far"), "critical override clear rejected");
		Policy.Step(nullptr, 3);
		Demands = Policy.GetDemands();
		Check(Demands[3].Key == "far" && Demands[3].Class == ResidencyDemandClass::Gameplay,
			"cleared requirement failed to restore spatial priority");
	}

	void HysteresisEntryAndExit() {
		auto Manifest = ManifestWith({Unit("edge", 0.0f)});
		ResidencyDemandPolicyLimits Limits;
		Limits.EnterRadius = 10.0f;
		Limits.LeaveRadius = 15.0f;
		Limits.LookaheadSeconds = 0.0f;
		Limits.GraceTicks = 2;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "initial focus rejected");
		Policy.Step(nullptr, 1);
		Check(Desired(Policy, "edge"), "entry did not create demand");
		Check(Policy.UpdateFocus(1, {12, 0, 0}), "hysteresis focus rejected");
		Policy.Step(nullptr, 2);
		Check(Desired(Policy, "edge"), "demand flapped between enter and leave radii");
		Check(Policy.UpdateFocus(1, {20, 0, 0}), "exit focus rejected");
		Policy.Step(nullptr, 3);
		Check(!Desired(Policy, "edge"), "outside leave radius remained spatially desired");
	}

	void LookaheadAndTeleportRevision() {
		auto Manifest = ManifestWith({Unit("start", 0.0f), Unit("ahead", 20.0f), Unit("far", 100.0f)});
		ResidencyDemandPolicyLimits Limits;
		Limits.EnterRadius = 5.0f;
		Limits.LeaveRadius = 6.0f;
		Limits.LookaheadSeconds = 10.0f;
		Limits.MaximumLookaheadDistance = 20.0f;
		Limits.GraceTicks = 0;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}, {100, 0, 0}), "accepted movement rejected");
		Policy.Step(nullptr, 1);
		Check(Desired(Policy, "ahead"), "bounded lookahead omitted content at its clamp");
		Check(!Desired(Policy, "far"), "lookahead exceeded its distance clamp");
		Check(Policy.UpdateFocus(1, {100, 0, 0}, {}), "teleport focus rejected");
		Policy.Step(nullptr, 2);
		Check(Desired(Policy, "far"), "teleport did not publish the current focus");
		Check(!Desired(Policy, "start") && !Desired(Policy, "ahead"),
			"old movement revision retained obsolete demand");
		Check(Policy.UpdateFocus(1, {0, 0, 0}) && Policy.UpdateFocus(1, {100, 0, 0}),
			"rapid focus revisions rejected");
		Policy.Step(nullptr, 3);
		Check(Desired(Policy, "far") && !Desired(Policy, "start"),
			"superseded rapid movement became current demand");
	}

	void StalePagedRevision() {
		auto Manifest = ManifestWith({Unit("a", 0.0f), Unit("b", 0.0f), Unit("c", 0.0f),
			Unit("teleport", 1'000.0f)});
		ResidencyDemandPolicyLimits Limits;
		Limits.EnterRadius = 1.0f;
		Limits.LeaveRadius = 2.0f;
		Limits.LookaheadSeconds = 0.0f;
		Limits.MaximumQueryCellsPerTick = 1;
		Limits.MaximumMembershipVisitsPerTick = 1;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "paged focus rejected");
		Policy.Step(nullptr, 1);
		Check(Policy.GetMetrics().PendingFocuses == 1, "dense query did not remain paged");
		Check(Policy.UpdateFocus(1, {1'000, 0, 0}), "paged query teleport rejected");
		StepThrough(Policy, 2, 20);
		Check(Policy.GetMetrics().StaleRevisions >= 1, "superseded paged revision was not rejected");
		Check(Desired(Policy, "teleport") && !Desired(Policy, "a") && !Desired(Policy, "b") &&
			!Desired(Policy, "c"), "stale paged candidates entered current demand");
	}

	void DenseMovingSweep() {
		PackageContentManifest Manifest;
		Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 7};
		for (std::size_t Index = 0; Index <= 300; ++Index) {
			const auto Number = std::to_string(Index);
			const auto Key = "unit/" + std::string(3 - Number.size(), '0') + Number;
			const float X = Index == 0 ? 0.0f : Index <= 7 ? 100.0f : Index == 8 ? 150.0f : 300.0f;
			Manifest.Entries.push_back(Unit(Key, X));
		}
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumCandidatesPerFocus = 8;
		Limits.MaximumFocusEvaluationsPerTick = 1;
		Limits.MaximumQueryCellsPerTick = 4;
		Limits.MaximumMembershipVisitsPerTick = 7;
		Limits.MaximumCandidateRevalidationsPerTick = 16;
		Limits.EnterRadius = 100.0f;
		Limits.LeaveRadius = 110.0f;
		Limits.LookaheadSeconds = 0.0f;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {100, 0, 0}), "initial dense focus rejected");
		std::uint64_t Tick = 0;
		while (Policy.GetMetrics().CompletedFocusEvaluations == 0 && Tick < 100)
			Policy.Step(nullptr, ++Tick);
		Check(Policy.GetMetrics().CompletedFocusEvaluations == 1 && !Desired(Policy, "unit/000") &&
			Desired(Policy, "unit/008"), "initial dense ranking did not discard the distant early entry");

		bool CompletedWhileMoving = false;
		for (std::size_t Move = 0; Move < 100; ++Move) {
			const float X = Move % 2 == 0 ? 0.0f : 1.0f;
			const auto BeforeUpdate = Policy.GetMetrics();
			Check(Policy.UpdateFocus(1, {X, 0, 0}), "moving dense focus rejected");
			const auto AfterUpdate = Policy.GetMetrics();
			Check(AfterUpdate.QueryPages == BeforeUpdate.QueryPages &&
				AfterUpdate.CandidateRevalidations == BeforeUpdate.CandidateRevalidations &&
				AfterUpdate.RankingOperations == BeforeUpdate.RankingOperations,
				"focus update performed paged query or candidate ranking work");
			Policy.Step(nullptr, ++Tick);
			const auto AfterStep = Policy.GetMetrics();
			Check(AfterStep.QueryCellsVisited - AfterUpdate.QueryCellsVisited <= Limits.MaximumQueryCellsPerTick &&
				AfterStep.QueryMembershipVisits - AfterUpdate.QueryMembershipVisits <= Limits.MaximumMembershipVisitsPerTick &&
				AfterStep.CandidateRevalidations - AfterUpdate.CandidateRevalidations <=
					Limits.MaximumCandidateRevalidationsPerTick &&
				AfterStep.RetainedCandidates <= 2 * Limits.MaximumCandidatesPerFocus,
				"moving dense evaluation exceeded one-tick query or revalidation budget");
			Check(!Desired(Policy, "unit/008"), "current-invalid candidate remained published after movement");
			Policy.Step(nullptr, Tick);
			const auto Repeated = Policy.GetMetrics();
			Check(Repeated.QueryPages == AfterStep.QueryPages &&
				Repeated.QueryCellsVisited == AfterStep.QueryCellsVisited &&
				Repeated.QueryMembershipVisits == AfterStep.QueryMembershipVisits &&
				Repeated.CandidateRevalidations == AfterStep.CandidateRevalidations &&
				Repeated.CompletedFocusEvaluations == AfterStep.CompletedFocusEvaluations,
				"repeating Step at the same tick consumed additional policy work");
			if (AfterStep.MovingSweepCompletions != 0) CompletedWhileMoving = true;
		}
		Check(CompletedWhileMoving && Policy.GetMetrics().CompletedFocusEvaluations >= 2,
			"continuous bounded movement prevented the dense paged sweep from completing");

		Check(Policy.UpdateFocus(1, {0, 0, 0}), "settled dense focus rejected");
		const auto InitialCompletions = Policy.GetMetrics().CompletedFocusEvaluations;
		for (std::size_t Step = 0; Step < 200 && Policy.GetMetrics().PendingFocuses != 0; ++Step)
			Policy.Step(nullptr, ++Tick);
		const auto Settled = Policy.GetDemands();
		Check(Policy.GetMetrics().CompletedFocusEvaluations > InitialCompletions &&
			Policy.GetMetrics().PendingFocuses == 0 && Settled.size() == 8 &&
			Settled.front().Key == "unit/000" && !Find(Settled, "unit/008"),
			"settled full sweep failed to recover the exact best eight current entries");
	}

	void EquivalentFocusAndLifetime() {
		auto Manifest = ManifestWith({Unit("shared", 0.0f)});
		ResidencyDemandPolicyLimits Limits;
		Limits.GraceTicks = 0;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}) && Policy.UpdateFocus(2, {0, 0, 0}),
			"equivalent trusted focuses rejected");
		Policy.Step(nullptr, 1);
		const auto Demands = Policy.GetDemands();
		Check(Demands.size() == 1 && Demands.front().Desired,
			"equivalent focuses duplicated content demand");
		Check(Policy.RemoveFocus(1), "first owner removal rejected");
		Policy.Step(nullptr, 2);
		Check(Desired(Policy, "shared"), "one owner removal revoked another owner demand");
		Check(Policy.RemoveFocus(2), "second owner removal rejected");
		StepThrough(Policy, 3, 5);
		Check(!Desired(Policy, "shared"), "ended owners retained content demand");
		Check(Policy.UpdateFocus(2, {0, 0, 0}), "reconnected owner rejected");
		Policy.Step(nullptr, 6);
		Check(Desired(Policy, "shared"), "reconnected owner failed to create fresh demand");
		Policy.Stop(nullptr);
		Check(Policy.GetMetrics().Focuses == 0, "stopped policy retained owner focus");
	}

	void BoundsAndDenial() {
		auto Manifest = ManifestWith({Unit("a", 0.0f), Unit("b", 0.0f), Unit("c", 0.0f)});
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumFocuses = 1;
		Limits.MaximumCandidatesPerFocus = 1;
		Limits.MaximumFocusEvaluationsPerTick = 1;
		Limits.MaximumQueryCellsPerTick = 1;
		Limits.MaximumMembershipVisitsPerTick = 1;
		Limits.MaximumDemandDeltasPerTick = 1;
		Limits.EnterRadius = 1.0f;
		Limits.LeaveRadius = 2.0f;
		Limits.LookaheadSeconds = 0.0f;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "bounded first focus rejected");
		Check(!Policy.UpdateFocus(0, {0, 0, 0}), "invalid owner identity admitted");
		Check(!Policy.UpdateFocus(2, {0, 0, 0}), "focus count cap admitted an extra owner");
		Check(!Policy.UpdateFocus(1, {10'000'001, 0, 0}), "out-of-range focus admitted");
		Check(!Policy.UpdateFocus(1, {0, 0, 0}, {}, static_cast<ResidencyDemandClass>(255)),
			"invalid priority class admitted");
		Check(!Policy.UpdateFocus(1, {std::numeric_limits<float>::quiet_NaN(), 0, 0}),
			"nonfinite position admitted");
		Check(!Policy.UpdateFocus(1, {0, 0, 0}, {std::numeric_limits<float>::infinity(), 0, 0}),
			"nonfinite accepted velocity admitted");
		Check(!Policy.RemoveFocus(2), "unknown owner removal reported success");
		StepThrough(Policy, 1, 20);
		const auto Metrics = Policy.GetMetrics();
		Check(Metrics.Focuses == 1 && Metrics.RetainedCandidates <= Limits.MaximumCandidatesPerFocus &&
			Metrics.QueryCellsVisited <= 20 * Limits.MaximumQueryCellsPerTick &&
			Metrics.QueryMembershipVisits <= 20 * Limits.MaximumMembershipVisitsPerTick &&
			Metrics.DemandRecords <= Manifest.Entries.size(),
			"policy exceeded finite focus, query, or retained-demand bounds");
		Check(Metrics.CompletedFocusEvaluations >= 1 && Policy.GetDemands().size() == 1,
			"dense candidates did not converge to the one-unit retention cap");
		auto InvalidLimits = Limits;
		InvalidLimits.MaximumDemandDeltasPerTick = 0;
		bool Rejected = false;
		try {
			ResidencyDemandPolicy Invalid(Manifest, InvalidLimits);
		} catch (const std::invalid_argument &) {
			Rejected = true;
		}
		Check(Rejected, "zero demand-delta budget was admitted");
	}

	void ManifestLifetime() {
		auto Original = ManifestWith({Unit("old", 0.0f)});
		auto Replacement = ManifestWith({Unit("new", 0.0f)});
		Replacement.Package.PackageVersion = Original.Package.PackageVersion + 1;
		ResidencyDemandPolicy OldPolicy(Original);
		Check(OldPolicy.UpdateFocus(1, {0, 0, 0}), "old policy focus rejected");
		OldPolicy.Step(nullptr, 1);
		Check(Desired(OldPolicy, "old"), "old manifest had no demand");
		OldPolicy.Stop(nullptr);
		ResidencyDemandPolicy NewPolicy(Replacement);
		Check(!NewPolicy.SetRequirement("old", ResidencyDemandClass::Critical),
			"replacement policy accepted a stale manifest key");
		Check(NewPolicy.UpdateFocus(1, {0, 0, 0}), "new policy focus rejected");
		NewPolicy.Step(nullptr, 2);
		Check(Desired(NewPolicy, "new") && !Desired(NewPolicy, "old"),
			"replacement policy inherited old manifest demand");
	}
}

int main() {
	try {
		RequirementPriorityAndDeadline();
		DefaultSpaceAndBoundary();
		StableDistanceAndKeyTie();
		HysteresisEntryAndExit();
		LookaheadAndTeleportRevision();
		StalePagedRevision();
		DenseMovingSweep();
		EquivalentFocusAndLifetime();
		BoundsAndDenial();
		ManifestLifetime();
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ResidencyTests] " << Error.what() << '\n';
		return 1;
	} catch (...) {
		std::cerr << "[Content:ResidencyTests] unexpected nonstandard exception\n";
		return 1;
	}
}
