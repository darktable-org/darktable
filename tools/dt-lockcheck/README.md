# dt-lockcheck

Finds `gui_data` fields that an IOP module shares between the pixelpipe and the
GTK main thread without locking them consistently.

Each IOP module keeps its per-instance GUI state in a
`dt_iop_<module>_gui_data_t` struct, reached as `g->field`. `gui_*` callbacks
and widget handlers run on the GTK main thread; the pipeline callbacks run
somewhere else, or sometimes on the main thread too — see
[Which thread is this on?](#which-thread-is-this-on). A field that both sides
touch has to be accessed inside `dt_iop_gui_enter/leave_critical_section()`
(the module's `gui_lock`), or under a private mutex. This script points at the
places where that is not done, or not done everywhere.

It is a lexical analysis, not a race detector — see
[Before you file anything](#before-you-file-anything).


## Requirements

Python 3.8 or newer. Standard library only, nothing to install or build.

[lemmalog](https://github.com/JordyZomer/lemmalog) is optional, and only needed
for `--why`, which prints the [proof tree](#proof-trees) behind each finding.


## Usage

Run it from anywhere inside a darktable checkout — this tool's own directory,
the project root, a build directory — or with the script on your `$PATH`. It
locates `src/iop` by itself:

```sh
./dt-lockcheck.py                        # the three main rules
./dt-lockcheck.py --module toneequal
./dt-lockcheck.py --rules violation
./dt-lockcheck.py --format csv > findings.csv
```

and says what it found before it starts:

```
dt-lockcheck: 94 sources in /home/you/darktable/src/iop (darktable root above $PWD)
```

That line goes to stderr, so it does not disturb `--format csv`/`json` output;
`-q` suppresses it. It is worth reading when you keep more than one checkout,
since it is what makes analysing the wrong one obvious.

To analyse a tree other than the one you are standing in:

```sh
./dt-lockcheck.py --src /path/to/darktable/src/iop
./dt-lockcheck.py --src /path/to/darktable          # a checkout root works too
```

`--why` adds the proof tree behind each finding — see
[Proof trees](#proof-trees). It is off by default and needs the
[lemmalog](https://github.com/JordyZomer/lemmalog) engine; `--lemmalog PATH`
points at a binary that is not on `$PATH`. **`--format csv`, `json` and
`lemmalog` never carry a tree and never invoke the engine**, so a script
consuming them behaves the same whether or not lemmalog is installed.

`$DT_LOCKCHECK_SRC` does the same as `--src`, for CI. The search order is
`--src`, then `$DT_LOCKCHECK_SRC`, then a walk up from the current directory,
then a walk up from the script's own location. The current directory comes
before the script location on purpose: if you symlink the script onto `$PATH`
out of one checkout, running it inside another still analyses the one you are
standing in. An explicit `--src` never falls back, so a typo fails loudly.

`--module` is repeatable. `--format json` dumps the raw extracted facts — every
field, every access site with its lock state, and the thread each function was
inferred to run on — if you would rather write your own rules over them.


## Exit status

| code | meaning |
| --- | --- |
| 0 | ran to completion, whatever it found |
| 2 | wrong invocation: `src/iop` could not be located, `--src` does not point at it, an unknown rule name, a `--module` that matched nothing, or a `--lemmalog` path that does not exist |

There is deliberately no "exit non-zero when there are findings" mode. Around
one finding in three is a false positive (see
[Before you file anything](#before-you-file-anything)), so a clean run does not
exist on any real tree and such a gate could only ever be red. A useful gate
would have to compare against a checked-in baseline and fail on findings that
are *new* — that does not exist here yet.

A CI job needs 1 and 2 kept apart: 2 means nothing was analysed, which a bare
"non-zero" check would otherwise read as a clean run turned bad.


## Which thread is this on?

The tool labels every function it analyses, and every site it reports, with one
of three threads. The middle one is the reason this is a section and not a
sentence.

| label | what it means |
| --- | --- |
| `gtk` | the GTK main thread: `gui_init`, `gui_update`, `gui_changed`, widget and draw callbacks, mouse and scroll handlers |
| `pipe` | a pixelpipe worker thread, never the main thread — for the instance that owns `gui_data`: `process`, `process_cl`, `process_tiling*`, `modify_roi_in/out`, `tiling_callback`, `distort_mask`, `output_format` |
| `either` | driven by the pipe, but GTK-thread code reaches it too, so it has no thread you can rely on |

`either` covers `commit_params`, `init_pipe`, `cleanup_pipe`,
`distort_transform`, `distort_backtransform`, `blend_colorspace` and
`default_colorspace`. It is easy to read those as pipe-only and be wrong:

- `basecurve`'s `init_pipe()` calls `commit_params()` straight back
  (`src/iop/basecurve.c:1450`), and switching image rebuilds the screen pipes
  from an idle callback (`src/views/darkroom.c:1398`) — that is the GTK main
  loop, so both run there.
- The colour picker calls `default_colorspace()` while building a picker button
  (`src/gui/color_picker_proxy.c:148`), with `NULL` for both `pipe` and `piece`;
  the blend GUI calls `blend_colorspace()` the same way.
- Mask handling and darkroom zoom call `distort_transform()` and
  `distort_backtransform()` directly.

An `either` site counts on **both** sides of the cross-thread test, since two
invocations of one such callback can land on different threads. When a finding
rests on one, that is worth a moment's thought before you accept it: the two
sites may in practice always run on the same thread on the path you care about.

Each label covers the static helpers reached from it, which the tool propagates
along the call graph within one file. That is where the mistakes hide — the
offending line is often two or three frames down, in a function whose name says
nothing about which thread got there.

## The rules

Reported most-worth-reading first. The first three run by default; add
`--rules ...,discipline_gap` for the noisy fourth.

| rule | what it means |
| --- | --- |
| `widget_from_pipe` | a `Gtk*`-typed field touched from a `pipe` or `either` function. GTK may only be called from the main thread |
| `violation` | the module locks the field somewhere *and* accesses it unlocked from the other side. The module's own code is the evidence that the lock is needed |
| `no_lock_share` | the field is written and shared across both threads, and the module never locks it at all |
| `discipline_gap` | locked somewhere, accessed unlocked, but no cross-thread share was proven. Mostly noise — read it last |

`widget_from_pipe` is the sharpest of the four: on the tree this was written
against it returns exactly one field across every module, and that one is a real
defect ([#21915](https://github.com/darktable-org/darktable/issues/21915)).


## Reading the output

```
=== violation  (33) ===========================================

basicadj.c  g->call_auto_exposure   (int)
   locked  : process_cl:1302[pipe](gui_lock), process:1420[pipe](gui_lock),
             _auto_levels_callback:233[gtk](gui_lock)
   unlocked: button_released:384[gtk], change_image:600[gtk]
```

**locked** lists the sites that establish the module's own convention — this is
your evidence that the field is meant to be protected. **unlocked** lists the
suspected defect. Sites are `function:line[thread]`, where the thread is one of
[`pipe`, `gtk` or `either`](#which-thread-is-this-on); `pipe` and `either` come
first, since the off-the-main-thread half is usually the interesting one. A `?`
means the tool could not work out where the function runs.

Open both sets and decide whether the two can really run at the same time. If
they can, the fix is normally to extend the existing critical section to the
unlocked sites — not to add a new lock.


## Before you file anything

Findings are **candidates**. Roughly two in three `violation` / `no_lock_share`
candidates held up under review on the tree this was developed against; the
`discipline_gap` tier did considerably worse. Please confirm one by reading the
code before opening an issue.

Four false-positive causes are worth knowing, because you will meet them:

- **Exclusion by protocol rather than by lock.** `rgblevels` hands work to the
  pipe through a state machine (`0` idle, `1` requested, `-1` running, `2`
  ready). While the state is `-1` the GTK side is excluded by the protocol, so
  the unlocked accesses in between are deliberate. The script cannot see this.
- **A lock passed into a callee.** Handled for the common
  `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)` shape, but not in
  general.
- **A field that merely sits inside a critical section taken for a different
  field.** Partly handled: a function that writes eight or more distinct fields
  under the lock, and never reads them back, is treated as bulk initialisation
  and no longer counts as evidence of a per-field convention.
- **A finding that rests entirely on an `either` site.** `either` counts on both
  sides of the cross-thread test because it *can* run on either thread — but on
  the path that matters it may always run on one. Check the proof tree, or the
  `[either]` labels in the report, before you trust such a finding.

And the limits:

- Lock depth is tracked lexically, so an early `leave_critical_section()` on one
  branch makes later code on other branches look unlocked.
- Thread inference is name-based plus call-graph propagation *within one file*. A
  callback reached only through a function pointer stored elsewhere comes out as
  thread `?` and is skipped.
- Coverage is `src/iop/*.c` and `*.cc` only. Shared code under `src/develop/` and
  `src/libs/` is not analysed.
- `g` is matched by convention, not by type. A function that binds `g` to
  something that is not the module's GUI data — a gain map, a gaussian handle —
  is skipped entirely, so any real `gui_data` access in it is missed too.
- Defects needing dataflow inside a function are out of reach — notably "pointer
  copied under the lock, then dereferenced after releasing it", which is a real
  and recurring shape here. Do not read a clean run as an all-clear.


## Proof trees

The rules are also written out as Datalog, in `rules.lemma`, for use with
[lemmalog](https://github.com/JordyZomer/lemmalog). Install it, put it on your
`$PATH`, and `--why` follows every finding in the report with the proof behind
it:

```sh
./dt-lockcheck.py --module basicadj --why
```

```
basicadj.c  g->box_cood   (dt_boundingbox_t)
   locked  : _auto_levels_callback:235[gtk](gui_lock)
   unlocked: _get_selected_area:1223[pipe], button_released:373[gtk], ...
   why     : violation(basicadj, box_cood, _get_selected_area)
     ↳ via rule/violation
       has_discipline(basicadj, box_cood)
         ↳ via rule/has_discipline
           access(basicadj, box_cood, _auto_levels_callback, gui_lock, w)
             ↳ asserted (base fact)
       shared(basicadj, box_cood)
         ↳ via rule/shared
           may_pipe(basicadj, box_cood)
             ↳ via rule/may_pipe
               touch(basicadj, box_cood, pipe)
                 ↳ via rule/touch
                   access(basicadj, box_cood, _get_selected_area, none, r)
                     ↳ asserted (base fact)
                   runs_on(basicadj, _get_selected_area, pipe)
                     ↳ asserted (base fact)
           ...
```

The tree bottoms out in the extracted base facts, which is what makes it worth
having: it names the exact locked site that is the evidence a discipline
exists, and the thread inference that made the field cross-thread — including
whether that inference went through `may_pipe`/`may_gtk` on an
[`either`](#which-thread-is-this-on) site. That reads as a ready-made skeleton
for a bug report, and it is also how you see that a finding rests on a thread
guess you disagree with.

The `why     :` line names the goal, because a finding may list several
unlocked sites and the tree explains only the first one — the `pipe` or
`either` site the report leads with.

**This is additive.** The Python and the Datalog implement the same rules and
produce the same findings; nothing about which findings are reported changes.

- **Off by default**, because the trees take a full report from roughly 250
  lines to 1600. They pay for themselves on a narrowed run — one `--module`, or
  one rule you are about to write up.
- `--why` **fails (exit 2) when no lemmalog binary can be found**, rather than
  quietly printing a plain report. You asked for the trees, so a missing engine
  is a problem to fix, not something to paper over.
- `--lemmalog PATH` / `$DT_LOCKCHECK_LEMMALOG` — a binary that is not on
  `$PATH`. `--lemmalog` implies `--why`, since that is the only reason to name
  it. A path that does not exist is an error even if `lemmalog` *is* on
  `$PATH`: an explicit path never falls back to the search.
- The trees are attached to `--format report` only. `csv`, `json` and
  `lemmalog` do not invoke the engine at all and produce the same findings and
  the same facts either way — deliberately, since those formats feed other
  tools and should not change shape because an optional binary is installed.
  `--why` combined with one of them is an error, not a silent no-op. The
  engine's output is captured rather than inherited, so it cannot reach stdout;
  the line naming the binary goes to stderr, like the other status lines.

Cost is one `lemmalog` run for the whole report, about 0.5 s over ~3.3k facts.
A failure *after* the engine has been located and started — engine error,
timeout, missing `rules.lemma` — degrades to a warning on stderr and a report
without trees, since by then there is a report worth printing.

If you would rather drive the engine yourself — to add rules of your own, or to
query interactively — `--format lemmalog` writes the base facts out:

```sh
( cat rules.lemma
  ./dt-lockcheck.py --format lemmalog
  echo run
  echo '? violation(M,F,Fn)'
) | lemmalog
```
