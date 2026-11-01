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
  mix. UTF-8 paths on every OS.
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
| `secrets.delete` | `places`, `plugin`, `name` | `{}`, after removing it from both places |
| `secrets.clear` | `places`, `plugin` | `{}`, after removing every secret of the plugin from both places (host only) |
| `secrets.reconcile` | `places` | `{ moved }`, the file-held secrets moved into a store that has appeared (host only, at start) |

`places` is what `places.resolve` answers. A secret is named `spisor/<plugin id>/<name>` in the store
and in the file. Each time the store takes a value, every secret in the file moves into it, written
to the store before it leaves the file, so an interruption never loses one. `forceFileStore`
(`SPISOR_SECRETS`) keeps every secret in the file and never touches the store.

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
