#pragma once

#include "../src/host/server/FarmD03FirstFault.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <source_location>

inline void TestFarmD03FirstFault() {
	using namespace gargantuan;
	using Evidence = host::detail::FarmD03FirstFault;
	using Word = Evidence::Word;
	using Record = network::detail::PooledServiceRecord;
	using Ledger = network::detail::PooledReliableServiceFeedback;
	auto Require = [](bool Value, std::source_location Site = std::source_location::current()) {
		if (!Value) throw std::runtime_error("first D03 fault regression at line " + std::to_string(Site.line()));
	};
	struct Scratch {
		std::filesystem::path Path = std::filesystem::temp_directory_path() /
			("gargantuan-d03-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Scratch() { if (!std::filesystem::create_directory(Path)) throw std::runtime_error("D03 test directory exists"); }
		~Scratch() { std::error_code Error; std::filesystem::remove_all(Path, Error); }
	} Directory;
	unsigned Case = 0;
	auto Path = [&] {
		auto Parent = Directory.Path / std::to_string(++Case);
		std::filesystem::create_directory(Parent);
		return Parent / "first-execution-fault.bin";
	};
	auto Read = [&](const std::filesystem::path &File, const Evidence::Snapshot &Expected) {
		std::ifstream Stream(File, std::ios::binary);
		const std::string Bytes{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
		const auto HeaderEnd = Bytes.find('\n');
		Require(HeaderEnd != std::string::npos && Bytes.substr(0, HeaderEnd).find("clock=steady_us\trecord_bytes=792") != std::string::npos);
		Require(Bytes.size() == HeaderEnd + 1 + Evidence::RecordBytes && Bytes.size() < 4096);
		for (std::size_t I = 0; I < Expected.Words.size(); ++I) {
			std::uint64_t Value = 0;
			for (unsigned J = 0; J < 8; ++J)
				Value |= static_cast<std::uint64_t>(static_cast<unsigned char>(Bytes[HeaderEnd + 1 + I * 8 + J])) << (J * 8);
			Require(Value == Expected.Words[I]);
		}
	};
	auto Complete = [&](Ledger &State, std::uint64_t At) {
		Record R;
		R.Connection = {13, 7}; R.NowMicroseconds = At; R.SimulationTick = 42;
		R.Point = network::detail::PooledObservationPoint::AdmissionComplete;
		R.ExecutionBefore = network::detail::CapturePooledExecution(State);
		R.ExecutionRejected = !State.CompleteAdmissionOpportunity(At, true);
		R.Result = State.Observe(R.Connection, {}, {}, At, 0, 0);
		R.InvalidAfter = State.Invalid;
		return R;
	};
	for (const auto Elapsed : {100000ULL, 100001ULL}) {
		Ledger State;
		Require(State.BeginAdmissionOpportunity(1000, true));
		auto R = Complete(State, 1000 + Elapsed);
		const auto File = Path(); Evidence Capture(File, "main-bound");
		Capture.MarkStage(Evidence::Stage::Qualifier, 41, 100);
		Capture.MarkStage(Evidence::Stage::Sleep, 41, 100500);
		Capture.MarkStage(Evidence::Stage::Poll, 42, 100600);
		Capture.MarkStage(Evidence::Stage::Engine, 42, 100700);
		Capture.MarkStage(Evidence::Stage::Session, 42, 100800);
		Capture.SetClock({99, 77, R.NowMicroseconds * 1000, 100, 102, 10000000, true});
		Require(Capture.Persist(R));
		Require(Capture.HasFault() == (Elapsed == 100001));
		Require(R.Result.MainOpportunityFailed == (Elapsed == 100001) && !R.Result.TurnoverFailed);
		if (Elapsed == 100000) { Require(!Capture.FaultPersisted()); continue; }
		const auto Saved = Capture.FirstFault();
		Require(Saved.Get(Word::RequiredSince) == 1000 && Saved.Get(Word::MainElapsed) == Elapsed &&
			Saved.Get(Word::MainElapsedValid) && Saved.Get(Word::MainFailed) && !Saved.Get(Word::TurnoverFailed));
		Require(Saved.Get(Word::ConnectionSlot) == 13 && Saved.Get(Word::ConnectionGeneration) == 7 &&
			Saved.Get(Word::HostStage) == static_cast<unsigned>(Evidence::Stage::Session) &&
			Saved.Get(Word::LastHostStage) == static_cast<unsigned>(Evidence::Stage::Engine) &&
			Saved.Get(Word::LastHostStageStarted) == 100700 && Saved.Get(Word::LastHostStageCompleted) == 100800 &&
			Saved.Get(Word::NativeClockValid) && Saved.Get(Word::Process) == 99 && Saved.Get(Word::NativeThread) == 77);
		const auto Qualifier = static_cast<std::size_t>(Word::CompletedStagesStart) + static_cast<std::size_t>(Evidence::Stage::Qualifier) * 4;
		Require(Saved.Words[Qualifier] == static_cast<unsigned>(Evidence::Stage::Qualifier) &&
			Saved.Words[Qualifier + 1] == 41 && Saved.Words[Qualifier + 2] == 100 &&
			Saved.Words[Qualifier + 3] == 100500); // Later Sleep/Poll/Engine cannot erase the preceding long qualifier.
		// A later fault, reused connection generation and teardown cannot replace the initiating owner.
		R.Connection = {2, 999}; R.NowMicroseconds += 500000; R.Point = network::detail::PooledObservationPoint::PeerTeardown;
		Capture.MarkStage(Evidence::Stage::Cleanup, 99, R.NowMicroseconds);
		Capture.SetClock({1, 2, 3, 4, 5, 6, true});
		Require(Capture.Persist(R) && Capture.FirstFault().Words == Saved.Words);
		Capture.Close(); Read(File, Saved);
	}
	for (const bool Retired : {false, true}) for (const auto Elapsed : {150000ULL, 150001ULL}) {
		Ledger State;
		Require(State.BeginAdmissionOpportunity(1000, true));
		Require(State.PublishOffer(77, 256, 1000));
		if (Retired) {
			State.Offer.reset(); State.PendingTurnover = Ledger::Turnover{77, 1000, false};
		}
		Require(State.CompleteAdmissionOpportunity(51000, true));
		Require(State.CompleteAdmissionOpportunity(101000, true));
		if (Retired) State.PendingTurnover->PermitAvailable = true;
		auto R = Complete(State, 1000 + Elapsed);
		const auto File = Path(); Evidence Capture(File, "turnover-bound");
		Require(Capture.Persist(R) && Capture.HasFault() == (Elapsed == 150001));
		Require(!R.Result.MainOpportunityFailed && R.Result.TurnoverFailed == (Elapsed == 150001));
		if (Elapsed == 150000) continue;
		const auto Saved = Capture.FirstFault();
		Require(Saved.Get(Word::MainElapsed) == 50001 && Saved.Get(Word::TurnoverFailed));
		Require(Saved.Get(Retired ? Word::TurnoverToken : Word::OfferToken) == 77 &&
			Saved.Get(Retired ? Word::TurnoverAt : Word::OfferAt) == 1000 &&
			Saved.Get(Retired ? Word::TurnoverElapsed : Word::OfferElapsed) == Elapsed);
		Capture.Close(); Read(File, Saved);
	}
	for (const bool Backwards : {false, true}) {
		Ledger State; Require(State.BeginAdmissionOpportunity(1000, true));
		if (!Backwards) State.Invalid = true;
		auto R = Complete(State, Backwards ? 999 : 1100);
		const auto File = Path(); Evidence Capture(File, "state-invalid");
		Require(Capture.Persist(R) && Capture.HasFault());
		const auto &Saved = Capture.FirstFault();
		Require(Saved.Get(Word::InvalidAfter) && !Saved.Get(Word::ResultValid) &&
			!Saved.Get(Word::MainFailed) && !Saved.Get(Word::TurnoverFailed));
		Require(Saved.Get(Word::BoundaryBackwards) == Backwards && Saved.Get(Word::MainBackwards) == Backwards &&
			Saved.Get(Word::MainElapsedValid) == !Backwards && Saved.Get(Word::BeforeInvalid) == !Backwards);
		Capture.Close(); Read(File, Saved);
	}
	{
		const auto File = Path(); Evidence Capture(File, "no-teardown-owner");
		Record R; R.Result.ExecutionFailed = true; R.Point = network::detail::PooledObservationPoint::PeerTeardown;
		Require(Capture.Persist(R) && !Capture.HasFault());
		Capture.Close();
		std::ifstream Stream(File, std::ios::binary); std::string Header;
		std::getline(Stream, Header); Require(Stream.peek() == std::char_traits<char>::eof());
	}
	{
		const auto File = Path(); Evidence Capture(File, "closed-writer");
		Capture.Close(); Ledger State; Require(State.BeginAdmissionOpportunity(1000, true));
		auto R = Complete(State, 101001);
		Require(!Capture.Persist(R) && Capture.HasFault() && Capture.Failed() && !Capture.FaultPersisted());
		Require(!Capture.Persist(R)); // No retry/replacement after a failed persistence attempt.
	}
	{
		const auto File = Path(); Evidence Capture(File, "exclusive"); bool Rejected = false;
		try { Evidence Duplicate(File, "exclusive"); } catch (const std::runtime_error &) { Rejected = true; }
		Require(Rejected);
	}
}
