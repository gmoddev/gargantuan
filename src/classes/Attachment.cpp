#include "gargantuan/classes/Attachment.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace gargantuan {
	namespace {
		void ValidateJointPath(std::string_view Value) {
			if (Value.empty()) return;
			if (Value.front() == '/' || Value.back() == '/' || Value.find('\\') != std::string_view::npos)
				throw std::invalid_argument("Attachment JointPath is not canonical");
			for (std::size_t Begin = 0; Begin < Value.size();) {
				const auto End = Value.find('/', Begin);
				const auto Segment = Value.substr(Begin, End == std::string_view::npos ? Value.size() - Begin : End - Begin);
				if (Segment.empty() || Segment == "." || Segment == "..")
					throw std::invalid_argument("Attachment JointPath contains an invalid segment");
				if (End == std::string_view::npos) break;
				Begin = End + 1;
			}
		}
	}

	std::string Attachment::GetJointPath() const {
		return JointPath;
	}

	void Attachment::SetJointPath(std::string Value) {
		AssertCanMutate();
		ValidateProtocolString(Value, AssetLimits::MaximumJointPathBytes, "Attachment JointPath");
		ValidateJointPath(Value);
		ValidatePropertyMutation("JointPath", Value);
		if (JointPath == Value) return;
		JointPath = std::move(Value);
		NotifyPropertyCommitted("JointPath");
	}

	CFrame Attachment::GetWorldCFrame() const {
		auto Self = std::dynamic_pointer_cast<Attachment>(
			const_cast<Attachment *>(this)->shared_from_this());
		if (auto Runtime = SpatialRuntime.lock())
			if (auto Transform = Runtime->ResolveAttachment(Self)) return Transform->WorldCFrame;
		if (auto Transform = SemanticSpatialResolver::ResolveStaticAttachment(Self))
			return Transform->WorldCFrame;
		return GetCFrame();
	}

	void Attachment::AttachSpatialRuntime(const std::shared_ptr<SemanticSpatialResolver> &Runtime) {
		SpatialRuntime = Runtime;
	}

	void Attachment::DetachSpatialRuntime(const SemanticSpatialResolver *Runtime) {
		if (auto Current = SpatialRuntime.lock(); Current && Current.get() != Runtime) return;
		SpatialRuntime.reset();
	}

	void Attachment::FireWorldCFrameChanged() {
		if (auto Signal = PropertyChangedSignals.find("WorldCFrame"); Signal != PropertyChangedSignals.end())
			Signal->second->Fire({});
	}
}
