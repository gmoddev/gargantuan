#!/usr/bin/env python3
"""Generate a read-only Jolt pending-rigid-energy observer patch from pinned source."""

from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import subprocess


PINNED_JOLT = "e77f175595e64cb44218cc9d9d56fc365ad0e36a"
SOURCE_NAME = "Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp"


def Main() -> None:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("Jolt", type=Path)
    Parser.add_argument("Output", type=Path)
    Parser.add_argument("--expected-commit", default=PINNED_JOLT,
                        help="Explicit source identity for a clean maintained fork; original pin by default")
    Args = Parser.parse_args()
    Revision = subprocess.check_output(
        ["git", "-C", str(Args.Jolt), "rev-parse", "HEAD"], text=True).strip()
    if Revision != Args.expected_commit:
        Parser.error("[Physics:EnergyObserver] Jolt revision differs from pinned source")
    Status = subprocess.check_output(
        ["git", "-C", str(Args.Jolt), "status", "--porcelain"], text=True).strip()
    if Status:
        Parser.error("[Physics:EnergyObserver] Pinned Jolt checkout must be clean")
    Original = (Args.Jolt / SOURCE_NAME).read_text(encoding="utf-8")
    Declaration = ('extern "C" void PhysicsJoltEnergyState(const void *Motion, '
                   'const void *Context, double PendingRigidKineticDelta);\n\n')
    Function = 'void SoftBodyMotionProperties::ApplyCollisionConstraintsAndUpdateVelocities(const SoftBodyUpdateContext &inContext)\n{'
    if Original.count(Function) != 1:
        Parser.error("[Physics:EnergyObserver] Collision function anchor differs")
    Modified = Original.replace(Function, Declaration + Function)
    Anchor = '\n\t// Calculate the new angular velocity for all rods\n'
    Observer = '''
\t// Read-only diagnostic: rigid impulses are still cached in colliding shapes
\t// here and have not yet been written to the rigid bodies. This fixture has
\t// one dynamic sphere shape per body and unit collision inverse-mass scales.
\tconst auto DiagnosticDot = [](Vec3Arg Left, Vec3Arg Right) -> double
\t{
\t\treturn double(Left.GetX()) * double(Right.GetX())
\t\t\t+ double(Left.GetY()) * double(Right.GetY())
\t\t\t+ double(Left.GetZ()) * double(Right.GetZ());
\t};
\tdouble PendingRigidKineticDelta = 0.0;
\tfor (const CollidingShape &Shape : mCollidingShapes)
\t{
\t\tif (Shape.mMotionType != EMotionType::Dynamic || !Shape.mUpdateVelocities || Shape.mInvMass <= 0.0f)
\t\t\tcontinue;
\t\tconst double RigidMass = 1.0 / double(Shape.mInvMass);
\t\tPendingRigidKineticDelta += 0.5 * RigidMass *
\t\t\t(DiagnosticDot(Shape.mLinearVelocity, Shape.mLinearVelocity)
\t\t\t- DiagnosticDot(Shape.mOriginalLinearVelocity, Shape.mOriginalLinearVelocity));
\t\tconst Mat44 Inertia = Shape.mInvInertia.Inversed3x3();
\t\tPendingRigidKineticDelta += 0.5 *
\t\t\t(DiagnosticDot(Shape.mAngularVelocity, Inertia * Shape.mAngularVelocity)
\t\t\t- DiagnosticDot(Shape.mOriginalAngularVelocity, Inertia * Shape.mOriginalAngularVelocity));
\t}
\tPhysicsJoltEnergyState(this, &inContext, PendingRigidKineticDelta);
'''
    if Modified.count(Anchor) != 1:
        Parser.error("[Physics:EnergyObserver] Rod anchor differs")
    Modified = Modified.replace(Anchor, Observer + Anchor)
    Patch = "".join(difflib.unified_diff(
        Original.splitlines(True), Modified.splitlines(True),
        fromfile="a/" + SOURCE_NAME, tofile="b/" + SOURCE_NAME))
    Args.Output.parent.mkdir(parents=True, exist_ok=True)
    Args.Output.write_text(Patch, encoding="utf-8", newline="\n")
    print("[Physics:EnergyObserver] Generated", Args.Output)


if __name__ == "__main__":
    Main()
