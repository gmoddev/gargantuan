"""Exact one-sweep energy test for two finite-compliance Jolt-style edges.

This 1D positive-orientation chain has one fixed anchor and two movable nodes.
The edge projection and velocity reconstruction are linear in this region.
It is a counterexample to universal one-sweep energy dissipation, not a cloth
trajectory model or a universal step-size criterion.
"""

import json
import numpy as np


def Analyze(ComplianceAnchor, CompliancePair, Substeps, Mass=0.1):
    Dt = 1.0 / (60.0 * Substeps)
    InverseMass = 1.0 / Mass
    AnchorFraction = InverseMass / (InverseMass + ComplianceAnchor / Dt**2)
    PairFraction = InverseMass / (2.0 * InverseMass + CompliancePair / Dt**2)
    Identity = np.eye(2)
    AnchorProjection = np.diag([1.0 - AnchorFraction, 1.0])
    PairProjection = np.array([[1.0 - PairFraction, PairFraction],
                               [PairFraction, 1.0 - PairFraction]])
    Projection = PairProjection @ AnchorProjection
    Transfer = np.block([[Projection, Projection],
                         [Projection - Identity, Projection]])
    Stiffness = np.array([
        [1.0 / ComplianceAnchor + 1.0 / CompliancePair, -1.0 / CompliancePair],
        [-1.0 / CompliancePair, 1.0 / CompliancePair]])
    Energy = np.block([[Stiffness, np.zeros((2, 2))],
                       [np.zeros((2, 2)), Mass / Dt**2 * Identity]])
    Root = np.linalg.cholesky(Energy)
    InverseRoot = np.linalg.inv(Root)
    GainMatrix = InverseRoot @ Transfer.T @ Energy @ Transfer @ InverseRoot.T
    GainEigenvalues, GainEigenvectors = np.linalg.eigh(GainMatrix)
    Witness = np.sqrt(2.0) * np.linalg.solve(Root.T, GainEigenvectors[:, -1])

    # Exact coupled implicit Euler is the positive control.
    CoupledPosition = np.linalg.inv(Identity + Dt**2 / Mass * Stiffness)
    ImplicitTransfer = np.block([[CoupledPosition, CoupledPosition],
                                 [CoupledPosition - Identity, CoupledPosition]])
    ImplicitGainMatrix = (InverseRoot @ ImplicitTransfer.T @ Energy @
                          ImplicitTransfer @ InverseRoot.T)

    def Components(State):
        Position = State[:2]
        Velocity = State[2:] / Dt
        Kinetic = 0.5 * Mass * Velocity @ Velocity
        AnchorElastic = 0.5 * Position[0]**2 / ComplianceAnchor
        PairElastic = 0.5 * (Position[1] - Position[0])**2 / CompliancePair
        return {'PositionDeviation': Position.tolist(), 'Velocity': Velocity.tolist(),
                'Kinetic': float(Kinetic), 'AnchorElastic': float(AnchorElastic),
                'PairElastic': float(PairElastic),
                'Total': float(Kinetic + AnchorElastic + PairElastic)}

    return {'Mass': Mass, 'ComplianceAnchor': ComplianceAnchor,
            'CompliancePair': CompliancePair, 'Substeps': Substeps, 'Dt': Dt,
            'AnchorFraction': AnchorFraction, 'PairFraction': PairFraction,
            'Projection': Projection.tolist(), 'Transfer': Transfer.tolist(),
            'MaximumSequentialEnergyFactor': float(GainEigenvalues[-1]),
            'SequentialSpectralRadius': float(max(abs(np.linalg.eigvals(Transfer)))),
            'MaximumCoupledImplicitEnergyFactor': float(
                np.linalg.eigvalsh(ImplicitGainMatrix)[-1]),
            'WitnessBefore': Components(Witness),
            'WitnessAfterSequential': Components(Transfer @ Witness),
            'WitnessAfterCoupledImplicit': Components(ImplicitTransfer @ Witness)}


if __name__ == '__main__':
    Cases = [(1e-5, 24), (1e-5, 48), (1e-5, 96),
             (1e-6, 24), (1e-6, 48), (1e-6, 96), (1e-6, 192)]
    print(json.dumps([Analyze(Compliance, 1e-4, Substeps)
                      for Compliance, Substeps in Cases], indent=2))
