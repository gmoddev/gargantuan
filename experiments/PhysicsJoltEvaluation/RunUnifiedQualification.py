"""Repeat/platform/worker qualification and physically paired observation-free timing."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import time

Parser=argparse.ArgumentParser()
Parser.add_argument('--exe',type=Path,required=True)
Parser.add_argument('--output',type=Path,required=True)
Args=Parser.parse_args(); Args.output.mkdir(parents=True,exist_ok=False)
Receipts=[]
def FrameHash(D):
    Frames=[{K:V for K,V in R.items() if K not in ['StepMs','FrameWallMs','AcceptedUpdates']} for R in D['Frames']]
    return hashlib.sha256(json.dumps(Frames,sort_keys=True,separators=(',',':')).encode()).hexdigest()
def Run(Name,Options,Mode='Quality'):
    Out=Args.output/f'{Name}.json'; Start=time.monotonic()
    with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
        P=subprocess.run([str(Args.exe.resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=600)
    assert P.returncode in [0,3],(Name,P.returncode)
    D=json.loads(Out.read_text(encoding='utf-8'))
    Expected=(D.get('QualityQualified') is True and D.get('ReferenceModelGateQualified') is True) if Mode=='Quality' else D.get('QualityQualified') is False
    if Mode=='Timing': Expected=Expected and D.get('AcceptedQualityQualified') is None
    if Mode=='HardReject': Expected=Expected and D.get('ReferenceModelPreconditionsQualified') is False and D.get('NonpositiveActiveConstraints',0)>0
    if Mode=='CoverageReject': Expected=Expected and D.get('AcceptedObservationCoverage') is False
    Counts=[len(F.get('AcceptedUpdates',[])) for F in D['Frames']]
    if Mode=='Quality': Expected=Expected and all(N==48 for N in Counts) and D.get('AcceptedObservationCoverage') is True and D.get('NonpositiveActiveConstraints')==0
    R={'Name':Name,'Options':Options,'Mode':Mode,'ExitCode':P.returncode,'Seconds':time.monotonic()-Start,
       'ExpectationMatched':Expected,'FrameStateSha256':FrameHash(D), 'QualityQualified':D.get('QualityQualified'),
       'AcceptedStates':sum(Counts),'ReferenceModelGateQualified':D.get('ReferenceModelGateQualified'),
       'ReferenceModelMaximumExcess':D.get('MaximumReferenceModelExcess'),
       'MeanStepMs':statistics.mean(F['StepMs'] for F in D['Frames']),
       'MeanFrameWallMs':statistics.mean(F['FrameWallMs'] for F in D['Frames']),
       'FullRunResources':D.get('Resources',D.get('FullRunResources')),
       'OutputSha256':hashlib.sha256(Out.read_bytes()).hexdigest()}
    Receipts.append(R); (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
    print(json.dumps({'Name':Name,'ExpectationMatched':Expected,'States':sum(Counts),'MeanStepMs':R['MeanStepMs']}),flush=True)
    return R
Base=['--backend','Jolt','--frames','150','--collision-steps','1','--world-updates','48','--soft-iterations','1','--stretch-compliance','1e-5']
for Case in ['RigidIntoCloth','Multiple','RubberPush','SphereOnCloth','Repeated']:
    for Workers in [1,2,8]:
        Reference=None
        for Repeat in [1,2,3]:
            Q=Run(f'{Case}-T{Workers}-R{Repeat}',[*Base,'--case',Case,'--workers',str(Workers)])
            if Reference is None: Reference=Q['FrameStateSha256']
            assert Q['FrameStateSha256']==Reference,'Repeated physical state changed'
            T=Run(f'{Case}-T{Workers}-TimingR{Repeat}',[*Base,'--case',Case,'--workers',str(Workers),'--observe-accepted','0'],Mode='Timing')
            assert T['FrameStateSha256']==Reference,'Observation changed the physical trajectory'
for Case in ['SphereOnCloth','Repeated','RubberPush']:
    Run(f'{Case}-Extended', [*Base,'--case',Case,'--workers','1','--frames','600'])
Run('HardConstraintReferenceReject',[*Base,'--case','RigidIntoCloth','--workers','1','--stretch-compliance','0','--frames','1'],Mode='HardReject')
Run('OmittedInternalEndpointReject',[*Base,'--case','RigidIntoCloth','--workers','1','--soft-iterations','2','--frames','1'],Mode='CoverageReject')
Verdict={'AllRequiredExpectationsMatched':all(R['ExpectationMatched'] for R in Receipts),'Cases':len(Receipts),
         'BinarySha256':hashlib.sha256(Args.exe.read_bytes()).hexdigest(),'QualifiedProfile':{'WorldUpdates':48,'SoftIterations':1,'StretchCompliance':1e-5}}
(Args.output/'Verdict.json').write_text(json.dumps(Verdict,indent=2),encoding='utf-8')
if not Verdict['AllRequiredExpectationsMatched']: raise SystemExit(3)
