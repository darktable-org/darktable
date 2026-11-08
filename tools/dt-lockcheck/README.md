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


## Usage

Run the script from anywhere inside a darktable checkout, such as this tool's
own directory, the project root or a build directory, or put it on your
`$PATH`. It finds `src/iop` by itself:

```sh
./dt-lockcheck.py
```

That is the ordinary invocation. It runs the default rules over all of
`src/iop`, and suppresses the findings that somebody has already read and
judged not to be defects, using a [list](#the-false-positive-list) checked in
beside the script. Before it starts, it prints the sources it found:

```
dt-lockcheck: 97 sources in /home/you/darktable/src/iop (darktable root above $PWD); 87 with a gui_data struct, 10 skipped
```

The end of the line counts the sources that have a `dt_iop_*_gui_data_t` at
all. The rest have no GUI state to share, and the tool does not analyse them.
The line goes to stderr, so it does not mix with `--format csv` or `json`
output, and `-q` suppresses it. If you keep more than one checkout, read it,
because it shows when the tool picked the wrong one.

### Options

The tables list every flag, in five groups. The paragraphs after them give the
details that do not fit in a table row.

**Which tree, and what goes to stderr:**

| flag | what it does |
| --- | --- |
| `--src PATH` | the tree to analyse, either `src/iop` or a checkout root. Without it, the script searches, as described below |
| `-q`, `--quiet` | drop the source-directory line from stderr. The [false-positive](#the-false-positive-list) banner still prints |

**What to report on:**

| flag | what it does |
| --- | --- |
| `--module NAME` | restrict the run to one module. Repeatable, and every name has to resolve |
| `--rules LIST` | which of [the rules](#the-rules) to report, comma-separated, or `ALL` for every rule, including the noisy one left out by default |
| `--include-false-positives` | also report the known false positives, which a default run leaves out. `--format report` or `csv` only |

**How to report it:**

| flag | what it does |
| --- | --- |
| `--format FMT` | `report`, `csv`, `json` or `lemmalog`. See [Output formats](#output-formats) |
| `--why` | append the [proof tree](#proof-trees) behind each finding. Needs lemmalog, and `--format report` |
| `--lemmalog PATH` | the lemmalog binary, when it is not on `$PATH`. Implies `--why` |

**Maintaining the [false-positive list](#the-false-positive-list):**

| flag | what it does |
| --- | --- |
| `--confirm-false-positive MODULE[:FIELD]` | record a judgement, or re-key a stale entry. Repeatable. The script rewrites the list and exits without reporting |
| `--false-positive-reason TEXT` | the judgement to record. Required for a new entry, optional when re-confirming. Every confirmation in the run has to name the same module |

**For an automated build:**

| flag | what it does |
| --- | --- |
| `--fail-on-findings` | exit 1 when anything is reported. This wide gate fails until every finding is fixed or recorded as a false positive. See [Exit status](#exit-status) |
| `--fail-on-stale-false-positives-only` | exit 1 only when a recorded false positive has gone stale, and report only those entries. This narrow gate can pass while findings are still unreviewed |

Two environment variables each stand in for a flag. They are for CI, where a
path does not belong in the invocation:

| variable | equivalent to |
| --- | --- |
| `$DT_LOCKCHECK_SRC` | `--src` |
| `$DT_LOCKCHECK_LEMMALOG` | `--lemmalog` |

To analyse a tree other than the one you are in:

```sh
./dt-lockcheck.py --src /path/to/darktable/src/iop
./dt-lockcheck.py --src /path/to/darktable          # a checkout root works too
```

The script looks for `src/iop` in this order: `--src`, then
`$DT_LOCKCHECK_SRC`, then a walk up from the current directory, then a walk up
from the script's own location. The current directory comes before the script
location on purpose. If you symlink the script onto `$PATH` from one checkout
and run it inside another, it analyses the one you are in. An explicit `--src`
never falls back to the search, so a typo fails with an error.

`--module` is repeatable, and every name has to resolve. One bad name in a list
fails the whole run instead of analysing the rest, so a CI job pinned to a
module list notices when a module is renamed. The script checks options that
can only be wrong about the invocation before it reads anything. These are an
unknown `--rules` name, and `--why` or a `--fail-on-*` gate with a `--format`
that cannot carry it. They fail the same way whatever `--format` asked for.

`--why` adds the proof tree behind each finding, as described in
[Proof trees](#proof-trees). It is off by default and needs the
[lemmalog](https://github.com/JordyZomer/lemmalog) engine. `--lemmalog PATH`
points at a binary that is not on `$PATH`. `--format csv`, `json` and
`lemmalog` never carry a tree and never start the engine, so a script that
consumes them behaves the same whether or not lemmalog is installed.

`--format` chooses between the human-readable report, a csv of the findings,
and the raw extracted facts as json or as Datalog assertions.
[Output formats](#output-formats) describes what each one contains.

The section [The false-positive list](#the-false-positive-list) covers
`--include-false-positives` and `--confirm-false-positive`. It explains what
suppression covers and what makes an entry expire. The script consults the
list after the rules have run, so the list changes which findings are reported
and nothing else. `json` and `lemmalog` carry the facts the rules run on, so
the list does not affect them, for the same reason `--rules` does not.

`--fail-on-findings` and `--fail-on-stale-false-positives-only` are the two CI
gates. Both are off by default, and a run may ask for at most one.
[Exit status](#exit-status) explains what each one fails on, and why only the
second can pass while findings are still unreviewed.


## Exit status

| code | meaning |
| --- | --- |
| 0 | the run completed, whatever it found |
| 1 | a `--fail-on-*` gate was given and it fired |
| 2 | wrong invocation: `src/iop` could not be located, `--src` does not point at it, an unknown rule name, a `--module` that names no source or a source with no `gui_data` struct, `--why` or a `--fail-on-*` gate with a `--format` that cannot carry one, or a `--lemmalog` path that does not exist |

There are two gates, and a run may ask for at most one of them.

- **`--fail-on-findings`** exits 1 when anything is reported. It stays red
  while any finding survives the
  [false-positive list](#the-false-positive-list), whether that finding is
  unreviewed, filed and unfixed, or wrong. It passes once every finding is
  either fixed or recorded as judged. That is the condition for adding it to
  CI.
- **`--fail-on-stale-false-positives-only`** exits 1 only when a recorded false
  positive has gone stale, which means that a judgement no longer covers the
  code it was made about. It passes as long as the list is kept up to date, so
  a CI job or a pre-commit hook can use it before every finding has been
  reviewed. It says nothing about findings nobody has judged, which is why it
  can pass. It also reports only the stale entries, in `report` and in `csv`.
  People read a gate's output to find out why it fired, and the other findings
  are not what it fired on.

Asking for both gates is an error, not a conjunction. One exit status cannot
carry two answers, and the wide gate already fails on everything the narrow
one does.

Either gate with `--format json` or `lemmalog` is an error too, for the same
reason `--why` is. Those formats carry the facts the rules run on and return
before any finding exists, so the gate would have nothing to fire on and the
run would exit 0 regardless. A CI job set up that way would pass without ever
running the check, so the script rejects the combination up front, like every
other option that can only be wrong about the invocation. A gate needs
`--format report` or `csv`.

`--fail-on-findings` fails on what the run reported, so it honours
`--include-false-positives` and then counts the suppressed findings too. This
is the consistent reading of both flags. It also means that the combination
can never pass while the list has an entry, because a false positive is still
a finding, which the list suppresses but does not remove. Combine the two flags
only with an empty list.

A gate that fails only on new findings would be a third kind. It would need a
checked-in baseline of all findings, and none exists here.

A CI job needs to tell exit codes 1 and 2 apart. Code 2 means that nothing was
analysed, and a plain "non-zero" check would read it as a clean run that
turned bad.


## Which thread is this on?

The tool labels every function it analyses, and every site it reports, with
one of three threads. Most of this section explains `either`.

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
- The colour picker calls `default_colorspace()` while it builds a picker
  button (`_init_picker()` in `src/gui/color_picker_proxy.c`), with `NULL` for
  both `pipe` and `piece`. The blend GUI calls `blend_colorspace()` the same
  way.
- Mask handling and darkroom zoom call `distort_transform()` and
  `distort_backtransform()` directly.

An `either` site counts on both sides of the cross-thread test, because two
calls of the same callback can run on different threads. When a finding rests
on such a site, check it before you accept it. On the path you care about,
both sites may always run on the same thread.

Each label also covers the static helpers reached from the labelled function.
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

The labels in the tables above come from the module API, so propagation never
overwrites them.

## The rules

The table lists the rules in the order the report uses, most worth reading
first. All but `discipline_gap` run by default. Add
`--rules ...,discipline_gap` for that one, or `--rules ALL` for every rule.

| rule | default | what it checks |
| --- | --- | --- |
| `widget_from_pipe` | yes | a `Gtk*`- or `Dtgtk*`-typed field touched from a `pipe` or `either` function, locked or not. Only the main thread may call GTK, so the lock does not matter |
| `violation` | yes | the module locks the field somewhere and also accesses it unlocked from the other side, and the field is shared across the two threads. The module's own code is the evidence that the lock is needed |
| `no_lock_share` | yes | the field is written and shared across both threads, and the module never locks it |
| `discipline_gap` | no | the field is locked somewhere and accessed unlocked, but no cross-thread sharing was proven. Mostly noise, so read it last |
| `pointer_share` | yes | a pointer-typed field shared across the two threads, with no unlocked access anywhere. The lock protects the pointer load, not the object, which the other thread may free while the reader uses it |

`--rules` takes a comma-separated list of the names above, in any order, or
the single token `ALL` for every rule. `ALL` is upper-case on purpose. The rule
names are lower-case everywhere, here and in `rules.lemma`, so an upper-case
token cannot be mistaken for a rule name, and a lower-case `all` is an unknown
rule, not a second spelling. `ALL` also means every rule the tool knows about,
so a script that asks for it picks up a rule added later without being edited.
That is the reason to use it instead of listing the names, and the reason not
to use it in a CI job that wants a fixed set. It may appear alongside names
(`--rules ALL,violation`), and then it wins.

The report lists a field under at most one rule, the first in that order that
matches. A widget reached from the pipe is a `widget_from_pipe` finding and
not also a `violation`. `violation`, `no_lock_share` and `discipline_gap`
ignore accesses in `gui_init`, `gui_cleanup` and `gui_reset`, because those
run once, before or after anything else can reach the field. The other two
rules do not need the exception. `widget_from_pipe` only fires on a `pipe` or
`either` function, and those three are labelled `gtk`. `pointer_share` asks
about the field's type and threads, not about any one access.

`widget_from_pipe` is the narrowest of the five. Across all of `src/iop` it
returns only a few fields, which says nothing about how often it is right,
only that it asks for little. For each hit, check how the pipe-side function
is reached. A `dev->proxy` accessor such as `exposure`'s
`_exposure_proxy_handle_event()` is labelled `either` because another module
calls it. Only the callers in the source show whether any of them runs on a
pipe thread. See [the widget hand-off](#before-you-file-anything) below for a
pattern that is a false positive.

The last line of a report counts its findings, so
`./dt-lockcheck.py --rules NAME --include-false-positives` gives the count for
one rule on your tree.

`discipline_gap` is the one rule outside the default set. It asks the least of
the five: a lock somewhere, an unlocked access anywhere, and, unlike
`violation`, no requirement that the two threads ever meet on the field. On the
tree the tool was developed against, it returned more findings than any other
rule, and only one of them named a field from a confirmed defect report. Most
of its findings are fields locked for one reason and read unlocked for
another, which is not a defect. Turn it on when you audit a single module by
hand:

```sh
./dt-lockcheck.py --module toneequal --rules discipline_gap
```

It is not worth reading across the whole tree.

`pointer_share` is the opposite case. It proves less than the other three
default rules, but it is in the default set anyway. Every field it reports is
correctly locked. What it says is that the lock is not enough on its own. The
critical section protects the pointer load, not the object, and the other
thread may free that object while the reader still uses it.
`dev-doc/GUI_Threading.md` puts it as "a scalar hands over cleanly; an
allocation does not". Proving that the pointer escapes the lock needs dataflow
inside a function, which this tool cannot do (see KNOWN LIMITS in the script).
So the rule fires on the pattern, and you have to read every hit before you
believe it.

It has few hits to read, because the pattern is rare. Few non-widget pointer
fields in `gui_data` are touched from both threads, and the other default
rules already report most of them, because some access to them is unlocked.
The rule was written for `colorreconstruction`'s `can`, the frozen bilateral
grid, filed as
[#22060](https://github.com/darktable-org/darktable/issues/22060). The report
describes the pattern: `process()` copies the pointer out of `gui_data` under
`gui_lock` and thaws the grid after releasing the lock, while the preview
pipe's own `process()` can free it and store a new one. Every access in that
pattern is under the lock, so the other four rules cannot see it.

```sh
./dt-lockcheck.py --rules pointer_share
```

The rule's output cannot grow much. The number of shared pointer fields limits
it, and that number is small. Even if every open finding in this tool were
"fixed" by adding locks, which is the wrong fix for a pointer, the rule would
return no more than those fields. It stays in the default set because its list
is short enough to read. If that stops being true, move it out next to
`discipline_gap`.

The rule exists to catch that wrong fix. If someone locks every access to a
shared buffer pointer, every other rule here stops reporting a field that can
still be used after it is freed. This rule still reports it.


## Output formats

`--format` chooses what goes to stdout. All four formats describe the same run
and differ in how much of it they keep:

| format | what it contains |
| --- | --- |
| `report` | the default: findings grouped by rule, for reading, with capped site lists |
| `csv` | one row per finding, with every site, for a spreadsheet or a diff between runs |
| `json` | the raw extracted facts, before any rule ran |
| `lemmalog` | the same facts as Datalog assertions, for the engine |

`report` and `csv` carry findings, so both honour `--rules`. `json` and
`lemmalog` carry the facts the rules run on, which exist before any rule runs,
so `--rules` does not change them. Filter or extend them on your side instead.
`--src` and `--module` narrow all four. Only `report` can carry proof trees
([`--why`](#proof-trees)). Asking for them with any other format is an error,
not a silent no-op.

The status banner, the lemmalog notices and every warning go to stderr, so you
can redirect stdout whatever the format.

### report

```
=== violation  (34) ===========================================

basicadj.c  g->call_auto_exposure   (int)
   locked  : process_cl:1302[pipe](gui_lock), process_cl:1304[pipe](gui_lock),
             process_cl:1335[pipe](gui_lock), process:1420[pipe](gui_lock),
             process:1422[pipe](gui_lock), process:1434[pipe](gui_lock), ... (+5 more)
   unlocked: button_released:384[gtk], change_image:600[gtk]
```

`locked` lists the sites that establish the module's own convention. They are
the evidence that the field is meant to be protected. `unlocked` lists the
suspected defect. Sites are `function:line[thread]`, where the thread is
[`pipe`, `gtk` or `either`](#which-thread-is-this-on). `pipe` and `either`
sites come first, because the half off the main thread is usually the
interesting one. A `?` means that the tool could not work out where the
function runs.

The report prints at most six locked and eight unlocked sites per finding and
says how many it left out. `--format csv` writes every site, so use it when the
count matters.

Open both sets of sites and decide whether the two can really run at the same
time. If they can, the usual fix is to extend the existing critical section to
the unlocked sites, not to add a new lock.

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

A final `N findings.` line closes the report. It counts every rule asked for,
and says how many findings the false-positive list suppressed.

### csv

A header row, then one row per finding. These are the same findings as in the
report, in the same order, with nothing capped:

| column | contents |
| --- | --- |
| `module` | the module name, i.e. the source file without its extension |
| `field` | the `gui_data` field, without the `g->` |
| `ctype` | the field's type as declared in the struct, including any `*` |
| `rule` | which of [the rules](#the-rules) fired |
| `locked_sites` | every locked site, space-separated, each `function:line[thread]` |
| `unlocked_sites` | every unlocked site, in the same form |
| `fp` | `stale` when the finding has a [false-positive entry](#the-false-positive-list) that the source has moved past, empty otherwise |

`fp` is the last column, not inserted between others, so a consumer that reads
the first six columns by position is not affected by it. Suppressed findings
are absent from the csv, as they are from the report.

The csv lacks two things the report shows: the name of the lock held at each
locked site (the report's `(gui_lock)` suffix), and the proof trees. Use `json`
for the first.

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
| `module` | the module name, repeated from the key |
| `src` | the basename of the analysed source, with its extension, which is `.cc` for the modules written in C++ |
| `fields` | every field of the `gui_data` struct, mapped to its declared type. A nested anonymous struct appears once, under its own member name, with type `struct` |
| `accesses` | one entry per `g->field` mention, `[function, field, lock, line, mode]` |
| `thread` | every function in the file, mapped to its inferred thread |

In `accesses`, `lock` is the innermost lock held at that line:

| `lock` | meaning |
| --- | --- |
| `none` | nothing is held. These are the sites a finding accuses |
| `gui_lock` | inside `dt_iop_gui_enter/leave_critical_section()` |
| `callee_lock` | the lock is not held here, but `&self->gui_lock` is passed to the callee, as in `dt_dev_sync_pixelpipe_hash()` |
| `bulk_init` | held, but in a function that writes eight or more fields under it and reads none back. That is bulk initialisation, which establishes no per-field convention. See [Before you file anything](#before-you-file-anything) |
| anything else | the name of a private mutex field, e.g. `histogram_lock`, taken with `dt_pthread_mutex_lock(&g->histogram_lock)` |

`mode` is `w` for a write and `r` for a read. `line` is 1-based, in the file
that `src` names.

In `thread` the value is `pipe`, `gtk`, `either` (see
[Which thread is this on?](#which-thread-is-this-on)) or `null` when inference
failed. A `null` function counts on neither side of the cross-thread test, so
it can never be what makes a field shared. Its unlocked accesses are still
listed as sites, and that is where the report's `?` comes from. The map covers
every function in the file, including ones that never touch `gui_data`.

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
| `gtk_type(T)` | `T` is a widget type, i.e. one whose name starts with `Gtk` or `Dtgtk`. Only types that appear in some `gui_data` struct are emitted. `is_widget` asks this |
| `ptr_type(T)` | `T` is a pointer type, i.e. one written with a `*`. Emitted on the same terms. `is_pointer` asks this |
| `ftype(M, F, T)` | field `F` of module `M` is declared `T` |
| `runs_on(M, Fn, T)` | function `Fn` of module `M` runs on thread `T` |
| `access(M, F, Fn, Lock, Mode)` | `Fn` reads or writes `F` with `Lock` held |

There are two differences from `json`, both because the rules do not use the
information. A function whose thread could not be inferred gets no `runs_on`
fact at all, instead of a `null` one. `access` carries no line number, so all
of one function's accesses to one field, under one lock and in one mode, are a
single fact however many lines they span.


## Recommended workflow

Scan with the defaults, then read the source at the sites the report names.
Use `--why` or `--format json` only when you have a question the report cannot
answer:

```sh
./dt-lockcheck.py                                  # the scan
./dt-lockcheck.py --module basicadj --why          # why does it believe this?
./dt-lockcheck.py --module basicadj --format json  # let me ask the facts myself
```

**Read the stale entries first.** The stderr banner names them, and the report
marks each one. Usually there are none. A stale entry means that somebody read
that exact code, concluded it was safe, and the code has since changed. Read
those findings before the ones nobody has judged.

**Read the source first.** To decide whether a finding is real, open the
`file:line` the report gives for the locked and the unlocked side. That is
usually faster than another run, and always more conclusive, because the
causes that make a finding wrong are the ones the tool cannot see: mutual
exclusion by protocol, a lock passed into a callee, a widget handed to the main
loop. [Before you file anything](#before-you-file-anything) has the full list.
`--why` tells you why the tool reached a conclusion. Only the source tells you
whether the conclusion is true.

**`--why` shows why the tool believes a finding.** For each finding it prints
the chain from the rule down to the base facts: the locked site that
established the convention, and the thread inference that made the field
cross-thread, including whether that inference went through an
[`either`](#which-thread-is-this-on) site. Run it when

- you doubt the thread labels, for an `either` site or a static helper that
  the call graph reached two or three calls below the callback. The tree names
  the exact `runs_on` fact, which you would otherwise reconstruct by hand;
- you are writing the finding up and want the evidence chain as the outline of
  a bug report.

Narrow the run first, to one `--module` or one `--rules`, because the trees
make a full report about six times longer. `--why` needs the lemmalog engine,
works with `--format report` only, and explains the first unlocked site of
each finding, not all of them. See [Proof trees](#proof-trees).

**`--format json` lets you query the facts yourself.** It is the whole
extracted fact base with no rule applied, so use it when your question is not
about a rule:

- every access to one field, including the ones no rule fired on, since the
  report only lists sites for fields that triggered a rule;
- the thread the tool inferred for each function, including the `null` ones
  where inference failed;
- a filter or a rule of your own over the facts, or a diff of two runs to see
  what a patch changed.

Use it with `--module` to keep the output small. An agent driving this tool
should consume this format. See [Output formats](#output-formats) for the
keys, and [Proof trees](#proof-trees) for `--format lemmalog`, which gives the
same facts in the form the Datalog rules use, if you would rather add rules
than write code.


## Before you file anything

Findings are candidates. On the tree the tool was developed against, about two
thirds of the default-rule findings named a field that a report from an
earlier `gui_data` audit also named. A spot-check of a dozen of the rest found
most of them to be real defects of the same kind. Nobody has measured a
false-positive rate, and `discipline_gap` is much worse than the four default
rules. `pointer_share` fires rarely, and a few hits do not measure precision
either, so read them like the rest. Confirm a finding by reading the code
before you open an issue.

Five causes of false positives come up often:

- **Exclusion by protocol instead of by lock.** `rgblevels` hands work to the
  pipe through a state machine (`0` idle, `1` requested, `-1` running, `2`
  ready). It changes state under `gui_lock`, even where the accesses that the
  states guard are not locked. That protects `g->params`, because only the
  thread that set the state to `-1` touches it. The script cannot see this.
  The protocol does not cover the whole module, though. The GTK side writes
  `box_cood`, `channel` and `call_auto_levels` without checking the state, so
  the protocol does not exclude their findings, and they are not false
  positives on this ground.
- **A lock passed into a callee.** The tool handles the common
  `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)` pattern, but not
  the general case.
- **A field that only sits inside a critical section taken for a different
  field.** The tool handles part of this. A function that writes eight or more
  distinct fields under the lock, and never reads them back, counts as bulk
  initialisation, and its accesses are not evidence of a per-field convention.
- **A widget touched from the pipe only to hand it to the main loop.** Taking
  `g_object_ref()` on the widget and passing it to `g_idle_add()` or
  `gdk_threads_add_idle()` is correct, because the ref is atomic and the GTK
  calls happen on the main thread. `widget_from_pipe` still reports it, because
  it sees a widget field touched from the pipe.
- **A finding that rests entirely on an `either` site.** An `either` site
  counts on both sides of the cross-thread test because it can run on either
  thread, but on the path that matters it may always run on one. Check the
  proof tree, or the `[either]` labels in the report, before you trust such a
  finding.

The tool also has these limits:

- It tracks lock depth lexically, so an early `leave_critical_section()` on one
  branch makes later code on other branches look unlocked.
- Thread inference uses function names, call-graph propagation within one
  file, and the [address-taken rules](#which-thread-is-this-on) above. A
  function it still cannot label gets thread `?`. Such a site counts on neither
  side of the cross-thread test, so it can never be what makes a field shared.
- Sharing means pipe versus GTK only. One pipe also uses `gui_data` to hand a
  value to another. For example, the preview pipe computes a whole-image
  property and the full pipe reads it. That needs the same lock (see
  *Passing Values Between Pipes Through `gui_data`* in
  `dev-doc/GUI_Threading.md`), and no rule here looks for it. The tool cannot
  see a field that both pipes touch and the GTK thread never does.
- Coverage is `src/iop/*.c` and `*.cc` only. The tool does not analyse shared
  code under `src/develop/` and `src/libs/`. Widening the glob would not help.
  Outside `src/iop`, the `*_gui_data_t` structs belong to imageio format and
  storage modules. Those hand their GUI values to the worker through a params
  snapshot (`write_image()` takes a `dt_imageio_module_data_t *` and never
  reaches `gui_data`), which is the separation this tool recommends.
  `src/libs` uses a different convention, `dt_lib_<name>_t *d = self->data`,
  and has no IOP callbacks for the thread labels to use. Shared code that takes
  the same `gui_lock`, such as `src/develop/preview_data.c`, would need a
  different extractor, because it has no `gui_data` struct.
- The tool matches `g` by name, not by type. It skips a function that binds
  `g` to something other than the module's GUI data, such as a gain map or a
  gaussian handle, so it also misses any real `gui_data` access in that
  function.
- The tool cannot find defects that need dataflow inside a function. One
  example is a pointer copied under the lock and dereferenced after the lock is
  released, which is a real and recurring pattern here. A clean run does not
  mean that there are no defects.


## The false-positive list

`false-positives.json`, next to the script, records the findings that a person
has read and judged not to be defects. Suppression is on by default. The
script does not report a finding that has a matching entry, so a later review
does not re-read what an earlier one decided.

The risk in such a list is that a suppression outlives the code it was true
about, and hides a defect that later appears in the same place. So every entry
carries a key computed over the finding. A source change that could alter the
judgement changes the key, and the entry goes stale. The finding then comes
back, with the recorded reason printed next to it.

Whenever the list affects a run, the stderr banner says how many findings it
suppressed, how many entries went stale and which entries are orphaned. `-q`
does not silence that line, so the number of suppressed findings is always
visible.

### The three states

The script identifies an entry by `(module, field, rule)`. The key is computed
over other data, so there are three cases:

| state | what it means | what the tool does |
| --- | --- | --- |
| current | the entry matches a finding, and the keys are equal | suppress the finding |
| stale | the entry matches a finding, and the keys differ | report the finding, with the recorded reason and the command to re-confirm it |
| orphan | the entry matches no finding | name it in the banner. The code has moved past it, and the entry should be deleted |

The rule is part of the identity on purpose. A field that moves from one rule
to another is a new finding and shows up as one, instead of inheriting a
judgement made under a rule it no longer matches.

The script counts orphans within the run's own scope. `--module` and `--rules`
narrow what the tool looked for, and an entry the run did not look for is not
an orphan.

### What the key covers

The key has two parts, because neither is safe alone:

- the facts: the field's type, the thread of every function that touches it,
  and the `(function, lock, mode)` of every access. This is the
  [`--format lemmalog`](#lemmalog) view, which has no line numbers, so moving
  code or renaming a local does not change it;
- the text of every access line, copied verbatim from the source.

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

The choice comes from a measurement over 6105 finding transitions across 80
commits that touch `src/iop`, plus seven targeted edits to a real entry:

| candidate key | fires | missed a rule change | missed a site change |
| --- | --- | --- | --- |
| whole-file hash | 9.6% | 0 | 0 |
| facts only | 0.16% | 0 | 7 |
| `function:line` sites | 5.6% | 0 | 2 |
| facts + access-line text, the key in use | 0.28% | 0 | 0 |

The whole-file hash is not a safe but excessive choice. It fires 34 times as
often for no extra safety, and a key that fires often leads people to
re-confirm entries without reading them.

### Confirming an entry

Do not edit the file by hand. Use the tool:

```sh
./dt-lockcheck.py --confirm-false-positive rgblevels:params --false-positive-reason "why this is not a defect"
./dt-lockcheck.py --confirm-false-positive rgblevels          # every stale entry in the module
```

`MODULE:FIELD` names one finding. A bare `MODULE` re-confirms every stale entry
in that module, which is what an audit of one module in one sitting produces.
It cannot add entries, on purpose. Adding one is a judgement about a single
field and has to name it, otherwise one word could silence a whole module.

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

A new entry requires `--false-positive-reason`, because the tool prints that
sentence back when the entry goes stale. An entry without one would be a
blanket suppression that nobody can re-judge. Re-confirming keeps the recorded
reason unless you pass a new one. Either way the tool rewrites the key, the
site list and the confirmation date, prints what it changed, and exits without
reporting.

Name functions in a reason, not line numbers. The report already prints
`function:line` for every site, and the key does not cover the reason. A line
number in the reason goes out of date with every unrelated edit above it, while
the entry stays current.

### Reading and overriding it

```sh
./dt-lockcheck.py                                       # stale entries are marked in the report
./dt-lockcheck.py --include-false-positives             # the suppressed findings too
./dt-lockcheck.py --fail-on-stale-false-positives-only  # exit 1, and report the stale entries alone
```

An ordinary run shows the entries in context. The stderr banner names every
stale entry, and the report marks each one with its recorded reason among the
other findings. The gate in the third line narrows the report to the stale
entries, which helps when you want to know why the gate fired. It only filters
the output. A full run takes as long either way, and `--module` does not make
it faster.

`--include-false-positives` puts the suppressed findings back in the report.
Use it when you review one module and want to re-read its judgements too. It
only changes what is reported, so it is an error with `--format json` or
`lemmalog`, which carry the facts the rules run on and never use the list.

`--fail-on-stale-false-positives-only` turns the same check into a gate for a
CI job or a pre-commit hook. It can pass before every finding has been
reviewed. Its report and its csv contain only the stale entries. It cannot be
combined with `--fail-on-findings`, and asking for both is an error, not a
conjunction.

Each entry also carries its site list as `function:line[thread](lock)`, only
so that a diff of the file is readable. Those line numbers are for information
and are not part of the key.

### What it is not

The list is not a baseline of every finding, and neither gate is a
new-findings gate. `--fail-on-findings` fails on every finding the list does
not suppress, including unreviewed ones and ones that are filed but unfixed.
`--fail-on-stale-false-positives-only` fails only on the judgements recorded
here, and cannot see a defect nobody has looked at. A gate that fails on new
findings would need a checked-in baseline of all findings, a different file
with different rules for when its entries expire. That does not exist here.

An entry also proves little on its own. It records that one person read one
finding at one commit and reached a conclusion. The key makes that conclusion
expire when the code changes. It cannot make the conclusion right.


## Proof trees

`rules.lemma` contains the same rules written as Datalog, for
[lemmalog](https://github.com/JordyZomer/lemmalog). Install lemmalog and put it
on your `$PATH`, and `--why` prints the proof behind every finding in the
report:

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
the exact locked site that is the evidence for the convention, and the thread
inference that made the field cross-thread, including whether that inference
went through `may_pipe` or `may_gtk` on an
[`either`](#which-thread-is-this-on) site. You can use it as the outline of a
bug report. It also shows when a finding rests on a thread guess you disagree
with.

The `why     :` line names the goal, because a finding may list several
unlocked sites and the tree explains only the first. That is the `pipe` or
`either` site the report lists first.

**`--why` changes no findings.** The Python code and the Datalog rules
implement the same rules and produce the same findings, so `--why` does not
change which findings are reported.

- `--why` is off by default, because the trees make a full report about six
  times longer. Use it on a narrowed run, such as one `--module` or one rule
  you are about to write up.
- `--why` fails with exit code 2 when it cannot find a lemmalog binary, instead
  of printing a plain report. You asked for the trees, so a missing engine is a
  problem to fix, not one to hide.
- `--lemmalog PATH` or `$DT_LOCKCHECK_LEMMALOG` names a binary that is not on
  `$PATH`. `--lemmalog` implies `--why`, because that is the only reason to
  name it. A path that does not exist is an error even if `lemmalog` is on
  `$PATH`, because an explicit path never falls back to the search.
- Only `--format report` carries the trees. `csv`, `json` and `lemmalog` do not
  start the engine, and produce the same findings and facts either way. This is
  deliberate, because those formats feed other tools, and their shape should
  not depend on whether an optional binary is installed. `--why` with one of
  them is an error, not a silent no-op. The script captures the engine's output
  instead of passing it through, so it cannot reach stdout. The line that names
  the binary goes to stderr, like the other status lines.

The cost is one `lemmalog` run for the whole report, over the facts that
`--format lemmalog` writes, and it roughly doubles the run time. If the engine
fails after the script has found and started it, for example with an engine
error, a timeout or a missing `rules.lemma`, the script prints a warning on
stderr and the report without trees, because by then it has a report worth
printing.

To drive the engine yourself, for example to add rules of your own or to query
it interactively, use `--format lemmalog` to write out the base facts:

```sh
( cat rules.lemma
  ./dt-lockcheck.py --format lemmalog
  echo run
  echo '? violation(M,F,Fn)'
) | lemmalog
```
