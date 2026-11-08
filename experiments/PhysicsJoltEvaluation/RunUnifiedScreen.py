"""Broader unified-Jolt quality screen after native S1 qualification; failures stay visible."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

Parser=argparse.ArgumentParser()
Parser.add_argument('--exe',type=Path,required=True)
Parser.add_argument('--output',type=Path,required=True)
Args=Parser.parse_args()
Args.output.mkdir(parents=True,exist_ok=False)
Receipts=[]
for Updates in [48,96]:
    for Case in ['RigidIntoCloth','Multiple','RubberPush','SphereOnCloth','Repeated']:
        Name=f'{Case}-U{Updates}'
        Options=['--backend','Jolt','--case',Case,'--frames','150','--workers','1','--collision-steps','1',
            '--world-updates',str(Updates),'--soft-iterations','1','--stretch-compliance','1e-5']
        Out=Args.output/f'{Name}.json'; Start=time.monotonic()
        with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
            P=subprocess.run([str(Args.exe.resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=300)
        assert P.returncode in [0,3],(Name,P.returncode)
        D=json.loads(Out.read_text(encoding='utf-8'))
        R={'Name':Name,'Options':Options,'ExitCode':P.returncode,'Seconds':time.monotonic()-Start,
            'QualityQualified':D.get('QualityQualified'), 'AcceptedQualityQualified':D.get('AcceptedQualityQualified'),
            'AcceptedPhysicalQualityQualified':D.get('AcceptedPhysicalQualityQualified'),
            'ReferenceModelGateQualified':D.get('ReferenceModelGateQualified'),
            'ReferenceModelMaxExcess':D.get('MaximumReferenceModelExcess'),
            'AcceptedKineticBudget':D.get('AcceptedNecessaryEnergyBudget'),
            'ContactEvidence':D.get('ContactEvidence'),'RigidResponseQualified':D.get('RigidResponseQualified'),
            'OutputSha256':hashlib.sha256(Out.read_bytes()).hexdigest()}
        Receipts.append(R)
        (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
        print(json.dumps(R),flush=True)
