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
places where that is not done, or not done everywhere — and, under the
non-default [`pointer_share`](#the-rules), at shared pointer fields where doing
it everywhere is still not enough.

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
./dt-lockcheck.py                               # the default rules
./dt-lockcheck.py --module toneequal
./dt-lockcheck.py --rules violation
./dt-lockcheck.py --rules ALL                   # known noisy rules, too
./dt-lockcheck.py --format csv > findings.csv
./dt-lockcheck.py --recheck                     # only the stale false positives
```

Findings that somebody has already judged not to be defects are suppressed by
default, from a list checked in beside the script — see
[The false-positive list](#the-false-positive-list) for how an entry expires,
and `--ignore-false-positives` for the full check.

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

The [false-positive list](#the-false-positive-list) is consulted after the rules
have run, so it changes which findings are *reported* and nothing else. `json`
and `lemmalog` carry the facts the rules run on and are untouched by it, for the
same reason `--rules` does not touch them.

`--fail-on-findings` makes a run that reported something exit 1, for CI. It is
off by default; see [Exit status](#exit-status) for why it is red on this tree
today.


## Exit status

| code | meaning |
| --- | --- |
| 0 | ran to completion, whatever it found |
| 1 | `--fail-on-findings` was given and something was reported |
| 2 | wrong invocation: `src/iop` could not be located, `--src` does not point at it, an unknown rule name, a `--module` that names no source or a source with no `gui_data` struct, `--why` with a `--format` that cannot carry a tree, or a `--lemmalog` path that does not exist |

"Exit non-zero when there are findings" is `--fail-on-findings`, and it is off
by default because it is red on this tree today: 74 findings survive the
[false-positive list](#the-false-positive-list), and most of them are unreviewed
or filed-and-unfixed rather than wrong. It passes once every finding is either
fixed or recorded as judged, which is the condition for wiring it into CI. A
gate that fails only on findings that are *new* is a different thing and would
need a checked-in baseline of all of them; that does not exist here.

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

Callbacks are labelled from **how their address is taken**, not from their name,
which is what reaches the handlers no naming convention describes:

| how the function is handed over | label |
| --- | --- |
| passed to a callee as an argument | the registrar's thread — the callee calls it synchronously |
| passed to `g_idle_add`, `g_signal_connect` and friends | `gtk`, whatever thread registered it |
| stored into a struct member or a file-scope table | `either` — it escapes the file, so the call site is unknown |

The first row is what covers darktable's own helpers: `dt_gui_connect_motion()`
and the bauhaus quad setters register handlers without any of the spellings a
keyword list would look for. The third is how the `dev->proxy` accessors are
labelled — another module calls them and cannot take your `gui_lock`, which
`dev-doc/GUI_Threading.md` covers under *Publishing `gui_data` Through a Proxy*
(that document arrives with the pending dev-doc PR).

Labels that come from the tables above are ground truth taken from the module
API, and propagation never overwrites them.

## The rules

Reported most-worth-reading first. All but `discipline_gap` run by default; add
`--rules ...,discipline_gap` for that one, or `--rules ALL` for everything.

| rule | default | what it checks |
| --- | --- | --- |
| `widget_from_pipe` | yes | a `Gtk*`- or `Dtgtk*`-typed field touched from a `pipe` or `either` function, locked or not. GTK may only be called from the main thread, so the lock is beside the point |
| `violation` | yes | the module locks the field somewhere *and* accesses it unlocked from the other side, with the field shared across the two threads. The module's own code is the evidence that the lock is needed |
| `no_lock_share` | yes | the field is written and shared across both threads, and the module never locks it at all |
| `discipline_gap` | no | locked somewhere, accessed unlocked, but no cross-thread share was proven. Mostly noise — read it last |
| `pointer_share` | yes | a pointer-typed field shared across the two threads, with no unlocked access anywhere. The lock protects the pointer load, not the object the other thread may free under the reader |

`--rules` takes a comma-separated list of the names above, in any order, or the
single token `ALL` for every rule there is. `ALL` is upper-case on purpose: the
rule names are lower-case everywhere, here and in `rules.lemma`, so an
upper-case token cannot be mistaken for one of them, and a lower-case `all` is
an unknown rule rather than a second spelling of the same thing. `ALL` also
means "whatever the tool knows about", so a script that asks for it picks up a
rule added later without being edited — which is the reason to use it over
spelling them out, and equally the reason not to use it in a CI job that
wants a fixed set. It may appear alongside names (`--rules ALL,violation`),
where it simply wins.

A field is reported under at most one rule, the first one in that order it
matches: a widget reached from the pipe is a `widget_from_pipe` finding and not
also a `violation`. `violation`, `no_lock_share` and `discipline_gap` ignore
accesses in `gui_init`, `gui_cleanup` and `gui_reset` — those run once, before
or after anything else can reach the field. The other two do not need the
exception: `widget_from_pipe` only fires on a `pipe` or `either` function, and
those three are labelled `gtk`, while `pointer_share` asks about the field's
type and threads rather than about any one access.

`widget_from_pipe` is the narrowest of the five: on the tree this was written
against it returns five fields across every module. One is a real defect
([#21915](https://github.com/darktable-org/darktable/issues/21915)); three are
`exposure`'s bauhaus sliders, read by `_exposure_proxy_handle_event()` through
`dt_bauhaus_slider_get()` from a `dev->proxy` accessor another module calls on a
thread of its own (two of the three are named by
[#21974](https://github.com/darktable-org/darktable/issues/21974)); the last, `colorharmonizer`'s `auto_detect`, is a false
positive — see [the widget hand-off](#before-you-file-anything) below. Five hits
say nothing about how often the rule is right, only that it asks for little.

Rule by rule, over all of `src/iop` on that tree. The second column counts
findings that name a field an already-confirmed defect report also names; the
rest are unreviewed candidates, not refutations:

| rule | findings | names a confirmed defect's field |
| --- | --- | --- |
| `widget_from_pipe` | 5 | 3 |
| `violation` | 34 | 16 |
| `no_lock_share` | 36 | 25 |
| `discipline_gap` | 59 | 1 |
| `pointer_share` | 1 | 1 |

`discipline_gap` is the one rule outside the default set, and that last row is
why. It asks the least of the
five: a lock somewhere, an unlocked access anywhere, and — unlike `violation` —
no requirement that the two threads ever meet on the field. Most of what comes back is a field locked for one reason and
read unlocked for another, which is not a defect. It is worth turning on when
you are auditing a single module by hand:

```sh
./dt-lockcheck.py --module toneequal --rules discipline_gap
```

and not worth reading across the whole tree.

`pointer_share` is the opposite case, and it is in the default set despite
proving less than the other three. Every one of its inputs is a *correctly
locked* field. What it says is that the lock cannot be enough on its own — the critical
section protects the pointer load, not the object, and the other thread may
free that object while the reader is still using it. `dev-doc/GUI_Threading.md`
puts it as "a scalar hands over cleanly; an allocation does not". Proving the
escape needs dataflow inside a function, which is out of reach here (see
KNOWN LIMITS in the script), so the rule fires on the shape and every hit has
to be read before it is believed.

It is cheap to read, because the shape is rare. On the tree this was written
against, `gui_data` holds 11 non-widget pointer fields touched from both
threads, and the other three default rules already report 10 of them. The
eleventh is `colorreconstruction`'s `can`, the frozen bilateral grid: copied
out of `gui_data` under `gui_lock` at `colorreconstruction.c:634`, dereferenced
at `:641` after the lock is released, and freed by the preview pipe at `:659`.
Every access is under the lock, so the other four rules cannot see it, and it
is a confirmed defect
([#22060](https://github.com/darktable-org/darktable/issues/22060)).

```sh
./dt-lockcheck.py --rules pointer_share
```

One finding tree-wide is the whole point, and the rule cannot grow far past it:
its output is bounded by the shared-pointer population, which is 11 fields on a
91-module tree. Even in the worst case — every open finding in this tool
"fixed" by adding locks, which is the wrong fix for a pointer — it returns 10.
A bounded, readable list is what keeps it in the default set; if it ever stops
being one, move it out beside `discipline_gap`.

What it is really guarding is that fix path. Lock every access to
`zonesystem`'s `in_preview_buffer`, `colormapping`'s `buffer`, `ashift`'s `buf`,
`exposure`'s `deflicker_histogram`, `channelmixerrgb`'s `checker` or
`toneequal`'s `full_preview_buf`, and every other rule here goes quiet on a
field that is still use-after-free shaped. This one does not.


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

A finding whose [false-positive entry](#the-false-positive-list) has gone stale
carries three extra lines — the judgement that was recorded, and the two
commands that act on it:

```
colorharmonizer.c  g->auto_detect   (GtkWidget *)
   unlocked: _update_histogram:306[pipe]
   was a known false positive, confirmed 2026-08-30:
     _update_histogram() runs on the pipe but never calls GTK: ...
   re-check: ./dt-lockcheck.py --module colorharmonizer --recheck
   if still not a defect: ./dt-lockcheck.py --confirm-false-positive colorharmonizer:auto_detect
```

A trailing `N findings.` line closes the report, counting every rule asked for,
and naming how many findings the false-positive list suppressed.

### csv

A header row, then one row per finding — the same findings as the report, in
the same order, with nothing capped:

| column | contents |
| --- | --- |
| `module` | the module name, i.e. the source file without its extension |
| `field` | the `gui_data` field, without the `g->` |
| `ctype` | the field's type as declared in the struct, `*` and all |
| `rule` | which of [the rules](#the-rules) fired |
| `locked_sites` | every locked site, space-separated, each `function:line[thread]` |
| `unlocked_sites` | every unlocked site, same shape |
| `fp` | `stale` when the finding has a [false-positive entry](#the-false-positive-list) the source has moved past, empty otherwise |

`fp` is appended rather than inserted, so a consumer reading the first six
columns by position is unaffected by its arrival. Suppressed findings are
absent from the csv exactly as they are from the report.

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
+ gtk_type("GtkWidget *")
+ ptr_type("GtkWidget *")
+ ftype("basicadj", "call_auto_exposure", "int")
+ runs_on("basicadj", "process", "pipe")
+ access("basicadj", "call_auto_exposure", "process", "gui_lock", "w")
```

| predicate | meaning |
| --- | --- |
| `gtk_type(T)` | `T` is a widget type, i.e. one whose name starts `Gtk` or `Dtgtk`. Only the types actually seen in some `gui_data` struct are emitted; this is what `is_widget` asks |
| `ptr_type(T)` | `T` is a pointer type, i.e. one written with a `*`. Emitted on the same terms, and what `is_pointer` asks |
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

**A round starts with `--recheck`**, which reports only the findings whose
recorded judgement the source has moved past. It is usually empty, and when it
is not, those findings are the ones with the most behind them: somebody read
that exact code and concluded it was safe, and it has since changed. Read them
before the ones nobody has ever ruled on.

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

Findings are **candidates**. On the tree this was developed against, 52 of the
76 default-settings findings name a field from a defect report that already
existed (76 is the count before suppression: two of them are recorded in the
[false-positive list](#the-false-positive-list), so a default run reports 74), measured against the 23 reports of the `gui_data` audit (#21915-#21919,
#21974, #22005-#22009, #22057-#22069); the other 24 have not been ruled on
either way, and a spot-check of a dozen of them found a clear majority to be
real defects of the same shape. No one has measured a false-positive rate, and
`discipline_gap` is markedly worse than the four it is left out beside.
`pointer_share` returns a single field on this tree and that field is a
confirmed defect, but one hit is not a precision measurement — read it like the
rest. Please confirm a finding by reading the code before opening an issue.

Five false-positive causes are worth knowing, because you will meet them:

- **Exclusion by protocol rather than by lock.** `rgblevels` hands work to the
  pipe through a state machine (`0` idle, `1` requested, `-1` running, `2`
  ready), and the transitions are taken under `gui_lock` even where the accesses
  they guard are not. `g->params` is genuinely protected that way: only the
  thread that took the state to `-1` touches it. The script cannot see this.
  It does **not** cover the whole module, though — `box_cood`, `channel` and
  `call_auto_levels` are all written from the GTK side without checking the
  state, so their findings are not excluded by the protocol and are not false
  positives on this ground.
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
  pipe and reports it. This one and `rgblevels`' `g->params` are the two
  entries the [false-positive list](#the-false-positive-list) ships with, so a
  default run no longer shows them.
- **A finding that rests entirely on an `either` site.** `either` counts on both
  sides of the cross-thread test because it *can* run on either thread — but on
  the path that matters it may always run on one. Check the proof tree, or the
  `[either]` labels in the report, before you trust such a finding.

And the limits:

- Lock depth is tracked lexically, so an early `leave_critical_section()` on one
  branch makes later code on other branches look unlocked.
- Thread inference is name-based, plus call-graph propagation *within one file*
  and the [address-taken rules](#which-thread-is-this-on) above. What it still
  cannot label comes out as thread `?`; such a site counts on neither side of
  the cross-thread test, so it can never be what makes a field shared.
- **Sharing means pipe-versus-GTK only.** `gui_data` is also the channel one
  pipe uses to hand a value to another — the preview pipe computes a
  whole-image property and the full pipe reads it. That needs the same lock
  (see *Passing Values Between Pipes Through `gui_data`* in
  `dev-doc/GUI_Threading.md`, arriving with the pending dev-doc PR), and no rule here looks
  for it: a field both pipes touch and the GTK thread never does is invisible
  to this tool.
- Coverage is `src/iop/*.c` and `*.cc` only. Shared code under `src/develop/`
  and `src/libs/` is not analysed. Widening the glob would not help: across all
  of `src`, a `*_gui_data_t` struct exists in 91 `src/iop` sources and 4 others
  (`imageio/format/{jxl,jpeg,webp}.c`, `imageio/storage/piwigo.c`), and those
  four hand their GUI values to the worker through a params snapshot
  (`write_image()` takes a `dt_imageio_module_data_t *` and never reaches
  `gui_data`), which is the separation this tool exists to recommend.
  `src/libs` uses a different convention entirely — `dt_lib_<name>_t *d =
  self->data` — and has no IOP callbacks for the thread labels to key off.
  Covering `src/develop/preview_data.c`, the one file outside `src/iop` that
  takes the same `gui_lock`, would need a different extractor: it has no
  `gui_data` struct.
- `g` is matched by convention, not by type. A function that binds `g` to
  something that is not the module's GUI data — a gain map, a gaussian handle —
  is skipped entirely, so any real `gui_data` access in it is missed too.
- Defects needing dataflow inside a function are out of reach — notably "pointer
  copied under the lock, then dereferenced after releasing it", which is a real
  and recurring shape here. Do not read a clean run as an all-clear.


## The false-positive list

`false-positives.json`, next to the script, records the findings a human has
read and judged **not** to be defects. Suppression is on by default: a finding
with a matching entry is not reported, so a later round does not re-read what an
earlier one already decided.

The risk in any such list is that a suppression outlives the code it was true
about, and silently hides a defect that grew where a false positive used to be.
So every entry carries a **key** over the finding, and a source change that
could alter the judgement invalidates it: the entry goes *stale*, the finding
comes back, and the reason recorded for it is printed alongside.

Whenever the list touches a run, the stderr banner says how many findings it
suppressed, how many entries went stale and which are orphaned. `-q` does not
silence that line — the list may not hide its own size.

### The three states

An entry is identified by `(module, field, rule)`, which is not what the key is
computed over. Three cases follow:

| state | what it means | what the tool does |
| --- | --- | --- |
| **current** | entry matches a finding, keys equal | suppress the finding |
| **stale** | entry matches a finding, keys differ | report the finding, with the recorded reason and the command to re-confirm it |
| **orphan** | entry matches no finding at all | name it in the banner; the code moved past it and the entry should be deleted |

The rule is part of the identity on purpose. A field that moves from one rule
to another is a *new* finding and reads as one, rather than inheriting a
judgement passed under a rule it no longer matches.

Orphans are counted against the run's own scope: `--module` and `--rules`
narrow what the tool looked for, and an entry the run never went looking for is
not an orphan.

### What the key covers

Two halves, because neither is safe alone:

- the **facts** — the field's type, the thread of every function that touches
  it, and the `(function, lock, mode)` of every access. This is exactly the
  [`--format lemmalog`](#lemmalog) view, with no line numbers, so moving code
  around or renaming a local does not fire it;
- the **text of every access line**, verbatim from the source.

Each catches what the other misses. Replacing colorharmonizer's
`gdk_threads_add_idle()` with a direct GTK call, or dropping its `g_object_ref`,
turns that entry's false positive into a real defect while leaving every fact
identical — same function, same lock, same mode — so a facts-only key would
carry the suppression silently forward. The converse is a critical section
removed from around an access: the facts show it, the access line's own text
does not.

This was measured rather than guessed, over 6105 finding-transitions across 80
commits touching `src/iop`, plus seven targeted edits against a real entry:

| candidate key | fires | missed a rule change | missed a site change |
| --- | --- | --- | --- |
| whole-file hash | 9.6% | 0 | 0 |
| facts only | 0.16% | 0 | 7 |
| `function:line` sites | 5.6% | 0 | 2 |
| **facts + access-line text** | **0.28%** | **0** | **0** |

The file hash is not "overkill but safe": it is 34× noisier for no extra
safety, and noise is what makes people re-confirm an entry without reading it.

### Confirming an entry

Nobody edits the file by hand:

```sh
./dt-lockcheck.py --confirm-false-positive rgblevels:params --reason "why this is not a defect"
./dt-lockcheck.py --confirm-false-positive rgblevels          # every stale entry in the module
```

`MODULE:FIELD` names one finding. A bare `MODULE` re-confirms every stale entry
in that module, which is what a one-sitting audit of one module produces — but
it deliberately **cannot add** entries: adding one is a judgement about a single
field and has to name it, or a whole module could be silenced with one word.

A new entry requires `--reason`, because that sentence is what gets printed back
when the entry goes stale, and an entry without one degenerates into a blanket
suppression nobody can re-judge. Re-confirming keeps the recorded reason unless
you pass a new one. Either way the tool rewrites the key, the site list and the
confirmation date, prints what it touched, and exits without reporting.

### Reading and overriding it

```sh
./dt-lockcheck.py --recheck                     # only the findings whose entry went stale
./dt-lockcheck.py --ignore-false-positives      # the full check, list ignored
```

`--recheck` composes with `--module` and `--format`. It is the "what do I have
to look at again?" run: on a clean tree it prints nothing, and it is the one to
pair with `--fail-on-findings` if you want a check that only complains about
judgements the source has moved past.

Each entry also carries its site list as `function:line[thread](lock)`, purely
so a diff of the file is legible. Those line numbers are informational and are
**not** part of the key.

### What it is not

It is not a baseline of every finding, and `--fail-on-findings` is not a
new-findings gate. The flag exits 1 when anything is reported, which on this
tree is red today: most of the 74 unsuppressed findings are unreviewed or filed
and unfixed, not false positives. It becomes usable when every finding is
either fixed or recorded here, which is why it is off by default and not wired
into CI. A gate that fails only on findings that are *new* would need a
checked-in baseline of all of them, a different artifact with different
invalidation needs; that does not exist here.

Nor does an entry mean much on its own. It records that one person read one
finding at one commit and reached a conclusion. The key is what makes that
conclusion expire; it cannot make it right.


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
