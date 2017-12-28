# darktable User Interface Translation How-To

darktable's user interface can be translated into many languages. For this the application uses gettext engine. It works like this: there is a text based editable PO file that contains a number of sequences like: a line in English, a line in target language and a reference to a line in source code where original text in English comes from. The build system then creates a binary version of each PO file with MO file extension and places it it in a system directory where running instance of darktable will pick and use it.

To start working on your translation you need to:

1. Fetch source code, preferably from git. Here is the magic command:

    ```
    git clone git://github.com/darktable-org/darktable.git
    ```

    Copy the checked out source code tree somewhere to work on it or, if you know git well enough, create your own branch and work on it.

2. Build darktable and install it

3. Create a new PO file for you language. For this you will need a package called intltool and some PO editor, preferably poEdit.

    ```
    $ cd darktable/po/
    $ intltool-update -pot
    ```

    This will create a translation template with POT file extension. Now start poEdit and in menu choose "File > New catalog from POT file...". Point it to the newly created POT file.

    The next thing that will happen is that you will be asked to define various settings. All of them are pretty much self-explanatory. I only have to note here that you don't have to fill in "Team" and "Team's email address" fields. darktable doesn't currently use plural forms anywhere, but if you use Lokalize instead of poEdit, this field will be automagically filled for you.

    Then you will be asked to save the newly created PO file. Give it a name like fr.po for French translation , de.po for German translation and so on. If you are not sure about two-letter code for you language, use the one that local websites in your native language use.

4. In the PO editor you will see a) a list of all translatable messages, a filed that contains original message in English and a field to input your translation. The best practice is to copy original message into translation will and then translate all the translatable bits. That way you won't forget about some markup we use. In poEdit copying original message can be done with `Alt+C` shortcut. It's also recommended in poEdit to go to "Edit > Preferences" and in the "Editor" tab enable the "Always change focus to text input field" checkbox.

5. The best practice is to frequently test your changes. To do it first open LINGUAS file and add the two-letter code for you language there. Save, quit. Now you only have to run `make` and `sudo make install` again and restart darktable to see your changes.

6. If you don't understand some terminology, refer to existing translations from UFRaw or Rawstudio or digiKam or ask in the mailing list.

7. When you are done, `bzip2` your PO file and send it to the mailing list.

Now about updates. Every new version of darktable contains new functionality and thus there are always new messages to translate. It's best to keep track of changes, but even so we announce strings freeze a few days before releasing. It means that no changes to translatable messages will be added, so your translation created during that period will work fine.

To update your translation from working tree:

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

    Or open your PO file in poEdit and in menu choose "Catalog > Update from POT file" and point it to the newly created file.

5. Update translation, send it to the list.

    That's all.


---

# A1. some more git magic:


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
