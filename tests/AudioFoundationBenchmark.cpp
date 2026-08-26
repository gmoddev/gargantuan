#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {
	std::atomic<std::uint64_t> AllocationCount = 0;
}

void *operator new(std::size_t Size) {
	AllocationCount.fetch_add(1, std::memory_order_relaxed);
	if (auto *Value = std::malloc(Size)) return Value;
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept {
	std::free(Value);
}
void operator delete(void *Value, std::size_t) noexcept {
	std::free(Value);
}

namespace {
	using Clock = std::chrono::steady_clock;

	void AppendU16(std::vector<std::uint8_t> &Bytes, std::uint16_t Value) {
		Bytes.push_back(static_cast<std::uint8_t>(Value));
		Bytes.push_back(static_cast<std::uint8_t>(Value >> 8));
	}

	void AppendU32(std::vector<std::uint8_t> &Bytes, std::uint32_t Value) {
		for (std::size_t Shift = 0; Shift < 4; ++Shift)
			Bytes.push_back(static_cast<std::uint8_t>(Value >> (Shift * 8)));
	}

	std::vector<std::uint8_t> MakeWave(std::uint32_t FrameCount) {
		constexpr std::uint32_t SampleRate = 48'000;
		constexpr std::uint16_t Channels = 1;
		const auto DataBytes = FrameCount * sizeof(std::int16_t);
		std::vector<std::uint8_t> Bytes;
		Bytes.reserve(44 + DataBytes);
		Bytes.insert(Bytes.end(), {'R', 'I', 'F', 'F'});
		AppendU32(Bytes, 36 + DataBytes);
		Bytes.insert(Bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
		AppendU32(Bytes, 16);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, Channels);
		AppendU32(Bytes, SampleRate);
		AppendU32(Bytes, SampleRate * sizeof(std::int16_t));
		AppendU16(Bytes, sizeof(std::int16_t));
		AppendU16(Bytes, 16);
		Bytes.insert(Bytes.end(), {'d', 'a', 't', 'a'});
		AppendU32(Bytes, DataBytes);
		for (std::uint32_t Frame = 0; Frame < FrameCount; ++Frame) {
			const auto Phase = static_cast<double>(Frame) * 440.0 * 6.283185307179586 / SampleRate;
			AppendU16(Bytes, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::sin(Phase) * 8'192.0)));
		}
		return Bytes;
	}

	void WriteBytes(const std::filesystem::path &Path, std::span<const std::uint8_t> Bytes) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
		if (!Output) throw std::runtime_error("Could not write audio benchmark fixture");
	}

	class BenchmarkBackend final : public gargantuan::IAudioBackend {
	  public:
		bool IsAvailable() const override {
			return Available;
		}
		std::uint32_t GetSampleRate() const override {
			return 48'000;
		}
		std::size_t GetQueuedFrames() override {
			if (Submitted) ++Metrics.ObservedEmptyQueueEvents;
			return 0;
		}
		bool Submit(std::span<const float> Samples) override {
			if (!Available || Samples.empty()) return false;
			Submitted = true;
			++SubmissionCount;
			Metrics.SubmittedFrames += Samples.size() / 2;
			return true;
		}
		void Clear() override {}
		void Shutdown() override {
			Available = false;
		}
		std::string GetDiagnostic() const override {
			return {};
		}
		gargantuan::AudioBackendMetrics GetMetrics() const override {
			return Metrics;
		}

		bool Available = true;
		bool Submitted = false;
		std::uint64_t SubmissionCount = 0;
		gargantuan::AudioBackendMetrics Metrics;
	};

	struct VoiceResult {
		std::uint64_t ElapsedNanoseconds = 0;
		std::uint64_t MixCpuNanoseconds = 0;
		std::uint64_t Allocations = 0;
		std::uint64_t Admissions = 0;
		std::uint64_t Rejections = 0;
		std::uint64_t SubmittedFrames = 0;
		std::uint64_t QueueEmptyObservations = 0;
	};

	VoiceResult RunVoices(
		const std::shared_ptr<gargantuan::AssetService> &Assets,
		std::string_view Reference,
		std::size_t VoiceCount,
		std::size_t Steps,
		bool Positional
	) {
		using namespace gargantuan;
		auto BackendOwner = std::make_unique<BenchmarkBackend>();
		auto *Backend = BackendOwner.get();
		AudioRuntime Runtime(Assets, std::move(BackendOwner));
		auto Anchor = std::make_shared<Part>();
		std::vector<std::shared_ptr<Sound>> Sounds;
		Sounds.reserve(VoiceCount);
		for (std::size_t Index = 0; Index < VoiceCount; ++Index) {
			auto SoundValue = std::make_shared<Sound>();
			SoundValue->SetSoundId(std::string(Reference));
			SoundValue->SetLooped(true);
			if (Positional) SoundValue->SetParent(Anchor);
			Runtime.RegisterSound(SoundValue);
			Sounds.push_back(std::move(SoundValue));
		}

		const auto LatencyStart = Clock::now();
		for (const auto &SoundValue : Sounds)
			SoundValue->Play();
		Runtime.Step(CFrame());
		const auto FirstSubmission = Clock::now();
		Runtime.Step(CFrame());
		const auto AllocationsBefore = AllocationCount.load(std::memory_order_relaxed);
		const auto Started = Clock::now();
		for (std::size_t Step = 0; Step < Steps; ++Step) {
			if (Positional)
				Anchor->SetCFrame(
					CFrame(glm::vec3(static_cast<float>(static_cast<int>(Step % 200) - 100), 0.0f, 15.0f))
				);
			Runtime.Step(CFrame());
		}
		const auto Finished = Clock::now();
		const auto Metrics = Runtime.GetMetrics();
		const auto AllocationsAfter = AllocationCount.load(std::memory_order_relaxed);
		std::cout << "PlayToFirstSubmissionUs="
				  << std::chrono::duration_cast<std::chrono::microseconds>(FirstSubmission - LatencyStart).count()
				  << '\n';
		Runtime.Shutdown();
		return {
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Finished - Started).count()
			),
			Metrics.MixCpuNanoseconds,
			AllocationsAfter - AllocationsBefore,
			Metrics.VoiceAdmissions,
			Metrics.VoiceRejections,
			Metrics.Backend.SubmittedFrames,
			Metrics.Backend.ObservedEmptyQueueEvents,
		};
	}

	void PrintResult(std::string_view Name, std::size_t Steps, const VoiceResult &Result) {
		std::cout << "Case=" << Name << " Steps=" << Steps << " WallUs=" << Result.ElapsedNanoseconds / 1'000
				  << " MixCpuUs=" << Result.MixCpuNanoseconds / 1'000 << " SteadyAllocations=" << Result.Allocations
				  << " Admissions=" << Result.Admissions << " Rejections=" << Result.Rejections
				  << " SubmittedFrames=" << Result.SubmittedFrames
				  << " QueueEmptyObservations=" << Result.QueueEmptyObservations << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	using namespace gargantuan;
	const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
	const std::size_t Steps = Quick ? 20 : 500;
	BootstrapNativeRuntimeSchema();
	const auto Root = std::filesystem::temp_directory_path() /
					  ("gargantuan-audio-benchmark-" + std::to_string(Clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	} CleanupValue{Root};
	WriteBytes(Root / "assets" / "short.wav", MakeWave(12'000));
	WriteBytes(Root / "assets" / "largest.wav", MakeWave(AssetLimits::MaximumAudioFrames));
	DiskFilesystem Filesystem(Root);
	SourceMount Mount(Filesystem);
	auto World = std::make_shared<DataModel>();
	auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
	if (!Assets) return 1;

	const auto DecodeStart = Clock::now();
	auto Largest = Assets->ImportProjectAsset(Mount, "assets/largest.wav", AssetKind::Audio, "Largest resident clip");
	const auto DecodeEnd = Clock::now();
	auto Short = Assets->ImportProjectAsset(Mount, "assets/short.wav", AssetKind::Audio, "Short SFX");
	if (!Largest.Ok || !Largest.Record || !Short.Ok || !Short.Record) return 1;
	std::cout << "LargestDecodeUs="
			  << std::chrono::duration_cast<std::chrono::microseconds>(DecodeEnd - DecodeStart).count() << '\n';

	const auto ResolveIterations = Quick ? 1'000u : 100'000u;
	const auto ResolveStart = Clock::now();
	std::uint64_t ResolvedFrames = 0;
	for (std::size_t Index = 0; Index < ResolveIterations; ++Index) {
		auto Resolved = Assets->ResolveAudio(Largest.Record->Reference.Value);
		if (Resolved) ResolvedFrames += Resolved->Value.FrameCount;
	}
	const auto ResolveEnd = Clock::now();
	std::cout << "AssetResolveIterations=" << ResolveIterations << " AssetResolveUs="
			  << std::chrono::duration_cast<std::chrono::microseconds>(ResolveEnd - ResolveStart).count()
			  << " ResolvedFrames=" << ResolvedFrames << '\n';

	for (const auto Count : {1u, 32u, 128u, 257u})
		PrintResult(
			std::to_string(Count) + "Voices",
			Steps,
			RunVoices(Assets, Largest.Record->Reference.Value, Count, Steps, false)
		);
	PrintResult("MovingPositional32", Steps, RunVoices(Assets, Largest.Record->Reference.Value, 32, Steps, true));
	PrintResult("LoopingShortSfx", Steps, RunVoices(Assets, Short.Record->Reference.Value, 32, Steps, false));
	return 0;
}
