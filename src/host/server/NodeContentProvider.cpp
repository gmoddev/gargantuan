#include "host/server/NodeContentProvider.hpp"

#include "content/v1/content.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace gargantuan::host {
	namespace Wire = ::gargantuan::node::content::v1;

	namespace {
		bool IsPortableEnvironmentName(std::string_view Value) {
			if (Value.empty()) return false;
			for (std::size_t Index = 0; Index < Value.size(); ++Index) {
				const auto Character = static_cast<unsigned char>(Value[Index]);
				if ((Index == 0 && Character != '_' && !std::isalpha(Character)) ||
					(Index > 0 && Character != '_' && !std::isalnum(Character)))
					return false;
			}
			return true;
		}

		std::optional<std::size_t> GetBoundedCStringLength(const char *Value, std::size_t MaximumBytes) {
			if (!Value) return std::nullopt;
			for (std::size_t Index = 0; Index <= MaximumBytes; ++Index)
				if (Value[Index] == '\0') return Index;
			return std::nullopt;
		}

		ContentProviderError MakeError(ContentProviderErrorCode Code, std::string Message) {
			return {Code, std::move(Message)};
		}

		ContentProviderError MapStatus(const grpc::Status &Status) {
			switch (Status.error_code()) {
			case grpc::StatusCode::CANCELLED:
				return MakeError(ContentProviderErrorCode::Cancelled, "Node content request was cancelled");
			case grpc::StatusCode::DEADLINE_EXCEEDED:
				return MakeError(ContentProviderErrorCode::DeadlineExceeded, "Node content request deadline elapsed");
			case grpc::StatusCode::NOT_FOUND:
				return MakeError(ContentProviderErrorCode::NotFound, "Node content is unavailable for this package identity");
			case grpc::StatusCode::RESOURCE_EXHAUSTED:
				return MakeError(ContentProviderErrorCode::ResourceExhausted, "Node content response exceeded its bound");
			case grpc::StatusCode::INVALID_ARGUMENT:
			case grpc::StatusCode::INTERNAL:
			case grpc::StatusCode::DATA_LOSS:
				return MakeError(ContentProviderErrorCode::InvalidResponse, "Node content response was invalid");
			default:
				return MakeError(ContentProviderErrorCode::Unavailable, "Node ContentStreaming request failed");
			}
		}

		std::chrono::system_clock::time_point
		GetSystemDeadline(const ContentRequestContext &Context, std::chrono::milliseconds ProviderDeadline) {
			const auto Remaining = Context.Deadline - std::chrono::steady_clock::now();
			const auto Bounded = std::min(
				std::chrono::duration_cast<std::chrono::milliseconds>(Remaining), ProviderDeadline
			);
			return std::chrono::system_clock::now() + std::max(Bounded, std::chrono::milliseconds::zero());
		}
	} // namespace

	struct NodeContentProvider::Impl final {
		explicit Impl(NodeContentProviderConfiguration Value) : Configuration(std::move(Value)) {}

		NodeContentProviderConfiguration Configuration;
		std::shared_ptr<grpc::Channel> Channel;
		std::unique_ptr<Wire::ContentStreaming::Stub> Stub;
		std::string WorkloadToken;
		std::atomic<bool> Stopping{true};
		std::atomic<std::uint64_t> NextRequestId{1};
		std::mutex ContextMutex;
		std::unordered_set<grpc::ClientContext *> ActiveContexts;

		void Register(grpc::ClientContext &Context) {
			std::scoped_lock Lock(ContextMutex);
			if (Stopping.load(std::memory_order_acquire)) throw std::runtime_error("provider is stopping");
			ActiveContexts.insert(&Context);
		}
		void Unregister(grpc::ClientContext &Context) {
			std::scoped_lock Lock(ContextMutex);
			ActiveContexts.erase(&Context);
		}
		void CancelActive() noexcept {
			std::scoped_lock Lock(ContextMutex);
			for (auto *Context : ActiveContexts)
				Context->TryCancel();
		}
		void ClearToken() noexcept {
			std::fill(WorkloadToken.begin(), WorkloadToken.end(), '\0');
			WorkloadToken.clear();
		}
		std::string GetRequestId() {
			return "server-content-" + std::to_string(NextRequestId.fetch_add(1, std::memory_order_relaxed));
		}
	};

	namespace {
		class ActiveCall final {
		  public:
			ActiveCall(NodeContentProvider::Impl &StateValue, const ContentRequestContext &EngineContextValue)
				: State(StateValue), EngineContext(EngineContextValue) {
				State.Register(Context);
				try {
					Watcher = std::jthread([this](std::stop_token Stop) {
						while (!Stop.stop_requested()) {
							if (EngineContext.IsCancelled() || EngineContext.IsExpired()) {
								Context.TryCancel();
								return;
							}
							std::this_thread::sleep_for(std::chrono::milliseconds(1));
						}
					});
				} catch (...) {
					State.Unregister(Context);
					throw;
				}
			}
			~ActiveCall() {
				Watcher.request_stop();
				if (Watcher.joinable()) Watcher.join();
				State.Unregister(Context);
			}

			NodeContentProvider::Impl &State;
			const ContentRequestContext &EngineContext;
			grpc::ClientContext Context;
			std::jthread Watcher;
		};
	} // namespace

	NodeContentProvider::NodeContentProvider(NodeContentProviderConfiguration Configuration)
		: State(std::make_unique<Impl>(std::move(Configuration))) {}

	NodeContentProvider::~NodeContentProvider() {
		Stop();
		if (State) State->ClearToken();
	}

	std::string_view NodeContentProvider::Name() const { return "node-content-v1"; }

	ContentProviderLifecycleResult NodeContentProvider::Start(const ContentRequestContext &Context) {
		const auto &Configuration = State->Configuration;
		if (Configuration.Endpoint.empty() || Configuration.Endpoint.size() > Configuration.MaximumEndpointBytes ||
			Configuration.WorkloadTokenEnvironment.size() > Configuration.MaximumSecretReferenceBytes ||
			!IsPortableEnvironmentName(Configuration.WorkloadTokenEnvironment) ||
			Configuration.RootCertificateFile.empty() ||
			Configuration.RootCertificateFile.string().size() > Configuration.MaximumCertificatePathBytes ||
			Configuration.MaximumManifestResponseBytes < 1024 ||
			Configuration.MaximumManifestResponseBytes > MaximumPackageContentManifestBytes ||
			Configuration.MaximumContentResponseBytes < 1024 ||
			Configuration.MaximumContentResponseBytes > MaximumPackageContentPayloadBytes ||
			Configuration.ConnectionDeadline < std::chrono::milliseconds(10) ||
			Configuration.ConnectionDeadline > std::chrono::seconds(10) ||
			Configuration.RequestDeadline < std::chrono::milliseconds(10) ||
			Configuration.RequestDeadline > std::chrono::seconds(30))
			return std::unexpected(MakeError(ContentProviderErrorCode::InvalidResponse, "Node content provider configuration is invalid"));

		std::error_code FileError;
		const auto CertificateBytes = std::filesystem::file_size(Configuration.RootCertificateFile, FileError);
		if (FileError || CertificateBytes == 0 || CertificateBytes > Configuration.MaximumRootCertificateBytes)
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node root certificate is unavailable or invalid"));
		std::ifstream CertificateInput(Configuration.RootCertificateFile, std::ios::binary);
		std::string Certificate(static_cast<std::size_t>(CertificateBytes), '\0');
		CertificateInput.read(Certificate.data(), static_cast<std::streamsize>(Certificate.size()));
		if (CertificateInput.gcount() != static_cast<std::streamsize>(Certificate.size()))
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node root certificate could not be read"));
		char ExtraByte = '\0';
		CertificateInput.read(&ExtraByte, 1);
		if (CertificateInput.gcount() != 0 || CertificateInput.bad())
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node root certificate could not be read"));

		const char *Token = std::getenv(Configuration.WorkloadTokenEnvironment.c_str());
		const auto TokenBytes = GetBoundedCStringLength(Token, Configuration.MaximumWorkloadTokenBytes);
		if (!TokenBytes || *TokenBytes == 0)
			return std::unexpected(MakeError(
				ContentProviderErrorCode::Unavailable,
				"Node workload token environment variable '" + Configuration.WorkloadTokenEnvironment +
					"' is missing, empty, or exceeds its bound"
			));

		grpc::SslCredentialsOptions Credentials;
		Credentials.pem_root_certs = std::move(Certificate);
		grpc::ChannelArguments Arguments;
		const auto MaximumResponse = std::max(
			Configuration.MaximumManifestResponseBytes, Configuration.MaximumContentResponseBytes
		);
		Arguments.SetMaxReceiveMessageSize(static_cast<int>(MaximumResponse + 1024));
		State->Channel = grpc::CreateCustomChannel(
			Configuration.Endpoint, grpc::SslCredentials(Credentials), Arguments
		);
		State->Stub = Wire::ContentStreaming::NewStub(State->Channel);
		State->WorkloadToken.assign(Token, *TokenBytes);
		State->Stopping.store(false, std::memory_order_release);
		const auto ConnectionDeadline = std::min(
			GetSystemDeadline(Context, Configuration.ConnectionDeadline),
			std::chrono::system_clock::now() + Configuration.ConnectionDeadline
		);
		if (Context.IsCancelled() || Context.IsExpired() || !State->Channel->WaitForConnected(ConnectionDeadline)) {
			Stop();
			State->Stub.reset();
			State->Channel.reset();
			State->ClearToken();
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node ContentStreaming TLS connection failed"));
		}
		return {};
	}

	void NodeContentProvider::Stop() {
		if (!State) return;
		State->Stopping.store(true, std::memory_order_release);
		State->CancelActive();
	}

	ContentManifestProviderResult
	NodeContentProvider::GetManifest(const ContentRequestContext &Context, const PackageContentNamespace &Package) {
		if (Context.IsCancelled()) return std::unexpected(MakeError(ContentProviderErrorCode::Cancelled, "Node content request was cancelled"));
		if (Context.IsExpired()) return std::unexpected(MakeError(ContentProviderErrorCode::DeadlineExceeded, "Node content request deadline elapsed"));
		if (!State->Stub || State->Stopping.load(std::memory_order_acquire))
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node content provider is stopped"));

		Wire::GetManifestRequest Request;
		const auto RequestId = State->GetRequestId();
		Request.set_request_id(RequestId);
		Request.set_project_id(Package.Project.ToString());
		Request.set_package_version(Package.PackageVersion);
		Wire::GetManifestResponse Response;
		try {
			ActiveCall Call(*State, Context);
			Call.Context.set_deadline(GetSystemDeadline(Context, State->Configuration.RequestDeadline));
			Call.Context.AddMetadata("authorization", "Bearer " + State->WorkloadToken);
			auto Status = State->Stub->GetManifest(&Call.Context, Request, &Response);
			if (!Status.ok()) return std::unexpected(MapStatus(Status));
		} catch (...) {
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node ContentStreaming request failed"));
		}
		if (Response.request_id() != RequestId || Response.project_id() != Package.Project.ToString() ||
			Response.package_version() != Package.PackageVersion ||
			Response.manifest().size() > State->Configuration.MaximumManifestResponseBytes ||
			!AssetContentId::Parse(Response.sha256()))
			return std::unexpected(MakeError(ContentProviderErrorCode::InvalidResponse, "Node manifest response identity or bounds are invalid"));
		return Response.manifest();
	}

	ContentPayloadProviderResult
	NodeContentProvider::GetContent(const ContentRequestContext &Context, const PackageContentIdentity &Identity) {
		if (Context.IsCancelled()) return std::unexpected(MakeError(ContentProviderErrorCode::Cancelled, "Node content request was cancelled"));
		if (Context.IsExpired()) return std::unexpected(MakeError(ContentProviderErrorCode::DeadlineExceeded, "Node content request deadline elapsed"));
		if (!State->Stub || State->Stopping.load(std::memory_order_acquire))
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node content provider is stopped"));

		Wire::GetContentRequest Request;
		const auto RequestId = State->GetRequestId();
		Request.set_request_id(RequestId);
		Request.set_project_id(Identity.Package.Project.ToString());
		Request.set_package_version(Identity.Package.PackageVersion);
		Request.set_content_key(Identity.Key);
		Wire::GetContentResponse Response;
		try {
			ActiveCall Call(*State, Context);
			Call.Context.set_deadline(GetSystemDeadline(Context, State->Configuration.RequestDeadline));
			Call.Context.AddMetadata("authorization", "Bearer " + State->WorkloadToken);
			auto Status = State->Stub->GetContent(&Call.Context, Request, &Response);
			if (!Status.ok()) return std::unexpected(MapStatus(Status));
		} catch (...) {
			return std::unexpected(MakeError(ContentProviderErrorCode::Unavailable, "Node ContentStreaming request failed"));
		}

		auto Digest = AssetContentId::Parse(Response.sha256());
		if (Response.request_id() != RequestId || Response.project_id() != Identity.Package.Project.ToString() ||
			Response.package_version() != Identity.Package.PackageVersion || Response.content_key() != Identity.Key ||
			Response.content().size() > State->Configuration.MaximumContentResponseBytes || !Digest)
			return std::unexpected(MakeError(ContentProviderErrorCode::InvalidResponse, "Node content response identity or bounds are invalid"));
		auto Bytes = std::make_shared<std::vector<std::uint8_t>>(Response.content().begin(), Response.content().end());
		return ContentPayload{Identity, *Digest, std::move(Bytes)};
	}
} // namespace gargantuan::host
