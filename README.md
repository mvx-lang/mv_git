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
names `CMD.MIN.*` and compiles into `libgit`. The verb chooses between them at
**run time**: it probes `CATALOGED("CMD.INIT")` — the MV catalog-lookup idiom,
resolving exactly the way `CALL` does — and dispatches by name through
`CALL @var`:

```
CI = "CMD.MIN.INIT" ; CA = "CMD.MIN.ADD" ; CR = "CMD.MIN.RUN"
IF CATALOGED("CMD.INIT") THEN
   CI = "CMD.INIT" ; CA = "CMD.ADD" ; CR = "CMD.RUN"
END
CALL @CI("GIT", "…") ; CALL @CA("INIT", "…", "GIT.INIT") ; … ; CALL @CR
```

Because the choice is at run time, **installing `mv_cmd` later is picked up with
no rebuild** — the next `git` run uses the full `CMD.*`; with nothing installed
it uses the bundled `CMD.MIN.*`. The distinct names mean the two never collide.
(The UniData `GIT` verb has its own dispatch and does not use `cmd`.)

## License

GPL-2.0-only. See [LICENSE](LICENSE).
