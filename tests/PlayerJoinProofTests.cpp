#include "host/player/JoinProof.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
	constexpr auto Environment = "GARGANTUAN_PLAYER_JOIN_PROOF_TEST";

	void SetValue(const std::optional<std::string> &Value) {
#if defined(_WIN32)
		const auto Result = _putenv_s(Environment, Value ? Value->c_str() : "");
#else
		const auto Result = Value ? setenv(Environment, Value->c_str(), 1) : unsetenv(Environment);
#endif
		if (Result != 0) throw std::runtime_error("test environment unavailable");
	}

	struct EnvironmentLifetime {
		std::optional<std::string> Previous;
		EnvironmentLifetime() {
			if (const auto *Value = std::getenv(Environment)) Previous = Value;
		}
		~EnvironmentLifetime() {
			try { SetValue(Previous); } catch (...) {}
		}
	};
}

int main() {
	EnvironmentLifetime EnvironmentOwner;
	try {
		const auto Require = [](bool Condition, const char *Label) {
			if (!Condition) throw std::runtime_error(Label);
		};
		SetValue(std::nullopt);
		Require(!gargantuan::host::detail::ReadPlayerJoinProof(Environment), "missing source accepted");
		const std::string Valid = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
		SetValue(Valid);
		const auto Proof = gargantuan::host::detail::ReadPlayerJoinProof(Environment);
		Require(Proof && Proof->size() == 32 && Proof->front() == std::byte{0x01} &&
			Proof->back() == std::byte{0xef}, "exact proof was corrupted");
		const auto Key = gargantuan::host::detail::ReadPlayerServerPublicKey(Environment);
		Require(Key && *Key == *Proof, "exact public server key was corrupted");
		for (const auto &Invalid : {std::string(), Valid.substr(1), Valid + "0",
				std::string(64, '0'), std::string(64, 'A'), std::string(64, 'x')}) {
			SetValue(Invalid);
			Require(!gargantuan::host::detail::ReadPlayerJoinProof(Environment), "malformed proof accepted");
			Require(!gargantuan::host::detail::ReadPlayerServerPublicKey(Environment), "malformed server key accepted");
		}
		SetValue(Valid);
		for (const auto &InvalidName : {std::string(), std::string(65, 'A'), std::string("1INVALID"),
				std::string("INVALID-NAME"), std::string("INVALID=NAME"), std::string("INVALID\0TAIL", 12)})
			Require(!gargantuan::host::detail::ReadPlayerJoinProof(InvalidName), "malformed source name accepted");
		std::cout << "[Network:Admission] Player join proof boundary passed.\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:Admission] " << Error.what() << '\n';
		return 1;
	}
}
