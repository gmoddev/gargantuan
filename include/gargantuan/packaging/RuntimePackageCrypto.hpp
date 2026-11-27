#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {

	inline constexpr std::size_t MaximumRuntimePackageSignedManifestBytes = 4 * 1024 * 1024;
	inline constexpr std::size_t MaximumRuntimePackageScriptBytecodeBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumRuntimePackageAssociatedDataBytes = 64 * 1024;
	inline constexpr std::size_t RuntimePackageScriptHeaderBytes = 4 + 4 + 32 + 12;
	inline constexpr std::size_t RuntimePackageScriptTagBytes = 16;
	inline constexpr std::size_t MaximumRuntimePackageScriptContainerBytes =
		MaximumRuntimePackageScriptBytecodeBytes + RuntimePackageScriptHeaderBytes + RuntimePackageScriptTagBytes;

	enum class RuntimePackageCryptoStatus : std::uint8_t {
		Ok,
		Unsupported,
		InvalidInput,
		KeyUnavailable,
		ProviderUnavailable,
		AuthenticationFailed,
		CryptoFailure,
	};

	using RuntimePackagePublicKey = std::array<unsigned char, 32>;
	using RuntimePackageSignature = std::array<unsigned char, 64>;

	class RuntimePackageSecret final {
	  public:
		RuntimePackageSecret() = default;
		explicit RuntimePackageSecret(std::span<const unsigned char, 32> Value) noexcept;
		~RuntimePackageSecret();
		RuntimePackageSecret(const RuntimePackageSecret &) = delete;
		RuntimePackageSecret &operator=(const RuntimePackageSecret &) = delete;
		RuntimePackageSecret(RuntimePackageSecret &&Other) noexcept;
		RuntimePackageSecret &operator=(RuntimePackageSecret &&Other) noexcept;

		[[nodiscard]] bool IsAvailable() const noexcept;
		[[nodiscard]] std::span<const unsigned char, 32> View() const noexcept;
		void Reset() noexcept;

	  private:
		std::array<unsigned char, 32> Bytes{};
		bool Available = false;
	};

	class RuntimePackagePlaintext final {
	  public:
		RuntimePackagePlaintext() = default;
		~RuntimePackagePlaintext();
		RuntimePackagePlaintext(const RuntimePackagePlaintext &) = delete;
		RuntimePackagePlaintext &operator=(const RuntimePackagePlaintext &) = delete;
		RuntimePackagePlaintext(RuntimePackagePlaintext &&Other) noexcept;
		RuntimePackagePlaintext &operator=(RuntimePackagePlaintext &&Other) noexcept;

		[[nodiscard]] std::string_view View() const noexcept;
		void Reset() noexcept;

	  private:
		std::vector<unsigned char> Bytes;
		friend RuntimePackageCryptoStatus OpenRuntimePackageScript(
			std::string_view, std::string_view, const RuntimePackageSecret &, RuntimePackagePlaintext &
		);
	};

	struct RuntimePackageKeyRequest {
		std::string_view ProjectId;
		std::string_view ReleaseId;
		std::string_view PublisherId;
		std::string_view KeyId;
	};

	// Trusted native enrollment may explicitly use an empty ReleaseId to span all releases of this project.
	struct RuntimePackageKeyScope {
		std::string ProjectId;
		std::string ReleaseId;
		std::string PublisherId;
		std::string KeyId;
	};

	class IRuntimePackageSigner {
	  public:
		virtual ~IRuntimePackageSigner() = default;
		// Ed25519 transcript: ASCII "Gargantuan runtime package manifest Ed25519 v1", NUL, exact ManifestBytes.
		[[nodiscard]] virtual RuntimePackageCryptoStatus Sign(
			const RuntimePackageKeyRequest &Request, std::string_view ManifestBytes, RuntimePackageSignature &Output
		) const = 0;
	};

	class IRuntimePackageTrustProvider {
	  public:
		virtual ~IRuntimePackageTrustProvider() = default;
		[[nodiscard]] virtual RuntimePackageCryptoStatus GetPublicKey(
			const RuntimePackageKeyRequest &Request, RuntimePackagePublicKey &Output
		) const = 0;
	};

	class IRuntimePackageKeyProvider {
	  public:
		virtual ~IRuntimePackageKeyProvider() = default;
		[[nodiscard]] virtual RuntimePackageCryptoStatus GetKey(
			const RuntimePackageKeyRequest &Request, RuntimePackageSecret &Output
		) const = 0;
	};

	enum class RuntimePackageProvisioningUse : std::uint8_t { Build, Runtime };
	enum class RuntimePackageProvisioningMode : std::uint8_t { Local, Host };

	struct RuntimePackageProvisionedSecurity {
		RuntimePackageCryptoStatus Status = RuntimePackageCryptoStatus::ProviderUnavailable;
		RuntimePackageProvisioningMode Mode = RuntimePackageProvisioningMode::Local;
		std::shared_ptr<const IRuntimePackageSigner> Signer;
		std::shared_ptr<const IRuntimePackageTrustProvider> Trust;
		std::shared_ptr<const IRuntimePackageKeyProvider> Keys;
	};

	[[nodiscard]] std::shared_ptr<const IRuntimePackageSigner>
	MakeProvisionedRuntimePackageSigner(RuntimePackageKeyScope Scope, RuntimePackageSecret Secret);
	[[nodiscard]] std::shared_ptr<const IRuntimePackageTrustProvider>
	MakeProvisionedRuntimePackageTrustProvider(RuntimePackageKeyScope Scope, RuntimePackagePublicKey PublicKey);
	[[nodiscard]] std::shared_ptr<const IRuntimePackageKeyProvider>
	MakeProvisionedRuntimePackageKeyProvider(RuntimePackageKeyScope Scope, RuntimePackageSecret Secret);

	// Call only with trusted native build or host configuration. Runtime enrollment never accepts signing seeds.
	[[nodiscard]] RuntimePackageProvisionedSecurity LoadProvisionedRuntimePackageSecurity(RuntimePackageProvisioningUse Use);
	[[nodiscard]] RuntimePackageProvisionedSecurity LoadProvisionedRuntimePackageSecurity(
		const std::filesystem::path &Path, RuntimePackageProvisioningUse Use
	);

	[[nodiscard]] bool IsRuntimePackageCryptoAvailable() noexcept;
	[[nodiscard]] RuntimePackageCryptoStatus GenerateRuntimePackageReleaseSalt(std::array<unsigned char, 32> &Output);
	[[nodiscard]] RuntimePackageCryptoStatus
	GetRuntimePackagePublicKey(const RuntimePackageSecret &Seed, RuntimePackagePublicKey &Output);
	[[nodiscard]] RuntimePackageCryptoStatus SignRuntimePackageManifest(
		std::string_view ManifestBytes, const RuntimePackageSecret &Seed, RuntimePackageSignature &Output
	);
	[[nodiscard]] RuntimePackageCryptoStatus VerifyRuntimePackageManifest(
		std::string_view ManifestBytes, const RuntimePackagePublicKey &Key, std::span<const unsigned char> Signature
	);
	// AssociatedData is the canonical, authenticated package/script context supplied by the runtime distribution layer.
	[[nodiscard]] RuntimePackageCryptoStatus SealRuntimePackageScript(
		std::string_view Bytecode, std::string_view AssociatedData, const RuntimePackageSecret &Key, std::string &Output
	);
	[[nodiscard]] RuntimePackageCryptoStatus OpenRuntimePackageScript(
		std::string_view Container, std::string_view AssociatedData, const RuntimePackageSecret &Key,
		RuntimePackagePlaintext &Output
	);

}
