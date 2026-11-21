# Translating darktable

This guide explains how to translate darktable's user interface.

darktable uses the GNU **gettext** system. Translatable strings live in the `po/` directory:

- `po/darktable.pot`: List of all english source strings. This is updated by the maintainers periodically.
- `po/<lang>.po` the actual **translation files** per language, named with the language code (`fr`, `de`, …) or language + region where needed (`pt_BR`, `zh_CN`, `ko_KR`). This is the file you edit.

At build time each `<lang>.po` is compiled into a binary `<lang>.mo` that darktable loads at runtime to replace the English text.

## General setup

French translations are maintained through [Weblate](https://hosted.weblate.org/) (see below), all other translations are maintained by editing locally on your machine and opening a PR against the repository.

## Before you start

**Coordinate with the existing translators of your language** before doing large amounts of work, so you don't duplicate effort or clash with work in progress. Also this helps to avoid merge conflicts when pushing updated po-files. 

When working on the translations take great care to keep the nomenclature consistent within darktable. 

## Updating an existing translation

*(French is handled via Weblate. See below. The steps below are for the other languages.)*

1. Get the two files from the repository and save them in the same directory: your language's [`po/<lang>.po`](https://github.com/darktable-org/darktable/tree/master/po) and the current [`po/darktable.pot`](https://github.com/darktable-org/darktable/blob/master/po/darktable.pot).
2. Open `<lang>.po` in Poedit.
3. Pull in new and changed source strings: **Translation → Update from POT file…** (older Poedit: **Catalog → Update from POT file…**) and select `darktable.pot`.
4. Translate the untranslated entries and review the **fuzzy** ones (automatic guesses that need checking).
5. Save. Poedit writes the updated `<lang>.po`.
6. Open a **pull request** for the updated `<lang>.po`.

## Starting a new language

1. Choose the code: ISO 639-1 two-letter (e.g. `fr`), with a region suffix only where it matters (`pt_BR`, `zh_CN`).
2. In Poedit, create a new translation from `darktable.pot` and save it as `po/<lang>.po`.
3. Add your `<lang>` code to [`po/LINGUAS`](https://github.com/darktable-org/darktable/blob/master/po/LINGUAS) so the translations in the `.desktop` and AppData/metainfo files are included as well. (The UI build itself picks up any `po/*.po` automatically.)
4. Open a pull request with the new `po/<lang>.po` (and the `LINGUAS` change).

## Getting your translation into a release

To be shipped in an official build a language must meet these criteria:

- no more than **20 untranslated** strings
- no more than **50 fuzzy** strings

Keeping your translation up to date throughout the cycle avoids a large chunk of work right before a release.

## Testing your translation locally

1. Save your work in Poedit (**File → Save**); this produces `<lang>.mo` next to `<lang>.po`.
2. Rename that file to `darktable.mo` and copy it into the `locale` directory of your darktable installation, under `<lang>/LC_MESSAGES/`, for example:
   - **macOS:** `/Applications/darktable.app/Contents/Resources/share/locale/<lang>/LC_MESSAGES/`
   - **Linux:** `<install-prefix>/share/locale/<lang>/LC_MESSAGES/` (e.g. `/usr/share/locale/...` or `/opt/darktable/share/locale/...`)
3. Launch darktable and select your language under **Settings → General → Interface Language**.

## Keeping PO diffs readable (optional)

PO files carry source-line references that change often and make diffs noisy. You can filter them out of `git diff` with a textconv:

```sh
git config diff.po.textconv "msgcat --no-location --no-wrap --sort-output"
```

## Using Weblate (French only)

French translations are maintained through [Weblate](https://hosted.weblate.org/projects/darktable/) to facilitate collaborative translations without merge conflicts. You need to register with weblate to translate there. 

The workflow for weblate is: Weblate pulls an updated POT automatically, translations happen on weblate and are pushed via PR roughly every 24 hours. 

## For maintainers: regenerating the template (`po/darktable.pot`)

> **Note.** Translators normally do **not** regenerate the template — you use the `darktable.pot` that is committed in the repository. This section is for maintainers keeping that template up to date.

To regenerate the template:

1. Configure and build darktable, so the generated sources exist under `build/`.
2. From the `po/` directory, run:

   ```sh
   intltool-update --gettext-package=darktable --pot
   ```

   The `--gettext-package=darktable` is required; otherwise the output is written to `untitled.pot` and the real template is left unchanged.
