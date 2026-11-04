#include "gargantuan/content/ResidencyDemandPolicy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		constexpr std::string_view DefaultPackageSpace = "default";
		constexpr float MaximumFocusCoordinate = 10'000'000.0f;

		bool Finite(glm::vec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool ValidClass(ResidencyDemandClass Class) {
			return Class >= ResidencyDemandClass::Critical && Class <= ResidencyDemandClass::Speculative;
		}

		float DistanceSquared(glm::vec3 Point, const PackageCoarseBounds &Bounds) {
			double Result = 0.0;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				const double Coordinate = Point[Axis];
				const double Outside = Coordinate < Bounds.Minimum[Axis]
					? static_cast<double>(Bounds.Minimum[Axis]) - Coordinate
					: Coordinate > Bounds.Maximum[Axis]
						? Coordinate - static_cast<double>(Bounds.Maximum[Axis]) : 0.0;
				Result += Outside * Outside;
			}
			return static_cast<float>(std::min(Result, static_cast<double>(std::numeric_limits<float>::max())));
		}

		// Distance from a finite movement segment to a box. The squared distance is
		// quadratic between the at-most-six box-plane crossings.
		float SegmentDistanceSquared(glm::vec3 Start, glm::vec3 End, const PackageCoarseBounds &Bounds) {
			std::array<double, 8> Breaks{};
			std::size_t Count = 0;
			Breaks[Count++] = 0.0;
			Breaks[Count++] = 1.0;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				const double Initial = Start[Axis];
				const double Delta = static_cast<double>(End[Axis]) - Initial;
				if (Delta == 0.0) continue;
				for (const double Plane : {static_cast<double>(Bounds.Minimum[Axis]),
					static_cast<double>(Bounds.Maximum[Axis])}) {
					const double Crossing = (Plane - Initial) / Delta;
					if (Crossing > 0.0 && Crossing < 1.0) Breaks[Count++] = Crossing;
				}
			}
			std::sort(Breaks.begin(), Breaks.begin() + Count);
			double Best = std::numeric_limits<double>::max();
			for (std::size_t Interval = 1; Interval < Count; ++Interval) {
				const double Lower = Breaks[Interval - 1];
				const double Upper = Breaks[Interval];
				const double Midpoint = (Lower + Upper) * 0.5;
				double Quadratic = 0.0;
				double Linear = 0.0;
				for (std::size_t Axis = 0; Axis < 3; ++Axis) {
					const double Initial = Start[Axis];
					const double Delta = static_cast<double>(End[Axis]) - Initial;
					const double AtMidpoint = Initial + Midpoint * Delta;
					const double Plane = AtMidpoint < Bounds.Minimum[Axis]
						? Bounds.Minimum[Axis] : AtMidpoint > Bounds.Maximum[Axis]
							? Bounds.Maximum[Axis] : AtMidpoint;
					if (AtMidpoint >= Bounds.Minimum[Axis] && AtMidpoint <= Bounds.Maximum[Axis]) continue;
					Quadratic += Delta * Delta;
					Linear += Delta * (Initial - Plane);
				}
				const double Time = Quadratic == 0.0 ? Lower : std::clamp(-Linear / Quadratic, Lower, Upper);
				double Distance = 0.0;
				for (std::size_t Axis = 0; Axis < 3; ++Axis) {
					const double Position = static_cast<double>(Start[Axis]) +
						Time * (static_cast<double>(End[Axis]) - Start[Axis]);
					const double Outside = Position < Bounds.Minimum[Axis]
						? static_cast<double>(Bounds.Minimum[Axis]) - Position
						: Position > Bounds.Maximum[Axis]
							? Position - static_cast<double>(Bounds.Maximum[Axis]) : 0.0;
					Distance += Outside * Outside;
				}
				Best = std::min(Best, Distance);
			}
			return static_cast<float>(std::min(Best, static_cast<double>(std::numeric_limits<float>::max())));
		}

		const PackageContentManifest &ValidatePolicyInput(
			const PackageContentManifest &Manifest, const ResidencyDemandPolicyLimits &Limits
		) {
			if (Manifest.Entries.size() > MaximumPackageContentUnits || Limits.MaximumFocuses == 0 ||
				Limits.MaximumFocuses > 512 || Limits.MaximumCandidatesPerFocus == 0 ||
				Limits.MaximumCandidatesPerFocus > 1'024 || Limits.MaximumFocusEvaluationsPerTick == 0 ||
				Limits.MaximumFocusEvaluationsPerTick > 512 || Limits.MaximumQueryCellsPerTick == 0 ||
				Limits.MaximumQueryCellsPerTick > MaximumPackageCoarseQueryCells ||
				Limits.MaximumMembershipVisitsPerTick == 0 ||
				Limits.MaximumMembershipVisitsPerTick > MaximumPackageCoarseQueryMembershipVisits ||
				Limits.MaximumDemandDeltasPerTick == 0 || Limits.MaximumDemandDeltasPerTick > 1'024 ||
				Limits.MaximumCandidateRevalidationsPerTick < 2 * Limits.MaximumCandidatesPerFocus ||
				Limits.MaximumCandidateRevalidationsPerTick > 65'536 ||
				Limits.MaximumRequestValidationVisitsPerTick == 0 ||
				Limits.MaximumRequestValidationVisitsPerTick > 65'536 ||
				!std::isfinite(Limits.EnterRadius) || !std::isfinite(Limits.LeaveRadius) ||
				!std::isfinite(Limits.LookaheadSeconds) || !std::isfinite(Limits.MaximumLookaheadDistance) ||
				Limits.EnterRadius <= 0.0f || Limits.LeaveRadius <= Limits.EnterRadius ||
				Limits.LeaveRadius > 4'096.0f || Limits.LookaheadSeconds < 0.0f ||
				Limits.LookaheadSeconds > 10.0f || Limits.MaximumLookaheadDistance < 0.0f ||
				Limits.MaximumLookaheadDistance > 4'096.0f || Limits.GraceTicks > 60'000)
				throw std::invalid_argument("[Content:ResidencyPolicy] invalid policy limits");
			const auto Slack = std::min(Limits.EnterRadius, 512.0f);
			const auto WorstEdge = 2.0 * Limits.LeaveRadius + Limits.MaximumLookaheadDistance + 2.0 * Slack;
			const auto WorstCellSpan = std::ceil(WorstEdge / 512.0) + 1.0;
			if (WorstCellSpan * WorstCellSpan * WorstCellSpan > MaximumPackageCoarseQueryCells)
				throw std::invalid_argument("[Content:ResidencyPolicy] query volume exceeds coarse cell limit");
			return Manifest;
		}

		std::shared_ptr<const PackageContentManifest> ValidatePolicySnapshot(
			std::shared_ptr<const PackageContentManifest> Manifest,
			const ResidencyDemandPolicyLimits &Limits
		) {
			if (!Manifest) throw std::invalid_argument("[Content:ResidencyPolicy] missing manifest snapshot");
			(void)ValidatePolicyInput(*Manifest, Limits);
			return Manifest;
		}
	}

	struct ResidencyDemandPolicy::Impl {
		struct Candidate final {
			std::size_t Index = 0;
			ResidencyDemandClass Class = ResidencyDemandClass::Presentation;
			std::optional<std::uint64_t> DeadlineTick;
			float DistanceSquared = 0.0f;
			std::uint64_t CostBytes = 0;
			std::uint64_t Revision = 0;
		};
		struct Requirement final {
			ResidencyDemandClass Class = ResidencyDemandClass::Gameplay;
			std::optional<std::uint64_t> DeadlineTick;
			std::uint64_t Revision = 0;
		};
		using RankKey = std::tuple<ResidencyDemandClass, std::uint64_t, float, std::uint64_t,
			std::string, std::size_t>;
		struct Focus final {
			glm::vec3 Position{};
			glm::vec3 Velocity{};
			ResidencyDemandClass Class = ResidencyDemandClass::Gameplay;
			std::uint64_t Revision = 0;
			bool NeedsEvaluation = true;
			bool NeedsRevalidation = false;
			bool MovedDuringSweep = false;
			bool Unhealthy = false;
			std::uint64_t RetryAtTick = 0;
			bool Attached = false;
			PackageCoarseBounds QueryBounds{};
			glm::vec3 Lookahead{};
			std::optional<PackageCoarseQueryCursor> Cursor;
			std::map<std::size_t, Candidate> Working;
			std::map<std::size_t, Candidate> Committed;
		};
		struct DemandRecord final {
			std::map<ResidencyFocusId, Candidate> Focuses;
			std::set<std::pair<RankKey, ResidencyFocusId>> RankedFocuses;
			std::optional<Requirement> Required;
			std::optional<std::uint64_t> GraceUntil;
			bool Requested = false;
		};
		struct PendingDelta final {
			bool Release = false;
			ResidencyDemandClass Class = ResidencyDemandClass::Speculative;
			std::uint64_t DeadlineTick = std::numeric_limits<std::uint64_t>::max();
			float DistanceSquared = std::numeric_limits<float>::max();
			std::uint64_t CostBytes = 0;
			std::string Key;
			std::size_t Index = 0;
			bool operator<(const PendingDelta &Other) const {
				if (Release != Other.Release) return !Release;
				if (Class != Other.Class) return Class < Other.Class;
				if (DeadlineTick != Other.DeadlineTick) return DeadlineTick < Other.DeadlineTick;
				if (DistanceSquared != Other.DistanceSquared) return DistanceSquared < Other.DistanceSquared;
				if (CostBytes != Other.CostBytes) return CostBytes < Other.CostBytes;
				if (Key != Other.Key) return Key < Other.Key;
				return Index < Other.Index;
			}
		};

		std::shared_ptr<const PackageContentManifest> ManifestOwner;
		const PackageContentManifest &Manifest;
		PackageContentCoarseIndex Index;
		ResidencyDemandPolicyLimits Limits;
		ResidencyDemandPolicyMetrics Metrics;
		std::map<ResidencyFocusId, Focus> Focuses;
		std::map<std::size_t, DemandRecord> Demands;
		std::set<std::pair<std::uint64_t, std::size_t>> Expiries;
		std::set<PendingDelta> Pending;
		std::map<std::size_t, PendingDelta> PendingByIndex;
		std::optional<PendingDelta> PendingScanAfter;
		ResidencyFocusId NextFocus = 0;
		std::uint64_t Revision = 0;
		std::uint64_t LastTick = 0;
		bool NextRelease = false;
		bool Stopped = false;
		ContentAvailabilityService *BoundAvailability = nullptr;
		std::shared_ptr<const PackageContentManifest> BoundManifest;
		bool ExactSnapshotRequired = false;

		explicit Impl(const PackageContentManifest &Input, ResidencyDemandPolicyLimits InputLimits)
			: Impl(std::make_shared<const PackageContentManifest>(ValidatePolicyInput(Input, InputLimits)),
				InputLimits, false) {}

		explicit Impl(std::shared_ptr<const PackageContentManifest> Input, ResidencyDemandPolicyLimits InputLimits)
			: Impl(std::move(Input), InputLimits, true) {}

		Impl(std::shared_ptr<const PackageContentManifest> Input, ResidencyDemandPolicyLimits InputLimits,
			bool RequireExactSnapshot)
			: ManifestOwner(ValidatePolicySnapshot(std::move(Input), InputLimits)), Manifest(*ManifestOwner),
			  Index(Manifest), Limits(InputLimits), ExactSnapshotRequired(RequireExactSnapshot) {}

		bool Better(const Candidate &Left, const Candidate &Right) const {
			if (Left.Class != Right.Class) return Left.Class < Right.Class;
			const auto LeftDeadline = Left.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max());
			const auto RightDeadline = Right.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max());
			if (LeftDeadline != RightDeadline) return LeftDeadline < RightDeadline;
			if (Left.DistanceSquared != Right.DistanceSquared) return Left.DistanceSquared < Right.DistanceSquared;
			if (Left.CostBytes != Right.CostBytes) return Left.CostBytes < Right.CostBytes;
			return Manifest.Entries[Left.Index].Key < Manifest.Entries[Right.Index].Key;
		}

		RankKey Rank(const Candidate &Value) const {
			return {Value.Class, Value.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max()),
				Value.DistanceSquared, Value.CostBytes, Manifest.Entries[Value.Index].Key, Value.Index};
		}

		Candidate Best(std::size_t Unit, const DemandRecord &Record) const {
			Candidate Result{.Index = Unit};
			Result.CostBytes = Manifest.Entries[Unit].UncompressedBytes;
			bool HaveResult = false;
			if (Record.Required) {
				Result.Class = Record.Required->Class;
				Result.DeadlineTick = Record.Required->DeadlineTick;
				Result.Revision = Record.Required->Revision;
				HaveResult = true;
			}
			if (!Record.RankedFocuses.empty()) {
				const auto &Value = Record.Focuses.at(Record.RankedFocuses.begin()->second);
				if (!HaveResult || Better(Value, Result)) Result = Value;
			}
			return Result;
		}

		void RemovePending(std::size_t Unit) {
			if (auto Found = PendingByIndex.find(Unit); Found != PendingByIndex.end()) {
				Pending.erase(Found->second);
				PendingByIndex.erase(Found);
			}
		}

		void Refresh(std::size_t Unit, std::uint64_t Tick) {
			auto Found = Demands.find(Unit);
			if (Found == Demands.end()) return;
			auto &Record = Found->second;
			RemovePending(Unit);
			const bool Desired = !Record.Focuses.empty() || Record.Required.has_value();
			if (Desired) {
				if (Record.GraceUntil) Expiries.erase({*Record.GraceUntil, Unit});
				Record.GraceUntil.reset();
				if (!Record.Requested) {
					const auto Selected = Best(Unit, Record);
					PendingDelta Delta{false, Selected.Class,
						Selected.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max()),
						Selected.DistanceSquared, Selected.CostBytes, Manifest.Entries[Unit].Key, Unit};
					Pending.insert(Delta);
					PendingByIndex.emplace(Unit, std::move(Delta));
				}
			} else if (Record.Requested) {
				if (!Record.GraceUntil) {
					Record.GraceUntil = Tick > std::numeric_limits<std::uint64_t>::max() - Limits.GraceTicks
						? std::numeric_limits<std::uint64_t>::max() : Tick + Limits.GraceTicks;
					Expiries.insert({*Record.GraceUntil, Unit});
				}
				if (*Record.GraceUntil <= Tick) {
					PendingDelta Delta{true, ResidencyDemandClass::Speculative,
						std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<float>::max(),
						0, Manifest.Entries[Unit].Key, Unit};
					Pending.insert(Delta);
					PendingByIndex.emplace(Unit, std::move(Delta));
				}
			} else {
				if (Record.GraceUntil) Expiries.erase({*Record.GraceUntil, Unit});
				Demands.erase(Found);
			}
		}

		void ChangeContribution(ResidencyFocusId Id, const Candidate &Value, bool Add, std::uint64_t Tick) {
			auto &Record = Demands[Value.Index];
			if (auto Existing = Record.Focuses.find(Id); Existing != Record.Focuses.end()) {
				Record.RankedFocuses.erase({Rank(Existing->second), Id});
				Record.Focuses.erase(Existing);
				++Metrics.RankingOperations;
			}
			if (Add) {
				Record.Focuses.emplace(Id, Value);
				Record.RankedFocuses.insert({Rank(Value), Id});
				++Metrics.RankingOperations;
			}
			Refresh(Value.Index, Tick);
		}

		void ResolveLookahead(Focus &Value) const {
			const glm::dvec3 Position(Value.Position);
			const glm::dvec3 Velocity(Value.Velocity);
			auto Displacement = Velocity * static_cast<double>(Limits.LookaheadSeconds);
			const auto Length = std::sqrt(glm::dot(Displacement, Displacement));
			if (Length > Limits.MaximumLookaheadDistance && Length > 0.0)
				Displacement *= static_cast<double>(Limits.MaximumLookaheadDistance) / Length;
			Value.Lookahead = glm::vec3(Position + Displacement);
		}

		PackageCoarseBounds RequiredBounds(const Focus &Value) const {
			PackageCoarseBounds Bounds;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				Bounds.Minimum[Axis] = std::min(Value.Position[Axis], Value.Lookahead[Axis]) - Limits.LeaveRadius;
				Bounds.Maximum[Axis] = std::max(Value.Position[Axis], Value.Lookahead[Axis]) + Limits.LeaveRadius;
			}
			return Bounds;
		}

		void BuildBounds(Focus &Value) const {
			ResolveLookahead(Value);
			Value.QueryBounds = RequiredBounds(Value);
			// A stable coarse envelope lets a moving Character finish a paged sweep.
			// Per-page candidates and retained demand are revalidated at each update.
			const auto Slack = std::min(Limits.EnterRadius, 512.0f);
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				Value.QueryBounds.Minimum[Axis] -= Slack;
				Value.QueryBounds.Maximum[Axis] += Slack;
			}
		}

		bool Covers(const PackageCoarseBounds &Outer, const PackageCoarseBounds &Inner) const {
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				if (Inner.Minimum[Axis] < Outer.Minimum[Axis] || Inner.Maximum[Axis] > Outer.Maximum[Axis])
					return false;
			return true;
		}

		std::optional<Candidate> CandidateFor(const Focus &Value, std::size_t Unit, bool WasDesired) const {
			if (Unit >= Manifest.Entries.size()) return std::nullopt;
			const auto &Entry = Manifest.Entries[Unit];
			if (!Entry.CoarseBounds || Entry.PackageSpaceKey != DefaultPackageSpace) return std::nullopt;
			const float Current = DistanceSquared(Value.Position, *Entry.CoarseBounds);
			const float Ahead = SegmentDistanceSquared(Value.Position, Value.Lookahead, *Entry.CoarseBounds);
			const float Enter = Limits.EnterRadius * Limits.EnterRadius;
			const float Leave = Limits.LeaveRadius * Limits.LeaveRadius;
			if (Current > Enter && Ahead > Enter && (!WasDesired || Current > Leave)) return std::nullopt;
			Candidate Result{.Index = Unit, .Class = Current <= Enter ? Value.Class : ResidencyDemandClass::Predictive,
				.DistanceSquared = std::min(Current, Ahead), .CostBytes = Entry.UncompressedBytes,
				.Revision = Value.Revision};
			return Result;
		}

		void Retain(std::map<std::size_t, Candidate> &Candidates, std::set<RankKey> &Ranks,
			const Candidate &Result) {
			if (auto Found = Candidates.find(Result.Index); Found != Candidates.end()) {
				if (Better(Result, Found->second)) {
					Ranks.erase(Rank(Found->second));
					Found->second = Result;
					Ranks.insert(Rank(Result));
					Metrics.RankingOperations += 2;
				}
				return;
			}
			if (Candidates.size() < Limits.MaximumCandidatesPerFocus) {
				Candidates.emplace(Result.Index, Result);
				Ranks.insert(Rank(Result));
				++Metrics.RankingOperations;
				return;
			}
			if (!Ranks.empty() && Rank(Result) < *Ranks.rbegin()) {
				const auto WorstUnit = std::get<5>(*Ranks.rbegin());
				Ranks.erase(std::prev(Ranks.end()));
				Candidates.erase(WorstUnit);
				Candidates.emplace(Result.Index, Result);
				Ranks.insert(Rank(Result));
				Metrics.RankingOperations += 2;
			}
		}

		void Consider(Focus &Value, std::set<RankKey> &Ranks, std::size_t Unit) {
			if (const auto CandidateValue = CandidateFor(Value, Unit, Value.Committed.contains(Unit)))
				Retain(Value.Working, Ranks, *CandidateValue);
		}

		void Revalidate(Focus &Value, ResidencyFocusId Id, std::uint64_t Tick) {
			ResolveLookahead(Value);
			if (Value.Cursor && Covers(Value.QueryBounds, RequiredBounds(Value)))
				Value.MovedDuringSweep = true;
			else if (Value.Cursor) {
				++Metrics.StaleRevisions;
				Value.Cursor.reset();
				Value.Working.clear();
				Value.MovedDuringSweep = false;
			}
			for (auto Current = Value.Committed.begin(); Current != Value.Committed.end();) {
				const auto CandidateValue = CandidateFor(Value, Current->first, true);
				++Metrics.CandidateRevalidations;
				if (!CandidateValue) {
					if (Value.Attached) ChangeContribution(Id, Current->second, false, Tick);
					Current = Value.Committed.erase(Current);
				} else {
					Current->second = *CandidateValue;
					if (Value.Attached) ChangeContribution(Id, Current->second, true, Tick);
					++Current;
				}
			}
			for (auto Current = Value.Working.begin(); Current != Value.Working.end();) {
				const auto CandidateValue = CandidateFor(Value, Current->first, Value.Committed.contains(Current->first));
				++Metrics.CandidateRevalidations;
				if (!CandidateValue) Current = Value.Working.erase(Current);
				else {
					Current->second = *CandidateValue;
					++Current;
				}
			}
			Value.NeedsRevalidation = false;
		}

		void Complete(Focus &Value, ResidencyFocusId Id, std::uint64_t Tick) {
			auto Replacement = std::move(Value.Working);
			if (Value.MovedDuringSweep) {
				++Metrics.MovingSweepCompletions;
				std::set<RankKey> Ranks;
				for (const auto &[Unit, CandidateValue] : Replacement) Ranks.insert(Rank(CandidateValue));
				Metrics.RankingOperations += Replacement.size();
				for (const auto &[Unit, CandidateValue] : Value.Committed)
					Retain(Replacement, Ranks, CandidateValue);
			}
			if (Value.Attached)
				for (const auto &[Unit, CandidateValue] : Value.Committed)
					if (!Replacement.contains(Unit)) ChangeContribution(Id, CandidateValue, false, Tick);
			for (const auto &[Unit, CandidateValue] : Replacement)
				ChangeContribution(Id, CandidateValue, true, Tick);
			Value.Committed = std::move(Replacement);
			Value.Working.clear();
			Value.Cursor.reset();
			Value.NeedsEvaluation = Value.MovedDuringSweep;
			Value.MovedDuringSweep = false;
			Value.Attached = true;
			Value.Unhealthy = false;
			++Metrics.CompletedFocusEvaluations;
		}

		void Evaluate(std::uint64_t Tick) {
			if (Focuses.empty()) return;
			std::size_t CellsLeft = Limits.MaximumQueryCellsPerTick;
			std::size_t VisitsLeft = Limits.MaximumMembershipVisitsPerTick;
			std::size_t FocusesLeft = Limits.MaximumFocusEvaluationsPerTick;
			std::size_t RevalidationsLeft = Limits.MaximumCandidateRevalidationsPerTick;
			auto Cursor = Focuses.upper_bound(NextFocus);
			if (Cursor == Focuses.end()) Cursor = Focuses.begin();
			std::size_t Inspected = 0;
			while (Inspected < Focuses.size() && FocusesLeft && CellsLeft && VisitsLeft) {
				if (Cursor == Focuses.end()) Cursor = Focuses.begin();
				auto Current = Cursor++;
				++Inspected;
				if (!Current->second.NeedsEvaluation) continue;
				if (Current->second.Unhealthy && Tick < Current->second.RetryAtTick) continue;
				auto &Value = Current->second;
				if (Value.NeedsRevalidation) {
					const auto Work = Value.Committed.size() + Value.Working.size();
					if (Work > RevalidationsLeft) break;
					RevalidationsLeft -= Work;
					Revalidate(Value, Current->first, Tick);
				}
				// Deferred revalidation must not move the rotation past work that
				// has not received an opportunity under the remaining tick budget.
				NextFocus = Current->first;
				if (!Value.Cursor) BuildBounds(Value);
				const PackageCoarseQueryLimits QueryLimits{
					std::min(CellsLeft, MaximumPackageCoarseQueryCells),
					std::min(VisitsLeft, MaximumPackageCoarseQueryMembershipVisits),
					std::min(VisitsLeft, Limits.MaximumCandidatesPerFocus),
				};
				const auto Page = Index.QueryPage(DefaultPackageSpace, Value.QueryBounds, QueryLimits, Value.Cursor);
				++Metrics.QueryPages;
				Metrics.QueryCellsVisited += Page.CellsVisited;
				Metrics.QueryMembershipVisits += Page.MembershipVisits;
				CellsLeft -= std::min(CellsLeft, Page.CellsVisited);
				VisitsLeft -= std::min(VisitsLeft, Page.MembershipVisits);
				--FocusesLeft;
				if (Page.Status == PackageCoarseQueryStatus::Complete || Page.Status == PackageCoarseQueryStatus::More) {
					std::set<RankKey> Ranks;
					for (const auto &[Unit, CandidateValue] : Value.Working) Ranks.insert(Rank(CandidateValue));
					Metrics.RankingOperations += Value.Working.size();
					for (const auto Unit : Page.Entries) Consider(Value, Ranks, Unit);
					if (Page.Status == PackageCoarseQueryStatus::Complete) Complete(Value, Current->first, Tick);
					else Value.Cursor = Page.NextCursor;
				} else {
					++Metrics.QueryFailures;
					Value.Working.clear();
					Value.Cursor.reset();
					Value.NeedsEvaluation = true;
					Value.MovedDuringSweep = false;
					Value.Unhealthy = true;
					Value.RetryAtTick = Tick > std::numeric_limits<std::uint64_t>::max() -
						std::max<std::uint64_t>(1, Limits.GraceTicks)
						? std::numeric_limits<std::uint64_t>::max()
						: Tick + std::max<std::uint64_t>(1, Limits.GraceTicks);
				}
			}
		}

		void Expire(std::uint64_t Tick) {
			std::size_t Left = Limits.MaximumDemandDeltasPerTick;
			while (Left-- && !Expiries.empty() && Expiries.begin()->first <= Tick) {
				const auto Unit = Expiries.begin()->second;
				Expiries.erase(Expiries.begin());
				if (auto Found = Demands.find(Unit); Found != Demands.end() && Found->second.GraceUntil)
					Refresh(Unit, Tick);
			}
		}

		void Apply(ContentAvailabilityService *Availability) {
			if (!Availability || !Availability->IsManifestAvailable()) return;
			if (BoundAvailability && Availability != BoundAvailability) {
				++Metrics.ServiceBindingFailures;
				return;
			}
			const auto CurrentManifest = Availability->GetManifestSnapshot();
			if (!CurrentManifest || (BoundManifest && CurrentManifest != BoundManifest)) {
				++Metrics.ServiceBindingFailures;
				return;
			}
			if (!BoundAvailability && CurrentManifest != ManifestOwner) {
				if (ExactSnapshotRequired) {
					++Metrics.ServiceBindingFailures;
					return;
				}
				if (CurrentManifest->Package != Manifest.Package ||
					CurrentManifest->InstanceSchemaVersion != Manifest.InstanceSchemaVersion ||
					CurrentManifest->Entries.size() != Manifest.Entries.size()) {
					++Metrics.ServiceBindingFailures;
					return;
				}
				for (std::size_t Unit = 0; Unit < Manifest.Entries.size(); ++Unit) {
					++Metrics.ManifestBindingEntriesCompared;
					const auto &Left = Manifest.Entries[Unit];
					const auto &Right = CurrentManifest->Entries[Unit];
					if (Left.Key != Right.Key || Left.BlobReference != Right.BlobReference ||
						Left.Digest != Right.Digest || Left.CompressedBytes != Right.CompressedBytes ||
						Left.UncompressedBytes != Right.UncompressedBytes || Left.ObjectCount != Right.ObjectCount ||
						Left.Dependencies != Right.Dependencies || Left.PackageSpaceKey != Right.PackageSpaceKey ||
						Left.Flags != Right.Flags || Left.CoarseBounds.has_value() != Right.CoarseBounds.has_value()) {
						++Metrics.ServiceBindingFailures;
						return;
					}
					if (Left.CoarseBounds && (Left.CoarseBounds->Minimum != Right.CoarseBounds->Minimum ||
						Left.CoarseBounds->Maximum != Right.CoarseBounds->Maximum)) {
						++Metrics.ServiceBindingFailures;
						return;
					}
				}
			}
			if (!BoundAvailability) {
				BoundAvailability = Availability;
				BoundManifest = CurrentManifest;
			}
			std::size_t FrontValidationLeft = std::max<std::size_t>(
				1, Limits.MaximumRequestValidationVisitsPerTick / 2);
			std::size_t ContinuationValidationLeft =
				Limits.MaximumRequestValidationVisitsPerTick - FrontValidationLeft;
			std::vector<PendingDelta> Requests;
			std::vector<PendingDelta> Releases;
			auto InspectRequest = [&](const PendingDelta &Delta, std::size_t &ValidationLeft) {
				auto Found = Demands.find(Delta.Index);
				if (Found == Demands.end()) return;
				std::optional<Candidate> CurrentBest;
				if (Found->second.Required) {
					CurrentBest = Candidate{.Index = Delta.Index, .Class = Found->second.Required->Class,
						.DeadlineTick = Found->second.Required->DeadlineTick,
						.CostBytes = Manifest.Entries[Delta.Index].UncompressedBytes,
						.Revision = Found->second.Required->Revision};
				}
				for (const auto &[RankValue, Id] : Found->second.RankedFocuses) {
					(void)RankValue;
					if (!ValidationLeft) break;
					--ValidationLeft;
					++Metrics.RequestValidationVisits;
					const auto FocusValue = Focuses.find(Id);
					const auto CandidateValue = Found->second.Focuses.find(Id);
					if (FocusValue == Focuses.end() || CandidateValue == Found->second.Focuses.end() ||
						FocusValue->second.NeedsRevalidation ||
						FocusValue->second.Revision != CandidateValue->second.Revision) continue;
					if (!CurrentBest || Better(CandidateValue->second, *CurrentBest))
						CurrentBest = CandidateValue->second;
					break;
				}
				if (!CurrentBest) return;
				PendingDelta Fresh{false, CurrentBest->Class,
					CurrentBest->DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max()),
					CurrentBest->DistanceSquared, CurrentBest->CostBytes, Delta.Key, Delta.Index};
				Requests.push_back(std::move(Fresh));
			};
			const auto SearchLimit = 8 * Limits.MaximumDemandDeltasPerTick;
			auto RequestCursor = Pending.begin();
			std::optional<PendingDelta> LastFront;
			for (std::size_t Inspected = 0;
				Inspected < SearchLimit && RequestCursor != Pending.end() && !RequestCursor->Release;
				++Inspected, ++RequestCursor) {
				LastFront = *RequestCursor;
				InspectRequest(*RequestCursor, FrontValidationLeft);
			}
			// The front is authoritative for ordinary priority. Only if every
			// front request is stale or blocked do we search a bounded continuation.
			if (Requests.empty() && LastFront && ContinuationValidationLeft) {
				if (!PendingScanAfter) PendingScanAfter = LastFront;
				auto Cursor = Pending.upper_bound(*PendingScanAfter);
				for (std::size_t Inspected = 0; Inspected < SearchLimit; ++Inspected) {
					if (Cursor == Pending.end() || Cursor->Release) Cursor = Pending.begin();
					if (Cursor == Pending.end() || Cursor->Release) break;
					const auto Delta = *Cursor++;
					PendingScanAfter = Delta;
					InspectRequest(Delta, ContinuationValidationLeft);
					if (!ContinuationValidationLeft) break;
				}
			}
			PendingDelta ReleaseBoundary;
			ReleaseBoundary.Release = true;
			ReleaseBoundary.Class = ResidencyDemandClass::Critical;
			ReleaseBoundary.DeadlineTick = 0;
			ReleaseBoundary.DistanceSquared = 0.0f;
			auto ReleaseCursor = Pending.lower_bound(ReleaseBoundary);
			for (std::size_t Inspected = 0;
				Inspected < SearchLimit && ReleaseCursor != Pending.end() && ReleaseCursor->Release;
				++Inspected, ++ReleaseCursor) Releases.push_back(*ReleaseCursor);
			std::sort(Requests.begin(), Requests.end());
			std::sort(Releases.begin(), Releases.end());
			std::size_t RequestIndex = 0;
			std::size_t ReleaseIndex = 0;
			std::size_t Left = Limits.MaximumDemandDeltasPerTick;
			while (Left && (RequestIndex < Requests.size() || ReleaseIndex < Releases.size())) {
				const bool UseRelease = ReleaseIndex < Releases.size() &&
					(RequestIndex == Requests.size() || NextRelease);
				const auto Delta = UseRelease ? Releases[ReleaseIndex++] : Requests[RequestIndex++];
				NextRelease = !UseRelease;
				auto Found = Demands.find(Delta.Index);
				if (Found == Demands.end()) {
					RemovePending(Delta.Index);
					continue;
				}
				const bool Accepted = Delta.Release ? Availability->ReleaseContent(Delta.Key)
					: Availability->RequestContent(Delta.Key);
				if (!Accepted) {
					++Metrics.RequestDeferrals;
					break;
				}
				Found->second.Requested = !Delta.Release;
				if (Delta.Release) ++Metrics.Releases;
				else ++Metrics.Requests;
				RemovePending(Delta.Index);
				if (Delta.Release) Refresh(Delta.Index, LastTick);
				--Left;
			}
		}
	};

	ResidencyDemandPolicy::ResidencyDemandPolicy(
		const PackageContentManifest &Manifest, ResidencyDemandPolicyLimits Limits
	) : State(std::make_unique<Impl>(Manifest, Limits)) {}

	ResidencyDemandPolicy::ResidencyDemandPolicy(
		std::shared_ptr<const PackageContentManifest> Manifest, ResidencyDemandPolicyLimits Limits
	) : State(std::make_unique<Impl>(std::move(Manifest), Limits)) {}

	ResidencyDemandPolicy::~ResidencyDemandPolicy() = default;

	bool ResidencyDemandPolicy::UpdateFocus(
		ResidencyFocusId Id, glm::vec3 Position, glm::vec3 AcceptedVelocity, ResidencyDemandClass Class
	) {
		if (!State || State->Stopped || Id == 0 || !ValidClass(Class) || !Finite(Position) || !Finite(AcceptedVelocity) ||
			std::abs(Position.x) > MaximumFocusCoordinate || std::abs(Position.y) > MaximumFocusCoordinate ||
			std::abs(Position.z) > MaximumFocusCoordinate || State->Revision == std::numeric_limits<std::uint64_t>::max())
			return false;
		auto Found = State->Focuses.find(Id);
		if (Found == State->Focuses.end() && State->Focuses.size() >= State->Limits.MaximumFocuses) return false;
		if (Found != State->Focuses.end() && Found->second.Position == Position &&
			Found->second.Velocity == AcceptedVelocity && Found->second.Class == Class) return true;
		auto &Value = State->Focuses[Id];
		Value.Position = Position;
		Value.Velocity = AcceptedVelocity;
		Value.Class = Class;
		Value.Revision = ++State->Revision;
		Value.NeedsRevalidation = true;
		Value.NeedsEvaluation = true;
		Value.Unhealthy = false;
		Value.RetryAtTick = 0;
		return true;
	}

	bool ResidencyDemandPolicy::RemoveFocus(ResidencyFocusId Id) {
		if (!State || State->Stopped) return false;
		auto Found = State->Focuses.find(Id);
		if (Found == State->Focuses.end()) return false;
		if (Found->second.Attached)
			for (const auto &[Unit, CandidateValue] : Found->second.Committed)
				State->ChangeContribution(Id, CandidateValue, false, State->LastTick);
		State->Focuses.erase(Found);
		return true;
	}

	bool ResidencyDemandPolicy::SetRequirement(
		std::string_view Key, ResidencyDemandClass Class, std::optional<std::uint64_t> DeadlineTick
	) {
		if (!State || State->Stopped || !ValidClass(Class) || State->Revision == std::numeric_limits<std::uint64_t>::max())
			return false;
		const auto Entry = std::lower_bound(State->Manifest.Entries.begin(), State->Manifest.Entries.end(), Key,
			[](const PackageContentEntry &Left, std::string_view Right) { return Left.Key < Right; });
		if (Entry == State->Manifest.Entries.end() || Entry->Key != Key) return false;
		const auto Unit = static_cast<std::size_t>(Entry - State->Manifest.Entries.begin());
		auto &Record = State->Demands[Unit];
		if (Record.Required && Record.Required->Class == Class && Record.Required->DeadlineTick == DeadlineTick) return true;
		Record.Required = Impl::Requirement{Class, DeadlineTick, ++State->Revision};
		State->Refresh(Unit, State->LastTick);
		return true;
	}

	bool ResidencyDemandPolicy::ClearRequirement(std::string_view Key) {
		if (!State || State->Stopped) return false;
		const auto Entry = std::lower_bound(State->Manifest.Entries.begin(), State->Manifest.Entries.end(), Key,
			[](const PackageContentEntry &Left, std::string_view Right) { return Left.Key < Right; });
		if (Entry == State->Manifest.Entries.end() || Entry->Key != Key) return false;
		const auto Unit = static_cast<std::size_t>(Entry - State->Manifest.Entries.begin());
		auto Found = State->Demands.find(Unit);
		if (Found == State->Demands.end() || !Found->second.Required) return false;
		Found->second.Required.reset();
		State->Refresh(Unit, State->LastTick);
		return true;
	}

	void ResidencyDemandPolicy::Step(ContentAvailabilityService *Availability, std::uint64_t Tick) {
		if (!State || State->Stopped || Tick == 0 || Tick <= State->LastTick) return;
		State->LastTick = Tick;
		State->Evaluate(Tick);
		State->Expire(Tick);
		State->Apply(Availability);
	}

	void ResidencyDemandPolicy::Stop(ContentAvailabilityService *Availability) {
		if (!State || State->Stopped) return;
		State->Stopped = true;
		if (Availability && Availability == State->BoundAvailability &&
			Availability->GetManifestSnapshot() == State->BoundManifest)
			for (const auto &[Unit, Record] : State->Demands)
				if (Record.Requested) (void)Availability->ReleaseContent(State->Manifest.Entries[Unit].Key);
		State->Focuses.clear();
		State->Demands.clear();
		State->Expiries.clear();
		State->Pending.clear();
		State->PendingByIndex.clear();
	}

	ResidencyDemandPolicyMetrics ResidencyDemandPolicy::GetMetrics() const {
		if (!State) return {};
		auto Result = State->Metrics;
		Result.Focuses = State->Focuses.size();
		Result.DemandRecords = State->Demands.size();
		Result.PendingDeltas = State->Pending.size();
		for (const auto &[Id, Value] : State->Focuses) {
			(void)Id;
			Result.PendingFocuses += Value.NeedsEvaluation;
			Result.UnhealthyFocuses += Value.Unhealthy;
			Result.RetainedCandidates += Value.Committed.size() + Value.Working.size();
		}
		return Result;
	}

	std::vector<ResidencyDemandSnapshot> ResidencyDemandPolicy::GetDemands() const {
		std::vector<ResidencyDemandSnapshot> Result;
		if (!State) return Result;
		Result.reserve(State->Demands.size());
		for (const auto &[Unit, Record] : State->Demands) {
			const bool Desired = !Record.Focuses.empty() || Record.Required.has_value();
			const auto Best = State->Best(Unit, Record);
			Result.push_back({State->Manifest.Entries[Unit].Key, Best.Class, Best.DeadlineTick,
				Best.DistanceSquared, Best.CostBytes, Best.Revision, Desired, Record.Requested});
		}
		std::ranges::sort(Result, [](const ResidencyDemandSnapshot &Left, const ResidencyDemandSnapshot &Right) {
			if (Left.Desired != Right.Desired) return Left.Desired;
			if (Left.Class != Right.Class) return Left.Class < Right.Class;
			const auto LeftDeadline = Left.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max());
			const auto RightDeadline = Right.DeadlineTick.value_or(std::numeric_limits<std::uint64_t>::max());
			if (LeftDeadline != RightDeadline) return LeftDeadline < RightDeadline;
			if (Left.DistanceSquared != Right.DistanceSquared) return Left.DistanceSquared < Right.DistanceSquared;
			if (Left.CostBytes != Right.CostBytes) return Left.CostBytes < Right.CostBytes;
			return Left.Key < Right.Key;
		});
		return Result;
	}
}
