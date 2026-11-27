#pragma once
#include "Box3DConversions.hpp"
#include "gargantuan/physics/PhysicsQueryTypes.hpp"
#include <box3d/collision.h>
#include <array>
#include <cmath>

namespace gargantuan {
	// The proxy borrows Points for its duration. The caller validates the rigid world transform.
	struct Box3DShapeQuery {
		std::array<b3Vec3,8> Points{};
		b3ShapeProxy Proxy{};
		b3Pos Origin{};
		bool Set(const PhysicsQueryShape& Shape) {
			Origin={Shape.Transform.Position.x,Shape.Transform.Position.y,Shape.Transform.Position.z};
			Proxy={};
			Proxy.points=Points.data();
			const auto SetPoint=[&](int I,glm::vec3 Local) { Points[I]=Box3DConversions::ToBox3(Shape.Transform.Rotation*Local); };
			if (Shape.Kind==PhysicsQueryShapeKind::Box) {
				for (unsigned I=0;I<3;++I)
					if (!std::isfinite(Shape.Size[I]) || Shape.Size[I]<=0 || Shape.Size[I]>MaximumPhysicsQueryExtent) return false;
				Proxy.count=8;
				for (int I=0;I<8;++I) SetPoint(I,Shape.Size*glm::vec3(I&1?.5f:-.5f,I&2?.5f:-.5f,I&4?.5f:-.5f));
			} else if (Shape.Kind==PhysicsQueryShapeKind::Sphere || Shape.Kind==PhysicsQueryShapeKind::Capsule) {
				if (!std::isfinite(Shape.Radius) || Shape.Radius<=0 || Shape.Radius>MaximumPhysicsQueryExtent) return false;
				Proxy.radius=Shape.Radius;
				Proxy.count=Shape.Kind==PhysicsQueryShapeKind::Sphere?1:2;
				if (Proxy.count==1) SetPoint(0,{});
				else {
					if (!std::isfinite(Shape.Height) || Shape.Height<2*Shape.Radius || Shape.Height>MaximumPhysicsQueryExtent) return false;
					const float Offset=Shape.Height*.5f-Shape.Radius;
					SetPoint(0,{0,-Offset,0}); SetPoint(1,{0,Offset,0});
				}
			} else return false;
			return true;
		}
	};
}
