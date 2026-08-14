// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";

// https://astro.build/config
export default defineConfig({
	site: "https://gargantuan.teamfireworks.org",
	integrations: [
		starlight({
			title: "Gargantuan",
			social: [
				{
					icon: "github",
					label: "GitHub",
					href: "https://github.com/teamfireworks/gargantuan",
				},
			],
			sidebar: [
				{
					label: "Guides",
					items: [
						{
							label: "Roblox Deviations",
							slug: "guides/roblox-deviations",
						},
					],
				},
				{
					label: "Developing",
					items: [
						{
							label: "Contributing to Gargantuan",
							slug: "developing/contributing-to-gargantuan",
						},
						{
							label: "Working on Gargantuan",
							slug: "developing/working-on-gargantuan",
						},
						{
							label: "EditorHost and Studio Boundary",
							slug: "developing/editor-host",
						},
						{
							label: "Runtime Schema",
							slug: "developing/runtime-schema",
						},
						{
							label: "Future Architecture",
							slug: "developing/future-architecture",
						},
						{
							label: "Roadmap",
							slug: "developing/roadmap",
						},
						{
							label: "Code of Conduct",
							slug: "developing/code-of-conduct",
						},
					],
				},
				{
					label: "Meta",
					items: [
						{
							label: "License",
							slug: "meta/license",
						},
						{
							label: "Additional Copyright Notices",
							slug: "meta/additional-copyright-notices",
						},
						{
							label: "For AI Agents",
							slug: "meta/agents",
						},
					],
				},
			],
		}),
	],
});
