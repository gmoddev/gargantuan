#pragma once

#include "../src/network/BoundedReplicationEncoding.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gargantuan::test {

inline bool RunBoundedReplicationEncodingTests() {
	using namespace network;
	using runtime_detail::WorkCounter;
	try {
		auto Require = [](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
		};
		auto Counter = [](const runtime_detail::WorkSample &Sample, WorkCounter Value) {
			return Sample.Counters[static_cast<std::size_t>(Value)];
		};
		auto MakeFrame = [](std::size_t Count, std::size_t StringBytes) {
			ReplicationFrame Frame{ReplicationProtocolVersion, ReplicationMessageKind::Incremental,
				ReplicationEpoch(1), ReliableReplicationSequence(2)};
			for (std::size_t Index = 0; Index < Count; ++Index)
				Frame.Operations.push_back({ReplicationEpoch(1), PropertyReplicationUpdate{
					{1, 2}, "Name", std::string(StringBytes, 'a')}});
			return Frame;
		};
		std::size_t Cases = 0;
		for (const auto [Count, Bytes] : std::array<std::pair<std::size_t, std::size_t>, 6>{
			{{0, 0}, {1, 0}, {1, 77}, {1, MaximumProtocolStringBytes}, {16, 24 * 1024}, {32, 24 * 1024}}}) {
			const auto Frame = MakeFrame(Count, Bytes);
			const auto Reference = EncodeReplicationFrame(Frame);
			Require(Reference.has_value(), "bounded fixture has a valid globally encodable frame");
			for (const auto Limit : {std::size_t{0}, std::size_t{1}, std::size_t{35}, std::size_t{36},
				Reference->size() - 1, Reference->size(), Reference->size() + 1,
				std::size_t{512 * 1024 - 32}, std::numeric_limits<std::size_t>::max()}) {
				const auto Encoded = network::detail::EncodeReplicationFrameBounded(Frame, Limit);
				if (Reference->size() <= Limit) {
					Require(Encoded && *Encoded == *Reference, "bounded success must preserve every canonical GRPL byte");
					Require(DecodeReplicationFrame(*Encoded).has_value(), "bounded success remains canonically decodable");
				} else {
					Require(!Encoded && Encoded.error().Code == SerializationErrorCode::LimitExceeded &&
						Encoded.error().Message == "Replication frame exceeds its byte limit",
						"hard bound retains the exact existing geometric-retry error");
				}
				++Cases;
			}
		}
		// Invalid data is deliberately last, after a prefix much larger than the
		// requested cap. Validation must not be short-circuited by writer failure.
		for (const auto &Invalid : std::array<std::string, 4>{std::string{"\xc0\xaf", 2},
			std::string{"a\0b", 3}, std::string{"\xed\xa0\x80", 3}, std::string(MaximumProtocolStringBytes + 1, 'x')}) {
			auto Frame = MakeFrame(32, 24 * 1024);
			std::get<PropertyReplicationUpdate>(Frame.Operations.back().Intent).Value = Invalid;
			const auto Reference = EncodeReplicationFrame(Frame);
			for (const auto Limit : {std::size_t{0}, std::size_t{36}, std::size_t{512 * 1024}}) {
				runtime_detail::WorkSample Sample{};
				runtime_detail::WorkCapture Capture(&Sample);
				const auto Encoded = network::detail::EncodeReplicationFrameBounded(Frame, Limit);
				Require(!Reference && !Encoded && Reference.error().Code == Encoded.error().Code &&
					Reference.error().Message == Encoded.error().Message &&
					Encoded.error().Code == SerializationErrorCode::InvalidValue,
					"late malformed value retains full-frame invalid-data error precedence");
				Require(Counter(Sample, WorkCounter::StructuralEncodePayloadBytes) == 0 &&
					Counter(Sample, WorkCounter::StructuralEncodeLimitFailures) == 0,
					"invalid frame is rejected before writing or size-failure classification");
				++Cases;
			}
		}
		// Both attempts are doomed, but the smaller hard bound must stop copying
		// payload earlier. This tests actual work, never a machine timing threshold.
		const auto Oversized = MakeFrame(129, MaximumProtocolStringBytes);
		runtime_detail::WorkSample ReferenceWork{}, BoundedWork{};
		auto EncodeMeasured = [&](bool Bounded, runtime_detail::WorkSample &Sample) {
			runtime_detail::WorkCapture Capture(&Sample);
			return Bounded ? network::detail::EncodeReplicationFrameBounded(Oversized, 512 * 1024)
				: EncodeReplicationFrame(Oversized);
		};
		const auto Reference = EncodeMeasured(false, ReferenceWork);
		const auto Bounded = EncodeMeasured(true, BoundedWork);
		Require(!Reference && !Bounded && Reference.error().Code == Bounded.error().Code &&
			Reference.error().Message == Bounded.error().Message,
			"oversized frame has the same canonical byte-limit failure");
		Require(Counter(BoundedWork, WorkCounter::StructuralEncodePayloadBytes) <= 512 * 1024 - 36 &&
			Counter(BoundedWork, WorkCounter::StructuralEncodePayloadBytes) <
				Counter(ReferenceWork, WorkCounter::StructuralEncodePayloadBytes) &&
			Counter(BoundedWork, WorkCounter::StructuralEncodeLimitFailures) == 1 &&
			Counter(ReferenceWork, WorkCounter::StructuralEncodeLimitFailures) == 1,
			"bounded encoder eliminates discarded payload writes without weakening validation");
		const auto OverGlobal = network::detail::EncodeReplicationFrameBounded(Oversized, std::numeric_limits<std::size_t>::max());
		Require(!OverGlobal && OverGlobal.error().Code == Reference.error().Code &&
			OverGlobal.error().Message == Reference.error().Message, "private cap cannot enlarge the global wire bound");
		std::cout << "[Network:BoundedEncoding] cases=" << Cases + 2
			<< " bounded_discarded_payload_bytes=" << Counter(BoundedWork, WorkCounter::StructuralEncodePayloadBytes)
			<< " reference_discarded_payload_bytes=" << Counter(ReferenceWork, WorkCounter::StructuralEncodePayloadBytes)
			<< " result=pass\n";
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:BoundedEncoding] " << Error.what() << '\n';
		return false;
	}
}

} // namespace gargantuan::test
