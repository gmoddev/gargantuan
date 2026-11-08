#!/usr/bin/env python3
"""Generate a private Jolt patch for retained-lambda edge projection sweeps.

Only the exported 169-vertex single-group cloth fixture is interpreted as a
whole-graph repeated sweep. The one-pass branch is source-identical to Jolt.
This generator does not modify the pristine Jolt checkout.
"""

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
    Args = Parser.parse_args()
    Revision = subprocess.check_output(
        ["git", "-C", str(Args.Jolt), "rev-parse", "HEAD"], text=True).strip()
    if Revision != PINNED_JOLT:
        Parser.error("[Physics:ConvergedPatch] Jolt revision differs from pinned source")
    Status = subprocess.check_output(
        ["git", "-C", str(Args.Jolt), "status", "--porcelain"], text=True).strip()
    if Status:
        Parser.error("[Physics:ConvergedPatch] Pinned Jolt checkout must be clean")
    Original = (Args.Jolt / SOURCE_NAME).read_text(encoding="utf-8")
    Old = '''void SoftBodyMotionProperties::ApplyEdgeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)
{
\tJPH_PROFILE_FUNCTION();

\tfloat inv_dt_sq = 1.0f / Square(inContext.mSubStepDeltaTime);

\t// Satisfy edge constraints
\tfor (const Edge *e = mSettings->mEdgeConstraints.data() + inStartIndex, *e_end = mSettings->mEdgeConstraints.data() + inEndIndex; e < e_end; ++e)
\t{
\t\tVertex &v0 = mVertices[e->mVertex[0]];
\t\tVertex &v1 = mVertices[e->mVertex[1]];

\t\t// Get positions
\t\tVec3 x0 = v0.mPosition;
\t\tVec3 x1 = v1.mPosition;

\t\t// Calculate current length
\t\tVec3 delta = x1 - x0;
\t\tfloat length = delta.Length();

\t\t// Apply correction
\t\tfloat denom = length * (v0.mInvMass + v1.mInvMass + e->mCompliance * inv_dt_sq);
\t\tif (denom < 1.0e-12f)
\t\t\tcontinue;
\t\tVec3 correction = delta * (length - e->mRestLength) / denom;
\t\tv0.mPosition = x0 + v0.mInvMass * correction;
\t\tv1.mPosition = x1 - v1.mInvMass * correction;
\t}
}'''
    New = '''extern "C" int PhysicsCouplingDiagnosticEdgeIterations();

void SoftBodyMotionProperties::ApplyEdgeConstraints(const SoftBodyUpdateContext &inContext, uint inStartIndex, uint inEndIndex)
{
\tJPH_PROFILE_FUNCTION();

\tfloat inv_dt_sq = 1.0f / Square(inContext.mSubStepDeltaTime);

\t// Keep the original one-pass path, including arithmetic order and allocation behavior.
\tif (PhysicsCouplingDiagnosticEdgeIterations() == 1)
\t{
\t\tfor (const Edge *e = mSettings->mEdgeConstraints.data() + inStartIndex, *e_end = mSettings->mEdgeConstraints.data() + inEndIndex; e < e_end; ++e)
\t\t{
\t\t\tVertex &v0 = mVertices[e->mVertex[0]];
\t\t\tVertex &v1 = mVertices[e->mVertex[1]];
\t\t\tVec3 x0 = v0.mPosition;
\t\t\tVec3 x1 = v1.mPosition;
\t\t\tVec3 delta = x1 - x0;
\t\t\tfloat length = delta.Length();
\t\t\tfloat denom = length * (v0.mInvMass + v1.mInvMass + e->mCompliance * inv_dt_sq);
\t\t\tif (denom < 1.0e-12f)
\t\t\t\tcontinue;
\t\t\tVec3 correction = delta * (length - e->mRestLength) / denom;
\t\t\tv0.mPosition = x0 + v0.mInvMass * correction;
\t\t\tv1.mPosition = x1 - v1.mInvMass * correction;
\t\t}
\t\treturn;
\t}

\t// Private single-group cloth counterfactual: reset each edge multiplier at
\t// integration start, then retain it across ordered sweeps in this call.
\tconst int diagnostic_iterations = PhysicsCouplingDiagnosticEdgeIterations();
\tJPH_ASSERT(diagnostic_iterations >= 2 && diagnostic_iterations <= 256);
\tArray<float> diagnostic_lambda(inEndIndex - inStartIndex, 0.0f);
\tfor (int pass = 0; pass < diagnostic_iterations; ++pass)
\t{
\t\tfor (uint index = inStartIndex; index < inEndIndex; ++index)
\t\t{
\t\t\tconst Edge &e = mSettings->mEdgeConstraints[index];
\t\t\tVertex &v0 = mVertices[e.mVertex[0]];
\t\t\tVertex &v1 = mVertices[e.mVertex[1]];
\t\t\tVec3 x0 = v0.mPosition;
\t\t\tVec3 x1 = v1.mPosition;
\t\t\tVec3 delta = x1 - x0;
\t\t\tfloat length = delta.Length();
\t\t\tfloat alpha = e.mCompliance * inv_dt_sq;
\t\t\tfloat weight = v0.mInvMass + v1.mInvMass;
\t\t\tfloat denom = length * (weight + alpha);
\t\t\tif (denom < 1.0e-12f)
\t\t\t\tcontinue;
\t\t\tfloat residual = length - e.mRestLength;
\t\t\tfloat &lambda = diagnostic_lambda[index - inStartIndex];
\t\t\tVec3 correction;
\t\t\tif (pass == 0)
\t\t\t{
\t\t\t\tcorrection = delta * residual / denom;
\t\t\t\tlambda = -residual / (weight + alpha);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tfloat delta_lambda = -(residual + alpha * lambda) / (weight + alpha);
\t\t\t\tlambda += delta_lambda;
\t\t\t\tcorrection = delta * (-delta_lambda / length);
\t\t\t}
\t\t\tv0.mPosition = x0 + v0.mInvMass * correction;
\t\t\tv1.mPosition = x1 - v1.mInvMass * correction;
\t\t}
\t}
}'''
    if Original.count(Old) != 1:
        Parser.error("[Physics:ConvergedPatch] Pinned edge implementation did not match exactly")
    Modified = Original.replace(Old, New)
    Patch = "".join(difflib.unified_diff(
        Original.splitlines(True), Modified.splitlines(True),
        fromfile="a/" + SOURCE_NAME, tofile="b/" + SOURCE_NAME))
    Args.Output.parent.mkdir(parents=True, exist_ok=True)
    Args.Output.write_text(Patch, encoding="utf-8", newline="\n")
    print("[Physics:ConvergedPatch] Generated", Args.Output)


if __name__ == "__main__":
    Main()
