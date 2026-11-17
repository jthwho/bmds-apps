# zlib 1.3.1, deflate only

From https://github.com/madler/zlib, tag `v1.3.1` (commit 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf),
unchanged: the files the VNC server's compressing needs (deflate and what it calls, and `gzguts.h`,
which `zutil.c` includes). zlib's licence is in `LICENSE` (the zlib licence: keep the notice).
Inflate isn't here: the server only sends.
