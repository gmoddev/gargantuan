#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
			return 0;
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
			Check(std::abs(RestoredSound->GetPlaybackSpeed() - 1.5f) < 0.0001f, "PlaybackSpeed persists");
			Check(RestoredSound->GetLooped(), "Looped persists");
			Check(RestoredSound->GetTimePosition() == 0.0f, "TimePosition remains transient");
			Check(
				RestoredSound->GetPlaybackState() == Enums::SoundPlaybackState::Stopped,
				"playback state remains transient"
			);
		}

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
