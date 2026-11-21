#ifndef RUNTIME_PUBLIC_STANDARD_H
#define RUNTIME_PUBLIC_STANDARD_H

/**
 * @file
 * runtime's `standard` services: the weivel standard's own definitions, served under the standard's ids with the
 * structs and the descriptions weivel/standard publishes, so this header declares their getters and nothing else. A
 * standard service's symbols read `runtime_standard_<name>_definition_of` and `_description` for the id
 * `weivel.<name>`, and its description is the standard's own, answered through a one-line forwarder.
 *
 * runtime serves `weivel.scope` and declares none of `weivel.config`, `weivel.secrets`, `weivel.paths` and
 * `weivel.log`: a plugin reaches its own of each only through the scope opened for it.
 */

#include "runtime/definition.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The static weivel_standard_scopes (`weivel/scope.h`), living until the library is unloaded. Its `open` reads the
 * plugin's manifest: the manifest's `id`, its `config` record and its `settings`, each declaration a record whose
 * `layer`, when it has one, is `application` or `portable`. A manifest that is unreadable or breaks one of those, or
 * whose config name or id is no portable path segment, is REFUSED at open, so one typo refuses that plugin's scope
 * rather than every later read. A scope opens only once the host is started. The scope's `config` is the plugin's
 * settings as runtime's `config` service merges them, flattened: a record's members become dotted keys, a text,
 * number or boolean is offered as its standard kind, and a null, a list, a member whose name is empty or holds a dot,
 * and anything a declared secret holds are not offered. A get answers NONE for any of them and a set refuses a
 * declared secret, which only `weivel.secrets` reaches. A set writes a declared key to its declared layer, the
 * portable one when it declares none, and any other key to the application layer; what runtime's `config` would
 * refuse as malformed there, such as a key with an empty segment, the scope refuses. The scope's `secrets` are
 * runtime's `secrets` under the manifest's id alone, refusing bytes that are not UTF-8 text without a NUL byte. Its
 * `paths` answer folders that are the plugin's alone, each named by its id: `cache` its folder in the cache place, and
 * `config` and `plugin` its own folder, `<portable>/plugins/<id>` where its portable settings file lives, or its
 * folder in the application's plugin folder for a host with no portable layer.
 * Its `log` hands each entry to a writer thread of the started host and never waits
 * on a file: a write is refused when the writer already holds 1024 entries or no host is started, a TRACE entry is
 * dropped, and the host's end writes every entry handed over before it returns. A manifest whose id no folder can
 * carry is refused at open.
 */
RUNTIME_DEFINITION_API const void *runtime_standard_scope_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
