#include "gargantuan/animation/AnimationRuntime.hpp"

#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/MeshPart.hpp"
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

		std::optional<glm::vec3> SampleVector(
			const std::shared_ptr<const std::vector<ImportedAnimationVectorKey>> &Keys,
			AssetAnimationInterpolation Interpolation,
			float Time
		) {
			if (!Keys || Keys->empty()) return std::nullopt;
			if (Time <= Keys->front().Time) return Keys->front().Value;
			if (Time >= Keys->back().Time) return Keys->back().Value;
			const auto Upper = std::upper_bound(Keys->begin(), Keys->end(), Time,
				[](float Value, const ImportedAnimationVectorKey &Key) { return Value < Key.Time; });
			const auto &Right = *Upper;
			const auto &Left = *(Upper - 1);
			if (Interpolation == AssetAnimationInterpolation::Step) return Left.Value;
			const auto Alpha = (Time - Left.Time) / (Right.Time - Left.Time);
			return glm::mix(Left.Value, Right.Value, Alpha);
		}

		std::optional<glm::quat> SampleRotation(
			const std::shared_ptr<const std::vector<ImportedAnimationRotationKey>> &Keys,
			AssetAnimationInterpolation Interpolation,
			float Time
		) {
			if (!Keys || Keys->empty()) return std::nullopt;
			if (Time <= Keys->front().Time) return Keys->front().Value;
			if (Time >= Keys->back().Time) return Keys->back().Value;
			const auto Upper = std::upper_bound(Keys->begin(), Keys->end(), Time,
				[](float Value, const ImportedAnimationRotationKey &Key) { return Value < Key.Time; });
			const auto &Right = *Upper;
			const auto &Left = *(Upper - 1);
			if (Interpolation == AssetAnimationInterpolation::Step) return Left.Value;
			auto RightRotation = Right.Value;
			if (glm::dot(Left.Value, RightRotation) < 0.0f) RightRotation = -RightRotation;
			const auto Alpha = (Time - Left.Time) / (Right.Time - Left.Time);
			return glm::normalize(glm::slerp(Left.Value, RightRotation, Alpha));
		}

		RenderMeshIdentity PosedMeshIdentity(ObjectId Object) {
			return {
				.Slot = (static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot,
				.Generation = 2,
			};
		}

		template <typename T>
		std::shared_ptr<std::vector<T>> AcquireBuffer(
			std::vector<std::shared_ptr<std::vector<T>>> &Pool,
			std::size_t Size,
			AnimationRuntimeMetrics &Metrics
		) {
			for (auto &Buffer : Pool) if (Buffer.use_count() == 1) {
				Buffer->resize(Size);
				return Buffer;
			}
			auto Buffer = std::make_shared<std::vector<T>>(Size);
			Pool.push_back(Buffer);
			++Metrics.BufferAllocations;
			return Buffer;
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
			std::vector<LocalPose> LocalPoses;
			std::vector<std::shared_ptr<std::vector<glm::mat4>>> ModelPools;
			std::vector<std::shared_ptr<std::vector<glm::mat4>>> PalettePools;
			std::vector<std::shared_ptr<std::vector<RenderVertex>>> VertexPools;
			AnimationPoseSnapshot Snapshot;
		};

		std::shared_ptr<AssetService> Assets;
		DiagnosticCallback Diagnostic;
		std::map<ObjectId, TrackedAnimator> Animators;
		std::set<std::pair<ObjectId, std::string>> EmittedDiagnostics;
		std::vector<RenderAnimationPoseState> PoseUpdates;
		std::vector<RenderAnimationPoseRemove> PoseRemoves;
		std::vector<ObjectId> ClaimedTargets;
		AnimationRuntimeMetrics Metrics;
		std::uint64_t NextPoseRevision = 0;
		bool ShutDown = false;

		Impl(std::shared_ptr<AssetService> AssetsValue, DiagnosticCallback DiagnosticValue)
			: Assets(std::move(AssetsValue)), Diagnostic(std::move(DiagnosticValue)) {
			PoseUpdates.reserve(64);
			PoseRemoves.reserve(64);
			ClaimedTargets.reserve(AnimationRuntime::MaximumAnimators);
		}

		void Emit(ObjectId Object, std::string Code, std::string Message) {
			if (EmittedDiagnostics.size() >= AnimationRuntime::MaximumAnimators ||
				!EmittedDiagnostics.emplace(Object, Code).second || !Diagnostic) return;
			Diagnostic(std::move(Code), std::move(Message));
		}

		void RemovePose(TrackedAnimator &Tracked) {
			if (Tracked.Published && Tracked.TargetObject.IsValid())
				PoseRemoves.push_back({Tracked.TargetObject});
			Tracked.Published = false;
			Tracked.Snapshot = {};
		}

		void FireEnded(const std::vector<std::shared_ptr<AnimationTrack>> &Tracks) {
			for (const auto &Track : Tracks) if (Track) Track->FirePendingEndedRuntime();
		}
	};

	AnimationRuntime::AnimationRuntime(std::shared_ptr<AssetService> Assets, DiagnosticCallback Diagnostic)
		: State(std::make_unique<Impl>(std::move(Assets), std::move(Diagnostic))) {}

	AnimationRuntime::~AnimationRuntime() {
		Shutdown();
	}

	void AnimationRuntime::RegisterAnimator(const std::shared_ptr<Animator> &AnimatorValue) {
		if (!State || State->ShutDown || !AnimatorValue || AnimatorValue->GetDestroyed() ||
			AnimatorValue->IsDestroying()) return;
		const auto Object = AnimatorValue->GetObjectId();
		if (!Object.IsValid()) return;
		if (!State->Animators.contains(Object) && State->Animators.size() >= MaximumAnimators) {
			State->Emit(Object, "AnimatorLimit", "Animation Animator capacity is exhausted");
			return;
		}
		auto &Tracked = State->Animators[Object];
		Tracked.Value = AnimatorValue;
	}

	void AnimationRuntime::Step(float DeltaTime) {
		if (!State || State->ShutDown) return;
		const auto Started = std::chrono::steady_clock::now();
		State->PoseUpdates.clear();
		State->PoseRemoves.clear();
		if (!std::isfinite(DeltaTime) || DeltaTime < 0.0f) DeltaTime = 0.0f;
		State->Metrics.ActiveRigs = 0;
		State->Metrics.ActiveTracks = 0;
		State->ClaimedTargets.clear();

		for (auto Iterator = State->Animators.begin(); Iterator != State->Animators.end();) {
			const auto Object = Iterator->first;
			auto &Tracked = Iterator->second;
			auto AnimatorValue = Tracked.Value.lock();
			if (!AnimatorValue || AnimatorValue->GetDestroyed() || AnimatorValue->IsDestroying()) {
				State->RemovePose(Tracked);
				if (AnimatorValue) AnimatorValue->InvalidateTracks();
				Iterator = State->Animators.erase(Iterator);
				continue;
			}
			if (!AnimatorValue->GetDataModel()) {
				State->RemovePose(Tracked);
				AnimatorValue->InvalidateTracks();
				Iterator = State->Animators.erase(Iterator);
				continue;
			}

			Tracked.TrackSnapshot = AnimatorValue->Tracks;
			std::ranges::sort(Tracked.TrackSnapshot, {}, &AnimationTrack::CreationSequence);
			for (const auto &Track : Tracked.TrackSnapshot) if (Track) (void)Track->AdvanceRuntime(DeltaTime);

			Tracked.CurrentTrackRevisions.clear();
			std::size_t VisibleTrackCount = 0;
			for (const auto &Track : Tracked.TrackSnapshot) {
				if (!Track || Track->Invalidated) continue;
				Tracked.CurrentTrackRevisions.emplace_back(Track->CreationSequence, Track->Revision);
				if ((Track->PlaybackState == Enums::AnimationPlaybackState::Playing ||
					Track->PlaybackState == Enums::AnimationPlaybackState::Paused || Track->NaturalEndPose) &&
					Track->Weight > 0.0f) ++VisibleTrackCount;
			}
			State->Metrics.ActiveTracks += VisibleTrackCount;
			if (VisibleTrackCount == 0) {
				State->RemovePose(Tracked);
				Tracked.ObservedTrackRevisions = Tracked.CurrentTrackRevisions;
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}

			const auto Parent = AnimatorValue->GetParent();
			auto MeshPartValue = Parent ? std::dynamic_pointer_cast<MeshPart>(*Parent) : nullptr;
			auto Mesh = State->Assets && MeshPartValue && !MeshPartValue->GetDestroyed() && !MeshPartValue->IsDestroying()
				? State->Assets->ResolveMeshResource(MeshPartValue->GetMesh()) : std::nullopt;
			if (!Mesh || !Mesh->Value.Skeleton || !Mesh->Value.Skeleton->Joints ||
				!Mesh->Value.SkinInfluences || !Mesh->Value.Vertices || !Mesh->Value.Indices ||
				Mesh->Value.Skeleton->Joints->empty() ||
				Mesh->Value.Skeleton->Joints->size() > MaximumBonesPerRig) {
				State->RemovePose(Tracked);
				State->Emit(Object, "RigUnavailable", "Animator parent does not resolve to a bounded skinned Mesh asset");
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}
			const auto TargetObject = MeshPartValue->GetObjectId();
			if (!TargetObject.IsValid()) {
				State->RemovePose(Tracked);
				State->Emit(Object, "RigUnavailable", "Animator parent does not have a live render identity");
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}
			const auto ClaimedPosition = std::lower_bound(
				State->ClaimedTargets.begin(), State->ClaimedTargets.end(), TargetObject);
			if (ClaimedPosition != State->ClaimedTargets.end() && *ClaimedPosition == TargetObject) {
				if (Tracked.TargetObject != TargetObject) State->RemovePose(Tracked);
				else {
					Tracked.Published = false;
					Tracked.Snapshot = {};
				}
				State->Emit(Object, "DuplicateAnimator", "Only one active Animator may target a MeshPart");
				State->FireEnded(Tracked.TrackSnapshot);
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
				State->Emit(Object, "IncompatibleSkeleton", "Active Animation track is incompatible with the current Mesh skeleton");
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}

			const bool TrackStateChanged = Tracked.CurrentTrackRevisions != Tracked.ObservedTrackRevisions;
			const bool RigChanged = Tracked.SourceContentRevision != Mesh->ContentRevision ||
				Tracked.SourceMesh != Mesh->Mesh || Tracked.Compatibility != Compatibility || TargetChanged;
			if (!TrackStateChanged && !RigChanged && Tracked.Published) {
				++State->Metrics.ActiveRigs;
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}

			const auto &Joints = *Mesh->Value.Skeleton->Joints;
			Tracked.LocalPoses.resize(Joints.size());
			auto ModelTransforms = AcquireBuffer(Tracked.ModelPools, Joints.size(), State->Metrics);
			auto Palette = AcquireBuffer(Tracked.PalettePools, Joints.size(), State->Metrics);
			bool PoseValid = true;
			const auto SamplingStarted = std::chrono::steady_clock::now();
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				const auto &Joint = Joints[JointIndex];
				auto &Pose = Tracked.LocalPoses[JointIndex];

				glm::vec3 TranslationSum(0.0f), ScaleSum(0.0f);
				float TranslationWeight = 0.0f, ScaleWeight = 0.0f;
				glm::vec4 RotationSum(0.0f);
				float RotationWeight = 0.0f;
				std::optional<glm::quat> RotationReference;
				std::array<std::pair<glm::quat, float>, Animator::MaximumTracks> RotationSamples;
				std::size_t RotationSampleCount = 0;

				for (const auto &Track : Tracked.TrackSnapshot) {
					if (!Track || Track->Invalidated || Track->Weight <= 0.0f ||
						(Track->PlaybackState == Enums::AnimationPlaybackState::Stopped && !Track->NaturalEndPose) ||
						JointIndex >= Track->JointTrackIndices.size()) continue;
					const auto ClipTrackIndex = Track->JointTrackIndices[JointIndex];
					if (ClipTrackIndex < 0 || static_cast<std::size_t>(ClipTrackIndex) >= Track->Resource.Value.Tracks->size()) continue;
					const auto &ClipTrack = (*Track->Resource.Value.Tracks)[ClipTrackIndex];
					if (auto Value = SampleVector(ClipTrack.TranslationKeys, ClipTrack.TranslationInterpolation,
						Track->TimePosition)) {
						TranslationSum += *Value * Track->Weight;
						TranslationWeight += Track->Weight;
					}
					if (auto Value = SampleVector(ClipTrack.ScaleKeys, ClipTrack.ScaleInterpolation,
						Track->TimePosition)) {
						ScaleSum += *Value * Track->Weight;
						ScaleWeight += Track->Weight;
					}
					if (auto Value = SampleRotation(ClipTrack.RotationKeys, ClipTrack.RotationInterpolation,
						Track->TimePosition)) {
						RotationSamples[RotationSampleCount++] = {*Value, Track->Weight};
						RotationWeight += Track->Weight;
					}
				}

				auto BlendVector = [](glm::vec3 Bind, glm::vec3 Sum, float Weight) {
					const auto BindWeight = std::max(0.0f, 1.0f - Weight);
					return (Sum + Bind * BindWeight) / std::max(1.0f, Weight);
				};
				Pose.Translation = BlendVector(Joint.BindTranslation, TranslationSum, TranslationWeight);
				Pose.Scale = BlendVector(Joint.BindScale, ScaleSum, ScaleWeight);
				const auto BindRotationWeight = std::max(0.0f, 1.0f - RotationWeight);
				if (BindRotationWeight > 0.0f) {
					RotationReference = Joint.BindRotation;
					RotationSum += glm::vec4(Joint.BindRotation.x, Joint.BindRotation.y,
						Joint.BindRotation.z, Joint.BindRotation.w) * BindRotationWeight;
				}
				for (std::size_t SampleIndex = 0; SampleIndex < RotationSampleCount; ++SampleIndex) {
					auto [Rotation, Weight] = RotationSamples[SampleIndex];
					if (!RotationReference) RotationReference = Rotation;
					if (glm::dot(*RotationReference, Rotation) < 0.0f) Rotation = -Rotation;
					RotationSum += glm::vec4(Rotation.x, Rotation.y, Rotation.z, Rotation.w) * Weight;
				}
				if (!RotationReference || glm::dot(RotationSum, RotationSum) < 1.0e-12f)
					Pose.Rotation = Joint.BindRotation;
				else Pose.Rotation = glm::normalize(glm::quat(RotationSum.w, RotationSum.x, RotationSum.y, RotationSum.z));

			}
			State->Metrics.SamplingAndBlendingCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SamplingStarted).count());

			const auto HierarchyStarted = std::chrono::steady_clock::now();
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				const auto &Joint = Joints[JointIndex];
				const auto &Pose = Tracked.LocalPoses[JointIndex];
				const auto Local = glm::translate(glm::mat4(1.0f), Pose.Translation) * glm::mat4_cast(Pose.Rotation) *
					glm::scale(glm::mat4(1.0f), Pose.Scale);
				(*ModelTransforms)[JointIndex] = Joint.Parent < 0 ? Local : (*ModelTransforms)[Joint.Parent] * Local;
				if (!IsFinite((*ModelTransforms)[JointIndex])) {
					State->RemovePose(Tracked);
					State->Emit(Object, "InvalidPose", "Animation pose produced a non-finite joint hierarchy");
					PoseValid = false;
					break;
				}
			}
			State->Metrics.HierarchyCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - HierarchyStarted).count());
			if (!PoseValid) {
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}

			const auto SkinMatrixStarted = std::chrono::steady_clock::now();
			for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
				(*Palette)[JointIndex] = (*ModelTransforms)[JointIndex] * Joints[JointIndex].InverseBindMatrix;
				if (!IsFinite((*Palette)[JointIndex])) {
					State->RemovePose(Tracked);
					State->Emit(Object, "InvalidPose", "Animation pose produced a non-finite bone palette");
					PoseValid = false;
					break;
				}
			}
			State->Metrics.SkinMatrixCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SkinMatrixStarted).count());
			if (!PoseValid) {
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}

			auto Vertices = AcquireBuffer(Tracked.VertexPools, Mesh->Value.Vertices->size(), State->Metrics);
			RenderBounds Bounds;
			const auto SkinningStarted = std::chrono::steady_clock::now();
			if (!SkinMeshCpu(Mesh->Value, *Palette, *Vertices, Bounds)) {
				State->Metrics.SkinningCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - SkinningStarted).count());
				State->RemovePose(Tracked);
				State->Emit(Object, "SkinningFailure", "CPU skinning rejected a singular or non-finite pose");
				State->FireEnded(Tracked.TrackSnapshot);
				++Iterator;
				continue;
			}
			State->Metrics.SkinningCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SkinningStarted).count());
			const auto PublicationStarted = std::chrono::steady_clock::now();
			if (State->NextPoseRevision == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("[Animation:Runtime] pose revision is exhausted");
			Tracked.PoseRevision = ++State->NextPoseRevision;
			Tracked.SourceContentRevision = Mesh->ContentRevision;
			Tracked.SourceMesh = Mesh->Mesh;
			Tracked.Compatibility = Compatibility;
			Tracked.ObservedTrackRevisions = Tracked.CurrentTrackRevisions;
			Tracked.Published = true;
			Tracked.Snapshot = {Compatibility, Tracked.PoseRevision, ModelTransforms, Palette};
			State->PoseUpdates.push_back({
				.Pose = {TargetObject, Mesh->Mesh, Tracked.PosedMesh, Tracked.PoseRevision, Palette},
				.TopologyRevision = Mesh->ContentRevision,
				.Vertices = Vertices,
				.Indices = Mesh->Value.Indices,
				.Bounds = Bounds,
			});
			++State->Metrics.ActiveRigs;
			State->Metrics.EvaluatedBones += Joints.size();
			State->Metrics.SkinnedVertices += Vertices->size();
			++State->Metrics.PoseUpdates;
			State->Metrics.PosePublicationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - PublicationStarted).count());
			State->FireEnded(Tracked.TrackSnapshot);
			++Iterator;
		}

		std::erase_if(State->PoseRemoves, [&](const auto &Remove) {
			return std::ranges::any_of(State->PoseUpdates, [&](const auto &Update) {
				return Update.Pose.Object == Remove.Object;
			});
		});
		State->Metrics.TrackedAnimators = State->Animators.size();
		State->Metrics.EvaluationCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count());
	}

	void AnimationRuntime::Shutdown() {
		if (!State || State->ShutDown) return;
		State->ShutDown = true;
		for (auto &[Object, Tracked] : State->Animators) {
			State->RemovePose(Tracked);
			if (auto AnimatorValue = Tracked.Value.lock()) AnimatorValue->InvalidateTracks();
		}
		State->Animators.clear();
		State->Metrics.TrackedAnimators = 0;
		State->Metrics.ActiveRigs = 0;
		State->Metrics.ActiveTracks = 0;
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
		for (const auto &[AnimatorObject, Tracked] : State->Animators) {
			(void)AnimatorObject;
			if (Tracked.Published && Tracked.TargetObject == Object) return Tracked.Snapshot;
		}
		return std::nullopt;
	}

	AnimationRuntimeMetrics AnimationRuntime::GetMetrics() const {
		return State ? State->Metrics : AnimationRuntimeMetrics{};
	}

	bool AnimationRuntime::SkinMeshCpu(
		const ImportedMesh &Mesh,
		std::span<const glm::mat4> BonePalette,
		std::vector<RenderVertex> &Output,
		RenderBounds &Bounds
	) {
		if (!Mesh.Vertices || !Mesh.SkinInfluences || Mesh.Vertices->empty() ||
			Mesh.SkinInfluences->size() != Mesh.Vertices->size() || BonePalette.empty() ||
			BonePalette.size() > MaximumBonesPerRig) return false;
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
				if (!std::isfinite(Weight) || Weight < 0.0f || Joint >= BonePalette.size()) return false;
				if (Weight == 0.0f) continue;
				const auto &Matrix = BonePalette[Joint];
				if (!IsFinite(Matrix)) return false;
				const glm::mat3 Linear(Matrix);
				if (std::abs(glm::determinant(Linear)) < 1.0e-12f) return false;
				const auto NormalMatrix = glm::transpose(glm::inverse(Linear));
				Position += (Matrix * glm::vec4(Source.Position, 1.0f)) * Weight;
				Normal += (NormalMatrix * Source.Normal) * Weight;
				Tangent += (NormalMatrix * glm::vec3(Source.Tangent)) * Weight;
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
