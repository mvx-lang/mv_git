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

## The cmd framework

The `GIT` verb builds its subcommands on the
[`cmd`](https://github.com/mvx-lang/mv_cmd) framework. So git runs **standalone,
without mv-package**, a minimal copy of `cmd` is bundled in `CMD.BP` under the
names `CMD.MIN.*` and compiles into `libgit`. The verb selects between them with
a single preprocessor directive — because a `.` ends an identifier, one
`$DEFINE CMD CMD.MIN` redirects every `CALL CMD.*` at once, with no change to
the call sites:

```
$IFNDEF HAVE_CMD
$DEFINE CMD CMD.MIN
$ENDIF
```

`mkpkg` defines `HAVE_CMD` automatically when the full `mv_cmd` is resolvable at
build time (a sibling package or on `$MVXPKGPATH`), so git binds to the full
`CMD.*` there and to the bundled `CMD.MIN.*` when built alone. The distinct
names mean the two never collide. (The UniData `GIT` verb has its own dispatch
and does not use `cmd`.)

## License

GPL-2.0-only. See [LICENSE](LICENSE).
