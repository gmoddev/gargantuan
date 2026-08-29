#include "gargantuan/runtime/SemanticSpatialResolver.hpp"

#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace gargantuan {
	namespace {
		glm::mat4 FrameMatrix(const CFrame &Frame) {
			return glm::translate(glm::mat4(1.0f), Frame.Position) * glm::mat4(Frame.Rotation);
		}

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const glm::mat4 &Value) {
			for (glm::length_t Column = 0; Column < 4; ++Column)
				for (glm::length_t Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Value[Column][Row])) return false;
			return true;
		}

		bool Near(const glm::mat4 &Left, const glm::mat4 &Right, float Epsilon = 1.0e-5f) {
			for (glm::length_t Column = 0; Column < 4; ++Column)
				for (glm::length_t Row = 0; Row < 4; ++Row)
					if (std::abs(Left[Column][Row] - Right[Column][Row]) > Epsilon) return false;
			return true;
		}

		std::optional<CFrame> FrameFromMatrix(const glm::mat4 &Matrix) {
			if (!IsFinite(Matrix) || std::abs(Matrix[3].w) < 1.0e-8f) return std::nullopt;
			const auto Position = glm::vec3(Matrix[3]) / Matrix[3].w;
			auto Right = glm::vec3(Matrix[0]);
			auto UpSource = glm::vec3(Matrix[1]);
			const auto BackSource = glm::vec3(Matrix[2]);
			if (!IsFinite(Position) || glm::dot(Right, Right) < 1.0e-12f ||
				glm::dot(UpSource, UpSource) < 1.0e-12f || glm::dot(BackSource, BackSource) < 1.0e-12f)
				return std::nullopt;
			Right = glm::normalize(Right);
			auto Up = UpSource - Right * glm::dot(Right, UpSource);
			if (glm::dot(Up, Up) < 1.0e-12f) Up = glm::cross(BackSource, Right);
			if (glm::dot(Up, Up) < 1.0e-12f) return std::nullopt;
			Up = glm::normalize(Up);
			auto Back = glm::normalize(glm::cross(Right, Up));
			if (glm::dot(Back, BackSource) < 0.0f) {
				Up = -Up;
				Back = -Back;
			}
			return CFrame(Position, glm::mat3(Right, Up, Back));
		}

		std::optional<SemanticSpatialTransform> StaticAttachmentTransform(
			const std::shared_ptr<Attachment> &AttachmentValue,
			std::vector<std::shared_ptr<Instance>> *Observed
		) {
			if (!AttachmentValue || AttachmentValue->GetDestroyed() || AttachmentValue->IsDestroying())
				return std::nullopt;
			std::vector<std::shared_ptr<Attachment>> Chain;
			Chain.reserve(8);
			std::shared_ptr<BasePart> Owner;
			std::shared_ptr<Instance> Current = AttachmentValue;
			for (std::size_t Depth = 0; Current && Depth < SemanticSpatialResolver::MaximumAttachmentDepth; ++Depth) {
				if (Current->GetDestroyed() || Current->IsDestroying()) return std::nullopt;
				if (auto AttachmentNode = std::dynamic_pointer_cast<Attachment>(Current))
					Chain.push_back(std::move(AttachmentNode));
				else if (auto Part = std::dynamic_pointer_cast<BasePart>(Current)) {
					Owner = std::move(Part);
					break;
				}
				auto Parent = Current->GetParent();
				Current = Parent ? *Parent : nullptr;
			}
			if (!Owner) return std::nullopt;
			std::ranges::reverse(Chain);
			auto Matrix = FrameMatrix(Owner->GetCFrame());
			if (Observed) Observed->push_back(Owner);
			for (const auto &Node : Chain) {
				Matrix *= FrameMatrix(Node->GetCFrame());
				if (Observed) Observed->push_back(Node);
			}
			auto WorldFrame = FrameFromMatrix(Matrix);
			if (!WorldFrame) return std::nullopt;
			return SemanticSpatialTransform{Matrix, *WorldFrame, 0, false};
		}
	}

	struct SemanticSpatialResolver::Impl {
		struct Entry {
			std::weak_ptr<Attachment> Value;
			std::weak_ptr<BasePart> Owner;
			std::vector<std::weak_ptr<Attachment>> Chain;
			std::vector<SignalConnection::Pointer> Connections;
			ObjectId ParentAttachment;
			ObjectId RigObject;
			std::optional<AssetContentId> BindingCompatibility;
			SemanticSpatialTransform Cached;
			bool Available = false;
			bool TopologyDirty = true;
		};

		struct RigRecord {
			std::weak_ptr<MeshPart> Owner;
			std::vector<ObjectId> Attachments;
			std::vector<SignalConnection::Pointer> OwnerConnections;
			std::string MeshReference;
			std::uint64_t ContentRevision = 0;
			AssetContentId Compatibility;
			std::shared_ptr<const std::vector<glm::mat4>> PoseTransforms;
			std::vector<glm::mat4> BindTransforms;
			std::map<std::string, std::size_t, std::less<>> JointIndices;
			glm::mat4 OwnerMatrix{1.0f};
			std::uint64_t PoseRevision = 0;
			bool SkeletonAvailable = false;
			bool ArtifactDirty = true;
			bool Available = false;
			bool Initialized = false;
			bool DirtyAll = true;
		};

		std::shared_ptr<AssetService> Assets;
		AnimationRuntime *Animation = nullptr;
		DiagnosticCallback Diagnostic;
		std::map<ObjectId, Entry> Entries;
		std::map<ObjectId, std::vector<ObjectId>> AttachmentChildren;
		std::map<ObjectId, RigRecord> Rigs;
		std::set<ObjectId> DirtyAttachments;
		std::set<std::pair<ObjectId, std::string>> EmittedDiagnostics;
		std::vector<ObjectId> ChangedRigs;
		std::vector<ObjectId> WorkAttachments;
		SemanticSpatialMetrics Metrics;
		std::uint64_t NextRevision = 0;
		std::uint64_t AssetChangeSequence = 1;
		std::size_t IndexedSemanticAnchors = 0;
		bool ShutDown = false;

		Impl(
			std::shared_ptr<AssetService> AssetsValue,
			AnimationRuntime *AnimationValue,
			DiagnosticCallback DiagnosticValue
		) : Assets(std::move(AssetsValue)), Animation(AnimationValue), Diagnostic(std::move(DiagnosticValue)) {
			ChangedRigs.reserve(64);
			WorkAttachments.reserve(256);
			if (Assets) AssetChangeSequence = Assets->ReadChanges(0).NextSequence;
		}

		void Emit(ObjectId Object, std::string Code, std::string Message) {
			if (EmittedDiagnostics.size() >= SemanticSpatialResolver::MaximumRegisteredAttachments ||
				!EmittedDiagnostics.emplace(Object, Code).second || !Diagnostic) return;
			Diagnostic(std::move(Code), std::move(Message));
		}

		void RemoveChildLink(ObjectId Parent, ObjectId Child) {
			if (!Parent.IsValid()) return;
			if (auto Found = AttachmentChildren.find(Parent); Found != AttachmentChildren.end()) {
				std::erase(Found->second, Child);
				if (Found->second.empty()) AttachmentChildren.erase(Found);
			}
		}

		void AddChildLink(ObjectId Parent, ObjectId Child) {
			if (!Parent.IsValid()) return;
			auto &Children = AttachmentChildren[Parent];
			auto Position = std::ranges::lower_bound(Children, Child);
			if (Position == Children.end() || *Position != Child) Children.insert(Position, Child);
		}

		void RemoveFromRig(ObjectId RigObject, ObjectId AttachmentObject) {
			if (!RigObject.IsValid()) return;
			auto Found = Rigs.find(RigObject);
			if (Found == Rigs.end()) return;
			const auto PreviousSize = Found->second.Attachments.size();
			std::erase(Found->second.Attachments, AttachmentObject);
			if (Found->second.Attachments.size() != PreviousSize && IndexedSemanticAnchors > 0)
				--IndexedSemanticAnchors;
			if (Found->second.Attachments.empty()) {
				for (auto &Connection : Found->second.OwnerConnections)
					if (Connection) Connection->Disconnect();
				Rigs.erase(Found);
			}
		}

		bool AddToRig(
			ObjectId RigObject,
			const std::shared_ptr<MeshPart> &Owner,
			ObjectId AttachmentObject
		) {
			if (IndexedSemanticAnchors >= SemanticSpatialResolver::MaximumSemanticAnchors) {
				Emit(AttachmentObject, "SemanticAnchorLimit", "Semantic Attachment world capacity is exhausted");
				return false;
			}
			auto [RigPosition, Inserted] = Rigs.try_emplace(RigObject);
			auto &Rig = RigPosition->second;
			Rig.Owner = Owner;
			if (Inserted) {
				auto MarkRigDirty = [this, RigObject](std::monostate) {
					if (auto Found = Rigs.find(RigObject); Found != Rigs.end()) Found->second.DirtyAll = true;
				};
				for (const auto Name : {"CFrame", "Size"})
					Rig.OwnerConnections.push_back(Owner->GetPropertyChangedSignal(Name)->Connect(MarkRigDirty));
				Rig.OwnerConnections.push_back(Owner->GetPropertyChangedSignal("Mesh")->Connect(
					[this, RigObject](std::monostate) {
						if (auto Found = Rigs.find(RigObject); Found != Rigs.end()) {
							Found->second.ArtifactDirty = true;
							Found->second.DirtyAll = true;
						}
					}
				));
				Rig.OwnerConnections.push_back(Owner->Destroying->Once(MarkRigDirty));
			}
			if (Rig.Attachments.size() >= SemanticSpatialResolver::MaximumSemanticAnchorsPerRig) {
				Emit(AttachmentObject, "RigAnchorLimit", "Semantic Attachment capacity for this MeshPart is exhausted");
				if (Rig.Attachments.empty()) {
					for (auto &Connection : Rig.OwnerConnections)
						if (Connection) Connection->Disconnect();
					Rigs.erase(RigObject);
				}
				return false;
			}
			auto Position = std::ranges::lower_bound(Rig.Attachments, AttachmentObject);
			if (Position == Rig.Attachments.end() || *Position != AttachmentObject) {
				Rig.Attachments.insert(Position, AttachmentObject);
				++IndexedSemanticAnchors;
			}
			return true;
		}

		void MarkDirty(ObjectId Object, bool Topology, bool ResetCompatibility, std::size_t Depth = 0) {
			if (ShutDown || Depth > SemanticSpatialResolver::MaximumAttachmentDepth) return;
			auto Found = Entries.find(Object);
			if (Found == Entries.end()) return;
			DirtyAttachments.insert(Object);
			++Metrics.DirtyMarks;
			if (Topology) Found->second.TopologyDirty = true;
			if (ResetCompatibility) Found->second.BindingCompatibility.reset();
			if (auto Children = AttachmentChildren.find(Object); Children != AttachmentChildren.end())
				for (const auto Child : Children->second)
					MarkDirty(Child, Topology, ResetCompatibility, Depth + 1);
		}

		void Unregister(ObjectId Object) {
			auto Found = Entries.find(Object);
			if (Found == Entries.end()) return;
			RemoveChildLink(Found->second.ParentAttachment, Object);
			RemoveFromRig(Found->second.RigObject, Object);
			for (auto &Connection : Found->second.Connections)
				if (Connection) Connection->Disconnect();
			if (auto Value = Found->second.Value.lock()) Value->DetachSpatialRuntime(nullptr);
			Entries.erase(Found);
			DirtyAttachments.erase(Object);
			AttachmentChildren.erase(Object);
		}

		void RefreshTopology(ObjectId Object) {
			auto Found = Entries.find(Object);
			if (Found == Entries.end()) return;
			auto &EntryValue = Found->second;
			auto AttachmentValue = EntryValue.Value.lock();
			if (!AttachmentValue || AttachmentValue->GetDestroyed() || AttachmentValue->IsDestroying()) {
				Unregister(Object);
				return;
			}
			RemoveChildLink(EntryValue.ParentAttachment, Object);
			RemoveFromRig(EntryValue.RigObject, Object);
			EntryValue.ParentAttachment = {};
			EntryValue.RigObject = {};
			EntryValue.Owner.reset();
			EntryValue.Chain.clear();

			std::vector<std::shared_ptr<Attachment>> ReverseChain;
			ReverseChain.reserve(8);
			std::shared_ptr<BasePart> Owner;
			std::shared_ptr<Instance> Current = AttachmentValue;
			bool HasBinding = false;
			for (std::size_t Depth = 0; Current && Depth < SemanticSpatialResolver::MaximumAttachmentDepth; ++Depth) {
				if (Current->GetDestroyed() || Current->IsDestroying()) break;
				if (auto Node = std::dynamic_pointer_cast<Attachment>(Current)) {
					if (Node.get() != AttachmentValue.get() && !EntryValue.ParentAttachment.IsValid())
						EntryValue.ParentAttachment = Node->GetObjectId();
					HasBinding = HasBinding || !Node->JointPath.empty();
					ReverseChain.push_back(std::move(Node));
				} else if (auto Part = std::dynamic_pointer_cast<BasePart>(Current)) {
					Owner = std::move(Part);
					break;
				}
				auto Parent = Current->GetParent();
				Current = Parent ? *Parent : nullptr;
			}
			if (!Owner && Current)
				Emit(Object, "AttachmentDepth", "Attachment ancestry exceeds the semantic depth bound");
			std::ranges::reverse(ReverseChain);
			EntryValue.Chain.reserve(ReverseChain.size());
			for (const auto &Node : ReverseChain) EntryValue.Chain.push_back(Node);
			EntryValue.Owner = Owner;
			AddChildLink(EntryValue.ParentAttachment, Object);
			if (HasBinding)
				if (auto MeshOwner = std::dynamic_pointer_cast<MeshPart>(Owner))
					if (AddToRig(MeshOwner->GetObjectId(), MeshOwner, Object))
						EntryValue.RigObject = MeshOwner->GetObjectId();
			EntryValue.TopologyDirty = false;
		}

		bool RefreshRig(RigRecord &Rig, ObjectId RigObject) {
			++Metrics.RigsVisited;
			auto Owner = Rig.Owner.lock();
			bool Changed = !Rig.Initialized;
			if (!Owner || Owner->GetDestroyed() || Owner->IsDestroying()) {
				Changed = Changed || Rig.Available;
				Rig.Available = false;
				Rig.PoseTransforms.reset();
				Rig.PoseRevision = 0;
				Rig.Initialized = true;
				Rig.DirtyAll = Rig.DirtyAll || Changed;
				return Changed;
			}
			const auto OwnerMatrix = FrameMatrix(Owner->GetCFrame()) *
				glm::scale(glm::mat4(1.0f), Owner->GetSize());
			if (!Near(Rig.OwnerMatrix, OwnerMatrix)) Changed = true;
			Rig.OwnerMatrix = OwnerMatrix;

			if (Rig.ArtifactDirty || !Rig.Initialized) {
				const auto MeshReference = Owner->GetMesh();
				const bool MeshChanged = Rig.MeshReference != MeshReference;
				auto Resource = Assets ? Assets->ResolveMeshResource(MeshReference) : std::nullopt;
				const bool ResourceValid = Resource && Resource->Value.Skeleton && Resource->Value.Skeleton->Joints &&
					!Resource->Value.Skeleton->Joints->empty() &&
					Resource->Value.Skeleton->Joints->size() <= AnimationRuntime::MaximumBonesPerRig;
				const auto ContentRevision = ResourceValid ? Resource->ContentRevision : 0;
				const auto Compatibility = ResourceValid ? Resource->Value.Skeleton->CompatibilityId : AssetContentId{};
				const bool ArtifactChanged = MeshChanged || Rig.ContentRevision != ContentRevision ||
					Rig.Compatibility != Compatibility || Rig.SkeletonAvailable != ResourceValid;
				Changed = true;
				Rig.MeshReference = MeshReference;
				Rig.ContentRevision = ContentRevision;
				Rig.Compatibility = Compatibility;
				Rig.SkeletonAvailable = ResourceValid;
				Rig.BindTransforms.clear();
				Rig.JointIndices.clear();
				if (ResourceValid) {
					const auto &Joints = *Resource->Value.Skeleton->Joints;
					Rig.BindTransforms.resize(Joints.size());
					for (std::size_t JointIndex = 0; JointIndex < Joints.size(); ++JointIndex) {
						const auto &Joint = Joints[JointIndex];
						const auto Local = glm::translate(glm::mat4(1.0f), Joint.BindTranslation) *
							glm::mat4_cast(Joint.BindRotation) * glm::scale(glm::mat4(1.0f), Joint.BindScale);
						Rig.BindTransforms[JointIndex] = Joint.Parent < 0
							? Local : Rig.BindTransforms[static_cast<std::size_t>(Joint.Parent)] * Local;
						Rig.JointIndices.emplace(Joint.Path, JointIndex);
					}
				}
				Rig.ArtifactDirty = false;
				Changed = Changed || ArtifactChanged;
			}
			Rig.Available = Rig.SkeletonAvailable && IsFinite(OwnerMatrix);

			auto Pose = Animation && Rig.Available ? Animation->GetPose(RigObject) : std::nullopt;
			const bool PoseValid = Pose && Pose->SkeletonCompatibilityId == Rig.Compatibility &&
				Pose->JointModelTransforms && Pose->JointModelTransforms->size() == Rig.BindTransforms.size();
			const auto PoseRevision = PoseValid ? Pose->PoseRevision : 0;
			if (Rig.PoseRevision != PoseRevision) Changed = true;
			Rig.PoseRevision = PoseRevision;
			Rig.PoseTransforms = PoseValid ? Pose->JointModelTransforms : nullptr;
			Rig.Initialized = true;
			Rig.DirtyAll = Rig.DirtyAll || Changed;
			return Changed;
		}

		std::optional<SemanticSpatialTransform> ResolveEntry(ObjectId Object, bool RefreshRigState = true) {
			auto Found = Entries.find(Object);
			if (Found == Entries.end()) return std::nullopt;
			auto &EntryValue = Found->second;
			auto AttachmentValue = EntryValue.Value.lock();
			if (!AttachmentValue || AttachmentValue->GetDestroyed() || AttachmentValue->IsDestroying()) return std::nullopt;
			auto Owner = EntryValue.Owner.lock();
			if (!Owner || Owner->GetDestroyed() || Owner->IsDestroying()) {
				EntryValue.Available = false;
				return std::nullopt;
			}

			auto Matrix = FrameMatrix(Owner->GetCFrame());
			bool UsedAnimatedBinding = false;
			bool HadBinding = false;
			RigRecord *Rig = nullptr;
			if (EntryValue.RigObject.IsValid()) {
				auto RigFound = Rigs.find(EntryValue.RigObject);
				if (RigFound != Rigs.end()) {
					Rig = &RigFound->second;
					if (RefreshRigState) RefreshRig(*Rig, EntryValue.RigObject);
				}
			}
			for (const auto &WeakNode : EntryValue.Chain) {
				auto Node = WeakNode.lock();
				if (!Node || Node->GetDestroyed() || Node->IsDestroying()) return std::nullopt;
				const auto Local = FrameMatrix(Node->GetCFrame());
				const auto &JointPath = Node->JointPath;
				if (JointPath.empty()) {
					Matrix *= Local;
					continue;
				}
				HadBinding = true;
				bool Resolved = false;
				if (Rig && Rig->Available) {
					if (EntryValue.BindingCompatibility && *EntryValue.BindingCompatibility != Rig->Compatibility) {
						Emit(Object, "IncompatibleSkeleton",
							"Attachment JointPath was invalidated by an incompatible Mesh skeleton revision");
					} else if (auto Joint = Rig->JointIndices.find(JointPath); Joint != Rig->JointIndices.end()) {
						const auto &Transforms = Rig->PoseTransforms ? *Rig->PoseTransforms : Rig->BindTransforms;
						if (Joint->second < Transforms.size()) {
							if (!EntryValue.BindingCompatibility) EntryValue.BindingCompatibility = Rig->Compatibility;
							Matrix = Rig->OwnerMatrix * Transforms[Joint->second] * Local;
							UsedAnimatedBinding = true;
							Resolved = true;
						}
					} else {
						Emit(Object, "JointPathUnavailable",
							"Attachment JointPath does not exist in the compatible Mesh skeleton");
					}
				} else {
					Emit(Object, "RigUnavailable",
						"Attachment JointPath has no bounded compatible Mesh skeleton and uses static semantics");
				}
				if (!Resolved) Matrix *= Local;
			}
			if (!IsFinite(Matrix)) {
				Emit(Object, "InvalidTransform", "Semantic Attachment transform is non-finite");
				return std::nullopt;
			}
			auto WorldFrame = FrameFromMatrix(Matrix);
			if (!WorldFrame) {
				Emit(Object, "InvalidTransform", "Semantic Attachment transform cannot produce a bounded CFrame");
				return std::nullopt;
			}
			++Metrics.AnchorResolutions;
			if (UsedAnimatedBinding) ++Metrics.AnimatedAnchorResolutions;
			else if (HadBinding) ++Metrics.StaticFallbackResolutions;
			const bool Changed = !EntryValue.Available || !Near(EntryValue.Cached.Matrix, Matrix) ||
				EntryValue.Cached.Animated != UsedAnimatedBinding;
			if (!Changed) {
				++Metrics.CacheHits;
				return EntryValue.Cached;
			}
			if (NextRevision == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("[Spatial:Semantic] anchor revision is exhausted");
			EntryValue.Cached = {Matrix, *WorldFrame, ++NextRevision, UsedAnimatedBinding};
			EntryValue.Available = true;
			++Metrics.ChangedAnchors;
			AttachmentValue->FireWorldCFrameChanged();
			return EntryValue.Cached;
		}
	};

	SemanticSpatialResolver::SemanticSpatialResolver(
		std::shared_ptr<AssetService> Assets,
		AnimationRuntime *Animation,
		DiagnosticCallback Diagnostic
	) : State(std::make_unique<Impl>(std::move(Assets), Animation, std::move(Diagnostic))) {}

	SemanticSpatialResolver::~SemanticSpatialResolver() {
		Shutdown();
	}

	void SemanticSpatialResolver::RegisterAttachment(const std::shared_ptr<Attachment> &AttachmentValue) {
		if (!State || State->ShutDown || !AttachmentValue || AttachmentValue->GetDestroyed() ||
			AttachmentValue->IsDestroying()) return;
		const auto Object = AttachmentValue->GetObjectId();
		if (!Object.IsValid()) return;
		if (State->Entries.contains(Object)) {
			State->MarkDirty(Object, true, true);
			return;
		}
		if (State->Entries.size() >= MaximumRegisteredAttachments) {
			State->Emit(Object, "AttachmentLimit", "Semantic spatial Attachment registration capacity is exhausted");
			return;
		}
		auto [Position, Inserted] = State->Entries.try_emplace(Object);
		if (!Inserted) return;
		auto &EntryValue = Position->second;
		EntryValue.Value = AttachmentValue;
		if (auto Self = weak_from_this().lock()) AttachmentValue->AttachSpatialRuntime(Self);
		auto WeakSelf = weak_from_this();
		EntryValue.Connections.push_back(AttachmentValue->GetPropertyChangedSignal("CFrame")->Connect(
			[WeakSelf, Object](std::monostate) {
				if (auto Self = WeakSelf.lock()) {
					const auto Started = std::chrono::steady_clock::now();
					Self->State->MarkDirty(Object, false, false);
					Self->State->Metrics.DirtyPropagationCpuNanoseconds += static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - Started).count());
				}
			}
		));
		EntryValue.Connections.push_back(AttachmentValue->GetPropertyChangedSignal("JointPath")->Connect(
			[WeakSelf, Object](std::monostate) {
				if (auto Self = WeakSelf.lock()) Self->State->MarkDirty(Object, true, true);
			}
		));
		EntryValue.Connections.push_back(AttachmentValue->AncestryChanged->Connect(
			[WeakSelf, Object](std::tuple<std::shared_ptr<Instance>, std::shared_ptr<Instance>>) {
				if (auto Self = WeakSelf.lock()) Self->State->MarkDirty(Object, true, true);
			}
		));
		EntryValue.Connections.push_back(AttachmentValue->Destroying->Once([WeakSelf, Object](std::monostate) {
			if (auto Self = WeakSelf.lock()) Self->State->Unregister(Object);
		}));
		State->DirtyAttachments.insert(Object);
	}

	void SemanticSpatialResolver::Step() {
		if (!State || State->ShutDown) return;
		const auto Started = std::chrono::steady_clock::now();
		if (State->Assets) {
			auto Changes = State->Assets->ReadChanges(State->AssetChangeSequence);
			State->AssetChangeSequence = Changes.NextSequence;
			if (Changes.RescanRequired)
				for (auto &[Object, Rig] : State->Rigs) {
					(void)Object;
					Rig.ArtifactDirty = true;
				}
			for (const auto &Change : Changes.Changes) {
				if (Change.Kind != AssetKind::Mesh) continue;
				for (auto &[Object, Rig] : State->Rigs) {
					(void)Object;
					if (Rig.MeshReference == Change.Reference) Rig.ArtifactDirty = true;
				}
			}
		}
		State->WorkAttachments.clear();
		std::size_t Processed = 0;
		while (!State->DirtyAttachments.empty() && Processed < MaximumRegisteredAttachments * 2) {
			const auto Object = *State->DirtyAttachments.begin();
			State->DirtyAttachments.erase(State->DirtyAttachments.begin());
			auto Found = State->Entries.find(Object);
			if (Found == State->Entries.end()) continue;
			if (Found->second.TopologyDirty) State->RefreshTopology(Object);
			if (State->Entries.contains(Object)) State->WorkAttachments.push_back(Object);
			++Processed;
		}

		State->ChangedRigs.clear();
		const auto BindingStarted = std::chrono::steady_clock::now();
		for (auto &[RigObject, Rig] : State->Rigs) {
			State->RefreshRig(Rig, RigObject);
			if (Rig.DirtyAll) State->ChangedRigs.push_back(RigObject);
		}
		for (const auto RigObject : State->ChangedRigs) {
			auto Found = State->Rigs.find(RigObject);
			if (Found == State->Rigs.end()) continue;
			State->WorkAttachments.insert(State->WorkAttachments.end(),
				Found->second.Attachments.begin(), Found->second.Attachments.end());
			Found->second.DirtyAll = false;
		}
		State->Metrics.BindingBookkeepingCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - BindingStarted).count());
		std::ranges::sort(State->WorkAttachments);
		State->WorkAttachments.erase(
			std::unique(State->WorkAttachments.begin(), State->WorkAttachments.end()),
			State->WorkAttachments.end());
		const auto ResolutionStarted = std::chrono::steady_clock::now();
		for (const auto Object : State->WorkAttachments)
			if (State->Entries.contains(Object)) (void)State->ResolveEntry(Object, false);
		State->Metrics.TransformResolutionCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - ResolutionStarted).count());
		State->Metrics.ResolutionCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Started).count());
	}

	void SemanticSpatialResolver::Shutdown() {
		if (!State || State->ShutDown) return;
		State->ShutDown = true;
		for (auto &[Object, EntryValue] : State->Entries) {
			(void)Object;
			for (auto &Connection : EntryValue.Connections)
				if (Connection) Connection->Disconnect();
			if (auto Value = EntryValue.Value.lock()) Value->DetachSpatialRuntime(this);
		}
		for (auto &[Object, Rig] : State->Rigs) {
			(void)Object;
			for (auto &Connection : Rig.OwnerConnections)
				if (Connection) Connection->Disconnect();
		}
		State->Entries.clear();
		State->AttachmentChildren.clear();
		State->Rigs.clear();
		State->DirtyAttachments.clear();
		State->IndexedSemanticAnchors = 0;
	}

	std::optional<SemanticSpatialTransform>
	SemanticSpatialResolver::ResolveAttachment(const std::shared_ptr<Attachment> &AttachmentValue) {
		if (!AttachmentValue || AttachmentValue->GetDestroyed() || AttachmentValue->IsDestroying()) return std::nullopt;
		if (!State || State->ShutDown) return ResolveStaticAttachment(AttachmentValue);
		const auto Object = AttachmentValue->GetObjectId();
		if (!State->Entries.contains(Object)) RegisterAttachment(AttachmentValue);
		auto Found = State->Entries.find(Object);
		if (Found == State->Entries.end()) return ResolveStaticAttachment(AttachmentValue);
		if (Found->second.TopologyDirty) State->RefreshTopology(Object);
		const bool AttachmentDirty = State->DirtyAttachments.erase(Object) != 0;
		if (!AttachmentDirty && Found->second.Available && Found->second.RigObject.IsValid()) {
			auto Rig = State->Rigs.find(Found->second.RigObject);
			if (Rig != State->Rigs.end() && !Rig->second.DirtyAll) {
				++State->Metrics.CacheHits;
				return Found->second.Cached;
			}
		}
		if (auto Result = State->ResolveEntry(Object)) return Result;
		return ResolveStaticAttachment(AttachmentValue);
	}

	std::optional<SemanticSpatialTransform> SemanticSpatialResolver::ResolveWorldTransform(
		const std::shared_ptr<Instance> &Value,
		std::vector<std::shared_ptr<Instance>> *Observed
	) {
		if (!Value || Value->GetDestroyed() || Value->IsDestroying()) return std::nullopt;
		std::shared_ptr<Instance> Current = Value;
		if (!std::dynamic_pointer_cast<Attachment>(Current) && !std::dynamic_pointer_cast<BasePart>(Current)) {
			auto Parent = Current->GetParent();
			Current = Parent ? *Parent : nullptr;
		}
		for (std::size_t Depth = 0; Current && Depth < MaximumAttachmentDepth; ++Depth) {
			if (Current->GetDestroyed() || Current->IsDestroying()) return std::nullopt;
			if (auto AttachmentValue = std::dynamic_pointer_cast<Attachment>(Current)) {
				auto Result = ResolveAttachment(AttachmentValue);
				if (Observed) {
					if (State && !State->ShutDown) {
						auto Found = State->Entries.find(AttachmentValue->GetObjectId());
						if (Found == State->Entries.end()) {
							(void)StaticAttachmentTransform(AttachmentValue, Observed);
							return Result;
						}
						if (auto Owner = Found->second.Owner.lock()) Observed->push_back(Owner);
						for (const auto &WeakNode : Found->second.Chain)
							if (auto Node = WeakNode.lock()) Observed->push_back(Node);
					} else {
						(void)StaticAttachmentTransform(AttachmentValue, Observed);
					}
				}
				return Result;
			}
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Current)) {
				if (Observed) Observed->push_back(Part);
				const auto Matrix = FrameMatrix(Part->GetCFrame());
				auto WorldFrame = FrameFromMatrix(Matrix);
				return WorldFrame ? std::optional(SemanticSpatialTransform{Matrix, *WorldFrame, 0, false})
					: std::nullopt;
			}
			auto Parent = Current->GetParent();
			Current = Parent ? *Parent : nullptr;
		}
		return std::nullopt;
	}

	SemanticSpatialMetrics SemanticSpatialResolver::GetMetrics() const {
		if (!State) return {};
		auto Result = State->Metrics;
		Result.RegisteredAttachments = State->Entries.size();
		Result.IndexedSemanticAnchors = State->IndexedSemanticAnchors;
		Result.IndexedRigs = State->Rigs.size();
		return Result;
	}

	std::optional<SemanticSpatialTransform>
	SemanticSpatialResolver::ResolveStaticAttachment(const std::shared_ptr<Attachment> &AttachmentValue) {
		return StaticAttachmentTransform(AttachmentValue, nullptr);
	}

	std::optional<SemanticSpatialTransform>
	SemanticSpatialResolver::ResolveStaticWorldTransform(const std::shared_ptr<Instance> &Value) {
		if (!Value || Value->GetDestroyed() || Value->IsDestroying()) return std::nullopt;
		std::shared_ptr<Instance> Current = Value;
		if (!std::dynamic_pointer_cast<Attachment>(Current) && !std::dynamic_pointer_cast<BasePart>(Current)) {
			auto Parent = Current->GetParent();
			Current = Parent ? *Parent : nullptr;
		}
		for (std::size_t Depth = 0; Current && Depth < MaximumAttachmentDepth; ++Depth) {
			if (auto AttachmentValue = std::dynamic_pointer_cast<Attachment>(Current))
				return ResolveStaticAttachment(AttachmentValue);
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Current)) {
				const auto Matrix = FrameMatrix(Part->GetCFrame());
				auto WorldFrame = FrameFromMatrix(Matrix);
				return WorldFrame ? std::optional(SemanticSpatialTransform{Matrix, *WorldFrame, 0, false})
					: std::nullopt;
			}
			auto Parent = Current->GetParent();
			Current = Parent ? *Parent : nullptr;
		}
		return std::nullopt;
	}
}
