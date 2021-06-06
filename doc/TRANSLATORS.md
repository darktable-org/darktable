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

## How to contribute:
- Send your po file via email (see `(officially) Translate the darktable User Interface for darktable releases (how-to, no git, just email)` below)
- Submit a pull request with your po file (see `Translate the darktable User Interface for darktable releases (how-to, git)`)
- 

# (officially) Translate the darktable User Interface for darktable releases (how-to, no git, just email)

1. Get the PO file for your language from the repository -> [po](../po)

2. To support making release translations easier, the darktable build process automatically creates a PO template (.POT) file, `darktable.pot` to use as base.

3. Get the latest [darktable.pot](../po/darktable.pot) file.

4. Put both files on the same directory. Open the po file with a po file editor, e.g. poedit:
    ```
    `$ poedit <lang>.po`
    ```
5. Update the PO file using the POT file: go to menu `[Catalog]` ->
`[Update from POT file...]` and select `darktable.pot` file.

6. Start translating. Saving will update `<lang>.po`.

7. When you are done, `bzip2` your PO file and send it to the [mailing list](https://www.darktable.org/contact/) or per instructions on the [wiki](https://github.com/darktable-org/darktable/wiki/Translations).

8. That's all!

## Translate the darktable User Interface for darktable releases (how-to, git)

1. Fork darktable
2. Fetch source code from your repository, preferably using git, and set darktable as upstream e.g.: 
    ```
    git clone git://github.com/<some-user-profile>/darktable.git

    ```
3. Set original repository as upstream ans sync master [relevant guide](https://ardalis.com/syncing-a-fork-of-a-github-repository-with-upstream/): 
    ```
    [first time only] git remote add upstream https://github.com/darktable-org/darktable.git
    $ git checkout master
    $ git fetch upstream
    $ git merge upstream/master
    $ git push
    ``` 
4. Create a branch for the target language if there isn't one already.
    ```
    $ git checkout -b <target language>
    ```
5. The latest [darktable.pot](../po/darktable.pot) file should now be available. To support making release translations easier, the darktable build process automatically creates the PO template (.POT) file, `darktable.pot` to use as base.

6. Get the PO file for the target language from the repository -> [po](../po)
    
    >If it doesn't exist, just create it using your po file editor, and [darktable.pot](../po/darktable.pot)
    >
    >Convention is to name these with the two-letter language code ```<lang>.po```

7. The po file must be in the po folder. Open the po file with a po file editor, e.g. poedit:
    ```
    $ poedit <lang>.po
    ```
8.  Update the PO file using the POT file: go to menu `[Catalog]` ->
`[Update from POT file...]` and select `darktable.pot` file.

6. Start translating. Saving will update `<lang>.po`.
7. Commit your changes often.
8. When done, sync your repository with upstream master, and push your changes to your remote branch fork on github:
   ```  
    $ git checkout master
    $ git fetch upstream
    $ git merge upstream/master
    $ git push
    $ git checkout -b <target language>
    $ git fetch upstream
    $ git merge upstream/master
   ```

9. Create pull request on https://github.com/darktable-org/darktable using Compare accross forks".

# (old) How to translate darktable's User Interface during development

To start working on your translation you need to:

1. Fetch source code, preferably using git. Here is the magic command:

    ```
    git clone git://github.com/darktable-org/darktable.git
    ```

    Copy the checked out source code tree somewhere to work on it or, if you know git well enough, create your own branch and work on it.

2. Build darktable and install it:
    
    * See [README.md](README.md) for general build instructions.
    * For Windows builds, see [BUILD.md](/packaging/windows/BUILD.md). 

3. Create a new PO file for your language. For this you will need a package called `intltool` and some PO editor, preferably poedit(formerly known as poEdit).

    ```
    $ cd darktable/po/
    $ intltool-update -pot
    ```

    This will create a translation template with POT file extension. Now start poedit and in the menu choose "File > New catalog from POT file...". Point it to the newly created POT file.

    The next thing that will happen is that you will be asked to define various settings. All of them are pretty much self-explanatory. It should be noted here that you don't have to fill in "Team" and "Team's email address" fields. darktable doesn't currently use plural forms anywhere, but if you use `Lokalize` instead of `poedit`, this field will be automagically filled for you.

    Then you will be asked to save the newly created PO file. Give it a name like fr.po for French translation, de.po for German translation and so on. If you are not sure about the two-letter code for your language, use the one that local websites in your native language use.

4. In the PO editor you will see a list of all translatable messages, a field that contains an original message in English and a field to input your translation. The best practice is to copy the original message into translation field and then translate all the translatable bits. That way you won't forget about some markup we use. It's also recommended in poedit to go to "Edit > Preferences" and in the "General" tab check the "Always change focus to text input field" checkbox.

5. The best practice is to frequently test your changes. To do this, first open the LINGUAS file with your favorite text editor(not poedit) and add the two-letter code for your language there. Save, quit. Now you only have to run `make` and `sudo make install` again and restart darktable to see your changes.

6. If you don't understand some terminology, refer to existing translations from UFRaw or Rawstudio or digiKam or ask in the mailing list.

7. When you are done, `bzip2` your PO file and send it to the mailing list.

Now about updates. Every new version of darktable contains new functionality and thus there are always new messages to translate. It's best to keep track of changes, but even so we announce strings freeze a few days before releasing. It means that no changes to translatable messages will be added, so your translation created during that period will work fine.

To update your translation from working your tree:

1. Switch to `master` branch by:

    ```
    $ git checkout master
    ```

2. Update and merge changes from `master` branch:

    ```
    $ git pull --rebase
    ```

3. Run

    ```
    $ cd po/
    $ intltool-update --pot
    ```

4. Run

    ```
    $ intltool-update your_PO_file
    ````

    Or open your PO file in poedit and in the menu choose "Catalog > Update from POT file" and point it to the newly created file.

5. Update translation, send it to the list.

    That's all.


---

# Some git magic:


This might be handy if you want to send a patch instead of the whole file:

Create a new branch `yourlanguage`:

```
$ git checkout -b yourlanguage
```

Then make your translation changes as above,

```
$ git add po/XX.po
$ git commit -a
```

In case the `master` branch changed, do a

```
$ git pull
```

If your master and `origin/master` diverged:

```
$ git checkout master
$ git reset --hard origin master
```

Rebase your patch to the current `master`:

```
$ git checkout yourlanguage
$ git rebase master
```

Create patches relative to current `master`:

```
$ git format-patch master
```

:)
