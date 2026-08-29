#!/usr/bin/env python3
"""dt-lockcheck — find gui_data fields that darktable IOP modules share between
the pixelpipe and the GTK main thread without consistent locking.

WHAT IT LOOKS FOR
  Every IOP module keeps per-instance GUI state in a `dt_iop_<module>_gui_data_t`
  struct reached as `g->field`.  Fields that both the pipe side and the GTK main
  thread touch must be accessed inside
  `dt_iop_gui_enter/leave_critical_section()` (the module's `gui_lock`), or a
  private mutex.  This script finds the fields where that is not done, or not
  done everywhere.

WHICH THREAD
  Every function is labelled `gtk`, `pipe` or `either`, and so is every site in
  the report.

    gtk     the GTK main thread: gui_init, gui_update, gui_changed, widget and
            draw callbacks, mouse and scroll handlers.
    pipe    a pixelpipe worker thread, never the main thread, for the instance
            that owns gui_data: process, process_cl, process_tiling*,
            modify_roi_in/out, tiling_callback, distort_mask, output_format.
    either  driven by the pipe, but GTK-thread code reaches it too, so it has no
            thread you can rely on: commit_params, init_pipe, cleanup_pipe,
            distort_transform, distort_backtransform, blend_colorspace,
            default_colorspace.  See the EITHER set below for why each is there.

  An `either` site counts on both sides of the cross-thread test, since two
  invocations can land on different threads.  A finding that rests only on such
  a site deserves a second look: on the path that matters it may in practice
  always run on one thread.

  Each label covers the static helpers reached from it, propagated along the
  call graph within one file -- which is where the mistakes hide, two or three
  frames below a callback whose name says nothing about its thread.

  Four rules, in descending order of how much they are worth reading:

    widget_from_pipe  a Gtk*-typed field touched from a `pipe` or `either`
                      function.  GTK must only be called from the main thread.
    violation         the module locks the field somewhere, and also accesses it
                      unlocked from the other side.  The module's own code is
                      the evidence that the field needs the lock.
    no_lock_share     the field is written and shared across the two threads and
                      the module never locks it at all.
    discipline_gap    the module locks the field somewhere and accesses it
                      unlocked, without a proven cross-thread share.  Noisiest;
                      read it last.

PROOF TREES (--why, off by default)
  The rules are also written as Datalog in rules.lemma, next to this script.
  With --why, each finding in the report is followed by the proof tree behind it:
  the locked site that established the discipline, the thread facts, and the
  unlocked site that broke it.  This needs the lemmalog engine
  (github.com/JordyZomer/lemmalog) on $PATH, in $DT_LOCKCHECK_LEMMALOG, or named
  by --lemmalog PATH; asking for --why without one is an error rather than a
  silent plain report.  The trees are additive: which findings are reported does
  not depend on them, and they roughly sextuple the length of a full report,
  which is why they are opt-in.

  The trees go into the report only.  --format csv/json/lemmalog never runs the
  engine and never carries a tree, so --why with one of those is an error too.
  The engine's output is captured rather than inherited, so it cannot leak into
  stdout; the line naming the binary goes to stderr.

HOW TO READ THE OUTPUT
  Each finding names the locked sites (the evidence a discipline exists) and the
  unlocked sites (the suspected defect), with file line numbers and the thread
  each function was inferred to run on.  Open both and decide.  This is a lexical
  analysis, not a verified race detector: every finding needs a human to confirm
  that the two sites can really run concurrently.

KNOWN FALSE-POSITIVE CAUSES (do not report these upstream without checking)
  * Mutual exclusion by protocol rather than by lock.  `rgblevels` hands work to
    the pipe through a state machine (0 idle / 1 requested / -1 running /
    2 ready); while the state is -1 the GTK side is excluded by the protocol, so
    the unlocked accesses in between are intentional.  The script cannot see this.
  * A lock passed into a callee.  Handled for the common
    `dt_dev_sync_pixelpipe_hash(..., &self->gui_lock, &g->hash)` shape, not in
    general.
  * A field that merely sits inside a critical section taken for a different
    field.  Partly handled: a function that writes >= 8 distinct fields under the
    lock and never reads them back is treated as bulk initialisation and
    establishes no per-field discipline.
  * A finding resting only on an `either` site -- see WHICH THREAD above.

KNOWN LIMITS
  * Lock depth is tracked lexically, so an early `leave_critical_section()` on one
    branch makes later code in other branches look unlocked.
  * Bug shapes needing dataflow inside a function are out of reach -- notably
    "pointer copied under the lock, dereferenced after releasing it".
  * Thread inference is name-based plus intra-file call-graph propagation.  A
    function reached only through a function pointer stored elsewhere, or from
    another translation unit, may come out as thread `None` and be skipped.
  * `g` is matched by convention, not by type.  A function that binds `g` to
    something other than the module's GUI data is skipped whole, so a real
    gui_data access in it is missed too.
  * Only `src/iop/*.c` and `*.cc`.  Shared code under `src/develop/` is not
    covered, and neither is `src/libs/`.

USAGE
  Run it from anywhere inside a darktable checkout, or with the script on $PATH:
  it locates src/iop by itself and reports the tree it picked on stderr.

  ./dt-lockcheck.py
  ./dt-lockcheck.py --module toneequal
  ./dt-lockcheck.py --rules violation,widget_from_pipe
  ./dt-lockcheck.py --format csv > findings.csv
  ./dt-lockcheck.py --format json > facts.json

  To analyse a tree other than the one you are standing in:

  ./dt-lockcheck.py --src /path/to/darktable/src/iop
  ./dt-lockcheck.py --src /path/to/darktable        # a checkout root works too

  $DT_LOCKCHECK_SRC does the same as --src, for CI.

  ./dt-lockcheck.py --module toneequal --why        # with the proof trees
  ./dt-lockcheck.py --why --lemmalog ~/lemmalog/target/release/lemmalog

  Python 3.8+, standard library only.  Exit status is 0 for a completed run
  whatever it found, and 2 for a wrong invocation.  There is deliberately no
  "exit non-zero on findings" mode: roughly one finding in three is a false
  positive, so a clean tree does not exist and such a gate could only ever be
  red.  See BEFORE YOU FILE ANYTHING.
"""
import argparse
import csv
import re, sys, os, glob, json, shutil, subprocess
from collections import defaultdict

# --- thread classification of the IOP entry points -------------------------
# Pipe-side: called by the pixelpipe on a worker thread.
# Pipeline callbacks that never reach the GTK main thread *for the instance that
# owns gui_data*: the three screen pipes each run on their own worker thread.
# (Instances in other develop contexts -- export, thumbnails, snapshots -- have
# gui_data == NULL, so they cannot touch the fields this tool looks at.)
PIPE = {
    "process", "process_cl", "process_tiling", "process_tiling_cl",
    "modify_roi_in", "modify_roi_out", "tiling_callback", "distort_mask",
    "output_format",
}
# Callbacks with no reliable thread affinity: the pipe drives them, but GTK-thread
# code reaches them too, so an access here may be on either thread.
#   commit_params      -- basecurve's init_pipe() calls it back, and the image
#                         switch rebuilds the screen pipes from an idle callback
#   init/cleanup_pipe  -- same node rebuild, same idle callback
#   distort_*transform -- mask handling and darkroom zoom call them directly
#   *_colorspace       -- the blend GUI and the colour picker call them directly
EITHER = {
    "commit_params", "init_pipe", "cleanup_pipe",
    "distort_transform", "distort_backtransform",
    "blend_colorspace", "default_colorspace",
}
# GTK-side: called from the main loop.
GTK = {
    "gui_init", "gui_update", "gui_cleanup", "gui_changed", "gui_focus",
    "gui_reset", "gui_post_expose", "gui_moved", "gui_button_pressed",
    "gui_button_released", "gui_scrolled", "gui_key_pressed",
    "mouse_moved", "button_pressed", "button_released", "scrolled",
    "reload_defaults", "color_picker_apply", "init_presets",
}
GTK_RE = re.compile(r"(_callback$|_clicked$|(^|_)draw$|(^|_)expose$|^_?gui_|_changed$|_toggled$|"
                    r"_press(ed)?$|_release(d)?$|_motion|_leave$|_enter$|_scroll|_activate$|draw)")

FUNC_HEAD = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*\t]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(")
FIELD_RE  = re.compile(r"\bg\s*->\s*([A-Za-z_][A-Za-z0-9_]*)")
CALL_RE   = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")

WRITE_RE = None  # built per field below
def is_write(src, f):
    """A write to g->f: assignment, compound assignment, ++/--, or address-taken."""
    if re.search(r"\bg\s*->\s*%s\s*(\+\+|--)" % f, src): return True
    if re.search(r"&\s*g\s*->\s*%s\b" % f, src):
        # ... unless the same call also hands the callee the lock, in which
        # case the callee locks and this is not an unsynchronised write
        if not re.search(r"&\s*\w+\s*->\s*gui_lock", src): return True
    if re.search(r"\bg\s*->\s*%s\s*(\[[^\]]*\])?\s*(\+|-|\*|/|\||&|\^)?=(?!=)" % f, src): return True
    # passed as a bare argument: the callee may write through the pointer/array
    if re.search(r"[(,]\s*g\s*->\s*%s\s*[,)]" % f, src): return True
    return False

ENTER = "dt_iop_gui_enter_critical_section"
LEAVE = "dt_iop_gui_leave_critical_section"
MUTEX_LOCK   = re.compile(r"\b(?:g_mutex_lock|pthread_mutex_lock|dt_pthread_mutex_lock)\s*\(\s*&?\s*g\s*->\s*(\w+)")
MUTEX_UNLOCK = re.compile(r"\b(?:g_mutex_unlock|pthread_mutex_unlock|dt_pthread_mutex_unlock)\s*\(\s*&?\s*g\s*->\s*(\w+)")

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
            head = " ".join(s.strip() for s in sig)
            m = None
            for m2 in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", head):
                m = m2
                break
            name = m.group(1) if m else None
            # brace-match to the closing '}' at column 0
            k = i + 1
            while k < n and not lines[k].startswith("}"):
                k += 1
            if name and not name in ("if", "for", "while", "switch", "typedef", "struct"):
                out.append((name, i + 1, k))
            i = k + 1
        else:
            i += 1
    return out

def gui_fields(text, module):
    m = re.search(r"typedef struct dt_iop_%s_gui_data_t\s*\{(.*?)\}\s*dt_iop_%s_gui_data_t" % (module, module),
                  text, re.S)
    if not m:
        m = re.search(r"typedef struct dt_iop_\w+_gui_data_t\s*\{(.*?)\}\s*dt_iop_\w+_gui_data_t", text, re.S)
    if not m:
        return set()
    body = m.group(1)
    fields = {}
    for line in body.splitlines():
        line = line.split("//")[0].strip().rstrip(";")
        if not line or line.startswith(("/*", "*", "#")):
            continue
        # strip the type, keep the declarator list
        for decl in line.split(","):
            d = decl.strip().rstrip(";")
            d = re.sub(r"\[[^\]]*\]", "", d)          # array bounds
            d = re.sub(r"\(.*", "", d)                 # function pointers
            names = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", d)
            if names:
                ty = names[0] if len(names) > 1 else "unknown"
                fields[names[-1]] = ty
    return fields

def classify(fnname):
    if fnname in PIPE:   return "pipe"
    if fnname in EITHER: return "either"
    if fnname in GTK:    return "gtk"
    return None

def analyse(path):
    module = re.sub(r"\.cc?$", "", os.path.basename(path))
    text = open(path, encoding="utf-8", errors="replace").read()
    lines = text.splitlines()
    fields = gui_fields(text, module)
    if not fields:
        return None
    funcs = split_functions(lines)
    accesses = []          # (fn, field, lockname|"none", line)
    callgraph = defaultdict(set)
    handler_refs = set()
    fnames = {f[0] for f in funcs}
    # `g` is only the module's GUI data by convention.  A handful of functions
    # bind it to something else (a gain map, a gaussian handle); their `g->x`
    # accesses are not gui_data accesses and must not be recorded.
    gui_ty = re.compile(r"\b\w*_gui_data_t\s*\*")
    other_g = re.compile(r"\b(\w+_t)\s*\*\s*(?:const\s+)?g\b")
    for (fn, s, e) in funcs:
        body = "\n".join(lines[max(0, s - 6):e])
        decls = other_g.findall(body)
        if decls and not any(d.endswith("_gui_data_t") for d in decls):
            continue
        depth = 0
        held = []
        for ln in range(s, e):
            src = lines[ln].split("//")[0]
            for c in CALL_RE.findall(src):
                if c in fnames and c != fn:
                    callgraph[fn].add(c)
            for ref in re.findall(r"[(,]\s*([A-Za-z_][A-Za-z0-9_]*)\s*[,)]", src):
                if ref in fnames and re.search(r"SIGNAL_CONNECT|SIGNAL_HANDLE|g_signal_connect|g_idle_add|"
                                               r"gtk_.*_set_.*_func|_draw_callback", src):
                    handler_refs.add(ref)
            if ENTER in src:
                depth += 1; held.append("gui_lock")
            for mm in MUTEX_LOCK.finditer(src):
                held.append(mm.group(1))
            # sorted, not bare set order: CPython randomises string hashing per
            # process, so an unsorted set here makes --format json/lemmalog come
            # out in a different order on every run and defeats diffing two runs
            for f in sorted(set(FIELD_RE.findall(src))):
                if f in fields:
                    lock = held[-1] if held else "none"
                    if lock == "none" and re.search(r"&\s*\w+\s*->\s*gui_lock", src):
                        lock = "callee_lock"
                    mode = "w" if is_write(src, f) else "r"
                    accesses.append((fn, f, lock, ln + 1, mode))
            if LEAVE in src:
                if held and held[-1] == "gui_lock": held.pop()
                depth = max(0, depth - 1)
            for mm in MUTEX_UNLOCK.finditer(src):
                if mm.group(1) in held: held.remove(mm.group(1))
    # A function that initialises many fields inside one critical section
    # establishes no per-field discipline: the lock is there for the batch,
    # not because any one of these fields needs it.  Mark those sites so the
    # rules do not read them as evidence of a convention.
    from collections import Counter
    bulk = defaultdict(set)
    reads = defaultdict(set)
    for (fn, f, lock, ln, mode) in accesses:
        if lock == "gui_lock":
            (bulk if mode == "w" else reads)[fn].add(f)
    # A function that also *reads* the fields it sets is updating state, not
    # initialising it, and its lock does count as evidence (toneequal's
    # update_curve_lut is the case this rules out).
    bulk_fns = {fn for fn, fields in bulk.items()
                if len(fields) >= 8 and not (fields & reads[fn])}
    accesses = [(fn, f, ("bulk_init" if fn in bulk_fns and lk == "gui_lock" else lk), ln, md)
                for (fn, f, lk, ln, md) in accesses]

    # propagate thread labels along the intra-file call graph
    thread = {fn: classify(fn) for (fn, _, _) in funcs}
    for fn in sorted(handler_refs):
        if thread.get(fn) is None:
            thread[fn] = "gtk"
    for fn in list(thread):
        if thread[fn] is None and GTK_RE.search(fn):
            thread[fn] = "gtk"
    changed = True
    rounds = 0
    while changed and rounds < 20:
        changed = False; rounds += 1
        for caller in sorted(callgraph):
            callees = callgraph[caller]
            if thread.get(caller):
                for c in sorted(callees):
                    if thread.get(c) is None:
                        thread[c] = thread[caller]; changed = True
                    elif thread[c] != thread.get(caller) and thread[c] in ("pipe", "gtk"):
                        thread[c] = "either"; changed = True
    return dict(module=module, fields=fields, accesses=accesses, thread=thread)



# Set-once functions: writes here are initialisation, not a shared-state update.
# init_pipe/cleanup_pipe are deliberately *not* in this set -- they run per pipe,
# and on either thread.
SETUP_FNS = {"gui_init", "gui_cleanup", "gui_reset"}

MAY_PIPE = ("pipe", "either")   # can run off the GTK main thread
MAY_GTK = ("gtk", "either")     # can run on the GTK main thread


def findings(res, wanted):
    """Apply the four rules to the extracted facts.

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
            # "either" counts on both sides: two invocations of one such callback
            # can land on different threads.
            shared = (any(t in MAY_PIPE for t in threads)
                      and any(t in MAY_GTK for t in threads))
            written = any(a[4] == "w" for a in live)
            # a locked site outside bulk initialisation is what establishes a
            # per-field convention
            locked = [a for a in sites if a[2] not in ("none", "bulk_init")]
            unlocked = [a for a in live if a[2] == "none"]
            widget = fields.get(field, "").startswith(("Gtk", "Dtgtk"))

            rule = None
            if widget and any(th.get(a[0]) in MAY_PIPE for a in sites):
                rule = "widget_from_pipe"
            elif locked and shared and unlocked:
                rule = "violation"
            elif shared and written and not locked:
                rule = "no_lock_share"
            elif locked and unlocked:
                rule = "discipline_gap"
            if rule and rule in wanted:
                out.append(dict(module=module, field=field, rule=rule,
                                ctype=fields.get(field, "?"),
                                locked=locked, unlocked=unlocked, thread=th))
    order = {"widget_from_pipe": 0, "violation": 1, "no_lock_share": 2, "discipline_gap": 3}
    out.sort(key=lambda f: (order[f["rule"]], f["module"], f["field"]))
    return out


def _pipe_first(sites, th):
    """Show pipe-thread sites first: the cross-thread half is the interesting one."""
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
    for m, r in res.items():
        for f, ty in sorted(r["fields"].items()):
            out.append(f'+ ftype("{m}", "{f}", "{ty}")')
        for fn, t in sorted(r["thread"].items()):
            if t:
                out.append(f'+ runs_on("{m}", "{fn}", "{t}")')
        # dedupe (the same access can be seen twice on one line) but emit in a
        # fixed order, for the same reason the field scan above is sorted
        for (fn, f, lock, ln, mode) in sorted({(a[0], a[1], a[2], a[3], a[4])
                                               for a in r["accesses"]}):
            out.append(f'+ access("{m}", "{f}", "{fn}", "{lock}", "{mode}")')
    return out


def site_str(a, th):
    return f"{a[0]}:{a[3]}[{th.get(a[0]) or '?'}]"


def print_report(fs, trees=None):
    by_rule = defaultdict(list)
    for f in fs:
        by_rule[f["rule"]].append(f)
    for rule in ("widget_from_pipe", "violation", "no_lock_share", "discipline_gap"):
        group = by_rule.get(rule)
        if not group:
            continue
        print(f"\n=== {rule}  ({len(group)}) " + "=" * (52 - len(rule)))
        for f in group:
            th = f["thread"]
            print(f"\n{f['module']}.c  g->{f['field']}   ({f['ctype']})")
            if f["locked"]:
                print("   locked  : " + ", ".join(site_str(a, th) + f"({a[2]})"
                                                  for a in _pipe_first(f["locked"], th)[:6]))
            print("   unlocked: " + ", ".join(site_str(a, th)
                                              for a in _pipe_first(f["unlocked"], th)[:8]))
            if trees:
                key = why_goal(f)[1]
                tree = trees.get(key)
                if tree:
                    # name the goal: the unlocked line above lists several sites
                    # and the tree only explains this one
                    print("   why     : " + key)
                    for line in tree:
                        print(ascii_safe("   " + line.rstrip()))
    print(f"\n{len(fs)} findings.")


# --- optional proof trees via lemmalog --------------------------------------
# The Datalog rules in rules.lemma restate the Python ones, so when the engine
# is installed the same findings can be re-derived there and each one printed
# with the base facts it rests on.  Purely additive: nothing here can change
# which findings are reported.
RULES_FILE = os.path.join(os.path.dirname(os.path.realpath(__file__)), "rules.lemma")
LEMMALOG_TIMEOUT = 120

# `pred(a, b)  (conf 1.000, prov [])` or `pred(a, b)  [unknown fact]`, at column
# zero; the tree body underneath is indented, and the engine's own progress
# chatter matches neither shape.
ANSWER_RE = re.compile(r"^([a-z_]+\([^)]*\))\s\s+(\(conf |\[unknown fact\])")
# every fact in this fact base is asserted flat, so these annotations are the
# same on every line and carry no information here
NOISE_RE = re.compile(r"\s+\(conf 1\.000, prov \[\]\)$")


def find_lemmalog(explicit):
    """The lemmalog binary.  Returns (path, how), or (None, why not).

    Same rule as --src: a path given explicitly never falls back to the search,
    so a typo is visible instead of quietly using some other binary.
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

    The two differ because lemmalog echoes a goal back with the quotes stripped,
    and the echo is what the answers have to be looked up by.  Arity follows the
    rule: no_lock_share is per field, the rest per unlocked site.
    """
    if f["rule"] == "no_lock_share":
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

    Returns {goal: [tree lines]}, empty on any failure -- the report is still
    worth printing without the trees, so nothing here is fatal.
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

    # Answers come back in query order, but key them by goal rather than by
    # position: a mismatch then degrades to a missing tree instead of pairing
    # the wrong proof with a finding.
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
# What identifies a checkout root.  imageop.h is the header declaring
# dt_iop_gui_enter_critical_section(), i.e. the thing this whole analysis is
# premised on; CMakeLists.txt confirms this is the tree root rather than some
# parent that merely happens to contain a src/iop.
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

    Accepts either the iop directory itself or a checkout root, since --src
    ~/darktable is the thing people type first.
    """
    if _is_root(d):
        d = os.path.join(d, "src", "iop")
    return d if len(iop_sources(d)) >= MIN_SOURCES else None


def find_src(explicit):
    """Locate src/iop.  Returns (path, how) on success, (None, tried) on failure.

    Order: an explicit --src is taken verbatim and never falls back, so a typo
    fails loudly instead of quietly analysing some other tree.  Otherwise
    $DT_LOCKCHECK_SRC, then a walk up from $PWD, then a walk up from the script
    itself.  $PWD comes before the script location deliberately: with the script
    symlinked onto $PATH out of checkout A, running it inside checkout B has to
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

    # realpath, not __file__: a symlink on $PATH has to resolve back into the
    # checkout the script actually lives in.
    for what, start in (("$PWD", os.getcwd()),
                        ("script", os.path.dirname(os.path.realpath(__file__)))):
        root = _walk_up(start)
        if root is None:
            tried.append(("above %s (%s)" % (what, start), "no darktable root up to /"))
            continue
        d = os.path.join(root, "src", "iop")
        if len(iop_sources(d)) >= MIN_SOURCES:
            return d, "darktable root above " + what
        tried.append(("above %s (%s)" % (what, start),
                      "found root %s, but no IOP sources in it" % root))
    return None, tried


def die(msg):
    """Exit 2 -- a wrong invocation, as opposed to 0 for a completed run."""
    sys.stderr.write("dt-lockcheck: %s\n" % msg)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(
        description="Find unsynchronised gui_data sharing in darktable IOP modules.",
        epilog="Findings are candidates, not confirmed bugs. See the module docstring "
               "for the known false-positive causes before filing anything.")
    ap.add_argument("--src",
                    help="darktable's src/iop, or a checkout root; by default it is "
                         "located from $DT_LOCKCHECK_SRC, the current directory, or "
                         "the script's own location")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="do not report the located source directory on stderr")
    ap.add_argument("--module", action="append",
                    help="restrict to this module (repeatable), e.g. --module toneequal")
    ap.add_argument("--rules", default="widget_from_pipe,violation,no_lock_share",
                    help="comma-separated rules to report; add discipline_gap for the "
                         "noisy tier (default: %(default)s)")
    ap.add_argument("--format", choices=("report", "csv", "json", "lemmalog"),
                    default="report",
                    help="report: human-readable; csv: one row per finding; "
                         "json: the raw extracted facts; lemmalog: base facts for a "
                         "Datalog engine.  Proof trees are added to report only; the "
                         "other three are unaffected by lemmalog")
    ap.add_argument("--why", action="store_true",
                    help="append the lemmalog proof tree behind each finding; needs "
                         "the lemmalog engine, and is an error if it cannot be found")
    ap.add_argument("--lemmalog", metavar="PATH",
                    help="the lemmalog binary to use, when it is not on $PATH or in "
                         "$DT_LOCKCHECK_LEMMALOG.  Implies --why")
    args = ap.parse_args()

    src, how = find_src(args.src)
    if src is None:
        sys.stderr.write("dt-lockcheck: cannot find darktable's src/iop.  Tried:\n")
        for what, why in how:
            sys.stderr.write("  %-34s %s\n" % (what, why))
        sys.stderr.write("Pass --src /path/to/darktable/src/iop "
                         "(or /path/to/darktable).\n")
        sys.exit(2)

    paths = iop_sources(src)
    if not args.quiet:
        # stderr: stdout carries the csv/json/lemmalog output.  Naming the route
        # is what makes analysing the wrong checkout obvious.
        sys.stderr.write("dt-lockcheck: %d sources in %s (%s)\n" % (len(paths), src, how))

    res = {}
    for p in paths:
        r = analyse(p)
        if r and (not args.module or r["module"] in args.module):
            res[r["module"]] = r
    if not res:
        if args.module:
            die("no module named %s among the %d sources in %s"
                % (", ".join(args.module), len(paths), src))
        die("no modules with a gui_data struct under %s" % src)

    if args.format == "json":
        json.dump(res, sys.stdout, indent=1)
        return
    if args.format == "lemmalog":
        print("\n".join(lemmalog_facts(res)))
        return

    wanted = set(args.rules.split(","))
    unknown = wanted - {"widget_from_pipe", "violation", "no_lock_share", "discipline_gap"}
    if unknown:
        die(f"unknown rule(s): {', '.join(sorted(unknown))}")
    fs = findings(res, wanted)

    trees = None
    if args.why or args.lemmalog:      # --lemmalog is only ever given to get trees
        if args.format != "report":
            die("--why adds proof trees to the report; --format %s cannot carry them"
                % args.format)
        exe, how = find_lemmalog(args.lemmalog)
        if not exe:
            die(how)
        if not args.quiet:
            sys.stderr.write("dt-lockcheck: proof trees from %s (%s)\n" % (exe, how))
        trees = why_trees(exe, res, fs)

    if args.format == "csv":
        w = csv.writer(sys.stdout)
        w.writerow(["module", "field", "ctype", "rule", "locked_sites", "unlocked_sites"])
        for f in fs:
            th = f["thread"]
            w.writerow([f["module"], f["field"], f["ctype"], f["rule"],
                        " ".join(site_str(a, th) for a in f["locked"]),
                        " ".join(site_str(a, th) for a in f["unlocked"])])
    else:
        print_report(fs, trees)


if __name__ == "__main__":
    # let `... | head` exit quietly instead of raising BrokenPipeError
    try:
        import signal
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    except (ImportError, AttributeError, ValueError):
        pass
    main()
