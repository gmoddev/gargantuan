"""Exact-source regression and bounded physical contact corpus on hosted runners.

A successful job means harness/regression assertions passed and all physical
verdicts were preserved. It does not promote failing original S1 geometry.
"""
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from InspectEnergyLedger import Inspect as InspectLedger
from ValidateFreeFlight import Inspect as InspectFreeFlight

Engine=Path(__file__).resolve().parents[3]
Evidence=Engine/'contact-evidence'
Evidence.mkdir(exist_ok=True)

def Run(Name,Command,Timeout=1800):
    Command=[str(Value) for Value in Command]
    with (Evidence/(Name+'.stdout')).open('wb') as Output, (Evidence/(Name+'.stderr')).open('wb') as Error:
        Result=subprocess.run(Command,cwd=Engine,stdout=Output,stderr=Error,timeout=Timeout)
    print('[Physics:HostedContact]',Name,'exit',Result.returncode,flush=True)
    if Result.returncode:
        print((Evidence/(Name+'.stdout')).read_text(errors='replace')[-4000:])
        print((Evidence/(Name+'.stderr')).read_text(errors='replace')[-4000:])
        raise RuntimeError(Name+' failed')

# The original fixed fixture is still reproduced by its original benchmark.
Run('FrozenAndOrderScreen',[sys.executable,Engine/'experiments/PhysicsJoltEvaluation/ContactQualification/RunScreen.py'],5400)
Build=Engine/'build-contact'
Run('BuildContactTargets',['cmake','--build',Build,'--parallel','2','--target',
    'PhysicsJoltContactQualification','PhysicsJoltAdmissionCacheQualification','PhysicsJoltSchedulingQualification'],1800)
Suffix='.exe' if platform.system()=='Windows' else ''
BinaryDirectory=Build/'experiments/PhysicsJoltEvaluation'
Run('RelativeResponseSelfTest',[BinaryDirectory/('PhysicsJoltContactQualification'+Suffix),'--self-test'],180)
for Target in ('PhysicsJoltAdmissionCacheQualification','PhysicsJoltSchedulingQualification'):
    Run(Target,[BinaryDirectory/(Target+Suffix)],300)
Corpus=Engine/'experiments/PhysicsJoltEvaluation/ContactQualification/RunCorpus.py'
Binary=BinaryDirectory/('PhysicsJoltContactQualification'+Suffix)
Run('FullCorpus',[sys.executable,Corpus,'--binary',Binary,'--engine',Engine,'--native',Engine/'.provider-jolt',
    '--output',Evidence/'Corpus-W1','--variants','jolt:A','jolt:IG','xpbd:A','xpbd:ES','xpbd:GS'],3600)
Small=['plane-0','plane-plus10um','plane-minus10um','face-center','edge-midpoint','corner-vertex','no-contact',
       'original128','original128-plus10um','original128-minus10um']
Run('ReplayAndWorker8',[sys.executable,Corpus,'--binary',Binary,'--engine',Engine,'--native',Engine/'.provider-jolt',
    '--output',Evidence/'Corpus-W8','--variants','jolt:A','jolt:IG','xpbd:A','xpbd:ES','xpbd:GS',
    '--workers','8','--cases',*Small],3600)
Run('ReplayWorker1',[sys.executable,Corpus,'--binary',Binary,'--engine',Engine,'--native',Engine/'.provider-jolt',
    '--output',Evidence/'Replay-W1','--variants','jolt:IG','xpbd:ES','--cases','face-center','plane-plus10um','no-contact'],600)
Run('ReplayFastFace',[sys.executable,Corpus,'--binary',Binary,'--engine',Engine,'--native',Engine/'.provider-jolt',
    '--output',Evidence/'Cull-W1','--variants','jolt:IG','--face-cull','2','--cases',
    'face-center','edge-midpoint','corner-vertex','plane-0','plane-plus10um','original128-plus10um','no-contact'],900)
for Replay in (Evidence/'Corpus-W8',Evidence/'Replay-W1',Evidence/'Cull-W1'):
    for Case in Replay.iterdir():
        if not Case.is_dir(): continue
        Original=Evidence/'Corpus-W1'/Case.name
        if (Case/'FrameHashes.json').exists():
            assert json.loads((Case/'FrameHashes.json').read_text())==json.loads((Original/'FrameHashes.json').read_text()),Case.name
Controls=[]
for PathValue in sorted((Engine/'experiments/PhysicsJoltEvaluation/ContactXpbd/Controls').glob('*.json')):
    Control=json.loads(PathValue.read_text())
    Controls.extend(dict(Control,CaseId=Control['CaseId']+'-N'+str(N),XpbdSubsteps=N) for N in (16,32,64))
Manifest=Evidence/'AccountingManifest.json'
Manifest.write_text(json.dumps(Controls,indent=2),encoding='utf-8')
Run('Accounting',[sys.executable,Corpus,'--binary',Binary,'--engine',Engine,'--native',Engine/'.provider-jolt',
    '--output',Evidence/'Accounting','--manifest',Manifest,'--variants','xpbd:A','xpbd:AH','xpbd:AS','xpbd:ES','xpbd:GS'],900)
Numerical=[]
for Case in sorted((Evidence/'Accounting').iterdir()):
    if not Case.is_dir(): continue
    Ledger=InspectLedger(Case)
    Precision=Case.name.startswith(('xpbd-AS-','xpbd-ES-','xpbd-GS-'))
    Row={'CaseId':Case.name,'Ledger':Ledger}
    if Precision:
        assert Ledger['VelocityPrecisionMode']=='2' and Ledger['DirectRawMomentumClosurePass'],Case.name
    if 'no-contact-uniform-launch' in Case.name:
        FreeFlight=InspectFreeFlight(Case)
        Row['FreeFlight']=FreeFlight
        assert FreeFlight['GatePass']==Precision,Case.name
    Numerical.append(Row)
(Evidence/'NumericalVerdicts.json').write_text(json.dumps(Numerical,indent=2),encoding='utf-8')
Expected={
    'jolt-A-original128':False,'jolt-IG-original128':False,
    'xpbd-A-original128':False,
    'jolt-IG-face-center':True,'jolt-IG-edge-midpoint':True,'jolt-IG-corner-vertex':True,
    'xpbd-ES-face-center':True,'xpbd-ES-edge-midpoint':True,'xpbd-ES-corner-vertex':True}
Summary={Row['CaseId']:Row for Row in json.loads((Evidence/'Corpus-W1/Summary.json').read_text())}
for Name,Pass in Expected.items():
    assert (Summary[Name]['Status']=='GEOMETRY_PASS')==Pass,Name
Result={'Source':subprocess.check_output(['git','rev-parse','HEAD'],cwd=Engine,text=True).strip(),
    'Native':subprocess.check_output(['git','-C',str(Engine/'.provider-jolt'),'rev-parse','HEAD'],text=True).strip(),
    'HarnessAndRegressionPass':True,'OriginalS1Qualified':False,'ExpectedGeometryVerdicts':Expected,
    'OriginalCaseStatuses':{Name:Row['Status'] for Name,Row in Summary.items() if Name.endswith('-original128')},
    'ReplayAndWorkersBitExact':True,'ConservativeFaceCullBitExact':True,
    'ShadowNumericalControlsPass':True,'Scope':'Experimental contact corrections only; physical failures remain failures, production unchanged'}
(Evidence/'HostedVerdict.json').write_text(json.dumps(Result,indent=2),encoding='utf-8')
print('[Physics:HostedContact] Regression/replay checks passed; original S1 remains rejected',flush=True)
