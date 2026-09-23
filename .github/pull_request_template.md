## Summary

<!--
Keep this to about five lines: what problem does the pull request solve, and
how? Say what the user gets, not how the change was written.
-->

## Referenced issue

<!--
Link the issue this pull request fixes or is related to, if any. Write
`Fixes #1234` or `Closes #1234` on its own line to auto-close the issue on
merge, or `Related: #1234` when the pull request does not resolve it.

Skip this section for small bug fixes that do not come from an issue.
Fixes and Closes line must be at the very end of the description.
-->

## Checklist

- [ ] I have read [CONTRIBUTING.md](../CONTRIBUTING.md) and the [coding style](https://github.com/darktable-org/darktable/wiki/Developer's-guide#coding-style).
- [ ] I have not merged master into the topic branch.
- [ ] The pull request is one logical change, and every commit compiles on its own.
- [ ] I ran the relevant tests: unit tests, `src/tests/integration/` where the pixelpipe is touched, or `darktable-cli` as a headless smoke test.
- [ ] New user-visible strings use `_()`, new preferences are registered in `data/darktableconfig.xml.in`.
- [ ] A `RELEASE_NOTES.md` entry was added (only needed if fixing an issue in a release). Do not reference GitHub issues.

## Test instructions

<!--
What we need to run to verify the change, and what you did not verify
yourself. Keep it to the point.
-->

## AI assistance

<!--
Only fill this in if the pull request was written with the help of an AI
coding agent. Keep it short and direct, developers cannot afford reading pages
of text for nothing. One or two lines: which agent was used, what it changed,
and what you verified yourself.
-->
