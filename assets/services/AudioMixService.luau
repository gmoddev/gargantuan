const c = require("@tools/classes").unpack()

return c({
	name = "AudioMixService",
	abstract = true,
	description = "Controls master and fixed category audio gain for the local runtime session.",
	since = "0.4.0",
	methods = {
		GetMasterGain = {
			description = "Returns master gain before mute is applied.",
			asLuau = true,
			luauSignature = "(self: AudioMixService) -> number",
		},
		SetMasterGain = {
			description = "Sets a finite local master gain from zero through one.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Gain: number) -> ()",
			requiredCapability = "MutateDataModel",
		},
		GetMasterMuted = {
			description = "Returns whether the local master mix is muted.",
			asLuau = true,
			luauSignature = "(self: AudioMixService) -> boolean",
		},
		SetMasterMuted = {
			description = "Mutes or unmutes the local master mix while playback continues.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Muted: boolean) -> ()",
			requiredCapability = "MutateDataModel",
		},
		GetCategoryGain = {
			description = "Returns category gain before mute or master gain is applied.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Category: Enum.SoundMixCategory) -> number",
		},
		SetCategoryGain = {
			description = "Sets a finite local category gain from zero through one.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Category: Enum.SoundMixCategory, Gain: number) -> ()",
			requiredCapability = "MutateDataModel",
		},
		GetCategoryMuted = {
			description = "Returns whether the selected local category is muted.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Category: Enum.SoundMixCategory) -> boolean",
		},
		SetCategoryMuted = {
			description = "Mutes or unmutes the selected local category while playback continues.",
			asLuau = true,
			luauSignature = "(self: AudioMixService, Category: Enum.SoundMixCategory, Muted: boolean) -> ()",
			requiredCapability = "MutateDataModel",
		},
	},
})
