#pragma once

// Test-only observer. Never used for acceptance or rate policy. One selected
// recipient gets detailed byte attribution; aggregate counters cover all peers.
#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/BinaryCodec.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace gargantuan::test {
struct StructuralBytesFixture;
inline thread_local StructuralBytesFixture *ActiveStructuralBytes = nullptr;
struct StructuralBytesFixture {
	using Frame = network::ReplicationFrame;
	StructuralBytesFixture *Previous = ActiveStructuralBytes;
	network::ConnectionId Selected;
	std::string Phase;
	std::vector<double> Frames, Operations, BytesPerOperation, PublishGroups, RemovalGroups;
	std::array<std::uint64_t, 1201> TickBytes{}, TickReliable{}, TickEncoded{};
	std::array<std::uint64_t, 9> Components{};
	std::uint64_t PreviousEncoded = 0;
	std::size_t Tick = 0;
	std::uint64_t AllFrames = 0, AllBytes = 0, AllOperations = 0, UnclassifiedGroups = 0;
	bool Enabled;
	explicit StructuralBytesFixture(bool Enable, network::ConnectionId Peer) : Selected(Peer), Enabled(Enable) {
		if (!Enabled) return;
		for (auto *Values : {&Frames, &Operations, &BytesPerOperation, &PublishGroups, &RemovalGroups}) Values->reserve(32768);
		ActiveStructuralBytes = this;
	}
	~StructuralBytesFixture() { if (Enabled) ActiveStructuralBytes = Previous; }
	void Begin(std::string_view Name) {
		Phase = Name; Tick = 0; AllFrames = AllBytes = AllOperations = UnclassifiedGroups = 0;
		TickBytes.fill(0); TickReliable.fill(0); TickEncoded.fill(0); Components.fill(0);
		for (auto *Values : {&Frames, &Operations, &BytesPerOperation, &PublishGroups, &RemovalGroups}) Values->clear();
	}
	void Advance() { if (Enabled && ++Tick >= TickBytes.size()) throw std::runtime_error("byte fixture tick bound"); }
	static void Add(std::vector<double> &Values, double Value) {
		if (Values.size() == 32768) throw std::runtime_error("byte fixture sample bound");
		Values.push_back(Value);
	}
	static std::size_t MapBytes(const std::map<std::string, WireValue> &Values) {
		network::GameBinaryWriter Writer;
		for (const auto &[Name, Value] : Values) { Writer.String(Name); network::WriteBinaryWireValue(Writer, Value); }
		return Writer.Bytes.size();
	}
	void Reliable(std::size_t Bytes) { if (Enabled) TickReliable.at(Tick) += Bytes; }
	void Encoded(std::uint64_t Bytes) { if (Enabled) { TickEncoded.at(Tick) = Bytes - PreviousEncoded; PreviousEncoded = Bytes; } }
	void Observe(const Frame &Value, std::size_t Bytes, network::ConnectionId Peer) {
		++AllFrames; AllBytes += Bytes; AllOperations += Value.Operations.size(); TickBytes.at(Tick) += Bytes;
		Add(Frames, static_cast<double>(Bytes)); Add(Operations, static_cast<double>(Value.Operations.size()));
		if (!Value.Operations.empty()) Add(BytesPerOperation, static_cast<double>(Bytes) / Value.Operations.size());
		if (Peer != Selected) return;
		std::size_t Accounted = 36 + 21 * Value.Schema.size(); Components[0] += Accounted;
		std::size_t RemovalBytes = 0;
		for (const auto &Operation : Value.Operations) {
			Frame Single{network::ReplicationProtocolVersion, network::ReplicationMessageKind::Incremental, Value.Epoch, Value.Sequence};
			Single.Operations.push_back(Operation);
			const auto Encoded = network::EncodeReplicationFrame(Single);
			if (!Encoded) throw std::runtime_error("byte fixture single-operation encoding failed");
			const auto Size = Encoded->size() - 36; Accounted += Size;
			if (const auto *Publish = std::get_if<network::PublishReplication>(&Operation.Intent)) {
				const auto Property = MapBytes(Publish->Properties), Attribute = MapBytes(Publish->Attributes);
				std::size_t Extension = 0, Custom = 0, Tags = 0;
				for (const auto &State : Publish->Extensions) Extension += 20 + 4 + MapBytes(State.Properties);
				for (const auto &State : Publish->CustomProperties) Custom += 20 + 4 + MapBytes(State.Properties);
				for (const auto &Tag : Publish->Tags) Tags += 4 + Tag.size();
				Components[1] += Size - Property - Attribute - Extension - Custom - Tags - 4 - Publish->Name.size();
				Components[8] += 4 + Publish->Name.size();
				Components[2] += Property; Components[3] += Attribute; Components[4] += Extension;
				Components[5] += Custom; Components[6] += Tags;
				// Single-operation encoding after the parent is already accepted.
				// Not an observation of the planner's actual group partition: the
				// first child may have selected its still-unknown parent with it.
				const bool Reference = std::any_of(Publish->Properties.begin(), Publish->Properties.end(),
					[](const auto &Item) { return std::holds_alternative<WireObjectReference>(Item.second); });
				if (Publish->Name.starts_with("ContentScale") && !Reference && Publish->Extensions.empty() && Publish->CustomProperties.empty())
					Add(PublishGroups, static_cast<double>(Size + 36));
				else ++UnclassifiedGroups;
			} else {
				Components[7] += Size;
				if (std::holds_alternative<network::DestroyReplication>(Operation.Intent) ||
					std::holds_alternative<network::UnpublishReplication>(Operation.Intent)) RemovalBytes += Size;
			}
		}
		// Conservative whole-frame removal envelope, NOT a generic inferred
		// KI-007 group decomposition. Kept separate from flat Enter samples.
		if (RemovalBytes) Add(RemovalGroups, static_cast<double>(Bytes));
		if (Accounted != Bytes) throw std::runtime_error("byte attribution disagrees with production codec");
	}
	static double Quantile(std::vector<double> Values, double Fraction) {
		if (Values.empty()) return -1;
		std::sort(Values.begin(), Values.end()); return Values[static_cast<std::size_t>((Values.size() - 1) * Fraction)];
	}
	void End() const {
		if (!Enabled) return;
		auto Print = [&](std::string_view Name, const std::vector<double> &Values) {
			std::cout << "[Network:StructuralBytes] phase=" << Phase << " metric=" << Name << " count=" << Values.size()
				<< " p50=" << Quantile(Values,.5) << " p95=" << Quantile(Values,.95) << " p99=" << Quantile(Values,.99)
				<< " max=" << Quantile(Values,1) << '\n';
		};
		Print("frame",Frames); Print("operations",Operations); Print("bytesPerOperation",BytesPerOperation);
		Print("flatEnterGroup",PublishGroups); Print("removalFrame",RemovalGroups);
		auto PrintTicks = [&](std::string_view Name, const auto &Samples) {
			std::vector<double> Values; Values.reserve(Tick);
			for (std::size_t I = 1; I <= Tick; ++I) Values.push_back(static_cast<double>(Samples[I]));
			Print(Name, Values);
		};
		PrintTicks("allPeersReceiveStructuralPerTick", TickBytes);
		PrintTicks("allPeersReceiveReliablePerTick", TickReliable);
		PrintTicks("allPeersEncodedStructuralPerTick", TickEncoded);
		std::size_t Burst = 0, MaximumBurst = 0;
		for (std::size_t I = 0; I <= Tick; ++I) { Burst = TickBytes[I] ? Burst + 1 : 0; MaximumBurst = std::max(MaximumBurst, Burst); }
		std::cout << "[Network:StructuralByteTotals] phase=" << Phase << " frames=" << AllFrames << " bytes=" << AllBytes
			<< " operations=" << AllOperations << " maxConsecutiveReceiveTicks=" << MaximumBurst << " unclassifiedGroups=" << UnclassifiedGroups;
		constexpr std::array Names{"headerSchema", "publishIdentityCounts", "properties", "attributes", "extensions", "custom", "tags", "otherOperations", "publishNames"};
		for (std::size_t I = 0; I < Names.size(); ++I) std::cout << ' ' << Names[I] << '=' << Components[I];
		std::cout << '\n';
	}
};
}
