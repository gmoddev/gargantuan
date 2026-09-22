"""Analyze RendererDepthTests raw PPMs (NumPy/Pillow, no Studio dependency).

The independent double-precision ray/box oracle deliberately ignores one-pixel
silhouette edges. Depth predictions are ideal quantization models, not readback
of the GPU attachment; raster interpolation can disagree at bin boundaries.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def Analyze(PathValue, Near, Far, Convention):
    Variant, Distance, Angle = map(int, PathValue.stem.split('-'))
    Scale = .1 if Variant == 2 else 1
    Target = np.array([-10.5, 15, -28.75]) * Scale
    Radians = np.radians(Angle)
    Eye = Target + np.array([np.cos(Radians), .55, np.sin(Radians)]) * Distance * Scale
    Look = Target - Eye
    Look /= np.linalg.norm(Look)
    Right = np.cross(Look, [0, 1, 0])
    Right /= np.linalg.norm(Right)
    Up = np.cross(Right, Look)
    Pixels = np.array(Image.open(PathValue))
    Height, Width = Pixels.shape[:2]
    Y, X = np.mgrid[:Height, :Width]
    Rays = (Look + Right * (((X + .5) / Width * 2 - 1) * Width / Height * np.tan(np.pi / 6))[..., None]
            + Up * ((1 - (Y + .5) / Height * 2) * np.tan(np.pi / 6))[..., None])
    Depths = []
    for Center, Size in [([-50.125, 0, 74.25], [205.75, 1, 341]), ([-10.5, 24, -28.75], [30.5, 59, 135])]:
        Center, Half = np.array(Center) * Scale, np.array(Size) * Scale / 2
        with np.errstate(divide='ignore', invalid='ignore'):
            A, B = (Center - Half - Eye) / Rays, (Center + Half - Eye) / Rays
        Entry, Exit = np.minimum(A, B).max(2), np.maximum(A, B).min(2)
        Depths.append(np.where((Entry <= Exit) & (Exit > 0), np.maximum(Entry, 0), np.inf))
    Floor, Block = Depths
    Interior = (Block < Floor) & (Block > Near) & (Block < Far)
    Expected = Interior.copy()
    for DY, DX in [(-1, 0), (1, 0), (0, -1), (0, 1)]:
        Interior &= np.roll(Expected, (DY, DX), (0, 1))
    Interior[[0, -1], :] = False
    Interior[:, [0, -1]] = False
    Actual = (Pixels[:, :, 0] == Pixels[:, :, 1]) & (Pixels[:, :, 1] == Pixels[:, :, 2])
    Missing = Interior & ~Actual
    Factor = 2 if Convention == 'no' else 1
    def Depth(Z):
        return (Far + (Factor - 1) * Near) / (Far - Near) - Factor * Far * Near / (Far - Near) / Z
    BlockDepth, FloorDepth = Depth(Block), Depth(Floor)
    Block16 = np.rint(np.clip(BlockDepth, 0, 1) * 65535)
    Floor16 = np.rint(np.clip(FloorDepth, 0, 1) * 65535)
    Predicted = Interior & (Block16 >= Floor16)
    Samples = []
    Candidates = np.argwhere(Missing & np.isfinite(Floor))
    for Index in np.linspace(0, len(Candidates) - 1, min(5, len(Candidates)), dtype=int):
        Row, Column = Candidates[Index]
        Samples.append({'Pixel': [int(Column), int(Row)], 'BlockViewZ': float(Block[Row, Column]),
                        'FloorViewZ': float(Floor[Row, Column]), 'BlockDepth': float(BlockDepth[Row, Column]),
                        'FloorDepth': float(FloorDepth[Row, Column]), 'BlockD16': int(Block16[Row, Column]),
                        'FloorD16': int(Floor16[Row, Column]),
                        'SeparationD16Steps': float((FloorDepth[Row, Column] - BlockDepth[Row, Column]) * 65535)})
    return {'Frame': PathValue.name, 'RawSha256': hashlib.sha256(Pixels.tobytes()).hexdigest(),
            'Expected': int(Interior.sum()), 'Missing': int(Missing.sum()),
            'D16PredictedMissing': int(Predicted.sum()), 'D16Disagreement': int((Interior & (Predicted != Missing)).sum()),
            'VisibleBlockDepthRange': [float(Block[Interior].min()), float(Block[Interior].max())], 'Samples': Samples}


if __name__ == '__main__':
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument('directory', type=Path)
    Parser.add_argument('--near', type=float, default=.1)
    Parser.add_argument('--far', type=float, default=100000)
    Parser.add_argument('--projection', choices=['no', 'zo'], default='no')
    Args = Parser.parse_args()
    Records = [Analyze(PathValue, Args.near, Args.far, Args.projection) for PathValue in sorted(Args.directory.glob('[012]-*.ppm'))]
    if len(Records) != 36:
        raise RuntimeError('Expected the complete 36-frame KI-008 matrix')
    (Args.directory / 'analysis.json').write_text(json.dumps({'Near': Args.near, 'Far': Args.far,
        'Projection': Args.projection, 'Records': Records}, indent=2) + '\n')
    print('[Render:DepthAnalysis]', len(Records), 'frames;', sum(Record['Missing'] for Record in Records), 'missing pixels')
