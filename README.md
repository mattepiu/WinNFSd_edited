# WinNFSd_edited

A Network File System (NFS) server for Windows. You can use any NFS client to mount a directory of Windows and read or write files via NFS protocol. It is useful when you usually access files of Windows on Linux.

This is a fork of [WinNFSd](http://sourceforge.net/projects/winnfsd/) by Ming-Yang Kao, further edited in 2011 by ZeWaren. This fork focuses on NFSv3 compliance — see *Limitations* below.

- License: GPL.
- Runs on all major versions of Windows.

## Quick start

1. Download or build `winnfsd.exe`.
2. Pick a folder to share. Allow inbound connections on **TCP and UDP ports 111, 1058, 2049** through Windows Defender Firewall (you will be prompted on first run).
3. On a Linux host on the same network:
   ```bash
   # Share d:\movies as the export "d:\movies" (auto-aliased to /d/movies).
   ./winnfsd.exe d:\movies
   ...
   Local IPs = 192.168.1.42 192.168.56.1
   Type 'help' to see help

   # From the Linux box:
   mount -t nfs 192.168.1.42:/d/movies /mnt
   ```
4. Or expose a folder under a clean alias:
   ```bash
   ./winnfsd.exe d:\movies /movies
   ...
   # From the Linux box:
   mount -t nfs 192.168.1.42:/movies /mnt
   ```

`.` expands to the current working directory:
```bash
$ cd D:\Media\Movies
$ winnfsd.exe . /movies
```

## Command-line interface

```
winnfsd.exe [ -id <uid> <gid> ] [ -log on | off ] <export path> [ alias path ]
```

| Argument | Required | Description |
|---|---|---|
| `<export path>` | yes | Absolute Windows path, e.g. `d:\work`, or `.` for the current directory. Surrounding double quotes are stripped. |
| `[alias path]` | no | NFS alias clients see on the wire. Must start with `/`. If omitted, an alias is auto-derived by replacing `:` with `/` and `\` with `/`. |
| `-id <uid> <gid>` | no | Two integers embedded in file attributes returned to clients. **Advisory only — not enforced as access control.** |
| `-log on \| off` | no | Toggles per-request log lines on stdout. Default: `on`. Can be flipped at runtime with the `log` command (see below). |

Examples:

```text
winnfsd.exe d:\work
winnfsd.exe d:\work /exports
winnfsd.exe . /exports
winnfsd.exe -id 1001 1001 d:\work /exports
winnfsd.exe -log off d:\work
```

## Network ports

| Service | Port | Protocol |
|---|---|---|
| Portmap (RPC program 100000) | 111  | TCP + UDP |
| NFS    (RPC program 100003) | 2049 | TCP + UDP |
| Mount   (RPC program 100005) | 1058 | TCP + UDP |

The ports are hard-coded; they cannot be changed on the command line. The server binds to all local interfaces (`INADDR_ANY`); the printed *Local IPs* line lists every IPv4 address the host responds to. Run `mount` from a client on any reachable interface.

> Windows Firewall will prompt for permission the first time `winnfsd.exe` listens on a public or private network. Click *Allow*.

## Mounting from clients

### Linux (kernel NFS client)

```bash
# NFSv3 (recommended for full features: READDIRPLUS, FSINFO, large reads)
mount -t nfs -o nfsvers=3 <server>:<alias> /mnt

# NFSv2 (fallback — fewer features, smaller reads)
mount -t nfs -o nfsvers=2 <server>:<alias> /mnt
```

The mount daemon here advertises **AUTH_UNIX** (flavour 1) only on v3. Clients that require a different auth flavour are rejected. Kernel NFS uses `AUTH_UNIX` by default, so this works out of the box on Debian/Ubuntu/Fedora/Arch/etc.

### macOS (BSD NFS client)

```bash
mount -t nfs -o nfsvers=3,resvport <server>:<alias> /mnt
```

`resvport` is needed if the server enforces privileged source ports; this server does not, so you may omit it on a trusted LAN.

### Kodi / LibreELEC (mediacenter)

In the NFS source: `nfs://<server>/<alias>` or use the built-in LibreELEC mount. Kodi negotiates NFSv3 by default.

### Solaris / illumos

```bash
mount -F nfs -o nfsvers=3 <server>:<alias> /mnt
```

## Interactive commands

After the server starts, a console prompt accepts commands. Commands are case-insensitive.

| Command | Effect |
|---|---|
| `about`  | Reprints the program banner. |
| `help`   | Lists the commands above. |
| `log on` / `log off` | Runtime toggle of per-request logging. |
| `list`   | Lists the host strings of clients currently mounted via MOUNT. |
| `reset`  | Unbinds the NFS program from the RPC dispatcher; subsequent NFS requests get PROC_UNAVAIL until the next `start`/restart. Useful for forcing clients to re-mount. Not shown by `help`. |
| `quit`   | Stops the server. If any client is mounted, you will be asked to confirm. |

Logging lines look like:
```text
NFS GETATTR  /d/movies OK
MOUNT MNT  from 192.168.1.10  Final local requested path: d:\movies
```

## Behaviour notes

- **Authentication.** The server reads RPC credentials only for logging and for the `-id`-derived uid/gid in returned attributes. It does **not** consult NTFS ACLs. Treating a mount as read-only is the host operator's responsibility.
- **Single export per process.** Each instance exports exactly one directory. Two exports require two copies of `winnfsd.exe`, but the ports are fixed — only one instance can run per host unless you wrap it in distinct network namespaces. Multiple aliases on a single path are not supported.
- **File-handle stability.** File handles are indexes into a process-local `FileTable`. **They are not persisted.** Restarting the server invalidates every handle the client has cached. Clients will see `NFS3ERR_STALE`; a remount is required. Persistent handles are planned but not yet implemented (see `FixPlan.md`).
- **Cookie verifier (READDIR / READDIRPLUS).** The verifier is derived from the directory's `mtime`. Modifying a directory invalidates outstanding cookies. Killing the server mid-listing and restarting also invalidates cookies.
- **Async write.** Every WRITE is committed synchronously (`committed=FILE_SYNC`); the verifier is constant `0`. Clients receive truthful completion data.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `mount: Protocol not supported` or `RPC: Authentication error` | MOUNT v3 is rejecting the client's auth negotiation — usually means a non-`AUTH_UNIX` flavour was advertised. This server advertises `AUTH_UNIX` only. | Force NFSv3 with `nfsvers=3` on the client. If still failing, check `rpcinfo -p <server>` lists port 2049. |
| Mount succeeds, `ls` returns empty or hangs | Wrong alias path. | Check the alias printed at startup: `Starting, path is: d:\work, path alias is: /exports`. |
| `permission denied` on mount | Client's source port is privileged but not on the kernel-reserved range (BSD NFS only); on Linux kernel NFS this is unusual. | On Solaris/macOS try `-o resvport` or remove it on Linux. |
| `nfs_readlink`/`nfs_statfs` errors | Some procedures return `PROC_UNAVAIL`. | This fork implements NFSv3 strictly where the spec is required for interop; less-common procedures (READLINK, COMMIT, PATHCONF) return `PROC_UNAVAIL`. |
| Stale handle errors after server restart | Handles not persisted across restarts. | Run `umount -f /mnt` on the client and remount. |
| File shows the wrong size after a write | Write-path returns the `fwrite` byte-count and synchronously commits; this should not happen. | Capture a log with `log on`, file an issue. |

## Limitations

- The fork focuses on NFSv3 *interoperability* and the read path. The full spec is implemented for READDIR, READDIRPLUS, GETATTR, SETATTR (partial), LOOKUP, CREATE/MKDIR/RENAME/REMOVE/RMDIR (partial), READ, WRITE.
- NTFS ACLs are not enforced. Set the export folder's NTFS permissions to grant the *networked* users the access you intend to allow.
- No persistent file handles. Server restart invalidates all client mounts.
- No multi-export. One export per process; effectively one export per host.
- No `showmount -e` support (the MOUNT sub-procedures DUMP / EXPORT / UMNTALL return `PROC_UNAVAIL`).
- Large-file streaming is verified for several-GB reads but bounded internally by `rtmax=32K` per FSINFO; clients respect this.

## Building from source

Required: `g++` (MinGW), `make`, Windows.

```
make
```

Output: `winnfsd.exe` in the repo root.

For a debug build uncomment `-g -D_DEBUG` in `Makefile`.

## License

GPL — see `LICENSE` if present, or the upstream [WinNFSd](http://sourceforge.net/projects/winnfsd/) project page.

Copyright (C) 2005 Ming-Yang Kao.
Edited in 2011 by ZeWaren.
Edited in 2026 by Matteo Azzali to expand compliance for read clients (i.e.: Kodi).
NFSv3 compliance fixes in this fork.
