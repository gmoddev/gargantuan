#pragma once

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/network/CharacterProtocol.hpp"
#include "gargantuan/network/ReplicationProtocol.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <unordered_set>

#ifdef GARGANTUAN_CONTENT_SCALE_NODE
#include "host/server/NodeContentProvider.hpp"
#endif

namespace gargantuan::test {
	inline constexpr std::string_view ScaleContentKey = "workspace/scale";
	inline constexpr std::string_view ScaleRootName = "ContentScaleRegion";
	inline constexpr std::string_view ScaleManifestReference = "content/content.manifest.json";

	inline void WriteScalePackage(const std::filesystem::path &Directory, std::size_t ObjectCount) {
		if (ObjectCount == 0 || ObjectCount > MaximumPackageContentObjectsPerUnit)
			throw std::invalid_argument("invalid scale object count");
		auto Root = std::static_pointer_cast<Instance>(std::make_shared<Folder>());
		Root->SetName(std::string(ScaleRootName));
		Root->SetArchivable(true);
		for (std::size_t Index = 1; Index < ObjectCount; ++Index) {
			auto Child = std::make_shared<Part>();
			Child->SetName("ContentScalePart" + std::to_string(Index));
			Child->SetArchivable(true);
			Child->SetAnchored(true);
			Child->SetCanCollide(false);
			Child->SetSize({2.0f, 1.0f, 3.0f});
			Child->SetCFrame(CFrame(static_cast<float>(Index % 32), 2.0f, static_cast<float>(Index / 32)));
			Child->SetParent(Root);
		}
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
		if (Bytes.size() > MaximumPackageContentPayloadBytes) throw std::runtime_error("scale payload exceeds limit");
		PackageContentManifest Manifest{
			.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17},
			.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
			.Entries = {{
				.Key = std::string(ScaleContentKey), .BlobReference = "content/regions/scale.instance.json",
				.Digest = AssetContentId::Hash(Bytes), .CompressedBytes = Bytes.size(), .UncompressedBytes = Bytes.size(),
				.ObjectCount = static_cast<std::uint32_t>(ObjectCount), .Dependencies = {},
				.PackageSpaceKey = "default", .CoarseBounds = PackageCoarseBounds{{-2, -2, -2}, {34, 5, 34}},
				.Flags = PackageContentFlags::ImmutableBaseline,
			}},
		};
		std::filesystem::create_directories(Directory / "content/regions");
		auto Write = [&](const std::filesystem::path &Path, const std::string &Value) {
			std::ofstream Output(Directory / Path, std::ios::binary | std::ios::trunc);
			Output.write(Value.data(), static_cast<std::streamsize>(Value.size()));
			if (!Output) throw std::runtime_error("cannot write scale package");
		};
		Write("content/regions/scale.instance.json", Encoded);
		Write(ScaleManifestReference, EncodePackageContentManifest(Manifest));
	}

	inline ContentAvailabilityConfiguration ScaleConfiguration(const std::filesystem::path &Directory, bool Node) {
		std::ifstream Input(Directory / ScaleManifestReference, std::ios::binary);
		std::string Encoded{std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
		auto Manifest = ParsePackageContentManifest(Encoded);
		if (!Manifest || Manifest->Entries.size() != 1) throw std::runtime_error("invalid scale manifest");
		const auto Digest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()));
		std::shared_ptr<IContentAvailabilityProvider> Provider;
		if (Node) {
#ifdef GARGANTUAN_CONTENT_SCALE_NODE
			auto Environment = [](const char *Name) -> std::string {
				const auto *Value = std::getenv(Name);
				if (!Value || !*Value) throw std::runtime_error("missing scale Node deployment configuration");
				return Value;
			};
			Provider = std::make_shared<host::NodeContentProvider>(host::NodeContentProviderConfiguration{
				.Endpoint = Environment("GARGANTUAN_ENGINE_ADAPTER_ENDPOINT"),
				.RootCertificateFile = Environment("GARGANTUAN_ENGINE_ADAPTER_ROOT_CERTIFICATE"),
				.WorkloadTokenEnvironment = "GARGANTUAN_ENGINE_ADAPTER_TOKEN",
			});
#else
			throw std::runtime_error("Node scale support is not linked in this benchmark");
#endif
		} else Provider = std::make_shared<LocalPackageContentProvider>(Directory, std::string(ScaleManifestReference), Digest);
		return {.Provider = std::move(Provider), .Package = Manifest->Package, .ManifestDigest = Digest,
			.Mode = ContentResidencyMode::OnDemand};
	}

	struct ContentScalePeer final {
		std::unordered_set<ObjectId> Objects;
		ObjectId Root;
		std::optional<network::CharacterControlTransition> Control;
		std::map<ObjectId, std::uint64_t> LastStateTick;
		std::unordered_set<ObjectId> RootMotionCharacters;
		std::uint64_t RootMotionStates = 0;
		std::uint64_t RootMotionMaximumGapTicks = 0;
		std::uint64_t MaximumGapTicks = 0;
		std::uint64_t StateCount = 0;
		std::uint64_t StateBytes = 0;
		std::uint64_t InputSequence = 0;
		std::string DisconnectDiagnostic;

		void Observe(std::span<const std::byte> Bytes) {
			if (Bytes.size() < 4) return;
			if (std::memcmp(Bytes.data(), "GRPL", 4) == 0) {
				auto Frame = network::DecodeReplicationFrame(Bytes);
				if (!Frame) throw std::runtime_error("scale peer received invalid GRPL");
				for (const auto &Operation : Frame->Operations) {
					if (const auto *Publish = std::get_if<network::PublishReplication>(&Operation.Intent)) {
						if (Publish->Name.starts_with("ContentScale")) {
							if (Publish->Name == ScaleRootName) {
								if (Publish->ClassName != "Folder" || Root.IsValid()) throw std::runtime_error("invalid scale root publication");
								Root = Publish->Object;
							} else {
								if (!Publish->Name.starts_with("ContentScalePart") || Publish->ClassName != "Part" ||
									!Publish->Parent || *Publish->Parent != Root) throw std::runtime_error("invalid scale hierarchy/class");
								const auto Index = std::stoull(Publish->Name.substr(16));
								const auto &Properties = Publish->Properties;
								const auto *Size = std::get_if<WireVector3>(&Properties.at("Size"));
								const auto *Transform = std::get_if<WireCFrame>(&Properties.at("CFrame"));
								if (!Size || Size->X != 2 || Size->Y != 1 || Size->Z != 3 ||
									Properties.at("Anchored") != WireValue(true) || Properties.at("CanCollide") != WireValue(false) ||
									!Transform || Transform->Components[0] != static_cast<float>(Index % 32) ||
									Transform->Components[1] != 2 || Transform->Components[2] != static_cast<float>(Index / 32))
									throw std::runtime_error("scale properties differ from immutable content");
							}
							Objects.insert(Publish->Object);
						}
					} else if (std::holds_alternative<network::UnpublishReplication>(Operation.Intent) ||
						std::holds_alternative<network::DestroyReplication>(Operation.Intent)) {
						const auto Object = network::GetReplicationObject(Operation.Intent);
						Objects.erase(Object);
						if (Object == Root) Root = {};
					}
				}
			} else if (std::memcmp(Bytes.data(), "GCHR", 4) == 0) {
				auto Message = network::DecodeCharacterMessage(Bytes);
				if (!Message) throw std::runtime_error("scale peer received invalid GCHR");
				if (const auto *Binding = std::get_if<network::CharacterControlTransition>(&*Message)) {
					if (Binding->Bound) Control = *Binding;
					else Control.reset();
				}
				auto ObserveState = [&](const network::CharacterAuthoritativeState &State) {
					auto &Last = LastStateTick[State.Character];
					if (RootMotionCharacters.contains(State.Character)) {
						++RootMotionStates;
						if (Last != 0 && State.AuthoritativeTick > Last)
							RootMotionMaximumGapTicks = std::max(RootMotionMaximumGapTicks, State.AuthoritativeTick - Last);
					}
					if (Last != 0 && State.AuthoritativeTick > Last)
						MaximumGapTicks = std::max(MaximumGapTicks, State.AuthoritativeTick - Last);
					Last = State.AuthoritativeTick;
					++StateCount;
				};
				if (const auto *Frame = std::get_if<network::CharacterStateFrame>(&*Message)) {
					for (const auto &State : Frame->GetStates()) ObserveState(State);
					StateBytes += Bytes.size();
				} else if (const auto *State = std::get_if<network::CharacterAuthoritativeState>(&*Message)) {
					ObserveState(*State);
					StateBytes += Bytes.size();
				}
			}
		}
	};
}
