# udt-callc — git's CallC contribution to UniData's libu2callc.so

The in-session `GIT` verb (`GIT.udt.b`, Model B) drives the record loop on the
current UniData session and calls the libgit2 work through CallC — the eight
`GIT*` functions declared in `funcs`.  The package manager aggregates this
fragment into UniData's single `libu2callc.so` via `udt-callc-build` when git is
installed (MVPKGOS "CALLC").

A binary release ships pre-built objects here (`gitcallcb.o`, `mvxgit.o`,
`udtgit_rt.o`, built against libgit2 + the UniData InterCall headers); `funcs`
declares the cfuncdef signatures and `libs` adds the libgit2 link flags.  The
git-object functions never touch records, so the InterCall session (opened
lazily by the record backend) never starts — no second session, no extra
licence.
