"""Both endpoint proofs are mandatory before the lifecycle may wake agents."""

from pathlib import Path
import sys
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import evidence_gate as Gate


class DualEvidenceGateTests(unittest.TestCase):
    def Proof(self, Role):
        return {"Success": True, "IsAdmin": False,
                "RunId": "fresh-run", "EndpointKind": Role,
                "Sid": (Gate.CLIENT_SID if Role == "CLIENT" else
                        Gate.WORKER_SID)}

    def test_both_checked_before_wake(self):
        Order = []

        def Client(*_):
            Order.append("client")
            return self.Proof("CLIENT")

        def Worker(*_):
            Order.append("worker")
            return self.Proof("WORKER")

        Gate.RequireEvidenceReady({"RunId": "fresh-run"}, None, Client, Worker)
        Order.append("wake")
        self.assertEqual(Order, ["client", "worker", "wake"])

    def test_client_pass_worker_fail_prevents_wake(self):
        Order = []

        def Worker(*_):
            Order.append("worker")
            return {**self.Proof("WORKER"), "Success": False}

        with self.assertRaisesRegex(RuntimeError, "WORKER"):
            Gate.RequireEvidenceReady(
                {"RunId": "fresh-run"}, None,
                lambda *_: self.Proof("CLIENT"), Worker)
        self.assertEqual(Order, ["worker"])

    def test_worker_pass_client_fail_prevents_wake(self):
        Order = []

        def Worker(*_):
            Order.append("worker")
            return self.Proof("WORKER")

        with self.assertRaisesRegex(RuntimeError, "CLIENT"):
            Gate.RequireEvidenceReady(
                {"RunId": "fresh-run"}, None,
                lambda *_: {**self.Proof("CLIENT"), "Success": False}, Worker)
        self.assertEqual(Order, [])

    def test_admin_worker_proof_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "WORKER"):
            Gate.RequireProof({**self.Proof("WORKER"), "IsAdmin": True},
                              "fresh-run", "WORKER", Gate.WORKER_SID)


if __name__ == "__main__":
    unittest.main()
