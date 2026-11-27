#include "gargantuan/packaging/RuntimePackageCrypto.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
	using namespace gargantuan;
	using namespace std::literals;
	using Status = RuntimePackageCryptoStatus;
	using Use = RuntimePackageProvisioningUse;

	constexpr auto SeedHex = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"sv;
	constexpr auto PublicHex = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"sv;
	constexpr auto ContentHex = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"sv;
	constexpr auto Manifest = "{\"Publisher\":\"Research\"}\n"sv;
	constexpr auto Bytecode = "bytecode\0payload"sv;
	constexpr auto AssociatedData = "project=Project\nrelease=Release\npublisher=Publisher\nscript=Script42\ncompiler=9.3\npolicy=Hybrid\n"sv;
	constexpr RuntimePackageKeyRequest KeyRequest{"Project", "Release", "Publisher", "Key"};

	void Require(bool Condition, std::string_view Description) {
		if (!Condition) throw std::runtime_error(std::string(Description));
	}

	template <std::size_t Size>
	std::array<unsigned char, Size> FromHex(std::string_view Hex) {
		const auto Digit = [](char Character) {
			return Character >= 'a' ? Character - 'a' + 10 : Character - '0';
		};
		Require(Hex.size() == Size * 2, "test vector length");
		std::array<unsigned char, Size> Result{};
		for (std::size_t Index = 0; Index < Size; ++Index)
			Result[Index] = static_cast<unsigned char>(Digit(Hex[Index * 2]) * 16 + Digit(Hex[Index * 2 + 1]));
		return Result;
	}

	void TestManifests() {
		const RuntimePackageSecret Seed(FromHex<32>(SeedHex));
		const RuntimePackageSecret WrongSeed(FromHex<32>(ContentHex));
		const auto Expected = FromHex<64>("9f8c9ec1ed5de01bbaec6a2def45f059d4a9cbf786ce61692f193f4762cbdd358"
			"31b6b73f3f0b7920203cd72a0cabe24d38ff846a3e809a1b48eacfddd26f60c");
		RuntimePackageSignature Signature{};
		RuntimePackagePublicKey Public{};
		Require(GetRuntimePackagePublicKey(Seed, Public) == Status::Ok && Public == FromHex<32>(PublicHex), "Ed25519 public vector");
		Require(SignRuntimePackageManifest(Manifest, Seed, Signature) == Status::Ok && Signature == Expected, "Ed25519 domain vector");
		Require(VerifyRuntimePackageManifest(Manifest, Public, Signature) == Status::Ok, "signature verifies");
		Require(VerifyRuntimePackageManifest(Manifest.substr(0, Manifest.size() - 1), Public, Signature) == Status::AuthenticationFailed,
			"exact manifest bytes required");
		Require(VerifyRuntimePackageManifest(Manifest, Public, std::span(Signature).first(63)) == Status::InvalidInput,
			"truncated signature rejected");
		RuntimePackagePublicKey WrongPublic{};
		Require(GetRuntimePackagePublicKey(WrongSeed, WrongPublic) == Status::Ok, "derive distinct publisher key");
		Require(VerifyRuntimePackageManifest(Manifest, WrongPublic, Signature) == Status::AuthenticationFailed, "untrusted signing key rejected");
		Signature[0] ^= 1;
		Require(VerifyRuntimePackageManifest(Manifest, Public, Signature) == Status::AuthenticationFailed, "changed signature rejected");
		const std::string Oversized(MaximumRuntimePackageSignedManifestBytes + 1, 'x');
		Require(SignRuntimePackageManifest(Oversized, Seed, Signature) == Status::InvalidInput && Signature == RuntimePackageSignature{},
			"oversized signing transcript rejected without output");
		Require(VerifyRuntimePackageManifest(Oversized, Public, Expected) == Status::InvalidInput, "oversized verification transcript rejected");
	}

	void TestProtectedContainers() {
		std::array<unsigned char, 32> ReleaseSalt{};
		std::array<unsigned char, 32> OtherReleaseSalt{};
		Require(GenerateRuntimePackageReleaseSalt(ReleaseSalt) == Status::Ok &&
			GenerateRuntimePackageReleaseSalt(OtherReleaseSalt) == Status::Ok && ReleaseSalt != OtherReleaseSalt,
			"protected releases have independent random salts");
		const RuntimePackageSecret Key(FromHex<32>(ContentHex));
		const RuntimePackageSecret WrongKey(FromHex<32>(SeedHex));
		const RuntimePackageSecret Unavailable;
		RuntimePackagePlaintext Plaintext;
		const auto KnownAnswer = FromHex<RuntimePackageScriptHeaderBytes + Bytecode.size() + RuntimePackageScriptTagBytes>(
			"4752533110000000202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b"
			"4c8c1649a32f9c513cd5ad59249df1581c9e3da0386d1b03342db2a2041c54c4");
		const std::string_view KnownContainer(reinterpret_cast<const char *>(KnownAnswer.data()), KnownAnswer.size());
		Require(OpenRuntimePackageScript(KnownContainer, AssociatedData, Key, Plaintext) == Status::Ok && Plaintext.View() == Bytecode,
			"HKDF/AES-GCM independent known answer");
		std::string Container;
		std::string Second;
		Require(SealRuntimePackageScript(Bytecode, AssociatedData, Key, Container) == Status::Ok, "seal bytecode");
		Require(SealRuntimePackageScript(Bytecode, AssociatedData, Key, Second) == Status::Ok && Container != Second,
			"repeated sealing creates a new envelope");
		Require(Container.find(Bytecode) == std::string::npos, "plaintext absent from protected container");
		Require(OpenRuntimePackageScript(Container, AssociatedData, Key, Plaintext) == Status::Ok && Plaintext.View() == Bytecode,
			"protected bytecode roundtrip");
		Require(OpenRuntimePackageScript(Container, AssociatedData, WrongKey, Plaintext) == Status::AuthenticationFailed && Plaintext.View().empty(),
			"incorrect key clears prior plaintext");
		Require(OpenRuntimePackageScript(Container, AssociatedData, Unavailable, Plaintext) == Status::KeyUnavailable && Plaintext.View().empty(),
			"missing key reported distinctly");
		for (const auto Field : {"Project", "Release", "Publisher", "Script42", "9.3", "Hybrid"}) {
			std::string Changed(AssociatedData);
			const auto Position = Changed.find(Field);
			Changed[Position] ^= 1;
			Require(OpenRuntimePackageScript(Container, Changed, Key, Plaintext) == Status::AuthenticationFailed && Plaintext.View().empty(),
				"protected script rejects context substitution");
		}
		for (const auto Position : {std::size_t(8), std::size_t(40), RuntimePackageScriptHeaderBytes, Container.size() - 1}) {
			auto Altered = Container;
			Altered[Position] ^= 1;
			Require(OpenRuntimePackageScript(Altered, AssociatedData, Key, Plaintext) == Status::AuthenticationFailed && Plaintext.View().empty(),
				"salt, nonce, ciphertext and tag are authenticated");
		}
		for (const auto Position : {0U, 3U, 4U, 7U}) {
			auto Altered = Container;
			Altered[Position] ^= 1;
			Require(OpenRuntimePackageScript(Altered, AssociatedData, Key, Plaintext) == Status::InvalidInput && Plaintext.View().empty(),
				"invalid header or size rejected before plaintext allocation");
		}
		for (const auto Size : {std::size_t(0), RuntimePackageScriptHeaderBytes, Container.size() - 1})
			Require(OpenRuntimePackageScript(std::string_view(Container).substr(0, Size), AssociatedData, Key, Plaintext) == Status::InvalidInput,
				"truncated framing rejected");
		Require(OpenRuntimePackageScript(Container + 'x', AssociatedData, Key, Plaintext) == Status::InvalidInput, "trailing bytes rejected");
		Require(OpenRuntimePackageScript(Bytecode, AssociatedData, Key, Plaintext) == Status::InvalidInput, "clear fallback rejected");
		Require(SealRuntimePackageScript(Bytecode, {}, Key, Second) == Status::InvalidInput && Second.empty(), "AAD required");
		const std::string OversizedAad(MaximumRuntimePackageAssociatedDataBytes + 1, 'x');
		Require(SealRuntimePackageScript(Bytecode, OversizedAad, Key, Second) == Status::InvalidInput, "AAD bounded on seal");
		Require(OpenRuntimePackageScript(Container, OversizedAad, Key, Plaintext) == Status::InvalidInput, "AAD bounded on open");
		const std::string Large(MaximumRuntimePackageScriptBytecodeBytes, 'x');
		Require(SealRuntimePackageScript(Large, AssociatedData, Key, Container) == Status::Ok, "maximum bytecode can be sealed");
		Require(OpenRuntimePackageScript(Container, AssociatedData, Key, Plaintext) == Status::Ok && Plaintext.View() == Large,
			"maximum bytecode can be opened");
		Require(OpenRuntimePackageScript(Container + 'x', AssociatedData, Key, Plaintext) == Status::InvalidInput && Plaintext.View().empty(),
			"oversized encrypted frame rejected");
		Require(SealRuntimePackageScript(Large + 'x', AssociatedData, Key, Container) == Status::InvalidInput && Container.empty(),
			"oversized plaintext rejected");
	}

	void TestProvisioning() {
		const RuntimePackageKeyScope Scope{"Project", "Release", "Publisher", "Key"};
		auto Key = RuntimePackageSecret(FromHex<32>(ContentHex));
		const auto Provider = MakeProvisionedRuntimePackageKeyProvider(Scope, std::move(Key));
		Require(Provider && !Key.IsAvailable() && std::ranges::all_of(Key.View(), [](auto Byte) { return Byte == 0; }),
			"move clears original secret");
		Require(Provider->GetKey(KeyRequest, Key) == Status::Ok && Key.IsAvailable(), "provisioned key is scoped");
		Require(Provider->GetKey({"Wrong", "Release", "Publisher", "Key"}, Key) == Status::KeyUnavailable && !Key.IsAvailable(),
			"project mismatch clears key output");
		Require(Provider->GetKey({"Project", "OtherRelease", "Publisher", "Key"}, Key) == Status::KeyUnavailable, "release scoped key");
		Require(Provider->GetKey({"Project", "Release", "WrongPublisher", "Key"}, Key) == Status::KeyUnavailable, "publisher scoped key");
		Require(Provider->GetKey({"Project", "Release", "Publisher", "OtherKey"}, Key) == Status::KeyUnavailable, "exact key id");
		Require(Provider->GetKey({"Project", "Release", "Publisher", "../secret"}, Key) == Status::InvalidInput, "identifier cannot select path");
		Require(!MakeProvisionedRuntimePackageKeyProvider(Scope, RuntimePackageSecret{}), "unavailable secret cannot be enrolled");
		Require(LoadProvisionedRuntimePackageSecurity("relative-enrollment", Use::Runtime).Status == Status::InvalidInput,
			"enrollment requires absolute operator path");

		struct TestDirectory final {
			const std::filesystem::path Path = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
				("gargantuan-crypto-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			TestDirectory() { Require(std::filesystem::create_directory(Path), "create owned enrollment fixture directory"); }
			~TestDirectory() { std::error_code Ignored; std::filesystem::remove_all(Path, Ignored); }
		} Folder;
		const auto File = Folder.Path / "enrollment";
		const auto Write = [&](std::string_view Value) {
			std::ofstream Output(File, std::ios::binary | std::ios::trunc);
			Output.write(Value.data(), static_cast<std::streamsize>(Value.size()));
			Output.close();
			Require(static_cast<bool>(Output), "write enrollment fixture");
			std::filesystem::permissions(File, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
		};
		const auto Header = "GargantuanRuntimePackageEnrollmentV1\r\nmode\tHost\r\n"s;
		const auto Trust = "trust\tProject\t*\tPublisher\tKey\t"s + std::string(PublicHex) + "\r\n";
		const auto RotatedTrust = "trust\tProject\tRelease\tPublisher\tRotated\t"s + std::string(ContentHex) + "\r\n";
		const auto Content = "content\tProject\tRelease\tPublisher\tKey\t"s + std::string(ContentHex) + "\r\n";
		const auto Signer = "signer\tProject\tRelease\tPublisher\tKey\t"s + std::string(SeedHex) + "\r\n";
		Write(Header + Trust + RotatedTrust + Content);
		const auto Runtime = LoadProvisionedRuntimePackageSecurity(File, Use::Runtime);
		Require(Runtime.Status == Status::Ok && Runtime.Mode == RuntimePackageProvisioningMode::Host && Runtime.Trust && Runtime.Keys &&
			!Runtime.Signer, "runtime trusted enrollment");
		RuntimePackagePublicKey KeyBytes{};
		Require(Runtime.Trust->GetPublicKey({"Project", "NextRelease", "Publisher", "Key"}, KeyBytes) == Status::Ok &&
			KeyBytes == FromHex<32>(PublicHex), "publisher project trust across releases is explicit");
		Require(Runtime.Trust->GetPublicKey({"Project", "Release", "Publisher", "Rotated"}, KeyBytes) == Status::Ok, "rotated key can coexist");
		Require(Runtime.Trust->GetPublicKey({"Project", "Release", "DifferentPublisher", "Key"}, KeyBytes) == Status::KeyUnavailable,
			"lookups cannot select untrusted publisher");
		Require(Runtime.Keys->GetKey(KeyRequest, Key) == Status::Ok && std::ranges::equal(Key.View(), FromHex<32>(ContentHex)), "read content key");
		std::error_code LinkError;
		const auto Link = Folder.Path / "link";
		std::filesystem::create_symlink(File, Link, LinkError);
		if (!LinkError)
			Require(LoadProvisionedRuntimePackageSecurity(Link, Use::Runtime).Status != Status::Ok, "symlinked enrollment rejected");
		Write(Header + Trust + Signer);
		Require(LoadProvisionedRuntimePackageSecurity(File, Use::Runtime).Status == Status::InvalidInput, "runtime enrollment rejects signer material");
		const auto Build = LoadProvisionedRuntimePackageSecurity(File, Use::Build);
		Require(Build.Status == Status::Ok && Build.Signer && Build.Trust, "build separate signer enrollment");
		Write(Header + Trust + Trust);
		Require(LoadProvisionedRuntimePackageSecurity(File, Use::Build).Status == Status::InvalidInput, "duplicate trust rows rejected");
		Write(Header + Trust + "trust\tProject\tRelease\tPublisher\tKey\t" + std::string(PublicHex));
		Require(LoadProvisionedRuntimePackageSecurity(File, Use::Build).Status == Status::InvalidInput, "overlapping trusted scope rejected");
		Write(Header + Content + "malformed\n");
		const auto Invalid = LoadProvisionedRuntimePackageSecurity(File, Use::Runtime);
		Require(Invalid.Status == Status::InvalidInput && !Invalid.Keys, "malformed rows do not publish a partial provider");
	#ifndef _WIN32
		Write(Header + Content);
		std::filesystem::permissions(File, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
		Require(LoadProvisionedRuntimePackageSecurity(File, Use::Runtime).Status == Status::InvalidInput, "private enrollment permissions required");
	#endif
	}

	void TestDisabledCrypto() {
		const RuntimePackageSecret Secret(FromHex<32>(SeedHex));
		RuntimePackageSignature Signature{};
		RuntimePackagePublicKey Public{};
		RuntimePackagePlaintext Plaintext;
		std::string Container = "previous result";
		std::array<unsigned char, 32> ReleaseSalt{};
		Require(GenerateRuntimePackageReleaseSalt(ReleaseSalt) == Status::Unsupported && ReleaseSalt == std::array<unsigned char, 32>{},
			"disabled release randomness fails closed");
		Require(GetRuntimePackagePublicKey(Secret, Public) == Status::Unsupported, "disabled public key derivation fails closed");
		Require(SignRuntimePackageManifest(Manifest, Secret, Signature) == Status::Unsupported, "disabled signing fails closed");
		Require(VerifyRuntimePackageManifest(Manifest, Public, Signature) == Status::Unsupported, "disabled verification fails closed");
		Require(SealRuntimePackageScript(Bytecode, AssociatedData, Secret, Container) == Status::Unsupported && Container.empty(),
			"disabled encryption fails closed");
		Require(OpenRuntimePackageScript(Bytecode, AssociatedData, Secret, Plaintext) == Status::Unsupported && Plaintext.View().empty(),
			"disabled decryption fails closed");
	}
}

int main() {
	try {
		TestProvisioning();
		if (IsRuntimePackageCryptoAvailable()) {
			TestManifests();
			TestProtectedContainers();
		} else {
			TestDisabledCrypto();
		}
		std::cout << "[Package:Crypto] Passed runtime package crypto and provisioning tests\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:Crypto] " << Error.what() << '\n';
		return 1;
	}
}
