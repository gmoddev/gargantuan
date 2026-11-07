"""Generate an isolated observational upstream patch from the exact pinned source."""
import argparse
import difflib
import pathlib
import subprocess
Parser=argparse.ArgumentParser()
Parser.add_argument('Jolt',type=pathlib.Path)
Parser.add_argument('Output',type=pathlib.Path)
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
Arguments.Output.write_text(''.join(difflib.unified_diff(Original.splitlines(True),Text.splitlines(True),fromfile='a/'+Name,tofile='b/'+Name)),encoding='utf-8',newline='\n')
print('Generated observational patch:',Arguments.Output)
