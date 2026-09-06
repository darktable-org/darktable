# AGENTS.md – darktable

Instructions for AI coding agents working on darktable. It covers what agents
specifically get wrong; it is not an overview of the codebase.

**Read [`dev-doc/`](dev-doc/README.md) before changing a subsystem.** It is
darktable's in-tree developer documentation -- IOP module API, pixelpipe,
introspection, GUI helpers, shortcuts, the AI subsystem -- versioned with the
code, so it describes the checkout you have. Prefer it over the wiki wherever
the two disagree. Most bad agent patches come from inferring an API instead of
opening the page that documents it.

The [Developer's guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide)
has the coding style, and [CONTRIBUTING.md](CONTRIBUTING.md) the social side.

## The one rule that matters most

**Every commit must compile on its own.**

`git bisect` is how darktable developers find regressions, and it is useless
across commits that do not build. A series where only the last commit compiles
is worse than one squashed commit, because it silently poisons the history for
everyone else.

If you produce a series, build at each step:

```bash
git rebase --exec 'cmake --build build' <base>
```

If you cannot or will not do that, squash into a single commit that builds.

Do not commit a "checkpoint" or "work in progress" state. Do not split a change
so that the halves only work together.

## Boundaries

Go ahead without asking: read anything, build, run the test suites, run
`darktable-cli`, and edit the files the task is about.

Ask first: adding a dependency, changing a public API or the database schema,
touching a subsystem the task did not name, and anything CONTRIBUTING.md calls
substantial -- discuss those before writing them, not after.

Finish at a staged change, and a drafted commit message if one was asked for.
Committing, pushing and opening the PR are the human's steps, and they will
usually not be asked for at all. Stage only the files the task touched -- never
`git add -A` over a tree you have not looked at -- and keep build output,
editor files and scratch notes out of it.

Never reformat, rename or tidy code the task did not touch, and never fix an
unrelated bug you notice in passing: mention it instead. Do not add a comment,
file or abstraction that nobody requested.

## Commits

Subject on one line, under about 72 characters, no full stop. Prefix with the
area (`history:`, `exif:`, `bauhaus:`, `cmake:`, `ci:`) when the subject alone
would not say which part of darktable moved. Common here, not required.

Both a bare subject and a subject plus body are normal; roughly half of recent
commits carry a body. A subject alone is enough when the diff explains itself
-- data additions, translation updates, version bumps, typos. Anything
behavioural earns a body saying what was wrong before and what the change does
about it, wrapped at about 72 columns. If you cannot tell which applies, write
the body: an unnecessary paragraph costs far less than a behavioural change
nobody can account for later.

```text
cmake: link libwayland-client when GTK3 has the Wayland backend

gui/gtk.c calls into libwayland-client directly to detect server-side
decoration support, but gtk+-3.0.pc does not list wayland-client. The
symbols were only resolved by accident, via libgdk-3's own DT_NEEDED,
which breaks on distros linking with `-Wl,--no-undefined`.

Fixes #1234
```

Close issues with `Fixes #1234` or `Closes #1234` on its own line at the end,
or `Related: #1234` when the commit does not resolve the issue. In a series,
only the commit that completes the fix carries the closing keyword. A
reference never replaces the explanation: "fix #1234" alone is useless to
anyone reading `git blame` without a browser, and the history outlives the
issue tracker.

Describe the change, not the process that produced it. "address review
comments" and "second attempt" mean nothing to a later reader who cannot see
the thread.

Commit trailers are rare here. Disclose AI assistance in the pull request
description instead of inventing a trailer convention.

## Build

The README's Compile section has the commands, for the `build.sh` route and
for driving cmake by hand. What it does not say:

- `--enable-ai` and `-DUSE_AI=ON` are the same switch: `build.sh` uppercases
  the feature name and prefixes `USE_`. The features are `ai`, `camera`,
  `colord`, `gmic`, `graphicsmagick`, `imagemagick`, `jxl`, `kwallet`,
  `libsecret`, `lua`, `mac_integration`, `map`, `mcp`, `opencl`, `opencv`,
  `openexr`, `openmp`, `unity` and `webp`. Left alone, cmake enables what it
  autodetects; a flag forces one feature either way
- to check that a change compiles you need neither prefix nor install: drop
  `--install --sudo`, or stop after `cmake --build`
- on Windows, build in the MSYS2 UCRT64 shell, not a plain Windows prompt --
  the toolchain and every dependency come from `mingw-w64-ucrt-x86_64-*`.
  `build.sh` works there with `--build-generator Ninja` and no `--sudo`
- if `AGENTS.local.md` gives its own build or run commands, use those instead:
  how a checkout is built is a local matter, not a project convention

Verify with a real build before proposing changes. A change that has only been
reasoned about is not finished. If you touch code behind an `#ifdef`, build
with that feature both on and off. The disabled path is where broken stubs
hide.

## Testing

Unit tests use cmocka and need `-DBUILD_TESTING=ON` plus `libcmocka-dev`.

Integration tests live in `src/tests/integration/` and check pixel output
against a reference within dE < 2, with OpenCL both off and on:

```bash
cd src/tests/integration && ./run 0001-exposure
```

They need ImageMagick's `compare` and python3 with `opencv`, `numpy` and
`colour-science`. **Run them if you touch the pixelpipe or any `src/iop/`
module.** They are the only thing that catches a change which builds, runs, and
quietly alters everyone's images.

`darktable-cli` runs the pipeline headless and is the fastest way to exercise a
change without the GUI:

```bash
darktable-cli <input> [<xmp>] <output>       # render one image
darktable-cli ... -d perf                    # timing output
darktable-cli ... --bench-module <module>    # run a module 50x, report timing
```

## Verify what you claim

The most common failure is confident output that was never executed.

- Read the code you are changing before changing it. Do not infer an API from
  its name
- When you state a fact about the codebase, cite the file and line you read it
  from
- Run the app, or at least the code path, when the change is user-visible.
  `darktable-cli` runs headless
- Say plainly what you did not verify. "This builds but I have not run it" is
  useful; silence is not
- Do not invent file paths, function names, config keys or CMake options. If
  you are unsure whether something exists, check
- Do not claim something works because it should

## Code style

The rules are in the
[Developer's guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide),
and the file you are editing shows most of them. The parts you would not
infer:

- **There is no `.clang-format` for darktable's own code, and no
  `tools/beautify_style.sh`.** Both were removed deliberately (`46b054cf15`,
  `b734b014cf`), though the wiki still mentions the script. The
  `.clang-format` files under `src/external/` belong to vendored projects; do
  not apply them to darktable code
- **SQL is formatted by hand.** Do not reflow, re-indent or tidy a query
- parallel loops use darktable's macros (`DT_OMP_FOR`, `DT_OMP_FOR_SIMD`), not
  bare `#pragma omp`
- file-local functions are `static` and start with an underscore, as in
  `_version_compare()`; public ones take a `dt_` prefix
- the vim/kate modeline footer is maintained by `tools/update_modelines.py`.
  Keep it when editing and do not hand-write one

Match the file you are editing, and do not reformat lines you did not
otherwise need to change.

## Comments

Comment the *why*, not the *what*. A comment restating the code is noise; a
comment explaining a non-obvious constraint saves the next person an hour.

- Record why a surprising choice was made, and what breaks without it
- Cite `file.c:line` when the reason lives in another file
- Do not narrate your process, leave TODO notes about your own work, or
  describe what the code "will" do
- Delete comments your change makes untrue. A stale comment is worse than none
- `FIXME` and `TODO` are established here, but use them for real pre-existing
  problems that outlive your change, not as a reminder to finish work you are
  in the middle of

Keep them short. If a comment needs five lines, usually the code needs
restructuring.

### Language and casing

**American English.** The tree uses `color`, never `colour`; likewise `center`,
`normalize`, `gray`, `canceled`. This covers comments and translatable strings
alike, and in strings it is not cosmetic: the `po/` catalogues translate from
American English, so a British spelling silently breaks the msgid that every
translation is keyed to.

**Comments and user-visible strings start lowercase, with no full stop at the
end:**

```c
// the encoder emits 1024x1024 regardless of the requested size
```

For strings this matters beyond style. `po/en@truecase.po` is a translation
whose whole purpose is to capitalize darktable's interface for English users
who want that, and it works only because the source strings are lowercase.
Capitalizing at the source both duplicates and defeats it. Normal
capitalization and punctuation inside a longer comment are fine; the rule is
about how it opens and closes.

**Plain ASCII punctuation.** Write `-`, not `–` or `—`, and straight quotes,
not curly ones. A typographic dash is correct in a numeric range such as `2–4`
or `0°–360°`, but not as a general substitute for other punctuation. Long
em-dashed asides read as machine-written and make an agent's comments stand out
from the code around them. Each one is standing in for punctuation that reads
better:

- an explanation or expansion becomes a **colon**:
  `// bail out early: the pipe has no input buffer yet`
- an aside becomes **commas**, or **parentheses** for a real digression:
  `// the decoder, which always emits 1024x1024, ignores the requested size`
- a contrast or abrupt turn becomes a **semicolon** or two sentences:
  `// it builds; it crashes on the OpenCL path`
- an afterthought tacked onto the end becomes a **full stop**, or gets cut

If none of those fit, the sentence is carrying two ideas, so split it. Short
declarative comments age better anyway.

## Pixelpipe code

Read [`dev-doc/IOP_Module_API.md`](dev-doc/IOP_Module_API.md) and
[`dev-doc/pixelpipe_architecture.md`](dev-doc/pixelpipe_architecture.md) first;
[`dev-doc/New_Module_Guide.md`](dev-doc/New_Module_Guide.md) walks through
adding a module.

Most `src/iop/*.c` modules have a C `process()` and an OpenCL `process_cl()`.
Keep both in sync, or explain why not.

- input and output are different buffers; no in-place pixel work
- flat indexing over nested loops, `restrict` on pointers, `const` where
  possible
- pixels are 4-channel RGBA; align on 64 bytes, use `DT_ALIGNED_PIXEL`
- no branches, casts or function calls in inner loops

`src/iop/useless.c` is the annotated boilerplate for a new module.

## Pitfalls

- `dt_film_t` must be zeroed with `dt_film_init()` before `dt_film_new()`. It
  holds a mutex, and an uninitialised one SIGKILLs on macOS in a way that looks
  like OOM
- `synch_all` starts every pipeline node at its module's `default_enabled`
  before replaying history, so auto-applied modules run whether or not the
  history asked for them
- masks: raster masks are mutually exclusive with drawn and parametric ones in
  `blend.c`; they do not compose
- GTK4 preparation is in progress. Use event controllers (`dt_gui_connect_key`,
  `dt_gui_connect_click`) rather than `"key-press-event"` and friends
- prefer existing darktable helpers (`dt_gui_*`, `dt_bauhaus_*`, `dt_ui_*`)
  over raw GTK; a widget built the wrong way looks wrong under the themes.
  [`dev-doc/GUI.md`](dev-doc/GUI.md),
  [`dev-doc/imageop_gui.md`](dev-doc/imageop_gui.md) and
  [`dev-doc/GUI_Recipes.md`](dev-doc/GUI_Recipes.md) have the patterns
- CMake 4.x refuses `file()` writes into the source tree; write to
  `${CMAKE_BINARY_DIR}`
- data files copied with `FILE(COPY ...)` are only copied at configure time, so
  editing them does not refresh the build tree

## Pull requests

- One logical change per PR. A PR that fixes a bug *and* adds a feature *and*
  reformats a file cannot be reviewed or reverted cleanly
- Say in the description what you ran, and what you did not
- Disclose that the change was written with AI assistance
- New user-visible strings need `_()`; new preferences need an entry in
  `data/darktableconfig.xml.in`

### Release notes

A PR that changes what a user experiences needs an entry in
`RELEASE_NOTES.md`, in the same PR: maintainers will not merge without one.

The file's own sections are the guide to what qualifies -- *The Big Ones*,
*UI/UX Improvements*, *Performance Improvements*, *Other Changes*, *Bug
Fixes*, *Lua*, *Changed Dependencies*. Put the entry under the heading that
already fits rather than adding one.

Nothing a user cannot observe needs an entry: refactors, test-only changes,
CI, comments, typos, translation updates. Neither does a regression fixed in
the same cycle that introduced it, since no release ever carried it. Leave
the camera-support sections alone -- `Base Support`, `White Balance Presets`
and `Noise Profiles` are filled in at release time, not per PR.

Keep the entry in its own commit (`RELEASE_NOTES.md: <what changed>`), so a
fix cherry-picked to a release branch does not drag a note written for a
different version with it.

Write it to read like its neighbours, and say what the user gets rather than
how it was done:

```text
- Fixed a crash at the end of neural restore's raw denoise on certain
  sensor sizes, where blending the tile seams wrote past the edge of
  the image.
```

If you cannot tell whether an entry is needed, open the PR without one and
ask, rather than inventing one or skipping it quietly.

## Local instructions

If `AGENTS.local.md` exists in the repo root, read it too and follow it
alongside this file. It is untracked: one developer's own instructions for
this checkout, not the project's.

It can change how you work -- the build and run commands above, for instance.
It does not change what a PR may contain: the conventions in this file still
govern that.
