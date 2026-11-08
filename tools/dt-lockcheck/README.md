# dt-lockcheck

dt-lockcheck finds `gui_data` fields that an IOP module shares between the
pixelpipe and the GTK main thread without locking them consistently.

Each IOP module keeps its per-instance GUI state in a
`dt_iop_<module>_gui_data_t` struct, reached as `g->field`. `gui_*` callbacks
and widget handlers run on the GTK main thread. The pipeline callbacks run on
other threads, and sometimes on the main thread too, as
[Which thread is this on?](#which-thread-is-this-on) explains. Code must access
a field that both sides touch inside `dt_iop_gui_enter/leave_critical_section()`
(the module's `gui_lock`), or under a private mutex. The script reports the
places where that does not happen, or does not happen everywhere. The
[`pointer_share`](#the-rules) rule also reports shared pointer fields where
locking every access is still not enough.

The tool is a lexical analysis, not a race detector. See
[Before you file anything](#before-you-file-anything).


## Requirements

Python 3.8 or newer, standard library only. There is nothing to install or
build.

[lemmalog](https://github.com/JordyZomer/lemmalog) is optional. Only `--why`
needs it, to print the [proof tree](#proof-trees) behind each finding.

[`dt-lockcheck-diff.py`](#comparing-two-trees), which compares two trees, also
needs `git` and `tar` on `$PATH`.


## Usage

Run the script from anywhere inside a darktable checkout, such as this tool's
own directory, the project root or a build directory, or put it on your
`$PATH`. It finds `src/iop` by itself:

```sh
./dt-lockcheck.py
```

This runs the default rules over all of `src/iop` and leaves out the findings
that somebody has already read and judged not to be defects, which the
[false-positive list](#the-false-positive-list) beside the script records.
Before the findings, the script prints the sources it found to stderr:

```
dt-lockcheck: 97 sources in /home/you/darktable/src/iop (darktable root above $PWD); 87 with a gui_data struct, 10 skipped
```

The tool analyzes only the sources with a `dt_iop_*_gui_data_t`, because the
rest have no GUI state to share. `-q` drops the line. If you keep more than one
checkout, read it, because it shows when the tool picked the wrong one.

### Options

The tables list every flag, grouped by purpose.

**Which tree, and what goes to stderr:**

| flag | what it does |
| --- | --- |
| `--src PATH` | the tree to analyze, either `src/iop` or a checkout root. Without it, the script searches, as described below |
| `-q`, `--quiet` | drop the source-directory line from stderr. The [false-positive](#the-false-positive-list) banner still prints |

**What to report on:**

| flag | what it does |
| --- | --- |
| `--module NAME` | restrict the run to one module. Repeatable. A name that matches no source, or a source with no `gui_data` struct, fails the whole run, so a CI job pinned to a module list notices a rename. It does not make the run faster, because the script analyzes every source and then filters |
| `--rules LIST` | which of [the rules](#the-rules) to report, comma-separated, or `ALL` for every rule, including the noisy one left out by default |
| `--include-false-positives` | also report the findings that the false-positive list suppresses, without their recorded reasons. `--format report` or `csv` only, and not with `--fail-on-stale-false-positives-only` or `--confirm-false-positive`, which need the list applied |

**How to report it:**

| flag | what it does |
| --- | --- |
| `--format FMT` | `report`, `csv`, `json` or `lemmalog`. See [Output formats](#output-formats) |
| `--why` | append the [proof tree](#proof-trees) behind each finding. Needs lemmalog, and `--format report` |
| `--lemmalog PATH` | the lemmalog binary, when it is not on `$PATH`. Implies `--why` |

**Maintaining the [false-positive list](#the-false-positive-list):**

| flag | what it does |
| --- | --- |
| `--confirm-false-positive MODULE[:FIELD]` | record a judgement, or re-key a stale entry. Repeatable. The script rewrites the list and exits without reporting, so it cannot be combined with another `--format`, `--why` or a gate |
| `--false-positive-reason TEXT` | the judgement to record. Required for a new entry, optional when re-confirming. Every confirmation in the run has to name the same module |

**For an automated build** (see [Exit status](#exit-status)):

| flag | what it does |
| --- | --- |
| `--fail-on-findings` | exit 1 when anything is reported. This wide gate fails until every finding is fixed or recorded as a false positive |
| `--fail-on-stale-false-positives-only` | exit 1 only when a recorded false positive has gone stale, and report only those entries. This narrow gate can pass while findings are still unreviewed |

Two environment variables stand in for flags. They are for CI, where a path
does not belong in the invocation:

| variable | equivalent to |
| --- | --- |
| `$DT_LOCKCHECK_SRC` | `--src` |
| `$DT_LOCKCHECK_LEMMALOG` | `--lemmalog` |

To analyze a tree other than the one you are in:

```sh
./dt-lockcheck.py --src /path/to/darktable/src/iop
./dt-lockcheck.py --src /path/to/darktable          # a checkout root works too
```

The script looks for `src/iop` in this order: `--src`, then
`$DT_LOCKCHECK_SRC`, then a walk up from the current directory, then a walk up
from the script's own location. The current directory comes before the script
location on purpose. If you symlink the script onto `$PATH` from one checkout
and run it inside another, it analyzes the one you are in. An explicit `--src`
never falls back to the search, so a typo fails with an error.



## Exit status

| code | meaning |
| --- | --- |
| 0 | the run completed, whatever it found |
| 1 | a `--fail-on-*` gate was given and it fired |
| 2 | the run did not complete, and stderr says why: a wrong invocation, such as an unknown rule name or a `--module` that names no source, a `src/iop` that cannot be found, a malformed `false-positives.json` in a run that reads it, or a `--why` that finds no lemmalog binary |

A CI job that only checks for a non-zero status cannot tell a gate that fired
from a run that did not complete, so check for 1 and 2 separately.

There are two gates, and a run may ask for at most one of them:

- **`--fail-on-findings`** exits 1 when anything is reported. It stays red
  while any finding survives the
  [false-positive list](#the-false-positive-list), whether that finding is
  unreviewed, filed and unfixed, or wrong. Without
  `--include-false-positives`, it passes once every finding is either fixed or
  recorded as a false positive, and only then is it useful in CI.
- **`--fail-on-stale-false-positives-only`** exits 1 only when a recorded false
  positive has gone stale, which means that the facts or the access lines it
  was judged on have changed. It passes as long as the list is kept up to date,
  so a CI job or a pre-commit hook can use it before every finding has been
  reviewed. It says nothing about findings that nobody has judged. It reports
  only the stale entries, in `report` and in `csv`, because those are what it
  fired on.

Asking for both gates is an error. One exit status cannot carry two answers,
and the wide gate already fails on everything the narrow one does.

Either gate with `--format json` or `lemmalog` is an error too. Those formats
carry the facts the rules run on, not findings, so the gate would have nothing
to fire on and the run would exit 0 regardless. A CI job set up that way would
pass without ever running the check. A gate needs `--format report` or `csv`.

`--fail-on-findings` fails on what the run reported. With
`--include-false-positives` the run also reports the findings that the list
would suppress, so a finding recorded as a false positive still fails the
gate.

A gate that fails only on the findings a change adds compares two trees, so it
is a separate script, `dt-lockcheck-diff.py`. See
[Comparing two trees](#comparing-two-trees).


## Which thread is this on?

The tool labels every function it analyzes, and every site it reports, with
one of three threads, or `?` when it cannot tell. Most of this section explains
`either`.

| label | what it means |
| --- | --- |
| `gtk` | the GTK main thread: `gui_init`, `gui_update`, `gui_changed`, widget and draw callbacks, mouse and scroll handlers |
| `pipe` | a pixelpipe worker thread, never the main thread, for the instance that owns `gui_data`: `process`, `process_cl`, `process_tiling*`, `modify_roi_in/out`, `tiling_callback`, `distort_mask`, `output_format` |
| `either` | driven by the pipe, but GTK-thread code reaches it too, so it has no thread you can rely on |

`either` covers `commit_params`, `init_pipe`, `cleanup_pipe`,
`distort_transform`, `distort_backtransform`, `blend_colorspace` and
`default_colorspace`. They look pipe-only, but GTK-thread code calls them too:

- `basecurve`'s `init_pipe()` calls `commit_params()` directly, and switching
  image rebuilds the screen pipes from an idle callback,
  `_dev_load_requested_image()` in `src/views/darkroom.c`. That callback runs
  in the GTK main loop, so both functions run there.
- The color picker calls `default_colorspace()` while it builds a picker button
  (`_init_picker()` in `src/gui/color_picker_proxy.c`), with `NULL` for both
  `pipe` and `piece`. The blend GUI calls `blend_colorspace()` the same way.
- Mask handling and darkroom zoom call `distort_transform()` and
  `distort_backtransform()` directly.

An `either` site counts on both sides of the cross-thread test, because two
calls of the same callback can run on different threads. When a finding rests
on such a site, check it before you accept it. On the path you care about,
both sites may always run on the same thread.

Each label also covers the static helpers reached from the labeled function.
The tool propagates labels along the call graph within one file. This matters
because the offending line is often two or three calls down, in a function
whose name says nothing about which thread reached it.

The tool labels callbacks by how their address is taken, not by their name.
That covers handlers that no naming convention describes:

| how the function is handed over | label |
| --- | --- |
| passed to a callee as an argument | the registrar's thread, because the callee calls it synchronously |
| passed to `g_idle_add`, `g_signal_connect` and similar | `gtk`, whatever thread registered it |
| stored in a struct member or a file-scope table | `either`, because it escapes the file and the call site is unknown |

The first row covers darktable's own helpers. `dt_gui_connect_motion()` and the
bauhaus quad setters register handlers without any of the spellings a keyword
list would look for. The third row gives the `dev->proxy` accessors their
label. Another module calls them, and that module cannot take your `gui_lock`.
`dev-doc/GUI_Threading.md` covers this under
*Publishing `gui_data` Through a Proxy*.

Propagation never changes the label of a module API callback, such as
`process()` or `gui_update()`, because the API fixes its thread.


## The rules

The table lists the rules in the order the report uses, most worth reading
first. A field is reported under at most one rule, the first in this order that
matches, so a widget reached from the pipe is a `widget_from_pipe` finding and
not also a `violation`.

| rule | default | what it checks |
| --- | --- | --- |
| `widget_from_pipe` | yes | a `Gtk*`- or `Dtgtk*`-typed field touched from a `pipe` or `either` function, locked or not. Only the main thread may call GTK, so the lock does not matter |
| `violation` | yes | the field is shared across the two threads and locked somewhere, and some access to it is unlocked. The module's own code is the evidence that the lock is needed |
| `no_lock_share` | yes | the field is written and shared across the two threads, and the module never locks it |
| `discipline_gap` | no | the field is locked somewhere and accessed unlocked, but no cross-thread sharing was proven. Mostly noise, so read it last |
| `pointer_share` | yes | a pointer-typed field shared across the two threads that no rule above reported: every access is locked, or the field is never locked and only read. A lock on the pointer does not protect the object behind it, which the other thread may free while the reader uses it |

When the rules decide whether a field is shared, written or accessed unlocked,
they ignore `gui_init`, `gui_cleanup` and `gui_reset`, because those run before
or after anything else can reach the field. A field written only there counts
as never written.

`--rules` takes a comma-separated list of the names above, or `ALL`. `ALL` is
upper-case so that it cannot be mistaken for a rule name, and lower-case `all`
is an unknown rule. `ALL` means every rule the tool knows, including any added
later, so a CI job that wants a fixed set should list the names instead. Next
to names (`--rules ALL,violation`), `ALL` wins.

The last line of a report counts its findings, so
`./dt-lockcheck.py --rules NAME --include-false-positives` gives the count for
one rule on your tree.

For each `widget_from_pipe` hit, check how the pipe-side function is reached.
A `dev->proxy` accessor such as `exposure`'s `_exposure_proxy_handle_event()`
is labeled `either` because another module calls it, and only the callers
show whether any of them runs on a pipe thread.
[Before you file anything](#before-you-file-anything) describes the widget
hand-off, a pattern that is a false positive.

`discipline_gap` asks the least of the five: a lock somewhere and an unlocked
access anywhere, with no requirement that the two threads ever meet on the
field. On the tree the tool was developed against, it returned more findings
than any other rule, and only one of them named a field from a confirmed
defect report. Most of its findings are fields locked for one reason and read
unlocked for another, which is not a defect. It is not worth reading across
the whole tree, but it helps when you audit a single module by hand:

```sh
./dt-lockcheck.py --module toneequal --rules discipline_gap
```

`pointer_share` is in the default set although it proves less than the other
default rules. The critical section protects the pointer load, not the object.
`dev-doc/GUI_Threading.md` puts it as "a scalar hands over cleanly; an
allocation does not". Proving that the pointer escapes the lock needs dataflow
inside a function, which this tool cannot do, so the rule fires on the pattern
and you have to read every hit.

The rule was written for `colorreconstruction`'s `can`, the frozen bilateral
grid, filed as
[#22060](https://github.com/darktable-org/darktable/issues/22060). `process()`
copies the pointer out of `gui_data` under `gui_lock` and thaws the grid after
releasing the lock, while the preview pipe's own `process()` can free it and
store a new one. Every access is under the lock, so the other four rules
cannot see it. For the same reason, the rule catches the wrong fix for this
kind of defect: locking every access to a shared buffer pointer silences every
other rule, but not this one.

```sh
./dt-lockcheck.py --rules pointer_share
```

Its output stays short, because few `gui_data` fields are pointers shared
between the threads, and adding locks cannot push it past that number. If the
list stops being short enough to read, move the rule out of the default set,
next to `discipline_gap`.


## Output formats

`--format` chooses what goes to stdout. All four formats describe the same run
and differ in how much of it they keep:

| format | what it contains |
| --- | --- |
| `report` | the default: findings grouped by rule, for reading, with capped site lists |
| `csv` | one row per finding, with every site, for a spreadsheet or a diff between runs |
| `json` | the raw extracted facts, before any rule ran |
| `lemmalog` | the same facts as Datalog assertions, for the engine |

`report` and `csv` carry findings, so `--rules` and the false-positive list
apply to them. `json` and `lemmalog` carry the facts the rules run on, so
neither changes them. Filter or extend those on your side instead. `--src` and
`--module` narrow all four. Only `report` can carry
[proof trees](#proof-trees).

The status banner, the lemmalog notices and every warning go to stderr, so you
can redirect stdout whatever the format.

### report

```
=== violation  (34) ===========================================

basicadj.c  g->call_auto_exposure   (int)
   locked  : process_cl:1302[pipe](gui_lock), process_cl:1304[pipe](gui_lock), process_cl:1335[pipe](gui_lock), process:1420[pipe](gui_lock), process:1422[pipe](gui_lock), process:1434[pipe](gui_lock), ... (+5 more)
   unlocked: button_released:384[gtk], change_image:600[gtk]
```

`locked` lists the sites that establish the module's own convention. They are
the evidence that the field is meant to be protected. `unlocked` lists the
suspected defect. A finding with no site of one kind has no such line: a
`no_lock_share` finding has no `locked` line, and a `pointer_share` finding
usually has no `unlocked` line. Sites are `function:line[thread]`, where the
thread is [`pipe`, `gtk` or `either`](#which-thread-is-this-on), and a locked
site also names the lock. `pipe` and `either` sites come first, because the
half off the main thread is usually the interesting one. A `?` means that the
tool could not work out where the function runs.

The report prints at most six locked and eight unlocked sites per finding and
says how many it left out. `--format csv` writes every site.

Open the listed sites and decide whether the accesses can really run at the
same time. If they can, the usual fix is to extend the module's existing
critical section to the unlocked sites, not to add a new lock. For
`pointer_share`, decide instead whether the other thread can free the object
behind the pointer while a reader still uses it.

When a finding's [false-positive entry](#the-false-positive-list) has gone
stale, the report shows the recorded judgement and the command that
re-confirms it:

```
exposure.c  g->black   (GtkWidget *)
   unlocked: _exposure_proxy_handle_event:880[either], _exposure_set_black:830[gtk], gui_changed:1052[gtk]
   was a known false positive, confirmed 2026-09-01:
     The widget accesses that the rule reports are in _exposure_proxy_handle_event(). ...
   re-read the source; if it is still not a defect:
     ./dt-lockcheck.py --confirm-false-positive exposure:black
```

The last line counts the findings of every rule asked for, and how many the
false-positive list suppressed.

### csv

A header row, then one row per finding. These are the same findings as in the
report, in the same order, with nothing capped:

| column | contents |
| --- | --- |
| `module` | the module name, which is the source file name without its extension |
| `field` | the `gui_data` field, without the `g->` |
| `ctype` | the field's type as declared in the struct, including any `*` |
| `rule` | which of [the rules](#the-rules) fired |
| `locked_sites` | every locked site, space-separated, each `function:line[thread]` |
| `unlocked_sites` | every unlocked site, in the same form |
| `fp` | `stale` when the finding has a [false-positive entry](#the-false-positive-list) that the source has moved past, empty otherwise |

`fp` comes last so that a consumer that reads the first six columns by
position still works. The csv lacks the name of the lock at each locked site
and the proof trees. `json` has the lock names.

### json

The whole extracted fact base, one object per analyzed module, before any rule
ran:

```json
{
 "basicadj": {
  "module": "basicadj",
  "src": "basicadj.c",
  "fields": { "call_auto_exposure": "int", "bt_auto_levels": "GtkWidget *" },
  "accesses": [ ["process", "call_auto_exposure", "gui_lock", 1420, "w"] ],
  "thread": { "process": "pipe", "button_released": "gtk", "flags": null }
 }
}
```

| key | contents |
| --- | --- |
| `module` | the module name, repeated from the key |
| `src` | the basename of the analyzed source, with its extension, which is `.cc` for the modules written in C++ |
| `fields` | every field of the `gui_data` struct, mapped to its declared type. A nested anonymous struct appears once, under its own member name, with type `struct` |
| `accesses` | one entry per `g->field` mention, `[function, field, lock, line, mode]` |
| `thread` | every function in the file, including ones that never touch `gui_data`, mapped to its inferred thread |

In `accesses`, `lock` is the innermost lock held at that line:

| `lock` | meaning |
| --- | --- |
| `none` | nothing is held. These are the sites a finding accuses |
| `gui_lock` | inside `dt_iop_gui_enter/leave_critical_section()` |
| `callee_lock` | the lock is not held here, but `&self->gui_lock` is passed to the callee, as in `dt_dev_sync_pixelpipe_hash()` |
| `bulk_init` | `gui_lock` is held, but in a function that writes eight or more fields under it and reads none of them back. That is bulk initialization, which establishes no per-field convention |
| anything else | the name of a private mutex field, such as `histogram_lock`, taken with `dt_pthread_mutex_lock(&g->histogram_lock)` |

`mode` is `w` for a write and `r` for a read. `line` is 1-based, in the file
that `src` names.

In `thread` the value is `pipe`, `gtk`, `either` or `null` when inference
failed. A `null` function counts on neither side of the cross-thread test, so
it can never be what makes a field shared. Its unlocked accesses are still
listed as sites, and that is where the report's `?` comes from.

Modules with no `gui_data` struct are absent, not present and empty. All
output is sorted, so two runs over the same tree diff cleanly.

### lemmalog

The same facts as `json`, written as lemmalog assertions, one per line, for
the Datalog rules in `rules.lemma`. [Proof trees](#proof-trees) shows how to
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
| `gtk_type(T)` | `T` is a widget type, one whose name starts with `Gtk` or `Dtgtk`. Only types that appear in some `gui_data` struct are emitted. `is_widget` asks this |
| `ptr_type(T)` | `T` is a pointer type, one written with a `*`. Emitted on the same terms. `is_pointer` asks this |
| `ftype(M, F, T)` | field `F` of module `M` is declared `T` |
| `runs_on(M, Fn, T)` | function `Fn` of module `M` runs on thread `T` |
| `access(M, F, Fn, Lock, Mode)` | `Fn` reads or writes `F` with `Lock` held |

There are two differences from `json`, both because the rules do not use the
information. A function whose thread could not be inferred gets no `runs_on`
fact at all, instead of a `null` one. `access` carries no line number, so all
of one function's accesses to one field, under one lock and in one mode, are a
single fact however many lines they span.


## Review the findings

Run the scan, then read the source at the sites the report names:

```sh
./dt-lockcheck.py                                  # the scan
./dt-lockcheck.py --module basicadj --why          # why does it believe this?
./dt-lockcheck.py --module basicadj --format json  # the facts behind it
```

1. Read the stale entries before the other findings. The stderr banner names
   them, and the report prints each one with its recorded reason. A stale entry
   means that somebody judged that exact code safe, and its facts or access
   lines have changed since. Usually there are none.
2. For each finding, open the sites it lists and decide whether it is real, as
   the [report](#report) format describes. The source settles that faster than
   another run. The causes that make a finding wrong are the ones the tool
   cannot see, such as exclusion by protocol, a lock passed into a callee, or a
   widget handed to the main loop.
   [Before you file anything](#before-you-file-anything) has the full list.

Use the other two commands only when the report leaves a question open:

- `--why` shows the facts behind a finding, such as the locked site that
  established a convention, and the thread inference that made the field
  shared, including whether it went through an
  [`either`](#which-thread-is-this-on) site. Use it when you doubt a thread
  label, for an `either` site or a static helper several calls below its
  callback, or when you want the evidence chain as the outline of a bug report.
  Narrow the run first, as [Proof trees](#proof-trees) explains.
- `--format json` gives the extracted facts with no rule applied. Use it to
  see every access to a field, including the ones no rule fired on, or the
  thread inferred for each function, including the ones where inference failed,
  or to run a filter or a rule of your own. Add `--module` to keep the output
  small. An agent driving this tool should consume this format.
  `--format lemmalog` gives the same facts in the form the Datalog rules use.

To see what a change does to the findings, use `dt-lockcheck-diff.py`, as
[Comparing two trees](#comparing-two-trees) describes.


## Before you file anything

Findings are candidates. On the tree the tool was developed against, about two
thirds of the default-rule findings named a field that a report from an
earlier `gui_data` audit also named. A spot-check of a dozen of the rest found
most of them to be real defects of the same kind. Nobody has measured a
false-positive rate, and `discipline_gap` is much worse than the four default
rules. Confirm a finding by reading the code before you open an issue.

Six causes of false positives come up often:

- **Exclusion by protocol instead of by lock.** `rgblevels` hands work to the
  pipe through a state machine (`0` idle, `1` requested, `-1` running, `2`
  ready). It changes state under `gui_lock`, even where the accesses that the
  states guard are not locked. That protects `g->params`, because only the
  thread that set the state to `-1` touches it. The script cannot see this.
  The protocol does not cover the whole module, though. The GTK side writes
  `box_cood`, `channel` and `call_auto_levels` without checking the state, so
  the protocol does not make their findings false positives.
- **A lock passed into a callee.** The tool handles the common
  `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)` pattern, but not
  the general case.
- **A field that only sits inside a critical section taken for a different
  field.** The tool handles part of this with the `bulk_init` lock state, which
  the [json](#json) section describes.
- **A widget touched from the pipe only to hand it to the main loop.** Taking
  `g_object_ref()` on the widget and passing it to `g_idle_add()` or
  `gdk_threads_add_idle()` is correct, because the ref is atomic and the GTK
  calls happen on the main thread. `widget_from_pipe` still reports it, because
  it sees a widget field touched from the pipe.
- **A finding that rests entirely on an `either` site.** On the path that
  matters, the site may always run on one thread. Check the proof tree, or the
  `[either]` labels in the report, before you trust such a finding.
- **A `pointer_share` hit on a field that is already handled correctly.**
  Holding the lock through the last use, copying the data instead of the
  pointer, and transferring ownership so that the publisher cannot free the
  object are all correct, and all leave a shared pointer field. The tool cannot
  tell them from the defect.

The tool also has these limits:

- It tracks lock depth lexically, so an early `leave_critical_section()` on one
  branch makes later code on other branches look unlocked.
- Thread inference uses function names, call-graph propagation within one
  file, and the [address-taken rules](#which-thread-is-this-on) above. A
  function it still cannot label gets thread `?`.
- Sharing means pipe versus GTK only. One pipe also uses `gui_data` to hand a
  value to another. For example, the preview pipe computes a whole-image
  property and the full pipe reads it. That needs the same lock (see
  *Passing Values Between Pipes Through `gui_data`* in
  `dev-doc/GUI_Threading.md`), and no rule here looks for it. The tool cannot
  see a field that both pipes touch and the GTK thread never does.
- It analyzes `src/iop/*.c` and `*.cc` only. A source that a module includes
  from a subdirectory, such as `hlreconstruct/opposed.c` in `highlights.c`, is
  not analyzed, so its `gui_data` accesses are missing from the module's facts.
- It does not analyze shared code under `src/develop/` and `src/libs/`, and
  widening the glob would not help. Outside `src/iop`, the `*_gui_data_t`
  structs belong to imageio format and storage modules. Those hand their GUI
  values to the worker through a params snapshot (`write_image()` takes a
  `dt_imageio_module_data_t *` and never reaches `gui_data`), which is the
  separation this tool recommends. `src/libs` uses a different convention,
  `dt_lib_<name>_t *d = self->data`, and has no IOP callbacks for the thread
  labels to use. Shared code that takes the same `gui_lock`, such as
  `src/develop/preview_data.c`, would need a different extractor, because it
  has no `gui_data` struct.
- It matches `g` by name, not by type. It skips a function that binds `g` to
  something other than the module's GUI data, such as a gain map or a gaussian
  handle, so it also misses any real `gui_data` access in that function.
- A nested anonymous struct is one field. `globaltonemap`'s
  `struct { GtkWidget *bias; ... } drago;` is the field `drago` of type
  `struct`, so `g->drago.bias` does not reach `widget_from_pipe`.
- It finds function boundaries by counting braces from an opening `{` in
  column zero. darktable uses that style throughout, but C in general does not.
- It cannot find defects that need dataflow inside a function. One example is
  a pointer copied under the lock and dereferenced after the lock is released,
  which is a real and recurring pattern here. A clean run does not mean that
  there are no defects.


## The false-positive list

`false-positives.json`, next to the script, records the findings that a person
has read and judged not to be defects. Unless you pass
`--include-false-positives`, the script does not report a finding that has a
matching entry, so a later review does not re-read what an earlier one
decided.

The risk in such a list is that a suppression outlives the code it was true
about, and hides a defect that later appears in the same place. So every entry
carries a key computed over the finding's facts and the text of its access
lines, as [What the key covers](#what-the-key-covers) explains. A change to
either makes the entry stale, and the finding comes back with the recorded
reason printed next to it. A change elsewhere does not, even when the judgement
depends on it, such as an edit to the state machine that makes an access safe.
When you change such code, re-read the entries that rely on it.

Whenever the list affects a run, the stderr banner says how many findings it
suppressed and names the stale and the orphaned entries. `-q` does not silence
that line, so the number of suppressed findings is always visible.

### The three states

The script identifies an entry by `(module, field, rule)`, and compares the key
separately, so an entry is in one of three states:

| state | what it means | what the tool does |
| --- | --- | --- |
| current | the entry matches a finding, and the keys are equal | suppress the finding |
| stale | the entry matches a finding, and the keys differ | report the finding, with the recorded reason and the command to re-confirm it |
| orphan | the entry matches no finding | name it in the banner. The code has moved past it, and the entry should be deleted |

The rule is part of the identity on purpose. A field that moves from one rule
to another is a new finding and shows up as one, instead of inheriting a
judgement made under a rule it no longer matches.

Orphans are counted within the run's own scope. `--module` and `--rules`
narrow what the tool looked for, and an entry the run did not look for is not
an orphan.

### What the key covers

The key has two parts, because neither is safe alone:

- the facts: the field's type, the thread of every function that touches it,
  and the `(function, lock, mode)` of every access. This is the
  [`--format lemmalog`](#lemmalog) view, which has no line numbers, so moving
  code or renaming a local does not change it;
- the text of every access line, with the surrounding whitespace stripped.

Each part catches what the other misses. When the key was designed,
colorharmonizer's `_update_histogram()` handed its `auto_detect` button to the
main loop with `g_object_ref()` and `gdk_threads_add_idle()`, and that finding
was recorded as a false positive. 7b8a19f0f5 later removed the hand-off.
Replacing the idle call with a direct GTK call, or dropping the ref, would have
turned that false positive into a real defect and left every fact the same: the
same function, the same lock, the same mode. A key over the facts alone would
have kept suppressing it. The opposite case is a critical section removed from
around an access. The facts show that change, and the text of the access line
does not.

When the key was designed, four candidates were measured over 6105 finding
transitions across 80 commits that touch `src/iop`, plus seven targeted edits
to a real entry:

| candidate key | fires | missed a rule change | missed a site change |
| --- | --- | --- | --- |
| whole-file hash | 9.6% | 0 | 0 |
| facts only | 0.16% | 0 | 7 |
| `function:line` sites | 5.6% | 0 | 2 |
| facts + access-line text, the key in use | 0.28% | 0 | 0 |

The whole-file hash fires 34 times as often as the key in use and catches
nothing more. A key that fires often teaches people to re-confirm entries
without reading them.

### Confirming an entry

Add and re-confirm entries with the tool, not by hand, because the tool
computes the key:

```sh
./dt-lockcheck.py --confirm-false-positive rgblevels:params --false-positive-reason "why this is not a defect"
./dt-lockcheck.py --confirm-false-positive rgblevels          # every stale entry in the module
```

`MODULE:FIELD` names one finding. A bare `MODULE` re-confirms every stale entry
in that module, which is what an audit of one module in one sitting produces.
It cannot add entries, on purpose. Adding one is a judgement about a single
field and has to name it, otherwise one word could silence a whole module.
A confirmation looks at every rule, whatever `--rules` says, so it also finds
the `discipline_gap` entries that the default rules leave out.
The tool never deletes entries. Delete an orphan from the file by hand.

A new entry requires `--false-positive-reason`, because the tool prints that
sentence back when the entry goes stale. An entry without one would be a
blanket suppression that nobody can re-judge. Re-confirming keeps the recorded
reason unless you pass a new one. Either way the tool rewrites the key, the
site list and the confirmation date, prints what it changed, and exits without
reporting.

`--confirm-false-positive` is repeatable, and `--false-positive-reason` is not.
When a reason is given, every confirmation in the run has to name the same
module. Several fields of one module often share one argument, as
`rgblevels`' state machine does, but a sentence about one module cannot be true
of another. Without a reason there is no such limit, and each entry keeps the
reason it already has.

When a confirmation overwrites a recorded reason, the tool says so, because the
key cannot catch that mistake:

```
rgblevels:params: reason updated, key a447820dfb82 unchanged under no_lock_share (previous reason replaced)
```

Name functions in a reason, not line numbers. The report already prints
`function:line` for every site, and the key does not cover the reason. A line
number in the reason goes out of date with every unrelated edit above it, while
the entry stays current.

Each entry also carries its site list as `function:line[thread](lock)`, only
so that a diff of the file is readable. Those line numbers are not part of the
key.

### Reviewing the entries

```sh
./dt-lockcheck.py                                       # stale entries are marked in the report
./dt-lockcheck.py --module rgblevels --include-false-positives  # one module, suppressed findings too
./dt-lockcheck.py --fail-on-stale-false-positives-only  # exit 1, and report the stale entries alone
```

An ordinary run shows the stale entries among the other findings, each with its
recorded reason. `--include-false-positives` with `--module` puts the
suppressed findings back in the report, for when you review one module and want
to see every finding. Their recorded reasons are only in
`false-positives.json`. The gate in the third line narrows the report to the
stale entries, which helps when you want to know why it fired.

### What it is not

The list is not a baseline of every finding, and neither gate fails only on
new findings. `dt-lockcheck-diff.py` does that, by comparing a change with its
base. See [Comparing two trees](#comparing-two-trees).

An entry also proves little on its own. It records that one person read one
finding at one commit and reached a conclusion. The key makes that conclusion
expire when the code it covers changes. It cannot make the conclusion right.


## Comparing two trees

`dt-lockcheck-diff.py`, beside `dt-lockcheck.py`, reports what a change does to
the findings: which ones it adds, removes or alters. Nearly every finding on a
current tree predates any given change, so this is the view to read when you
review one. It is also the gate a CI job can use before every existing finding
has been reviewed, because the findings that predate the change cancel out.

```sh
./dt-lockcheck-diff.py master                  # the working tree against its merge base with master
./dt-lockcheck-diff.py --head HEAD HEAD^1      # one commit against its first parent
./dt-lockcheck-diff.py --fail-on-new-findings --github --head HEAD HEAD^1   # in a CI job
```

Run it inside the checkout under review, or name that checkout with `--repo`.
Like `git diff BASE...HEAD`, it compares the head with the merge base of `BASE`
and the head. The head is the working tree, untracked files included, unless
`--head` names a commit. The script extracts committed trees with
`git archive`, so it never changes the checkout. It runs dt-lockcheck on both
trees with every rule and prints the findings that differ.

Each finding is `new`, `gone` or `changed`, and lists only the sites whose
count changed, with the line numbers on each side. For example, one of the
four findings reported for 31b2dacaf0, the commit that added demosaic's
capture-sharpen auto-tune:

```
new      demosaic  g->new_radius  (float)  violation  [newly reported]
    locked    process[pipe]  0 -> 1   head: 775
    locked    process_cl[pipe]  0 -> 1   head: 1088
    locked    reload_defaults[gtk]  0 -> 1   head: 1573
    unlocked  _ui_pipe_done[gtk]  0 -> 2   head: 1766, 1770
    unlocked  gui_update[gtk]  0 -> 1   head: 1699
```

### How findings are matched

- By module and field. dt-lockcheck reports a field under one rule at most, so
  a field that moves to another rule is one `changed` finding, with a `rule:`
  line.
- Sites are compared as counts per function, lock state and thread, not by
  line number. Code that only moved is not a difference.
- One `dt-lockcheck.py` and its `false-positives.json` judge both trees. By
  default these are the copies beside the script, which in a CI job are the
  change's own. `--lockcheck` names another copy. A change to the rules or to
  the list therefore acts on both sides alike, and only the change to
  `src/iop` shows.

### Newly reported findings

A finding is *reported* on one side when an ordinary `./dt-lockcheck.py` run
would print it: its rule is a default rule, and no current false-positive
entry suppresses it. The report tags two kinds of difference:

| tag | meaning | fails `--fail-on-new-findings` |
| --- | --- | --- |
| `[newly reported]` | the head reports the finding and the base does not: a new finding, a field that moved into a default rule from `discipline_gap`, or a false-positive entry that the change made stale by editing the code it was judged on | yes |
| `[more unlocked accesses]` | the base already reports the finding, and the change adds unlocked accesses to it | no |

The second kind does not fail, because the change did not introduce the
finding. Everything else in the report is information: findings the change
removes, `discipline_gap` findings, added locked accesses, a changed field
type, and false-positive changes that leave the head clean.

### When it fires

A newly reported finding is a candidate, like any other. Read the source at
the sites the report names, as [Before you file anything](#before-you-file-anything)
describes. Then:

- if it is a defect, fix it in the change;
- if it is not, record why, and commit the updated list with the change:

  ```sh
  ./dt-lockcheck.py --confirm-false-positive MODULE:FIELD --false-positive-reason "why this is not a defect"
  ```

  The head then matches the entry, the comparison stops counting the finding,
  and the entry shows in the change's diff for the reviewer to check.

For a stale entry, read the reason recorded for it first:
`./dt-lockcheck.py --module MODULE` prints it beside the finding. If the
reason still holds for the edited code, re-confirm the entry with
`./dt-lockcheck.py --confirm-false-positive MODULE:FIELD`. Otherwise fix the
code. [Confirming an entry](#confirming-an-entry) has the details.

### In GitHub Actions

`--github` adds two things for a workflow:

- one annotation per tagged finding, on one of its access lines in the head,
  preferably an unlocked access the change added, so that it shows next to the
  code in the pull request. A newly reported
  finding is an error under `--fail-on-new-findings` and a warning otherwise.
  More unlocked accesses are always a warning. GitHub caps the number of
  annotations per step, so the script writes one per finding, not one per
  access;
- the report, under a one-line verdict, appended to the job summary when
  `$GITHUB_STEP_SUMMARY` is set.

A workflow triggered by `pull_request` checks out GitHub's merge of the pull
request into its target branch. The first parent of that merge commit is the
target branch, so `--head HEAD HEAD^1` compares exactly what merging would
change, and a checkout with `fetch-depth: 2` is enough. darktable's own job is
`.github/workflows/dt-lockcheck.yml`.

### Exit status of the comparison

| code | meaning |
| --- | --- |
| 0 | the comparison ran, and `--fail-on-new-findings` was not given or did not fire |
| 1 | `--fail-on-new-findings` fired |
| 2 | the comparison did not run: not inside a checkout, a ref that names no commit, no merge base, or dt-lockcheck failing on one of the trees |


## Proof trees

`rules.lemma` contains the same rules written as Datalog, for
[lemmalog](https://github.com/JordyZomer/lemmalog). With lemmalog on your
`$PATH`, `--why` prints the proof behind each finding in the report:

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

The tree ends at the extracted base facts, and that makes it useful. It names
the facts the finding rests on, such as the locked site that is the evidence
for a convention, and the thread inference that made the field cross-thread,
including whether that inference went through `may_pipe` or `may_gtk` on an
[`either`](#which-thread-is-this-on) site. You can use it as the outline of a
bug report. It also shows when a finding rests on a thread guess you disagree
with.

The `why     :` line names the goal. `no_lock_share` and `pointer_share` are
proven per field. `violation` and `discipline_gap` are proven per unlocked
access, and the tree explains the first unlocked site the report lists, which
is a `pipe` or `either` site when there is one. `widget_from_pipe` is proven
per `pipe` or `either` access, locked or not, and the tree explains the first
such site the report lists.

The Python code and the Datalog rules produce the same findings, so `--why`
does not change which findings are reported. It does change these things:

- The trees make a full report several times longer, so `--why` is off by
  default. Use it on a narrowed run, such as one `--module` or one rule you are
  about to write up.
- It runs lemmalog once for the whole report, over the facts that
  `--format lemmalog` writes, which roughly doubles the run time.
- It fails with exit code 2 when it cannot find a lemmalog binary, instead of
  printing a plain report. You asked for the trees, so a missing engine is a
  problem to fix, not one to hide. If lemmalog is found but the proof run
  fails, because `rules.lemma` is missing, the engine reports an error or it
  times out, the script prints a warning on stderr and the report without
  trees.
- `--lemmalog PATH` or `$DT_LOCKCHECK_LEMMALOG` names a binary that is not on
  `$PATH`. `--lemmalog` implies `--why`, because that is the only reason to
  name it. A path that does not exist is an error even if `lemmalog` is on
  `$PATH`, because an explicit path never falls back to the search.
- Only `--format report` carries the trees. `--why` with another format is an
  error, not a silent no-op. `csv`, `json` and `lemmalog` never start the
  engine, because those formats feed other tools, and their shape should not
  depend on whether an optional binary is installed. The script captures the
  engine's output, so it cannot reach stdout.

To drive the engine yourself, for example to add rules of your own or to query
it interactively, use `--format lemmalog` to write out the base facts:

```sh
( cat rules.lemma
  ./dt-lockcheck.py --format lemmalog
  echo run
  echo '? violation(M,F,Fn)'
) | lemmalog
```
