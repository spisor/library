## What this is

The C conventions every spisor and forebay library shares, as one static CMake target,
`spisor_library`. A library written in C links it and gets, without writing any of it again:

- **Status and refusal** (`spisor/status.h`): `spisor_status` (OK, NONE, MALFORMED, REFUSED, OUT_OF_MEMORY)
  and the size-prefixed `spisor_refusal`, which every definition of a library built on this one answers, so no
  public header of a library includes a weivel header. They restate weivel's `weivel_status` and
  `weivel_standard_service_refusal`, and `restatement_test` holds every size, offset and number to weivel's.
- **The value tree** (`spisor/value.h`): `spisor_value`, weivel's `weivel_standard_value` restated and held to it by
  the same test, one typed tree (null, boolean, finite number, text,
  list, record) for data a library only stores or hands back and whose shape belongs to someone else, such as a
  setting's value; `spisor_value_check`, `spisor_value_build` and `spisor_value_release`. `spisor/json_value.h`
  converts between a tree and the JSON a file on disk holds.
- **Definition helpers** (`spisor/definition.h`): what every library's definitions share, so none keeps its own
  copy: `SPISOR_SHAPE_THROUGH` and `spisor_has_shape` for reading a size-prefixed struct only through its first
  shape, `SPISOR_IS_EMPTY_ANSWER`, `SPISOR_IS_EMPTY_COUNTED_ANSWER` and `SPISOR_CLEAR_ANSWER` for the empty answer
  an operation fills, `spisor_refuse_as`, `spisor_refuse_shape` and `spisor_release_refusal` for refusing, and
  `spisor_overwrite` for a secret's bytes before they are freed.
- **One allocator** (`spisor/allocation.h`, `spisor/json_allocation.h`): every allocation in shipped code goes
  through `spisor_memory_allocate` and its family, cJSON's included, so an out-of-memory test sees all of them.
  `spisor_library` carries no allocator itself: each final binary links exactly one of `spisor_allocator` and the
  test-only `spisor_allocator_counting`, and the configure refuses anything else.
- **JSON reading** (`spisor/json_kind.h`): reading one whole JSON document exactly as `JSON.parse`
  reads it, for the files a library keeps on disk. Nothing crosses a library's boundary as JSON.
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
- **JSON out** (`spisor/json_write.h`): `spisor_json_stringify_pretty` and `spisor_json_stringify`, exactly what
  `JSON.stringify(value, null, 2)` and `JSON.stringify(value)` return, down to JavaScript's key order.
- **JSON files** (`spisor/json_file.h`): `spisor_read_json_file`, basekit's `readJson` without the
  fallback (decode like Node, strip whole-line `//` comments, then `JSON.parse`), answering absent,
  unreadable or parsed; and `spisor_strip_comment_lines` on its own.

**What it is not.** It is not a library a plugin consumes: it has no declaration and no release of
its own, and it is reached only by another library's build. It holds no product logic either; what
an app registry means lives in `forebay/forebay-library`. `runtime`, below, is built on it and is a separate
target.

## The runtime library

`runtime/` builds `runtime`, a shared library holding the behaviour behind a plugin's `ctx.paths`,
`ctx.config`, `ctx.log` and `ctx.secrets`, so every language binding answers them the same way, and the registry of installed hosts with its change record, commands and install planning, which every host shares. It is a **host-only**
library: the host opens it for its own use, and it is **never passed in a plugin's `libraries`**. It is
not a plugin service, and it holds no events and no homes. Until publishing resumes it is opened
from a local build (`cmake -S . -B build && cmake --build build --config Release` leaves it under
`build/bin`).

Every operation takes the home, the places and the plugin's facts in its argument and keeps no state
between calls, so a settings file edited by hand is read afresh by the next call. The exceptions are
`secrets`, which reads and writes the OS credential store (Credential Manager, Keychain, Secret
Service), and that store may prompt the user, and `weivel.scope`, which opens scopes in the host its start began
and keeps that host's log writer. Every operation is safe from two threads at once: `secrets` serialises its rewrites
of the secrets file within the process, and a replaced file's temporary name is unique to its process and call.

### The definitions

Each service is a **definition**: a static C struct that begins with its `size`, then the provider's `context`,
then one function pointer per operation and one release per kind of answer. A consumer gets it from the
service's exported symbol, `runtime_<service>_definition_of`, a `const void *f(void)` whose struct lives until
the library is unloaded, and calls its operations directly. An operation takes the `context` first and a
size-prefixed argument struct, fills the caller's empty answer and, when it refuses, the caller's
`spisor_refusal`, and answers a `spisor_status`; the caller hands each back to the release for its kind. A
size-prefixed struct is read only up to its `size` and only ever grows at its end, so every struct's header
comment says where its first shape ends. Text handed in is NUL-terminated UTF-8 borrowed for the call; what an
answer points at is the provider's until its release. An empty answer has NULL storage and every count zero,
an operation refuses an answer whose array count arrives without storage, and an out-of-memory answer is left
empty.

`runtime_short_shape_test` calls every operation of every definition `runtime/library.json` declares, through the
built `runtime`, with each argument and answer one byte short of its first shape, and `runtime_empty_answer_test`
calls every operation whose answer counts an array with that count nonzero beside NULL storage; each requires
MALFORMED with every answer untouched, a text or unsized struct input passed NULL, so every operation checks its
argument and answer shapes before a required input. Both walk the description each definition answers, its
operations' parameters, its types' first shapes and the members its arrays name as their counts, so an operation
added to a header is checked without a test changing. The walk is `testing/described_checks.c`, test-only, and
`spisor_add_described_checks(LIBRARY <target> DECLARATION <library.json>)` adds the same two tests, named after the
library, for another one. `declaration_test` holds every header to one getter per definition struct it defines.

`runtime/library.json` declares the library in the weivel standard's format, `runtime` 0.2.0, each service one
function of kind `definition` and its `description`, `runtime_<service>_definition_description`, or for a standard
service `weivel.<name>` the getter `runtime_standard_<name>_definition_of` and its `_description`, and a test keeps
it complete against the headers; another loads the built `runtime` through `weivel/c`'s host, which checks every
description with the standard's check, and calls `paths` and `weivel.scope` through the registered definitions. Each
description is generated at build time by the weivel standard's `weivel_describe` from `spisor/status.h` and every
`include/runtime` header, so a language that cannot read a C header binds the definition from it, and
`description_test` loads each one from the built `runtime` and runs the standard's own check on it. The symbol is
the definition's name with `_description`, which is the name another library nesting a runtime definition looks
for. `runtime` links `weivel_standard` statically, and no other weivel library. It declares no `release`, since
every definition releases its own answers, and the binary exports its definitions and their descriptions and
nothing else, which `runtime_exports_only_definitions_test` holds: what it links statically is hidden by
`--exclude-libs` on Linux and by an exported symbols list on macOS, generated from the same headers the test reads.
Each header documents its operations, what they take and answer, and every refusal; this file says what they
do together. A translation of a header in another language is checked against the `layouts` test program,
which prints every public struct's fields with offset, size and `sizeof`, and which the configure holds to the
headers (`cmake/layouts_check.cmake`). Operation names are the service's own in snake case; dotted names below,
such as `places.resolve`, mean that service's operation. No header a consumer reads names cJSON, though
`spisor_library` links it publicly: every build compiles `public_headers_without_cjson`, which includes each
`include/runtime` header, `spisor/status.h`, `spisor/value.h` and `spisor/definition.h` with cJSON poisoned and
off the include path.
`spisor_refuse_name_in_headers` (`cmake/header_isolation.cmake`) builds the same check for another library.

| service | header | operations |
|---|---|---|
| `paths` | `include/runtime/paths.h` | `resolve` |
| `identity` | `include/runtime/identity.h` | `parse`, `folder`, `resolve`, `agree`, `also_at`, `accepts_version` |
| `marketplace` | `include/runtime/marketplace.h` | `build`, `sources`, `read`, `lookup`, `remember` |
| `places` | `include/runtime/places.h` | `resolve` |
| `config` | `include/runtime/config.h` | `all`, `get`, `set` |
| `settings` | `include/runtime/settings.h` | `all`, `set` |
| `log` | `include/runtime/log.h` | `write` |
| `secrets` | `include/runtime/secrets.h` | `get`, `set`, `remove`, `clear`, `reconcile` |
| `registry` | `include/runtime/registry.h` | `register_host`, `get`, `list`, `forget` |
| `changes` | `include/runtime/changes.h` | `apply`, `reverse` |
| `commands` | `include/runtime/commands.h` | `request`, `rename`, `remove`, `add` |
| `install` | `include/runtime/install.h` | `warnings`, `packages`, `plan`, `detect`, `entry`, `runtime`, `application`, `register_loader`, `by_id`, `ecosystem` |
| `fetch` | `include/runtime/fetch.h` | `get` |
| `catalog` | `include/runtime/catalog.h` | `read`, `sources`, `merge`, `check`, `current`, `resolve` |
| `cache` | `include/runtime/cache.h` | `install`, `installed`, `check`, `damaged`, `locate`, `platform`, `sweep`, `deploy` |
| `updates` | `include/runtime/updates.h` | `apply`, `check`, `rollback`, `recorded`, `run` |
| `boot` | `include/runtime/boot.h` | `read`, `write`, `resolve`, `place` |
| `processes` | `include/runtime/processes.h` | `hold`, `release`, `prune`, `ready`, `await_ready`, `record_run`, `live_runs`, `remove_runs` |
| `hosts` | `include/runtime/hosts.h` | `register_host`, `list`, `remove`, `provenance`, `start` |
| `loader` | `include/runtime/loader.h` | `check`, `home`, `installed` |
| `detect` | `include/runtime/detect.h` | `on_path` |
| `lists` | `include/runtime/lists.h` | `plugins`, `applications` |
| `ui` | `include/runtime/ui.h` | `navigation`, `settings_view`, `plan`, `render`, `plugin_list`, `action` |
| `weivel.scope` | `include/runtime/standard.h` | `open`, and the scope's `config`: `all`, `get`, `set`; `secrets`: `remove`, `get`, `set`; `paths`: `cache`, `config`, `plugin`; `log`: `write` |

24 services, 104 operations.

A `places` argument is what `places.resolve` answers.

### The standard services

`include/runtime/standard.h` serves the weivel standard's own definitions under the standard's ids, with the
standard's structs (`weivel/scope.h`) and its descriptions, each exported through a one-line forwarder such as
`runtime_standard_scope_definition_description`, so runtime describes none of them again. runtime serves
`weivel.scope` and declares none of `weivel.config`, `weivel.secrets`, `weivel.paths` and `weivel.log`, which a
plugin reaches only through its own scope. A scope opens only once the host is started, and its `open` reads the
plugin's manifest: the `id`, the `config` record and the `settings`, each declaration a record whose `layer` is
`application` or `portable` when it has one, and refuses at open a manifest one typo breaks, an unknown layer, a
declaration or `config` that is no record, no `id`, or a config name or id no settings file can be named by, so one
typo refuses that plugin rather than every later read. The scope's `config` answers `config.all`, `get` and `set`
for that plugin alone, flattened: a record's members become dotted keys, a text, number or boolean is offered as
the standard's kind, and a null, a list, a member whose name is empty or holds a dot, and a declared secret are not
offered, so a get answers NONE for them; a set of a declared secret is refused, since only `weivel.secrets` reaches
one. Its `secrets` are runtime's `secrets` under the plugin's id alone, in the host's secret places; a secret is
UTF-8 text without a NUL byte, as every place runtime keeps one holds, and other bytes are refused. Its `paths`
answer folders that are the plugin's alone, named by its id: `cache` its folder in the cache place, and `config` and
`plugin` its own folder, `<portable>/plugins/<id>`, which holds its portable `config.json`, or its folder in the
application's plugin folder for a host with no portable layer; each is resolved once at open. Its `log` hands each entry, its fields appended as compact JSON, to one
writer thread the started host owns, which appends it through runtime's `log`, so a write never waits on a file;
the writer holds at most 1024 entries, a write beyond them is refused, and the host's end writes every entry handed
over before it returns, after which a write is refused until the next begin. A TRACE entry is dropped, since
runtime's log keeps no such level. A scope keeps its own copy of the host and the manifest, so it outlives the
host's end, and releasing it frees everything it holds once the writer has written every entry it handed over.
`scope_test`, `scope_services_test` and `out_of_memory_scope_test` hold it, the last failing every allocation of
the open, the release and each operation; `scope_threads_test` calls a scope from two threads at once.

A host opens a screen by calling `ui.plan` in rounds, answering the calls each names through its own engine, and
`ui.render` once a plan names none; `ui.plan` refuses a seventeenth round, so a screen whose bindings chain deeper
than 16 is never rendered. `ui.action` answers the call a declared action makes, its arguments the declared
argument, then the input, then the row's `item` and `items`, and refuses an action the manifest does not declare.

### The places

`places.resolve` answers four places for the OS, from the user's home and an `environment` the host
passes (it reads no process state). Without a `home` argument the user's home is derived from that
environment (`USERPROFILE` on Windows, else `HOME`) and answered as `home`. An override is used only when it is a non-empty absolute path
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

A registration is `{ id, kind: "host", version, home, application?, provenance, requiresCommand,
start?, commands, changes }`, `home` being an absolute path, without which `register_host` and
`hosts.register_host` refuse it, and `start` being `{ runtime, entry }`: the language runtime the host needs
(`javascript`, `jvm` or `native`) and its entry, a relative path inside its installed bundle, which is
what a launcher starts a background host through. `register_host` replaces `version`, `home`,
`application`, `provenance`, `requiresCommand` and `start` and keeps the stored `commands` and
`changes`, which the services that record them own, so a host may call it on every start.
`hosts.register_host` is what a host calls on every start: it keeps the provenance its first start
recorded, decided under the registry lock, then requests its commands. `get` answers the registration, or NONE for an unknown
host. `list` answers every host sorted by id with an `orphan` flag, set when `provenance.location` no
longer exists, plus the ids of registrations it could not read in `unreadable`. `forget` deletes the
registration and the host's two folders, and is refused while a change in `changes` is not marked
`"reversed": true`. A registration that exists but cannot be read is reported and never overwritten.

A provenance is `{ method, version, location, os, architecture, installedAt, ... }`, every named
member a non-empty string and kept whole with any further members the host adds: `method` is how
the host was installed (`npm`, `winget`, `forebay-installer`), `version` the version that method
installed, `location` the absolute path it lives at, `os` and `architecture` the platform it was
installed for, and `installedAt` when, as an ISO 8601 UTC timestamp, `YYYY-MM-DDTHH:MM:SS` with an
optional fraction and a closing `Z`, which is what JavaScript's `toISOString` writes. Only that
shape is checked, not the calendar.

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
| `json-key` | `file`, `key` (array of strings), `value`, `create`? | `file`, `key`, `value`, `previous` or `createdFrom`, `before` (the file's bytes), `after` (digest of the bytes written), `created`? | a file still exactly as written gets its `before` bytes back, or is deleted when it was created; otherwise, while the key still holds `value`, only its own bytes are undone in place: `previous` is put back as the file held it, or the key goes with each object it created that is left empty; a key no longer holding `value` is left and reported in `drifted` |
| `profile-line` | `file`, `line`, `create`? | `file`, `line`, `separated`, `created`? | removes the exact line, and the newline added before it; a file it created is deleted once nothing else is left in it |
| `path-entry` | `directory`, `home`, `shell`? | Windows: `directory`, `home`, `entry`, `separated`, `created`; POSIX: `directory`, `home`, and a `profile-line` child per profile | Windows: removes the exact entry it added to the user's `Path`, and the value when it created it; POSIX: its children do |

A `json-key` edits its file in place: every byte it does not own is kept, a present key has only
its value's bytes replaced, and a missing one is added as the last member of its object (inside any
objects it has to create) in that object's own indentation, line ending and spacing, or compact on
one line when the object is on one line or nothing in the file shows a style. A `file` is refused
over anything that already exists, a `json-key` over a file that is not a readable JSON object or
is missing unless it asks to `create` it (made as `JSON.stringify(document, null, 2)` and a newline), a `profile-line` over a missing file unless it asks to `create` it, and a kind runtime does not know
is refused before anything is written. A `directory` or `profile-line` the user already has, or a
`json-key` already holding its value, is not recorded (`recorded: false`). Exactly three kinds are
reference-counted across every registration: `profile-line`, `directory` and `path-entry`. One held
by another unreversed record, of this host or another, is recorded again and stays until the last
holder is reversed; a registration that cannot be read counts as a holder. A `json-key` and a `file`
are not: each record is undone on its own. A change this host
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
counter, which `registry.register_host` carries over like `changes`. `reverse` stores `"reversed": true` (and `"drifted": true` for a drift) after
each change, so an interrupted removal resumes where it stopped and reverses nothing twice.
`reversed` counts the drifted entries too. A JSON file is rewritten as `JSON.stringify(value, null,
2)` with its original final newline.

Every read-modify-write of a registration (`registry.register_host`, `registry.forget`, `changes.apply`,
and each step of `changes.reverse`, from its holder check to its store) holds one OS file lock on
`<state>/hosts/.lock`, so two hosts racing on a shared line or directory, or a host registering while
another call records its change, never lose or orphan anything. The OS releases the lock when a
process exits, so a crash never leaves it held. A `commands` call holds it once for the whole call
and applies its changes under it, so it never takes it twice.

### Commands

A host's commands are shims in `<state>/bin`, recorded as `file` changes of that host. On POSIX
the shim is `<name>`, an `sh` script running `exec '<target>' '<argument>'... "$@"`, every word
single-quoted. On Windows it is `<name>.exe`, a copy of `runtime-launcher.exe`, beside `<name>.shim`,
UTF-8 lines: `spisor-shim 1`, the target's path, then each fixed argument. The launcher reads the
`.shim` next to its own path, refuses one without that first line, runs the target with the fixed
arguments quoted as `CommandLineToArgvW` reads them back and then its own arguments exactly as given,
and exits with the target's exit code. `arguments`, absent meaning none, are the fixed arguments a
command always passes before the caller's, so a target can be an interpreter and its script. It replaces a `.cmd` shim because `cmd.exe` reads a
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
shim is rewritten in place, under the record it already has, when the target or the arguments
changed, but the entry itself is never changed by a host: a name
the user renamed stays renamed, a command the user deleted stays deleted, and a command the user
added under the same name is never matched by a host's request. `rename`, `remove` and
`add` are the user's, mark the entry `owner: "user"`, and refuse a held name rather than fall back.
`remove` refuses the last active command of a host with `requiresCommand`; renaming it is allowed.
A name is a letter or digit followed by letters, digits, `-`, `_` or `.`, at most 64; a target is
an absolute path with no line break, and `arguments` an array of strings with none.

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
`"portable"`, or, when no `layer` is given, unless the key is declared: a declared key goes to the layer its declaration names, and to the portable one when it names none. A host with no portable layer omits `portable` and a portable write is refused. An
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

It is held to recorded answers in its own CI: `parity_test` lays out each case under `parity/cases`,
calls the built runtime through its definitions as a host does, and compares every answer and the
files it left with `parity/expected`, which holds basekit's answers with the rulings that depart from
basekit folded in.

## Using it

Check it out at `siblings/library` beside weivel's `core` and `standard` at `siblings/core` and `siblings/standard`,
each at the commit your `repo.json` pins, as CI's dependency checkout does, and add it as a subdirectory:

```cmake
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/siblings/library ${PROJECT_BINARY_DIR}/siblings/library)
target_link_libraries(your_library PRIVATE spisor_library)
```

The library reads weivel from the TOP-LEVEL project's `siblings/`, so the consumer's pins are the ones built, and
it stops with an error naming the missing checkout otherwise. Both targets are built position independent, so they
link into a shared library on Linux. Tests are built only when this repository is the top-level project.

## Reading a JSON file

A library reads every JSON file it keeps (a registry file, say) through `spisor_parse_json_reporting`
instead of calling cJSON directly, and is told whether memory or the text failed. It reads a document as
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
- **Environment lookups are case-sensitive, except on Windows**, where names match ignoring ASCII
  case, as the host's `process.env` matches them.

## The one command

`install/install.sh` (macOS and Linux) and `install/install.ps1` (Windows) are what the one command
runs. Each reads the catalog it is given, fetches the latest `spisor-host` build for this platform
(the `spisor-host` executable, `spisor-boot`, and on Windows `runtime-launcher.exe`) into
`<cache>/bundles/spisor-host/<version>/`, every file checked against its size and SHA-256, and runs
that `spisor-host install --catalog=<catalog>` with the rest of its arguments, which installs
everything else. A file already there with its hash is not fetched again. They need only what each
OS ships: curl, `sha256sum` or `shasum`, and awk on macOS and Linux (a Linux without curl is told to
install it), PowerShell on Windows.

No catalog is published yet, so a run without one is refused. Pass a local one built by
`spisor-host catalog build`:

```
sh install/install.sh <path to catalog.json> [install options]
powershell -ExecutionPolicy Bypass -File install/install.ps1 <path to catalog.json> [install options]
```

`spisor-host marketplace build --list <path> --out <path> [--token-env <variable> --token-host <host>]` builds the generated copy of a marketplace
list with `marketplace.build`, stamping it with the current UTC time, and writes it to `--out` atomically, creating the folder
it lies in. `--out` may not be the list itself. A copy already at `--out` is the previous copy: an entry whose repository cannot
be looked up is carried over from it unchanged, or, when it has none, printed as skipped with its reason, and the copy is
still written. `--token-env` names an environment variable holding an access token, which `identity.resolve` sends as an
authorization header only to the host the ids name (never to another host, a redirect included) and which is never printed or
written. `--token-host` is required with `--token-env` and names the one host the token is for, `github.com` for github ids or the
instance host for gitea and forgejo ids; a variable that is unset, empty or not usable as a token is refused, and so is a list
holding an id on another host, before any lookup. A list with entries whose copy would hold none (every lookup refused, rate limited, not found or lost to the
network) is refused with the reasons seen and writes nothing; an empty list still writes an empty copy.
Gitea, Forgejo and Codeberg answer 401 to a lookup by number without a token. A refusal writes nothing, leaves an existing
copy as it was and exits 1; a name in a message is printed with its control characters escaped, and the JSON the commands
print has 0x7F and U+0080 to U+009F escaped as well.

`spisor-host install --name <[marketplace/]name> --application <id> [--accept <id>] [--accept-primary <id>]` installs one
plugin by the name a marketplace gives it, from the marketplaces listed under `marketplaces` in the portable `settings.json`,
into the `plugin` folder (from `paths.resolve`) of the home of the registered application `--application` names; without
`--application` it is refused. No host runs a plugin deployed this way yet, since hosts move to deployed plugins in plan four.
It reads each source's copy with `marketplace.read` into a fresh folder under the cache that it removes again, and a source
that cannot be read is printed with its fix and skipped while the others are still searched; a source that names a `provider`
is printed as skipped, since only a plugin can ask it. `marketplace.lookup` settles the name: a name found under two ids lists
each `<marketplace>/<name>` form and installs nothing, and one found nowhere says so. `marketplace.remember` checks the id
against the one on record for that marketplace and name, and, for a bare name, for the bare name too, so a name that moves
to another marketplace asks as a changed id does; a different one prints both ids and installs nothing until the command is
run again with `--accept` naming the new id. `install.by_id` then installs the id into the cache and deploys it, and only
once that has succeeded are the names recorded, so a failed first install records nothing. A primary that the repo's
copies newly name prints the recorded and new primary and installs nothing until `--accept-primary` names it; the two flags
are never taken for each other. The JSON of what was installed is printed, then what `install.warnings` finds the plugin
depends on by a name. Everything printed that came from a source, a copy or a refusal is printed with its control characters
escaped; the command takes no `--catalog` or `--plugin`.

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
