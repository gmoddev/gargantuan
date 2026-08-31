#include "gargantuan/animation/AnimationRuntime.hpp"

#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace gargantuan {
	namespace {
		struct LocalPose {
			glm::vec3 Translation{0.0f};
			glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
			glm::vec3 Scale{1.0f};
		};

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const glm::vec4 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) &&
				std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		bool IsFinite(const glm::mat4 &Value) {
			for (glm::length_t Column = 0; Column < 4; ++Column)
				if (!IsFinite(Value[Column])) return false;
			return true;
		}

		bool IsValidIdentitySummary(std::span<const ObjectId> Objects, std::size_t MaximumObjects) {
			if (Objects.size() > MaximumObjects) return false;
			ObjectId Previous;
			for (const auto Object : Objects) {
				if (!Object.IsValid() || (Previous.IsValid() && !(Previous < Object))) return false;
				Previous = Object;
			}
			return true;
		}

		bool ContainsIdentity(std::span<const ObjectId> Objects, ObjectId Object) {
			return std::ranges::binary_search(Objects, Object);
		}

		std::uint64_t StableScheduleHash(ObjectId Object) {
			std::uint64_t Value = (static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot;
			Value += 0x9e3779b97f4a7c15ull;
			Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
			Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
			return Value ^ (Value >> 31);
		}

		AnimationDistanceBand ClassifyDistance(float Distance) {
			if (Distance <= AnimationUpdatePolicySettings::NearExitDistance) return AnimationDistanceBand::Near;
			if (Distance <= AnimationUpdatePolicySettings::MidExitDistance) return AnimationDistanceBand::Mid;
			if (Distance <= AnimationUpdatePolicySettings::FarExitDistance) return AnimationDistanceBand::Far;
			return AnimationDistanceBand::VeryFar;
		}

		AnimationDistanceBand ApplyDistanceHysteresis(AnimationDistanceBand Current, float Distance) {
			switch (Current) {
			case AnimationDistanceBand::Near:
				return Distance > AnimationUpdatePolicySettings::NearExitDistance ? ClassifyDistance(Distance)
																				  : Current;
			case AnimationDistanceBand::Mid:
				if (Distance < AnimationUpdatePolicySettings::NearEnterDistance) return AnimationDistanceBand::Near;
				return Distance > AnimationUpdatePolicySettings::MidExitDistance ? ClassifyDistance(Distance) : Current;
			case AnimationDistanceBand::Far:
				if (Distance < AnimationUpdatePolicySettings::MidEnterDistance) return ClassifyDistance(Distance);
				return Distance > AnimationUpdatePolicySettings::FarExitDistance ? AnimationDistanceBand::VeryFar
																				 : Current;
			case AnimationDistanceBand::VeryFar:
				return Distance < AnimationUpdatePolicySettings::FarEnterDistance ? ClassifyDistance(Distance)
																				  : Current;
			}
			return AnimationDistanceBand::Near;
		}

		std::uint64_t CadenceForBand(AnimationDistanceBand Band) {
			switch (Band) {
			case AnimationDistanceBand::Mid:
				return AnimationUpdatePolicySettings::MidCadenceNanoseconds;
			case AnimationDistanceBand::Far:
				return AnimationUpdatePolicySettings::FarCadenceNanoseconds;
			case AnimationDistanceBand::VeryFar:
				return AnimationUpdatePolicySettings::VeryFarCadenceNanoseconds;
			case AnimationDistanceBand::Near:
				return 0;
			}
			return 0;
		}

		int PolicyDemand(AnimationUpdatePolicyClass Policy) {
			switch (Policy) {
			case AnimationUpdatePolicyClass::VisualFrozen:
				return 0;
			case AnimationUpdatePolicyClass::ReducedRate:
				return 1;
			case AnimationUpdatePolicyClass::FullRate:
				return 2;
			case AnimationUpdatePolicyClass::SemanticRequired:
				return 3;
			}
			return 0;
		}

		std::optional<glm::vec3> SampleVector(
			const std::shared_ptr<const std::vector<ImportedAnimationVectorKey>> &Keys,
			AssetAnimationInterpolation Interpolation,
			float Time,
			AnimationRuntimeMetrics *Metrics
		) {
			if (!Keys || Keys->empty()) return std::nullopt;
			const auto LookupStarted = Metrics ? std::chrono::steady_clock::now()
											   : std::chrono::steady_clock::time_point{};
			auto RecordLookup = [&] {
				if (!Metrics) return;
				Metrics->KeyframeLookupCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - LookupStarted
					)
						.count()
				);
			};
			if (Time <= Keys->front().Time) {
				RecordLookup();
				return Keys->front().Value;
			}
			if (Time >= Keys->back().Time) {
				RecordLookup();
				return Keys->back().Value;
			}
			const auto Upper = std::upper_bound(
				Keys->begin(), Keys->end(), Time, [](float Value, const ImportedAnimationVectorKey &Key) {
					return Value < Key.Time;
				}
			);
			RecordLookup();
			const auto &Right = *Upper;
			const auto &Left = *(Upper - 1);
			if (Interpolation == AssetAnimationInterpolation::Step) return Left.Value;
			const auto InterpolationStarted = Metrics ? std::chrono::steady_clock::now()
													  : std::chrono::steady_clock::time_point{};
			const auto Alpha = (Time - Left.Time) / (Right.Time - Left.Time);
			const auto Result = glm::mix(Left.Value, Right.Value, Alpha);
			if (Metrics)
				Metrics->InterpolationCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - InterpolationStarted
					)
						.count()
				);
			return Result;
		}

		std::optional<glm::quat> SampleRotation(
			const std::shared_ptr<const std::vector<ImportedAnimationRotationKey>> &Keys,
			AssetAnimationInterpolation Interpolation,
			float Time,
			AnimationRuntimeMetrics *Metrics
		) {
			if (!Keys || Keys->empty()) return std::nullopt;
			const auto LookupStarted = Metrics ? std::chrono::steady_clock::now()
											   : std::chrono::steady_clock::time_point{};
			auto RecordLookup = [&] {
				if (!Metrics) return;
				Metrics->KeyframeLookupCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - LookupStarted
					)
						.count()
				);
			};
			if (Time <= Keys->front().Time) {
				RecordLookup();
				return Keys->front().Value;
			}
			if (Time >= Keys->back().Time) {
				RecordLookup();
				return Keys->back().Value;
			}
			const auto Upper = std::upper_bound(
				Keys->begin(), Keys->end(), Time, [](float Value, const ImportedAnimationRotationKey &Key) {
					return Value < Key.Time;
				}
			);
			RecordLookup();
			const auto &Right = *Upper;
			const auto &Left = *(Upper - 1);
			if (Interpolation == AssetAnimationInterpolation::Step) return Left.Value;
			const auto InterpolationStarted = Metrics ? std::chrono::steady_clock::now()
													  : std::chrono::steady_clock::time_point{};
			auto RightRotation = Right.Value;
			if (glm::dot(Left.Value, RightRotation) < 0.0f) RightRotation = -RightRotation;
			const auto Alpha = (Time - Left.Time) / (Right.Time - Left.Time);
			const auto Result = glm::normalize(glm::slerp(Left.Value, RightRotation, Alpha));
			if (Metrics)
				Metrics->InterpolationCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - InterpolationStarted
					)
						.count()
				);
			return Result;
		}

		RenderMeshIdentity PosedMeshIdentity(ObjectId Object) {
			return {
				.Slot = (static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot,
				.Generation = 2,
			};
		}

		template <typename T>
		std::shared_ptr<std::vector<T>> AcquireBuffer(
			std::vector<std::shared_ptr<std::vector<T>>> &Pool, std::size_t Size, AnimationRuntimeMetrics &Metrics
		) {
			for (auto &Buffer : Pool)
				if (Buffer.use_count() == 1) {
					Buffer->resize(Size);
					return Buffer;
				}
			auto Buffer = std::make_shared<std::vector<T>>(Size);
			Pool.push_back(Buffer);
			++Metrics.BufferAllocations;
			return Buffer;
		}

		enum class PoseEvaluationFailure : std::uint8_t {
			None,
			InvalidHierarchy,
			InvalidPalette,
			CpuSkinning,
		};

		struct PoseTrackInput {
			std::shared_ptr<const std::vector<ImportedAnimationTrack>> ClipTracks;
			std::shared_ptr<const std::vector<std::int32_t>> JointTrackIndices;
			std::uint64_t CreationSequence = 0;
			std::uint64_t Revision = 0;
			std::uint64_t ControlRevision = 0;
			float TimePosition = 0.0f;
			float Weight = 0.0f;
		};

		struct PoseEvaluationWork {
			ImportedMesh Mesh;
			std::array<PoseTrackInput, Animator::MaximumTracks> Tracks;
			std::size_t TrackCount = 0;
			std::vector<LocalPose> *LocalPoses = nullptr;
			std::shared_ptr<std::vector<glm::mat4>> ModelTransforms;
			std::shared_ptr<std::vector<RenderSkinPaletteEntry>> Palette;
			std::shared_ptr<std::vector<RenderVertex>> Vertices;
			RenderBounds Bounds;
			AnimationRuntimeMetrics Timing;
			PoseEvaluationFailure Failure = PoseEvaluationFailure::None;
			std::uint64_t WorkerCpuNanoseconds = 0;
			bool CpuSkinningFallback = false;
			bool DetailedProfiling = false;
		};

		void EvaluatePose(PoseEvaluationWork &Work) {
			const auto WorkerStarted = std::chrono::steady_clock::now();
			const auto &Joints = *Work.Mesh.Skeleton->Joints;
			auto *DetailedMetrics = Work.DetailedProfiling ? &Work.Timing : nullptr;
			const auto SamplingStarted = std::chrono::steady_clock::now();
			const auto LookupBefore = Work.Timing.KeyframeLookupCpuNanoseconds;
			const auto InterpolationBefore = Work.Timing.InterpolationCpuNanoseconds;
			const auto BindFallbackBefore = Work.Timing.BindPoseFallbackCpuNanoseconds;
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				const auto &Joint = Joints[JointIndex];
				auto &Pose = Work.LocalPoses->at(JointIndex);
				glm::vec3 TranslationSum(0.0f), ScaleSum(0.0f);
				float TranslationWeight = 0.0f, ScaleWeight = 0.0f;
				glm::vec4 RotationSum(0.0f);
				float RotationWeight = 0.0f;
				std::optional<glm::quat> RotationReference;
				std::array<std::pair<glm::quat, float>, Animator::MaximumTracks> RotationSamples;
				std::size_t RotationSampleCount = 0;
				for (std::size_t TrackIndex = 0; TrackIndex < Work.TrackCount; ++TrackIndex) {
					const auto &Track = Work.Tracks[TrackIndex];
					if (!Track.ClipTracks || !Track.JointTrackIndices || JointIndex >= Track.JointTrackIndices->size())
						continue;
					const auto ClipTrackIndex = Track.JointTrackIndices->at(JointIndex);
					if (ClipTrackIndex < 0 || static_cast<std::size_t>(ClipTrackIndex) >= Track.ClipTracks->size())
						continue;
					const auto &ClipTrack = Track.ClipTracks->at(static_cast<std::size_t>(ClipTrackIndex));
					if (auto Value = SampleVector(
							ClipTrack.TranslationKeys,
							ClipTrack.TranslationInterpolation,
							Track.TimePosition,
							DetailedMetrics
						)) {
						TranslationSum += *Value * Track.Weight;
						TranslationWeight += Track.Weight;
					}
					if (auto Value = SampleVector(
							ClipTrack.ScaleKeys, ClipTrack.ScaleInterpolation, Track.TimePosition, DetailedMetrics
						)) {
						ScaleSum += *Value * Track.Weight;
						ScaleWeight += Track.Weight;
					}
					if (auto Value = SampleRotation(
							ClipTrack.RotationKeys, ClipTrack.RotationInterpolation, Track.TimePosition, DetailedMetrics
						)) {
						RotationSamples[RotationSampleCount++] = {*Value, Track.Weight};
						RotationWeight += Track.Weight;
					}
				}
				auto BlendVector = [&](glm::vec3 Bind, glm::vec3 Sum, float Weight) {
					const auto BindFallbackStarted = DetailedMetrics ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					const auto BindWeight = std::max(0.0f, 1.0f - Weight);
					const auto Blended = Sum + Bind * BindWeight;
					if (DetailedMetrics)
						Work.Timing.BindPoseFallbackCpuNanoseconds += static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - BindFallbackStarted
							)
								.count()
						);
					return Blended / std::max(1.0f, Weight);
				};
				Pose.Translation = BlendVector(Joint.BindTranslation, TranslationSum, TranslationWeight);
				Pose.Scale = BlendVector(Joint.BindScale, ScaleSum, ScaleWeight);
				const auto BindRotationWeight = std::max(0.0f, 1.0f - RotationWeight);
				if (BindRotationWeight > 0.0f) {
					const auto BindFallbackStarted = DetailedMetrics ? std::chrono::steady_clock::now()
																	 : std::chrono::steady_clock::time_point{};
					RotationReference = Joint.BindRotation;
					RotationSum +=
						glm::vec4(
							Joint.BindRotation.x, Joint.BindRotation.y, Joint.BindRotation.z, Joint.BindRotation.w
						) *
						BindRotationWeight;
					if (DetailedMetrics)
						Work.Timing.BindPoseFallbackCpuNanoseconds += static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - BindFallbackStarted
							)
								.count()
						);
				}
				for (std::size_t SampleIndex = 0; SampleIndex < RotationSampleCount; ++SampleIndex) {
					auto [Rotation, Weight] = RotationSamples[SampleIndex];
					if (!RotationReference) RotationReference = Rotation;
					if (glm::dot(*RotationReference, Rotation) < 0.0f) Rotation = -Rotation;
					RotationSum += glm::vec4(Rotation.x, Rotation.y, Rotation.z, Rotation.w) * Weight;
				}
				Pose.Rotation = !RotationReference || glm::dot(RotationSum, RotationSum) < 1.0e-12f
									? Joint.BindRotation
									: glm::normalize(
										  glm::quat(RotationSum.w, RotationSum.x, RotationSum.y, RotationSum.z)
									  );
			}
			const auto SamplingNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - SamplingStarted)
					.count()
			);
			Work.Timing.SamplingAndBlendingCpuNanoseconds = SamplingNanoseconds;
			if (DetailedMetrics) {
				const auto Instrumented = Work.Timing.KeyframeLookupCpuNanoseconds - LookupBefore +
										  Work.Timing.InterpolationCpuNanoseconds - InterpolationBefore +
										  Work.Timing.BindPoseFallbackCpuNanoseconds - BindFallbackBefore;
				Work.Timing.TrackBlendingCpuNanoseconds = SamplingNanoseconds > Instrumented
															  ? SamplingNanoseconds - Instrumented
															  : 0;
			}

			const auto HierarchyStarted = std::chrono::steady_clock::now();
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				const auto &Joint = Joints[JointIndex];
				const auto &Pose = Work.LocalPoses->at(JointIndex);
				const auto Local = glm::translate(glm::mat4(1.0f), Pose.Translation) * glm::mat4_cast(Pose.Rotation) *
								   glm::scale(glm::mat4(1.0f), Pose.Scale);
				Work.ModelTransforms->at(
					JointIndex
				) = Joint.Parent < 0 ? Local : Work.ModelTransforms->at(static_cast<std::size_t>(Joint.Parent)) * Local;
				if (!IsFinite(Work.ModelTransforms->at(JointIndex))) {
					Work.Failure = PoseEvaluationFailure::InvalidHierarchy;
					break;
				}
			}
			Work.Timing.HierarchyCpuNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - HierarchyStarted
				)
					.count()
			);
			if (Work.Failure != PoseEvaluationFailure::None) {
				Work.WorkerCpuNanoseconds = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - WorkerStarted
					)
						.count()
				);
				return;
			}

			const auto SkinMatrixStarted = std::chrono::steady_clock::now();
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				auto &Entry = Work.Palette->at(JointIndex);
				Entry.PositionMatrix = Work.ModelTransforms->at(JointIndex) * Joints[JointIndex].InverseBindMatrix;
				const glm::mat3 Linear(Entry.PositionMatrix);
				if (!IsFinite(Entry.PositionMatrix) || std::abs(glm::determinant(Linear)) < 1.0e-12f) {
					Work.Failure = PoseEvaluationFailure::InvalidPalette;
					break;
				}
				Entry.NormalMatrix = glm::mat4(glm::transpose(glm::inverse(Linear)));
				if (!IsFinite(Entry.NormalMatrix)) {
					Work.Failure = PoseEvaluationFailure::InvalidPalette;
					break;
				}
			}
			Work.Timing.SkinMatrixCpuNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SkinMatrixStarted
				)
					.count()
			);
			if (Work.Failure == PoseEvaluationFailure::None && Work.CpuSkinningFallback) {
				const auto SkinningStarted = std::chrono::steady_clock::now();
				if (!AnimationRuntime::SkinMeshCpu(Work.Mesh, *Work.Palette, *Work.Vertices, Work.Bounds))
					Work.Failure = PoseEvaluationFailure::CpuSkinning;
				Work.Timing.SkinningCpuNanoseconds = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - SkinningStarted
					)
						.count()
				);
			}
			Work.WorkerCpuNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - WorkerStarted)
					.count()
			);
		}
	}

	struct AnimationRuntime::Impl {
		struct TrackedAnimator {
			std::weak_ptr<Animator> Value;
			ObjectId TargetObject;
			RenderMeshIdentity PosedMesh;
			std::uint64_t PoseRevision = 0;
			std::uint64_t SourceContentRevision = 0;
			RenderMeshIdentity SourceMesh;
			AssetContentId Compatibility;
			bool Published = false;
			std::vector<std::shared_ptr<AnimationTrack>> TrackSnapshot;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> ObservedTrackRevisions;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> CurrentTrackRevisions;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> ObservedControlRevisions;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> CurrentControlRevisions;
			std::vector<LocalPose> LocalPoses;
			std::vector<std::shared_ptr<std::vector<glm::mat4>>> ModelPools;
			std::vector<std::shared_ptr<std::vector<RenderSkinPaletteEntry>>> PalettePools;
			std::vector<std::shared_ptr<std::vector<RenderVertex>>> VertexPools;
			AnimationPoseSnapshot Snapshot;
			AnimationUpdatePolicyClass Policy = AnimationUpdatePolicyClass::FullRate;
			AnimationDistanceBand DistanceBand = AnimationDistanceBand::Near;
			double LastVisibleTime = 0.0;
			std::uint64_t LastCadenceInterval = 0;
			std::uint64_t LastCadenceTick = 0;
			bool PolicyInitialized = false;
			bool DistanceBandInitialized = false;
			bool HasVisibleSample = false;
		};

		struct PendingEvaluation {
			ObjectId AnimatorObject;
			ObjectId TargetObject;
			TrackedAnimator *Tracked = nullptr;
			RenderMeshIdentity SourceMesh;
			std::uint64_t SourceContentRevision = 0;
			AssetContentId Compatibility;
			PoseEvaluationWork Work;
			bool ExplicitRefresh = false;
		};

		std::shared_ptr<AssetService> Assets;
		DiagnosticCallback Diagnostic;
		std::map<ObjectId, TrackedAnimator> Animators;
		std::map<ObjectId, AnimationPoseSnapshot> Poses;
		std::set<std::pair<ObjectId, std::string>> EmittedDiagnostics;
		std::vector<RenderAnimationPoseState> PoseUpdates;
		std::vector<RenderAnimationPoseRemove> PoseRemoves;
		std::vector<ObjectId> ClaimedTargets;
		std::vector<ObjectId> RefreshRequests;
		std::vector<PendingEvaluation> PendingEvaluations;
		std::unique_ptr<JobSystem> PoseJobs;
		std::shared_ptr<JobGroup> PoseJobGroup = std::make_shared<JobGroup>();
		std::function<void()> BeforePoseMergeForTesting;
		AnimationRuntimeMetrics Metrics;
		AnimationRuntimeOptions Options;
		std::uint64_t NextPoseRevision = 0;
		std::uint64_t RuntimeNanoseconds = 0;
		double RuntimeSeconds = 0.0;
		std::uint64_t LastVisibilityGeneration = 0;
		std::uint64_t LastVisibilityPublication = 0;
		bool ShutDown = false;

		Impl(
			std::shared_ptr<AssetService> AssetsValue,
			DiagnosticCallback DiagnosticValue,
			AnimationRuntimeOptions OptionsValue
		)
			: Assets(std::move(AssetsValue)), Diagnostic(std::move(DiagnosticValue)), Options(OptionsValue) {
			PoseUpdates.reserve(64);
			PoseRemoves.reserve(64);
			ClaimedTargets.reserve(AnimationRuntime::MaximumAnimators);
			RefreshRequests.reserve(AnimationRuntime::MaximumAnimators);
			PendingEvaluations.reserve(64);
		}

		void Emit(ObjectId Object, std::string Code, std::string Message) {
			if (EmittedDiagnostics.size() >= AnimationRuntime::MaximumAnimators ||
				!EmittedDiagnostics.emplace(Object, Code).second || !Diagnostic)
				return;
			Diagnostic(std::move(Code), std::move(Message));
		}

		void RemovePose(TrackedAnimator &Tracked) {
			if (Tracked.Published && Tracked.TargetObject.IsValid()) {
				PoseRemoves.push_back({Tracked.TargetObject});
				Poses.erase(Tracked.TargetObject);
			}
			Tracked.Published = false;
			Tracked.Snapshot = {};
		}

		void FireEnded(const std::vector<std::shared_ptr<AnimationTrack>> &Tracks) {
			for (const auto &Track : Tracks)
				if (Track) Track->FirePendingEndedRuntime();
		}
	};

	AnimationRuntime::AnimationRuntime(
		std::shared_ptr<AssetService> Assets, DiagnosticCallback Diagnostic, AnimationRuntimeOptions Options
	)
		: State(std::make_unique<Impl>(std::move(Assets), std::move(Diagnostic), Options)) {}

	AnimationRuntime::~AnimationRuntime() {
		Shutdown();
	}

	void AnimationRuntime::RegisterAnimator(const std::shared_ptr<Animator> &AnimatorValue) {
		if (!State || State->ShutDown || !AnimatorValue || AnimatorValue->GetDestroyed() ||
			AnimatorValue->IsDestroying())
			return;
		const auto Object = AnimatorValue->GetObjectId();
		if (!Object.IsValid()) return;
		if (!State->Animators.contains(Object) && State->Animators.size() >= MaximumAnimators) {
			State->Emit(Object, "AnimatorLimit", "Animation Animator capacity is exhausted");
			return;
		}
		auto &Tracked = State->Animators[Object];
		Tracked.Value = AnimatorValue;
	}

	void AnimationRuntime::RequestPoseRefresh(ObjectId Object) {
		if (!State || State->ShutDown || !Object.IsValid()) return;
		auto Position = std::ranges::lower_bound(State->RefreshRequests, Object);
		if (Position != State->RefreshRequests.end() && *Position == Object) return;
		if (State->RefreshRequests.size() >= MaximumAnimators) return;
		State->RefreshRequests.insert(Position, Object);
	}

	void AnimationRuntime::Step(float DeltaTime) {
		Step(DeltaTime, {});
	}

	void AnimationRuntime::Step(float DeltaTime, const AnimationUpdateContext &Context) {
		if (!State || State->ShutDown) return;
		const auto Started = std::chrono::steady_clock::now();
		State->PoseUpdates.clear();
		State->PoseRemoves.clear();
		if (!std::isfinite(DeltaTime) || DeltaTime < 0.0f) DeltaTime = 0.0f;
		const auto DeltaNanoseconds = static_cast<std::uint64_t>(
			std::llround(std::min(static_cast<double>(DeltaTime), 60.0) * 1'000'000'000.0)
		);
		if (State->RuntimeNanoseconds > std::numeric_limits<std::uint64_t>::max() - DeltaNanoseconds)
			State->RuntimeNanoseconds = 0;
		else
			State->RuntimeNanoseconds += DeltaNanoseconds;
		State->RuntimeSeconds += static_cast<double>(DeltaTime);
		if (!std::isfinite(State->RuntimeSeconds)) State->RuntimeSeconds = 0.0;
		State->Metrics.ActiveRigs = 0;
		State->Metrics.ActiveTracks = 0;
		State->Metrics.FullRateAnimators = 0;
		State->Metrics.ReducedRateAnimators = 0;
		State->Metrics.FrozenVisualAnimators = 0;
		State->Metrics.SemanticRequiredAnimators = 0;
		State->Metrics.ActivePoseJobs = 0;
		State->ClaimedTargets.clear();
		State->PendingEvaluations.clear();

		const bool SemanticSummaryValid = Context.SemanticRequirementsComplete &&
										  IsValidIdentitySummary(Context.SemanticRequiredObjects, MaximumAnimators);
		bool VisibilityUsable = false;
		if (Context.Environment == AnimationRuntimeEnvironment::Graphical && Context.VisibilityComplete &&
			Context.VisibilityGeneration != 0 && Context.VisibilityPublication != 0 &&
			IsValidIdentitySummary(Context.VisibleObjects, MaximumAnimators)) {
			const bool StaleGeneration = Context.VisibilityGeneration < State->LastVisibilityGeneration;
			const bool StalePublication = Context.VisibilityGeneration == State->LastVisibilityGeneration &&
										  Context.VisibilityPublication < State->LastVisibilityPublication;
			if (StaleGeneration || StalePublication)
				++State->Metrics.VisibilityFeedbackDrops;
			else {
				State->LastVisibilityGeneration = Context.VisibilityGeneration;
				State->LastVisibilityPublication = Context.VisibilityPublication;
				VisibilityUsable = true;
			}
		} else if (Context.Environment == AnimationRuntimeEnvironment::Graphical &&
				   (Context.VisibilityComplete || Context.VisibilityGeneration != 0 ||
					Context.VisibilityPublication != 0))
			++State->Metrics.VisibilityFeedbackDrops;

		for (auto Iterator = State->Animators.begin(); Iterator != State->Animators.end();) {
			const auto Object = Iterator->first;
			auto &Tracked = Iterator->second;
			auto AnimatorValue = Tracked.Value.lock();
			if (!AnimatorValue || AnimatorValue->GetDestroyed() || AnimatorValue->IsDestroying()) {
				State->RemovePose(Tracked);
				if (Tracked.TargetObject.IsValid()) std::erase(State->RefreshRequests, Tracked.TargetObject);
				if (AnimatorValue) AnimatorValue->InvalidateTracks();
				Iterator = State->Animators.erase(Iterator);
				continue;
			}
			if (!AnimatorValue->GetDataModel()) {
				State->RemovePose(Tracked);
				if (Tracked.TargetObject.IsValid()) std::erase(State->RefreshRequests, Tracked.TargetObject);
				AnimatorValue->InvalidateTracks();
				Iterator = State->Animators.erase(Iterator);
				continue;
			}

			Tracked.TrackSnapshot = AnimatorValue->Tracks;
			std::ranges::sort(Tracked.TrackSnapshot, {}, &AnimationTrack::CreationSequence);
			const auto TrackAdvanceStarted = std::chrono::steady_clock::now();
			for (const auto &Track : Tracked.TrackSnapshot)
				if (Track) (void)Track->AdvanceRuntime(DeltaTime);
			State->Metrics.TrackAdvanceCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - TrackAdvanceStarted
				)
					.count()
			);

			Tracked.CurrentTrackRevisions.clear();
			Tracked.CurrentControlRevisions.clear();
			std::size_t VisibleTrackCount = 0;
			for (const auto &Track : Tracked.TrackSnapshot) {
				if (!Track || Track->Invalidated) continue;
				Tracked.CurrentTrackRevisions.emplace_back(Track->CreationSequence, Track->Revision);
				Tracked.CurrentControlRevisions.emplace_back(Track->CreationSequence, Track->ControlRevision);
				if ((Track->PlaybackState == Enums::AnimationPlaybackState::Playing ||
					 Track->PlaybackState == Enums::AnimationPlaybackState::Paused || Track->NaturalEndPose) &&
					Track->Weight > 0.0f)
					++VisibleTrackCount;
			}
			State->Metrics.ActiveTracks += VisibleTrackCount;
			if (VisibleTrackCount == 0) {
				State->RemovePose(Tracked);
				if (Tracked.TargetObject.IsValid()) std::erase(State->RefreshRequests, Tracked.TargetObject);
				Tracked.ObservedTrackRevisions = Tracked.CurrentTrackRevisions;
				Tracked.ObservedControlRevisions = Tracked.CurrentControlRevisions;
				++Iterator;
				continue;
			}

			const auto Parent = AnimatorValue->GetParent();
			auto MeshPartValue = Parent ? std::dynamic_pointer_cast<MeshPart>(*Parent) : nullptr;
			auto Mesh = State->Assets && MeshPartValue && !MeshPartValue->GetDestroyed() &&
								!MeshPartValue->IsDestroying()
							? State->Assets->ResolveMeshResource(MeshPartValue->GetMeshReferenceRuntime())
							: std::nullopt;
			if (!Mesh || !Mesh->Value.Skeleton || !Mesh->Value.Skeleton->Joints || !Mesh->Value.SkinInfluences ||
				!Mesh->Value.Vertices || !Mesh->Value.Indices || Mesh->Value.Skeleton->Joints->empty() ||
				Mesh->Value.Skeleton->Joints->size() > MaximumBonesPerRig) {
				State->RemovePose(Tracked);
				State->Emit(
					Object, "RigUnavailable", "Animator parent does not resolve to a bounded skinned Mesh asset"
				);
				++Iterator;
				continue;
			}
			const auto TargetObject = MeshPartValue->GetObjectId();
			if (!TargetObject.IsValid()) {
				State->RemovePose(Tracked);
				State->Emit(Object, "RigUnavailable", "Animator parent does not have a live render identity");
				++Iterator;
				continue;
			}
			const auto ClaimedPosition = std::lower_bound(
				State->ClaimedTargets.begin(), State->ClaimedTargets.end(), TargetObject
			);
			if (ClaimedPosition != State->ClaimedTargets.end() && *ClaimedPosition == TargetObject) {
				if (Tracked.TargetObject != TargetObject)
					State->RemovePose(Tracked);
				else {
					Tracked.Published = false;
					Tracked.Snapshot = {};
				}
				State->Emit(Object, "DuplicateAnimator", "Only one active Animator may target a MeshPart");
				++Iterator;
				continue;
			}
			State->ClaimedTargets.insert(ClaimedPosition, TargetObject);
			const bool TargetChanged = Tracked.TargetObject != TargetObject;
			if (TargetChanged) {
				State->RemovePose(Tracked);
				Tracked.TargetObject = TargetObject;
				Tracked.PosedMesh = PosedMeshIdentity(TargetObject);
			}
			const auto Compatibility = Mesh->Value.Skeleton->CompatibilityId;
			const bool Compatible = std::ranges::all_of(Tracked.TrackSnapshot, [&](const auto &Track) {
				return !Track || Track->Invalidated ||
					   (Track->PlaybackState == Enums::AnimationPlaybackState::Stopped && !Track->NaturalEndPose) ||
					   Track->Resource.Value.SkeletonCompatibilityId == Compatibility;
			});
			if (!Compatible) {
				State->RemovePose(Tracked);
				State->Emit(
					Object,
					"IncompatibleSkeleton",
					"Active Animation track is incompatible with the current Mesh skeleton"
				);
				++Iterator;
				continue;
			}

			const auto PolicyStarted = std::chrono::steady_clock::now();

			const bool TrackStateChanged = Tracked.CurrentTrackRevisions != Tracked.ObservedTrackRevisions;
			const bool ControlStateChanged = Tracked.CurrentControlRevisions != Tracked.ObservedControlRevisions;
			const bool RigChanged = Tracked.SourceContentRevision != Mesh->ContentRevision ||
									Tracked.SourceMesh != Mesh->Mesh || Tracked.Compatibility != Compatibility ||
									TargetChanged;
			const bool SemanticRequired = !SemanticSummaryValid ||
										  ContainsIdentity(Context.SemanticRequiredObjects, TargetObject);
			AnimationUpdatePolicyClass NextPolicy = AnimationUpdatePolicyClass::FullRate;
			std::uint64_t CadenceNanoseconds = 0;
			if (SemanticRequired)
				NextPolicy = AnimationUpdatePolicyClass::SemanticRequired;
			else if (Context.Environment == AnimationRuntimeEnvironment::Headless)
				NextPolicy = AnimationUpdatePolicyClass::VisualFrozen;
			else if (Context.Environment == AnimationRuntimeEnvironment::Graphical) {
				if (!VisibilityUsable || !Context.HasViewOrigin || !IsFinite(Context.ViewOrigin))
					NextPolicy = AnimationUpdatePolicyClass::FullRate;
				else {
					const auto Distance = glm::distance(MeshPartValue->GetCFrame().Position, Context.ViewOrigin);
					if (!std::isfinite(Distance))
						NextPolicy = AnimationUpdatePolicyClass::FullRate;
					else {
						Tracked.DistanceBand = Tracked.DistanceBandInitialized
												   ? ApplyDistanceHysteresis(Tracked.DistanceBand, Distance)
												   : ClassifyDistance(Distance);
						Tracked.DistanceBandInitialized = true;
						const bool Visible = ContainsIdentity(Context.VisibleObjects, TargetObject);
						if (Visible) {
							Tracked.HasVisibleSample = true;
							Tracked.LastVisibleTime = State->RuntimeSeconds;
						}
						const bool RecentlyVisible = Visible ||
													 (Tracked.HasVisibleSample &&
													  State->RuntimeSeconds - Tracked.LastVisibleTime <=
														  AnimationUpdatePolicySettings::RecentlyVisibleGraceSeconds);
						if (!RecentlyVisible)
							NextPolicy = AnimationUpdatePolicyClass::VisualFrozen;
						else if (Tracked.DistanceBand == AnimationDistanceBand::Near)
							NextPolicy = AnimationUpdatePolicyClass::FullRate;
						else {
							NextPolicy = AnimationUpdatePolicyClass::ReducedRate;
							CadenceNanoseconds = CadenceForBand(Tracked.DistanceBand);
						}
					}
				}
			}

			switch (NextPolicy) {
			case AnimationUpdatePolicyClass::FullRate:
				++State->Metrics.FullRateAnimators;
				break;
			case AnimationUpdatePolicyClass::ReducedRate:
				++State->Metrics.ReducedRateAnimators;
				break;
			case AnimationUpdatePolicyClass::VisualFrozen:
				++State->Metrics.FrozenVisualAnimators;
				break;
			case AnimationUpdatePolicyClass::SemanticRequired:
				++State->Metrics.SemanticRequiredAnimators;
				break;
			}
			const bool PolicyEscalated = Tracked.PolicyInitialized &&
										 PolicyDemand(NextPolicy) > PolicyDemand(Tracked.Policy);
			if (Tracked.PolicyInitialized && Tracked.Policy != NextPolicy) ++State->Metrics.PolicyTransitions;
			Tracked.Policy = NextPolicy;
			Tracked.PolicyInitialized = true;

			bool CadenceDue = false;
			if (NextPolicy == AnimationUpdatePolicyClass::ReducedRate && CadenceNanoseconds != 0) {
				const auto Phase = StableScheduleHash(TargetObject) % CadenceNanoseconds;
				const auto BaseTick = State->RuntimeNanoseconds / CadenceNanoseconds;
				const auto Remainder = State->RuntimeNanoseconds % CadenceNanoseconds;
				const auto CadenceTick = BaseTick + (Remainder >= CadenceNanoseconds - Phase ? 1 : 0);
				if (Tracked.LastCadenceInterval != CadenceNanoseconds) {
					Tracked.LastCadenceInterval = CadenceNanoseconds;
					Tracked.LastCadenceTick = CadenceTick;
				} else if (Tracked.LastCadenceTick != CadenceTick) {
					Tracked.LastCadenceTick = CadenceTick;
					CadenceDue = true;
				}
			} else
				Tracked.LastCadenceInterval = 0;

			const auto RefreshPosition = std::ranges::lower_bound(State->RefreshRequests, TargetObject);
			const bool ExplicitRefresh = RefreshPosition != State->RefreshRequests.end() &&
										 *RefreshPosition == TargetObject;
			const bool HeadlessVisualOnly = Context.Environment == AnimationRuntimeEnvironment::Headless &&
											!SemanticRequired;
			const bool ImmediateRefresh = ExplicitRefresh ||
										  (!HeadlessVisualOnly && (RigChanged || ControlStateChanged ||
																   PolicyEscalated || !Tracked.Published));
			bool ShouldEvaluate = ImmediateRefresh;
			if (!ShouldEvaluate) {
				switch (NextPolicy) {
				case AnimationUpdatePolicyClass::FullRate:
				case AnimationUpdatePolicyClass::SemanticRequired:
					ShouldEvaluate = TrackStateChanged;
					break;
				case AnimationUpdatePolicyClass::ReducedRate:
					ShouldEvaluate = TrackStateChanged && CadenceDue;
					break;
				case AnimationUpdatePolicyClass::VisualFrozen:
					break;
				}
			}
			if (!ShouldEvaluate) {
				State->Metrics.PolicyCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - PolicyStarted
					)
						.count()
				);
				if (TrackStateChanged || ControlStateChanged || RigChanged || ExplicitRefresh || !Tracked.Published) {
					++State->Metrics.SkippedPoseEvaluations;
					if (HeadlessVisualOnly) ++State->Metrics.HeadlessVisualPoseSkips;
				}
				if (Tracked.Published) ++State->Metrics.ActiveRigs;
				++Iterator;
				continue;
			}
			State->Metrics.PolicyCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - PolicyStarted)
					.count()
			);
			if (ImmediateRefresh) ++State->Metrics.ImmediatePoseRefreshes;
			if (State->PendingEvaluations.size() >= MaximumAnimators)
				throw std::length_error("[Animation:Runtime] bounded pose evaluation capacity is exhausted");

			const auto &Joints = *Mesh->Value.Skeleton->Joints;
			Tracked.LocalPoses.resize(Joints.size());
			auto &Pending = State->PendingEvaluations.emplace_back();
			Pending.AnimatorObject = Object;
			Pending.TargetObject = TargetObject;
			Pending.Tracked = &Tracked;
			Pending.SourceMesh = Mesh->Mesh;
			Pending.SourceContentRevision = Mesh->ContentRevision;
			Pending.Compatibility = Compatibility;
			Pending.ExplicitRefresh = ExplicitRefresh;
			Pending.Work.Mesh = Mesh->Value;
			Pending.Work.LocalPoses = &Tracked.LocalPoses;
			Pending.Work.ModelTransforms = AcquireBuffer(Tracked.ModelPools, Joints.size(), State->Metrics);
			Pending.Work.Palette = AcquireBuffer(Tracked.PalettePools, Joints.size(), State->Metrics);
			Pending.Work.Bounds = Mesh->Value.Bounds;
			Pending.Work.CpuSkinningFallback = State->Options.CpuSkinningFallback;
			Pending.Work.DetailedProfiling = State->Options.DetailedProfiling;
			if (State->Options.CpuSkinningFallback)
				Pending.Work.Vertices = AcquireBuffer(
					Tracked.VertexPools, Mesh->Value.Vertices->size(), State->Metrics
				);

			for (const auto &Track : Tracked.TrackSnapshot) {
				if (!Track || Track->Invalidated || Track->Weight <= 0.0f ||
					(Track->PlaybackState == Enums::AnimationPlaybackState::Stopped && !Track->NaturalEndPose))
					continue;
				if (Pending.Work.TrackCount >= Animator::MaximumTracks)
					throw std::length_error("[Animation:Runtime] bounded track snapshot capacity is exhausted");
				auto &Input = Pending.Work.Tracks[Pending.Work.TrackCount++];
				Input.ClipTracks = Track->Resource.Value.Tracks;
				Input.JointTrackIndices = Track->JointTrackIndices;
				Input.CreationSequence = Track->CreationSequence;
				Input.Revision = Track->Revision;
				Input.ControlRevision = Track->ControlRevision;
				Input.TimePosition = Track->TimePosition;
				Input.Weight = Track->Weight;
			}
			++Iterator;
			continue;
		}

		constexpr std::size_t MinimumParallelPoseEvaluations = 32;
		if (State->Options.ParallelPoseEvaluation && State->Options.PoseWorkerCount != 1 &&
			State->PendingEvaluations.size() >= MinimumParallelPoseEvaluations) {
			if (!State->PoseJobs) {
				State->PoseJobs = std::make_unique<JobSystem>(State->Options.PoseWorkerCount);
				State->Metrics.PoseWorkerCapacity = State->PoseJobs->GetWorkerCount();
			}
			const auto BatchCount = std::min({
				State->PendingEvaluations.size(),
				State->PoseJobs->GetWorkerCount(),
				static_cast<std::size_t>(64),
			});
			State->Metrics.ActivePoseJobs = BatchCount;
			State->Metrics.PoseJobsScheduled += State->PendingEvaluations.size();
			State->Metrics.PoseJobBatches += BatchCount;
			const auto SchedulingStarted = std::chrono::steady_clock::now();
			for (std::size_t Batch = 0; Batch < BatchCount; ++Batch) {
				const auto Begin = State->PendingEvaluations.size() * Batch / BatchCount;
				const auto End = State->PendingEvaluations.size() * (Batch + 1) / BatchCount;
				State->PoseJobs->Submit(
					[State = State.get(), Begin, End] {
						for (auto Index = Begin; Index < End; ++Index)
							EvaluatePose(State->PendingEvaluations[Index].Work);
					},
					State->PoseJobGroup
				);
			}
			State->Metrics.PoseJobSchedulingCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SchedulingStarted
				)
					.count()
			);

			const auto WaitStarted = std::chrono::steady_clock::now();
			State->PoseJobGroup->Wait();
			State->Metrics.PoseJobWaitNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - WaitStarted)
					.count()
			);
			State->Metrics.ActivePoseJobs = 0;
			if (auto Exception = State->PoseJobGroup->GetFirstException()) std::rethrow_exception(Exception);
		} else {
			for (auto &Pending : State->PendingEvaluations)
				EvaluatePose(Pending.Work);
		}
		if (State->BeforePoseMergeForTesting) {
			auto Callback = std::move(State->BeforePoseMergeForTesting);
			State->BeforePoseMergeForTesting = {};
			Callback();
		}

		const auto MergeStarted = std::chrono::steady_clock::now();
		for (auto &Pending : State->PendingEvaluations) {
			auto &Work = Pending.Work;
			State->Metrics.KeyframeLookupCpuNanoseconds += Work.Timing.KeyframeLookupCpuNanoseconds;
			State->Metrics.InterpolationCpuNanoseconds += Work.Timing.InterpolationCpuNanoseconds;
			State->Metrics.TrackBlendingCpuNanoseconds += Work.Timing.TrackBlendingCpuNanoseconds;
			State->Metrics.BindPoseFallbackCpuNanoseconds += Work.Timing.BindPoseFallbackCpuNanoseconds;
			State->Metrics.SamplingAndBlendingCpuNanoseconds += Work.Timing.SamplingAndBlendingCpuNanoseconds;
			State->Metrics.HierarchyCpuNanoseconds += Work.Timing.HierarchyCpuNanoseconds;
			State->Metrics.SkinMatrixCpuNanoseconds += Work.Timing.SkinMatrixCpuNanoseconds;
			State->Metrics.SkinningCpuNanoseconds += Work.Timing.SkinningCpuNanoseconds;
			State->Metrics.PoseWorkerCpuNanoseconds += Work.WorkerCpuNanoseconds;
			const auto Found = State->Animators.find(Pending.AnimatorObject);
			auto AnimatorValue = Pending.Tracked ? Pending.Tracked->Value.lock() : nullptr;
			bool InputCurrent = Found != State->Animators.end() && &Found->second == Pending.Tracked && AnimatorValue &&
								!AnimatorValue->GetDestroyed() && !AnimatorValue->IsDestroying() &&
								AnimatorValue->GetObjectId() == Pending.AnimatorObject &&
								Pending.Tracked->TargetObject == Pending.TargetObject;
			std::size_t CurrentTrackCount = 0;
			if (InputCurrent)
				for (const auto &Track : Pending.Tracked->TrackSnapshot) {
					if (!Track || Track->Invalidated || Track->Weight <= 0.0f ||
						(Track->PlaybackState == Enums::AnimationPlaybackState::Stopped && !Track->NaturalEndPose))
						continue;
					if (CurrentTrackCount >= Work.TrackCount) {
						InputCurrent = false;
						break;
					}
					const auto &Input = Work.Tracks[CurrentTrackCount++];
					if (Input.CreationSequence != Track->CreationSequence || Input.Revision != Track->Revision ||
						Input.ControlRevision != Track->ControlRevision) {
						InputCurrent = false;
						break;
					}
				}
			InputCurrent = InputCurrent && CurrentTrackCount == Work.TrackCount;
			if (InputCurrent) {
				const auto Parent = AnimatorValue->GetParent();
				auto MeshPartValue = Parent ? std::dynamic_pointer_cast<MeshPart>(*Parent) : nullptr;
				auto CurrentMesh = State->Assets && MeshPartValue
									   ? State->Assets->ResolveMeshResource(MeshPartValue->GetMeshReferenceRuntime())
									   : std::nullopt;
				InputCurrent = CurrentMesh && CurrentMesh->Mesh == Pending.SourceMesh &&
							   CurrentMesh->ContentRevision == Pending.SourceContentRevision &&
							   CurrentMesh->Value.Skeleton &&
							   CurrentMesh->Value.Skeleton->CompatibilityId == Pending.Compatibility;
			}
			if (!InputCurrent) {
				++State->Metrics.StalePoseJobDrops;
				if (Pending.Tracked && Pending.Tracked->Published) ++State->Metrics.ActiveRigs;
				continue;
			}

			auto &Tracked = *Pending.Tracked;
			if (Work.Failure != PoseEvaluationFailure::None) {
				State->RemovePose(Tracked);
				if (Work.Failure == PoseEvaluationFailure::CpuSkinning)
					State->Emit(
						Pending.AnimatorObject, "SkinningFailure", "CPU skinning rejected a singular or non-finite pose"
					);
				else
					State->Emit(
						Pending.AnimatorObject,
						"InvalidPose",
						Work.Failure == PoseEvaluationFailure::InvalidHierarchy
							? "Animation pose produced a non-finite joint hierarchy"
							: "Animation pose produced a singular or non-finite skin palette"
					);
				continue;
			}
			if (State->NextPoseRevision == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("[Animation:Runtime] pose revision is exhausted");
			Tracked.PoseRevision = ++State->NextPoseRevision;
			Tracked.SourceContentRevision = Pending.SourceContentRevision;
			Tracked.SourceMesh = Pending.SourceMesh;
			Tracked.Compatibility = Pending.Compatibility;
			Tracked.ObservedTrackRevisions = Tracked.CurrentTrackRevisions;
			Tracked.ObservedControlRevisions = Tracked.CurrentControlRevisions;
			Tracked.Published = true;
			const auto SemanticJointCacheStarted = std::chrono::steady_clock::now();
			Tracked.Snapshot = {Pending.Compatibility, Tracked.PoseRevision, Work.ModelTransforms, Work.Palette};
			State->Poses.insert_or_assign(Pending.TargetObject, Tracked.Snapshot);
			State->Metrics.SemanticJointCacheCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SemanticJointCacheStarted
				)
					.count()
			);
			const auto PublicationStarted = std::chrono::steady_clock::now();
			State->PoseUpdates.push_back({
				.Pose =
					{
						.Object = Pending.TargetObject,
						.SourceMesh = Pending.SourceMesh,
						.PosedMesh = State->Options.CpuSkinningFallback ? Tracked.PosedMesh : RenderMeshIdentity{},
						.PoseRevision = Tracked.PoseRevision,
						.Palette = {RenderSkeletonIdentity{Pending.Compatibility.Bytes}, Work.Palette},
						.Mode = State->Options.CpuSkinningFallback ? RenderAnimationSkinningMode::CpuFallback
																   : RenderAnimationSkinningMode::GpuPalette,
					},
				.TopologyRevision = Pending.SourceContentRevision,
				.Vertices = Work.Vertices,
				.Indices = State->Options.CpuSkinningFallback ? Work.Mesh.Indices : nullptr,
				.Bounds = Work.Bounds,
			});
			++State->Metrics.ActiveRigs;
			State->Metrics.EvaluatedBones += Work.ModelTransforms->size();
			if (Work.Vertices) State->Metrics.SkinnedVertices += Work.Vertices->size();
			++State->Metrics.PoseUpdates;
			++State->Metrics.PoseEvaluations;
			State->Metrics.PosePublicationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - PublicationStarted
				)
					.count()
			);
			if (Pending.ExplicitRefresh) {
				const auto Refresh = std::ranges::lower_bound(State->RefreshRequests, Pending.TargetObject);
				if (Refresh != State->RefreshRequests.end() && *Refresh == Pending.TargetObject)
					State->RefreshRequests.erase(Refresh);
			}
		}
		State->Metrics.PoseMergeCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - MergeStarted)
				.count()
		);
		for (auto &[AnimatorObject, Tracked] : State->Animators) {
			(void)AnimatorObject;
			State->FireEnded(Tracked.TrackSnapshot);
		}

		std::erase_if(State->PoseRemoves, [&](const auto &Remove) {
			return std::ranges::any_of(State->PoseUpdates, [&](const auto &Update) {
				return Update.Pose.Object == Remove.Object;
			});
		});
		State->Metrics.TrackedAnimators = State->Animators.size();
		State->Metrics.EvaluationCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
		);
	}

	void AnimationRuntime::Shutdown() {
		if (!State || State->ShutDown) return;
		State->ShutDown = true;
		if (State->PoseJobs) {
			State->PoseJobs->Shutdown(true);
			State->PoseJobs.reset();
		}
		for (auto &[Object, Tracked] : State->Animators) {
			State->RemovePose(Tracked);
			if (auto AnimatorValue = Tracked.Value.lock()) AnimatorValue->InvalidateTracks();
		}
		State->Animators.clear();
		State->Poses.clear();
		State->RefreshRequests.clear();
		State->PendingEvaluations.clear();
		State->Metrics.TrackedAnimators = 0;
		State->Metrics.ActiveRigs = 0;
		State->Metrics.ActiveTracks = 0;
		State->Metrics.ActivePoseJobs = 0;
		State->Metrics.PoseWorkerCapacity = 0;
	}

	void AnimationRuntime::SetBeforePoseMergeForTesting(std::function<void()> Callback) {
		if (!State || State->ShutDown)
			throw std::runtime_error("[Animation:Runtime] cannot install a test merge hook after shutdown");
		State->BeforePoseMergeForTesting = std::move(Callback);
	}

	const std::vector<RenderAnimationPoseState> &AnimationRuntime::GetPoseUpdates() const {
		static const std::vector<RenderAnimationPoseState> Empty;
		return State ? State->PoseUpdates : Empty;
	}

	const std::vector<RenderAnimationPoseRemove> &AnimationRuntime::GetPoseRemoves() const {
		static const std::vector<RenderAnimationPoseRemove> Empty;
		return State ? State->PoseRemoves : Empty;
	}

	void AnimationRuntime::ClearChanges() {
		if (!State) return;
		State->PoseUpdates.clear();
		State->PoseRemoves.clear();
	}

	std::optional<AnimationPoseSnapshot> AnimationRuntime::GetPose(ObjectId Object) const {
		if (!State) return std::nullopt;
		auto Found = State->Poses.find(Object);
		return Found == State->Poses.end() ? std::nullopt : std::optional(Found->second);
	}

	AnimationRuntimeMetrics AnimationRuntime::GetMetrics() const {
		return State ? State->Metrics : AnimationRuntimeMetrics{};
	}

	bool AnimationRuntime::SkinMeshCpu(
		const ImportedMesh &Mesh,
		std::span<const RenderSkinPaletteEntry> SkinPalette,
		std::vector<RenderVertex> &Output,
		RenderBounds &Bounds
	) {
		if (!Mesh.Vertices || !Mesh.SkinInfluences || Mesh.Vertices->empty() ||
			Mesh.SkinInfluences->size() != Mesh.Vertices->size() || SkinPalette.empty() ||
			SkinPalette.size() > MaximumBonesPerRig) return false;
		Output.resize(Mesh.Vertices->size());
		for (std::size_t VertexIndex = 0; VertexIndex < Mesh.Vertices->size(); ++VertexIndex) {
			const auto &Source = (*Mesh.Vertices)[VertexIndex];
			const auto &Influence = (*Mesh.SkinInfluences)[VertexIndex];
			glm::vec4 Position(0.0f);
			glm::vec3 Normal(0.0f), Tangent(0.0f);
			float WeightSum = 0.0f;
			for (std::size_t InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex) {
				const auto Weight = Influence.Weights[InfluenceIndex];
				const auto Joint = Influence.Joints[InfluenceIndex];
				if (!std::isfinite(Weight) || Weight < 0.0f || Joint >= SkinPalette.size()) return false;
				if (Weight == 0.0f) continue;
				const auto &Entry = SkinPalette[Joint];
				if (!IsFinite(Entry.PositionMatrix) || !IsFinite(Entry.NormalMatrix)) return false;
				Position += (Entry.PositionMatrix * glm::vec4(Source.Position, 1.0f)) * Weight;
				Normal += glm::vec3(Entry.NormalMatrix * glm::vec4(Source.Normal, 0.0f)) * Weight;
				Tangent += glm::vec3(Entry.NormalMatrix * glm::vec4(glm::vec3(Source.Tangent), 0.0f)) * Weight;
				WeightSum += Weight;
			}
			if (std::abs(WeightSum - 1.0f) > 1.0e-4f || std::abs(Position.w) < 1.0e-8f ||
				glm::dot(Normal, Normal) < 1.0e-12f || glm::dot(Tangent, Tangent) < 1.0e-12f) return false;
			auto &Destination = Output[VertexIndex];
			Destination = Source;
			Destination.Position = glm::vec3(Position) / Position.w;
			Destination.Normal = glm::normalize(Normal);
			Destination.Tangent = {glm::normalize(Tangent), Source.Tangent.w};
			if (!IsFinite(Destination.Position) || !IsFinite(Destination.Normal) || !IsFinite(Destination.Tangent))
				return false;
			if (VertexIndex == 0) Bounds = {Destination.Position, Destination.Position};
			else {
				Bounds.Minimum = glm::min(Bounds.Minimum, Destination.Position);
				Bounds.Maximum = glm::max(Bounds.Maximum, Destination.Position);
			}
		}
		return true;
	}
}
