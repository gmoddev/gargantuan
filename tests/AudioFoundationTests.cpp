#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/AudioMixService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <luacode.h>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Exception, typename Callback> void CheckThrows(Callback CallbackValue, const char *Message) {
		try {
			CallbackValue();
		} catch (const Exception &) {
			return;
		} catch (...) {}
		Check(false, Message);
	}

	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
	}

	void AppendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Value >> (Shift * 8)));
	}

	std::vector<std::uint8_t> MakeWave(
		std::uint32_t SampleRate,
		std::uint16_t Channels,
		std::uint32_t FrameCount,
		std::int16_t Left = 16'384,
		std::int16_t Right = 0
	) {
		const auto DataBytes = FrameCount * Channels * static_cast<std::uint32_t>(sizeof(std::int16_t));
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(44 + DataBytes);
		Bytes.insert(Bytes.end(), {'R', 'I', 'F', 'F'});
		AppendU32(Bytes, 36 + DataBytes);
		Bytes.insert(Bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
		AppendU32(Bytes, 16);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, Channels);
		AppendU32(Bytes, SampleRate);
		AppendU32(Bytes, SampleRate * Channels * static_cast<std::uint32_t>(sizeof(std::int16_t)));
		AppendU16(Bytes, Channels * static_cast<std::uint16_t>(sizeof(std::int16_t)));
		AppendU16(Bytes, 16);
		Bytes.insert(Bytes.end(), {'d', 'a', 't', 'a'});
		AppendU32(Bytes, DataBytes);
		for (std::uint32_t Frame = 0; Frame < FrameCount; ++Frame) {
			AppendU16(Bytes, static_cast<std::uint16_t>(Left));
			if (Channels == 2) AppendU16(Bytes, static_cast<std::uint16_t>(Right));
		}
		return Bytes;
	}

	void WriteBytes(const std::filesystem::path &Path, std::span<const std::uint8_t> Bytes) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		if (!Output) throw std::runtime_error("Could not write audio test fixture");
	}

	class CapturingBackend final : public gargantuan::IAudioBackend {
	  public:
		bool Available = true;
		bool FailSubmission = false;
		bool ShutDown = false;
		std::size_t QueuedFrames = 0;
		std::vector<float> LastSubmission;
		std::uint64_t SubmissionCount = 0;
		gargantuan::AudioBackendMetrics Metrics;

		bool IsAvailable() const override {
			return Available && !ShutDown;
		}
		std::uint32_t GetSampleRate() const override {
			return 48'000;
		}
		std::size_t GetQueuedFrames() override {
			return QueuedFrames;
		}
		bool Submit(std::span<const float> Samples) override {
			if (FailSubmission) {
				Available = false;
				++Metrics.SubmissionFailures;
				return false;
			}
			LastSubmission.assign(Samples.begin(), Samples.end());
			++SubmissionCount;
			Metrics.SubmittedFrames += Samples.size() / 2;
			return true;
		}
		void Clear() override {
			LastSubmission.clear();
		}
		void Shutdown() override {
			ShutDown = true;
		}
		std::string GetDiagnostic() const override {
			return Available ? std::string{} : "injected device failure";
		}
		gargantuan::AudioBackendMetrics GetMetrics() const override {
			return Metrics;
		}
	};

	float PeakChannel(const std::vector<float> &Samples, std::size_t Channel) {
		float Peak = 0.0f;
		for (std::size_t Index = Channel; Index < Samples.size(); Index += 2)
			Peak = std::max(Peak, std::abs(Samples[Index]));
		return Peak;
	}

	void TestMixServiceComposition() {
		using namespace gargantuan;
		const auto LocalWorld = std::make_shared<DataModel>();
		const auto ServerWorld = std::make_shared<DataModel>();
		Check(!LocalWorld->FindService("AudioMixService"), "the mixer has no authoring-scope service instance");
		CheckThrows<std::runtime_error>([&] { (void)LocalWorld->GetService("AudioMixService"); },
			"GetService cannot construct an unattached mixer");
		HeadlessRenderer LocalRenderer(Vector2(320.0f, 180.0f));
		HeadlessRenderer ServerRenderer(Vector2(320.0f, 180.0f));
		Engine Local(LocalWorld, &LocalRenderer);
		Engine Server(ServerWorld, &ServerRenderer, {}, {.AudioEnabled = true, .Mode = RuntimeMode::NetworkServer});
		Check(LocalWorld->FindService("AudioMixService").has_value(), "offline Engine installs one discoverable mixer");
		Check(!ServerWorld->FindService("AudioMixService") && !Server.Audio->IsAvailable(),
			"dedicated Engine creates neither a local mixer nor a device");
		CheckThrows<std::runtime_error>([&] { (void)ServerWorld->GetService("AudioMixService"); },
			"GetService cannot create a mixer in a dedicated world");
		const auto Mixer = std::dynamic_pointer_cast<AudioMixService>(LocalWorld->GetService("AudioMixService"));
		if (!Mixer) throw std::runtime_error("Offline AudioMixService fixture did not bind");
		const auto Revision = LocalWorld->GetAuthoritativeRevision();
		const auto Sequence = ChangeJournal::Get().CreateCursor(LocalWorld->GetObjectId()).NextSequence;
		constexpr std::string_view Source = R"(
			local Mixer = game:GetService("AudioMixService")
			local Category = Enum.SoundMixCategory
			assert(game:FindService("AudioMixService") == Mixer)
			assert(Mixer:GetMasterGain() == 1 and not Mixer:GetMasterMuted())
			Mixer:SetMasterGain(0.5)
			Mixer:SetCategoryGain(Category.UI, 0.25)
			Mixer:SetCategoryMuted(Category.Ambience, true)
			assert(Mixer:GetMasterGain() == 0.5 and Mixer:GetCategoryGain(Category.UI) == 0.25)
			assert(Mixer:GetCategoryMuted(Category.Ambience) and not Mixer:GetCategoryMuted(Category.UI))
			assert(not pcall(function() Mixer:SetMasterGain("0.25") end))
			assert(not pcall(function() Mixer:SetMasterGain(0 / 0) end))
			assert(not pcall(function() Mixer:SetMasterMuted(0) end))
			assert(not pcall(function() Mixer:SetCategoryGain(Enum.SoundPlaybackState.Playing, 0) end))
			assert(not pcall(function() Mixer:SetCategoryGain(Category.UI, 1.01) end))
			assert(Mixer:GetMasterGain() == 0.5 and Mixer:GetCategoryGain(Category.UI) == 0.25)
		)";
		std::size_t BytecodeSize = 0;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Local.Script->CompileOptions, &BytecodeSize);
		if (!Bytecode) throw std::runtime_error("Could not compile audio mixer fixture");
		const auto LoadStatus = luau_load(Local.Script->L, "audio-mix", Bytecode, BytecodeSize, 0);
		std::free(Bytecode);
		{
			ScriptSecurityScope Client(ScriptSecurityContext::ClientRuntime());
			const auto Status = LoadStatus == LUA_OK ? lua_pcall(Local.Script->L, 0, 0, 0) : LoadStatus;
			if (Status != LUA_OK) {
				const auto *Message = lua_tostring(Local.Script->L, -1);
				std::cerr << "[Audio:Tests] " << (Message ? Message : "unknown Luau error") << '\n';
			}
			Check(Status == LUA_OK, "client Luau can change mix settings and invalid arguments fail explicitly");
		}
		Check(LocalWorld->GetAuthoritativeRevision() == Revision &&
			ChangeJournal::Get().CreateCursor(LocalWorld->GetObjectId()).NextSequence == Sequence,
			"gain and mute do not create authoring revisions or journal records");
		const auto MixerSnapshot = CaptureSnapshotObject(Mixer);
		Check(MixerSnapshot.Properties.empty(), "mix settings are absent from structural replication properties");
		const std::array DeniedContexts{ScriptSecurityContext::ServerRuntime(), ScriptSecurityContext::StudioCoreUi(),
			ScriptSecurityContext{ScriptExecutionDomain::Client, {ScriptCapability::NetworkReceive}}};
		for (const auto &Context : DeniedContexts) {
			ScriptSecurityScope Denied(Context);
			CheckThrows<std::runtime_error>([&] { (void)Mixer->GetMasterGain(); }, "unauthorized mix reads fail");
			CheckThrows<std::runtime_error>([&] { Mixer->SetMasterMuted(true); }, "unauthorized mix writes fail");
		}
		{
			ExecutionDomainScope Worker(ExecutionDomain::Worker);
			CheckThrows<std::logic_error>([&] { Mixer->SetMasterGain(0.0); }, "workers cannot access the local mix");
		}
		LocalWorld->SetArchivable(true);
		Mixer->SetArchivable(true);
		auto OverlayChild = std::make_shared<Folder>();
		OverlayChild->SetName("RuntimeMixChild");
		OverlayChild->SetArchivable(true);
		OverlayChild->SetParent(Mixer);
		const auto Authored = std::make_shared<Sound>();
		Authored->SetArchivable(true);
		Authored->SetMixCategory(Enums::SoundMixCategory::Ambience);
		Authored->SetParent(LocalWorld);
		std::shared_ptr<Instance> Root = LocalWorld;
		const auto Saved = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		Check(Saved.find("AudioMixService") == std::string::npos && Saved.find("RuntimeMixChild") == std::string::npos,
			"the runtime mixer subtree is excluded from authored JSON regardless of Archivable");
		std::shared_ptr<Instance> ExplicitRoot = Mixer;
		CheckThrows<std::invalid_argument>([&] {
			(void)InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, ExplicitRoot);
		}, "the host-only mixer cannot be an explicit JSON document root");
		const std::array MixerRoots{ExplicitRoot};
		Check(!InstanceSerialization::CaptureGasDocument(MixerRoots, Gas::DocumentKind::Model),
			"the host-only mixer cannot be an explicit GASM root");
		std::istringstream MixerInput(R"({"Version":0,"Name":"AudioMixService","ClassName":"AudioMixService",
			"Properties":{},"Children":[]})");
		Check(!InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, MixerInput).Ok,
			"an authored JSON node cannot construct a host mixer");
		const std::array Roots{Root};
		const auto Place = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Place,
			Gas::PlaceMetadata{ProjectId::New(), {}});
		Check(Place.has_value() && std::ranges::none_of(Place->Nodes, [](const auto &Node) {
			return Node.Class == SchemaId::FromNativeName("Engine", "AudioMixService") || Node.Name == "RuntimeMixChild";
		}), "the runtime mixer subtree is excluded from authored GASP regardless of Archivable");
		if (!Place) throw std::runtime_error("Could not capture GASP mix fixture");
		const auto PlaceBytes = Gas::Encode(*Place);
		if (!PlaceBytes) throw std::runtime_error("Could not encode GASP mix fixture");
		const auto ParsedPlace = Gas::Decode(*PlaceBytes);
		if (!ParsedPlace) throw std::runtime_error("Could not decode GASP mix fixture");
		const auto RestoredPlace = InstanceSerialization::MaterializeGasDocument(*ParsedPlace, true);
		if (!RestoredPlace.Succeeded()) throw std::runtime_error("Could not materialize GASP mix fixture");
		const auto RestoredSound = std::dynamic_pointer_cast<Sound>(
			RestoredPlace.Roots.front()->FindFirstChildOfClass("Sound", false));
		Check(RestoredSound && RestoredSound->GetMixCategory() == Enums::SoundMixCategory::Ambience,
			"authored mix category persists in GASP");
		RestoredPlace.Roots.front()->Destroy();
		auto InjectedPlace = *ParsedPlace;
		InjectedPlace.Nodes.push_back({
			.Parent = 1,
			.Class = MixerSnapshot.ClassSchemaId,
			.DefinitionVersion = MixerSnapshot.ClassDefinitionVersion,
			.Name = MixerSnapshot.Name,
		});
		Check(!InstanceSerialization::MaterializeGasDocument(InjectedPlace, true).Succeeded(),
			"an authored GASP node cannot construct a host mixer");
		const auto Snapshot = CaptureSnapshot(LocalWorld);
		Check(std::ranges::none_of(Snapshot.Objects, [&](const auto &Object) {
			return Object.Id == MixerSnapshot.Id || Object.Id.ToObjectId() == OverlayChild->GetObjectId();
		}), "structural capture excludes the same local overlay subtree");

		network::ReplicationCoordinator Coordinator(ServerWorld);
		network::ReplicaApplier Replica;
		const network::ConnectionId Peer{1, 1};
		const network::ReplicationEpoch Epoch(1);
		const auto Baseline = Coordinator.AddPeer(Peer, Epoch);
		if (!Baseline.Succeeded() || !Replica.ApplyFrame(*Baseline.Frame).Succeeded())
			throw std::runtime_error("Could not initialize structural AudioMix denial fixture");
		const auto ReplicaRoot = Replica.GetReplicaRoot();
		const auto SequenceBefore = Replica.GetNextSequence();
		const network::ReplicationFrame InjectedFrame{
			network::ReplicationProtocolVersion, network::ReplicationMessageKind::Incremental, Epoch, SequenceBefore, {},
			{{Epoch, network::PublishReplication{
				.Object = MixerSnapshot.Id.ToObjectId(),
				.ClassSchemaId = MixerSnapshot.ClassSchemaId,
				.DefinitionVersion = MixerSnapshot.ClassDefinitionVersion,
				.Parent = ServerWorld->GetObjectId(),
				.ClassName = MixerSnapshot.ClassName,
				.Name = MixerSnapshot.Name,
			}}}};
		Check(Replica.ApplyFrame(InjectedFrame).Status == network::ReplicaApplyStatus::SemanticRejection &&
				Replica.GetReplicaRoot() == ReplicaRoot && Replica.GetNextSequence() == SequenceBefore,
			"structural preflight denies a host mixer node before changing the live replica");
		Replica.Reset();

		Local.Destroy();
		Check(!LocalWorld->FindService("AudioMixService"), "stopped Engine retires local service discovery");
		CheckThrows<std::runtime_error>([&] { (void)Mixer->GetMasterGain(); }, "retained mixer fails after runtime shutdown");
		Server.Destroy();
		LocalWorld->Destroy();
		ServerWorld->Destroy();
	}

	void TestCategoryMix(
		const std::shared_ptr<gargantuan::DataModel> &World,
		const std::shared_ptr<gargantuan::AssetService> &Assets,
		const std::string &StereoReference, const std::string &MonoReference, const std::string &ShortReference
	) {
		using namespace gargantuan;
		using Category = Enums::SoundMixCategory;
		auto Mixer = std::make_shared<AudioMixService>();
		Mixer->SetParent(World);
		const auto Other = std::make_shared<DataModel>();
		const auto OtherMixer = std::make_shared<AudioMixService>();
		OtherMixer->SetParent(Other);
		auto OtherAssets = std::dynamic_pointer_cast<AssetService>(Other->GetService("AssetService"));
		AudioRuntime Headless(OtherAssets, nullptr);
		CheckThrows<std::invalid_argument>([&] { Headless.AttachMixService(OtherMixer, RuntimeMode::NetworkServer); },
			"native server composition cannot bind a mix");
		CheckThrows<std::invalid_argument>([&] { Headless.AttachMixService(Mixer, RuntimeMode::NetworkClient); },
			"native client composition cannot attach another DataModel's mix");
		Headless.AttachMixService(OtherMixer, RuntimeMode::NetworkClient);
		Check(!Headless.IsAvailable() && OtherMixer->GetMasterGain() == 1.0,
			"a headless client still owns valid local preferences");
		auto BackendOwner = std::make_unique<CapturingBackend>();
		auto *Backend = BackendOwner.get();
		Backend->QueuedFrames = AudioRuntime::TargetQueuedFrames - AudioRuntime::MixBlockFrames;
		AudioRuntime Runtime(Assets, std::move(BackendOwner));
		Runtime.AttachMixService(Mixer, RuntimeMode::Offline);
		auto Effects = std::make_shared<Sound>();
		Effects->SetSoundId(StereoReference);
		Effects->SetLooped(true);
		auto Ui = std::make_shared<Sound>();
		Ui->SetSoundId(MonoReference);
		Ui->SetLooped(true);
		Ui->SetMixCategory(Category::UI);
		Runtime.RegisterSound(Effects);
		Runtime.RegisterSound(Ui);
		Mixer->SetMasterMuted(true);
		Effects->Play();
		Ui->Play();
		Runtime.Step(CFrame());
		Check(Backend->LastSubmission.size() == AudioRuntime::MixBlockFrames * 2 &&
				PeakChannel(Backend->LastSubmission, 0) == 0.0f && PeakChannel(Backend->LastSubmission, 1) == 0.0f &&
				Effects->GetTimePosition() > 0.0f && Ui->GetTimePosition() > 0.0f,
			"muting before the first playback suppresses its opening block while both voices advance");
		Mixer->SetMasterMuted(false);
		Runtime.Step(CFrame());
		Runtime.Step(CFrame());
		Check(Effects->GetMixCategory() == Category::Effects &&
			Backend->LastSubmission.size() == AudioRuntime::MixBlockFrames * 2,
			"newly defaulted sounds route to Effects and keep one fixed mix block");
		if (Backend->LastSubmission.size() != AudioRuntime::MixBlockFrames * 2)
			throw std::runtime_error("Audio fake backend did not receive the fixed mix block");
		Check(Backend->LastSubmission.front() == 1.0f && Backend->LastSubmission[1] == 0.5f &&
			Backend->LastSubmission.back() == 0.5f, "unity routing preserves stereo PCM exactly");
		Mixer->SetMasterGain(0.5);
		Mixer->SetCategoryGain(Category::UI, 0.5);
		Runtime.Step(CFrame());
		Check(Backend->LastSubmission.front() == 1.0f && Backend->LastSubmission.back() == 0.125f,
			"changed category and master gain ramp from prior to target gain across one block");
		Runtime.Step(CFrame());
		Check(Backend->LastSubmission.front() == 0.375f && Backend->LastSubmission[1] == 0.125f &&
			OtherMixer->GetMasterGain() == 1.0 && OtherMixer->GetCategoryGain(Category::UI) == 1.0,
			"master and category multiply per voice and preserve the separate client session");
		CheckThrows<std::invalid_argument>([&] { Mixer->SetMasterGain(std::numeric_limits<double>::infinity()); },
			"nonfinite native gains reject");
		CheckThrows<std::invalid_argument>([&] { Mixer->SetCategoryMuted(static_cast<Category>(-1), true); },
			"invalid native categories reject");
		CheckThrows<std::invalid_argument>([&] { Ui->SetMixCategory(static_cast<Category>(999)); },
			"invalid native Sound routing rejects");
		Mixer->SetCategoryMuted(Category::UI, true);
		Runtime.Step(CFrame());
		Runtime.Step(CFrame());
		Check(Backend->LastSubmission.front() == 0.25f && Backend->LastSubmission[1] == 0.0f &&
			Mixer->GetCategoryGain(Category::UI) == 0.5,
			"category mute leaves other voices audible and retains its configured gain");
		Mixer->SetMasterMuted(true);
		Runtime.Step(CFrame());
		const auto PreviousPosition = Ui->GetTimePosition();
		auto OneShot = std::make_shared<Sound>();
		OneShot->SetSoundId(ShortReference);
		std::size_t Ended = 0;
		const auto EndConnection = OneShot->Ended->Connect([&](std::monostate) { ++Ended; });
		Runtime.RegisterSound(OneShot);
		OneShot->Play();
		Runtime.Step(CFrame());
		Check(PeakChannel(Backend->LastSubmission, 0) == 0.0f && PeakChannel(Backend->LastSubmission, 1) == 0.0f &&
			Ui->GetTimePosition() > PreviousPosition && Ended == 1,
			"master mute leaves looping time, completion signals and fixed queue bounds intact");
		Mixer->SetMasterMuted(false);
		Mixer->SetCategoryMuted(Category::UI, false);
		Runtime.Step(CFrame());
		Runtime.Step(CFrame());
		Check(Backend->LastSubmission.front() == 0.375f && Backend->LastSubmission[1] == 0.125f,
			"unmute restores existing gains and active voices");
		EndConnection->Disconnect();
		Runtime.Shutdown();
		AudioRuntime Restarted(Assets, nullptr);
		Restarted.AttachMixService(Mixer, RuntimeMode::Offline);
		Check(Mixer->GetMasterGain() == 1.0 && Mixer->GetCategoryGain(Category::UI) == 1.0,
			"a replacement runtime starts with fresh preferences");
		Restarted.Shutdown();
		Headless.Shutdown();
		Mixer->Destroy();
		Other->Destroy();
	}
}

int main() {
	try {
		using namespace gargantuan;
		BootstrapNativeRuntimeSchema();

		const auto Root = std::filesystem::temp_directory_path() /
						  ("gargantuan-audio-foundation-" +
						   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(Root / "assets");
		WriteBytes(Root / "assets" / "tone.wav", MakeWave(8'000, 1, 4'000));
		WriteBytes(Root / "assets" / "stereo.wav", MakeWave(8'000, 2, 4'000, 16'384, 0));
		WriteBytes(Root / "assets" / "short.wav", MakeWave(8'000, 1, 16));
		WriteBytes(Root / "assets" / "bad.wav", std::vector<std::uint8_t>{'n', 'o', 'p', 'e'});
		WriteBytes(Root / "assets" / "long.wav", MakeWave(48'000, 2, 48'000 * 31));

		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto World = std::make_shared<DataModel>();
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		Check(Assets && WorkspaceValue, "audio tests establish canonical AssetService and Workspace");

		auto Imported = Assets->ImportProjectAsset(Mount, "assets/tone.wav", AssetKind::Audio, "Tone");
		auto StereoImported = Assets->ImportProjectAsset(Mount, "assets/stereo.wav", std::nullopt, "Stereo");
		auto ShortImported = Assets->ImportProjectAsset(Mount, "assets/short.wav", AssetKind::Audio, "Short");
		Check(
			Imported.Ok && Imported.Record && Imported.Record->Kind == AssetKind::Audio && StereoImported.Ok &&
				StereoImported.Record && ShortImported.Ok && ShortImported.Record,
			"PCM16 WAV imports explicitly and through extension detection"
		);
		auto Audio = Assets->ResolveAudio(Imported.Record->Reference.Value);
		Check(
			Audio && Audio->Value.SampleRate == 8'000 && Audio->Value.Channels == 1 &&
				Audio->Value.FrameCount == 4'000 && Audio->Value.Pcm16 && Audio->Value.Pcm16->size() == 4'000,
			"Audio resolves as bounded engine-owned PCM16 metadata and samples"
		);
		Check(
			!Assets->ImportProjectAsset(Mount, "assets/bad.wav", AssetKind::Audio, "Bad").Ok,
			"malformed WAV is rejected without publication"
		);
		Check(
			!Assets->ImportProjectAsset(Mount, "assets/long.wav", AssetKind::Audio, "Long").Ok,
			"audio longer than the 30-second resident limit is rejected"
		);
		Check(
			GetAssetKindName(AssetKind::Audio) == "Audio" && ParseAssetKind("Audio") == AssetKind::Audio,
			"Audio is a stable AssetKind name"
		);

		auto Snapshot = Assets->CaptureProjectAssets();
		auto ReopenedAssets = std::make_shared<AssetService>();
		ReopenedAssets->LoadProjectAssetSnapshot(Snapshot);
		auto Reopened = ReopenedAssets->ResolveAudio(Imported.Record->Reference.Value);
		Check(
			Reopened && Reopened->Value.FrameCount == Audio->Value.FrameCount && Reopened->Value.Pcm16 &&
				*Reopened->Value.Pcm16 == *Audio->Value.Pcm16,
			"canonical Audio artifact saves and reopens without source WAV decode"
		);

		auto PersistedSound = std::make_shared<Sound>();
		PersistedSound->SetArchivable(true);
		PersistedSound->SetName("PersistedSound");
		PersistedSound->SetSoundId(Imported.Record->Reference.Value);
		PersistedSound->SetVolume(0.4f);
		PersistedSound->SetMixCategory(Enums::SoundMixCategory::Music);
		PersistedSound->SetPlaybackSpeed(1.5f);
		PersistedSound->SetLooped(true);
		PersistedSound->SetTimePosition(0.2f);
		std::shared_ptr<Instance> SerializedRoot = PersistedSound;
		const auto Serialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, SerializedRoot
		);
		std::istringstream SerializedInput(Serialized);
		auto RestoredState = InstanceSerialization::Deserialize(
			InstanceSerialization::InstanceFormat::Json, SerializedInput
		);
		auto RestoredSound = std::dynamic_pointer_cast<Sound>(RestoredState.Instance);
		Check(RestoredState.Ok, "serialized Sound document reopens");
		Check(static_cast<bool>(RestoredSound), "serialized Sound reopens as a Sound Instance");
		if (RestoredSound) {
			Check(RestoredSound->GetSoundId() == Imported.Record->Reference.Value, "SoundId persists");
			Check(std::abs(RestoredSound->GetVolume() - 0.4f) < 0.0001f, "Volume persists");
			Check(RestoredSound->GetMixCategory() == Enums::SoundMixCategory::Music, "authored mix category persists in JSON");
			Check(std::abs(RestoredSound->GetPlaybackSpeed() - 1.5f) < 0.0001f, "PlaybackSpeed persists");
			Check(RestoredSound->GetLooped(), "Looped persists");
			Check(RestoredSound->GetTimePosition() == 0.0f, "TimePosition remains transient");
			Check(
				RestoredSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped,
				"playback state remains transient"
			);
		}
		std::istringstream LegacyInput(R"({"Version":0,"Name":"LegacySound","ClassName":"Sound",
			"Properties":{},"Children":[]})");
		const auto Legacy = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, LegacyInput);
		const auto LegacySound = std::dynamic_pointer_cast<Sound>(Legacy.Instance);
		Check(Legacy.Ok && LegacySound && LegacySound->GetMixCategory() == Enums::SoundMixCategory::Effects,
			"legacy Sound files without an authored category default to Effects");
		if (LegacySound) LegacySound->Destroy();
		const std::array ModelRoots{SerializedRoot};
		const auto Model = InstanceSerialization::CaptureGasDocument(ModelRoots, Gas::DocumentKind::Model);
		if (!Model) throw std::runtime_error("Could not capture audio GASM fixture");
		const auto Bytes = Gas::Encode(*Model);
		if (!Bytes) throw std::runtime_error("Could not encode audio GASM fixture");
		const auto Parsed = Gas::Decode(*Bytes);
		if (!Parsed) throw std::runtime_error("Could not decode audio GASM fixture");
		const auto RestoredModel = InstanceSerialization::MaterializeGasDocument(*Parsed, false);
		const auto GasSound = RestoredModel.Succeeded() ? std::dynamic_pointer_cast<Sound>(RestoredModel.Roots.front()) : nullptr;
		Check(GasSound && GasSound->GetMixCategory() == Enums::SoundMixCategory::Music,
			"authored mix category persists in GASM");
		if (GasSound) GasSound->Destroy();
		PersistedSound->SetParent(WorkspaceValue);
		const auto SoundSnapshot = CaptureSnapshotObject(PersistedSound);
		Check(SoundSnapshot.Properties.at("MixCategory") == WireValue{WireEnumItem{"SoundMixCategory", "Music"}},
			"structural replication carries authored mix category as a native enum");
		PersistedSound->Destroy();
		TestMixServiceComposition();
		TestCategoryMix(World, Assets, StereoImported.Record->Reference.Value,
			Imported.Record->Reference.Value, ShortImported.Record->Reference.Value);

		CheckThrows<std::invalid_argument>(
			[] {
				auto Invalid = std::make_shared<Sound>();
				Invalid->SetSoundId("assets/tone.wav");
			},
			"SoundId rejects raw source paths"
		);
		CheckThrows<std::invalid_argument>(
			[] {
				auto Invalid = std::make_shared<Sound>();
				Invalid->SetRollOffMaxDistance(5.0f);
				Invalid->SetRollOffMinDistance(6.0f);
			},
			"Sound rolloff endpoints reject an inverted range"
		);

		std::vector<std::pair<std::string, std::string>> Diagnostics;
		auto BackendOwner = std::make_unique<CapturingBackend>();
		auto *Backend = BackendOwner.get();
		AudioRuntime Runtime(Assets, std::move(BackendOwner), [&](std::string Code, std::string Message) {
			Diagnostics.emplace_back(std::move(Code), std::move(Message));
		});

		auto UiSound = std::make_shared<Sound>();
		UiSound->SetSoundId(StereoImported.Record->Reference.Value);
		Runtime.RegisterSound(UiSound);
		UiSound->Play();
		Runtime.Step(CFrame());
		Check(
			UiSound->GetPlaybackState() == Enums::SoundPlaybackState::Playing &&
				PeakChannel(Backend->LastSubmission, 0) > 0.45f && PeakChannel(Backend->LastSubmission, 1) < 0.001f,
			"non-spatial stereo playback preserves authored left/right channels"
		);
		const auto PositionBeforePause = UiSound->GetTimePosition();
		const auto SubmissionsBeforeHold = Backend->SubmissionCount;
		Runtime.SetPlaybackHeld(true);
		Runtime.Step(CFrame());
		Check(Backend->SubmissionCount == SubmissionsBeforeHold && UiSound->GetTimePosition() == PositionBeforePause &&
			UiSound->GetPlaybackState() == Enums::SoundPlaybackState::Playing, "gameplay hold freezes the mixer and preserves voice intent");
		Runtime.SetPlaybackHeld(false);
		UiSound->Pause();
		Runtime.Step(CFrame());
		Check(
			UiSound->GetPlaybackState() == Enums::SoundPlaybackState::Paused &&
				UiSound->GetTimePosition() == PositionBeforePause,
			"Pause preserves authoritative TimePosition"
		);
		UiSound->Resume();
		Runtime.Step(CFrame());
		Check(UiSound->GetTimePosition() > PositionBeforePause, "Resume continues from the paused position");
		UiSound->SetTimePosition(0.1f);
		Runtime.Step(CFrame());
		Check(UiSound->GetTimePosition() >= 0.1f, "setting TimePosition during playback seeks the active voice");
		UiSound->Stop();
		Runtime.Step(CFrame());
		Check(
			UiSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped && UiSound->GetTimePosition() == 0.0f,
			"Stop releases the voice and resets TimePosition"
		);

		auto Slow = std::make_shared<Sound>();
		auto Fast = std::make_shared<Sound>();
		Slow->SetSoundId(Imported.Record->Reference.Value);
		Fast->SetSoundId(Imported.Record->Reference.Value);
		Fast->SetPlaybackSpeed(2.0f);
		Runtime.RegisterSound(Slow);
		Runtime.RegisterSound(Fast);
		Slow->Play();
		Fast->Play();
		Runtime.Step(CFrame());
		Check(
			Fast->GetTimePosition() > Slow->GetTimePosition() * 1.9f,
			"PlaybackSpeed uses bounded rate resampling and advances pitch/speed together"
		);
		Slow->Stop();
		Fast->Stop();
		Runtime.Step(CFrame());

		auto SpatialPart = std::make_shared<Part>();
		SpatialPart->SetAnchored(true);
		SpatialPart->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
		SpatialPart->SetParent(WorkspaceValue);
		auto SpatialSound = std::make_shared<Sound>();
		SpatialSound->SetSoundId(Imported.Record->Reference.Value);
		SpatialSound->SetRollOffMinDistance(1.0f);
		SpatialSound->SetRollOffMaxDistance(100.0f);
		SpatialSound->SetParent(SpatialPart);
		Runtime.RegisterSound(SpatialSound);
		SpatialSound->Play();
		Runtime.Step(CFrame());
		Check(
			PeakChannel(Backend->LastSubmission, 1) > PeakChannel(Backend->LastSubmission, 0) * 10.0f,
			"a Sound under a right-side Part uses listener-space stereo panning"
		);
		SpatialPart->SetCFrame(CFrame(-20.0f, 0.0f, 0.0f));
		SpatialSound->Play();
		Runtime.Step(CFrame());
		Check(
			PeakChannel(Backend->LastSubmission, 0) > PeakChannel(Backend->LastSubmission, 1) * 10.0f,
			"moving a parent Part updates positional panning without redundant Sound position"
		);
		SpatialPart->SetCFrame(CFrame(100.0f, 0.0f, 0.0f));
		SpatialSound->SetRollOffMaxDistance(50.0f);
		SpatialSound->Play();
		Runtime.Step(CFrame());
		Check(
			PeakChannel(Backend->LastSubmission, 0) < 0.001f && PeakChannel(Backend->LastSubmission, 1) < 0.001f,
			"linear attenuation is silent at and beyond RollOffMaxDistance"
		);

		auto AttachmentValue = std::make_shared<Attachment>();
		AttachmentValue->SetCFrame(CFrame(10.0f, 0.0f, 0.0f));
		AttachmentValue->SetParent(SpatialPart);
		auto AttachedSound = std::make_shared<Sound>();
		AttachedSound->SetSoundId(Imported.Record->Reference.Value);
		AttachedSound->SetRollOffMaxDistance(200.0f);
		AttachedSound->SetParent(AttachmentValue);
		Runtime.RegisterSound(AttachedSound);
		SpatialPart->SetCFrame(CFrame(-20.0f, 0.0f, 0.0f));
		AttachedSound->Play();
		Runtime.Step(CFrame());
		Check(
			PeakChannel(Backend->LastSubmission, 0) > PeakChannel(Backend->LastSubmission, 1),
			"Attachment local transform composes through its BasePart anchor"
		);
		SpatialSound->Stop();
		AttachedSound->Stop();
		Runtime.Step(CFrame());

		auto Quiet = std::make_shared<Sound>();
		Quiet->SetSoundId(Imported.Record->Reference.Value);
		Quiet->SetVolume(0.25f);
		Runtime.RegisterSound(Quiet);
		Quiet->Play();
		Runtime.Step(CFrame());
		Check(PeakChannel(Backend->LastSubmission, 0) <= 0.126f, "Volume applies a bounded linear gain before mixing");
		Quiet->Stop();

		auto ShortSound = std::make_shared<Sound>();
		ShortSound->SetSoundId(ShortImported.Record->Reference.Value);
		int EndedCount = 0;
		auto EndedConnection = ShortSound->Ended->Connect([&](std::monostate) { ++EndedCount; });
		Runtime.RegisterSound(ShortSound);
		ShortSound->Play();
		for (int Index = 0; Index < 4; ++Index)
			Runtime.Step(CFrame());
		Check(
			ShortSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped && EndedCount == 1,
			"non-looped playback fires Ended exactly once and returns to Stopped"
		);
		ShortSound->SetLooped(true);
		ShortSound->Play();
		for (int Index = 0; Index < 8; ++Index)
			Runtime.Step(CFrame());
		Check(
			ShortSound->GetPlaybackState() == Enums::SoundPlaybackState::Playing && EndedCount == 1,
			"loop boundaries wrap deterministically without Ended or queue growth"
		);
		ShortSound->Stop();

		WriteBytes(Root / "assets" / "tone.wav", MakeWave(8'000, 1, 4'000, 8'192));
		const auto Reimported = Assets->ReimportProjectAsset(Mount, Imported.Record->Reference.Value);
		Check(
			Reimported.Ok && Reimported.Record && Reimported.Record->ContentRevision > Imported.Record->ContentRevision,
			"Audio reimport preserves AssetId and advances changed content revision"
		);

		auto ReferencingSound = std::make_shared<Sound>();
		ReferencingSound->SetSoundId(Imported.Record->Reference.Value);
		ReferencingSound->SetParent(WorkspaceValue);
		Check(
			!Assets->DeleteProjectAsset(Imported.Record->Reference.Value).Ok,
			"AssetService refuses deletion while a live Sound references the Audio asset"
		);
		ReferencingSound->Destroy();

		auto DestroyedSound = std::make_shared<Sound>();
		DestroyedSound->SetSoundId(Imported.Record->Reference.Value);
		Runtime.RegisterSound(DestroyedSound);
		DestroyedSound->Play();
		Runtime.Step(CFrame());
		DestroyedSound->Destroy();
		Runtime.Step(CFrame());
		Check(Runtime.GetMetrics().ActiveVoices == 0, "destroying a Sound retires its voice without a stale callback");

		auto ParentPart = std::make_shared<Part>();
		ParentPart->SetParent(WorkspaceValue);
		auto ChildSound = std::make_shared<Sound>();
		ChildSound->SetSoundId(Imported.Record->Reference.Value);
		ChildSound->SetParent(ParentPart);
		Runtime.RegisterSound(ChildSound);
		ChildSound->Play();
		Runtime.Step(CFrame());
		ParentPart->Destroy();
		Runtime.Step(CFrame());
		Check(Runtime.GetMetrics().ActiveVoices == 0, "destroying a Sound parent retires playback safely");

		{
			auto UnavailableBackend = std::make_unique<CapturingBackend>();
			UnavailableBackend->Available = false;
			AudioRuntime Unavailable(Assets, std::move(UnavailableBackend));
			auto Silent = std::make_shared<Sound>();
			Silent->SetSoundId(Imported.Record->Reference.Value);
			Unavailable.RegisterSound(Silent);
			Silent->Play();
			Unavailable.Step(CFrame());
			Check(
				!Unavailable.IsAvailable() && Silent->GetPlaybackState() == Enums::SoundPlaybackState::Stopped,
				"device-unavailable playback fails open without retaining an active voice"
			);
		}

		auto MissingSound = std::make_shared<Sound>();
		MissingSound->SetSoundId("asset://11111111111111111111111111111111");
		Runtime.RegisterSound(MissingSound);
		MissingSound->Play();
		Runtime.Step(CFrame());
		Check(
			MissingSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped &&
				std::ranges::any_of(
					Diagnostics, [](const auto &Diagnostic) { return Diagnostic.first == "AssetUnavailable"; }
				),
			"a missing canonical Audio asset fails open with one bounded diagnostic"
		);

		auto RepeatedSound = std::make_shared<Sound>();
		RepeatedSound->SetSoundId(Imported.Record->Reference.Value);
		Runtime.RegisterSound(RepeatedSound);
		for (int Cycle = 0; Cycle < 100; ++Cycle) {
			RepeatedSound->Play();
			Runtime.Step(CFrame());
			RepeatedSound->Stop();
			Runtime.Step(CFrame());
		}
		Check(
			RepeatedSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped &&
				RepeatedSound->GetTimePosition() == 0.0f,
			"repeated Play/Stop cycles retain coherent semantic state and release each voice"
		);

		{
			auto FailingBackend = std::make_unique<CapturingBackend>();
			FailingBackend->FailSubmission = true;
			AudioRuntime Failing(Assets, std::move(FailingBackend));
			auto DeviceLostSound = std::make_shared<Sound>();
			DeviceLostSound->SetSoundId(Imported.Record->Reference.Value);
			Failing.RegisterSound(DeviceLostSound);
			DeviceLostSound->Play();
			Failing.Step(CFrame());
			Check(
				DeviceLostSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped,
				"injected device loss retires voices while game execution continues"
			);
		}

		{
			auto LimitBackend = std::make_unique<CapturingBackend>();
			AudioRuntime Limited(Assets, std::move(LimitBackend));
			std::vector<std::shared_ptr<Sound>> Voices;
			Voices.reserve(AudioRuntime::MaximumVoices + 1);
			for (std::size_t Index = 0; Index <= AudioRuntime::MaximumVoices; ++Index) {
				auto Voice = std::make_shared<Sound>();
				Voice->SetSoundId(Imported.Record->Reference.Value);
				Voice->SetLooped(true);
				Limited.RegisterSound(Voice);
				Voice->Play();
				Voices.push_back(std::move(Voice));
			}
			Limited.Step(CFrame());
			const auto Metrics = Limited.GetMetrics();
			Check(
				Metrics.ActiveVoices == AudioRuntime::MaximumVoices && Metrics.VoiceRejections == 1,
				"the 256-voice bound deterministically rejects the newest excess voice"
			);
			Limited.Shutdown();
			Check(
				std::ranges::none_of(
					Voices,
					[](const auto &Voice) { return Voice->GetPlaybackState() != Enums::SoundPlaybackState::Stopped; }
				),
				"shutdown during playback retires every semantic voice before backend teardown"
			);
		}

		Runtime.Shutdown();
		Check(
			!Runtime.IsAvailable() && Runtime.GetMetrics().ActiveVoices == 0,
			"AudioRuntime shutdown is bounded and leaves no active voices"
		);
		EndedConnection->Disconnect();
		World->Destroy();
		std::filesystem::remove_all(Root);

		if (Failures != 0) {
			std::cerr << Failures << " audio foundation checks failed\n";
			return 1;
		}
		std::cout << "Audio Foundation 1 tests passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "Unhandled audio foundation exception: " << Error.what() << '\n';
		return 1;
	} catch (...) {
		std::cerr << "Unhandled non-standard audio foundation exception\n";
		return 1;
	}
}
