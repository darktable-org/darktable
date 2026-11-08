#!/usr/bin/env python3
"""dt-lockcheck-diff reports the dt-lockcheck findings that a change adds,
removes or alters.

dt-lockcheck reports every suspicious gui_data access in src/iop, and on a
current tree nearly all of them predate any given change.  A reviewer or a CI
job needs the difference: this script runs dt-lockcheck on the base and on the
head of a change, and prints only the findings that differ between the two.
Findings that predate the change cancel out, so the comparison can gate a
change before every existing finding has been reviewed.

Findings are matched by (module, field).  dt-lockcheck reports a field under
one rule at most, so a field that moves to another rule is one changed
finding.  Sites are compared as counts per function, lock state and thread,
without line numbers, so code that only moved is not a difference.  One
dt-lockcheck.py and one false-positives.json judge both trees, so a change to
the rules or to the list acts on both sides alike.

A finding is reported when an ordinary dt-lockcheck run would print it: its
rule is a default rule, and no current false-positive entry suppresses it.  A
finding the head reports and the base does not is newly reported, whether it
is new, moved into a default rule, or lost its false-positive entry because
the change edited the code the entry was judged on.  --fail-on-new-findings
fails on those alone.  More unlocked accesses to a finding the base already
reports are flagged but never fail the run, because the change did not
introduce that finding.  See "Comparing two trees" in README.md.
"""

import argparse
import collections
import csv
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
SITE = re.compile(r"^(?P<fn>.+):(?P<line>\d+)\[(?P<thread>[^\]]*)\]$")

NEW = "newly reported"
MORE = "more unlocked accesses"
README_HINT = "see 'Comparing two trees' in tools/dt-lockcheck/README.md"


class Failure(Exception):
    pass


def _git(repo, *args):
    proc = subprocess.run(["git", "-C", repo, *args],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise Failure(f"git {' '.join(args)}: {proc.stderr.decode().strip()}")
    return proc.stdout


def _commit(repo, ref):
    try:
        return _git(repo, "rev-parse", "--verify", "--quiet",
                    f"{ref}^{{commit}}").decode().strip()
    except Failure:
        raise Failure(f"{ref}: not a commit in {repo}") from None


def _extract_iop(repo, commit, dest):
    # git archive rather than a worktree or checkout: the comparison is
    # read-only, and a worktree would register itself in the repository's .git
    os.mkdir(dest)
    archive = _git(repo, "archive", commit, "src/iop")
    proc = subprocess.run(["tar", "-x", "-C", dest], input=archive,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        raise Failure(f"tar: {proc.stderr.decode().strip()}")
    return os.path.join(dest, "src", "iop")


def _default_rules(lockcheck):
    # taken from the dt-lockcheck.py that judges both trees, so that a rule
    # added to it or moved out of the default set needs no edit here
    sys.dont_write_bytecode = True     # leave no __pycache__ beside the tool
    spec = importlib.util.spec_from_file_location("dt_lockcheck", lockcheck)
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
        return set(mod.DEFAULT_RULES)
    except Exception as e:
        raise Failure(f"cannot read DEFAULT_RULES from {lockcheck}: {e}") from None


def _lockcheck_csv(lockcheck, src, *extra):
    # --rules ALL although discipline_gap is too noisy to read tree-wide: its
    # unchanged findings cancel out like every other rule's, and a field that
    # moves from it into a default rule has to be seen moving
    proc = subprocess.run([sys.executable, lockcheck, "-q", "--src", src,
                           "--rules", "ALL", "--format", "csv", *extra],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True)
    if proc.returncode != 0:
        raise Failure(f"dt-lockcheck on {src} exited {proc.returncode}:\n"
                      f"{proc.stderr.strip()}")
    return list(csv.DictReader(io.StringIO(proc.stdout)))


def _lockcheck(lockcheck, src):
    # two runs, because neither carries both answers: --include-false-positives
    # lists every finding but drops the csv's `stale` marker, and a normal run
    # keeps the marker but leaves the suppressed findings out
    reported = {(r["module"], r["field"], r["rule"]): r["fp"]
                for r in _lockcheck_csv(lockcheck, src)}
    findings = {}
    for row in _lockcheck_csv(lockcheck, src, "--include-false-positives"):
        key = (row["module"], row["field"])
        # matching by field relies on dt-lockcheck giving a field one rule at
        # most; if that ever changes, two findings would silently merge here
        if key in findings:
            raise Failure(f"dt-lockcheck reported {key[0]} g->{key[1]} under two "
                          f"rules, which this comparison cannot match")
        sites = collections.defaultdict(list)
        for kind in ("locked", "unlocked"):
            for site in row[f"{kind}_sites"].split():
                m = SITE.match(site)
                if not m:
                    raise Failure(f"unexpected site {site!r} in dt-lockcheck output")
                sites[(kind, m["fn"], m["thread"])].append(int(m["line"]))
        findings[key] = {"rule": row["rule"], "ctype": row["ctype"],
                         "fp": reported.get((row["module"], row["field"], row["rule"]),
                                            "current"),
                         "sites": sites}
    return findings


def _reported(f, default_rules):
    """Whether an ordinary dt-lockcheck run would print this finding."""
    return bool(f) and f["rule"] in default_rules and f["fp"] != "current"


def _count(f, site):
    return len(f["sites"].get(site, [])) if f else 0


def _compare(base, head, default_rules):
    # the head's order first: dt-lockcheck sorts its output most-worth-reading first
    keys = list(head) + [k for k in base if k not in head]
    diffs = []
    for key in keys:
        b, h = base.get(key), head.get(key)
        all_sites = set(b["sites"] if b else ()) | set(h["sites"] if h else ())
        sites = [s for s in sorted(all_sites) if _count(b, s) != _count(h, s)]
        if (b and h and not sites and b["rule"] == h["rule"]
                and b["ctype"] == h["ctype"] and b["fp"] == h["fp"]):
            continue
        verdict = None
        if _reported(h, default_rules):
            if not _reported(b, default_rules):
                verdict = NEW
            elif any(s[0] == "unlocked" and _count(h, s) > _count(b, s) for s in sites):
                verdict = MORE
        diffs.append({"key": key, "b": b, "h": h, "sites": sites, "verdict": verdict})
    return diffs


def _made_stale(d):
    return bool(d["b"] and d["h"] and d["b"]["fp"] == "current"
                and d["h"]["fp"] == "stale")


def _lines(f, site):
    return ", ".join(map(str, f["sites"].get(site, []))) if f else ""


def _verdict_line(diffs):
    n_new = sum(d["verdict"] == NEW for d in diffs)
    n_more = sum(d["verdict"] == MORE for d in diffs)
    if not (n_new or n_more):
        return "nothing newly reported"
    return f"{n_new} {NEW}, {n_more} already reported with {MORE}"


def _report(diffs, n_base, n_head, base_label, head_label):
    out = [f"dt-lockcheck-diff, all rules: {base_label} -> {head_label}",
           f"{n_base} findings on base, {n_head} on head, known false positives "
           f"included; {len(diffs)} differ"]
    for d in diffs:
        (module, field), b, h = d["key"], d["b"], d["h"]
        status = "new" if not b else "gone" if not h else "changed"
        f = h or b
        tag = f"  [{d['verdict']}]" if d["verdict"] else ""
        out.append("")
        out.append(f"{status:8} {module}  g->{field}  ({f['ctype']})  {f['rule']}{tag}")
        if b and h and b["rule"] != h["rule"]:
            out.append(f"    rule: {b['rule']} -> {h['rule']}")
        if b and h and b["ctype"] != h["ctype"]:
            out.append(f"    type: {b['ctype']} -> {h['ctype']}")
        if (b["fp"] if b else "") != (h["fp"] if h else ""):
            entry = ", ".join(f"{x['fp']} on {side}" for side, x in
                              (("base", b), ("head", h)) if x and x["fp"])
            out.append(f"    false-positive entry: {entry}")
            if h and h["fp"] == "stale":
                edited = ("the change edited that code" if _made_stale(d) else
                          "the code it was judged on differs from the head")
                # the recorded reason is in dt-lockcheck's report format only,
                # not in the csv this script reads
                out.append(f"      somebody judged this finding harmless and {edited};"
                           f" read the recorded reason with:"
                           f" dt-lockcheck.py --module {module}")
            elif h and h["fp"] == "current":
                out.append("      the head matches a recorded judgement that this is"
                           " not a defect, so a normal dt-lockcheck run does not"
                           " report it")
        for s in d["sites"]:
            kind, fn, thread = s
            nb, nh = _count(b, s), _count(h, s)
            where = "   ".join(p for p in (f"base: {_lines(b, s)}" if nb else "",
                                           f"head: {_lines(h, s)}" if nh else "") if p)
            out.append(f"    {kind:9} {fn}[{thread}]  {nb} -> {nh}   {where}")
    out.append("")
    out.append(_verdict_line(diffs) if diffs else "no differences")
    return out


def _source_file(iop_dir, module):
    # the csv names the module, not the file; dt-lockcheck reads *.c and *.cc
    for ext in (".c", ".cc"):
        if os.path.isfile(os.path.join(iop_dir, module + ext)):
            return f"src/iop/{module}{ext}"
    return None


def _anchor(d):
    """The head line an annotation points at: preferably an unlocked access the
    change added.  Sites are compared as counts, so which line of a function is
    the added one is unknown, and the first line of that function stands in."""
    h = d["h"]
    grown = [s for s in d["sites"] if _count(h, s) > _count(d["b"], s)]
    for pool in ([s for s in grown if s[0] == "unlocked"],
                 [s for s in h["sites"] if s[0] == "unlocked"],
                 grown, list(h["sites"])):
        if pool:
            return min(line for s in pool for line in h["sites"][s])
    return None


def _unlocked_fns(d, only_grown):
    h = d["h"]
    fns = [f"{s[1]}[{s[2]}]" for s in sorted(h["sites"]) if s[0] == "unlocked"
           and (not only_grown or _count(h, s) > _count(d["b"], s))]
    return ", ".join(fns) or "none"


def _gh_escape(s, prop=False):
    # GitHub workflow commands percent-encode these; property values also
    # need ':' and ',' encoded, because those separate the properties
    s = s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if prop:
        s = s.replace(":", "%3A").replace(",", "%2C")
    return s


def _annotations(diffs, head_iop, gate):
    out = []
    for d in diffs:
        if not d["verdict"]:
            continue
        (module, field), b, h = d["key"], d["b"], d["h"]
        if d["verdict"] == NEW:
            level = "error" if gate else "warning"
            if _made_stale(d):
                msg = (f"the change edited code under a recorded false positive "
                       f"({h['rule']}). Read the recorded reason with "
                       f"'dt-lockcheck.py --module {module}', then fix the code "
                       f"or re-confirm the entry; {README_HINT}")
            else:
                moved = f", was {b['rule']}" if b and b["rule"] != h["rule"] else ""
                msg = (f"newly reported under {h['rule']}{moved}; unlocked in "
                       f"{_unlocked_fns(d, False)}. Fix it, or record why it is "
                       f"not a defect; {README_HINT}")
        else:
            level = "warning"
            msg = (f"already reported under {h['rule']}; the change adds unlocked "
                   f"accesses in {_unlocked_fns(d, True)}")
        props = [f"title={_gh_escape(f'dt-lockcheck: {module} g->{field}', True)}"]
        path, line = _source_file(head_iop, module), _anchor(d)
        if path:
            props.insert(0, f"file={_gh_escape(path, True)}")
            if line:
                props.insert(1, f"line={line}")
        out.append(f"::{level} {','.join(props)}::{_gh_escape(msg)}")
    return out


def _write_summary(path, diffs, report):
    with open(path, "a", encoding="utf-8") as f:
        f.write("### dt-lockcheck-diff\n\n")
        f.write(f"**{_verdict_line(diffs)}**\n\n")
        f.write("```text\n" + "\n".join(report) + "\n```\n")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0].replace("\n", " "),
        epilog="Like `git diff BASE...HEAD`, the base side is the merge base of"
               " BASE and the head.  Line numbers under \"base:\" are into that"
               " merge base, under \"head:\" into the head (the working tree,"
               " untracked files included, when --head is not given).  Exit"
               " status: 0 when the comparison ran and no gate fired; 1 when"
               " --fail-on-new-findings fired; 2 on any error.")
    parser.add_argument("base", metavar="BASE",
                        help="the base of the change: branch, tag or commit")
    parser.add_argument("--head", metavar="REF",
                        help="the commit under review; by default the working tree")
    parser.add_argument("--lockcheck", metavar="PATH",
                        default=os.path.join(HERE, "dt-lockcheck.py"),
                        help="the dt-lockcheck.py that judges both trees, with its"
                             " false-positives.json (default: the one beside"
                             " this script)")
    parser.add_argument("--repo", metavar="DIR", default=".",
                        help="the darktable checkout under review (default: the"
                             " current directory)")
    parser.add_argument("--scratch", metavar="DIR",
                        help="where to extract the trees; a temporary directory is"
                             " created inside it and removed afterwards (default:"
                             " the system temp dir)")
    parser.add_argument("--fail-on-new-findings", action="store_true",
                        help="exit 1 when the head reports a finding that the base"
                             " does not, for CI.  More unlocked accesses to a"
                             " finding the base already reports do not count")
    parser.add_argument("--github", action="store_true",
                        help="for GitHub Actions: emit one annotation per flagged"
                             " finding, and append the report to the job summary"
                             " when $GITHUB_STEP_SUMMARY is set")
    args = parser.parse_args()

    scratch = None
    try:
        if not os.path.isfile(args.lockcheck):
            raise Failure(f"--lockcheck {args.lockcheck}: no such file")
        default_rules = _default_rules(args.lockcheck)
        repo = _git(args.repo, "rev-parse", "--show-toplevel").decode().strip()
        named_base = _commit(repo, args.base)
        head = _commit(repo, args.head) if args.head else None
        try:
            base = _git(repo, "merge-base", named_base, head or "HEAD").decode().strip()
        except Failure:
            raise Failure(f"{args.base} and {args.head or 'HEAD'} have no merge"
                          f" base") from None

        scratch = tempfile.mkdtemp(prefix="dt-lockcheck-diff.", dir=args.scratch)
        base_findings = _lockcheck(args.lockcheck,
                                   _extract_iop(repo, base, os.path.join(scratch, "base")))
        if head:
            head_iop = _extract_iop(repo, head, os.path.join(scratch, "head"))
            head_label = head[:10]
        else:
            head_iop = os.path.join(repo, "src", "iop")
            head_label = "working tree"
        head_findings = _lockcheck(args.lockcheck, head_iop)

        base_label = (base[:10] if base == named_base
                      else f"{base[:10]} (merge base of {args.base})")
        diffs = _compare(base_findings, head_findings, default_rules)
        report = _report(diffs, len(base_findings), len(head_findings),
                         base_label, head_label)
        print("\n".join(report))
        if args.github:
            # before the scratch tree goes: annotations look up each module's file
            for line in _annotations(diffs, head_iop, args.fail_on_new_findings):
                print(line)
            summary = os.environ.get("GITHUB_STEP_SUMMARY")
            if summary:
                _write_summary(summary, diffs, report)
            else:
                print("dt-lockcheck-diff: $GITHUB_STEP_SUMMARY is not set; no job"
                      " summary written", file=sys.stderr)
        if args.fail_on_new_findings and any(d["verdict"] == NEW for d in diffs):
            return 1
        return 0
    except (Failure, OSError) as e:
        print(f"dt-lockcheck-diff: {e}", file=sys.stderr)
        return 2
    finally:
        if scratch:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
