## What this is

The C conventions every spisor and forebay library shares, as one static CMake target,
`spisor_library`. A library written in C links it and gets, without writing any of it again:

- **The `json` kind** (`spisor/json_kind.h`): parsing a call's input, including the
  `{"argument": A, "context": {...}}` envelope a function that declares context receives; writing a
  result; writing the `{"message", "fix"}` failure object with its non-zero status; and the release
  function a library's own `<library>_release` calls.
- **Paths** (`spisor/path.h`): Node's `path.join` and `path.isAbsolute` for both platform rules,
  chosen by an argument rather than by the OS the code runs on, and basekit's `expandHome`.
- **JavaScript semantics** (`spisor/javascript.h`): Node's lossy UTF-8 decoding, as
  `readFileSync(file, "utf8")` reads a file; JavaScript's whitespace and `String.prototype.trim`;
  and `String(value)` for a parsed JSON value, which is also what a value used as a property key
  becomes. A library porting JavaScript takes these rather than writing its own.
- **Files** (`spisor/file.h`): Node's `existsSync`, reading a whole file, creating a directory with
  its parents, appending, and replacing a file so a reader sees the old content or the new, never a
  mix. A replace follows a symlink and changes the target, leaving the link in place (a dotfiles
  manager's), and refuses a dangling one; `spisor_replace_private_file` refuses any symlink instead
  of following it, and keeps the temporary file and the result owner-only (0600) from creation.
  `spisor_is_link` tells a link apart. UTF-8 paths on every OS.
- **JSON out** (`spisor/json_write.h`): `spisor_json_stringify_pretty`, exactly what
  `JSON.stringify(value, null, 2)` returns, down to JavaScript's key order.
- **JSON files** (`spisor/json_file.h`): `spisor_read_json_file`, basekit's `readJson` without the
  fallback (decode like Node, strip whole-line `//` comments, then `JSON.parse`), answering absent,
  unreadable or parsed; and `spisor_strip_comment_lines` on its own.

**What it is not.** It is not a library a plugin consumes: it has no declaration and no release of
its own, and it is reached only by another library's build. It holds no product logic either; what
an app registry means lives in `forebay/library`. `runtime`, below, is built on it and is a separate
target.

## The runtime library

`runtime/` builds `runtime`, a shared library holding the behaviour behind a plugin's `ctx.paths`,
`ctx.config`, `ctx.log` and `ctx.secrets`, so every language binding answers them the same way. It is a **host-only**
library: the host opens it for its own use, and it is **never passed in a plugin's `libraries`**. It is
not a plugin service, and it holds no events and no homes. Until publishing resumes it is opened
from a local build (`cmake -S . -B build && cmake --build build --config Release` leaves it under
`build/bin`).

Every function takes the home, the places and the plugin's facts in its argument and keeps no state
between calls, so a settings file edited by hand is read afresh by the next call. The one exception is
`secrets`, which reads and writes the OS credential store (Credential Manager, Keychain, Secret
Service), and that store may prompt the user.

| function | takes | answers |
|---|---|---|
| `paths.resolve` | `home`, `names`, `places`, `plugin` | `home`, `repos`, `plugin`, `cache`, `config` directories, and with `places` the plugin's `portable` and `pluginCache` folders |
| `places.resolve` | `home`, `environment` | `portable`, `state`, `cache`, `secretsFile`, `forceFileStore` |
| `config.all` | `home`, `names`, `portable`, `plugin`, `config` | `{ values }`, the defaults, the portable file and the plugin's file deep merged in that order |
| `config.get` | `home`, `names`, `portable`, `plugin`, `config`, `key` | `{ value }` of a dot path, or `{}` when absent |
| `config.set` | `home`, `names`, `portable`, `plugin`, `config`, `key`, `value`, `layer` | `{ previous }`, after writing the plugin's file in the `application` (default) or `portable` layer |
| `log.write` | `home`, `names`, `portable`, `plugin`, `config`, `start`, `time`, `level`, `message`, `noColor` | `{ console }` when a console line is due |
| `secrets.get` | `places`, `plugin`, `name` | `{ value }` from the secrets file, else the store, or `{}`; a file copy is always the newer one |
| `secrets.set` | `places`, `plugin`, `name`, `value` | `{ place }`, `"store"` or `"file"` when the store declines or is absent |
| `secrets.delete` | `places`, `plugin`, `name` | `{}`, after removing it from both places; a store that declines keeps its copy and the file's |
| `secrets.clear` | `places`, `plugin` | `{}`, after removing every secret of the plugin from both places (host only) |
| `secrets.reconcile` | `places` | `{ moved }`, the file-held secrets moved into a store that has appeared (host only, at start) |
| `registry.register` | `places` (`state`), `registration` | `{ registration }`, after writing the host's record and keeping its stored commands and changes |
| `registry.get` | `places` (`state`), `id` | `{ registration }`, or `{}` for an unknown host |
| `registry.list` | `places` (`state`) | `{ hosts: [{ registration, orphan }], unreadable }`, sorted by id |
| `registry.forget` | `places` (`state`, `cache`), `id` | `{ forgotten }`, after deleting the record and the host's folders; refused while a change is not reversed |

`places` is what `places.resolve` answers.

### The places

`places.resolve` answers four places for the OS, from the user's home and an `environment` the host
passes (it reads no process state). An override is used only when it is a non-empty absolute path
and is ignored otherwise.

| place | Windows | macOS | Linux |
|---|---|---|---|
| portable (`SPISOR_HOME`) | `%USERPROFILE%\.spisor` | `~/.spisor` | `~/.spisor` |
| machine state (`SPISOR_STATE`) | `%LOCALAPPDATA%\spisor` | `~/Library/Application Support/spisor` | `$XDG_STATE_HOME/spisor`, else `~/.local/state/spisor` |
| cache (`SPISOR_CACHE`) | `%LOCALAPPDATA%\spisor\cache` | `~/Library/Caches/spisor` | `$XDG_CACHE_HOME/spisor`, else `~/.cache/spisor` |
| secrets file (`SPISOR_SECRETS`, a directory) | `<machine state>/secrets.json` | same | same |

Windows has no guessed default: without a usable `LOCALAPPDATA` the machine state and cache cannot
be placed, so it is refused unless overrides cover the places that need it. `SPISOR_SECRETS` also
forces the file store (`forceFileStore`).

### The host registry

`registry` keeps one record per installed host in the machine state place, `<state>/hosts/<id>.json`,
written with the atomic replacement, so two hosts registering at once never contend for one file. The
host's own files live in `<state>/hosts/<id>/` and `<cache>/hosts/<id>/`. A host id is a portable path
segment, trimmed as it is, else the call is refused.

A registration is `{ id, kind: "host", version, home?, application?, provenance, requiresCommand,
commands, changes }`. `register` replaces `version`, `home`, `application`, `provenance` and
`requiresCommand` and keeps the stored `commands` and `changes`, which the services that record them
own, so a host may call it on every start. `get` answers `{ registration }`, or `{}` for an unknown
host. `list` answers every host sorted by id with an `orphan` flag, true when `provenance.location` no
longer exists, plus the ids of registrations it could not read in `unreadable`. `forget` deletes the
registration and the host's two folders, and is refused while a change in `changes` is not marked
`"reversed": true`. A registration that exists but cannot be read is reported and never overwritten.

### The portable layer

The portable place holds `settings.json` (the ecosystem settings, which `log.write` reads) and
`plugins/<manifest id>/config.json` per plugin, named by the manifest `id`, never `config.name`;
that folder is the plugin's `portable` path from `paths.resolve`, beside its `pluginCache` folder in
the cache place. `config` reads defaults, then the portable file, then the application file, later
winning and objects merging key by key. `config.set` writes the application file unless `layer` is
`"portable"`. A host with no portable layer omits `portable` and a portable write is refused. An
unreadable file is never overwritten, and a write through a symlinked portable file changes the
target and leaves the link.

### Secrets and their stores

A secret is named `spisor/<plugin id>/<name>` in every store and in the file, and is a UTF-8 string
of at most 2,560,000 bytes on every OS (`RUNTIME_SECRET_VALUE_CAP_BYTES`); a longer one is refused
before either place is touched, and a hand-edited file entry over it is left in the file and skipped
by the move. `get` reads the file first, then the store: a file entry exists only when written
while the store refused, and every later successful store write empties the file, so a file value is
always newer, and a file-held secret causes no store access. Each time the store takes a value,
every secret in the file moves into it, written to the store before it leaves the file, so an
interruption never loses one. The file is owner-only, and a secrets file that is a symlink is never
written through: the set is refused and names `SPISOR_SECRETS`, which is how the file moves.
`forceFileStore` keeps every secret in the file and never touches the store.

- **Windows** (Credential Manager) holds about 2.5 KB per entry, so a longer value is split into
  generation-tagged chunk entries under their own root, `spisor-chunks/<plugin id>/<name>#<generation>#<n>`,
  and the base entry, written last, names the generation and count. A failed rewrite leaves the old
  value readable. 1000 chunks are the cap above.
- **macOS** keeps generic-password Keychain items under the service `spisor`.
- **Linux** loads libsecret at run time, so a machine without Secret Service falls back to the file
  instead of failing to load. Items carry the attributes `entry` and `owner` = `spisor`, and `list`
  searches by `owner`. A session bus of `autolaunch:` counts as no bus.

It is held to basekit in its own CI, through the TypeScript and the Java binding, case by case; the
places it departs from basekit are recorded one by one in `parity/divergences.json`, each with the
section of the spec that rules it.

## Using it

Reach it through CMake `FetchContent`, pinned to a full commit SHA:

```cmake
include(FetchContent)
FetchContent_Declare(spisor_library
    GIT_REPOSITORY https://github.com/spisor/library.git
    GIT_TAG <full commit sha>)
FetchContent_MakeAvailable(spisor_library)
target_link_libraries(your_library PRIVATE spisor_library)
```

Both targets are built position independent, so they link into a shared library on Linux. Tests are
built only when this repository is the top-level project.

## What a call may rely on

Every document is read through `spisor_parse_json`, which a library also uses for any JSON it
reads itself (a registry file, say) instead of calling cJSON directly. It reads a document as
`JSON.parse` does:

- **Invalid UTF-8 is refused**, because cJSON accepts it. A library reading a file the way Node
  does decodes it with `spisor_decode_utf8_lossy` first, so the replacement happens where Node's
  does and the parse never sees invalid bytes.
- **The grammar is `JSON.parse`'s, checked before cJSON reads the text**, because cJSON is laxer:
  it skips a UTF-8 BOM, takes any byte up to 0x20 as whitespace, accepts raw control characters in
  a string, and reads `01` and `1.`. So a document starting with a BOM is refused, as `JSON.parse`
  refuses it; whitespace is SP, TAB, LF and CR only; only whitespace may follow the value.
- **A string holding a NUL, key or value at any depth, is refused**, because cJSON would silently
  cut it short at the NUL.
- **An unpaired surrogate escape such as `\ud800` reads as U+FFFD**, which is what Node's UTF-8
  encoding of such a string holds; `JSON.stringify` emits one for an unpaired UTF-16 value, which a
  Windows environment variable or argument can carry. cJSON refuses it, so the escape is rewritten
  before cJSON reads the text.
- **A duplicated key keeps its first position and its last value**, so every cJSON lookup and
  iteration over the tree sees what JavaScript sees.
- **An envelope missing `argument` or `context`, or carrying a malformed or unknown fact, is a
  failure object naming what is wrong.** The facts are `environment` (an object of strings), `home`
  (a string) and `arguments` (an array of strings).
- **Environment lookups are case-sensitive, except on Windows**, where names match ignoring ASCII
  case, as the host's `process.env` matches them.

## Path vectors

`test/vectors/path-join.json` is generated by `node scripts/path-vectors.mjs`, which runs Node's own
`path.win32` and `path.posix` over the case list, and it is committed so the C tests need no Node.
The tests check every vector under BOTH platform rules on whichever OS runs them. The vectors were
generated with Node v26.3.1, whose Windows join also handles reserved device names (`CON`, `COM1`
and the rest); regenerate them and re-run the tests when moving to another Node.

## Vendored cJSON

`vendor/cjson/` holds `cJSON.c`, `cJSON.h` and `LICENSE` from cJSON **v1.7.19**, tag commit
`c859b25da02955fef659d658b8f324b5cde87be3`, unmodified. It builds as its own target without this
repository's warning flags, which apply to everything else.
