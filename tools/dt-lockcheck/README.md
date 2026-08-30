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
./dt-lockcheck.py                        # the three default rules
./dt-lockcheck.py --module toneequal
./dt-lockcheck.py --rules violation
./dt-lockcheck.py --rules ALL                    # all four, discipline_gap too
./dt-lockcheck.py --format csv > findings.csv
```

and says what it found before it starts:

```
dt-lockcheck: 97 sources in /home/you/darktable/src/iop (darktable root above $PWD); 87 with a gui_data struct, 10 skipped
```

The tail names how many sources carry a `dt_iop_*_gui_data_t` at all: the rest
have no GUI state to share and are not analysed. That line goes to stderr, so it does not disturb `--format csv`/`json` output;
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

`--module` is repeatable, and **every** name given has to resolve: one bad name
in a list fails the whole run rather than quietly analysing the rest, so a CI
job pinned to a module list notices when a module is renamed. Options that can
only be wrong about the invocation — an unknown `--rules` name, `--why` with a
non-report `--format` — are checked before anything is read, so they fail the
same way whatever `--format` asked for.

`--format` chooses between the human-readable report, a csv of the findings,
and the raw extracted facts as json or as Datalog assertions — see
[Output formats](#output-formats) for what each one contains.


## Exit status

| code | meaning |
| --- | --- |
| 0 | ran to completion, whatever it found |
| 2 | wrong invocation: `src/iop` could not be located, `--src` does not point at it, an unknown rule name, a `--module` that names no source or a source with no `gui_data` struct, `--why` with a `--format` that cannot carry a tree, or a `--lemmalog` path that does not exist |

There is deliberately no "exit non-zero when there are findings" mode. A run
that finds nothing does not exist on any real tree, and a run always carries
some false positives (see
[Before you file anything](#before-you-file-anything)), so such a gate could
only ever be red. A useful gate
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
`--rules ...,discipline_gap` for the noisy fourth, or `--rules ALL` for
everything.

| rule | default | what it checks |
| --- | --- | --- |
| `widget_from_pipe` | yes | a `Gtk*`- or `Dtgtk*`-typed field touched from a `pipe` or `either` function, locked or not. GTK may only be called from the main thread, so the lock is beside the point |
| `violation` | yes | the module locks the field somewhere *and* accesses it unlocked from the other side, with the field shared across the two threads. The module's own code is the evidence that the lock is needed |
| `no_lock_share` | yes | the field is written and shared across both threads, and the module never locks it at all |
| `discipline_gap` | no | locked somewhere, accessed unlocked, but no cross-thread share was proven. Mostly noise — read it last |

`--rules` takes a comma-separated list of the names above, in any order, or the
single token `ALL` for every rule there is. `ALL` is upper-case on purpose: the
rule names are lower-case everywhere, here and in `rules.lemma`, so an
upper-case token cannot be mistaken for one of them, and a lower-case `all` is
an unknown rule rather than a second spelling of the same thing. `ALL` also
means "whatever the tool knows about", so a script that asks for it picks up a
rule added later without being edited — which is the reason to use it over
spelling the four out, and equally the reason not to use it in a CI job that
wants a fixed set. It may appear alongside names (`--rules ALL,violation`),
where it simply wins.

A field is reported under at most one rule, the first one in that order it
matches: a widget reached from the pipe is a `widget_from_pipe` finding and not
also a `violation`. The lower three ignore accesses in `gui_init`,
`gui_cleanup` and `gui_reset` — those run once, before or after anything else
can reach the field. `widget_from_pipe` does not need the exception: it only
fires on a `pipe` or `either` function, and those three are labelled `gtk`.

`widget_from_pipe` is the narrowest of the four: on the tree this was written
against it returns two fields across every module. One is a real defect
([#21915](https://github.com/darktable-org/darktable/issues/21915)); the other,
`colorharmonizer`'s `auto_detect`, is a false positive — see
[the widget hand-off](#before-you-file-anything) below. Two hits say nothing
about how often the rule is right, only that it asks for little.

Rule by rule, over all of `src/iop` on that tree. The second column counts
findings that name a field an already-confirmed defect report also names; the
rest are unreviewed candidates, not refutations:

| rule | findings | names a confirmed defect's field |
| --- | --- | --- |
| `widget_from_pipe` | 2 | 1 |
| `violation` | 34 | 16 |
| `no_lock_share` | 36 | 25 |
| `discipline_gap` | 59 | 1 |

`discipline_gap` is the only rule outside the default set, and that last row is
why. It asks the least of the four: a lock somewhere, an unlocked access
anywhere, and — unlike `violation` — no requirement that the two threads ever
meet on the field. Most of what comes back is a field locked for one reason and
read unlocked for another, which is not a defect. It is worth turning on when
you are auditing a single module by hand:

```sh
./dt-lockcheck.py --module toneequal --rules discipline_gap
```

and not worth reading across the whole tree.


## Output formats

`--format` picks what goes to stdout. All four describe the same run and differ
in how much of it survives:

| format | what it contains |
| --- | --- |
| `report` | the default: findings grouped by rule, human-readable, site lists capped |
| `csv` | one row per finding, every site, for a spreadsheet or a diff between runs |
| `json` | the raw extracted facts, before any rule ran |
| `lemmalog` | the same facts as Datalog assertions, for the engine |

`report` and `csv` carry *findings*, so both honour `--rules`. `json` and
`lemmalog` sit upstream of the rules and carry the facts the rules run on, so
`--rules` does not change them — filter or extend on your side instead.
`--src` and `--module` narrow all four. Only `report` can carry proof trees
([`--why`](#proof-trees)); asking for them with any other format is an error
rather than a silent no-op.

The status banner, the lemmalog notices and every warning go to stderr, so
stdout is safe to redirect whatever the format.

### report

```
=== violation  (34) ===========================================

basicadj.c  g->call_auto_exposure   (int)
   locked  : process_cl:1302[pipe](gui_lock), process_cl:1304[pipe](gui_lock),
             process_cl:1335[pipe](gui_lock), process:1420[pipe](gui_lock),
             process:1422[pipe](gui_lock), process:1434[pipe](gui_lock), ... (+5 more)
   unlocked: button_released:384[gtk], change_image:600[gtk]
```

**locked** lists the sites that establish the module's own convention — this is
your evidence that the field is meant to be protected. **unlocked** lists the
suspected defect. Sites are `function:line[thread]`, where the thread is one of
[`pipe`, `gtk` or `either`](#which-thread-is-this-on); `pipe` and `either` come
first, since the off-the-main-thread half is usually the interesting one. A `?`
means the tool could not work out where the function runs.

The report prints at most six locked and eight unlocked sites per finding and
says how many it left out. `--format csv` writes every one, so that is what to
use when the count matters.

Open both sets and decide whether the two can really run at the same time. If
they can, the fix is normally to extend the existing critical section to the
unlocked sites — not to add a new lock.

A trailing `N findings.` line closes the report, counting every rule asked for.

### csv

A header row, then one row per finding — the same findings as the report, in
the same order, with nothing capped:

| column | contents |
| --- | --- |
| `module` | the module name, i.e. the source file without its extension |
| `field` | the `gui_data` field, without the `g->` |
| `ctype` | the field's type as declared in the struct, without any `*` |
| `rule` | which of [the four rules](#the-rules) fired |
| `locked_sites` | every locked site, space-separated, each `function:line[thread]` |
| `unlocked_sites` | every unlocked site, same shape |

Two things the report shows and the csv does not: the name of the lock held at
each locked site (the report's `(gui_lock)` suffix), and the proof trees. Use
`json` for the first.

### json

The whole extracted fact base, one object per analysed module, before any rule
ran:

```json
{
 "basicadj": {
  "module": "basicadj",
  "src": "basicadj.c",
  "fields": { "call_auto_exposure": "int", "bt_auto_levels": "GtkWidget" },
  "accesses": [ ["process", "call_auto_exposure", "gui_lock", 1420, "w"] ],
  "thread": { "process": "pipe", "button_released": "gtk", "flags": null }
 }
}
```

| key | contents |
| --- | --- |
| `module` | the module name, the key repeated |
| `src` | the basename of the analysed source, with its extension — `.cc` for `bilateral`, `lens` and `tonemap`, which are C++ |
| `fields` | every field of the `gui_data` struct, mapped to its declared type. A nested anonymous struct appears once, under its own member name, with type `struct` |
| `accesses` | one entry per `g->field` mention, `[function, field, lock, line, mode]` |
| `thread` | every function in the file, mapped to its inferred thread |

In `accesses`, `lock` is the innermost lock held at that line:

| `lock` | meaning |
| --- | --- |
| `none` | nothing held — the sites a finding accuses |
| `gui_lock` | inside `dt_iop_gui_enter/leave_critical_section()` |
| `callee_lock` | the lock is not held here, but `&self->gui_lock` is passed to the callee, as in `dt_dev_sync_pixelpipe_hash()` |
| `bulk_init` | held, but in a function that writes eight or more fields under it and reads none back — bulk initialisation, which establishes no per-field convention. See [Before you file anything](#before-you-file-anything) |
| anything else | the name of a private mutex field, e.g. `histogram_lock`, taken with `dt_pthread_mutex_lock(&g->histogram_lock)` |

`mode` is `w` for a write and `r` for a read. `line` is 1-based, into the file
named by `src`.

In `thread` the value is `pipe`, `gtk`, `either` (see
[Which thread is this on?](#which-thread-is-this-on)) or `null` when inference
failed. A `null` function counts on neither side of the cross-thread test, so
it can never be what makes a field shared, but its unlocked accesses are still
listed as sites — that is where the report's `?` comes from. The map covers
every function in the file, including ones that never touch `gui_data`.

Modules with no `gui_data` struct are absent, not present and empty. Everything
is emitted in sorted order, so two runs over the same tree diff cleanly.

### lemmalog

The same facts as `json`, written as lemmalog assertions, one per line, to feed
the Datalog rules in `rules.lemma` — see [Proof trees](#proof-trees) for how to
run the two together.

```
+ gtk_type("GtkWidget")
+ ftype("basicadj", "call_auto_exposure", "int")
+ runs_on("basicadj", "process", "pipe")
+ access("basicadj", "call_auto_exposure", "process", "gui_lock", "w")
```

| predicate | meaning |
| --- | --- |
| `gtk_type(T)` | `T` is a widget type, i.e. one whose name starts `Gtk` or `Dtgtk`. Only the types actually seen in some `gui_data` struct are emitted; this is what `is_widget` asks |
| `ftype(M, F, T)` | field `F` of module `M` is declared `T` |
| `runs_on(M, Fn, T)` | function `Fn` of module `M` runs on thread `T` |
| `access(M, F, Fn, Lock, Mode)` | `Fn` touches `F` with `Lock` held, reading or writing |

Two differences from `json`, both because the rules do not use them: a function
whose thread could not be inferred gets no `runs_on` fact at all rather than a
`null` one, and `access` carries no line number, which also lets the repeated
accesses on one line collapse into a single fact.


## Recommended workflow

Scan with the defaults, then read the source at the sites the report names, and
reach for `--why` or `--format json` only when you have a question the report
itself cannot answer:

```sh
./dt-lockcheck.py                                  # the scan
./dt-lockcheck.py --module basicadj --why          # why does it believe this?
./dt-lockcheck.py --module basicadj --format json  # let me ask the facts myself
```

**Read the source first.** For "is this finding real?", the report already
gives `file:line` for both the locked and the unlocked side. Opening those two
places is usually faster and always more conclusive than another run, because
the causes that make a finding wrong are the ones the tool cannot see at all:
mutual exclusion by protocol, a lock passed into a callee, a widget handed to
the main loop. See [Before you file anything](#before-you-file-anything) for
the full list. `--why` tells you why the *tool* concluded something; only the
source tells you whether it is true.

**`--why` answers "why does the tool believe this?"** It prints, per finding,
the chain from the rule down to the base facts: the locked site that
established the discipline, and the thread inference that made the field
cross-thread — including whether that went through an
[`either`](#which-thread-is-this-on) site. It is worth a run when

- you doubt the thread labels — an `either` site, or a static helper the call
  graph reached two or three frames below the callback. The tree names the exact
  `runs_on` fact, which is what you would otherwise reconstruct by hand;
- you are writing the finding up, and want the evidence chain as the skeleton of
  a bug report.

Narrow it first — one `--module`, or one `--rules` — since the trees take a full
report from roughly 250 lines to 1600. It needs the lemmalog engine, attaches to
`--format report` only, and explains the *first* unlocked site of each finding
rather than all of them. See [Proof trees](#proof-trees).

**`--format json` answers "let me query the facts myself."** It is the whole
extracted fact base with no rule applied, so it is the one to use when a rule is
not the question:

- every access to one field, including the ones no rule fired on — the report
  only lists sites for fields that triggered a rule;
- what thread the tool inferred for each function, including the `null` ones it
  gave up on;
- a filter or a rule of your own over the facts, or a diff of two runs to see
  what a patch changed.

Pair it with `--module` to keep it small. For an agent driving this tool, this
is the format to consume; see [Output formats](#output-formats) for the keys,
and [Proof trees](#proof-trees) for `--format lemmalog`, which is the same facts
against the Datalog rules if you would rather add rules than write code.


## Before you file anything

Findings are **candidates**. On the tree this was developed against, 42 of the
72 default-settings findings name a field from a defect report that already
existed; the other 30 have not been ruled on either way, and a spot-check of a
dozen of them found a clear majority to be real defects of the same shape. No
one has measured a false-positive rate, and the `discipline_gap` tier is
markedly worse than the other three. Please confirm a finding by reading the
code before opening an issue.

Five false-positive causes are worth knowing, because you will meet them:

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
- **A widget touched from the pipe only to hand it to the main loop.**
  colorharmonizer's `_update_histogram()` runs on a pipe thread and does
  `g_object_ref(g->auto_detect)` followed by `gdk_threads_add_idle()`. The ref
  is atomic and the GTK call itself happens on the main thread, so this is the
  correct idiom — but `widget_from_pipe` sees a widget field touched from the
  pipe and reports it.
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
   unlocked: _get_selected_area:1223[pipe], button_released:373[gtk], ... (+9 more)
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
