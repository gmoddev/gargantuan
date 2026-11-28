#pragma once
#include <functional>
#include "JoinProof.hpp"
#include "HostInvitationChannel.hpp"
#include <filesystem>

namespace gargantuan {
	class ApplicationPresentation;
	class BaseRenderer;
	struct RuntimePackagePayload;
	namespace host {
		struct PrivateServerPackage {
			std::filesystem::path Root;
			std::string Manifest;
			std::filesystem::path RuntimeEnrollment;
			std::optional<network::TransportEndpoint> Listener;
			std::optional<HostInvitationChannel> Invitations;
		};
		struct PlayerAdmission {
			network::TransportEndpoint Endpoint;
			detail::PlayerJoinProof Proof;
			std::optional<network::TransportServerPublicKey> ServerKey;
			std::optional<HostInvitationChannel> Refresh;
		};
		struct LauncherDirectory {
			std::string Source;
			HostInvitationChannel Channel;
		};
		int RunStandalonePlayer(ApplicationPresentation &App, BaseRenderer &Device,
			const RuntimePackagePayload &Package, const std::filesystem::path &Root, const std::function<std::filesystem::path()> &Profile, int MaximumFrames, std::optional<PlayerAdmission> Admission = {}, std::optional<PrivateServerPackage> PrivateServer = {}, bool TracePresentation = false, bool SessionSmoke = false, std::optional<LauncherDirectory> Directory = {});
	}
}
