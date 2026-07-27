# mv_git — record-git for MultiValue

`mv_git` is the **record-git engine**: git for a MultiValue account, where the
account's hash-file records are read and written directly to/from git objects in
the account's own `.git`, so the working tree *is* the live records (no export
copy). It builds a git drop-in that additionally understands MV accounts.

The same engine is compiled per platform:

- **`mvx-git`** — the build for [MVX](https://github.com/mvx-lang/mvx). It links
  the MVX runtime (`libmvxrt`) and reads records through the MVX storage
  drivers. MVX consumes this repo as a submodule at `packages/git` and builds
  `mvx-git` as part of its system build.
- **`udt-git`** — the build for Rocket **UniData** (in progress). Same commands,
  same on-disk git history, but records are read through UniData's own file I/O.

An account is exchanged between platforms in a backend-neutral **open account
format** (canonical `VOC`/`MD`, `.mvx`, and dictionaries, with `.DICT/%FILE%`
type reduced to `DIR` or `hash`), so a clone/checkout produced by one build can
be rebuilt into a live account by the other.

## Building

As an MVX submodule the build is driven by MVX's `mkpkg.sh` (which runs
`build-native.sh`). Standalone, point it at an MVX source tree that has been
built:

```sh
MVX_ROOT=/path/to/mvx-lang ./build-native.sh
```

`build-native.sh` needs the MVX runtime headers (`$MVX_ROOT/runtime/include`)
and `libmvxrt` (`$MVX_ROOT/build/lib`), plus `libgit2`.

## Layout

`src/` — the engine (`mvxgit.c`) and the CLI (`mvx-git.c`); `BP/`, `VOC/`,
`PKG` — the MVX package (the `GIT` verb and its subroutines). Records are stored
as directory files so the repo is itself a legible MVX package account.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
