"""Bounded architecture-cost comparison; preserve unsupported and rejected rows."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
import RunEvaluation

Parser=argparse.ArgumentParser()
Parser.add_argument('--build',type=Path,required=True)
Parser.add_argument('--jolt',type=Path,required=True)
Parser.add_argument('--output',type=Path,required=True)
Args=Parser.parse_args(); Args.output.mkdir(parents=True,exist_ok=False)
Root=Path(__file__).resolve().parents[2]
Bin=Args.build/'experiments/PhysicsJoltEvaluation'; Suffix='.exe' if os.name=='nt' else ''
Paths={K:Bin/(N+Suffix) for K,N in {'Rigid':'PhysicsJoltUnifiedRigidEvaluation','Jolt':'PhysicsJoltNativeCouplingDiagnostic','XPBD':'PhysicsJoltXpbdCouplingDiagnostic'}.items()}
Hash=RunEvaluation.Hash
Source=RunEvaluation.Git(Root,'rev-parse','HEAD')
assert RunEvaluation.Git(Args.jolt,'rev-parse','HEAD')=='e77f175595e64cb44218cc9d9d56fc365ad0e36a'
assert not RunEvaluation.Git(Args.jolt,'status','--porcelain')
Vendor=RunEvaluation.VerifyVendors(Root,None)
Changed=RunEvaluation.Git(Root,'diff','--name-only','HEAD','--ignore-submodules=all').splitlines()
assert all(P.startswith(('src/classes/generated/','include/gargantuan/classes/generated/','src/services/generated/','include/gargantuan/services/generated/')) for P in Changed),Changed
(Args.output/'GeneratedSource.patch').write_text(RunEvaluation.SourcePatch(Root),encoding='utf-8')
Files={Root/'CMakeLists.txt',Root/'tools/classgen.luau'}
for Directory in ['src','include','cmake','vendor','assets/classes','assets/services','tools/classgen','experiments/PhysicsJoltEvaluation']:
    Files.update(P for P in (Root/Directory).rglob('*') if P.is_file() and not P.is_symlink() and not any(X in P.parts for X in ['.git','__pycache__','Xcode']))
Inputs={str(P.relative_to(Root)).replace('\\','/'):Hash(P) for P in sorted(Files)}
(Args.output/'InputSha256.json').write_text(json.dumps(Inputs,indent=2),encoding='utf-8')
BinaryHashes={K:Hash(P) for K,P in Paths.items()}
(Args.output/'Metadata.json').write_text(json.dumps({'Source':Source,'Vendor':Vendor,'Binaries':BinaryHashes,'Platform':platform.platform(),
    'CpuCount':os.cpu_count(),'InputFiles':len(Inputs),'StartedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2),encoding='utf-8')
Receipts=[]
def Run(Name,Kind,Options):
    Out=Args.output/f'{Name}.jsonl'; Start=time.monotonic()
    with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
        P=subprocess.run([str(Paths[Kind].resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=600)
    Rows=[json.loads(L) for L in Out.read_text(encoding='utf-8').splitlines()]
    assert P.returncode in ([0,1] if Kind=='Rigid' else [0]),(Name,P.returncode)
    R={'Name':Name,'Kind':Kind,'Options':Options,'ExitCode':P.returncode,'Seconds':time.monotonic()-Start,'OutputSha256':Hash(Out)}
    if Kind=='Rigid':
        R['Rows']=[X for X in Rows if 'Case' in X]
    elif Kind=='XPBD':
        Summary=next(X for X in Rows if X.get('Record')=='Summary')
        R['Summary']=Summary
        R['BoundedReferencePass']=bool(Summary['Finite'] and Summary['MaximumOmittedHardEdgeCount']==0 and
            Summary['MaximumKineticPlusCompliantEdgeEstimate']<=1.05*Summary['InitialKineticEnergy']+1e-5 and
            Summary['MaxPinDrift']<=1e-4 and Summary['MaxEdgeStretch']<=.1 and Summary['MaxParticlePenetration']<=.1)
    else:
        Frames=[X for X in Rows if X.get('Type')=='Frame']
        R['MeanStepMs']=statistics.mean(X['StepMs'] for X in Frames)
        R['FrameStateSha256']=hashlib.sha256(json.dumps([{K:V for K,V in X.items() if K!='StepMs'} for X in Frames],sort_keys=True).encode()).hexdigest()
        R['LastFrame']=Frames[-1]
        R['MaxKPlusReference']=max(X['KineticEnergy']+X['CompliantEdgeElasticEstimate'] for X in Rows if X.get('Type') in ['Microstep','Frame'])
    Receipts.append(R); (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
    print(json.dumps({'Name':Name,'ExitCode':P.returncode,'Seconds':R['Seconds']}),flush=True)
    return R
Cases=['static-world','stack','dense-contacts','mostly-static','body-churn','sleep-wake','mesh-contact','advanced-queries','mesh-query-dedup','filter-updates','moving-ccd-off','moving-ccd-on','joint-motors','spring-distance','articulated-chain','concurrent-admission']
for Backend in ['box3d','jolt']:
    for Updates,Steps in [(1,4),(48,1)]:
        for Case in Cases:
            Run(f'Rigid-{Backend}-{Case}-U{Updates}', 'Rigid',[f'--backend={Backend}',f'--case={Case}','--workers=1',f'--world-updates={Updates}',f'--collision-steps={Steps}'])
for Repeat in [1,2,3]:
    for Workers in [1,2,8]:
        for Backend in ['box3d','jolt']:
            if Backend=='box3d' and Workers!=1: continue
            for Case in ['stack','dense-contacts','mostly-static']:
                Run(f'Cost-{Backend}-{Case}-T{Workers}-R{Repeat}','Rigid',[f'--backend={Backend}',f'--case={Case}',f'--workers={Workers}','--world-updates=48','--collision-steps=1'])
        for Updates in [48,96]:
            JoltArgs=['--frames','150','--world-updates',str(Updates),'--iterations','1','--compliance','1e-5','--friction','0','--particles','0','--workers',str(Workers)]
            Run(f'Paired-Jolt-U{Updates}-T{Workers}-R{Repeat}','Jolt',JoltArgs)
            Run(f'Paired-XPBD-U{Updates}-T{Workers}-R{Repeat}','XPBD',['--frames','150','--substeps',str(Updates),'--interleaved','1','--soft-substeps','1','--iterations','1','--workers',str(Workers),'--stretch-compliance','1e-5','--bend-compliance','1e-4','--friction','0','--damping',str(2.4/(60*Updates)),'--clamp','0','--diagnostics','0','--collider-path','linear'])
assert BinaryHashes=={K:Hash(P) for K,P in Paths.items()}
assert Inputs=={N:Hash(Root/N) for N in Inputs}
(Args.output/'Completed.json').write_text(json.dumps({'Source':Source,'Processes':len(Receipts),'InputsUnchanged':True,'BinariesUnchanged':True,'PhysicalEquivalenceClaim':False,'Scope':'Explicit quality/response and cost observations; global-update adapter loops include per-update extraction and are not an optimized batched provider'},indent=2),encoding='utf-8')
