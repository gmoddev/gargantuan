#include "gargantuan/packaging/RuntimePackageCrypto.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <utility>

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gargantuan {
	namespace {
		using Status = RuntimePackageCryptoStatus;
		using namespace std::literals;

		void Wipe(std::span<unsigned char> Bytes) noexcept {
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
			if (!Bytes.empty()) OPENSSL_cleanse(Bytes.data(), Bytes.size());
#else
			for (volatile unsigned char &Byte : Bytes) Byte = 0;
#endif
		}

		template <std::size_t Size>
		struct SecureScratch final {
			std::array<unsigned char, Size> Bytes{};
			~SecureScratch() { Wipe(Bytes); }
		};

#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		constexpr auto ManifestDomain = "Gargantuan runtime package manifest Ed25519 v1\0"sv;
		constexpr auto ScriptDomain = "Gargantuan runtime script AAD v1\0"sv;
		constexpr auto ScriptKeyDomain = "Gargantuan runtime script AES-256-GCM key v1"sv;
		constexpr auto ScriptMagic = "GRS1"sv;
		constexpr std::size_t ScriptSizeOffset = 4;
		constexpr std::size_t ScriptSaltOffset = 8;
		constexpr std::size_t ScriptSaltBytes = 32;
		constexpr std::size_t ScriptNonceOffset = ScriptSaltOffset + ScriptSaltBytes;
		constexpr std::size_t ScriptNonceBytes = 12;
		static_assert(ScriptNonceOffset + ScriptNonceBytes == RuntimePackageScriptHeaderBytes);
		static_assert(MaximumRuntimePackageScriptBytecodeBytes <= std::numeric_limits<int>::max());
		static_assert(MaximumRuntimePackageAssociatedDataBytes <= std::numeric_limits<int>::max());

		using ManagedKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
		using ManagedKeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
		using ManagedDigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
		using ManagedCipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

		struct PreserveOpenSslErrors final {
			const bool Marked = ERR_set_mark() == 1;
			~PreserveOpenSslErrors() {
				if (!Marked || ERR_pop_to_mark() != 1) ERR_clear_error();
			}
		};

		const unsigned char *AsBytes(std::string_view Value) noexcept {
			return reinterpret_cast<const unsigned char *>(Value.data());
		}

		ManagedKey GetSigningKey(const RuntimePackageSecret &Seed) {
			return ManagedKey(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, Seed.View().data(), Seed.View().size()),
				EVP_PKEY_free);
		}

		bool ValidManifest(std::string_view Value) noexcept {
			return !Value.empty() && Value.size() <= MaximumRuntimePackageSignedManifestBytes;
		}

		std::string ManifestMessage(std::string_view Manifest) {
			std::string Result;
			Result.reserve(ManifestDomain.size() + Manifest.size());
			Result.append(ManifestDomain).append(Manifest);
			return Result;
		}

		bool ValidAssociatedData(std::string_view AssociatedData) noexcept {
			return !AssociatedData.empty() && AssociatedData.size() <= MaximumRuntimePackageAssociatedDataBytes;
		}

		bool DeriveScriptKey(const RuntimePackageSecret &Key, const unsigned char *Salt, std::span<unsigned char, 32> Output) {
			ManagedKeyContext Context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
			auto Size = Output.size();
			return Context && EVP_PKEY_derive_init(Context.get()) == 1 &&
				EVP_PKEY_CTX_set_hkdf_mode(Context.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) == 1 &&
				EVP_PKEY_CTX_set_hkdf_md(Context.get(), EVP_sha256()) == 1 &&
				EVP_PKEY_CTX_set1_hkdf_salt(Context.get(), Salt, static_cast<int>(ScriptSaltBytes)) == 1 &&
				EVP_PKEY_CTX_set1_hkdf_key(Context.get(), Key.View().data(), static_cast<int>(Key.View().size())) == 1 &&
				EVP_PKEY_CTX_add1_hkdf_info(Context.get(), AsBytes(ScriptKeyDomain), static_cast<int>(ScriptKeyDomain.size())) == 1 &&
				EVP_PKEY_derive(Context.get(), Output.data(), &Size) == 1 && Size == Output.size();
		}

		// The header and caller-supplied package/script context form one fixed-order AAD transcript.
		bool AddScriptAad(EVP_CIPHER_CTX *Context, bool Encrypt, std::string_view Header, std::string_view AssociatedData) {
			const auto Update = Encrypt ? &EVP_EncryptUpdate : &EVP_DecryptUpdate;
			int Written = 0;
			return Update(Context, nullptr, &Written, AsBytes(ScriptDomain), static_cast<int>(ScriptDomain.size())) == 1 &&
				Update(Context, nullptr, &Written, AsBytes(Header), static_cast<int>(Header.size())) == 1 &&
				Update(Context, nullptr, &Written, AsBytes(AssociatedData), static_cast<int>(AssociatedData.size())) == 1;
		}
#endif
	}

	RuntimePackageSecret::RuntimePackageSecret(std::span<const unsigned char, 32> Value) noexcept : Available(true) {
		std::copy(Value.begin(), Value.end(), Bytes.begin());
	}

	RuntimePackageSecret::~RuntimePackageSecret() { Reset(); }

	RuntimePackageSecret::RuntimePackageSecret(RuntimePackageSecret &&Other) noexcept
		: Bytes(Other.Bytes), Available(Other.Available) {
		Other.Reset();
	}

	RuntimePackageSecret &RuntimePackageSecret::operator=(RuntimePackageSecret &&Other) noexcept {
		if (this != &Other) {
			Reset();
			Bytes = Other.Bytes;
			Available = Other.Available;
			Other.Reset();
		}
		return *this;
	}

	bool RuntimePackageSecret::IsAvailable() const noexcept { return Available; }
	std::span<const unsigned char, 32> RuntimePackageSecret::View() const noexcept { return Bytes; }

	void RuntimePackageSecret::Reset() noexcept {
		Wipe(Bytes);
		Available = false;
	}

	RuntimePackagePlaintext::~RuntimePackagePlaintext() { Reset(); }
	RuntimePackagePlaintext::RuntimePackagePlaintext(RuntimePackagePlaintext &&Other) noexcept = default;

	RuntimePackagePlaintext &RuntimePackagePlaintext::operator=(RuntimePackagePlaintext &&Other) noexcept {
		if (this != &Other) {
			Reset();
			Bytes = std::move(Other.Bytes);
		}
		return *this;
	}

	std::string_view RuntimePackagePlaintext::View() const noexcept {
		if (Bytes.empty()) return {};
		return {reinterpret_cast<const char *>(Bytes.data()), Bytes.size()};
	}

	void RuntimePackagePlaintext::Reset() noexcept {
		Wipe(Bytes);
		std::vector<unsigned char>().swap(Bytes);
	}

	bool IsRuntimePackageCryptoAvailable() noexcept {
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		return true;
#else
		return false;
#endif
	}

	Status GenerateRuntimePackageReleaseSalt(std::array<unsigned char, 32> &Output) {
		return GenerateSecureRandomBytes(Output);
	}

	Status GenerateSecureRandomBytes(std::array<unsigned char, 32> &Output) {
		Output.fill(0);
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		const PreserveOpenSslErrors Errors;
		std::array<unsigned char, 32> Candidate{};
		if (RAND_bytes(Candidate.data(), static_cast<int>(Candidate.size())) != 1) return Status::CryptoFailure;
		Output = Candidate;
		return Status::Ok;
#else
		return Status::Unsupported;
#endif
	}

	Status GetRuntimePackagePublicKey(const RuntimePackageSecret &Seed, RuntimePackagePublicKey &Output) {
		Output.fill(0);
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		if (!Seed.IsAvailable()) return Status::KeyUnavailable;
		const PreserveOpenSslErrors Errors;
		auto Key = GetSigningKey(Seed);
		RuntimePackagePublicKey Candidate{};
		auto Size = Candidate.size();
		if (!Key || EVP_PKEY_get_raw_public_key(Key.get(), Candidate.data(), &Size) != 1 || Size != Candidate.size())
			return Status::CryptoFailure;
		Output = Candidate;
		return Status::Ok;
#else
		(void)Seed;
		return Status::Unsupported;
#endif
	}

	Status SignRuntimePackageManifest(std::string_view Manifest, const RuntimePackageSecret &Seed, RuntimePackageSignature &Output) {
		Output.fill(0);
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		if (!ValidManifest(Manifest)) return Status::InvalidInput;
		if (!Seed.IsAvailable()) return Status::KeyUnavailable;
		const PreserveOpenSslErrors Errors;
		auto Key = GetSigningKey(Seed);
		ManagedDigestContext Context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
		if (!Key || !Context || EVP_DigestSignInit(Context.get(), nullptr, nullptr, nullptr, Key.get()) != 1)
			return Status::CryptoFailure;
		const auto Message = ManifestMessage(Manifest);
		RuntimePackageSignature Candidate{};
		auto Size = Candidate.size();
		if (EVP_DigestSign(Context.get(), Candidate.data(), &Size, AsBytes(Message), Message.size()) != 1 || Size != Candidate.size())
			return Status::CryptoFailure;
		Output = Candidate;
		return Status::Ok;
#else
		(void)Manifest;
		(void)Seed;
		return Status::Unsupported;
#endif
	}

	Status VerifyRuntimePackageManifest(std::string_view Manifest, const RuntimePackagePublicKey &Key,
		std::span<const unsigned char> Signature) {
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		if (!ValidManifest(Manifest) || Signature.size() != RuntimePackageSignature{}.size()) return Status::InvalidInput;
		const PreserveOpenSslErrors Errors;
		ManagedKey Public(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, Key.data(), Key.size()), EVP_PKEY_free);
		ManagedDigestContext Context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
		if (!Public || !Context || EVP_DigestVerifyInit(Context.get(), nullptr, nullptr, nullptr, Public.get()) != 1)
			return Status::CryptoFailure;
		const auto Message = ManifestMessage(Manifest);
		const auto Verified = EVP_DigestVerify(Context.get(), Signature.data(), Signature.size(), AsBytes(Message), Message.size());
		if (Verified == 1) return Status::Ok;
		return Verified == 0 ? Status::AuthenticationFailed : Status::CryptoFailure;
#else
		(void)Manifest;
		(void)Key;
		(void)Signature;
		return Status::Unsupported;
#endif
	}

	Status SealRuntimePackageScript(std::string_view Bytecode, std::string_view AssociatedData,
		const RuntimePackageSecret &Key, std::string &Output) {
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		const auto Reject = [&Output](Status Reason) { Output.clear(); return Reason; };
		if (Bytecode.empty() || Bytecode.size() > MaximumRuntimePackageScriptBytecodeBytes || !ValidAssociatedData(AssociatedData))
			return Reject(Status::InvalidInput);
		if (!Key.IsAvailable()) return Reject(Status::KeyUnavailable);
		const PreserveOpenSslErrors Errors;
		std::string Container(RuntimePackageScriptHeaderBytes + Bytecode.size() + RuntimePackageScriptTagBytes, '\0');
		std::copy(ScriptMagic.begin(), ScriptMagic.end(), Container.begin());
		for (std::size_t Index = 0; Index < 4; ++Index)
			Container[ScriptSizeOffset + Index] = static_cast<char>((Bytecode.size() >> (Index * 8)) & 0xffU);
		auto *Bytes = reinterpret_cast<unsigned char *>(Container.data());
		if (RAND_bytes(Bytes + ScriptSaltOffset, static_cast<int>(ScriptSaltBytes + ScriptNonceBytes)) != 1)
			return Reject(Status::CryptoFailure);
		SecureScratch<32> ScriptKey;
		if (!DeriveScriptKey(Key, Bytes + ScriptSaltOffset, ScriptKey.Bytes)) return Reject(Status::CryptoFailure);
		ManagedCipherContext Context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!Context || EVP_EncryptInit_ex(Context.get(), EVP_aes_256_gcm(), nullptr, ScriptKey.Bytes.data(), Bytes + ScriptNonceOffset) != 1 ||
			!AddScriptAad(Context.get(), true, std::string_view(Container.data(), RuntimePackageScriptHeaderBytes), AssociatedData))
			return Reject(Status::CryptoFailure);
		int Written = 0;
		int Final = 0;
		if (EVP_EncryptUpdate(Context.get(), Bytes + RuntimePackageScriptHeaderBytes, &Written, AsBytes(Bytecode),
				static_cast<int>(Bytecode.size())) != 1 || Written != static_cast<int>(Bytecode.size()) ||
			EVP_EncryptFinal_ex(Context.get(), Bytes + RuntimePackageScriptHeaderBytes + Bytecode.size(), &Final) != 1 || Final != 0 ||
			EVP_CIPHER_CTX_ctrl(Context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(RuntimePackageScriptTagBytes),
				Bytes + RuntimePackageScriptHeaderBytes + Bytecode.size()) != 1)
			return Reject(Status::CryptoFailure);
		Output = std::move(Container);
		return Status::Ok;
#else
		(void)Bytecode;
		(void)AssociatedData;
		(void)Key;
		Output.clear();
		return Status::Unsupported;
#endif
	}

	Status OpenRuntimePackageScript(std::string_view Container, std::string_view AssociatedData,
		const RuntimePackageSecret &Key, RuntimePackagePlaintext &Output) {
#if defined(GARGANTUAN_RUNTIME_PACKAGE_OPENSSL) && GARGANTUAN_RUNTIME_PACKAGE_OPENSSL
		// A caller's input views may reference a previous authenticated output.
		const RuntimePackagePlaintext Previous(std::move(Output));
		Output.Reset();
		constexpr auto Overhead = RuntimePackageScriptHeaderBytes + RuntimePackageScriptTagBytes;
		if (Container.size() <= Overhead || Container.size() > MaximumRuntimePackageScriptContainerBytes ||
			!Container.starts_with(ScriptMagic) || !ValidAssociatedData(AssociatedData))
			return Status::InvalidInput;
		const auto *Bytes = AsBytes(Container);
		std::uint32_t DeclaredSize = 0;
		for (std::size_t Index = 0; Index < 4; ++Index)
			DeclaredSize |= static_cast<std::uint32_t>(Bytes[ScriptSizeOffset + Index]) << (Index * 8);
		if (DeclaredSize != Container.size() - Overhead) return Status::InvalidInput;
		if (!Key.IsAvailable()) return Status::KeyUnavailable;
		const PreserveOpenSslErrors Errors;
		SecureScratch<32> ScriptKey;
		if (!DeriveScriptKey(Key, Bytes + ScriptSaltOffset, ScriptKey.Bytes)) return Status::CryptoFailure;
		ManagedCipherContext Context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!Context || EVP_DecryptInit_ex(Context.get(), EVP_aes_256_gcm(), nullptr, ScriptKey.Bytes.data(), Bytes + ScriptNonceOffset) != 1 ||
			!AddScriptAad(Context.get(), false, Container.substr(0, RuntimePackageScriptHeaderBytes), AssociatedData))
			return Status::CryptoFailure;
		std::array<unsigned char, RuntimePackageScriptTagBytes> Tag{};
		std::copy_n(Bytes + RuntimePackageScriptHeaderBytes + DeclaredSize, Tag.size(), Tag.begin());
		if (EVP_CIPHER_CTX_ctrl(Context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(Tag.size()), Tag.data()) != 1)
			return Status::CryptoFailure;
		RuntimePackagePlaintext Candidate;
		Candidate.Bytes.resize(DeclaredSize);
		int Written = 0;
		if (EVP_DecryptUpdate(Context.get(), Candidate.Bytes.data(), &Written, Bytes + RuntimePackageScriptHeaderBytes,
				static_cast<int>(DeclaredSize)) != 1 || Written != static_cast<int>(DeclaredSize))
			return Status::CryptoFailure;
		SecureScratch<RuntimePackageScriptTagBytes> FinalBytes;
		int Final = 0;
		if (EVP_DecryptFinal_ex(Context.get(), FinalBytes.Bytes.data(), &Final) != 1) return Status::AuthenticationFailed;
		if (Final != 0) return Status::CryptoFailure;
		Output = std::move(Candidate);
		return Status::Ok;
#else
		(void)Container;
		(void)AssociatedData;
		(void)Key;
		Output.Reset();
		return Status::Unsupported;
#endif
	}

	namespace {
		constexpr std::size_t MaximumEnrollmentBytes = 64 * 1024;
		constexpr std::size_t MaximumEnrollmentEntries = 256;
		constexpr std::size_t MaximumEnrollmentIdentifierBytes = 128;
		constexpr auto EnrollmentHeader = "GargantuanRuntimePackageEnrollmentV1"sv;

		bool ValidIdentifier(std::string_view Value) noexcept {
			return !Value.empty() && Value.size() <= MaximumEnrollmentIdentifierBytes &&
				std::ranges::all_of(Value, [](char Character) {
					return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
						(Character >= '0' && Character <= '9') || Character == '.' || Character == '_' || Character == '-';
				});
		}

		bool ValidScope(const RuntimePackageKeyScope &Scope) noexcept {
			return ValidIdentifier(Scope.ProjectId) && (Scope.ReleaseId.empty() || ValidIdentifier(Scope.ReleaseId)) &&
				ValidIdentifier(Scope.PublisherId) && ValidIdentifier(Scope.KeyId);
		}

		bool ValidRequest(const RuntimePackageKeyRequest &Request) noexcept {
			return ValidIdentifier(Request.ProjectId) && ValidIdentifier(Request.ReleaseId) &&
				ValidIdentifier(Request.PublisherId) && ValidIdentifier(Request.KeyId);
		}

		bool Matches(const RuntimePackageKeyScope &Scope, const RuntimePackageKeyRequest &Request) noexcept {
			return Scope.ProjectId == Request.ProjectId && Scope.PublisherId == Request.PublisherId &&
				Scope.KeyId == Request.KeyId && (Scope.ReleaseId.empty() || Scope.ReleaseId == Request.ReleaseId);
		}

		template <typename ValueType>
		struct EnrollmentEntry final {
			RuntimePackageKeyScope Scope;
			ValueType Value;
		};

		using SecretEntries = std::vector<EnrollmentEntry<RuntimePackageSecret>>;
		using TrustEntries = std::vector<EnrollmentEntry<RuntimePackagePublicKey>>;

		template <typename Entries>
		bool Overlaps(const RuntimePackageKeyScope &Scope, const Entries &Existing) noexcept {
			return std::ranges::any_of(Existing, [&](const auto &Entry) {
				const auto &Other = Entry.Scope;
				return Scope.ProjectId == Other.ProjectId && Scope.PublisherId == Other.PublisherId && Scope.KeyId == Other.KeyId &&
					(Scope.ReleaseId.empty() || Other.ReleaseId.empty() || Scope.ReleaseId == Other.ReleaseId);
			});
		}

		template <typename Entries>
		const auto *FindEnrollment(const Entries &Values, const RuntimePackageKeyRequest &Request) noexcept {
			const auto Found = std::ranges::find_if(Values, [&](const auto &Entry) { return Matches(Entry.Scope, Request); });
			return Found != Values.end() ? &*Found : nullptr;
		}

		class EnrolledSigner final : public IRuntimePackageSigner {
		  public:
			explicit EnrolledSigner(SecretEntries Values) : Values(std::move(Values)) {}
			Status Sign(const RuntimePackageKeyRequest &Request, std::string_view Manifest, RuntimePackageSignature &Output) const override {
				Output.fill(0);
				if (!ValidRequest(Request)) return Status::InvalidInput;
				const auto *Entry = FindEnrollment(Values, Request);
				return Entry ? SignRuntimePackageManifest(Manifest, Entry->Value, Output) : Status::KeyUnavailable;
			}
		  private:
			const SecretEntries Values;
		};

		class EnrolledTrust final : public IRuntimePackageTrustProvider {
		  public:
			explicit EnrolledTrust(TrustEntries Values) : Values(std::move(Values)) {}
			Status GetPublicKey(const RuntimePackageKeyRequest &Request, RuntimePackagePublicKey &Output) const override {
				Output.fill(0);
				if (!ValidRequest(Request)) return Status::InvalidInput;
				const auto *Entry = FindEnrollment(Values, Request);
				if (!Entry) return Status::KeyUnavailable;
				Output = Entry->Value;
				return Status::Ok;
			}
		  private:
			const TrustEntries Values;
		};

		class EnrolledKeys final : public IRuntimePackageKeyProvider {
		  public:
			explicit EnrolledKeys(SecretEntries Values) : Values(std::move(Values)) {}
			Status GetKey(const RuntimePackageKeyRequest &Request, RuntimePackageSecret &Output) const override {
				Output.Reset();
				if (!ValidRequest(Request)) return Status::InvalidInput;
				const auto *Entry = FindEnrollment(Values, Request);
				if (!Entry) return Status::KeyUnavailable;
				Output = RuntimePackageSecret(Entry->Value.View());
				return Status::Ok;
			}
		  private:
			const SecretEntries Values;
		};

		struct EnrollmentBuffer final {
			std::vector<unsigned char> Bytes = std::vector<unsigned char>(MaximumEnrollmentBytes + 1);
			std::size_t Size = 0;
			bool AllowsSecrets = false;
			~EnrollmentBuffer() { Wipe(Bytes); }
			std::string_view View() const noexcept { return {reinterpret_cast<const char *>(Bytes.data()), Size}; }
		};

		bool IsEnrollmentPath(const std::filesystem::path &Path) {
			if (!Path.is_absolute() || !Path.has_filename() || Path.native().size() > 4096) return false;
			for (const auto &Part : Path.relative_path()) {
				const auto &Name = Part.native();
				if (Part.empty() || Part == "." || Part == ".." || Name.find(std::filesystem::path::value_type{}) != Name.npos)
					return false;
#ifdef _WIN32
				if (Name.find(L':') != Name.npos || Name.back() == L'.' || Name.back() == L' ') return false;
#endif
			}
			return true;
		}

#ifdef _WIN32
		class EnrollmentHandle final {
		  public:
			explicit EnrollmentHandle(HANDLE Value) noexcept : Value(Value) {}
			~EnrollmentHandle() { if (Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
			EnrollmentHandle(const EnrollmentHandle &) = delete;
			EnrollmentHandle &operator=(const EnrollmentHandle &) = delete;
			EnrollmentHandle(EnrollmentHandle &&Other) noexcept : Value(std::exchange(Other.Value, INVALID_HANDLE_VALUE)) {}
			HANDLE Get() const noexcept { return Value; }
		  private:
			HANDLE Value;
		};

		Status ReadEnrollment(const std::filesystem::path &Path, EnrollmentBuffer &Buffer) {
			// Pin every verified directory against renaming while opening the leaf.
			std::vector<EnrollmentHandle> Ancestors;
			const auto Relative = Path.relative_path();
			auto Current = Path.root_path();
			for (auto Part = Relative.begin(); Part != Relative.end(); ++Part) {
				Current /= *Part;
				const bool Leaf = std::next(Part) == Relative.end();
				const auto Flags = FILE_FLAG_OPEN_REPARSE_POINT | (Leaf ? 0 : FILE_FLAG_BACKUP_SEMANTICS);
				const auto Share = FILE_SHARE_READ | (Leaf ? 0 : FILE_SHARE_WRITE);
				EnrollmentHandle Handle(CreateFileW(Current.c_str(), Leaf ? GENERIC_READ : FILE_READ_ATTRIBUTES,
					Share, nullptr, OPEN_EXISTING, Flags, nullptr));
				if (Handle.Get() == INVALID_HANDLE_VALUE) return Status::ProviderUnavailable;
				BY_HANDLE_FILE_INFORMATION Info{};
				if (!GetFileInformationByHandle(Handle.Get(), &Info)) return Status::ProviderUnavailable;
				if ((Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
					((Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == Leaf)
					return Status::InvalidInput;
				if (!Leaf) {
					Ancestors.push_back(std::move(Handle));
					continue;
				}
				if (GetFileType(Handle.Get()) != FILE_TYPE_DISK || Info.nFileSizeHigh != 0 ||
					Info.nFileSizeLow == 0 || Info.nFileSizeLow > MaximumEnrollmentBytes)
					return Status::InvalidInput;
				DWORD Read = 0;
				if (!ReadFile(Handle.Get(), Buffer.Bytes.data(), Info.nFileSizeLow, &Read, nullptr) || Read != Info.nFileSizeLow)
					return Status::ProviderUnavailable;
				Buffer.Size = Read;
				// Native enrollment on Windows is provisioned with an operator-owned ACL.
				Buffer.AllowsSecrets = true;
				return Status::Ok;
			}
			return Status::InvalidInput;
		}
#else
		class EnrollmentHandle final {
		  public:
			explicit EnrollmentHandle(int Value) noexcept : Value(Value) {}
			~EnrollmentHandle() { if (Value >= 0) close(Value); }
			EnrollmentHandle(const EnrollmentHandle &) = delete;
			EnrollmentHandle &operator=(const EnrollmentHandle &) = delete;
			void Replace(int NewValue) noexcept { if (Value >= 0) close(Value); Value = NewValue; }
			int Get() const noexcept { return Value; }
		  private:
			int Value;
		};

		Status ReadEnrollment(const std::filesystem::path &Path, EnrollmentBuffer &Buffer) {
			EnrollmentHandle Handle(open(Path.root_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
			if (Handle.Get() < 0) return Status::ProviderUnavailable;
			const auto Relative = Path.relative_path();
			for (auto Part = Relative.begin(); Part != Relative.end(); ++Part) {
				const bool Leaf = std::next(Part) == Relative.end();
				const auto Flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (Leaf ? O_NONBLOCK : O_DIRECTORY);
				const auto Descriptor = openat(Handle.Get(), Part->c_str(), Flags);
				if (Descriptor < 0) return errno == ELOOP || errno == ENOTDIR ? Status::InvalidInput : Status::ProviderUnavailable;
				Handle.Replace(Descriptor);
			}
			struct stat Info{};
			if (fstat(Handle.Get(), &Info) != 0) return Status::ProviderUnavailable;
			if (!S_ISREG(Info.st_mode) || (Info.st_uid != geteuid() && Info.st_uid != 0) || (Info.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
				Info.st_size <= 0 || static_cast<std::uintmax_t>(Info.st_size) > MaximumEnrollmentBytes)
				return Status::InvalidInput;
			Buffer.AllowsSecrets = (Info.st_mode & (S_IRWXG | S_IRWXO)) == 0;
			while (Buffer.Size < Buffer.Bytes.size()) {
				const auto Count = read(Handle.Get(), Buffer.Bytes.data() + Buffer.Size, Buffer.Bytes.size() - Buffer.Size);
				if (Count < 0 && errno == EINTR) continue;
				if (Count < 0) return Status::ProviderUnavailable;
				if (Count == 0) break;
				Buffer.Size += static_cast<std::size_t>(Count);
			}
			if (Buffer.Size != static_cast<std::size_t>(Info.st_size) || Buffer.Size > MaximumEnrollmentBytes)
				return Status::InvalidInput;
			return Status::Ok;
		}
#endif

		std::string_view TakeLine(std::string_view &Contents) noexcept {
			const auto Separator = Contents.find('\n');
			auto Line = Contents.substr(0, Separator);
			Contents.remove_prefix(Separator == Contents.npos ? Contents.size() : Separator + 1);
			if (Line.ends_with('\r')) Line.remove_suffix(1);
			return Line;
		}

		bool ParseFields(std::string_view Line, std::array<std::string_view, 6> &Fields) noexcept {
			for (std::size_t Index = 0; Index + 1 < Fields.size(); ++Index) {
				const auto Separator = Line.find('\t');
				if (Separator == Line.npos) return false;
				Fields[Index] = Line.substr(0, Separator);
				Line.remove_prefix(Separator + 1);
			}
			Fields.back() = Line;
			return Line.find('\t') == Line.npos;
		}

		int HexDigit(char Character) noexcept {
			if (Character >= '0' && Character <= '9') return Character - '0';
			if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
			if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
			return -1;
		}

		bool DecodeHexKey(std::string_view Hex, std::span<unsigned char, 32> Output) noexcept {
			if (Hex.size() != Output.size() * 2) return false;
			for (std::size_t Index = 0; Index < Output.size(); ++Index) {
				const auto High = HexDigit(Hex[Index * 2]);
				const auto Low = HexDigit(Hex[Index * 2 + 1]);
				if (High < 0 || Low < 0) return false;
				Output[Index] = static_cast<unsigned char>(High * 16 + Low);
			}
			return true;
		}

		RuntimePackageProvisionedSecurity EnrollmentFailure(Status Cause) {
			RuntimePackageProvisionedSecurity Result;
			Result.Status = Cause;
			return Result;
		}

		RuntimePackageProvisionedSecurity ParseEnrollment(const EnrollmentBuffer &Buffer, RuntimePackageProvisioningUse Use) {
			auto Contents = Buffer.View();
			if (TakeLine(Contents) != EnrollmentHeader) return EnrollmentFailure(Status::InvalidInput);
			const auto Mode = TakeLine(Contents);
			if (Mode != "mode\tLocal" && Mode != "mode\tHost") return EnrollmentFailure(Status::InvalidInput);
			if (Contents.empty()) return EnrollmentFailure(Status::InvalidInput);
			TrustEntries Trust;
			SecretEntries Signers;
			SecretEntries Keys;
			for (std::size_t Index = 0; !Contents.empty(); ++Index) {
				std::array<std::string_view, 6> Fields;
				if (Index >= MaximumEnrollmentEntries || !ParseFields(TakeLine(Contents), Fields)) return EnrollmentFailure(Status::InvalidInput);
				const auto &[Kind, Project, Release, Publisher, KeyId, Hex] = Fields;
				if (!ValidIdentifier(Project) || (Release != "*" && !ValidIdentifier(Release)) ||
					!ValidIdentifier(Publisher) || !ValidIdentifier(KeyId))
					return EnrollmentFailure(Status::InvalidInput);
				RuntimePackageKeyScope Scope{std::string(Project), Release == "*" ? std::string() : std::string(Release),
					std::string(Publisher), std::string(KeyId)};
				SecureScratch<32> Key;
				if (!DecodeHexKey(Hex, Key.Bytes)) return EnrollmentFailure(Status::InvalidInput);
				if (Kind == "trust") {
					if (Overlaps(Scope, Trust)) return EnrollmentFailure(Status::InvalidInput);
					Trust.push_back({std::move(Scope), Key.Bytes});
					continue;
				}
				if (!Buffer.AllowsSecrets || (Kind != "content" && Kind != "signer") ||
					(Kind == "signer" && Use != RuntimePackageProvisioningUse::Build))
					return EnrollmentFailure(Status::InvalidInput);
				auto &Destination = Kind == "signer" ? Signers : Keys;
				if (Overlaps(Scope, Destination)) return EnrollmentFailure(Status::InvalidInput);
				Destination.push_back({std::move(Scope), RuntimePackageSecret(Key.Bytes)});
			}
			RuntimePackageProvisionedSecurity Result;
			Result.Status = Status::Ok;
			Result.Mode = Mode == "mode\tLocal" ? RuntimePackageProvisioningMode::Local : RuntimePackageProvisioningMode::Host;
			if (!Signers.empty()) Result.Signer = std::make_shared<EnrolledSigner>(std::move(Signers));
			if (!Trust.empty()) Result.Trust = std::make_shared<EnrolledTrust>(std::move(Trust));
			if (!Keys.empty()) Result.Keys = std::make_shared<EnrolledKeys>(std::move(Keys));
			return Result;
		}
	}

	std::shared_ptr<const IRuntimePackageSigner> MakeProvisionedRuntimePackageSigner(
		RuntimePackageKeyScope Scope, RuntimePackageSecret Secret) {
		if (!ValidScope(Scope) || !Secret.IsAvailable()) return {};
		SecretEntries Values;
		Values.push_back({std::move(Scope), std::move(Secret)});
		return std::make_shared<EnrolledSigner>(std::move(Values));
	}

	std::shared_ptr<const IRuntimePackageTrustProvider> MakeProvisionedRuntimePackageTrustProvider(
		RuntimePackageKeyScope Scope, RuntimePackagePublicKey PublicKey) {
		if (!ValidScope(Scope)) return {};
		TrustEntries Values;
		Values.push_back({std::move(Scope), PublicKey});
		return std::make_shared<EnrolledTrust>(std::move(Values));
	}

	std::shared_ptr<const IRuntimePackageKeyProvider> MakeProvisionedRuntimePackageKeyProvider(
		RuntimePackageKeyScope Scope, RuntimePackageSecret Secret) {
		if (!ValidScope(Scope) || !Secret.IsAvailable()) return {};
		SecretEntries Values;
		Values.push_back({std::move(Scope), std::move(Secret)});
		return std::make_shared<EnrolledKeys>(std::move(Values));
	}

	RuntimePackageProvisionedSecurity LoadProvisionedRuntimePackageSecurity(
		const std::filesystem::path &Path, RuntimePackageProvisioningUse Use) {
		try {
			if (!IsEnrollmentPath(Path)) return EnrollmentFailure(Status::InvalidInput);
			EnrollmentBuffer Buffer;
			const auto Loaded = ReadEnrollment(Path, Buffer);
			return Loaded == Status::Ok ? ParseEnrollment(Buffer, Use) : EnrollmentFailure(Loaded);
		} catch (const std::exception &) {
			return EnrollmentFailure(Status::ProviderUnavailable);
		}
	}

	RuntimePackageProvisionedSecurity LoadProvisionedRuntimePackageSecurity(RuntimePackageProvisioningUse Use) {
		try {
#ifdef _WIN32
			const wchar_t *Name = Use == RuntimePackageProvisioningUse::Build ?
				L"GARGANTUAN_RUNTIME_PACKAGE_BUILD_ENROLLMENT" : L"GARGANTUAN_RUNTIME_PACKAGE_RUNTIME_ENROLLMENT";
			std::array<wchar_t, 4097> Value{};
			const auto Count = GetEnvironmentVariableW(Name, Value.data(), static_cast<DWORD>(Value.size()));
			if (Count == 0) return EnrollmentFailure(Status::ProviderUnavailable);
			if (Count >= Value.size()) return EnrollmentFailure(Status::InvalidInput);
			return LoadProvisionedRuntimePackageSecurity(std::filesystem::path(Value.data()), Use);
#else
			const char *Name = Use == RuntimePackageProvisioningUse::Build ?
				"GARGANTUAN_RUNTIME_PACKAGE_BUILD_ENROLLMENT" : "GARGANTUAN_RUNTIME_PACKAGE_RUNTIME_ENROLLMENT";
			const char *Value = std::getenv(Name);
			return Value && *Value ? LoadProvisionedRuntimePackageSecurity(std::filesystem::path(Value), Use) :
				EnrollmentFailure(Status::ProviderUnavailable);
#endif
		} catch (const std::exception &) {
			return EnrollmentFailure(Status::ProviderUnavailable);
		}
	}
}
