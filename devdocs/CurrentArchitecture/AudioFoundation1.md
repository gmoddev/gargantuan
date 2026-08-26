---
status: current
date: 2026-08-26
owner: runtime-audio
related_code:
  - assets/classes/Sound.luau
  - include/gargantuan/audio/AudioBackend.hpp
  - include/gargantuan/audio/AudioRuntime.hpp
  - include/gargantuan/classes/Sound.hpp
  - src/assets/AssetImporter.cpp
  - src/audio/AudioRuntime.cpp
  - src/audio/SdlAudioBackend.cpp
  - tests/AudioFoundationTests.cpp
  - tests/AudioFoundationBenchmark.cpp
---

# Audio Foundation 1

## Public Sound API

`Sound` is a constructible schema-backed `Instance`; audio files remain assets
owned by the one public `AssetService`. There is no public `AudioService`,
`AudioAssetService`, `SoundAssetService`, or `MusicService`.

The authored properties are:

| Property | Foundation 1 contract |
| --- | --- |
| `SoundId : string` | Empty or strict `asset://`/`builtin://` reference with the `AssetReference:Audio` editor hint. Playback accepts only a resolved Audio-kind asset. |
| `Volume : number` | Finite linear gain in `[0, 1]`; default `1`. |
| `PlaybackSpeed : number` | Finite simple-rate multiplier in `[0.25, 4]`; default `1`. Speed and pitch change together. |
| `Looped : boolean` | Wraps at the canonical frame boundary without queue growth. |
| `RollOffMinDistance : number` | Finite nonnegative full-gain distance; default `10`. It must remain below max. |
| `RollOffMaxDistance : number` | Finite silent distance in `(min, 100000]`; default `100`. |

`TimePosition` is a transient, noneditable runtime number in `[0, 30]`. A
script may read it or set it to seek; it is deliberately neither serialized nor
replicated. `PlaybackState` is the read-only `SoundPlaybackState` enum with
exact values `Stopped`, `Playing`, and `Paused`. It is the semantic authority;
an SDL stream or device state never becomes gameplay state.

`Play()` admits one voice starting at the current `TimePosition`. Calling it
again on the same Sound replaces/restarts that voice from zero. `Pause()` keeps
the position, `Resume()` continues a paused voice, and `Stop()` releases it and
resets the position to zero. Changing `SoundId` while active restarts from zero;
changing `TimePosition` seeks on the next runtime reconciliation. Natural
non-looped completion stops, resets the position, and fires `Ended` exactly
once. Stop, destruction, device failure, missing assets, and voice rejection do
not masquerade as natural completion.

## AssetService Audio kind and importer

`AssetKind::Audio` has stable wire value `4`, after Image `0`, Mesh `1`, Font
`2`, and Material `3`. The existing importer registry owns the private WAV
importer and the existing catalog, reimport, integrity, persistence, reference
safety, metadata, and package paths apply unchanged. `ResolveAudio` returns an
engine-owned immutable resource, never a decoder or backend handle.

Foundation 1 accepts RIFF/WAVE with one PCM `fmt ` chunk and one `data` chunk:

- little-endian integer PCM format 1;
- 16-bit samples;
- mono or stereo;
- sample rates from 8 kHz through 48 kHz;
- nonempty, frame-aligned sample data.

WAV was selected because it needs no new media framework or runtime decoder and
keeps packaging and security behavior explicit. OGG Vorbis, MP3, and streaming
decode are deferred.

## Canonical artifact and bounds

Audio requires canonical artifact version 2. Its self-contained payload is the
artifact magic/version/kind followed by sample rate, channel count, frame count,
and little-endian interleaved PCM16. Source WAV files are not needed at runtime.
Content hashes cover metadata and PCM bytes.

Foundation 1 intentionally chooses bounded resident decode, not an unbounded
all-audio policy. One clip is limited to 30 seconds, 1,440,000 frames, two
channels, and 5,760,000 decoded PCM bytes. The common 8 MiB source bound still
applies, and the WAV chunk table is limited to 128 entries. The importer checks
RIFF length, chunk padding, duplicate required chunks, format/byte/block rates,
sample-rate/channel/bit-depth support, frame multiplication, decoded size, and
duration before allocating the PCM vector. Empty/truncated/trailing/impossible
layouts fail before publication. External URLs and nested formats do not exist.

Short SFX and bounded music cues can therefore be resident. Long music and
incremental decode are explicit Foundation 2 work rather than silently decoding
multi-hour input into memory.

## Backend boundary and runtime ownership

The private ownership chain is:

```text
Sound authored/runtime semantics
    -> AudioRuntime (main runtime thread, voices/listener/mixer/residency)
    -> IAudioBackend (stereo float frames and queue metrics only)
    -> SdlAudioBackend (SDL device stream)
```

Public schema and `Sound` headers contain no SDL types, device IDs, channels,
streams, or backend pointers. `IAudioBackend` deals only in availability,
sample rate, queued frames, interleaved float submission, clear/shutdown,
diagnostics, and bounded metrics. Tests inject a deterministic fake backend.

`AudioRuntime` owns weak Sound identities, at most one voice per Sound, immutable
canonical resources, the fixed 256-frame stereo mix block, a pre-reserved
completion list, and backend lifetime. Every Engine binds Sound descendants to
that runtime. The semantic/runtime thread resolves assets, admits voices,
updates positions, mixes, and marshals `Ended` by directly finishing the Sound
after a block. Destroyed Sounds and destroyed spatial ancestors are observed
before use.

The SDL implementation uses SDL 3.4's push-stream API. Gargantuan registers no
audio callback: the runtime thread mixes bounded blocks and
`SDL_PutAudioStreamData` copies them into SDL's stream; SDL's device thread only
drains SDL-owned data. Consequently no audio callback can call Luau, inspect the
DataModel, read files, decode, acquire engine mutexes, allocate engine objects,
or log. Shutdown first stops semantic voices, clears the stream, destroys it,
and then releases the SDL audio subsystem reference.

## Listener, positioning, attenuation, and pan

The one gameplay listener is the current runtime `Workspace.CurrentCamera`
transform. Standalone and Studio Play use the runtime camera. Edit-mode/editor
camera preview is not implemented and cannot leak into Play.

A Sound is positional only when walking its parent chain finds an `Attachment`
chain anchored by a `BasePart`. Attachment CFrames are accumulated into the
first BasePart CFrame. A Sound directly under a BasePart is at that part's world
position. Sounds under folders, GUI, the DataModel, or any other non-spatial
chain are 2D and retain source stereo. There is no redundant `Sound.Position`.

Positional stereo clips are downmixed to mono before panning. Distance gain is
the deterministic linear curve:

```text
distance <= min: 1
distance >= max: 0
otherwise:       1 - (distance - min) / (max - min)
```

The normalized listener-space right component produces `[-1, 1]` pan. Equal-
power cosine/sine gains map that pan to stereo, then distance and Volume are
applied. This is ordinary desktop stereo positioning, not HRTF, binaural audio,
occlusion, or environmental acoustics.

## Mixing, looping, speed, and voice limits

The mixer output is 48 kHz, stereo float. Mono duplicates into stereo for 2D;
stereo 2D preserves channels. Linear interpolation resamples canonical PCM at
`sourceRate / outputRate * PlaybackSpeed`, so playback speed changes pitch.
Final accumulation clamps to `[-1, 1]`.

Looping wraps the fractional frame position with `fmod` at the clip frame count.
The backend target queue is 1,024 frames; one Engine step may submit at most
four fixed 256-frame blocks, so neither loops nor slow runtime frames create an
unbounded command or buffer queue.

There are exactly 256 simultaneous admitted voices per AudioRuntime. Admission
is deterministic by Sound `ObjectId` reconciliation order. At the limit the
newest request is rejected, its Sound returns to Stopped/position zero, and one
bounded diagnostic is emitted per Sound/code. Foundation 1 has no speculative
priority property or stealing heuristic.

## Device failure and headless behavior

`EngineProviderConfiguration.AudioEnabled` defaults false. Headless players,
CTest, servers, and direct PlaySession tests do not initialize SDL audio; Sound
Instances still deserialize and their methods remain safe. A requested play on
an unavailable provider returns the Sound to Stopped with a bounded diagnostic.

Graphical standalone/player hosts enable the SDL backend. The production
EditorHost enables it only for its isolated runtime Play clone; authoring/edit
mode never opens an audio device. SDL initialization failure, no default device,
queue-query failure, and submission/device loss all fail open: gameplay keeps
running, voices are stopped, and repeat diagnostics are bounded. The runtime
never throws through the game loop for device availability.

## Persistence, packaging, and Studio

Authored Sound properties serialize through schema version 1; runtime state and
`TimePosition` do not. Studio Play receives the exact authoring snapshot and
asset snapshot, but creates independent Sound objects and voices. Stop destroys
the AudioRuntime before the runtime world, so no voice crosses sessions and all
authoring Sounds/properties remain unchanged.

Audio artifacts use the generic AssetService capture/write/load pipeline and
the generic PackageBuilder asset manifest. Packages contain `.gasset` PCM data,
not source WAV paths. The existing SDL runtime distribution already contains
the selected backend. FirstCompleteGame's package smoke now carries and validates
its Audio catalog/artifact after relocation.

Studio needs no audio panel. The generic schema Insert Object path discovers
Sound, the inspector edits its six authored fields, and `AssetReference:Audio`
filters the common picker to Audio. The common import dialog admits `.wav`; the
catalog, reimport, delete, save/reopen, Play, and diagnostics stay generic.

## Performance and validation

`gargantuan_audio_foundation_benchmark` measures WAV decode, AssetService
resolve, Play-to-first-software-submission latency, wall/mixer CPU, ordinary
heap allocations, admitted/rejected voices, submitted frames, and queue-empty
observations. Cases cover 1, 32, 128, and 257 requests, 32 moving positional
voices, short looping SFX, and the maximum resident clip. Queue-empty metrics
are software-side observations, not fabricated hardware/audible latency.

The 2026-08-26 Windows Release baseline decoded the maximum clip in 46.072 ms
and resolved it 100,000 times in 17.088 ms. Across 500 mixer steps, measured
mixer CPU was 3.564 ms for one voice, 93.046 ms for 32, 372.267 ms for 128,
and 741.908 ms for the capped 256 admissions from 257 requests. The 32-voice
moving-positional and looping-SFX cases measured 98.432 ms and 91.908 ms.
Play-to-first-software-submission ranged from 15 us for one voice to 1.862 ms
at limit stress. These are reproducible software observations on the test host,
not guarantees about device or audible latency.

The functional gate covers valid/automatic WAV import, malformed and oversize
rejection, canonical save/reopen, property validation, 2D stereo, volume,
speed, play/pause/resume/stop/seek, looping, `Ended`, attenuation/pan, moving
Part and Attachment anchors, missing/reimported/deleted assets, Sound/parent
destruction, 256-voice admission, device unavailable/loss, repeated lifecycle,
and shutdown. FirstCompleteGame adds three positional pickup cues and one 2D
round-completion cue without making gameplay depend on hearing them.

## Explicit deferrals

Foundation 1 does not implement OGG/MP3, streaming or long-form music,
pitch-preserving time stretch, overlapping voices from one Sound, priorities or
voice stealing, public buses/groups/master volume, advanced HRTF, Doppler,
occlusion, reverb, environmental acoustics, DSP graphs, microphone input, voice
chat, capture, procedural synthesis, HTTP/radio, editor preview, or a music
timeline. Foundation 2 should prioritize bounded streaming decode and buffering,
then buses/master volume and evidence-based priority/stealing, followed by
device hotplug/recovery and higher-quality optional spatialization.
