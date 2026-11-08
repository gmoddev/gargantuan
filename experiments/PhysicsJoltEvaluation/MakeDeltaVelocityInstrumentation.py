"""Generate a fixture-scoped, opt-in Jolt velocity-reconstruction ablation.

Mode 0 uses pinned Jolt's position difference. Mode 1 accumulates intended edge
and volume corrections; mode 2 accumulates the corrections actually applied to
float positions. All modes retain the original position assignments and the
position-only contact correction. The caller must reject other position
constraint families before choosing mode 1 or 2.
"""

import argparse
import difflib
from pathlib import Path
import subprocess


Parser = argparse.ArgumentParser()
Parser.add_argument('Jolt', type=Path)
Parser.add_argument('Output', type=Path)
Args = Parser.parse_args()
Pinned = 'e77f175595e64cb44218cc9d9d56fc365ad0e36a'
Head = subprocess.check_output(
    ['git', '-C', str(Args.Jolt), 'rev-parse', 'HEAD'], text=True).strip()
if Head != Pinned:
    raise RuntimeError(f'[Physics:JoltDelta] Expected pinned Jolt {Pinned}, found {Head}')
if subprocess.check_output(
        ['git', '-C', str(Args.Jolt), 'status', '--porcelain'], text=True).strip():
    raise RuntimeError('[Physics:JoltDelta] Jolt source must be clean')

HeaderName = 'Jolt/Physics/SoftBody/SoftBodyVertex.h'
SourceName = 'Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp'
HeaderOriginal = (Args.Jolt / HeaderName).read_text(encoding='utf-8')
SourceOriginal = (Args.Jolt / SourceName).read_text(encoding='utf-8')


def ReplaceOnce(Source, Old, New):
    Count = Source.count(Old)
    if Count != 1:
        raise RuntimeError(f'[Physics:JoltDelta] Expected one anchor, found {Count}: {Old[:80]!r}')
    return Source.replace(Old, New)


Header = ReplaceOnce(
    HeaderOriginal,
    '\tVec3\t\t\tmVelocity;\t\t\t\t\t\t\t///< Velocity, relative to the center of mass of the soft body\n',
    '\tVec3\t\t\tmVelocity;\t\t\t\t\t\t\t///< Velocity, relative to the center of mass of the soft body\n'
    '\t// Isolated numerical ablation: initialized on every internal integration.\n'
    '\tVec3\t\t\tDiagnosticDampedVelocity = Vec3::sZero();\n'
    '\tVec3\t\t\tDiagnosticProjectionDelta = Vec3::sZero();\n')

Source = ReplaceOnce(
    SourceOriginal, 'JPH_NAMESPACE_BEGIN\n',
    'extern "C" int PhysicsJoltVelocityMode(); // 0: pinned, 1: intended, 2: applied\n\n'
    'JPH_NAMESPACE_BEGIN\n')

Source = ReplaceOnce(
    Source,
    '\t\t// Integrate\n\t\tVec3 position = v.mPosition;\n'
    '\t\tv.mPreviousPosition = position;\n'
    '\t\tv.mPosition = position + v.mVelocity * dt;',
    '\t\t// Integrate. Preserve the pinned position assignment in every mode.\n'
    '\t\tif (PhysicsJoltVelocityMode() != 0)\n'
    '\t\t{\n'
    '\t\t\tv.DiagnosticDampedVelocity = v.mVelocity;\n'
    '\t\t\tv.DiagnosticProjectionDelta = Vec3::sZero();\n'
    '\t\t}\n'
    '\t\tVec3 position = v.mPosition;\n'
    '\t\tv.mPreviousPosition = position;\n'
    '\t\tv.mPosition = position + v.mVelocity * dt;')

Source = ReplaceOnce(
    Source,
    'void SoftBodyMotionProperties::ApplyVolumeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)\n'
    '{\n\tJPH_PROFILE_FUNCTION();',
    'void SoftBodyMotionProperties::ApplyVolumeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)\n'
    '{\n\tJPH_PROFILE_FUNCTION();\n\tconst int DiagnosticMode = PhysicsJoltVelocityMode();')
Source = ReplaceOnce(
    Source,
    '\t\tv1.mPosition = x1 - minus_lambda * w1 * d1c;\n'
    '\t\tv2.mPosition = x2 - minus_lambda * w2 * d2c;\n'
    '\t\tv3.mPosition = x3 - minus_lambda * w3 * d3c;\n'
    '\t\tv4.mPosition = x4 - minus_lambda * w4 * d4c;',
    '\t\tv1.mPosition = x1 - minus_lambda * w1 * d1c;\n'
    '\t\tv2.mPosition = x2 - minus_lambda * w2 * d2c;\n'
    '\t\tv3.mPosition = x3 - minus_lambda * w3 * d3c;\n'
    '\t\tv4.mPosition = x4 - minus_lambda * w4 * d4c;\n'
    '\t\tif (DiagnosticMode == 1)\n'
    '\t\t{\n'
    '\t\t\tv1.DiagnosticProjectionDelta += -minus_lambda * w1 * d1c;\n'
    '\t\t\tv2.DiagnosticProjectionDelta += -minus_lambda * w2 * d2c;\n'
    '\t\t\tv3.DiagnosticProjectionDelta += -minus_lambda * w3 * d3c;\n'
    '\t\t\tv4.DiagnosticProjectionDelta += -minus_lambda * w4 * d4c;\n'
    '\t\t}\n'
    '\t\telse if (DiagnosticMode == 2)\n'
    '\t\t{\n'
    '\t\t\tv1.DiagnosticProjectionDelta += v1.mPosition - x1;\n'
    '\t\t\tv2.DiagnosticProjectionDelta += v2.mPosition - x2;\n'
    '\t\t\tv3.DiagnosticProjectionDelta += v3.mPosition - x3;\n'
    '\t\t\tv4.DiagnosticProjectionDelta += v4.mPosition - x4;\n'
    '\t\t}')

Source = ReplaceOnce(
    Source,
    'void SoftBodyMotionProperties::ApplyEdgeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)\n'
    '{\n\tJPH_PROFILE_FUNCTION();',
    'void SoftBodyMotionProperties::ApplyEdgeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)\n'
    '{\n\tJPH_PROFILE_FUNCTION();\n\tconst int DiagnosticMode = PhysicsJoltVelocityMode();')
Source = ReplaceOnce(
    Source,
    '\t\tv0.mPosition = x0 + v0.mInvMass * correction;\n'
    '\t\tv1.mPosition = x1 - v1.mInvMass * correction;',
    '\t\tv0.mPosition = x0 + v0.mInvMass * correction;\n'
    '\t\tv1.mPosition = x1 - v1.mInvMass * correction;\n'
    '\t\tif (DiagnosticMode == 1)\n'
    '\t\t{\n'
    '\t\t\tv0.DiagnosticProjectionDelta += v0.mInvMass * correction;\n'
    '\t\t\tv1.DiagnosticProjectionDelta -= v1.mInvMass * correction;\n'
    '\t\t}\n'
    '\t\telse if (DiagnosticMode == 2)\n'
    '\t\t{\n'
    '\t\t\tv0.DiagnosticProjectionDelta += v0.mPosition - x0;\n'
    '\t\t\tv1.DiagnosticProjectionDelta += v1.mPosition - x1;\n'
    '\t\t}')

Source = ReplaceOnce(
    Source,
    'void SoftBodyMotionProperties::ApplyCollisionConstraintsAndUpdateVelocities(const SoftBodyUpdateContext &inContext)\n'
    '{\n\tJPH_PROFILE_FUNCTION();',
    'void SoftBodyMotionProperties::ApplyCollisionConstraintsAndUpdateVelocities(const SoftBodyUpdateContext &inContext)\n'
    '{\n\tJPH_PROFILE_FUNCTION();\n\tconst int DiagnosticMode = PhysicsJoltVelocityMode();')
Source = ReplaceOnce(
    Source,
    '\t\t\tv.mVelocity = (v.mPosition - v.mPreviousPosition) / dt;',
    '\t\t\tif (DiagnosticMode == 0)\n'
    '\t\t\t\tv.mVelocity = (v.mPosition - v.mPreviousPosition) / dt;\n'
    '\t\t\telse\n'
    '\t\t\t\tv.mVelocity = v.DiagnosticDampedVelocity + v.DiagnosticProjectionDelta / dt;')

# The fixture-scoped modes do not account for the other position writers in
# this file. The diagnostic executable must reject those constraint families.
# The collision correction is after reconstruction and intentionally excluded.
Patch = ''.join(difflib.unified_diff(
    HeaderOriginal.splitlines(True), Header.splitlines(True),
    fromfile='a/' + HeaderName, tofile='b/' + HeaderName))
Patch += ''.join(difflib.unified_diff(
    SourceOriginal.splitlines(True), Source.splitlines(True),
    fromfile='a/' + SourceName, tofile='b/' + SourceName))
Args.Output.write_text(Patch, encoding='utf-8', newline='\n')
print('[Physics:JoltDelta] Generated', Args.Output)
