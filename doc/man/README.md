Authors
=======

To add a new man page just write it in .pod format and add it to the list at the top of `CMakeLists.txt`. Then rerun cmake. Make sure to add the `$Date$` and `$Release$` lines in the end of the file.

When editing a man page make sure to update the `$Date$` and `$Release$`.

Translators
===========

For translating man pages we use `po4a`.

To add a new translation just add your language code to `doc/man/po/LINGUAS` and re-run cmake. Then run `make update-manpage-<language code>` from the toplevel `build/` directory. The same command has to be used to update the .po file later when a .pod was changed.

To build the manpage without compiling darktable you can run `make manpages` to generate all of them or just `make manpage-<language code>` for one of the translations. The result will be in `build/doc/man/*.1` for the untranslated man pages and `build/doc/man/<language code>/*.1` for the translated ones. You can look at them by pointing `man` to the file, for example `man build/doc/man/<language code>/darktable.1`.
