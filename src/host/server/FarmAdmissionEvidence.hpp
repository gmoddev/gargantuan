#pragma once

#include "network/ReliableByteAdmissionDiagnostics.hpp"

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gargantuan::host::detail {
	// A farm-only observation buffer. The GameSession thread only copies fixed
	// event records; file creation and formatting happen after the workload.
	class FarmAdmissionEvidence final {
		class ExclusiveWriter final {
			int Handle = -1;
			std::uint64_t Written = 0;
		  public:
			explicit ExclusiveWriter(const std::filesystem::path &Path) {
#if defined(_WIN32)
			if (_wsopen_s(&Handle, Path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY | _O_NOINHERIT,
				_SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) throw std::runtime_error("fairness evidence file is not new");
#else
			Handle = open(Path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (Handle < 0) throw std::runtime_error("fairness evidence file is not new");
#endif
			}
			~ExclusiveWriter() {
				if (Handle >= 0) {
#if defined(_WIN32)
					_close(Handle);
#else
					close(Handle);
#endif
				}
			}
			ExclusiveWriter(const ExclusiveWriter &) = delete;
			ExclusiveWriter &operator=(const ExclusiveWriter &) = delete;
			void Write(std::string_view Bytes) {
				if (Bytes.size() > MaximumFileBytes - Written) throw std::runtime_error("fairness evidence byte limit exceeded");
				while (!Bytes.empty()) {
#if defined(_WIN32)
					const auto Count = _write(Handle, Bytes.data(), static_cast<unsigned>(Bytes.size()));
#else
					const auto Count = write(Handle, Bytes.data(), Bytes.size());
#endif
					if (Count <= 0) throw std::runtime_error("fairness evidence write failed");
					Bytes.remove_prefix(static_cast<std::size_t>(Count));
					Written += static_cast<std::uint64_t>(Count);
				}
			}
			void Flush() {
#if defined(_WIN32)
				if (_commit(Handle) != 0) throw std::runtime_error("fairness evidence flush failed");
#else
				if (fsync(Handle) != 0) throw std::runtime_error("fairness evidence flush failed");
#endif
			}
			[[nodiscard]] std::uint64_t BytesWritten() const { return Written; }
		};
		static constexpr std::uint64_t MaximumEvents = 65'536;
		static constexpr std::uint64_t MaximumFileBytes = 32ull * 1024 * 1024;
		std::string RunId;
		std::filesystem::path Path;
		std::vector<network::detail::AdmissionEvidenceEvent> Events;
		bool Overflow = false, WriteFailed = false, Dumped = false;
		std::uint64_t FileBytes = 0;
		network::detail::AdmissionEvidenceSink Sink{this, Record};
		network::detail::AdmissionEvidenceSink *Previous = nullptr;
		static const char *Kind(network::detail::AdmissionEvidenceKind Value) noexcept {
			using K = network::detail::AdmissionEvidenceKind;
			switch (Value) {
			case K::ExactDemand: return "exact_demand";
			case K::CreditEligible: return "credit_eligible";
			case K::EligibilityInterrupted: return "eligibility_interrupted";
			case K::GrantAccepted: return "grant_accepted";
			case K::GrantRetired: return "grant_retired";
			case K::GrantReleased: return "grant_released";
			case K::GrantTerminalReleased: return "grant_terminal_released";
			case K::ReservationRolledBack: return "reservation_rolled_back";
			case K::DemandDisposed: return "demand_disposed";
			}
			return "invalid";
		}
		static const char *Reason(network::detail::AdmissionEvidenceReason Value) noexcept {
			using R = network::detail::AdmissionEvidenceReason;
			switch (Value) {
			case R::None: return "none";
			case R::NoWork: return "no_work";
			case R::Unexamined: return "unexamined";
			case R::Replaced: return "replaced";
			case R::Rollback: return "rollback";
			case R::GenerationRemoved: return "generation_removed";
			case R::TerminalRelease: return "terminal_release";
			case R::FeedbackUnavailable: return "feedback_unavailable";
			}
			return "invalid";
		}
		static void Record(void *Context, const network::detail::AdmissionEvidenceEvent &Event) noexcept {
			auto &Self = *static_cast<FarmAdmissionEvidence *>(Context);
			if (Self.Events.size() == MaximumEvents) { Self.Overflow = true; return; }
			try { Self.Events.push_back(Event); } catch (...) { Self.Overflow = true; }
		}
		static std::string Format(const network::detail::AdmissionEvidenceEvent &Event) {
			std::ostringstream Line;
			Line << "event\t" << Kind(Event.Kind) << '\t' << Reason(Event.Reason)
				<< '\t' << Event.Connection.Slot << '\t' << Event.Connection.Generation
				<< '\t' << Event.DemandId << '\t' << Event.EligibilityEpisode
				<< '\t' << Event.GrantToken << '\t' << Event.ExactBytes
				<< '\t' << Event.AtMicroseconds << '\t' << Event.CreditThresholdAtMicroseconds
				<< '\t' << Event.EligibleSinceMicroseconds << '\t' << Event.PeerCreditBytes
				<< '\t' << Event.GlobalCreditBytes << '\t' << Event.ActiveGrants
				<< '\t' << Event.GrantDeferrals << '\t' << Event.FundedDeferrals
				<< '\t' << Event.CreditDeferrals << '\t' << Event.FairnessDeferrals << '\n';
			return Line.str();
		}
	  public:
		// Recovery records share the run-local exclusive writer and hard file
		// bound, but never the admission event schema or its conservation totals.
		template<class WriteRecords>
		void WriteRecovery(std::string_view Case, WriteRecords &&Write) const {
			if (Case != "gameplay" && Case != "structural" && Case != "mixed")
				throw std::invalid_argument("unknown recovery evidence case");
			ExclusiveWriter Output(Path.parent_path() / ("recovery-" + std::string(Case) + ".tsv"));
			Write(Output);
			Output.Flush();
		}
		explicit FarmAdmissionEvidence(std::string Value, std::filesystem::path EvidencePath)
			: RunId(std::move(Value)), Path(std::move(EvidencePath)) {
			if (!Path.is_absolute() || Path.filename() != "admission-fairness.tsv" ||
				!std::filesystem::is_directory(Path.parent_path()) ||
				std::filesystem::is_symlink(Path.parent_path()) || std::filesystem::exists(Path))
				throw std::invalid_argument("farm fairness evidence path must name a new role-local file");
			Events.reserve(MaximumEvents);
			Previous = network::detail::ActiveAdmissionEvidence;
			network::detail::ActiveAdmissionEvidence = &Sink;
		}
		~FarmAdmissionEvidence() { Dump(); network::detail::ActiveAdmissionEvidence = Previous; }
		FarmAdmissionEvidence(const FarmAdmissionEvidence &) = delete;
		FarmAdmissionEvidence &operator=(const FarmAdmissionEvidence &) = delete;
		void Dump() noexcept {
			if (Dumped) return;
			Dumped = true;
			network::detail::ActiveAdmissionEvidence = Previous;
			try {
				ExclusiveWriter Output(Path);
				Output.Write("format=GargantuanAdmissionEvidenceV2\trun=" + RunId + "\n");
				for (const auto &Event : Events) Output.Write(Format(Event));
				Output.Write("end\t" + std::to_string(Events.size()) + "\t" +
					(Overflow ? std::string("1\n") : std::string("0\n")));
				Output.Flush();
				FileBytes = Output.BytesWritten();
			} catch (...) { WriteFailed = true; }
		}
		[[nodiscard]] bool Valid() const { return Dumped && !Overflow && !WriteFailed; }
		// Main-owned qualification audit reads a stable slice immediately after
		// each session step. The returned view must not survive another append.
		[[nodiscard]] std::span<const network::detail::AdmissionEvidenceEvent> EventsSince(
			std::size_t Index) const {
			if (Index > Events.size()) return {};
			return std::span(Events).subspan(Index);
		}
		[[nodiscard]] bool Overflowed() const { return Overflow; }
		[[nodiscard]] bool Failed() const { return WriteFailed; }
		[[nodiscard]] std::uint64_t Count() const { return Events.size(); }
		[[nodiscard]] std::uint64_t BytesWritten() const { return FileBytes; }
		[[nodiscard]] static constexpr std::uint64_t FileLimit() { return MaximumFileBytes; }
		[[nodiscard]] static constexpr std::uint64_t EventLimit() { return MaximumEvents; }
	};
}
