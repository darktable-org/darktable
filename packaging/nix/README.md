# Building with Nix

You can use `nix build "git+file://$PWD?dir=packaging/nix&submodules=1"` executed from the project root
to build darktable via `nix`.

This incantation is necessary because darktable uses Git submodules and those are excluded by default
in a `nix build` command.

The `submodules=1` parameter confirms that Git submodules should be initialized when evaluating.
The `dir` parameter is necessary since the `flake.nix` file is not in the project root.
