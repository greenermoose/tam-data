# AI development provenance

Fred Horch directs this project. AI assistants have contributed planning,
implementation, review, and documentation. Attribution below distinguishes
verified transcripts from information available only in commit trailers.
An author name in Git alone is not evidence of human-only implementation.

## How to read this record

The [Tamlinux public AI provenance standard](https://github.com/greenermoose/tamlinux/blob/main/docs/ai-provenance-standard.md)
explains the record format. Guiding prompts, decisions, verification, and
evidence limitations are indexed in [docs/ai/README.md](docs/ai/README.md).
Transcripts are retained privately; public records omit their identifiers.

## Toolchain

| Tool | Version evidence | Model evidence | Scope |
| --- | --- | --- | --- |
| Antigravity (`agy`) | `1.5.3` in the 2026-11-08 commit trailers; later session CLI version unverified | `gemini-3.8-flash-high` and `gemini-4-argon-high` in initial commit trailers; later session model selections described in the records | Initial project, roadmap, workplans, naming, and milestone documentation |
| OpenCode (`opencode`) | `2.2.5` in commit `25b24c9` | `big-pickle` in that commit's trailers; transcript not found | Phase 0 implementation |
| Codex (`codex`) | `0.167.2`, checked by the runtime detector on 2026-11-12 | `gpt-6.1-sol`, detected from the current runtime | Publication preparation, documentation, licensing, demo capture, and verification |

Historical versions are attributed to their evidence, not presented as fresh
runtime checks. No model or tool version is assigned to an undocumented
session by assuming it matches a different session.

## Milestones

| Work | Commits / record | Authorship boundary |
| --- | --- | --- |
| Initial project, roadmap, Phase 0 workplan | `d6b593b`, `2b03cc4`, `d3c755a`; [foundation record](docs/ai/2026-11-08-foundation.md) | AI-assisted planning with Antigravity; prompts verified from retained transcripts |
| Phase 0 code and tests | `25b24c9`; [foundation record](docs/ai/2026-11-08-foundation.md) | OpenCode contribution recorded in commit trailers; implementation prompt and transcript not found |
| Rename to tam-data and document version milestones | `fcd4846`, `79d1d76`; [naming and roadmap record](docs/ai/2026-11-12-naming-and-roadmap.md) | Antigravity-assisted changes; retained prompts and model selections verified, CLI versions unverified |
| GPL licensing, publication preparation, and initial GitHub publication | [preparation record](docs/ai/2026-11-12-publication-preparation.md) | Codex-assisted documentation, verification, and repository publication on Fred's explicit request; existing application source unchanged |
