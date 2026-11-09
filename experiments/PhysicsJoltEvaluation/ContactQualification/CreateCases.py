"""Prespecified bounded contact corpus; no candidate-dependent gate selection."""
import copy
import json
import math
from pathlib import Path


def Sphere(Center=(5.2, 3, 0), Velocity=(-1.2 / .66, 0, 0), Motion='kinematic', Radius=1, Mass=1):
    return {'Id': 'sphere-0', 'CenterM': list(Center), 'RadiusM': Radius, 'MassKg': Mass,
            'Motion': Motion, 'Trajectory': {'Kind': 'linear_clamped',
                'VelocityMPerS': list(Velocity), 'DurationSeconds': .66}}


def Cases():
    Base = {'Resolution': 17, 'Frames': 60, 'Workers': 1, 'XpbdSubsteps': 32,
            'InitialSpeedXMPerS': 2, 'Pins': 'top', 'Spheres': [Sphere()],
            'Gates': {'MaximumAxialStrain': .1, 'MaximumPinErrorM': .0001,
                      'MaximumSpherePenetrationM': .001}}
    Result = []

    def Add(Name, **Values):
        Case = copy.deepcopy(Base)
        Case.update(Values)
        Case['CaseId'] = Name
        Result.append(Case)
        return Case

    Add('plane-0')
    for Offset, Label in [(1e-5,'plus10um'), (-1e-5,'minus10um'), (.01,'plus1cm'), (-.01,'minus1cm')]:
        Add('plane-' + Label, Spheres=[Sphere((5.2,3,Offset))])
    Add('static-pose', Spheres=[Sphere(Motion='static')])
    Add('plane-no-launch', InitialSpeedXMPerS=0)
    Negative = Add('no-contact', Spheres=[])
    Negative['Gates']['MaximumAxialStrain'] = .05
    Add('oblique-plus', Spheres=[Sphere((5.2,2,.01), (-1.2/.66,1,.3))])
    Add('oblique-minus', Spheres=[Sphere((5.2,2,-.01), (-1.2/.66,1,-.3))])
    for Name, X, Y in [('face-center',1/3,7/3), ('edge-midpoint',.5,3), ('corner-vertex',0,3)]:
        Add(Name, Resolution=9, InitialSpeedXMPerS=0, GravityMPerS2=[0,0,0],
            Spheres=[Sphere((X,Y,.7), (0,0,-1.4), Radius=.2)])
    Add('pins-corners', Pins=[0,16,272,288], Spheres=[Sphere((5.2,3,.01))])
    Add('pins-left', Pins=list(range(0,289,17)), Spheres=[Sphere((5.2,3,.01))])
    Add('pins-free', Pins=[], Spheres=[Sphere((5.2,3,.01))])
    Add('radius-half', Spheres=[Sphere((4.7,3,.01),(-.7/.66,0,0),Radius=.5)])
    Add('radius-1p5', Spheres=[Sphere((5.7,3,.01),(-1.7/.66,0,0),Radius=1.5)])
    Add('mass-quarter', Material={'ParticleMassKg':.025}, Spheres=[Sphere((5.2,3,.01))])
    Add('mass-four', Material={'ParticleMassKg':.4}, Spheres=[Sphere((5.2,3,.01))])
    Fast = Sphere((5.2,3,.01), (-4.8/.66,0,0))
    Fast['Trajectory']['DurationSeconds'] = .66/4
    Add('speed-4x', Spheres=[Fast])
    Repeat = Sphere((5.2,3,.01), (-.9,0,0))
    Repeat['Trajectory'].update(Kind='sine', FrequencyHz=.75)
    Add('repeat-3', Frames=240, Spheres=[Repeat])
    First, Second = Sphere((5.2,1,.01)), Sphere((-5.2,5,-.01),(1.2/.66,0,0))
    Second['Id'] = 'sphere-1'
    Add('double-opposed', Spheres=[First, Second])
    Add('settle-600', Frames=600, Spheres=[Sphere((5.2,3,.01))])
    for Mass in (1,100):
        Add('dynamic-' + str(Mass), Spheres=[Sphere((5.2,3,.01),(-3,0,0),'dynamic',Mass=Mass)])

    # Rotate complete zero-launch inputs. These are coordinate-frame controls,
    # not a different impact direction mislabeled as a cloth rotation.
    def Rotate(Vector, Axis, Angle):
        C, S = math.cos(Angle), math.sin(Angle)
        X, Y, Z = Vector
        return [X,C*Y-S*Z,S*Y+C*Z] if Axis == 'x' else [C*X+S*Z,Y,-S*X+C*Z]

    for Name, Axis, Angle in [('rot-xz','x',math.pi/2), ('rot-yz','y',math.pi/2),
                              ('rot-oblique','x',.61)]:
        Original = Sphere((5.2,3,.01))
        Center = Rotate([5.2,0,.01], Axis, Angle)
        Center[1] += 3
        Original['CenterM'] = Center
        Original['Trajectory']['VelocityMPerS'] = Rotate([-1.2/.66,0,0], Axis, Angle)
        Quaternion = [math.cos(Angle/2), math.sin(Angle/2) if Axis=='x' else 0,
                      math.sin(Angle/2) if Axis=='y' else 0, 0]
        Add(Name, InitialSpeedXMPerS=0, RotationWxyz=Quaternion,
            GravityMPerS2=Rotate([0,-9.81,0],Axis,Angle), Spheres=[Original])
    for N in (9,32):
        Add('resolution-' + str(N), Resolution=N, Spheres=[Sphere((5.2,3,.01))])
    for Offset, Name in [(0,'original128'),(.00001,'original128-plus10um'),(-.00001,'original128-minus10um')]:
        Add(Name, Resolution=128, Frames=40, Spheres=[Sphere((5.2,3,Offset),Motion='static')])
    return Result


if __name__ == '__main__':
    import argparse
    Parser = argparse.ArgumentParser()
    Parser.add_argument('Output', type=Path)
    Args = Parser.parse_args()
    Args.Output.mkdir(parents=True, exist_ok=True)
    Corpus = Cases()
    (Args.Output / 'Cases.json').write_text(json.dumps(Corpus, indent=2), encoding='utf-8')
    print('[Physics:ContactCases]', len(Corpus), 'prespecified definitions')
