// Included inside the private Box3D adapter; no native query storage escapes.
            using ShapeQueryProxy = Box3DShapeQuery;
            bool BuildShapeQueryProxy(const PhysicsQueryShape &Shape, ShapeQueryProxy &Out) const {
                return IsRigidRotation(Shape.Transform) && Out.Set(Shape);
            }
            bool AcceptAdvancedQueryBody(PhysicsBodyId Body, const PhysicsQueryFilter &Filter) const {
                const auto *Record = FindBody(Body);
                if (!Record || !Record->Description.CanQuery.value_or(Record->Description.CanCollide)) return false;
                if (Record->Description.RequiresBlockGridQuery && !Filter.IncludeBlockGrids) return false;
                if (!PhysicsCanCollide(Filter.CollisionGroup, Filter.CollisionMask,
                    Record->Description.CollisionGroup, Record->Description.CollisionMask)) return false;
                const bool Listed = std::binary_search(Filter.Bodies.begin(), Filter.Bodies.end(), Body);
                return Filter.Type == PhysicsQueryFilterType::Include ? Listed : !Listed;
            }
            [[nodiscard]] PhysicsRaycastResult ShapeCast(const PhysicsShapeCastRequest &Request) const override {
                PhysicsRaycastResult Result;
                ShapeQueryProxy Proxy;
                const float Distance = glm::length(Request.Direction);
                if (!b3World_IsValid(World)) return {.Status=PhysicsOperationStatus::BackendFailure, .Message="Physics world unavailable"};
                if (!BuildShapeQueryProxy(Request.Shape, Proxy) || !IsFinite(Request.Direction) ||
                    !std::isfinite(Distance) || Distance < MinimumRaycastDistance || Distance > MaximumRaycastDistance ||
                    Request.Filter.CollisionGroup >= 16 || Request.Filter.Bodies.size() > MaximumRaycastFilterBodies ||
                    !std::ranges::is_sorted(Request.Filter.Bodies))
                    return {.Status=PhysicsOperationStatus::InvalidDescription, .Message="Invalid or oversized shape cast"};
                struct ContextType {
                    const Box3DPhysicsBackend *Self;
                    const PhysicsShapeCastRequest *Request;
                    PhysicsRaycastResult *Result;
                    float Distance;
                } Context{this, &Request, &Result, Distance};
                Result.Candidates.reserve(16);
                auto Collect = [](b3ShapeId Shape, b3Pos Point, b3Vec3 Normal, float Fraction,
                    std::uint64_t, int, int, void *Raw) -> float {
                    auto &C = *static_cast<ContextType *>(Raw);
                    auto Owner = C.Self->FindShapeOwner(Shape);
                    if (!Owner || !C.Self->AcceptAdvancedQueryBody(*Owner, C.Request->Filter)) return 1;
                    if (!std::isfinite(Fraction) || Fraction < 0 || Fraction > 1) {
                        C.Result->Status=PhysicsOperationStatus::BackendFailure; return 0;
                    }
                    PhysicsRaycastHit Hit{*Owner, {float(Point.x),float(Point.y),float(Point.z)}, Box3DConversions::FromBox3(Normal), Fraction*C.Distance};
                    const float NormalLength=glm::length(Hit.Normal);
                    if (!IsFinite(Hit.Position) || !IsFinite(Hit.Normal) || !std::isfinite(NormalLength) || NormalLength < MinimumRaycastDistance) {
                        C.Result->Status=PhysicsOperationStatus::BackendFailure; return 0;
                    }
                    Hit.Normal/=NormalLength;
                    auto Found=std::ranges::find(C.Result->Candidates,*Owner,&PhysicsRaycastHit::Body);
                    if (Found!=C.Result->Candidates.end()) {
                        const auto Key=[](const PhysicsRaycastHit &Value) { return std::tuple(Value.Distance, Value.Position.x, Value.Position.y, Value.Position.z, Value.Normal.x, Value.Normal.y, Value.Normal.z); };
                        if (Key(Hit) < Key(*Found)) *Found=Hit;
                        return 1;
                    }
                    if (C.Result->Candidates.size()==MaximumRaycastCandidates) {C.Result->CandidatesTruncated=true;return 0;}
                    C.Result->Candidates.push_back(Hit);return 1;
                };
                auto NativeFilter=b3DefaultQueryFilter();
                NativeFilter.categoryBits=std::uint64_t{1} << Request.Filter.CollisionGroup;
                NativeFilter.maskBits=Request.Filter.CollisionMask;
                (void)b3World_CastShape(World,Proxy.Origin,&Proxy.Proxy,Box3DConversions::ToBox3(Request.Direction),NativeFilter,Collect,&Context);
                if (!Result.Succeeded() || Result.CandidatesTruncated) {
                    Result.Status=PhysicsOperationStatus::BackendFailure;Result.Message="Shape cast failed or candidate bound reached";Result.Candidates.clear();return Result;
                }
                std::ranges::sort(Result.Candidates,[](const auto &A,const auto &B){return A.Distance!=B.Distance?A.Distance<B.Distance:A.Body<B.Body;});
                return Result;
            }
            [[nodiscard]] PhysicsOverlapResult Overlap(const PhysicsOverlapRequest &Request) const override {
                PhysicsOverlapResult Result;ShapeQueryProxy Proxy;
                if (!b3World_IsValid(World)) return {.Status=PhysicsOperationStatus::BackendFailure,.Message="Physics world unavailable"};
                if (!BuildShapeQueryProxy(Request.Shape,Proxy) || Request.Filter.CollisionGroup>=16 ||
                    Request.Filter.Bodies.size()>MaximumRaycastFilterBodies || !std::ranges::is_sorted(Request.Filter.Bodies))
                    return {.Status=PhysicsOperationStatus::InvalidDescription,.Message="Invalid or oversized overlap query"};
                struct ContextType {const Box3DPhysicsBackend *Self;const PhysicsQueryFilter *Filter;PhysicsOverlapResult *Result;} Context{this,&Request.Filter,&Result};
                Result.Bodies.reserve(16);
                auto Collect=[](b3ShapeId Shape,void *Raw)->bool {
                    auto &C=*static_cast<ContextType *>(Raw);auto Owner=C.Self->FindShapeOwner(Shape);
                    if (!Owner || !C.Self->AcceptAdvancedQueryBody(*Owner,*C.Filter)) return true;
                    if (std::ranges::find(C.Result->Bodies,*Owner)!=C.Result->Bodies.end()) return true;
                    if(C.Result->Bodies.size()==MaximumRaycastCandidates){C.Result->Truncated=true;return false;}
                    C.Result->Bodies.push_back(*Owner);return true;
                };
                auto NativeFilter=b3DefaultQueryFilter();
                NativeFilter.categoryBits=std::uint64_t{1} << Request.Filter.CollisionGroup;
                NativeFilter.maskBits=Request.Filter.CollisionMask;
                (void)b3World_OverlapShape(World,Proxy.Origin,&Proxy.Proxy,NativeFilter,Collect,&Context);
                if(Result.Truncated){Result.Status=PhysicsOperationStatus::BackendFailure;Result.Message="Overlap candidate bound reached";Result.Bodies.clear();return Result;}
                std::ranges::sort(Result.Bodies);return Result;
            }
