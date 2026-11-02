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
`ctx.config`, `ctx.log` and `ctx.secrets`, so every language binding answers them the same way, and the registry of installed hosts with its change record, commands and install planning, which every host shares. It is a **host-only**
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
| `changes.apply` | `places` (`state`), `host`, `change` | `{ recorded, sequence? }`, after making the change and appending it to the host's `changes` |
| `changes.reverse` | `places` (`state`), `host` | `{ reversed, drifted }`, after undoing every unreversed change, newest first |
| `commands.request` | `places` (`state`), `environment`, `host`, `home`, `name`, `target` | `{ name }`, the name given (a fallback when the asked one is held), or `{}` for a command the user deleted |
| `commands.rename` | `places` (`state`), `environment`, `host`, `from`, `to` | `{ name }`, after moving the shim; a held `to` is refused |
| `commands.delete` | `places` (`state`), `host`, `name` | `{ deleted: true }`, after removing the shim; refused for a required host's last command |
| `commands.add` | `places` (`state`), `environment`, `host`, `home`, `name`, `target` | `{ name }`, after making the shim; a held name is refused |
| `install.packages` | `places` (`portable`), `item`, `manifestPackages` | `{ packages }`, the item's package under each method, as `{ name, options? }`; pure |
| `install.plan` | `places` (`portable`), `item`, `manifestPackages`, `methods`, `chosen`, `installedProviders`, `catalog` | `{ steps }`, each `{ "install-provider": <catalog entry> }` or `{ "install": { method, package? } }`; pure |

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

### The change record

Everything a host changes outside its own folders goes through `changes.apply`, so removing it can
replay the record and leave the machine as it was, even once the host's binary is gone. Each entry
in `changes` is `{ sequence, kind, state, reversed, ... }`. Every path in it is absolute and stored
normalized (one separator form, no trailing separator), and two paths name one target when they are
equal so, ignoring ASCII case on Windows:

| kind | the change carries | the record keeps | reversal |
|---|---|---|---|
| `file` | `path`, `content` or `source` (an absolute file whose bytes are copied), `executable`? | `path`, `digest` of the bytes | deletes what stands at `path`, a link itself and never its target; a directory there is left and reported |
| `directory` | `path` | `path` | removes it only once empty; one still holding something is left and reported |
| `json-key` | `file`, `key` (array of strings), `value` | `file`, `key`, `value`, `previous` or `createdFrom` | restores `previous` or deletes the key and the objects it created, only while the key still holds `value`; otherwise it is left and reported in `drifted` |
| `profile-line` | `file`, `line`, `create`? | `file`, `line`, `separated`, `created`? | removes the exact line, and the newline added before it; a file it created is deleted once nothing else is left in it |
| `path-entry` | `directory`, `home`, `shell`? | Windows: `directory`, `home`, `entry`, `separated`, `created`; POSIX: `directory`, `home`, and a `profile-line` child per profile | Windows: removes the exact entry it added to the user's `Path`, and the value when it created it; POSIX: its children do |

A `file` is refused over anything that already exists, a `json-key` over a file that is missing or
not a readable JSON object, a `profile-line` over a missing file unless it asks to `create` it, and a kind runtime does not know
is refused before anything is written. A `directory` or `profile-line` the user already has, or a
`json-key` already holding its value, is not recorded (`recorded: false`). Both are reference-counted across every registration: one held by
another unreversed record, of this host or another, is recorded again and stays until the last
holder is reversed; a registration that cannot be read counts as a holder. A change this host
already holds unreversed (a `file` at the same path, a `json-key` setting the same value at the same
key, or the same target of a shared kind) is answered with that record's sequence and `recorded:
false`, and nothing is recorded, so a host may apply its changes on every start. A `file` asked
for again with different content is rewritten in place under the same record (created again if it
is gone, refused if a directory or link stands there) and answered the same way.

A `path-entry` puts `directory` on the user's PATH. On Windows it is one entry of the `Path` value
under `HKCU\Environment`, read unexpanded and written back with its type (`REG_EXPAND_SZ` when the
value is created), followed by a `WM_SETTINGCHANGE` broadcast; a directory already on `Path` and held
by no host is the user's, is not recorded and is never removed. On POSIX it is recorded with one
`profile-line` child, `export PATH="<directory>:$PATH"`, for each of `<home>/.profile`, `.bashrc`,
`.zshrc` and `.zprofile` that exists, and for the login shell's profile, which is created when
missing: `.zprofile` when `shell` (the caller's `SHELL`) ends in `zsh` or the OS is macOS, otherwise
`.profile`. A created profile is recorded `created` and deleted on reversal only if nothing but the
line was in it; a record of a line another host holds inherits that host's `created`. Each child is
an entry of its own with `parent` set to the `path-entry`'s sequence, so it is reference-counted and
reversed like any other line. Applying it again adds children only for profiles without this host's
line, under the first apply's `path-entry`. A `path-entry` is reference-counted by its directory, and on POSIX its
home too. A directory holding `;`, `%`, `"` (Windows) or `"`, `\`, `$`, `` ` ``, `:` (POSIX) is refused.

The entry is stored with `state: "pending"` before the disk is touched and marked `"applied"`
after; a pending entry found by the next call is completed when its effect is on the disk and
dropped otherwise. A sequence is never handed out twice: the registration keeps a `nextSequence`
counter, which `registry.register` carries over like `changes`. `reverse` stores `"reversed": true` (and `"drifted": true` for a drift) after
each change, so an interrupted removal resumes where it stopped and reverses nothing twice.
`reversed` counts the drifted entries too. A JSON file is rewritten as `JSON.stringify(value, null,
2)` with its original final newline. `runtime_changes_apply` is the same operation for C callers.

Every read-modify-write of a registration (`registry.register`, `registry.forget`, `changes.apply`,
and each step of `changes.reverse`, from its holder check to its store) holds one OS file lock on
`<state>/hosts/.lock`, so two hosts racing on a shared line or directory, or a host registering while
another call records its change, never lose or orphan anything. The OS releases the lock when a
process exits, so a crash never leaves it held. A `commands` call holds it once for the whole call
and applies its changes under it, so it never takes it twice.

### Commands

A host's commands are shims in `<state>/bin`, recorded as `file` changes of that host. On POSIX
the shim is `<name>`, an `sh` script running `exec '<target>' "$@"`. On Windows it is `<name>.exe`, a
copy of `runtime-launcher.exe`, beside `<name>.shim`, which holds the target's UTF-8 path: the
launcher reads the `.shim` next to its own path, runs the target with its own arguments exactly as
given, and exits with the target's exit code. It replaces a `.cmd` shim because `cmd.exe` reads a
batch file in the console's code page, which garbles a non-ASCII target. The launcher is built with
the library and found beside the module that holds it, so it ships beside `runtime.dll`. The first
command also records `<state>/bin` as a `directory` and a `path-entry` in the user's home, with the
call's `SHELL`, so removing the last host that has one takes the directory off PATH. Each command
is an entry `{ name, requested, owner: "host" | "user", state: "active" | "deleted" }` in the
registration's `commands`; `requested` is `null` for a command the user added, which no host asked
for.

A name is held by an active command of any host, by anything already in `<state>/bin`, and by an
executable on the `PATH` the call carries, `<state>/bin` left out: on Windows `<directory>/<name>`
with an extension of `PATHEXT` (`.COM;.EXE;.BAT;.CMD` when it is absent), on POSIX an executable
`<directory>/<name>`. `request` gives a held name's first free fallback, `<name>2`, `<name>3` and
so on, so a host asking for `cc` where the C compiler is installed gets `cc2` and never shadows it.
`request`, `rename` and `add` refuse a call whose `environment` carries no `PATH` string (`Path` on
Windows matches too), since without one every name would look free.
A host asking again for a name it requested is answered with that entry as it stands, and its
shim is rewritten when the target moved, but the entry itself is never changed by a host: a name
the user renamed stays renamed, a command the user deleted stays deleted, and a command the user
added under the same name is never matched by a host's request. `rename`, `delete` and
`add` are the user's, mark the entry `owner: "user"`, and refuse a held name rather than fall back.
`delete` refuses the last active command of a host with `requiresCommand`; renaming it is allowed.
A name is a letter or digit followed by letters, digits, `-`, `_` or `.`, at most 64; a target is
an absolute path with no line break.

### Install

`install` decides; it runs nothing, and a host executes the steps it answers. `install.packages`
merges an item's package for each install method: the manifest's mappings first, then the item's
entry in `<portable>/packages.json`, whose mapping for a method replaces the manifest's for that
method only. A mapping is a package name or `{ name, options? }` and is answered in the object form;
an absent file adds nothing, and a file that is unreadable, not an object, or holding a mapping of
another shape is refused with a fix naming it.

`install.plan` turns the method the user chose into steps. `chosen` must be one of `methods`.
`marketplace`, fetching straight from a marketplace source, is built into every host: it is always
plannable and needs no package or provider. Any other method needs a package mapping and a provider:
when `chosen` is not in `installedProviders`, the first `catalog` entry whose `installMethod` is
`chosen` is installed first as an `install-provider` step, and none is a refusal naming the method.
The caller orders `catalog`, so earlier marketplaces take precedence.

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
