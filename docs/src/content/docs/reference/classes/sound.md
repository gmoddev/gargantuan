---
title: Sound
description: Public creator reference for Sound.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Plays one canonical AssetService Audio asset as a 2D or hierarchy-anchored 3D voice.

Kind `Class`. Abstract `False`. Service policy `Default`.

Abstract status describes the declaration. Constructibility can also depend on native restrictions and capabilities.

Inherited members from [Instance](./instance.md).

## Properties

### Sound.Looped

Whether playback wraps at the canonical clip boundary.

```json
{
  "Type": "t.Bool",
  "Unmodified": "false",
  "Serializable": true,
  "Replicated": true,
  "Category": "Playback"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.MixCategory

Fixed presentation category for each local player's mix. Defaults to Effects.

```json
{
  "Type": "t.EnumItem(\"SoundMixCategory\")",
  "Unmodified": "Enums::SoundMixCategory::Effects",
  "OverrideRead": "GetMixCategory",
  "OverrideWrite": "SetMixCategory",
  "Serializable": true,
  "Replicated": true,
  "Category": "Audio"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.PlaybackSpeed

Simple sample-rate multiplier that changes speed and pitch together.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Playback",
  "Range": "{ minimum = 0.25, maximum = 4 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.PlaybackState

Authoritative runtime state: Stopped, Playing, or Paused.

```json
{
  "Type": "t.EnumItem(\"SoundPlaybackState\")",
  "Unmodified": "Enums::SoundPlaybackState::Stopped",
  "Category": "Playback",
  "WriteLevel": "Never",
  "Editable": false
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.RollOffMaxDistance

Distance at or above which a positional sound is silent.

```json
{
  "Type": "t.Float",
  "Unmodified": "100.0f",
  "OverrideRead": "GetRollOffMaxDistance",
  "OverrideWrite": "SetRollOffMaxDistance",
  "Serializable": true,
  "Replicated": true,
  "Category": "Spatial",
  "Range": "{ minimum = 0.01, maximum = 100000 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.RollOffMinDistance

Distance at or below which a positional sound has full gain.

```json
{
  "Type": "t.Float",
  "Unmodified": "10.0f",
  "OverrideRead": "GetRollOffMinDistance",
  "OverrideWrite": "SetRollOffMinDistance",
  "Serializable": true,
  "Replicated": true,
  "Category": "Spatial",
  "Range": "{ minimum = 0, maximum = 100000 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.SoundId

Strict Audio asset reference; empty means no clip.

```json
{
  "Type": "t.String",
  "Unmodified": "'\"\"'",
  "OverrideRead": "GetSoundId",
  "OverrideWrite": "SetSoundId",
  "Serializable": true,
  "Replicated": true,
  "Category": "Audio",
  "EditorHint": "AssetReference:Audio"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.TimePosition

Transient playback position in seconds; writes seek the current voice.

```json
{
  "Type": "t.Float",
  "Unmodified": "0.0f",
  "OverrideRead": "GetTimePosition",
  "OverrideWrite": "SetTimePosition",
  "Category": "Playback",
  "EditorHint": "RuntimeOnly",
  "Editable": false,
  "Range": "{ minimum = 0, maximum = 30 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.Volume

Linear voice gain before positional attenuation.

```json
{
  "Type": "t.Float",
  "Unmodified": "1.0f",
  "Serializable": true,
  "Replicated": true,
  "Category": "Audio",
  "Range": "{ minimum = 0, maximum = 1 }"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Methods

### Sound.Pause

Pauses this Sound while preserving TimePosition.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.Play

Starts or replaces this Sound's one voice.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.Resume

Continues a paused Sound.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### Sound.Stop

Releases this Sound's voice and resets TimePosition to zero.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

### Sound.Ended

Fired once when non-looped playback reaches the clip boundary.

```json
{}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
