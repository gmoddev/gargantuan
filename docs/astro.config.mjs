// @ts-check
import { defineConfig } from "astro/config";
import starlight from "@astrojs/starlight";
import { unified } from "@astrojs/markdown-remark";
import RepositoryLinks from "./plugins/repository-links.mjs";

export default defineConfig({
  site: "https://gmoddev.github.io",
  base: "/gargantuan",
  markdown: { processor: unified({ remarkPlugins: [RepositoryLinks] }) },
  integrations: [
    starlight({
      title: "Gargantuan",
      social: [{ icon: "github", label: "GitHub", href: "https://github.com/gmoddev/gargantuan" }],
      sidebar: [
        { label: "Getting started", items: [
          { label: "Capabilities and platforms", slug: "guides/getting-started" },
          { label: "Build and run", slug: "developing/working-on-gargantuan" },
          { label: "Your first project", slug: "guides/first-project" },
          { label: "Porting from Roblox", slug: "guides/roblox-deviations" },
        ] },
        { label: "Engine architecture", items: [
          { label: "Authority and lifecycle", slug: "developing/engine-architecture" },
          { label: "Runtime schema", slug: "developing/runtime-schema" },
          { label: "Attributes", slug: "developing/instance-attributes" },
          { label: "Tags", slug: "developing/instance-tags" },
          { label: "GUI runtime", slug: "developing/gui-runtime" },
        ] },
        { label: "Developer API", items: [
          { label: "API organization", slug: "api" },
          { label: "Instances and services", slug: "api/instances-and-services" },
          { label: "World and rendering", slug: "api/world-and-rendering" },
          { label: "Input, GUI and media", slug: "api/input-gui-and-media" },
          { label: "Networking and persistence", slug: "api/networking-and-persistence" },
          { label: "Declaration catalog", slug: "api/declaration-catalog" },
          { label: "Datatypes and globals", slug: "api/datatypes-and-globals" },
        ] },
        { label: "Renderer, physics and networking", items: [
          { label: "SDL GPU renderer", slug: "developing/renderer" },
          { label: "Publication and projection", slug: "developing/render-extraction" },
          { label: "Rigid and deformable physics", slug: "developing/physics" },
          { label: "Current networking", slug: "developing/current-networking" },
        ] },
        { label: "Studio integration", items: [
          { label: "Engine, Studio and Node", slug: "developing/engine-studio-node" },
          { label: "EditorHost boundary", slug: "developing/editor-host" },
        ] },
        { label: "Contributing and history", items: [
          { label: "Contributing", slug: "developing/contributing-to-gargantuan" },
          { label: "Code of conduct", slug: "developing/code-of-conduct" },
          { label: "History and evidence", slug: "developing/history" },
          { label: "Future architecture", slug: "developing/future-architecture" },
          { label: "Future concerns", slug: "developing/future-architecture-concerns" },
          { label: "Networking design", slug: "developing/networking-architecture" },
          { label: "Roadmap", slug: "developing/roadmap" },
        ] },
        { label: "Policy and licenses", items: [
          { label: "For AI agents", slug: "meta/agents" },
          { label: "License", slug: "meta/license" },
          { label: "Copyright notices", slug: "meta/additional-copyright-notices" },
        ] },
      ],
    }),
  ],
});
