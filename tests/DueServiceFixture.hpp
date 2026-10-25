#pragma once

#include "../src/runtime/PublicationLatencyDiagnostics.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace gargantuan::test {
// Offline analysis of a bounded trace. Called after measured work, never by a
// production scheduling decision. FrameBegin supplies the clock of the actual
// due tick, including work before Character publication in that tick.
struct DueServiceResult {
	std::vector<double> Milliseconds, RootMilliseconds;
	std::vector<double> SnapshotMilliseconds, ProducedMilliseconds, AcceptedMilliseconds, ClientMilliseconds;
	std::vector<double> ForcedMilliseconds;
	std::uint64_t Produced = 0, Suppressed = 0, Missing = 0, Unresolved = 0;
	std::uint64_t AcceptedMissing = 0, Forced = 0;
	std::uint64_t Rescheduled = 0;
	std::uint64_t Retired = 0, RetiredDue = 0;
	std::uint64_t UnknownOrigin = 0, WrongIdentity = 0, Lateness = 0, Future = 0;
	runtime_detail::PublicationLatencyRecord Worst{};
};

inline double ServicePercentile(std::vector<double> Values, double Fraction) {
	if (Values.empty()) return 0;
	std::ranges::sort(Values);
	return Values[static_cast<std::size_t>((Values.size() - 1) * Fraction)];
}

inline DueServiceResult MeasureDueService(std::span<const runtime_detail::PublicationLatencyRecord> Records,
	const std::set<ObjectId> &Roots = {}) {
	using Record = runtime_detail::PublicationLatencyRecord;
	using Relationship = std::pair<network::ConnectionId, ObjectId>;
	using StateKey = std::tuple<network::ConnectionId, ObjectId, std::uint64_t, std::uint64_t, std::uint64_t>;
	using SnapshotKey = std::tuple<ObjectId, std::uint64_t, std::uint64_t>;
	std::map<std::uint64_t, std::uint64_t> Frames;
	std::map<SnapshotKey, Record> Snapshots;
	std::map<StateKey, std::uint64_t> Observed;
	std::map<StateKey, std::uint64_t> Accepted, Client;
	std::set<StateKey> ProducedKeys;
	std::map<Relationship, std::uint64_t> Scheduled, Pending;
	std::set<Relationship> ConfirmedDue;
	DueServiceResult Result;
	double Maximum = -1;
	for (const auto &V : Records) {
		const std::string_view Stage(V.Stage);
		if (Stage == "FrameBegin") Frames.emplace(V.Tick, V.Nanoseconds);
		if (Stage == "StateBuilt") Snapshots[{V.Object, V.Tick, V.Sequence}] = V;
		if (V.Kind == 5 && Stage == "SchedulerAccepted") Accepted.emplace(StateKey{V.Connection, V.Object, V.Due, V.Tick, V.Sequence}, V.Nanoseconds);
		if (V.Kind == 5 && Stage == "ClientHandled") Client.emplace(StateKey{V.Connection, V.Object, V.Due, V.Tick, V.Sequence}, V.Nanoseconds);
		if (Stage == "ObserverState") Observed.emplace(StateKey{V.Connection, V.Object, V.Due, V.Tick, V.Sequence}, V.Nanoseconds);
	}
	for (const auto &V : Records) {
		const std::string_view Stage(V.Stage);
		const Relationship Peer{V.Connection, V.Object};
		if (Stage == "RecipientRetired") {
			Result.Retired += Scheduled.erase(Peer);
			Result.RetiredDue += ConfirmedDue.erase(Peer);
			Pending.erase(Peer);
		} else if (Stage == "FrameBegin") {
			for (const auto &[Key, Due] : Scheduled) if (Due <= V.Tick) Pending.try_emplace(Key, Due);
		} else if (Stage == "CharacterDue") {
			Pending.try_emplace(Peer, V.Due);
			ConfirmedDue.insert(Peer);
		} else if (Stage == "CharacterNextDue") {
			// Importance refresh runs before wheel discovery. It may legitimately
			// move a forecast when the cadence tier changes. Once CharacterDue
			// confirms discovery, rescheduling alone cannot count as service.
			if (V.Due > V.Tick && Pending.contains(Peer) && !ConfirmedDue.contains(Peer)) {
				Pending.erase(Peer); ++Result.Rescheduled;
			}
			Scheduled[Peer] = V.Due;
		} else if (Stage == "CharacterUnchanged") {
			++Result.Suppressed; Pending.erase(Peer); ConfirmedDue.erase(Peer);
		} else if (Stage == "SchedulerAccepted" && V.Kind == 5) {
			// PublishState's forced path can satisfy a scheduled relationship
			// directly without passing through ordinary CharacterProduced.
			if (const auto Due = Pending.find(Peer); Due != Pending.end() && V.Tick >= Due->second) {
				Result.Lateness = std::max(Result.Lateness, V.Tick - Due->second);
				Pending.erase(Due); ConfirmedDue.erase(Peer);
			}
		} else if (Stage == "CharacterProduced") {
			++Result.Produced; Pending.erase(Peer); ConfirmedDue.erase(Peer);
			Result.Lateness = std::max(Result.Lateness, V.Tick >= V.Due ? V.Tick - V.Due : 0);
			const auto Snapshot = Snapshots.find({V.Object, V.Tick, V.Sequence});
			const auto Start = Frames.find(V.Due);
			if (Snapshot == Snapshots.end()) { ++Result.WrongIdentity; continue; }
			const StateKey Key{V.Connection, V.Object, Snapshot->second.Epoch, V.Tick, V.Sequence};
			ProducedKeys.insert(Key);
			const auto End = Observed.find(Key);
			if (End == Observed.end() || !Accepted.contains(Key)) { ++Result.Missing; continue; }
			if (Start == Frames.end() || End->second < Start->second) { ++Result.UnknownOrigin; continue; }
			const double Elapsed = (End->second - Start->second) / 1e6;
			if (Elapsed > Maximum) { Maximum = Elapsed; Result.Worst = V; }
			Result.Milliseconds.push_back(Elapsed);
			Result.SnapshotMilliseconds.push_back((Snapshot->second.Nanoseconds - Start->second) / 1e6);
			Result.ProducedMilliseconds.push_back((V.Nanoseconds - Start->second) / 1e6);
			Result.AcceptedMilliseconds.push_back((Accepted.at(Key) - Start->second) / 1e6);
			if (const auto Handled = Client.find(Key); Handled != Client.end())
				Result.ClientMilliseconds.push_back((Handled->second - Start->second) / 1e6);
			if (Roots.contains(V.Object)) Result.RootMilliseconds.push_back(Elapsed);
		}
	}
	Result.Unresolved = Pending.size();
	for (const auto &[Key, At] : Accepted) {
		const auto Seen = Observed.find(Key);
		Result.AcceptedMissing += Seen == Observed.end();
		if (!ProducedKeys.contains(Key)) {
			++Result.Forced;
			const auto Snapshot = Snapshots.find({std::get<1>(Key), std::get<3>(Key), std::get<4>(Key)});
			if (Snapshot == Snapshots.end() || Snapshot->second.Epoch != std::get<2>(Key)) ++Result.WrongIdentity;
			else if (Seen != Observed.end() && Seen->second >= Snapshot->second.Nanoseconds)
				Result.ForcedMilliseconds.push_back((Seen->second - Snapshot->second.Nanoseconds) / 1e6);
		}
	}
	if (!Frames.empty()) for (const auto &[Key, Due] : Scheduled) Result.Future += Due > Frames.rbegin()->first;
	return Result;
}

// Exact all-interval token-bucket demand: count/bytes minus elapsed refill.
// Packet headers and the GNS adapter allowance are charged once per message.
struct ServiceBucket {
	double Minimum = 0, Required = 0, Total = 0;
	void Add(double Amount, double Seconds, double Rate) {
		Minimum = std::min(Minimum, Total - Rate * Seconds);
		Total += Amount;
		Required = std::max(Required, Total - Rate * Seconds - Minimum);
	}
};

inline bool PrintDueService(std::ostream &Output, std::string_view Phase,
	std::span<const runtime_detail::PublicationLatencyRecord> Records, std::size_t Peers,
	const std::set<ObjectId> &Roots) {
	const auto Result = MeasureDueService(Records, Roots);
	for (const auto &[Name, Samples] : std::initializer_list<std::pair<const char *, const std::vector<double> *>>{
		{"snapshot", &Result.SnapshotMilliseconds}, {"produced", &Result.ProducedMilliseconds},
		{"accepted", &Result.AcceptedMilliseconds}, {"clientHandled", &Result.ClientMilliseconds},
		{"forcedBuiltToObserve", &Result.ForcedMilliseconds}})
		Output << "[Content:ServiceStage] phase=" << Phase << " stage=" << Name << " samples=" << Samples->size()
			<< " p50Ms=" << ServicePercentile(*Samples, .50) << " p95Ms=" << ServicePercentile(*Samples, .95)
			<< " p99Ms=" << ServicePercentile(*Samples, .99) << " maxMs=" << ServicePercentile(*Samples, 1) << '\n';
	for (const bool Root : {false, true}) {
		const auto &Samples = Root ? Result.RootMilliseconds : Result.Milliseconds;
		Output << "[Content:DueService] phase=" << Phase << " root=" << Root << " samples=" << Samples.size()
			<< " p50Ms=" << ServicePercentile(Samples, .50) << " p95Ms=" << ServicePercentile(Samples, .95)
			<< " p99Ms=" << ServicePercentile(Samples, .99) << " maxMs=" << ServicePercentile(Samples, 1)
			<< " produced=" << Result.Produced << " unchanged=" << Result.Suppressed
			<< " missing=" << Result.Missing << " unresolvedDue=" << Result.Unresolved
			<< " acceptedMissing=" << Result.AcceptedMissing << " forced=" << Result.Forced
			<< " forecastRescheduled=" << Result.Rescheduled
			<< " retiredRelationships=" << Result.Retired << " retiredConfirmedDue=" << Result.RetiredDue
			<< " unknownOrigin=" << Result.UnknownOrigin << " identityErrors=" << Result.WrongIdentity
			<< " latenessTicks=" << Result.Lateness << " futureRelationships=" << Result.Future
			<< " worstPeer=" << Result.Worst.Connection.Slot << ':' << Result.Worst.Connection.Generation
			<< " worstObject=" << Result.Worst.Object.Slot << ':' << Result.Worst.Object.Generation
			<< " worstTick=" << Result.Worst.Tick << " worstSequence=" << Result.Worst.Sequence << '\n';
	}
	using Buckets = std::pair<ServiceBucket, ServiceBucket>;
	std::map<std::pair<std::string_view, network::ConnectionId>, Buckets> PeerBuckets;
	std::map<std::string_view, Buckets> GlobalBuckets;
	std::map<std::pair<std::string_view, std::uint32_t>, std::pair<std::uint64_t, std::uint64_t>> Traffic;
	std::map<std::pair<std::string_view, network::ConnectionId>, std::uint64_t> Starts;
	std::map<std::string_view, std::vector<double>> Costs;
	std::map<std::string_view, std::uint64_t> PreviousClientService;
	std::map<std::tuple<ObjectId, std::uint64_t, std::uint64_t, std::uint64_t>, std::set<network::ConnectionId>> Fanout, ForcedFanout;
	std::set<std::tuple<ObjectId, std::uint64_t, std::uint64_t>> RpcPending;
	std::size_t RpcMaximum = 0;
	std::uint32_t MaximumRemoteFrame = 0;
	const double Origin = Records.empty() ? 0 : Records.front().Nanoseconds / 1e9;
	const double MessageRate = Peers <= 32 ? 512 : 1024, ByteRate = Peers <= 32 ? 262144 : 524288;
	for (const auto &V : Records) {
		const std::string_view Stage(V.Stage);
		// This fixture has one actual gameplay client. These are callback-start
		// intervals and semantic handler gaps, distinct from client work cost.
		std::string_view ClientService;
		if (Stage == "ClientBegin") ClientService = "ClientInterval";
		else if (Stage == "ClientHandled" && V.Kind == 405) ClientService = "ClientCharacterGap";
		else if (Stage == "ClientHandled" && V.Kind >= 100 && V.Kind < 200) ClientService = "ClientRemoteGap";
		if (!ClientService.empty()) {
			if (const auto Previous = PreviousClientService.find(ClientService); Previous != PreviousClientService.end())
				Costs[ClientService].push_back((V.Nanoseconds - Previous->second) / 1e6);
			PreviousClientService[ClientService] = V.Nanoseconds;
		}
		for (const auto &[Begin, End] : {std::pair{"ServerBegin", "ServerDone"}, {"ClientBegin", "ClientDone"},
			std::pair{"DrainBegin", "DrainDone"}, {"PaceBegin", "FrameDone"}, {"FrameBegin", "FrameDone"}}) {
			const auto Key = std::pair<std::string_view, network::ConnectionId>{Begin, V.Connection};
			if (Stage == Begin) Starts[Key] = V.Nanoseconds;
			if (Stage == End) if (const auto Start = Starts.find(Key); Start != Starts.end()) {
				Costs[Begin].push_back((V.Nanoseconds - Start->second) / 1e6); Starts.erase(Start);
			}
		}
		if (Stage == "IngressSubmit" && V.Kind == 103) {
			RpcPending.emplace(V.Object, V.Epoch, V.Sequence); RpcMaximum = std::max(RpcMaximum, RpcPending.size());
		}
		if (Stage == "ClientHandled" && V.Kind == 104) RpcPending.erase({V.Object, V.Epoch, V.Sequence});
		if (V.Kind == 5 && (Stage == "EgressSubmit" || Stage == "EgressReliable")) {
			auto &Map = Stage == "EgressSubmit" ? Fanout : ForcedFanout;
			Map[{V.Object, V.Due, V.Tick, V.Sequence}].insert(V.Connection);
		}
		if (V.Kind == 5) continue; // per-state companion, not another packet
		if (Stage == "EgressSubmit" || Stage == "IngressSubmit") {
			if (V.Kind >= 100 && V.Kind < 200) MaximumRemoteFrame = std::max(MaximumRemoteFrame, V.Bytes);
			auto &Count = Traffic[{Stage, V.Kind}]; ++Count.first; Count.second += V.Bytes + 32;
		}
		if ((Stage != "EgressReliable" && Stage != "IngressReliable") || V.Kind == 200) continue;
		const double Seconds = V.Nanoseconds / 1e9 - Origin;
		auto &Peer = PeerBuckets[{Stage, V.Connection}];
		Peer.first.Add(1, Seconds, 64); Peer.second.Add(V.Bytes + 32, Seconds, 32768);
		auto &Global = GlobalBuckets[Stage];
		Global.first.Add(1, Seconds, MessageRate); Global.second.Add(V.Bytes + 32, Seconds, ByteRate);
	}
	bool Qualified = !Result.Milliseconds.empty() && !Result.Missing && !Result.Unresolved && !Result.AcceptedMissing &&
		!Result.UnknownOrigin && !Result.WrongIdentity && RpcMaximum <= 4 && RpcPending.empty() && MaximumRemoteFrame <= 16384;
	Output << "[Content:RpcAccounting] phase=" << Phase << " concurrentMax=" << RpcMaximum
		<< " pending=" << RpcPending.size() << " maxFrame=" << MaximumRemoteFrame << '\n';
	for (const auto &[Name, Samples] : Costs) {
		double Total = 0; for (const auto Sample : Samples) Total += Sample;
		Output << "[Content:ServiceOwner] phase=" << Phase << " owner=" << Name << " samples=" << Samples.size()
			<< " totalMs=" << Total << " p50Ms=" << ServicePercentile(Samples, .5)
			<< " p95Ms=" << ServicePercentile(Samples, .95) << " p99Ms=" << ServicePercentile(Samples, .99)
			<< " maxMs=" << ServicePercentile(Samples, 1) << '\n';
	}
	for (const bool Forced : {false, true}) {
		const auto &Map = Forced ? ForcedFanout : Fanout;
		std::size_t Deliveries = 0, Maximum = 0;
		for (const auto &[Key, Recipients] : Map) { Deliveries += Recipients.size(); Maximum = std::max(Maximum, Recipients.size()); }
		Output << "[Content:Fanout] phase=" << Phase << " forced=" << Forced << " uniqueStates=" << Map.size()
			<< " deliveries=" << Deliveries << " maximumRecipients=" << Maximum << '\n';
	}
	for (const auto &[Key, Count] : Traffic)
		Output << "[Content:Traffic] phase=" << Phase << " direction=" << Key.first << " kind=" << Key.second
			<< " messages=" << Count.first << " bytesWithAdapter=" << Count.second << '\n';
	for (const auto Direction : {"IngressReliable", "EgressReliable"}) {
		double Messages = 0, Bytes = 0;
		for (const auto &[Key, Bucket] : PeerBuckets) if (Key.first == Direction) {
			Messages = std::max(Messages, Bucket.first.Required); Bytes = std::max(Bytes, Bucket.second.Required);
		}
		const auto &Global = GlobalBuckets[Direction];
		const bool Fits = Messages <= 16.00001 && Bytes <= 20480.00001 &&
			Global.first.Required <= (Peers <= 32 ? 128.00001 : 256.00001) &&
			Global.second.Required <= (Peers <= 32 ? 163840.00001 : 327680.00001);
		Qualified &= Fits;
		Output << "[Content:WorkloadBudget] phase=" << Phase << " direction=" << Direction
			<< " peerMessageBurst=" << Messages << " peerByteBurst=" << Bytes
			<< " globalMessageBurst=" << Global.first.Required << " globalByteBurst=" << Global.second.Required
			<< " fits=" << Fits << '\n';
	}
	return Qualified;
}
}
