#include "host/editor/NodeDevelopmentProfile.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "host/server/NodePersistenceProvider.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gargantuan::host {
	namespace {
		constexpr std::size_t MaximumProfileBytes = 8192;

		[[noreturn]] void InvalidProfile() { throw std::invalid_argument("Private Studio Node development profile is invalid"); }

		std::filesystem::path DecodeAbsolutePath(std::string_view Text) {
			if (Text.empty() || Text.size() > 1024 || Text.find('\0') != std::string_view::npos) InvalidProfile();
			const auto Utf8 = std::u8string_view(reinterpret_cast<const char8_t *>(Text.data()), Text.size());
			std::filesystem::path Path(Utf8);
			if (!Path.is_absolute() || Path != Path.lexically_normal()) InvalidProfile();
#if defined(_WIN32)
			const auto Drive = Path.root_name().native();
			if (Drive.size() != 2 || Drive[1] != L':') InvalidProfile();
#endif
			return Path;
		}

		bool IsWithin(const std::filesystem::path &File, const std::filesystem::path &Directory) {
			for (auto Current = File.parent_path(); !Current.empty(); Current = Current.parent_path()) {
				if (std::filesystem::equivalent(Current, Directory)) return true;
				if (Current == Current.parent_path()) break;
			}
			return false;
		}

		void RequirePrivateFile(const std::filesystem::path &File) {
			if (!std::filesystem::is_regular_file(File)) InvalidProfile();
			for (auto Current = File; !Current.empty(); Current = Current.parent_path()) {
#if defined(_WIN32)
				const auto Attributes = GetFileAttributesW(Current.c_str());
				if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) InvalidProfile();
#else
				if (std::filesystem::is_symlink(std::filesystem::symlink_status(Current))) InvalidProfile();
#endif
				if (Current == Current.parent_path()) break;
			}
		}

		bool IsIdentity(std::string_view Value) {
			return !Value.empty() && std::ranges::all_of(Value, [](unsigned char Ch) {
				return (Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') ||
					(Ch >= '0' && Ch <= '9') || Ch == '-' || Ch == '_' || Ch == '.';
			});
		}

		bool IsGeneration(std::string_view Value) {
			if (Value.size() != 36 || Value[8] != '-' || Value[13] != '-' ||
				Value[18] != '-' || Value[23] != '-') return false;
			std::string Compact(Value);
			std::erase(Compact, '-');
			return ProjectId::Parse(Compact).has_value();
		}
	} // namespace

	EditorNodeDevelopmentPersistence LoadStudioNodeDevelopmentProfile(std::string_view ProfilePath) {
		const auto File = DecodeAbsolutePath(ProfilePath);
		RequirePrivateFile(File);
		const auto Size = std::filesystem::file_size(File);
		if (Size == 0 || Size > MaximumProfileBytes) InvalidProfile();
		std::ifstream Input(File, std::ios::binary);
		std::string Bytes(static_cast<std::size_t>(Size), '\0');
		Input.read(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
		if (!Input || Input.peek() != std::char_traits<char>::eof() || Input.bad()) InvalidProfile();
		const auto Json = JsonCodec::Parse(Bytes, MaximumProfileBytes, "private Studio Node profile");
		if (!Json || !Json->is_object() || Json->size() != 11 || !Json->contains("Version") ||
			!(*Json)["Version"].is_number_unsigned() || (*Json)["Version"] != 1) InvalidProfile();

		const auto String = [&Json](std::string_view Key, std::size_t Bound) -> std::string {
			const auto Found = Json->find(Key);
			if (Found == Json->end() || !Found->is_string()) InvalidProfile();
			const auto &Value = Found->get_ref<const std::string &>();
			if (Value.empty() || Value.size() > Bound) InvalidProfile();
			return Value;
		};
		const auto Project = ProjectId::Parse(String("ProjectId", 32));
		const auto ProjectRoot = std::filesystem::canonical(DecodeAbsolutePath(String("ProjectRoot", 1024)));
		if (!Project || !std::filesystem::is_directory(ProjectRoot)) InvalidProfile();
		NodePersistenceProviderConfiguration Configuration{
			.Endpoint = String("Endpoint", 36),
			.RootCertificateFile = DecodeAbsolutePath(String("RootCertificateFile", 1024)),
			.WorkloadTokenFile = DecodeAbsolutePath(String("WorkloadTokenFile", 1024)),
			.Development = NodeDevelopmentBinding{
				.Project = *Project,
				.Tenant = String("TenantId", 128),
				.Database = String("Database", 63),
				.DatabaseGeneration = String("DatabaseGeneration", 36),
				.NodeInstance = String("NodeInstance", 128),
				.CertificateSha256 = String("RootCertificateSha256", 64),
			},
		};
		RequirePrivateFile(Configuration.RootCertificateFile);
		RequirePrivateFile(Configuration.WorkloadTokenFile);
		const auto &Binding = *Configuration.Development;
		if ((!Configuration.Endpoint.starts_with("127.0.0.1:") && !Configuration.Endpoint.starts_with("[::1]:")) ||
			!IsIdentity(Binding.Tenant) || !IsIdentity(Binding.Database) ||
			!Binding.Database.starts_with("gargantuan_studio_dev_") || !IsIdentity(Binding.NodeInstance) ||
			!IsGeneration(Binding.DatabaseGeneration) || !AssetContentId::Parse(Binding.CertificateSha256) ||
			IsWithin(File, ProjectRoot) || IsWithin(Configuration.RootCertificateFile, ProjectRoot) ||
			IsWithin(Configuration.WorkloadTokenFile, ProjectRoot))
			InvalidProfile();
		return {*Project, ProjectRoot, [Configuration = std::move(Configuration)] {
			RequirePrivateFile(Configuration.RootCertificateFile);
			RequirePrivateFile(Configuration.WorkloadTokenFile);
			return std::make_shared<NodePersistenceProvider>(Configuration);
		}};
	}
} // namespace gargantuan::host
