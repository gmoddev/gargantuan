---
title: AudioMixService
description: Public creator reference for AudioMixService.
---

Qualified development candidate `830f5f26ea5642c9a6d7e281db8df0fb3982e5df`. Local and unreleased. Declaration coverage establishes structure. Runtime authority and behavioral qualification remain specific to each API.

Controls master and fixed category audio gain for the local runtime session.

Kind `Service`. Abstract `True`. Service policy `Default`.

Access through `game:GetService("AudioMixService")` subject to the service policy and runtime context.

Inherited members from [Instance](./instance.md).

## Properties

## Methods

### AudioMixService.GetCategoryGain

Returns category gain before mute or master gain is applied.

```text
(self: AudioMixService, Category: Enum.SoundMixCategory) -> number
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.GetCategoryMuted

Returns whether the selected local category is muted.

```text
(self: AudioMixService, Category: Enum.SoundMixCategory) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.GetMasterGain

Returns master gain before mute is applied.

```text
(self: AudioMixService) -> number
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.GetMasterMuted

Returns whether the local master mix is muted.

```text
(self: AudioMixService) -> boolean
```

```json
{
  "AsLuau": true
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.SetCategoryGain

Sets a finite local category gain from zero through one.

```text
(self: AudioMixService, Category: Enum.SoundMixCategory, Gain: number) -> ()
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.SetCategoryMuted

Mutes or unmutes the selected local category while playback continues.

```text
(self: AudioMixService, Category: Enum.SoundMixCategory, Muted: boolean) -> ()
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.SetMasterGain

Sets a finite local master gain from zero through one.

```text
(self: AudioMixService, Gain: number) -> ()
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

### AudioMixService.SetMasterMuted

Mutes or unmutes the local master mix while playback continues.

```text
(self: AudioMixService, Muted: boolean) -> ()
```

```json
{
  "AsLuau": true,
  "RequiredCapability": "MutateDataModel"
}
```

Metadata reproduces declaration expressions. Missing defaults, validation, permissions, yielding or lifecycle details require behavioral documentation. Omitted flags imply no documented guarantee.

## Signals

## Behavioral guides

Use the [creator API guides](../../api/index.mdx) for lifecycle, authority and subsystem limits.
