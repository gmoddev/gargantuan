#include "editor/StreamingDiagnosticsEncoding.hpp"

namespace gargantuan {
	JsonCodec::Json EncodeStreamingDiagnosticsSnapshot(const StreamingDiagnostics &Snapshot) {
		using Json = JsonCodec::Json;
		const auto &Policy = Snapshot.Policy;
		const auto &Metrics = Policy.Metrics;
		const auto &Content = Snapshot.Content;
		Json Units = Json::array(), Samples = Json::array();
		const auto StateName = [](ContentResidencyState State) {
			switch (State) {
			case ContentResidencyState::Unavailable: return "Unavailable";
			case ContentResidencyState::Requested: return "Requested";
			case ContentResidencyState::Acquiring: return "Acquiring";
			case ContentResidencyState::Available: return "Available";
			case ContentResidencyState::Admitting: return "Admitting";
			case ContentResidencyState::Resident: return "Resident";
			case ContentResidencyState::Evicting: return "Evicting";
			case ContentResidencyState::Failed: return "Failed";
			}
			return "Unavailable";
		};
		const auto Duration = [](const ContentAvailabilityDurationMetric &Metric) -> Json {
			return {{"Samples", Metric.Samples}, {"TotalMicroseconds", Metric.TotalMicroseconds},
				{"MaximumMicroseconds", Metric.MaximumMicroseconds}};
		};
		const auto ClassName = [](ResidencyDemandClass Class) {
			switch (Class) {
			case ResidencyDemandClass::Critical: return "Critical";
			case ResidencyDemandClass::Gameplay: return "Gameplay";
			case ResidencyDemandClass::Predictive: return "Predictive";
			case ResidencyDemandClass::Presentation: return "Presentation";
			case ResidencyDemandClass::Speculative: return "Speculative";
			}
			return "Speculative";
		};
		for (const auto &Unit : Policy.Units) {
			const auto &Demand = Unit.Demand;
			Units.push_back({{"ManifestIndex", Unit.ManifestIndex}, {"Key", Demand.Key},
				{"Class", ClassName(Demand.Class)}, {"DeadlineTick", Demand.DeadlineTick ? Json(*Demand.DeadlineTick) : Json(nullptr)},
				{"Revision", Demand.Revision}, {"DistanceSquared", Demand.DistanceSquared}, {"CostBytes", Demand.CostBytes},
				{"Desired", Demand.Desired}, {"PolicyRequestOwned", Demand.Requested}, {"InGrace", Unit.InGrace},
				{"AvailabilityState", Unit.AvailabilityState ? Json(StateName(*Unit.AvailabilityState)) : Json(nullptr)},
				{"GraceUntilTick", Unit.GraceUntilTick ? Json(*Unit.GraceUntilTick) : Json(nullptr)},
				{"PendingRelease", Unit.PendingRelease}});
		}
		for (const auto &Sample : Policy.TimingSamples)
			Samples.push_back({{"Tick", Sample.Tick}, {"EvaluationNanoseconds", Sample.EvaluationNanoseconds},
				{"ReconciliationNanoseconds", Sample.ReconciliationNanoseconds}, {"MembershipVisits", Sample.MembershipVisits},
				{"Requests", Sample.Requests}, {"Releases", Sample.Releases}, {"BudgetFlags", Sample.BudgetFlags}});
		return {{"Version", 1}, {"Enabled", Snapshot.Enabled},
			{"Status", GetStreamingDiagnosticStatusName(Snapshot.Status)}, {"SimulationTick", Snapshot.SimulationTick},
			{"ContentConfigured", Snapshot.ContentConfigured}, {"ManifestAvailable", Snapshot.ManifestAvailable},
			{"LastContentStepNanoseconds", Snapshot.LastContentStepNanoseconds},
			{"Policy", {{"Enabled", Policy.Enabled}, {"Stopped", Policy.Stopped}, {"Tick", Policy.Tick},
				{"Revision", Policy.Revision}, {"ManifestUnits", Policy.ManifestUnits}, {"Focuses", Metrics.Focuses},
				{"PendingFocuses", Metrics.PendingFocuses}, {"UnhealthyFocuses", Metrics.UnhealthyFocuses},
				{"Candidates", Metrics.RetainedCandidates}, {"DemandRecords", Metrics.DemandRecords},
				{"DesiredUnits", Policy.DesiredUnits}, {"GraceExpiryRecords", Policy.GraceExpiryRecords},
				{"FocusContributionRecords", Policy.FocusContributionRecords},
				{"OwnedRequests", Policy.OwnedRequests}, {"PendingDeltas", Metrics.PendingDeltas},
				{"Requests", Metrics.Requests}, {"Releases", Metrics.Releases}, {"RequestDeferrals", Metrics.RequestDeferrals},
				{"StaleDecisions", Metrics.StaleRevisions}, {"QueryFailures", Metrics.QueryFailures},
				{"ServiceBindingFailures", Metrics.ServiceBindingFailures}, {"BudgetLimitedTicks", Policy.BudgetLimitedTicks},
				{"MemoryAccounting", "LogicalPayloadV1"}, {"PolicyStateLogicalBytes", Policy.PolicyStateLogicalBytes},
				{"TimingAccounting", "CorePhasesExcludeObservation"},
				{"ReferencedManifestEntryBytes", Policy.ReferencedManifestEntryBytes},
				{"IndexMembershipPayloadBytes", Policy.IndexMembershipPayloadBytes},
				{"Limits", {{"Focuses", Policy.Limits.MaximumFocuses}, {"CandidatesPerFocus", Policy.Limits.MaximumCandidatesPerFocus},
					{"QueryPagesPerTick", Policy.Limits.MaximumFocusEvaluationsPerTick}, {"QueryCellsPerTick", Policy.Limits.MaximumQueryCellsPerTick},
					{"MembershipVisitsPerTick", Policy.Limits.MaximumMembershipVisitsPerTick}, {"DemandDeltasPerTick", Policy.Limits.MaximumDemandDeltasPerTick},
					{"CandidateRevalidationsPerTick", Policy.Limits.MaximumCandidateRevalidationsPerTick},
					{"RequestEligibilityVisitsPerTick", Policy.Limits.MaximumRequestValidationVisitsPerTick}}},
				{"Units", std::move(Units)}, {"NextIndex", Policy.NextIndex ? Json(*Policy.NextIndex) : Json(nullptr)},
				{"TimingSamples", std::move(Samples)}}},
			{"Content", {{"Stopped", Content.Stopped}, {"Requests", Content.Requests}, {"Acquisitions", Content.Acquisitions},
				{"Admissions", Content.Admissions}, {"Evictions", Content.Evictions}, {"Failures", Content.Failures},
				{"Cancellations", Content.Cancellations}, {"CacheHits", Content.CacheHits},
				{"CompletionCapacityDeferrals", Content.CompletionCapacityDeferrals}, {"AdmissionDeferrals", Content.AdmissionDeferrals},
				{"DecodedCapacityDeferrals", Content.DecodedCapacityDeferrals}, {"ReservedCompletionBytes", Content.ReservedCompletionBytes},
				{"CachedPayloadBytes", Content.CachedPayloadBytes}, {"DecodedDocumentBytes", Content.DecodedDocumentBytes},
				{"RetainedRecords", Content.RetainedRecords},
				{"Timing", {{"AcceptedToQueued", Duration(Content.Timing.AcceptedToQueued)},
					{"QueuedToProviderStart", Duration(Content.Timing.QueuedToProviderStart)},
					{"Provider", Duration(Content.Timing.Provider)}, {"Verification", Duration(Content.Timing.Verification)},
					{"WorkerPreparation", Duration(Content.Timing.WorkerPreparation)},
					{"PreparationToCommit", Duration(Content.Timing.PreparationToCommit)}, {"Commit", Duration(Content.Timing.Commit)},
					{"EndToEnd", Duration(Content.Timing.EndToEnd)}, {"Step", Duration(Content.Timing.Step)}}}}}};
	}
}
