#pragma once
#include "../src/host/common/FarmPublicationEvidence.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

inline void TestFarmOrdinaryEvidence() {
#if defined(_WIN32)
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
		("farm-ordinary-" + std::to_string(runtime_detail::PublicationLatencyNow()));
	if (!std::filesystem::create_directory(Root)) throw std::runtime_error("farm traffic fixture directory is not new");
	const auto Path = Root / "publication-service-0.bin";
	try {
		{
			host::detail::FarmPublicationEvidence Evidence(false, "traffic-test", 0, 17, Path, {1, 1}, 3);
			std::ostringstream Clock; Evidence.WriteTrafficClock(Clock);
			if (Clock.str().find("contract=sender_qpc_v1") == std::string::npos ||
				Clock.str().find("frequency=0") != std::string::npos)
				throw std::runtime_error("farm sender QPC domain missing");
			Evidence.SetTrafficPhase("baseline");
			runtime_detail::RecordPublicationLatency({.Stage="OrdinaryReliableSent", .Connection={1, 1},
				.Kind=2, .Bytes=212, .Operations=1});
			Evidence.SetTrafficPhase("baseline", true);
			Evidence.SetTrafficPhase("baseline", true); // Retained phase cannot reopen during overload.
			Evidence.Dump();
			if (!Evidence.Valid() || Evidence.Count() != 3) throw std::runtime_error("farm bounded traffic metadata lost");
		}
		std::ifstream Input(Path, std::ios::binary);
		std::string Header; std::getline(Input, Header);
		std::array<unsigned char, 240> Bytes{};
		Input.read(reinterpret_cast<char *>(Bytes.data()), Bytes.size());
		auto Integer = [&](std::size_t Offset, std::size_t Width) {
			std::uint64_t Value=0;
			for (std::size_t I=0; I<Width; ++I) Value |= static_cast<std::uint64_t>(Bytes[Offset+I]) << (I*8);
			return Value;
		};
		if (Input.gcount() != static_cast<std::streamsize>(Bytes.size()) || Integer(0,2) != 26 || Integer(2,2) != 1 ||
			Integer(80,2) != 24 || Integer(82,2) != 1 || Integer(100,4) != 212 ||
			Integer(160,2) != 26 || Integer(162,2) != 0 || !Integer(72,8) ||
			Integer(72,8) > Integer(152,8) || Integer(152,8) > Integer(232,8))
			throw std::runtime_error("farm complete-byte/phase/QPC encoding differs");
	} catch (...) { std::filesystem::remove_all(Root); throw; }
	std::filesystem::remove_all(Root);
#endif
}
