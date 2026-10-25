"""Require exact endpoint-identity evidence proofs before lifecycle wake."""


CLIENT_SID = "S-1-5-21-2820064101-3801502750-265446247-1004"
WORKER_SID = "S-1-5-21-455006656-4040886684-1921607991-1006"


def RequireProof(Proof, RunId, EndpointKind, Sid):
    if (not Proof.get("Success") or Proof.get("IsAdmin") or
            Proof.get("Sid") != Sid or Proof.get("RunId") != RunId or
            Proof.get("EndpointKind") != EndpointKind):
        raise RuntimeError(EndpointKind + " evidence preflight failed before wake")


def RequireEvidenceReady(Stage, Artifact, ClientCheck, WorkerCheck):
    ClientProof = ClientCheck(Stage, Artifact)
    RequireProof(ClientProof, Stage["RunId"], "CLIENT", CLIENT_SID)
    WorkerProof = WorkerCheck(Stage, Artifact)
    RequireProof(WorkerProof, Stage["RunId"], "WORKER", WORKER_SID)
    return {"CLIENT": ClientProof, "WORKER": WorkerProof}
