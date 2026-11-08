"""Discriminating pristine-Jolt schedule tests. Retain every rejected candidate."""
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
Jobs=[]
Jobs.append(('SpringCalibration',['--fixture','Spring','--frames','30','--compliance','1e-5','--contact','0','--speed','0',
    '--iterations','1','--world-updates','24','--observe-updates','1','--damping','0','--particles','1']))
for Count in [1,2]:
    Common=['--count',str(Count),'--compliance','1e-5','--particles','0']
    Jobs.append((f'Original-C{Count}-S1I24',['--frames','150',*Common]))
    Jobs.append((f'Batched-C{Count}-S24I1',['--frames','600',*Common,'--steps','24','--iterations','1']))
    for Updates in [24,48,96]:
        Jobs.append((f'Interleaved-C{Count}-U{Updates}', ['--frames','600',*Common,'--world-updates',str(Updates),
            '--iterations','1','--observe-updates','1']))
for Updates in [24,48,96]:
    Jobs.append((f'FreeCompliant-U{Updates}', ['--frames','600','--compliance','1e-5','--contact','0','--speed','0',
        '--seed','1','--world-updates',str(Updates),'--iterations','1','--observe-updates','1','--particles','0']))
Receipts=[]
BinarySha=hashlib.sha256(Args.native.read_bytes()).hexdigest()
for Name,Options in Jobs:
    Out=Args.output/f'{Name}.jsonl'
    Start=time.monotonic()
    with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
        Result=subprocess.run([str(Args.native.resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=300)
    assert Result.returncode==0,Name
    QualityPath=Args.output/f'{Name}.quality.json'
    Calibration=['--spring-calibration',str(Args.output/'SpringCalibration.quality.json')] if Name!='SpringCalibration' else []
    subprocess.run([sys.executable,str(Root/'EnergyQualification.py'),'--input',str(Out),'--output',str(QualityPath),*Calibration],check=True)
    Quality=json.loads(QualityPath.read_text(encoding='utf-8'))
    Receipt={'Name':Name,'Options':Options,'ExitCode':Result.returncode,'Seconds':time.monotonic()-Start,
        'BinarySha256':BinarySha,'OutputSha256':hashlib.sha256(Out.read_bytes()).hexdigest(),'Qualification':Quality}
    Receipts.append(Receipt)
    (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
    print(json.dumps({'Name':Name,'QualityStatus':Quality['Status'],
        'MaximumAcceptedKineticEnergyJ':Quality['MaximumAcceptedKineticEnergyJ'],
        'MaximumAcceptedKPlusEdgeModelJ':Quality['MaximumAcceptedKPlusEdgeModelJ']}),flush=True)
assert BinarySha==hashlib.sha256(Args.native.read_bytes()).hexdigest()
