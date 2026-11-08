"""Independent platform/repeat/worker qualification of the measured U48 material profile."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

Parser=argparse.ArgumentParser()
Parser.add_argument('--native',type=Path,required=True)
Parser.add_argument('--output',type=Path,required=True)
Args=Parser.parse_args()
Args.output.mkdir(parents=True,exist_ok=False)
Root=Path(__file__).resolve().parent
Receipts=[]
def StateHash(PathName,Type):
    H=hashlib.sha256()
    for Line in PathName.open(encoding='utf-8'):
        R=json.loads(Line)
        if R.get('Type')==Type:
            R.pop('StepMs',None)
            H.update((json.dumps(R,sort_keys=True,separators=(',',':'))+'\n').encode())
    return H.hexdigest()
def Run(Name,Options,Expected='Pass',Timing=False):
    Out=Args.output/f'{Name}.jsonl'; Start=time.monotonic()
    with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
        P=subprocess.run([str(Args.native.resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=600)
    assert P.returncode==0,Name
    Quality=None
    if not Timing:
        QualityPath=Args.output/f'{Name}.quality.json'
        Calibration=[] if Name=='Spring' else ['--spring-calibration',str(Args.output/'Spring.quality.json')]
        subprocess.run([sys.executable,str(Root/'EnergyQualification.py'),'--input',str(Out),'--output',str(QualityPath),*Calibration],check=True)
        Quality=json.loads(QualityPath.read_text(encoding='utf-8'))
    Match=True if Timing else (Quality['CompleteBoundedPass'] if Expected=='Pass' else not Quality['CompleteBoundedPass'])
    R={'Name':Name,'Options':Options,'ExitCode':P.returncode,'Seconds':time.monotonic()-Start,'Expectation':Expected,
       'ExpectationMatched':Match,'FrameStateSha256':StateHash(Out,'Frame'),'MicrostepStateSha256':StateHash(Out,'Microstep'),
       'OutputSha256':hashlib.sha256(Out.read_bytes()).hexdigest(),'Qualification':Quality}
    Receipts.append(R); (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
    print(json.dumps({'Name':Name,'Match':Match,'Status':Quality['Status'] if Quality else 'TIMING_SEPARATE'}),flush=True)
    return R
Run('Spring',['--fixture','Spring','--frames','30','--compliance','1e-5','--contact','0','--speed','0','--iterations','1','--world-updates','24','--observe-updates','1','--damping','0','--particles','1'])
Base=['--compliance','1e-5','--particles','0','--world-updates','48','--iterations','1']
for Count in [1,2]:
    for Workers in [1,2,8]:
        Group=[]
        for Repeat in [1,2,3]:
            Group.append(Run(f'C{Count}-T{Workers}-R{Repeat}',[*Base,'--count',str(Count),'--workers',str(Workers),'--frames','600','--observe-updates','1']))
        assert len({R['FrameStateSha256'] for R in Group})==1 and len({R['MicrostepStateSha256'] for R in Group})==1,'Nondeterministic repeated candidate'
        Timing=Run(f'C{Count}-T{Workers}-Timing',[*Base,'--count',str(Count),'--workers',str(Workers),'--frames','600'],Timing=True)
        assert Timing['FrameStateSha256']==Group[0]['FrameStateSha256'],'Observer changed candidate state'
Run('C2-Extended30Seconds',[*Base,'--count','2','--frames','1800','--observe-updates','1'])
Run('Free-Extended',[*Base,'--contact','0','--speed','0','--seed','1','--frames','1800','--observe-updates','1'])
Run('OriginalS1Reject',['--count','2','--compliance','1e-5','--particles','0','--frames','150'],Expected='Reject')
Run('FreeU24Reject',['--compliance','1e-5','--particles','0','--contact','0','--speed','0','--seed','1','--iterations','1','--world-updates','24','--observe-updates','1','--frames','150'],Expected='Reject')
for Name,Extra in [('Speed10',['--speed','10']),('Frictionless',['--friction','0']),('RestitutionHalf',['--restitution','0.5'])]:
    Run(f'C2-{Name}',[*Base,'--count','2','--frames','600','--observe-updates','1',*Extra])
Result={'AllRequiredExpectationsMatched':all(R['ExpectationMatched'] for R in Receipts),'Cases':len(Receipts),
    'NativeBinarySha256':hashlib.sha256(Args.native.read_bytes()).hexdigest(),
    'Scope':'Specified positive-compliance material and qualified world-update profile, not arbitrary material or global physics certification'}
(Args.output/'Verdict.json').write_text(json.dumps(Result,indent=2),encoding='utf-8')
if not Result['AllRequiredExpectationsMatched']: raise SystemExit(3)
