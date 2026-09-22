# NFSv3 Compliance Fix Plan

This document describes the work required to bring the codebase in `C:\Temp\WinNFSd_edited` to full RFC 1813 (NFSv3) compliance with respect to cross-platform client interop (Linux kernel NFSv3, Solaris/macOS, Windows Services-for-NFS).

**Status snapshot (2026-07-18)**:

- **Closed**: B6, B7, B8 (M1.1 — see `READDIR_fix.md`); B9 (M1.2 — LOOKUP `dir_attributes`); **B12 (READ `eof`)**; **A9 (Mount MNT v3 advertises AUTH_UNIX in `auth_flavors`)**.
- **Kodi-read critical fix (not previously ticketed)**: `ProcedureREAD` truncated 64-bit `offset3` to `(long)` via `fseek`, breaking every media file > 2 GiB. Rewritten to use `_fseeki64`/`fseeko64`; `FSINFO.maxfilesize` raised from `0x7FFFFFFF` to `0x7FFFFFFFFFFFFFFF` to match. `fopen` NULL is now checked and returns `NFS3ERR_IO`.
- **Planned but not yet committed**: B1 (ACCESS), B10 (GETATTR follow-flag). The plan for these is accepted but the source has not been edited for them.
- **Open**: B2 (SETATTR), B3 (CREATE), B4 (MKDIR), B5 (RENAME), B11 (WRITE count); Group C items; Group A6 (errno mapping); Milestone 2 (handle persistence); the multi-address banner tweak in `winnfsd.cpp` is shipped.

### Media-streaming performance pass (2026-07-18)

Optimization of the read path for a mediacenter client streaming video. Files touched: `NFS3Prog.cpp/.h`, `RPCProg.h`, `RPCServer.cpp`, `Socket.cpp`.

| Change | Rationale |
|---|---|
| Persistent open-file LRU cache (8 files) | Eliminate open/close + Windows AV on-access rescan on every ~32 KiB chunk (thousands per file). |
| `FILE_FLAG_SEQUENTIAL_SCAN` + `FILE_SHARE_DELETE` on cached handles | OS read-ahead for the sequential playback pattern; file stays deletable/renamable while playing. |
| File size cached at open | Removes the per-request seek-to-end previously used for `eof`. |
| `READ` reads directly into the wire buffer without zero-fill (`opaque::SetSize(len,false)`) | Removes the stdio intermediate copy and a wasted memset per chunk. |
| Per-transport `rtmax` (TCP 512 KiB, UDP 32 KiB), threaded via `ProcessParam.nType` | Far fewer READ round trips when streaming over TCP; RFC 1813 §3.3.19 permits per-transport values. |
| `READ` clamps `count` to `GetRTMax()` | Robustness: a misbehaving client cannot force a huge allocation or overflow the 1 MiB socket buffer. |
| `CSocket::Send` loops until the full reply is sent | Correctness prerequisite for large replies: TCP `send()` may be partial; a short send would truncate the RPC record. |
| TCP receive-side framing (`CSocketStream::HasCompleteRecord` + `CompactInput`, `CSocket::Run`) | A TCP byte stream can split one RPC record across `recv()` calls or coalesce several; accumulate until a complete record is buffered and leave any partial tail for the next read. |
| `TCP_NODELAY` on accepted TCP sockets | Avoid Nagle/delayed-ACK latency on the many small request/response exchanges of a stream. |

### Crash fix: 64-bit directory-search handle (2026-07-18)

Found by runtime testing: **READDIR/READDIRPLUS crashed the server intermittently** on the 64-bit build. `_findfirst` returns `intptr_t` (64-bit on Win64), but the result was stored in an `int`, truncating the handle; the subsequent `_findnext`/`_findclose` then operated on a corrupted pointer. The fault was memory-layout dependent (it vanished under a debugger), i.e. a latent bug exposed only by the 64-bit link. Fixed by declaring the find handle as `intptr_t` in `FileTable.cpp` (`FileExists`), `NFS2Prog.cpp`, and both `NFS3Prog.cpp` functions. This affected **directory listing** — a core Kodi operation — so it is as important as the read-path work.

Runtime verification (MinGW g++ 15.2.0, `make`, then a hand-rolled RPC client over both TCP and UDP): MOUNT returns `auth_flavors=[1]`; FSINFO reports `rtmax` 512 KiB (TCP) / 32 KiB (UDP) and `maxfilesize` 2⁶³−1; GETATTR, LOOKUP, READDIR, READDIRPLUS all return correct type/size/attributes; READ streams a 5 MiB file bit-exact (sha256 matches) with correct `eof`; and a 2.42 GiB file reads correctly at a 2.6 GB offset (the >2 GiB path). 100/100 READDIR calls stayed up after the handle fix.

### Open note: mount table never expires

`MOUNT` entries are only removed by `UMNT`. A client that disappears without unmounting leaves its slot; after `MOUNT_NUM_MAX` (100) such entries, further mounts are refused with `MNT3ERR_ACCES`. Pre-existing design limitation (documented in the README). Not on the steady-state Kodi read path, but worth an expiry/UMNTALL improvement later.

## 0. Classification key

- **Group A — Windows port simplifications (acceptable)**: deliberate choices permitted by the spec.
- **Group B — Real defects (must fix)**: violate the spec, break or risk breaking client interop.
- **Group C — Nice-to-have**: missing implementation or quality improvement, not a violation.

## 1. Compliance triage (carried over from the report)

### Group A — Windows-specific simplifications (Document, keep)
| # | Item | Reasoning |
|---|------|-----------|
| A1 | `fsid` hard-coded to 4 | Spec-defined fsid is opaque to the client. Legal. |
| A2 | `time.nseconds = 0` | With `time_delta = {1,0}` in FSINFO this is honest. |
| A3 | `verf == 0` for WRITE/COMMIT and `cookieverf` for READDIR | Server always commits (committed=FILE_SYNC), no async data; invariant holds. Improve diagnostics later. |
| A4 | `maxfilesize = 0x7FFFFFFF` | Honest "32-bit offsets" advertisement. |
| A5 | File handle layout = index into process-local `FileTable` | Legal under spec. Cross-restart stability needs Milestone 2 work. |
| A6 | errno → nfsstat mapping limited to few codes | Spec wants finer mapping. Implement in Milestone 1 alongside the spec-violation fixes that need it. |
| A7 | `FileExists` exact-case basename match | Restricts to case-sensitive semantics on case-insensitive NTFS. Track in C5. |
| A8 | Single export | Mount protocol permits. Document. |
| A9 | Mount MNT v3 advertises empty auth_flavors | Should advertise `AUTH_UNIX` (1 entry). — *planned (M1.2)* |
| A10 | `DUMP`/`EXPORT`/`UMNTALL` route through `NOIMP` slot | Legal `PROC_UNAVAIL`; better to implement in M3. |

### Group B — Real defects (must fix)
| # | Item | Spec citation |
|---|------|---------------|
| B1 | **ACCESS**: returns client bits unchanged. | §3.3.4 ACCESS — *planned (M1.3)* |
| B2 | **SETATTR**: ignores uid/gid/size/atime/mtime/sattrguard3. | §3.3.2 SETATTR |
| B3 | **CREATE**: collapses UNCHECKED/GUARDED/EXCLUSIVE. | §3.3.8 CREATE |
| B4 | **MKDIR**: drops `attributes`. | §3.3.9 MKDIR |
| B5 | **RENAME**: no XDEV, no EXIST/ISDIR/NOTEMPTY mapping. | §3.3.14 RENAME |
| **B6** | **READDIR/READDIRPLUS**: wire encoding uses interleaved `eof`/entry stream instead of per-entry `value_follows` bool. | §3.3.16 / §3.3.17 — **done (M1.1)** |
| B7 | **READDIR/READDIRPLUS**: no `cookieverf` validation. | §3.3.16 IMPLEMENTATION — **done (M1.1)** |
| B8 | **READDIR/READDIRPLUS**: bogus `eof` semantics, hard entry cap of 10. | §3.3.16 IMPLEMENTATION — **done (M1.1)** |
| B9 | **LOOKUP**: never populates `dir_attributes`. | §3.3.3 LOOKUP — **done (M1.2; build verification pending)** |
| B10 | **GETATTR**: failure-case attribute follow-flag is unreliable. | §3.3.1 GETATTR — *planned (M1.2)* |
| B11 | **WRITE**: returns requested count instead of `fwrite` return. | §3.3.7 — *open* |
| B12 | **READ**: `eof` one byte early. | §3.3.6 — *planned (M1.2)* |

### Group C — Nice-to-have
- C1 `SYMLINK`/`MKNOD`: return `NFS3ERR_NOTSUPP` with wcc (honest), not `PROC_UNAVAIL`.
- C2 `LINK`: advertise `FSF3_LINK=0` in FSINFO when not supported.
- C3 Implement `READLINK`.
- C4 Implement `FSSTAT/FSINFO` faithfully, returning actual FS stats.
- C5 Make `FSINFO` reflect real NTFS limits (linkmax, maxfilesize, etc.)
- C6 Implement `PATHCONF`.
- C7 Implement `COMMIT`.
- C8 Implement `MOUNT DUMP/EXPORT/UMNTALL` so `showmount -e` works.
- C9 Persist the `FileTable` across restarts (Milestone 2).

## 2. Three-milestone roadmap

### Milestone 1 — Cross-compat baseline
1. ~~**Fix READDIR/READDIRPLUS wire encoding** (B6, B7, B8).~~ **Done** — `READDIR_fix.md`.
2. Fix ACCESS (B1) — *planned (M1.3)*.
3. Fix CREATE (B3).
4. Fix SETATTR (B2).
5. Fix MKDIR (B4).
6. Fix RENAME (B5).
7. Fix LOOKUP dir_attributes (B9) — **done (M1.2; build verification pending)**.
8. Mount MNT v3: advertise AUTH_UNIX; real MNT3ERR_NOENT (A9) — *planned (M1.2)*.
9. errno → nfsstat3 mapping helper (A6).
10. WRITE: return actual count + boot-stable verf (B11 + A3).
11. GETATTR failure-case follow-flag (B10) — *planned (M1.2)*.
12. READ `eof` off-by-one (B12) — *planned (M1.2)*.

### Milestone 2 — Stale-handle & cross-restart
1. Persist handle → path mapping across server restart.
2. On startup, repopulate from disk; otherwise return `NFS3ERR_STALE` for unknown handles.
3. Surface fsid in handle, so handles are export-root-stable.

### Milestone 3 — Optional / cosmetic
1. Implement `PATHCONF` (C6), `COMMIT` (C7).
2. Implement `MOUNT` sub-procedures (C8).
3. Implement `FSSTAT` (C4), `READLINK` (C3).
4. Document the Group A simplifications in README.

## 3. Confirmations required from maintainer before touching anything beyond Milestone 1.1

1. **Auth model**: only read credentials for logging today; do we keep that posture or hook into the auth block for real?
2. **File-handle persistence**: write to disk on every `AddItem` (I/O cost) or accept reset-on-restart + STALE?
3. **Permissions**: enforce Windows ACLs on uid/gid/size SETATTR, or documented best-effort?
4. **Cross-compat target**: Linux kernel, *BSD, Solaris, Windows-NFS-client — set of clients dictates strictness.
5. **Auth flavor advertisement**: AUTH_UNIX only?

## 4. Files touched per milestone
- `NFS3Prog.h`/`NFS3Prog.cpp`: most fixes (READDIR/READDIRPLUS, ACCESS, SETATTR, CREATE, MKDIR, RENAME, LOOKUP, GETATTR, READ/WRITE).
- `MountProg.cpp`: auth flavor, error mapping, optional DUMP/EXPORT/UMNTALL.
- `FileTable.cpp`: handle stability for Milestone 2.
- `NFSProg.cpp`: untouched (router).
- `RPCProg.h`/`RPCServer.cpp`: untouched.
- New helper module: `NFS3Prog.cpp` private functions, no new files needed for M1.

## 5. Milestone 1.1 — READDIR/READDIRPLUS wire encoding fix

**Status**: **done** (B6, B7, B8 closed 2026-06-23; see `READDIR_fix.md` for the change report).

**Spec references**:
- §3.3.16 READDIR: `entry3 { fileid3, filename3 name, cookie3, entry3 *nextentry }` is encoded as a variable-length list where each element is prefixed by `bool value_follows`; the sentinel after the last element is `bool value_follows=FALSE`.
- §3.3.17 READDIRPLUS: same encoding for `entryplus3 { fileid3, filename3 name, cookie3, post_op_attr name_attributes, post_op_fh3 name_handle, entryplus3 *nextentry }`.
- §3.3.16/3.3.17 IMPLEMENTATION: cookieverf must be validated; server may return fewer than `count`/`maxcount` bytes; eof must be TRUE only when the previous/empty list ended the directory.

**Plan** (executed in the same change):
1. Capture pre-iteration directory attrs (mtime) into a cookieverf to advertise to clients.
2. Track `lastCookie` (server-side enumeration position) so client resume points work.
3. Encode entries as `(bool)true` → `(fileid, filename, cookie)` for each, then `(bool)false` to terminate the list.
4. Skip ".", ".." (return them last with the standard UNIX fileid encoding; spec implementation hints they shouldn't appear, but Linux clients tolerate them).
5. Output `eof` once, after the terminating `value_follows=false`.
6. Implement a real `cookieverf` validator using mtime; on mismatch: return `NFS3ERR_BAD_COOKIE` (will require reading cookieverf properly without throwing due to length mismatch).
7. Replace the hard `j = 10` cap with a soft cap respecting `count` (READDIR) / `maxcount` (READDIRPLUS) plus a per-call entry-budget returning `eof=FALSE` if we hit it.

**Files modified**: `NFS3Prog.cpp` only.

**Verification**:
- Decode with `rpc-nfs3` Wireshark dissector against a Linux kernel client doing `ls /nfs`.
- Run Linux `getfattr -m . -d /nfs` to confirm `st_size`/`ctime`/`mtime` are correct attributes.
- Cross-read: kill the server mid-listing, restart, re-issue READDIR with old cookie → `BAD_COOKIE`.
