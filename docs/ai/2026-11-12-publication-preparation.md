# GPL licensing and publication preparation

- **Date**: 2026-11-12.
- **Tool**: Codex (`codex`) `0.167.2`, checked with the runtime detector.
- **Model**: `gpt-6.1-sol`, detected from the current runtime.
- **Source baseline**: `79d1d76`.
- **Commit**: The commit adding this record; consult this file's Git history.
- **Authorship boundary**: AI-assisted review, documentation, licensing,
  demo capture, and build verification. Application source and tests are
  unchanged by this work.
- **Transcript**: Retained privately by the author.

## Guiding prompts

> I am working on a spreadsheet program called tam-data. Can you see the repo for that?

> Does this look worthwhile to put on GitHub?

> Address all four. The LICENSE should be GPL like the rest of Tamlinux.

> Can we create a repo on GitHub now?

“All four” refers to the preceding review's proposed license and provenance,
build instructions for users outside Tamlinux, screenshot and demo example,
and performance claims qualified as unmeasured goals.

## Decisions and changes

- Use GPL version 3 or later, matching Tamlinux's stated license. Copy the
  existing Tamlinux GPLv3 license text and state its application in the README.
- Explain core-only `libtam` acquisition, explicit prefix and `pkg-config`
  discovery, optional installation, and demo controls.
- Capture the real built-in demo in a terminal and include only that window
  in the README image. No generated UI or invented file-loading features.
- Mark startup and memory figures as unmeasured targets in the README and
  roadmap. State that only the demo currently runs.
- Backfill earlier AI attribution from retained transcripts and commit
  trailers, explicitly recording the missing implementation transcript.
- The initial preparation request stayed local. Fred's later explicit
  request authorized creating the public GitHub repository and publishing
  the existing development branch.

## GitHub publication

Created [greenermoose/tam-data](https://github.com/greenermoose/tam-data) as
a public repository on 2026-11-12 and connected it as `origin`. Publication
uses the existing `dev` branch as the development/default branch. The local
`main` branch is not part of this initial publication; no tag, release, or
deployment is included.

Commit `8b34fdb` contains the prepared licensing, provenance, screenshot,
and documentation. This follow-up records Fred's publication request.

## Verification

- `make` built the current source with strict warnings enabled.
- `make test` passed all 362 checks with ASan/UBSan and leak detection;
  the initial sandboxed attempt hit LeakSanitizer's tracing restriction,
  and the permitted run outside the sandbox completed successfully.
- Exported fresh source trees and built against published `libtam` main
  commit `02576574d4b1438a1edf510d7a1b357643d2cfcc` (version 0.4.0).
  Core-only library build and installation into a temporary prefix passed.
- Both explicit `LIBTAM_PREFIX` and `pkg-config` discovery built successfully
  against that prefix. The fresh source test run also passed all 362 checks.
- Installed the executable into a separate temporary prefix and verified
  `--version` reports `tam-data 0.1.0`.
- Reviewed the actual demo screenshot and verified its dimensions. No
  physical Linux-console test or performance benchmark was performed.
- Checked local Markdown links, the GPL license copy, diff whitespace, and
  public provenance files for private identifiers and paths.
- Before first publication, scanned all 71 Git objects reachable from the
  prepared `dev` head for private identifiers, transcript paths, and private
  repository references; no matches were found.
