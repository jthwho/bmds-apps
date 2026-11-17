# zlib 1.3.1, deflate and inflate

From https://github.com/madler/zlib, tag `v1.3.1` (commit 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf),
unchanged: the files the VNC server's compressing needs (deflate and what it calls, and `gzguts.h`,
which `zutil.c` includes), and inflate's (`inflate.c`, `inffast.c`, `inftrees.c` and their headers,
`inffixed.h`) for the Extended Clipboard's copies from viewers (devplan/vnc.md §23). zlib's licence
is in `LICENSE` (the zlib licence: keep the notice). Not here: gzip files, `infback.c`, `compress.c`
and `uncompr.c`.
