[wiki](https://github.com/darktable-org/darktable/wiki/Translations)
# Introduction

darktable's user interface can be translated into many languages. 

In short, to support a translation, the build process needs a xx.po file with the two-letter language code, e.g. fr.po.

To make sure your translation makes it into an official build:
- subscribe to the relevant [mailing list](https://www.darktable.org/contact/)
- ensure you keep your translation up to date to avoid large work just before the release
- entry criteria:
  - no more than 20 untranslated strings 
  - no more than 50 fuzzy strings 

## How darktable uses the .po files

The application uses gettext engine for translation, and it works like this, a text based editable PO file contains a number of sequences containing: 
- a line in English (source language)
- a line in target language, and 
- a reference to a line in source code where original text in English comes from. 

The build system then creates a binary version of each PO file with MO file extension and places it in a system directory where a running instance of darktable will pick it up and use it to replace English text with target language text.

## How to contribute your translation:
- Send your po file via email (see `(officially) Translate the darktable User Interface for darktable releases (how-to, no git, just email)` below)
- Submit a pull request with your po file

# Translate the darktable User Interface for darktable releases

1. Get the PO file for your language from the repository -> [po](../po)

2. To support making release translations easier, the darktable build process automatically creates a PO template (.POT) file, `darktable.pot` to use as base.

3. Get the latest [darktable.pot](../po/darktable.pot) file.

4. Put both files on the same directory. Open the po file with a po file editor, e.g. poedit:
    ```bash
    $ poedit <your_lang>.po
    $ poedit darktable.pot
    ```
5. Update the PO file using the POT file: go to menu `[Catalog]` ->
`[Update from POT file...]` and select `darktable.pot` file.

6. Start translating. Saving will update `<lang>.po`.

7. When you are done, `bzip2` your PO file and send it to the [mailing list](https://www.darktable.org/contact/) or per instructions on the [wiki](https://github.com/darktable-org/darktable/wiki/Translations).

8. That's all!
