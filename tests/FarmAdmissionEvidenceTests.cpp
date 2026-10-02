#include "host/server/FarmAdmissionEvidence.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
	void Check(bool Value, const char *Message) {
		if (!Value) throw std::runtime_error(Message);
	}
	std::string Read(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}
}

int main() {
	using gargantuan::host::detail::FarmAdmissionEvidence;
	using gargantuan::network::detail::AdmissionEvidenceEvent;
	using gargantuan::network::detail::AdmissionEvidenceKind;
	using gargantuan::network::detail::ActiveAdmissionEvidence;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-fairness-evidence-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	try {
		Check(std::filesystem::create_directory(Root), "new isolated evidence directory");
		const auto Path = Root / "admission-fairness.tsv";
		{
			FarmAdmissionEvidence Evidence("12345678-1234-4234-8234-123456789abc", Path);
			Check(ActiveAdmissionEvidence != nullptr, "farm sink installed");
			ActiveAdmissionEvidence->Record(ActiveAdmissionEvidence->Context, AdmissionEvidenceEvent{
				.Kind = AdmissionEvidenceKind::ExactDemand, .Connection = {1, 1},
				.DemandId = 1, .ExactBytes = 524'288, .AtMicroseconds = 250'000,
				.ExactCandidateFingerprint = {3, 5},
			});
			Check(Evidence.EventsSince(0).size() == 1 &&
				Evidence.EventsSince(0).front().ExactCandidateFingerprint == std::array<std::uint64_t, 2>{3, 5} &&
				Evidence.EventsSince(1).empty(), "in-memory audit preserves the exact encoded identity");
			Check(!std::filesystem::exists(Path), "callback performs no hot-path file I/O");
			Evidence.Dump();
			Check(Evidence.Valid() && Evidence.Count() == 1 && Evidence.BytesWritten() > 64 &&
				Evidence.BytesWritten() < FarmAdmissionEvidence::FileLimit(), "bounded new file completed");
			Check(ActiveAdmissionEvidence == nullptr, "sink removed before file dump completes");
		}
		const auto Original = Read(Path);
		Check(Original.find("format=GargantuanAdmissionEvidenceV1\trun=12345678-1234-4234-8234-123456789abc\n") == 0 &&
			Original.find("event\texact_demand\tnone\t1\t1\t1\t") != std::string::npos &&
			Original.ends_with("end\t1\t0\n"), "typed file and complete trailer");
		bool RejectedExisting = false;
		try { FarmAdmissionEvidence Duplicate("12345678-1234-4234-8234-123456789abc", Path); }
		catch (const std::invalid_argument &) { RejectedExisting = true; }
		Check(RejectedExisting && Read(Path) == Original, "existing evidence cannot be overwritten");
		bool RejectedName = false;
		try { FarmAdmissionEvidence WrongName("12345678-1234-4234-8234-123456789abc", Root / "other.tsv"); }
		catch (const std::invalid_argument &) { RejectedName = true; }
		Check(RejectedName, "only fixed evidence filename is accepted");
		std::filesystem::remove(Path);
		{
			FarmAdmissionEvidence Evidence("12345678-1234-4234-8234-123456789abc", Path);
			for (std::uint64_t Index = 0; Index <= FarmAdmissionEvidence::EventLimit(); ++Index)
				ActiveAdmissionEvidence->Record(ActiveAdmissionEvidence->Context, AdmissionEvidenceEvent{
					.Kind = AdmissionEvidenceKind::ExactDemand, .Connection = {1, 1},
					.DemandId = Index + 1, .ExactBytes = 77, .AtMicroseconds = Index + 1,
				});
			Evidence.Dump();
			Check(!Evidence.Valid() && Evidence.Overflowed() && !Evidence.Failed() &&
				Evidence.Count() == FarmAdmissionEvidence::EventLimit() &&
				Evidence.BytesWritten() <= FarmAdmissionEvidence::FileLimit(),
				"event overflow is bounded and invalidates qualification");
		}
		Check(Read(Path).ends_with("end\t65536\t1\n"), "overflow marker is explicit in retained file");
		std::filesystem::remove(Path);
		std::filesystem::remove(Root);
		std::cout << "[Qualification:Admission] file_tests=PASS\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Qualification:Admission] file_tests=FAIL " << Error.what() << '\n';
		return 1;
	}
}
