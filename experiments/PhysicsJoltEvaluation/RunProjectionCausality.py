"""Small nonlinear geometry controls and per-edge kinetic-work decomposition."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

Parser=argparse.ArgumentParser()
Parser.add_argument('--replay',type=Path,required=True)
Parser.add_argument('--output',type=Path,required=True)
Args=Parser.parse_args()
Args.output.mkdir(parents=True,exist_ok=False)
Fixtures=Path(__file__).resolve().parent/'CausalFixtures'
Original=json.loads((Fixtures/'NativeOptimizedClothGraph.json').read_text(encoding='utf-8'))
def Small(Positions,Edges):
    return {'Positions':Positions,'InverseMasses':[0]*(len(Positions)-1)+[10],
        'Edges':[{'I':I,'J':J,'RestLength':1,'Compliance':0} for I,J in Edges]}
Graphs={
    'OnePin':(Small([[-1,0,0],[0,0,0]],[(0,1)]),1),
    'TangentPins':(Small([[-1,0,0],[1,0,0],[0,0,0]],[(0,2),(1,2)]),2),
    'OrthogonalPins':(Small([[-1,0,0],[0,-1,0],[0,0,0]],[(0,2),(1,2)]),2),
}
for Name,Predicate in [
    ('Cloth',lambda E:True),
    ('SingleDiagonal',lambda E:abs(E['I']-E['J'])!=12),
    ('NoDiagonals',lambda E:abs(E['I']-E['J']) not in (12,14)),
    ('NoBend',lambda E:E['Compliance']==0),
    ('NoDiagonalsNoBend',lambda E:E['Compliance']==0 and abs(E['I']-E['J']) not in (12,14)),
]:
    Graphs[Name]=({**Original,'Edges':[E for E in Original['Edges'] if Predicate(E)]},84)
Receipts=[]
def Run(Name,Graph,Seed,Precision,Rest,Steps,Trace=False):
    GraphPath=Args.output/f'{Name}.graph.json'
    GraphPath.write_text(json.dumps(Graph,separators=(',',':')),encoding='utf-8')
    Options=['--graph',str(GraphPath),'--precision',Precision,'--rest',Rest,'--steps',str(Steps),
             '--frames','150','--seed-index',str(Seed),'--seed-z','1','--particles','1' if Trace else '0']
    if Trace: Options+=['--edge-trace',str(Args.output/f'{Name}.edges.jsonl'),'--edge-start','52','--edge-end','64']
    Out=Args.output/f'{Name}.jsonl'
    with Out.open('w',encoding='utf-8') as Stdout,(Args.output/f'{Name}.stderr.txt').open('w',encoding='utf-8') as Stderr:
        Result=subprocess.run([str(Args.replay.resolve()),*Options],stdout=Stdout,stderr=Stderr,timeout=120)
    Rows=[json.loads(L) for L in Out.read_text(encoding='utf-8').splitlines()]
    Frames=[R for R in Rows if R['Type']=='Frame']
    Receipts.append({'Name':Name,'Options':Options,'ExitCode':Result.returncode,'Frames':len(Frames),
        'MaximumKE':max((R['KineticEnergy'] for R in Frames),default=None),
        'OutputSha256':hashlib.sha256(Out.read_bytes()).hexdigest()})
    (Args.output/'Receipts.json').write_text(json.dumps(Receipts,indent=2),encoding='utf-8')
    assert Result.returncode==0 and len(Frames)==150,Name
for Name,(Graph,Seed) in Graphs.items():
    if Name in ['OnePin','TangentPins','OrthogonalPins']:
        for Precision in ['float','double']:
            for Steps in [1,4]: Run(f'{Name}-{Precision}-S{Steps}',Graph,Seed,Precision,'recomputed',Steps)
    else: Run(f'{Name}-double-S4',Graph,Seed,'double','recomputed',4)
Run('Cloth-float-captured-S4',Original,84,'float','captured',4,True)
