"""Generate an isolated observational upstream patch from the exact pinned source."""
import argparse
import difflib
import pathlib
import subprocess
Parser=argparse.ArgumentParser()
Parser.add_argument('Jolt',type=pathlib.Path)
Parser.add_argument('Output',type=pathlib.Path)
Parser.add_argument('--s1',action='store_true',help='Add observational contact-position ledger and opt-in behavioral counterfactuals')
Arguments=Parser.parse_args()
assert subprocess.check_output(['git','-C',str(Arguments.Jolt),'rev-parse','HEAD'],text=True).strip()=='e77f175595e64cb44218cc9d9d56fc365ad0e36a'
assert not subprocess.check_output(['git','-C',str(Arguments.Jolt),'status','--porcelain'],text=True).strip()
Name='Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp'
Original=(Arguments.Jolt/Name).read_text()
Text=Original
def Replace(Old,New):
    global Text
    assert Text.count(Old)==1,(Old,Text.count(Old))
    Text=Text.replace(Old,New)
Declarations='''extern "C" void PhysicsCouplingDiagnosticStage(const char *, const void *, const void *);
extern "C" void PhysicsCouplingDiagnosticReconstruction(const void *, const void *, const void *, float);
extern "C" void PhysicsCouplingDiagnosticContact(const void *, const void *, const void *, float, float,
    const void *, const void *, const void *, const void *, const void *);

'''
Replace('JPH_NAMESPACE_BEGIN\n',Declarations+'JPH_NAMESPACE_BEGIN\n')
Replace('void SoftBodyMotionProperties::IntegratePositions(const SoftBodyUpdateContext &inContext)\n{',
        'void SoftBodyMotionProperties::IntegratePositions(const SoftBodyUpdateContext &inContext)\n{\n\tPhysicsCouplingDiagnosticStage("BeforeIntegration", this, &inContext);')
Replace('\n}\n\nvoid SoftBodyMotionProperties::ApplyDihedralBendConstraints',
        '\n\tPhysicsCouplingDiagnosticStage("AfterIntegration", this, &inContext);\n}\n\nvoid SoftBodyMotionProperties::ApplyDihedralBendConstraints')
Replace('void SoftBodyMotionProperties::ApplyCollisionConstraintsAndUpdateVelocities(const SoftBodyUpdateContext &inContext)\n{',
        'void SoftBodyMotionProperties::ApplyCollisionConstraintsAndUpdateVelocities(const SoftBodyUpdateContext &inContext)\n{\n\tPhysicsCouplingDiagnosticStage("BeforeCollision", this, &inContext);')
Replace('\n\t// Calculate the new angular velocity for all rods',
        '\n\tPhysicsCouplingDiagnosticStage("AfterCollision", this, &inContext);\n\n\t// Calculate the new angular velocity for all rods')
Replace('\t\t\tv.mVelocity = (v.mPosition - v.mPreviousPosition) / dt;',
        '\t\t\tv.mVelocity = (v.mPosition - v.mPreviousPosition) / dt;\n\t\t\tPhysicsCouplingDiagnosticReconstruction(this, &v, &prev_v, dt);')
Replace('\t\t\t\t\t\t\t// Apply impulse to particle',
        '\t\t\t\t\t\t\tconst Vec3 DiagnosticParticle = v.mVelocity;\n\t\t\t\t\t\t\tconst Vec3 DiagnosticLinear = cs.mLinearVelocity;\n\t\t\t\t\t\t\tconst Vec3 DiagnosticAngular = cs.mAngularVelocity;\n\n\t\t\t\t\t\t\t// Apply impulse to particle')
Replace('\t\t\t\t\t\t\t// Mark that the velocities of the body we hit need to be updated',
        '\t\t\t\t\t\t\tPhysicsCouplingDiagnosticContact(&v, &p, &contact_normal, projected_distance, cs.mInvMass,\n\t\t\t\t\t\t\t\t&DiagnosticParticle, &DiagnosticLinear, &DiagnosticAngular, &cs.mLinearVelocity, &cs.mAngularVelocity);\n\n\t\t\t\t\t\t\t// Mark that the velocities of the body we hit need to be updated')
if Arguments.s1:
    Replace(Declarations,Declarations+'''extern "C" int PhysicsS1DiagnosticProjectionPasses();
extern "C" bool PhysicsS1DiagnosticRefreshSpherePlane();
extern "C" void PhysicsS1DiagnosticProjection(const char *, const void *, const void *, const void *, const void *, float);

''')
    # All native update groups have finished at this point. These extra sweeps
    # deliberately change the algorithm; defaults execute zero extra sweeps.
    Replace('\tPhysicsCouplingDiagnosticStage("BeforeCollision", this, &inContext);',
        '''\tfor (int DiagnosticPass = 1; DiagnosticPass < PhysicsS1DiagnosticProjectionPasses(); ++DiagnosticPass)
        ApplyEdgeConstraints(inContext, 0, uint(mSettings->mEdgeConstraints.size()));
    PhysicsCouplingDiagnosticStage("BeforeCollision", this, &inContext);''')
    Replace('\t\t\t\t// Check if there is a collision',
        '''\t\t\t\t// Counterfactual for this sphere-only probe: refresh tangent geometry at
                // the SAME frozen rigid center. This does not advance rigid pose.
                if (PhysicsS1DiagnosticRefreshSpherePlane())
                {
                    Vec3 DiagnosticCenter = mCollidingShapes[v.mCollidingShapeIndex].mCenterOfMassTransform.GetTranslation();
                    Vec3 DiagnosticDelta = v.mPosition - DiagnosticCenter;
                    if (DiagnosticDelta.LengthSq() > 1.0e-12f)
                    {
                        Vec3 DiagnosticNormal = DiagnosticDelta.Normalized();
                        v.mCollisionPlane = Plane::sFromPointAndNormal(DiagnosticCenter + 0.45f * DiagnosticNormal, DiagnosticNormal);
                    }
                }
                // Check if there is a collision''')
    Replace('\t\t\t\t\tv.mPosition += contact_normal * projected_distance;',
        '''\t\t\t\t\tVec3 DiagnosticCenter = cs.mCenterOfMassTransform.GetTranslation();
                    PhysicsS1DiagnosticProjection("Before", this, &v, &DiagnosticCenter, &contact_normal, projected_distance);
                    v.mPosition += contact_normal * projected_distance;
                    PhysicsS1DiagnosticProjection("After", this, &v, &DiagnosticCenter, &contact_normal, projected_distance);''')
Arguments.Output.write_text(''.join(difflib.unified_diff(Original.splitlines(True),Text.splitlines(True),fromfile='a/'+Name,tofile='b/'+Name)),encoding='utf-8',newline='\n')
print('Generated S1 diagnostic patch:' if Arguments.s1 else 'Generated observational patch:',Arguments.Output)
