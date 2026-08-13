<div align="center">

<img src="./assets/github/banner.png" alt="Gargantuan" width="656px" />
<br/>
<img src="./assets/github/demo-sphere.gif" alt="Gargantuan" width="324px" />
<img src="./assets/github/demo-waveform.gif" alt="Gargantuan" width="324px" />

<h3>An Independently Maintained Fork of Gargantuan</h3>

<a href="./LICENSE.md">
<img src="https://img.shields.io/github/license/gmoddev/gargantuan?style=flat-square&label=License" alt="MPL-2.0 License" />
</a>
<a href="https://github.com/teamfireworks/gargantuan">
<img src="https://img.shields.io/badge/Upstream-Team%20Fireworks-informational?style=flat-square" alt="Upstream Gargantuan" />
</a>

</div>

## About This Fork

This repository is an independently maintained fork of
[Gargantuan](https://github.com/teamfireworks/gargantuan), originally developed
and maintained by [Team Fireworks](https://github.com/teamfireworks).

It was split from upstream for personal development and experimentation with a
different architectural direction and development methodology. It is not intended
to replace or represent the upstream Gargantuan project.

The goal of this fork is to retain the core idea that made Gargantuan interesting
to me — a standalone game engine built around Luau, Instances, a DataModel, and a
familiar Roblox-inspired development model — while allowing me to experiment with
the runtime, tooling, security model, replication architecture, and eventually
Studio in ways that may differ substantially from upstream.

Development in this repository is expected to diverge over time.

This fork remains public. Team Fireworks and other Gargantuan contributors are
welcome to reference, adapt, or independently implement ideas and changes made
here where permitted by the project's license.

For the original Gargantuan project, documentation, community, and contribution
process, see the upstream repository:

https://github.com/teamfireworks/gargantuan

## Current Direction

Current development is focused primarily on strengthening the runtime foundation
before expanding the engine's feature set.

Work in this fork includes or is expected to include:

- Explicit Instance lifetime, ownership, and hierarchy contracts.
- An authoritative mutation model with validated and recorded state changes.
- Stable object identity suitable for serialization and replication.
- Deterministic snapshots and ordered incremental replication.
- Explicit execution domains and security boundaries.
- A Luau-first scripting environment built around the DataModel and Services.
- Improved project and script synchronization workflows.
- A future standalone Studio/editor built around the same runtime contracts.

Longer term, the intent is to explore a development environment that retains the
productivity and familiarity of Roblox's DataModel/Luau model while providing
greater control over the underlying engine and its APIs.

This is experimental work and should not currently be considered a production
replacement for Gargantuan or Roblox.

## Upstream Gargantuan

Gargantuan is a 3D game engine scriptable using Luau, independently developed by
Team Fireworks.

The original project describes its goals as providing a powerful, productive,
multiplatform game engine with a familiar Luau API surface while allowing
developers to own their platform, assets, and core scripts.

Upstream development is maintained separately by Team Fireworks:

- Repository: https://github.com/teamfireworks/gargantuan
- Documentation: https://gargantuan.teamfireworks.org/
- Contributing: https://gargantuan.teamfireworks.org/developing/contributing-to-gargantuan

Changes in this repository should not be interpreted as changes proposed,
approved, or maintained by Team Fireworks.

## Prior Art

The original Gargantuan design was informed by several other game engines and
projects. These references are retained from upstream for attribution:

| Resource | Info |
| --- | --- |
| [Kinemium Engine](https://github.com/Qquaded/Kinemium-Engine) | Initial reference implementation for some datatypes |
| [Phoenix Engine](https://github.com/PhoenixWhitefire/PhoenixEngine) | Initial reference implementation for Instances and the renderer |
| [Kitbash'd](https://github.com/kitbashd) | Previously inspired the renderer |
| [Flux](https://github.com/thegalaxydev/flux) | Inspired the architecture of Instances and userdatas |
| [Librebox](https://github.com/StayBlue/librebox-demo/) | Examples used to test the Gargantuan engine |
| [Roblox Creator Documentation](https://create.roblox.com) | API design inspiration |

## License

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at http://mozilla.org/MPL/2.0/.

This fork retains the licensing and applicable copyright notices of the
upstream Gargantuan source from which it was derived.

## Legal Notice

This repository is an independently maintained fork of Gargantuan.

The original Gargantuan project was created and is maintained by Team Fireworks.
This fork is not maintained, authorized, or endorsed by Team Fireworks, and
changes made here should not be attributed to the upstream maintainers.

Gargantuan and this fork are independent projects and are NOT affiliated with,
authorized by, endorsed by, or in any way officially connected with Roblox
Corporation. "Roblox" is a registered trademark of Roblox Corporation.

No reverse engineering, decompilation, or extraction of proprietary binaries,
source code, or assets belonging to Roblox Corporation is represented as part of
this fork. The engine implementation is based on independently implemented
runtime and API concepts intended for developer familiarity and interoperability.
