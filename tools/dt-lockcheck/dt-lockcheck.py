#!/usr/bin/env python3
"""dt-lockcheck finds gui_data fields that darktable IOP modules share between
the pixelpipe and the GTK main thread without consistent locking.

WHAT IT LOOKS FOR
  Every IOP module keeps per-instance GUI state in a `dt_iop_<module>_gui_data_t`
  struct, reached as `g->field`.  Code must access a field that both the pipe
  side and the GTK main thread touch inside
  `dt_iop_gui_enter/leave_critical_section()` (the module's `gui_lock`), or under
  a private mutex.  This script finds the fields where that does not happen, or
  does not happen everywhere.

WHICH THREAD
  The script labels every function `gtk`, `pipe` or `either`, and the report
  labels every site the same way.  A function it cannot label shows as `?`.

    gtk     the GTK main thread: gui_init, gui_update, gui_changed, widget and
            draw callbacks, mouse and scroll handlers.
    pipe    a pixelpipe worker thread, never the main thread, for the instance
            that owns gui_data: process, process_cl, process_tiling*,
            modify_roi_in/out, tiling_callback, distort_mask, output_format.
    either  driven by the pipe, but GTK-thread code reaches it too, so it has no
            thread you can rely on: commit_params, init_pipe, cleanup_pipe,
            distort_transform, distort_backtransform, blend_colorspace,
            default_colorspace.  The EITHER set below says why each is there.

  An `either` site counts on both sides of the cross-thread test, because two
  calls can run on different threads.  Check a finding that rests only on such
  a site.  On the path that matters, it may always run on one thread.

  Each label also covers the static helpers reached from the labelled function,
  propagated along the call graph within one file.  This matters because the
  offending line is often two or three calls below a callback whose name says
  nothing about its thread.

  The script also labels a function by how its address is taken.  That covers
  handlers that no naming convention describes:
    * a function passed to a callee as an argument runs on the registrar's
      thread, because the callee calls it synchronously;
    * a function passed to g_idle_add, g_signal_connect or similar is always
      `gtk`, whatever thread registered it;
    * a function stored in a struct member or a file-scope table is `either`,
      because it escapes this file and the call site is unknown.
  Labels from the PIPE, EITHER and GTK sets come from the module API, so
  propagation never overwrites them.

  The five rules, in the order of how much they are worth reading:

    widget_from_pipe  a Gtk*- or Dtgtk*-typed field touched from a `pipe` or
                      `either` function.  Only the main thread may call GTK.
    violation         the field is shared across the two threads and locked
                      somewhere, and some access to it is unlocked.  The
                      module's own code is the evidence that the field needs
                      the lock.
    no_lock_share     the field is written and shared across the two threads,
                      and the module never locks it.
    discipline_gap    the module locks the field somewhere and accesses it
                      unlocked, but no cross-thread sharing was proven.  The
                      noisiest rule, so read it last.
    pointer_share     a pointer-typed field shared across the two threads that
                      no rule above reported: every access is locked, or the
                      field is never locked and only read.  A lock protects
                      the pointer load, not the object, which the other thread
                      may free while the reader uses it.  The rule fires on the
                      pattern, not on a proven escape, so read each hit before
                      you believe it.  The pattern is rare, and the number of
                      such fields limits the rule's output.

  --rules picks the set, by name, or as the upper-case ALL for every rule.  All
  but discipline_gap are on by default.  discipline_gap has to be asked for,
  because on a whole tree most of its findings are fields locked for one reason
  and read unlocked for another.  The report lists a field under at most one
  rule, the first in the order above that matches.

PROOF TREES (--why, off by default)
  rules.lemma, next to this script, contains the same rules written as Datalog.
  With --why, the report follows each finding with the proof tree behind it.
  The tree shows the facts behind the finding, such as the locked site that
  established a convention, the thread facts, and the unlocked site that broke
  it.  --why needs the lemmalog engine
  (github.com/JordyZomer/lemmalog) on $PATH, in $DT_LOCKCHECK_LEMMALOG, or named
  by --lemmalog PATH.  Asking for --why without one is an error, not a silent
  plain report.  The trees do not change which findings are reported.  They make
  a full report several times longer, which is why they are opt-in.

  Only the report carries the trees.  --format csv, json and lemmalog never run
  the engine and never carry a tree, so --why with one of those is an error too.
  The script captures the engine's output instead of passing it through, so it
  cannot reach stdout.  The line that names the binary goes to stderr.

HOW TO READ THE OUTPUT
  Each finding names its locked sites, which are the evidence that a convention
  exists, and its unlocked sites, which are the suspected defect, with the line
  numbers and the thread inferred for each function.  A finding lists only the
  kinds of site it has: no_lock_share has no locked site.  This is a lexical
  analysis, not a verified race detector.  A person has to confirm, for every
  finding, that the accesses can really run at the same time.

  pointer_share is the exception.  Its claim is not a missing lock, but that a
  lock on the pointer does not protect the object behind it, so it usually has
  no unlocked site.  Read its sites and the code that frees or replaces the
  object, and decide whether the object can be freed while the reader still
  uses it.

THE FALSE-POSITIVE LIST (false-positives.json, next to this script)
  The list records the findings that somebody has read and judged not to be
  defects, so that a later review does not read them again.  Suppression is on
  by default, and the stderr banner always says how many entries were applied,
  with or without -q.

  Every entry carries a key over the finding's facts and over the source text of
  its access lines.  An edit to either changes the key.  The entry then goes
  stale, the finding comes back, and the report prints the recorded reason with
  it.  An edit elsewhere does not, even when the judgement depends on it, such
  as a change to the state machine that makes an access safe.  An entry that matches no finding at all is an
  orphan.  The banner names orphans, so that they can be deleted and the list
  does not grow.

    --include-false-positives  report the suppressed findings too
    --confirm-false-positive MODULE:FIELD --false-positive-reason TEXT
                               add one entry, or re-key it
    --confirm-false-positive MODULE
                               re-key every stale entry in that module
    --fail-on-findings         exit 1 if anything is reported (off by default)
    --fail-on-stale-false-positives-only   exit 1 only on a stale entry,
                               and report only the stale entries

  A new entry needs --false-positive-reason, because the tool prints that
  sentence back when the entry goes stale.  A bare module re-confirms entries
  but never adds them, so silencing a whole module has to name each field.
  "The false-positive list" in README.md explains what the key covers and what
  was measured.

KNOWN FALSE-POSITIVE CAUSES (check these before you report a finding upstream)
  * Mutual exclusion by protocol instead of by lock.  `rgblevels` hands work to
    the pipe through a state machine (0 idle, 1 requested, -1 running, 2 ready).
    It changes state under gui_lock, even where the accesses that the states
    guard are not locked.  That protects g->params, because only the thread that
    set the state to -1 touches it.  The script cannot see this.  The protocol
    does not cover the whole module, though.  The GTK side writes box_cood,
    channel and call_auto_levels without checking the state, so the protocol
    does not exclude those findings.
  * A lock passed into a callee.  The script handles the common
    `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)` pattern, but not
    the general case.
  * A field that only sits inside a critical section taken for a different
    field.  The script handles part of this.  A function that writes 8 or more
    distinct fields under the lock and never reads them back counts as bulk
    initialisation and establishes no per-field convention.
  * A pointer_share hit on a field that is already handled correctly.  The rule
    sees a shared pointer, not an escape past the lock.  The three correct
    patterns all leave the field a shared pointer: holding the lock through the
    last use, copying the data instead of the pointer, and transferring
    ownership so that the publisher cannot free it.  The script cannot tell them
    apart, so a module that uses one of them still gets reported.  How many hits
    are real depends on how many modules use one of these patterns, not on the
    rule.
  * A widget touched from the pipe only to hand it to the main loop.  Taking
    `g_object_ref()` on the widget and passing it to `g_idle_add()` or
    `gdk_threads_add_idle()` is correct, because the ref is atomic and the GTK
    calls happen on the main thread.
  * A finding that rests only on an `either` site.  See WHICH THREAD above.

KNOWN LIMITS
  * The script tracks lock depth lexically, so an early
    `leave_critical_section()` on one branch makes later code in other branches
    look unlocked.
  * Defects that need dataflow inside a function are out of reach.  One example
    is a pointer copied under the lock and dereferenced after the lock is
    released.  pointer_share finds the pattern that allows that, by asking
    whether the field is a pointer that both threads share, but never the escape
    itself, because it cannot see where the value is used relative to the
    critical section.
  * Thread inference uses function names, call-graph propagation within one
    file, and the address-taken rules above.  A function it still cannot label
    gets no thread (`?` in the report, null in json).  Such a function counts
    on neither side of the cross-thread test, so it can never be what makes a
    field shared.
  * Sharing means pipe versus GTK only.  One pipe also uses gui_data to hand a
    value to another.  For example, the preview pipe computes a whole-image
    property and the full pipe reads it (dev-doc/GUI_Threading.md, "Passing
    Values Between Pipes Through gui_data").  That needs the same lock, and no
    rule here looks for it.  The script cannot see a field that both pipes touch
    and GTK never does.  A rule for it is not worth having.  The modules that do
    this are the ones that call dt_dev_sync_pixelpipe_hash(), and `grep -l`
    lists them faster than any rule.  Their payload and hash fields are also
    touched on the GTK side, so the existing rules already treat them as shared
    and judge their locking, and a rule for fields touched on `pipe` and never
    on `gtk` would find none of them.  What a correctly locked payload can still
    get wrong is a pointer escaping the lock, and pointer_share covers that.
  * The script matches `g` by name, not by type.  It skips a whole function that
    binds `g` to something other than the module's GUI data, so it also misses
    any real gui_data access in that function.
  * A nested anonymous struct in the gui_data is one field, not several.
    globaltonemap's `struct { GtkWidget *bias; ... } drago;` is the field
    `drago` of type "struct", so `g->drago.bias` does not reach
    widget_from_pipe.
  * The script finds function boundaries by counting braces from an opening `{`
    in column zero.  darktable uses that style throughout, but C in general
    does not.
  * Only `src/iop/*.c` and `*.cc` are covered, not shared code under
    `src/develop/` or `src/libs/`.  A source that a module includes from a
    subdirectory, such as hlreconstruct/opposed.c in highlights.c, is not
    read either, so its gui_data accesses are missing.  The script skips a
    source with no `dt_iop_<module>_gui_data_t`, because it has nothing to
    share, and the stderr banner says how many it skipped.

USAGE
  Run it from anywhere inside a darktable checkout, or with the script on $PATH.
  It finds src/iop by itself and prints the tree it picked on stderr.

  ./dt-lockcheck.py
  ./dt-lockcheck.py --module toneequal
  ./dt-lockcheck.py --rules violation,widget_from_pipe
  ./dt-lockcheck.py --rules ALL                     # including discipline_gap
  ./dt-lockcheck.py --format csv > findings.csv
  ./dt-lockcheck.py --format json > facts.json

  To analyse a tree other than the one you are in:

  ./dt-lockcheck.py --src /path/to/darktable/src/iop
  ./dt-lockcheck.py --src /path/to/darktable        # a checkout root works too

  $DT_LOCKCHECK_SRC does the same as --src, for CI.

  ./dt-lockcheck.py --module toneequal --why        # with the proof trees
  ./dt-lockcheck.py --why --lemmalog ~/lemmalog/target/release/lemmalog

  README.md, next to this script, has the long form.  It describes what each
  --format contains field by field, what each rule checks, and which false
  positives to expect.

  Python 3.8+, standard library only.  The exit status is 0 for a completed run,
  whatever it found, 1 when a --fail-on-* gate was given and fired, and 2 when
  the run did not complete: a wrong invocation, a malformed
  false-positives.json in a run that reads it, or --why without a lemmalog
  binary.  A wrong invocation includes either --fail-on-* gate with --format
  json or lemmalog, because those carry the facts the rules run on and leave a
  gate nothing to fire on.  Both gates are off by default.
  --fail-on-findings passes only once every finding is either fixed or recorded
  in false-positives.json.  --fail-on-stale-false-positives-only fails only when
  a recorded entry has gone stale, so it can pass while findings are still
  unreviewed.  See "Exit status" in README.md.
"""
import argparse
import csv
import datetime, functools, hashlib
import re, sys, os, glob, json, shutil, subprocess
from collections import defaultdict

# --- thread classification of the IOP entry points -------------------------
# Pipe-side: called by the pixelpipe on a worker thread.
# These pipeline callbacks never reach the GTK main thread for the instance that
# owns gui_data, because each of the three screen pipes runs on its own worker
# thread.  Instances in other develop contexts, such as export, thumbnails and
# snapshots, have gui_data == NULL, so they cannot touch the fields this tool
# looks at.
PIPE = {
    "process", "process_cl", "process_tiling", "process_tiling_cl",
    "modify_roi_in", "modify_roi_out", "tiling_callback", "distort_mask",
    "output_format",
}
# Callbacks with no reliable thread affinity.  The pipe drives them, but
# GTK-thread code reaches them too, so an access here may be on either thread.
#   commit_params       basecurve's init_pipe() calls it directly, and the image
#                       switch rebuilds the screen pipes from an idle callback
#   init/cleanup_pipe   the same node rebuild, from the same idle callback
#   distort_*transform  mask handling and darkroom zoom call them directly
#   *_colorspace        the blend GUI and the colour picker call them directly
EITHER = {
    "commit_params", "init_pipe", "cleanup_pipe",
    "distort_transform", "distort_backtransform",
    "blend_colorspace", "default_colorspace",
}
# GTK-side: called from the main loop.
#   change_image  the darkroom's image switch calls it from the idle callback
#                 _dev_load_requested_image().  Its name matches no GTK_RE
#                 pattern, so without this entry it stays unlabelled in a
#                 module that does not also call it from gui_init()
GTK = {
    "gui_init", "gui_update", "gui_cleanup", "gui_changed", "gui_focus",
    "gui_reset", "gui_post_expose", "gui_moved", "gui_button_pressed",
    "gui_button_released", "gui_scrolled", "gui_key_pressed",
    "mouse_moved", "button_pressed", "button_released", "scrolled",
    "reload_defaults", "color_picker_apply", "init_presets", "change_image",
}
# Name-based labelling, applied before any propagation.  (^|_)action_process is
# darktable's shortcut dispatch (dt_action_def_t.process).  darktable registers
# those handlers in a file-scope table and runs them from the GTK main loop.
GTK_RE = re.compile(r"(_callback$|_clicked$|(^|_)draw$|(^|_)expose$|^_?gui_|_changed$|_toggled$|"
                    r"_press(ed)?$|_release(d)?$|_motion|_leave$|_enter$|_scroll|_activate$|draw|"
                    r"(^|_)action_process)")

# Handover forms that always end on the GTK main loop, whatever thread
# registered the callback.  Everything else hands the function to a callee that
# will call it synchronously, so the callback runs on the registrar's thread.
DEFER_RE = re.compile(r"g_idle_add|gdk_threads_add_idle|g_timeout_add|"
                      r"g_signal_connect|SIGNAL_CONNECT|SIGNAL_HANDLE")
# A function pointer stored into a struct member or a global escapes the file:
# `instance->get_effective_exposure = _exposure_proxy_get_effective_exposure;`
# is called from somewhere this analysis cannot see.
ESCAPE_RE = re.compile(r"(?:->|\.)\s*[A-Za-z_]\w*\s*=\s*([A-Za-z_]\w*)\s*[;,]")
# A name in a positional initialiser, such as `{ "x", _handler, ... }`.  That is
# how the dt_action_def_t tables register their shortcut handlers.
STATIC_REF_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?=[,}])")

FIELD_RE  = re.compile(r"\bg\s*->\s*([A-Za-z_][A-Za-z0-9_]*)")
CALL_RE   = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
# `g` is only the module's GUI data by convention.  A few functions bind it to
# something else, such as a gain map or a gaussian handle.  Their `g->x`
# accesses are not gui_data accesses, and the script must not record them.
OTHER_G   = re.compile(r"\b(\w+_t)\s*\*\s*(?:const\s+)?g\b")

# What may follow `g->field` while the write still goes to the same object,
# namely array subscripts and member selections.  `g->box.x = 1` and
# `g->lines[l].p1[0] = v` are writes to `box` and to `lines`.  Without this
# pattern they would count as reads, because the assignment operator would have
# to follow the field name directly.
PATH = r"(?:\s*(?:\[[^\]]*\]|\.\s*[A-Za-z_]\w*|->\s*[A-Za-z_]\w*))*"
# an explicit cast in front of an argument does not make it any less likely
# that the callee writes through the pointer: gtk_x((GtkTreeView *)g->w)
CAST = r"(?:\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*)?"
# A bare identifier in an argument list, past any cast.  The pattern matches the
# separator after the name by lookahead and does not consume it.  re.findall
# does not return overlapping matches, so consuming the separator would make a
# bare identifier that directly follows another one unmatchable, and
# DT_CONTROL_SIGNAL_HANDLE(SIGNAL, handler) has exactly that form.
CALLBACK_ARG_RE = re.compile(r"[(,]\s*" + CAST + r"([A-Za-z_][A-Za-z0-9_]*)\s*(?=[,)])")

ENTER = "dt_iop_gui_enter_critical_section"
LEAVE = "dt_iop_gui_leave_critical_section"
MUTEX_LOCK   = re.compile(r"\b(?:g_mutex_lock|pthread_mutex_lock|dt_pthread_mutex_lock)\s*\(\s*&?\s*g\s*->\s*(\w+)")
MUTEX_UNLOCK = re.compile(r"\b(?:g_mutex_unlock|pthread_mutex_unlock|dt_pthread_mutex_unlock)\s*\(\s*&?\s*g\s*->\s*(\w+)")
# The lock handed to a callee that takes it around the access, as in
# `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)`.  The call site
# looks unlocked, but it is not.  Two scans use this.  is_write() must not count
# such a call as an unsynchronised write, and analyse() records the access under
# `callee_lock`.
LOCK_ARG = re.compile(r"&\s*\w+\s*->\s*gui_lock")


@functools.lru_cache(maxsize=None)
def _write_res(f):
    """The write patterns for one field, built once and reused."""
    g = r"\bg\s*->\s*" + re.escape(f) + PATH
    return (
        # postfix and prefix increment/decrement
        re.compile(g + r"\s*(?:\+\+|--)"),
        re.compile(r"(?:\+\+|--)\s*" + g + r"\b"),
        # address taken
        re.compile(r"&\s*" + g[2:] + r"\b"),
        # assignment and every compound assignment, including <<= >>= %=
        re.compile(g + r"\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)"),
        # passed as an argument, with or without a cast, because the callee may
        # write through the pointer or array.  The pattern matches the trailing
        # separator by lookahead, so two such arguments in a row both match.
        re.compile(r"[(,]\s*" + CAST + g[2:] + r"\s*(?=[,)])"),
    )


def is_write(src, f):
    """A write to g->f: assignment, compound assignment, ++/--, or address-taken.

    The address-taken and passed-to-a-callee forms are deliberately
    over-inclusive, because an address or a bare pointer handed to a callee is
    only a possible write.  The mode only affects no_lock_share, where a missed
    write is a missed finding and a spurious one is a candidate that a person
    then rejects.
    """
    post, pre, addr, assign, arg = _write_res(f)
    if post.search(src) or pre.search(src): return True
    if addr.search(src):
        # ... unless the same call also hands the callee the lock, in which
        # case the callee locks and this is not an unsynchronised write
        if not LOCK_ARG.search(src): return True
    if assign.search(src): return True
    if arg.search(src): return True
    return False

# Comments and literals, in the order they win at a given position: `//` before
# `/*` so a `/*` inside a line comment is not read as an opening, and both
# before the quotes so a quote inside a comment is not read as a literal.
MASK_RE = re.compile(r'//[^\n]*'
                     r'|/\*.*?(?:\*/|\Z)'
                     r'|"(?:\\.|[^"\\\n])*"?'
                     r"|'(?:\\.|[^'\\\n])*'?", re.S)


def _blank(m):
    """One comment or literal, replaced by spaces of the same width."""
    t = m.group(0)
    q = t[0] if t[0] in "\"'" else ""
    if q and len(t) > 1 and t[-1] == q:
        # keep the quotes, so that the next scan sees an empty literal and not
        # two unrelated tokens run together
        return q + re.sub(r"[^\n]", " ", t[1:-1]) + q
    return re.sub(r"[^\n]", " ", t)


def mask_text(text):
    """Blank out comments and the bodies of string/char literals, in place.

    Every scan below works on lines and reports 1-based line numbers, so the
    mask has to keep both the line count and the column of every remaining
    character.  `/* ... */` becomes spaces, not nothing.  Stripping only `//`
    would let disabled code enter the fact base as real accesses, and a
    commented-out enter/leave_critical_section() would shift the lock depth of
    a whole function.  Literal bodies are blanked too, so that a brace or a
    `g->x` inside a message is not read as code.
    """
    return MASK_RE.sub(_blank, text)


# A macro invocation before the declarator, such as DT_OMP_DECLARE_SIMD(...) or
# __attribute__((...)), would otherwise be taken for the function name, because
# the name scan keeps the first identifier followed by '('.  Only all-caps
# macros and __attribute__ are dropped, because a lower-case name before '(' is
# the declarator itself.
MACRO_HEAD = re.compile(r"\s*(?:__attribute__|[A-Z_][A-Z0-9_]*)\s*\(")


def strip_leading_macros(head):
    """A reconstructed signature with any leading macro invocations removed."""
    while True:
        m = MACRO_HEAD.match(head)
        if not m:
            return head
        depth, j = 0, m.end() - 1
        while j < len(head):
            if head[j] == "(":
                depth += 1
            elif head[j] == ")":
                depth -= 1
                if depth == 0: break
            j += 1
        if j >= len(head):
            return head                       # unbalanced, so leave it alone
        rest = head[j + 1:]
        # keep what we had if nothing that looks like a declarator follows,
        # because then the "macro" was the definition itself, e.g. a function
        # generated by HSL_CALLBACK(gain)
        if not CALL_RE.search(rest):
            return head
        head = rest


def split_functions(lines):
    """Yield (name, start_line, end_line) for top-level function bodies."""
    out, i, n = [], 0, len(lines)
    while i < n:
        if lines[i].startswith("{"):
            # back up over the signature
            j = i - 1
            sig = []
            while (j >= 0 and lines[j].strip()
                   and not lines[j].rstrip().endswith((";", "}", "{", "*/"))
                   and not lines[j].lstrip().startswith(("//", "/*", "*", "#"))):
                sig.insert(0, lines[j]); j -= 1
                if len(sig) > 20: break
            head = strip_leading_macros(" ".join(s.strip() for s in sig))
            m = CALL_RE.search(head)
            name = m.group(1) if m else None
            # brace-match to the end of the body.  Counting braces is more
            # reliable than looking for a '}' in column zero.  A closing brace
            # indented by one space, as in splittoning and vibrance, would make
            # the next function part of this one, merging two bodies under the
            # first one's name.
            k, depth = i, 0
            while k < n:
                depth += lines[k].count("{") - lines[k].count("}")
                if depth <= 0: break
                k += 1
            k = min(k, n - 1)
            if name and name not in {"if", "for", "while", "switch", "typedef", "struct"}:
                out.append((name, i + 1, k))
            i = k + 1
        else:
            i += 1
    return out

def struct_body(text, name_re):
    """The body of a `typedef struct [tag] { ... } NAME;` whose NAME matches.

    The search starts at the closing `} NAME;` and goes backwards by brace
    matching, not forwards from `typedef struct {`.  The struct tag is optional
    (liquify has none), and a forward non-greedy match without a tag to anchor
    it would match whatever earlier anonymous typedef the file contains.
    """
    for m in re.finditer(r"\}\s*(%s)\s*;" % name_re, text):
        depth, j = 0, m.start()
        while j >= 0:
            if text[j] == "}":
                depth += 1
            elif text[j] == "{":
                depth -= 1
                if depth == 0: break
            j -= 1
        if j < 0:
            continue
        if re.search(r"typedef\s+struct\s*(?:[A-Za-z_]\w*\s*)?$", text[:j]):
            return text[j + 1:m.start()]
    return None


def gui_fields(text, module):
    body = struct_body(text, r"dt_iop_%s_gui_data_t" % re.escape(module))
    if body is None:
        body = struct_body(text, r"dt_iop_\w+_gui_data_t")
    if body is None:
        return {}
    # a nested anonymous struct or union keeps only its own member name.  The
    # type of `struct { ... } drago;` is recorded as "struct", and g->drago.bias
    # is an access to `drago`, not to `bias` (see KNOWN LIMITS)
    while True:
        stripped = re.sub(r"\{[^{}]*\}", " ", body)
        if stripped == body: break
        body = stripped
    fields = {}
    # split on ';', not on newlines, because a declarator list may continue on
    # the next line (clipping's `float a, b,\n c;`), and only the first line
    # carries the type
    for stmt in body.split(";"):
        stmt = " ".join(l for l in stmt.splitlines() if not l.lstrip().startswith("#"))
        stmt = stmt.strip()
        if not stmt:
            continue
        # one type, then a declarator list.  The type is written once, and the
        # comma-separated declarators after it share it.  Parsing each
        # declarator on its own would lose the type for every field but the
        # first, and widget_from_pipe would miss most Gtk widget fields.
        ty = None
        for decl in stmt.split(","):
            d = decl.strip().rstrip(";")
            d = re.sub(r"\[[^\]]*\]", "", d)          # array bounds
            d = re.sub(r"\(.*", "", d)                 # function pointers
            names = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", d)
            if not names:
                continue
            if ty is None:
                ty = " ".join(names[:-1]) if len(names) > 1 else "unknown"
            # `*` binds to the declarator, not to the type the list shares.  In
            # `float *buf, count;` only `buf` is a pointer.  The pointer flag is
            # recorded because a pointer is the kind of field that a correctly
            # held lock can still fail to protect, as pointer_share explains.
            # Nothing else in `d` can contain a `*` at this point, because
            # mask_text() blanked the comments, and the substitutions above
            # removed array bounds (`[CHANNELS * PIXEL_CHAN]`) and function
            # pointers.
            stars = "*" * d.count("*")
            fields[names[-1]] = (ty + " " + stars) if stars else ty
    return fields

def _module_of(path):
    """The module name behind a source path.  Some IOPs are .cc, not .c."""
    return re.sub(r"\.cc?$", "", os.path.basename(path))

def classify(fnname):
    if fnname in PIPE:   return "pipe"
    if fnname in EITHER: return "either"
    if fnname in GTK:    return "gtk"
    return None

def analyse(path):
    module = _module_of(path)
    # every scan below runs on the masked text, in which comments and literal
    # bodies are blanked out, so disabled code cannot enter the fact base
    text = mask_text(open(path, encoding="utf-8", errors="replace").read())
    lines = text.splitlines()
    fields = gui_fields(text, module)
    if not fields:
        return None
    funcs = split_functions(lines)
    accesses = []          # (fn, field, lockname|"none", line)
    callgraph = defaultdict(set)
    callback_refs = defaultdict(set)   # callee -> {(kind, registrar)}
    escaped = set()                    # callees whose address leaves the file
    fnames = {f[0] for f in funcs}
    for (fn, s, e) in funcs:
        body = "\n".join(lines[max(0, s - 6):e])
        decls = OTHER_G.findall(body)
        if decls and not any(d.endswith("_gui_data_t") for d in decls):
            continue
        held = []
        for ln in range(s, e):
            src = lines[ln]
            for c in CALL_RE.findall(src):
                if c in fnames and c != fn:
                    callgraph[fn].add(c)
            # A bare identifier that names a file-local function and appears in
            # an argument list is a callback.  C converts a function designator
            # to a pointer, so it cannot be anything else.  A list of
            # registration spellings would miss darktable's own helpers, such as
            # dt_gui_connect_motion() and the bauhaus quad setters, and every
            # handler they register.
            for ref in CALLBACK_ARG_RE.findall(src):
                if ref in fnames and ref != fn:
                    callback_refs[ref].add(("defer" if DEFER_RE.search(src) else "direct", fn))
            for ref in ESCAPE_RE.findall(src):
                if ref in fnames and ref != fn:
                    escaped.add(ref)
            if ENTER in src:
                held.append("gui_lock")
            for mm in MUTEX_LOCK.finditer(src):
                held.append(mm.group(1))
            # sorted, not in set order, because CPython randomises string
            # hashing per process.  An unsorted set here would put --format json
            # and lemmalog output in a different order on every run, and two
            # runs could not be diffed.
            for f in sorted(set(FIELD_RE.findall(src))):
                if f in fields:
                    lock = held[-1] if held else "none"
                    if lock == "none" and LOCK_ARG.search(src):
                        lock = "callee_lock"
                    mode = "w" if is_write(src, f) else "r"
                    accesses.append((fn, f, lock, ln + 1, mode))
            if LEAVE in src:
                if held and held[-1] == "gui_lock": held.pop()
            for mm in MUTEX_UNLOCK.finditer(src):
                if mm.group(1) in held: held.remove(mm.group(1))
    # A function that initialises many fields inside one critical section
    # establishes no per-field convention.  The lock is there for the batch,
    # not because any one of these fields needs it.  Mark those sites so that
    # the rules do not count them as evidence of a convention.
    bulk = defaultdict(set)
    reads = defaultdict(set)
    for (fn, f, lock, ln, mode) in accesses:
        if lock == "gui_lock":
            (bulk if mode == "w" else reads)[fn].add(f)
    # A function that also reads the fields it sets is updating state, not
    # initialising it, and its lock does count as evidence.  toneequal's
    # update_curve_lut() is an example.
    bulk_fns = {fn for fn, fields in bulk.items()
                if len(fields) >= 8 and not (fields & reads[fn])}
    accesses = [(fn, f, ("bulk_init" if fn in bulk_fns and lk == "gui_lock" else lk), ln, md)
                for (fn, f, lk, ln, md) in accesses]

    # A name mentioned outside every function body, and not called there, has
    # had its address taken in a static initialiser.  The dt_action_def_t
    # tables register their shortcut handlers by position, which no
    # argument-list or assignment pattern above can see.
    covered = set()
    for (_fn, _s, _e) in funcs:
        covered.update(range(_s, _e))
    for ln, src in enumerate(lines):
        if ln in covered:
            continue
        for ref in STATIC_REF_RE.findall(src):
            if ref in fnames:
                escaped.add(ref)

    # propagate thread labels along the intra-file call graph
    thread = {fn: classify(fn) for (fn, _, _) in funcs}
    # Labels from the PIPE, EITHER and GTK sets come from the module API itself
    # (dev-doc/GUI_Threading.md lists which callback runs where), so they are
    # ground truth and propagation must not overwrite them.  Without this, an
    # imprecise call-graph edge into process(), from a token that only looks
    # like a call, would turn a pipe root into "either", which counts on both
    # sides and creates false sharing.
    pinned = {fn for fn in thread if thread[fn] is not None}
    for fn in list(thread):
        if thread[fn] is None and GTK_RE.search(fn):
            thread[fn] = "gtk"
    # a deferred handover runs on the main loop, whatever thread registered it
    for fn, refs in sorted(callback_refs.items()):
        if thread.get(fn) is None and any(k == "defer" for k, _ in refs):
            thread[fn] = "gtk"
    # an escaped function pointer is called from outside this file, so neither
    # thread can be ruled out.  The label is correct but has a cost.  "either"
    # counts on both sides of the cross-thread test, so it can make a field look
    # shared on its own.  See "Before you file anything" in README.md
    for fn in sorted(escaped):
        if thread.get(fn) is None:
            thread[fn] = "either"
    def join(fn, t):
        """Widen fn's thread label to admit a call from thread `t`.

        The lattice lives here instead of at each of the two call sites below.
        An unlabelled function takes `t`, a second and different label raises
        pipe or gtk to "either", and "either" is already the top.  Returns
        whether the label changed, which is what ends the fixpoint loop.
        """
        cur = thread.get(fn)
        if cur is None:
            thread[fn] = t
            return True
        if cur != t and cur in ("pipe", "gtk"):
            thread[fn] = "either"
            return True
        return False

    changed = True
    rounds = 0
    while changed and rounds < 20:
        changed = False; rounds += 1
        for caller in sorted(callgraph):
            callees = callgraph[caller]
            if thread.get(caller):
                for c in sorted(callees):
                    if c not in pinned and join(c, thread[caller]):
                        changed = True
        # a synchronously-invoked callback runs on its registrar's thread
        for callee, refs in sorted(callback_refs.items()):
            if callee in pinned:
                continue
            for kind, registrar in sorted(refs):
                t = thread.get(registrar)
                if kind != "direct" or not t:
                    continue
                if join(callee, t):
                    changed = True
    # the basename, not just the module, because some IOPs are .cc, and
    # <module>.c would name a path that does not exist
    return dict(module=module, src=os.path.basename(path), fields=fields,
                accesses=accesses, thread=thread)



# Set-once functions, whose writes are initialisation and not a shared-state
# update.  init_pipe and cleanup_pipe are deliberately not in this set, because
# they run per pipe, and on either thread.
SETUP_FNS = {"gui_init", "gui_cleanup", "gui_reset"}

# In report order, most worth reading first.  discipline_gap is the one rule
# outside the default set, because it proves no cross-thread sharing and is
# mostly noise.  See "The rules" in README.md.
ALL_RULES = ("widget_from_pipe", "violation", "no_lock_share", "discipline_gap",
             "pointer_share")
DEFAULT_RULES = ("widget_from_pipe", "violation", "no_lock_share",
                 "pointer_share")
# --rules ALL means every rule, so a caller that wants all of them does not need
# an update when a rule is added.  It is upper-case to keep it apart from the
# rule names, which are lower-case throughout, including in the Datalog.
ALL_RULES_TOKEN = "ALL"


MAY_PIPE = {"pipe", "either"}   # can run off the GTK main thread
MAY_GTK = {"gtk", "either"}     # can run on the GTK main thread


def findings(res, wanted):
    """Apply the rules to the extracted facts.

    Mirrors rules.lemma one-for-one; kept in Python so the tool has no
    dependency on a Datalog engine.
    """
    out = []
    for module, r in sorted(res.items()):
        th, fields = r["thread"], r["fields"]
        by_field = defaultdict(list)
        for a in r["accesses"]:
            by_field[a[1]].append(a)
        for field, sites in sorted(by_field.items()):
            live = [a for a in sites if a[0] not in SETUP_FNS]
            threads = {th.get(a[0]) for a in live}
            # "either" counts on both sides, because two calls of the same
            # callback can run on different threads.
            shared = (any(t in MAY_PIPE for t in threads)
                      and any(t in MAY_GTK for t in threads))
            written = any(a[4] == "w" for a in live)
            # a locked site outside bulk initialisation is what establishes a
            # per-field convention
            locked = [a for a in sites if a[2] not in ("none", "bulk_init")]
            unlocked = [a for a in live if a[2] == "none"]
            widget = fields.get(field, "").startswith(("Gtk", "Dtgtk"))
            pointer = fields.get(field, "").endswith("*")

            rule = None
            if widget and any(th.get(a[0]) in MAY_PIPE for a in sites):
                rule = "widget_from_pipe"
            elif locked and shared and unlocked:
                rule = "violation"
            elif shared and written and not locked:
                rule = "no_lock_share"
            elif locked and unlocked:
                rule = "discipline_gap"
            # Last, so that it claims only fields no other rule did.  The field
            # can be unsafe even when every access is under the lock, because
            # the lock protects the pointer load, not the object, which the
            # other thread may free before the reader is done with it (dev-doc/
            # GUI_Threading.md, "A scalar hands over cleanly; an allocation does
            # not").  Proving the escape needs dataflow inside a function, which
            # this tool cannot do, so the rule fires on the pattern alone and
            # every hit needs reading.  No widget can reach here, because a
            # shared widget is touched from pipe or either by definition, so
            # widget_from_pipe already took it.
            elif pointer and shared:
                rule = "pointer_share"
            if rule and rule in wanted:
                out.append(dict(module=module, src=r.get("src", module + ".c"),
                                field=field, rule=rule,
                                ctype=fields.get(field, "?"),
                                locked=locked, unlocked=unlocked, thread=th))
    order = {r: i for i, r in enumerate(ALL_RULES)}
    out.sort(key=lambda f: (order[f["rule"]], f["module"], f["field"]))
    return out


def _pipe_first(sites, th):
    """Show pipe-thread sites first, because the cross-thread half is the interesting one."""
    return sorted(sites, key=lambda a: (th.get(a[0]) not in MAY_PIPE, a[3]))


def ascii_safe(line):
    """The proof trees use U+21B3; degrade for a non-UTF-8 stdout."""
    enc = getattr(sys.stdout, "encoding", None) or "ascii"
    try:
        line.encode(enc)
    except UnicodeEncodeError:
        return line.replace("\u21b3", "->")
    return line


def lemmalog_facts(res):
    """The extracted facts as lemmalog assertions, one per line."""
    out = []
    # the rules ask `gtk_type(T)`; emit the widget types actually seen
    seen = {ty for r in res.values() for ty in r["fields"].values()
            if ty.startswith(("Gtk", "Dtgtk"))}
    for ty in sorted(seen):
        out.append(f'+ gtk_type("{ty}")')
    # likewise for `ptr_type(T)`, which is what is_pointer asks
    for ty in sorted({ty for r in res.values() for ty in r["fields"].values()
                      if ty.endswith("*")}):
        out.append(f'+ ptr_type("{ty}")')
    for m, r in sorted(res.items()):
        for f, ty in sorted(r["fields"].items()):
            out.append(f'+ ftype("{m}", "{f}", "{ty}")')
        for fn, t in sorted(r["thread"].items()):
            if t:
                out.append(f'+ runs_on("{m}", "{fn}", "{t}")')
        # Deduplicate what is emitted, not the accesses.  The fact carries no
        # line number, so two accesses that differ only by their line are the
        # same assertion, and asserting it twice tells the engine nothing.
        # Deduplicating the access tuples instead would leave about half the
        # access facts duplicated.  The output is sorted for the same reason as
        # the field scan above, so that two runs can be diffed.
        for (fn, f, lock, mode) in sorted({(a[0], a[1], a[2], a[4])
                                           for a in r["accesses"]}):
            out.append(f'+ access("{m}", "{f}", "{fn}", "{lock}", "{mode}")')
    return out


def site_str(a, th):
    return f"{a[0]}:{a[3]}[{th.get(a[0]) or '?'}]"


def _sites(sites, th, cap, extra=None):
    """A capped, pipe-first site list, with the number of sites it left out.

    The report says how many sites it left out.  A finding can carry forty
    sites, and a report that printed eight of them without saying so would
    understate its evidence compared with --format csv, which writes every one.
    """
    shown = _pipe_first(sites, th)[:cap]
    line = ", ".join(site_str(a, th) + (extra(a) if extra else "") for a in shown)
    rest = len(sites) - len(shown)
    return line + (f", ... (+{rest} more)" if rest else "")


def print_report(fs, trees=None, suppressed=0, total=None):
    """`total`, when given, means fs is a filtered slice of a larger run."""
    by_rule = defaultdict(list)
    for f in fs:
        by_rule[f["rule"]].append(f)
    for rule in ALL_RULES:
        group = by_rule.get(rule)
        if not group:
            continue
        print(f"\n=== {rule}  ({len(group)}) " + "=" * (52 - len(rule)))
        for f in group:
            th = f["thread"]
            print(f"\n{f['src']}  g->{f['field']}   ({f['ctype']})")
            if f["locked"]:
                print("   locked  : " + _sites(f["locked"], th, 6,
                                               lambda a: f"({a[2]})"))
            # pointer_share rests on the field's type and its threads, not on
            # a missing lock, so it can reach here with no unlocked site to
            # name.  Its evidence is then the locked list above: every access
            # is under the lock, and the field is unsafe anyway.
            if f["unlocked"]:
                print("   unlocked: " + _sites(f["unlocked"], th, 8))
            # A stale entry means that somebody judged this exact finding
            # harmless and the code has changed since.  Print the judgement with
            # it, because re-reading it is most of the work of deciding again.
            # Print the command that acts on the decision too, because the
            # alternative is editing the list by hand.
            if f.get("fp"):
                e = f["fp"]
                print("   was a known false positive, confirmed %s:" % e["confirmed"])
                print("     %s" % e["reason"])
                print("   re-read the source; if it is still not a defect:")
                print("     ./dt-lockcheck.py --confirm-false-positive %s:%s"
                      % (f["module"], f["field"]))
            if trees:
                key = why_goal(f)[1]
                tree = trees.get(key)
                if tree:
                    # name the goal, because the unlocked line above lists
                    # several sites and the tree only explains this one
                    print("   why     : " + key)
                    for line in tree:
                        print(ascii_safe("   " + line.rstrip()))
    if total is not None:
        print("\n%d stale of %d findings; the rest are not what this gate asks "
              "about." % (len(fs), total))
    else:
        print("\n%d findings%s." % (len(fs),
              ", %d suppressed as known false positives" % suppressed
              if suppressed else ""))


# --- the checked-in false-positive list -------------------------------------
# Findings that a person has read and judged not to be defects, so that the next
# review does not have to read them again.  The risk is that a suppression
# outlives the code it was true about and hides a real defect.  So every entry
# carries a key over the finding's own facts and over the source text of its
# access lines.  An edit to either changes the key, the entry goes stale, and
# the finding comes back with the recorded reason attached.  An edit elsewhere
# that the judgement depends on does not.  "The false-positive list" in README.md has the measurements behind
# that choice.
FP_FILE = os.path.join(os.path.dirname(os.path.realpath(__file__)),
                       "false-positives.json")
FP_VERSION = 1
FP_KEYS = ("module", "field", "rule", "key", "reason", "confirmed", "sites")


def fp_ident(e):
    """What names one judgement.  A finding and its entry agree on these three,
    which is how the two are matched up."""
    return (e["module"], e["field"], e["rule"])


def _module_lines(srcdir, res, module, cache):
    """The module's source split into lines, read at most once per run."""
    if module not in cache:
        path = os.path.join(srcdir, res[module]["src"])
        cache[module] = open(path, encoding="utf-8",
                             errors="replace").read().splitlines()
    return cache[module]


def finding_key(f, res, srcdir, cache):
    """What a source change has to alter for a suppression to expire.

    The key has two parts, because neither is safe alone.

    The facts part is the lemmalog view of the field: its type, the thread of
    every function that touches it, and the (function, lock, mode) of every
    access.  It has no line numbers, so moving code around or renaming a local
    does not change it.

    The text part is the source line behind every access.  It catches the
    edits that turn a false positive into a defect while leaving the facts
    identical.  The key was designed on colorharmonizer's former widget
    hand-off, which 7b8a19f0f5 removed.  Replacing its
    `gdk_threads_add_idle(...)` with a direct GTK call, or dropping its
    `g_object_ref`, would have kept the same function, lock and mode.  The
    facts part catches the opposite case, a critical section removed around an
    access, which the text of the access line does not show.
    """
    r = res[f["module"]]
    th = r["thread"]
    acc = [a for a in r["accesses"] if a[1] == f["field"]]
    lines = _module_lines(srcdir, res, f["module"], cache)

    def text(a):
        # the raw source, not the masked text the rules run on, because a
        # comment added to an access line often matters to the judgement
        return lines[a[3] - 1].strip() if 0 < a[3] <= len(lines) else ""

    facts = ["ftype=" + r["fields"].get(f["field"], "?")]
    facts += ["runs_on(%s,%s)" % (fn, th.get(fn))
              for fn in sorted({a[0] for a in acc})]
    facts += ["access(%s,%s,%s)" % t
              for t in sorted({(a[0], a[2], a[4]) for a in acc})]
    body = ("\n".join(facts) + "\n"
            + repr(sorted((a[0], a[2], a[4], text(a)) for a in acc)))
    return hashlib.sha256(body.encode()).hexdigest()[:12]


def finding_sites(f, res):
    """Every site of the field, as `fn:line[thread](lock)`.

    This duplicates the key on purpose.  The key is twelve hex digits, and the
    site list is what makes a diff of the list readable.  The line numbers here
    are for information and are not part of the key.
    """
    r = res[f["module"]]
    th = r["thread"]
    acc = sorted({a for a in r["accesses"] if a[1] == f["field"]},
                 key=lambda a: (a[3], a[0]))
    return [site_str(a, th) + "(%s)" % a[2] for a in acc]


def load_fp_list(path):
    """The checked-in entries, or [] when there is no list yet.

    Every malformed entry is fatal, not skipped.  A list that drops an entry it
    cannot parse suppresses less than it claims, and one that keeps a duplicate
    suppresses under a rule nobody judged.
    """
    if not os.path.exists(path):
        return []
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, ValueError) as e:
        die("cannot read the false-positive list %s: %s" % (path, e))
    if not isinstance(doc, dict) or doc.get("version") != FP_VERSION:
        die("%s: expected a json object with \"version\": %d" % (path, FP_VERSION))
    entries = doc.get("entries")
    if not isinstance(entries, list):
        die("%s: \"entries\" must be a list" % path)
    seen = set()
    for e in entries:
        if not isinstance(e, dict) or any(k not in e for k in FP_KEYS):
            die("%s: every entry needs %s" % (path, ", ".join(FP_KEYS)))
        if e["rule"] not in ALL_RULES:
            die("%s: entry %s:%s names rule %r, which this version does not have"
                % (path, e["module"], e["field"], e["rule"]))
        ident = (e["module"], e["field"], e["rule"])
        if ident in seen:
            die("%s: two entries for %s:%s under %s"
                % (path, e["module"], e["field"], e["rule"]))
        seen.add(ident)
    return entries


def save_fp_list(path, entries):
    """Rewrite the list, sorted so a change shows up as one entry in a diff."""
    doc = {"version": FP_VERSION,
           "_README": "dt-lockcheck findings judged not to be defects.  "
                      "Maintained with --confirm-false-positive, not by hand; "
                      "see tools/dt-lockcheck/README.md.",
           "entries": sorted(entries,
                             key=lambda e: (e["module"], e["field"], e["rule"]))}
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=2, sort_keys=True)
        fh.write("\n")
    os.replace(tmp, path)   # never leave a half-written list behind


def fp_scope(entries, res, wanted):
    """The entries this run could have matched.

    An entry is out of scope when its module was not analysed (`--module`) or
    its rule was not asked for (`--rules`).  Such an entry is not an orphan,
    because the run never looked for it.
    """
    return [e for e in entries if e["module"] in res and e["rule"] in wanted]


def apply_fp(fs, entries, res, wanted):
    """Split the findings against the list.

    Returns (kept, suppressed, orphans).  `kept` is what the run reports, which
    is every finding with no entry plus every finding whose entry has gone
    stale.  A stale finding carries its entry as `fp`, so that the report can
    print the reason.  `orphans` are in-scope entries that no finding matched.
    The code has moved past them, and they should be deleted.
    """
    index = {fp_ident(e): e for e in entries}
    kept, suppressed, matched = [], [], set()
    for f in fs:
        ident = fp_ident(f)
        e = index.get(ident)
        if e is None:
            kept.append(f)
            continue
        matched.add(ident)
        if e["key"] == f["key"]:
            suppressed.append(e)
        else:
            f["fp"] = e
            kept.append(f)
    orphans = [e for e in fp_scope(entries, res, wanted)
               if fp_ident(e) not in matched]
    return kept, suppressed, orphans


def fp_banner(suppressed, stale, orphans, path):
    """Always printed, with or without `-q`, so the number of suppressed findings is always visible."""
    if not (suppressed or stale or orphans):
        return
    parts = ["%d suppressed" % len(suppressed)]
    # name stale and orphaned entries, not just count them.  A
    # --fail-on-stale-false-positives-only job fails on them, and a CI log that
    # says only "1 stale" makes the reader run the tool again to find out which
    # entry it is
    if stale:
        parts.append("%d stale (%s)"
                     % (len(stale), ", ".join(sorted(
                         "%s:%s" % (f["module"], f["field"]) for f in stale))))
    if orphans:
        parts.append("%d orphaned (%s)"
                     % (len(orphans), ", ".join(sorted(
                         "%s:%s" % (e["module"], e["field"]) for e in orphans))))
    sys.stderr.write("dt-lockcheck: false positives: %s; from %s\n"
                     % (", ".join(parts), path))


def parse_fp_spec(spec, res):
    """`module` or `module:field`, checked against what the run analysed."""
    module, _, field = spec.partition(":")
    if not module or spec.count(":") > 1:
        die("--confirm-false-positive takes MODULE or MODULE:FIELD, not %r" % spec)
    if module not in res:
        die("--confirm-false-positive %s: %s was not analysed in this run"
            % (spec, module))
    return module, (field or None)


def confirm_fps(specs, reason, fs, entries, res, wanted, path):
    """Re-key stale entries, and add new ones, in place.

    A bare module name re-confirms every stale entry in it, which is what an
    audit of one module in one sitting produces.  It cannot create entries, on
    purpose.  Adding one is a judgement about a single field and has to name it,
    otherwise one word could silence a whole module.
    """
    found = {fp_ident(f): f for f in fs}
    today = datetime.date.today().isoformat()
    touched = 0
    for spec in specs:
        module, field = parse_fp_spec(spec, res)
        if field is None:
            stale = []
            for e in fp_scope(entries, res, wanted):
                f = found.get(fp_ident(e))
                if e["module"] == module and f and f["key"] != e["key"]:
                    stale.append((e, f))
            if not stale:
                print("%s: no stale entry to re-confirm" % module)
            for e, f in stale:
                _rekey(e, f, res, reason, today)
                touched += 1
            continue
        # a field is reported under at most one rule, so module:field names at
        # most one entry and at most one finding
        ent = next((e for e in entries
                    if e["module"] == module and e["field"] == field), None)
        fnd = next((f for f in fs
                    if f["module"] == module and f["field"] == field), None)
        if ent is not None:
            f = found.get(fp_ident(ent))
            if f is None:
                print("%s:%s: no finding under %s any more -- the entry is an "
                      "orphan, delete it rather than re-confirming it"
                      % (module, field, ent["rule"]))
            elif f["key"] == ent["key"] and not reason:
                print("%s:%s: already current (key %s), nothing to do"
                      % (module, field, ent["key"]))
            else:
                _rekey(ent, f, res, reason, today)
                touched += 1
        elif fnd is not None:
            if not reason:
                die("a new entry needs --false-positive-reason: say why %s:%s "
                    "is not a defect, "
                    "because that sentence is what gets printed back when the "
                    "entry goes stale" % (module, field))
            entries.append(dict(module=module, field=field, rule=fnd["rule"],
                                key=fnd["key"], reason=reason, confirmed=today,
                                sites=finding_sites(fnd, res)))
            print("%s:%s: added under %s, key %s"
                  % (module, field, fnd["rule"], fnd["key"]))
            touched += 1
        else:
            die("--confirm-false-positive %s: no finding and no entry for that "
                "field.  Rules in this run: %s"
                % (spec, ",".join(r for r in ALL_RULES if r in wanted)))
    if touched:
        save_fp_list(path, entries)
        print("%s: %d entr%s written" % (path, touched,
                                         "y" if touched == 1 else "ies"))


def _rekey(e, f, res, reason, today):
    """Move an entry onto the finding as it stands now."""
    was = e["key"]
    e["key"] = f["key"]
    e["sites"] = finding_sites(f, res)
    e["confirmed"] = today
    # a sweep that replaces a recorded reason says so, because the key cannot
    # detect a judgement overwritten by one that was not about this field
    replaced = bool(reason) and reason != e["reason"]
    if reason:
        e["reason"] = reason
    print("%s:%s: %s under %s%s"
          % (e["module"], e["field"],
             "re-confirmed, key %s -> %s" % (was, e["key"]) if was != e["key"]
             else "reason updated, key %s unchanged" % e["key"],
             e["rule"], " (previous reason replaced)" if replaced else ""))


# --- optional proof trees via lemmalog --------------------------------------
# The Datalog rules in rules.lemma restate the Python ones.  When the engine is
# installed, it can derive the same findings again and print each one with the
# base facts it rests on.  Nothing here can change which findings are reported.
RULES_FILE = os.path.join(os.path.dirname(os.path.realpath(__file__)), "rules.lemma")
LEMMALOG_TIMEOUT = 120

# `pred(a, b)  (conf 1.000, prov [])` or `pred(a, b)  [unknown fact]`, at column
# zero.  The tree body underneath is indented, and the engine's own progress
# messages match neither form.
ANSWER_RE = re.compile(r"^([a-z_]+\([^)]*\))\s\s+(\(conf |\[unknown fact\])")
# every fact in this fact base is asserted flat, so these annotations are the
# same on every line and carry no information here
NOISE_RE = re.compile(r"\s+\(conf 1\.000, prov \[\]\)$")


def find_lemmalog(explicit):
    """The lemmalog binary.  Returns (path, how), or (None, why not).

    The same rule as for --src applies.  An explicit path never falls back to
    the search, so a typo shows up as an error instead of silently using some
    other binary.
    """
    if explicit:
        return ((explicit, "--lemmalog") if os.path.isfile(explicit)
                else (None, "no lemmalog binary at %s" % explicit))
    env = os.environ.get("DT_LOCKCHECK_LEMMALOG")
    if env:
        return ((env, "$DT_LOCKCHECK_LEMMALOG") if os.path.isfile(env)
                else (None, "no lemmalog binary at $DT_LOCKCHECK_LEMMALOG=%s" % env))
    found = shutil.which("lemmalog")
    if found:
        return found, "$PATH"
    return None, ("--why needs the lemmalog engine, which is not on $PATH.  Install "
                  "it from https://github.com/JordyZomer/lemmalog, or point "
                  "--lemmalog at the binary.")


def why_goal(f):
    """(query, key) for a finding, or (None, None).

    The two differ because lemmalog echoes a goal back with the quotes
    stripped, and the answers have to be looked up by the echo.  The arity
    depends on the rule.  no_lock_share and pointer_share are per field, and
    the rest are per unlocked site.
    """
    if f["rule"] in ("no_lock_share", "pointer_share"):
        args = (f["module"], f["field"])
    else:
        # the site the report leads with, so the tree explains the line above it
        sites = _pipe_first(f["unlocked"], f["thread"])
        if not sites:
            return None, None
        args = (f["module"], f["field"], sites[0][0])
    return ("why %s(%s)" % (f["rule"], ", ".join('"%s"' % a for a in args)),
            "%s(%s)" % (f["rule"], ", ".join(args)))


def why_trees(exe, res, fs):
    """Run every finding's `why` through lemmalog in one pass.

    Returns {goal: [tree lines]}, or an empty dict on any failure.  The report
    is still worth printing without the trees, so nothing here is fatal.
    """
    if not os.path.exists(RULES_FILE):
        sys.stderr.write("dt-lockcheck: %s is missing, skipping proof trees\n" % RULES_FILE)
        return {}
    queries = [q for q in (why_goal(f)[0] for f in fs) if q]
    if not queries:
        return {}
    with open(RULES_FILE, encoding="utf-8") as fh:
        script = "\n".join([fh.read()] + lemmalog_facts(res)
                           + ["run"] + queries) + "\n"
    try:
        p = subprocess.run([exe], input=script, capture_output=True, text=True,
                           timeout=LEMMALOG_TIMEOUT)
    except (OSError, subprocess.SubprocessError) as e:
        sys.stderr.write("dt-lockcheck: lemmalog failed (%s), skipping proof trees\n" % e)
        return {}
    if p.returncode != 0:
        first = (p.stderr or "").strip().splitlines()[:1]
        sys.stderr.write("dt-lockcheck: lemmalog exited %d%s, skipping proof trees\n"
                         % (p.returncode, ": " + first[0] if first else ""))
        return {}

    # Answers come back in query order, but key them by goal, not by position.
    # A mismatch then means a missing tree, not the wrong proof printed with a
    # finding.
    trees, cur = {}, None
    for line in p.stdout.splitlines():
        if not line.strip():
            continue
        if line[0].isspace():
            if cur is not None:
                cur.append(NOISE_RE.sub("", line))
            continue
        m = ANSWER_RE.match(line)
        if m:
            cur = trees.setdefault(m.group(1), [])
            if m.group(2).startswith("["):
                cur.append("   (not derivable from the Datalog rules)")
    return trees


# --- locating darktable's src/iop ------------------------------------------
# What identifies a checkout root.  imageop.h declares
# dt_iop_gui_enter_critical_section(), which the whole analysis depends on.
# CMakeLists.txt confirms that this is the tree root, not some parent directory
# that also contains a src/iop.
ROOT_MARKERS = ("src/iop", "src/develop/imageop.h", "CMakeLists.txt")
MAX_WALK_UP = 12   # levels to climb; no checkout root sits deeper than this
MIN_SOURCES = 10   # src/iop holds ~90 modules, so a near-empty dir is the wrong one


def iop_sources(d):
    """The *.c / *.cc directly under `d`, sorted."""
    return sorted(glob.glob(os.path.join(d, "*.c")) +
                  glob.glob(os.path.join(d, "*.cc")))


def _is_root(d):
    return all(os.path.exists(os.path.join(d, m)) for m in ROOT_MARKERS)


def _walk_up(start):
    """Nearest darktable checkout root at or above `start`, else None."""
    d = os.path.abspath(start)
    for _ in range(MAX_WALK_UP):
        if _is_root(d):
            return d
        parent = os.path.dirname(d)
        if parent == d:          # hit the filesystem root
            return None
        d = parent
    return None


def _as_iop_dir(d):
    """Normalise a user-supplied path to an iop directory, or None.

    Accepts either the iop directory itself or a checkout root, because people
    often type --src ~/darktable first.
    """
    if _is_root(d):
        d = os.path.join(d, "src", "iop")
    return d if len(iop_sources(d)) >= MIN_SOURCES else None


def find_src(explicit):
    """Locate src/iop.  Returns (path, how) on success, (None, tried) on failure.

    An explicit --src is used as given and never falls back, so a typo fails
    with an error instead of analysing some other tree.  Otherwise the order is
    $DT_LOCKCHECK_SRC, then a walk up from $PWD, then a walk up from the script
    itself.  $PWD comes before the script location on purpose.  With the script
    symlinked onto $PATH from checkout A, running it inside checkout B has to
    analyse B.
    """
    if explicit:
        d = _as_iop_dir(explicit)
        return (d, "--src") if d else (None, [("--src " + explicit,
                                               "not a checkout root and no IOP sources in it")])
    tried = []
    env = os.environ.get("DT_LOCKCHECK_SRC")
    if env:
        d = _as_iop_dir(env)
        if d:
            return d, "$DT_LOCKCHECK_SRC"
        tried.append(("$DT_LOCKCHECK_SRC=" + env, "no IOP sources there"))
    else:
        tried.append(("$DT_LOCKCHECK_SRC", "not set"))

    # realpath, not __file__, because a symlink on $PATH has to resolve to the
    # checkout that contains the script.
    for what, start in (("$PWD", os.getcwd()),
                        ("script", os.path.dirname(os.path.realpath(__file__)))):
        root = _walk_up(start)
        if root is None:
            tried.append(("above %s (%s)" % (what, start), "no darktable root up to /"))
            continue
        d = _as_iop_dir(root)
        if d:
            return d, "darktable root above " + what
        tried.append(("above %s (%s)" % (what, start),
                      "found root %s, but no IOP sources in it" % root))
    return None, tried


def die(msg):
    """Exit 2 for a wrong invocation, as opposed to 0 for a completed run."""
    sys.stderr.write("dt-lockcheck: %s\n" % msg)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(
        description="Find unsynchronised gui_data sharing in darktable IOP modules.",
        epilog="Findings are candidates, not confirmed bugs. See the module docstring "
               "for the known false-positive causes before filing anything.")
    ap.add_argument("--src",
                    help="darktable's src/iop, or a checkout root.  By default "
                         "the script looks in $DT_LOCKCHECK_SRC, then up from the "
                         "current directory, then up from its own location")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="do not report the located source directory on stderr")
    ap.add_argument("--module", action="append",
                    help="restrict to this module (repeatable), e.g. --module toneequal")
    ap.add_argument("--rules", default=",".join(DEFAULT_RULES), metavar="LIST",
                    help="comma-separated rules to report, from: "
                         + ", ".join(ALL_RULES) + "; or " + ALL_RULES_TOKEN
                         + " (upper-case) for every rule.  "
                         "discipline_gap is noisy and is the one rule left "
                         "out by default (default: %(default)s).  "
                         "Affects report and csv only, because json and "
                         "lemmalog carry the facts the rules run on.  See README.md "
                         "for what each rule checks")
    ap.add_argument("--format", choices=("report", "csv", "json", "lemmalog"),
                    default="report",
                    help="report: human-readable findings; csv: one row per finding, "
                         "with every site; json: the raw extracted facts, before the "
                         "rules; lemmalog: those facts as Datalog assertions.  Only "
                         "report gets proof trees, and lemmalog does not affect the "
                         "other three.  See README.md for what each format "
                         "contains")
    ap.add_argument("--include-false-positives", action="store_true",
                    dest="include_fp",
                    help="also report the findings that false-positives.json "
                         "suppresses, without their recorded reasons.  Use it "
                         "when you review one module and want to see every "
                         "finding")
    ap.add_argument("--confirm-false-positive", action="append",
                    metavar="MODULE[:FIELD]", dest="confirm",
                    help="record that a finding is not a defect, or that a stale "
                         "entry still holds (repeatable).  MODULE:FIELD names "
                         "one finding; a bare MODULE re-confirms every stale "
                         "entry in it, but cannot add new ones.  Rewrites "
                         "false-positives.json and exits without reporting")
    ap.add_argument("--false-positive-reason", metavar="TEXT", dest="reason",
                    help="why the finding is not a defect.  Required for a new "
                         "entry, because the tool prints it back when the entry "
                         "goes stale.  Optional when re-confirming, where it "
                         "replaces the recorded reason.  One judgement is about "
                         "one module, so every --confirm-false-positive in the "
                         "run has to name the same module")
    ap.add_argument("--fail-on-findings", action="store_true",
                    help="exit 1 when anything is reported, for CI.  Off by "
                         "default.  It passes only "
                         "once every finding is either fixed or recorded in "
                         "false-positives.json")
    ap.add_argument("--fail-on-stale-false-positives-only", action="store_true",
                    dest="fail_on_stale",
                    help="exit 1 only when a false-positive entry has gone "
                         "stale, which means that somebody judged the finding "
                         "harmless and its facts or access lines have changed "
                         "since.  The weaker "
                         "gate, which a CI job can use before every finding has "
                         "been dealt with.  Reports only the stale entries, "
                         "because the other findings are not what this gate "
                         "checks")
    ap.add_argument("--why", action="store_true",
                    help="append the lemmalog proof tree behind each finding.  "
                         "Needs the lemmalog engine, and is an error if the engine "
                         "cannot be found")
    ap.add_argument("--lemmalog", metavar="PATH",
                    help="the lemmalog binary to use, when it is not on $PATH or in "
                         "$DT_LOCKCHECK_LEMMALOG.  Implies --why")
    args = ap.parse_args()

    # Check everything that can only be wrong about the invocation here, before
    # any output.  If these checks came after the json and lemmalog early
    # returns, `--rules bogus --format json` and `--why --format json` would
    # print their output and exit 0 instead of failing.
    wanted = set(args.rules.split(","))
    # ALL is upper-case so that it can never collide with a rule name, and so
    # that a lower-case "all" is an unknown rule, not a second spelling.  The
    # command line alone has to show which rules a run reported.
    if ALL_RULES_TOKEN in wanted:
        wanted = (wanted - {ALL_RULES_TOKEN}) | set(ALL_RULES)
    unknown = wanted - set(ALL_RULES)
    if unknown:
        die("unknown rule(s): %s.  Pick from %s, or %s for every one"
            % (", ".join(sorted(unknown)), ", ".join(ALL_RULES), ALL_RULES_TOKEN))
    if (args.why or args.lemmalog) and args.format != "report":
        die("--why adds proof trees to the report; --format %s cannot carry them"
            % args.format)
    # the false-positive flags contradict each other in three ways, and each
    # would otherwise fail silently: a suppression that was asked to be ignored
    # and consulted at once, a re-read with nothing to check against, or a
    # reason recorded nowhere
    if args.include_fp and (args.fail_on_stale or args.confirm):
        die("--include-false-positives reports what the list suppresses and "
            "consults it for nothing else; "
            "--fail-on-stale-false-positives-only and --confirm-false-positive "
            "both need the list applied")
    # the list applies to findings, and these two formats carry the facts the
    # rules run on, not findings.  The flag could only be a silent no-op there,
    # which is also why --why is an error with them
    if args.include_fp and args.format not in ("report", "csv"):
        die("--include-false-positives changes which findings are reported; "
            "--format %s carries the facts the rules run on, which the list "
            "never touches" % args.format)
    # a gate acts on findings, and json and lemmalog return the fact base before
    # the rules have produced any.  The run would exit 0 whatever the gate asked
    # about, so a CI job set up that way would pass without ever checking
    gate = ("--fail-on-findings" if args.fail_on_findings else
            "--fail-on-stale-false-positives-only" if args.fail_on_stale
            else None)
    if gate and args.format not in ("report", "csv"):
        die("%s fails on findings; --format %s carries the facts the rules run "
            "on and produces no finding for a gate to fire on" % (gate, args.format))
    # the two gates answer different questions, and one run cannot report both
    # answers in its exit status, so asking for both is a mistake, not a
    # conjunction.  --fail-on-findings already fails on everything the other
    # gate does
    if args.fail_on_findings and args.fail_on_stale:
        die("--fail-on-findings and --fail-on-stale-false-positives-only are "
            "two gates; pick one.  The first fails on any finding; the second "
            "fails only on a judgement the source has moved past")
    if args.confirm and (args.fail_on_findings or args.fail_on_stale):
        die("--confirm-false-positive rewrites the list and reports nothing, so "
            "there is no finding for a --fail-on-* gate to act on")
    if args.reason and not args.confirm:
        die("--false-positive-reason records why a finding is not a defect, "
            "so it needs a --confirm-false-positive to record it on")
    # One reason is one judgement, and a judgement is about one module.  Several
    # fields of the same module often share one, as rgblevels' state machine
    # does, but a sentence about one module cannot be true of another.  Without
    # a reason there is no such limit, and each entry keeps the reason it
    # already has.
    if args.reason:
        mods = {c.partition(":")[0] for c in args.confirm}
        if len(mods) > 1:
            die("--false-positive-reason is one judgement, so every "
                "--confirm-false-positive in the run has to name the same "
                "module; got %s.  Confirming across modules without a reason "
                "keeps each entry's own" % ", ".join(sorted(mods)))

    src, how = find_src(args.src)
    if src is None:
        sys.stderr.write("dt-lockcheck: cannot find darktable's src/iop.  Tried:\n")
        for what, why in how:
            sys.stderr.write("  %-34s %s\n" % (what, why))
        sys.stderr.write("Pass --src /path/to/darktable/src/iop "
                         "(or /path/to/darktable).\n")
        sys.exit(2)

    paths = iop_sources(src)
    sources = {_module_of(p): p for p in paths}

    analysed = {}
    for p in paths:
        r = analyse(p)
        if r:
            analysed[r["module"]] = r

    if args.module:
        # a name that is not a source at all and a source with no gui_data
        # struct are different mistakes, and neither may pass silently.
        # Otherwise, asking for one good and one bad name would analyse the good
        # one and exit 0
        missing = [m for m in args.module if m not in sources]
        if missing:
            die("no module named %s among the %d sources in %s"
                % (", ".join(sorted(missing)), len(paths), src))
        empty = [m for m in args.module if m not in analysed]
        if empty:
            die("no gui_data struct in %s" % ", ".join(
                sorted(os.path.basename(sources[m]) for m in empty)))
        res = {m: analysed[m] for m in sorted(set(args.module))}
    else:
        res = analysed
    if not res:
        die("no modules with a gui_data struct under %s" % src)

    if not args.quiet:
        # stderr, because stdout carries the csv, json or lemmalog output.
        # Naming the route shows when the wrong checkout is being analysed, and
        # naming the skipped count shows a module that would otherwise drop out
        # unnoticed.
        skipped = len(paths) - len(analysed)
        sys.stderr.write("dt-lockcheck: %d sources in %s (%s); %d with a gui_data "
                         "struct, %d skipped\n"
                         % (len(paths), src, how, len(analysed), skipped))

    if args.format == "json":
        json.dump(res, sys.stdout, indent=1)
        return
    if args.format == "lemmalog":
        print("\n".join(lemmalog_facts(res)))
        return

    fs = findings(res, wanted)

    # Consult the list after the rules ran, not before, because it suppresses
    # findings and does not change what the rules derive.  --format json and
    # lemmalog returned above, for the same reason that --rules does not affect
    # them.  They carry the facts, not the findings.
    entries, suppressed, orphans = [], [], []
    if not args.include_fp:
        entries = load_fp_list(FP_FILE)
        if entries or args.confirm:
            cache = {}
            for f in fs:
                f["key"] = finding_key(f, res, src, cache)
        if args.confirm:
            confirm_fps(args.confirm, args.reason, fs, entries, res, wanted,
                        FP_FILE)
            return
        if entries:
            fs, suppressed, orphans = apply_fp(fs, entries, res, wanted)
    stale = [f for f in fs if f.get("fp")]
    fp_banner(suppressed, stale, orphans, FP_FILE)

    # The narrow gate fails on the stale entries alone, so it reports only
    # those.  The other findings are not what it checks, and they usually far
    # outnumber the stale ones.  The banner above still gives the totals.
    reported = stale if args.fail_on_stale else fs

    trees = None
    if args.why or args.lemmalog:      # --lemmalog is only ever given to get trees
        exe, how = find_lemmalog(args.lemmalog)
        if not exe:
            die(how)
        if not args.quiet:
            sys.stderr.write("dt-lockcheck: proof trees from %s (%s)\n" % (exe, how))
        trees = why_trees(exe, res, reported)

    if args.format == "csv":
        w = csv.writer(sys.stdout)
        # `fp` is appended rather than inserted so a consumer that reads the
        # first six columns positionally still works
        w.writerow(["module", "field", "ctype", "rule", "locked_sites",
                    "unlocked_sites", "fp"])
        for f in reported:
            th = f["thread"]
            w.writerow([f["module"], f["field"], f["ctype"], f["rule"],
                        " ".join(site_str(a, th) for a in f["locked"]),
                        " ".join(site_str(a, th) for a in f["unlocked"]),
                        "stale" if f.get("fp") else ""])
    elif args.fail_on_stale:
        print_report(reported, trees, total=len(fs))
    else:
        print_report(fs, trees, len(suppressed))

    # exit 1 only on request, because a run that found something is still a
    # completed run.  The wide gate fails until every finding is dealt with.
    # The narrow one asks only whether the recorded judgements still hold, so a
    # CI job can pass it before then.
    if (args.fail_on_findings and fs) or (args.fail_on_stale and stale):
        sys.exit(1)


if __name__ == "__main__":
    # let `... | head` exit quietly instead of raising BrokenPipeError
    try:
        import signal
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (ImportError, AttributeError, ValueError):
        pass
    main()
