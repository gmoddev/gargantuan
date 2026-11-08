"""Contact-position causal controls and precision replay; no qualification verdicts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

Parser = argparse.ArgumentParser()
Parser.add_argument('--instrumented', type=Path, required=True)
Parser.add_argument('--replay', type=Path, required=True)
Parser.add_argument('--baseline', type=Path, required=True)
Parser.add_argument('--output', type=Path, required=True)
Args = Parser.parse_args()
Args.output.mkdir(parents=True, exist_ok=False)
Receipts = []
def Hash(P): return hashlib.sha256(P.read_bytes()).hexdigest()
def Run(Name, Program, Options):
    Out = Args.output / f'{Name}.jsonl'
    Err = Args.output / f'{Name}.stderr.txt'
    Start = time.monotonic()
    with Out.open('w', encoding='utf-8') as Stdout, Err.open('w', encoding='utf-8') as Stderr:
        Result = subprocess.run([str(Program.resolve()), *Options], stdout=Stdout, stderr=Stderr, timeout=180)
    Rows = [json.loads(Line) for Line in Out.read_text(encoding='utf-8').splitlines()]
    Frames = [R for R in Rows if R.get('Type')=='Frame']
    Receipts.append({'Name': Name, 'Options': Options, 'BinarySha256': Hash(Program), 'OutputSha256': Hash(Out),
        'ExitCode': Result.returncode, 'Seconds': time.monotonic()-Start, 'Frames': len(Frames),
        'MaximumKE': max((R['KineticEnergy'] for R in Frames), default=None),
        'MaximumKEPlusEstimate': max((R['KineticEnergy']+R.get('CompliantEdgeElasticEstimate',0) for R in Frames), default=None)})
    (Args.output/'Receipts.json').write_text(json.dumps(Receipts, indent=2), encoding='utf-8')
    assert Result.returncode==0 and len(Frames)==150, Name
    return Frames
Parity = []
for Count in [1, 2]:
    Common = ['--frames','150','--count',str(Count),'--compliance','1e-5','--particles','0']
    for Scale in [1, 0, 0.5, 0.75]:
        Name=f'Projection-C{Count}-Scale{Scale}'
        Frames=Run(Name,Args.instrumented,Common+['--contact-projection-scale',str(Scale),'--trace',str(Args.output/f'{Name}.phases.jsonl'),'--trace-start','17','--trace-end','20'])
        if Scale==1:
            Old=[json.loads(L) for L in (Args.baseline/f'Material-C{Count}-S1-Clean.jsonl').read_text(encoding='utf-8').splitlines()]
            Old=[{K:V for K,V in R.items() if K!='StepMs'} for R in Old if R['Type']=='Frame']
            New=[{K:V for K,V in R.items() if K!='StepMs'} for R in Frames]
            Parity.append({'Count': Count, 'ExactFrameStateParity': Old==New})
            assert Old==New, f'Default changed C{Count}'
    for Through in [17, 18]:
        Name=f'ContactThrough-C{Count}-F{Through}'
        Frames=Run(Name,Args.instrumented,Common+['--contact-through',str(Through)])
        assert all(R['Contacts']==0 for R in Frames if R['Frame']>Through), 'Contact-disable control ineffective'
    for Steps,Iterations in [(2,12),(1,48),(2,24),(2,48),(4,24)]:
        Name=f'Cadence-C{Count}-S{Steps}-I{Iterations}'
        Trace=['--trace',str(Args.output/f'{Name}.phases.jsonl'),'--trace-start','17','--trace-end','20'] if Count==2 else []
        Run(Name,Args.instrumented,Common+['--steps',str(Steps),'--iterations',str(Iterations)]+Trace)
for Scale in [0, 0.5]:
    Name=f'OneEvent-C2-Scale{Scale}'
    Run(Name,Args.instrumented,['--frames','150','--count','2','--compliance','1e-5','--particles','0',
        '--contact-projection-scale',str(Scale),'--projection-frame','18','--projection-iteration','1','--projection-vertex','100',
        '--trace',str(Args.output/f'{Name}.phases.jsonl'),'--trace-start','17','--trace-end','20'])
for Steps in [1, 4]:
    Run(f'CompliantFree-S{Steps}',Args.instrumented,['--frames','150','--steps',str(Steps),'--contact','0','--speed','0','--seed','1','--compliance','1e-5','--particles','0'])
    for Precision in ['float', 'double', 'longdouble']:
        for Rest in ['captured', 'recomputed']:
            Run(f'Replay-{Precision}-{Rest}-S{Steps}', Args.replay,
                ['--graph',str(Args.baseline/'OptimizedClothGraph.json'),'--precision',Precision,'--rest',Rest,'--steps',str(Steps),'--frames','150','--seed-z','1','--particles','1' if Precision=='float' and Rest=='captured' else '0'])
(Args.output/'DefaultParity.json').write_text(json.dumps(Parity,indent=2),encoding='utf-8')
