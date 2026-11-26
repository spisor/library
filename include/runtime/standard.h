#ifndef RUNTIME_PUBLIC_STANDARD_H
#define RUNTIME_PUBLIC_STANDARD_H

/**
 * @file
 * runtime's `standard` services: the weivel standard's own definitions, served under the standard's ids with the
 * structs weivel/standard publishes, so this header declares their getters and nothing else. A standard service's
 * symbols read `runtime_standard_<name>_definition_of` and `_description` for the id `weivel.<name>`, and runtime
 * generates its description from the standard's header, naming that getter as the function answering it.
 *
 * runtime serves `weivel.host` and `weivel.scope` and declares none of `weivel.config`, `weivel.secrets`,
 * `weivel.paths` and `weivel.log`: a plugin reaches its own of each only through the scope opened for it.
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

/**
 * The static weivel_standard_host (`weivel/host.h`), living until the library is unloaded, which runs a loader's
 * plugins: a process makes one run at a time, from a `start` answering OK until its `stop`.
 *
 * `start` reads its arguments, after the layer's runtime, `javascript` or `native`, as `<loader id>` for a SERVE run,
 * `<loader id> in-process` for a SERVE run that serves the process it runs in alone, or `<loader id> action <plugin>
 * <action> <input> [<result>]` for an ACTION run, the input a JSON object and the result, when given, the absolute
 * path of a file directly inside the state place's `results` folder, no segment of it `.` or `..`, and reads this
 * process's environment. An in-process run, as a host started inside another application's
 * process makes, never reads or writes the loader's holders and ready records, so any number of them serve at once,
 * each in its own process: its `started` publishes nothing, its `tick` stamps its run record and runs no prune, and
 * only a stop request ends it. The layer is the javascript-host at
 * the version its boot hint names, beside every library that hint names, and hosts' start plans the run, a SERVE
 * run as the loader's session start; an in-process one reuses the run a live run record names, as a start that is
 * not the session start does, so it never switches to a scheduled version under a running host. The plan loads every
 * hinted library that has a binary but runtime itself, which the program was started with, each with the
 * `library.json` beside it, and activates every plugin of hosts' start's
 * plan that cache's locate finds installed, in id order. An ACTION plan's `action_plugin` is the `manifest` of the
 * plan's plugin of that id, the very text, since the program matches it exactly; `action_name` is the action's id,
 * `action_service` and `action_method` the service and method its declared `call` names, so the program calls that
 * method of the service the plugin registered, and `action_arguments` what ui's action merges from that manifest: the
 * declared argument, then the input. A SERVE plan leaves every member naming the action NULL. The scopes then begin,
 * over the loader's home and the places, so `weivel.scope` opens the plan's plugins' scopes until `stop`.
 *
 * `start` answers MALFORMED for a refusal or plan short of its first shape, a plan that is not empty, and arguments
 * that are NULL beside a count or hold a NULL. It answers REFUSED for arguments naming no run, a result path that is
 * no such file, a second start before a `stop`, a layer with no hinted version, a start hosts' start refuses, a
 * located plugin whose manifest names no entry installed beside it, a plugin the plan does not run, a manifest that
 * cannot be read, an action ui's action refuses, as one the manifest does not declare, an ACTION run whose plan's `size` cannot hold `action_service` and
 * `action_method`, the program too old for the run, and nothing written past that size, and scopes that cannot
 * begin; and OUT_OF_MEMORY. A start that does not answer OK leaves no run record of its own and no scopes begun, and
 * a refused SERVE start that is not in-process publishes its refusal to the loader's ready record once the places
 * resolved.
 *
 * `started`, `tick`, `finished` and `stop` refuse when no run is started. `started` takes one outcome per plugin of
 * the plan, in its order, each naming the manifest the plan named, and answers MALFORMED for any other, and REFUSED
 * for a second `started`. A plugin that was refused or failed leaves the run running with the plugins that
 * activated, as the archived host quarantined one: an ACTION run's program tells `finished` a failure when the
 * action's own plugin was not activated. A SERVE run then publishes to the ready record of its identity that it is
 * ready, when every plugin activated, and otherwise a refusal naming the first plugin that did not, never an empty
 * environment. Its environment is empty but in the layer holding the front door of a loader whose standard declares
 * `application.proxy`: there it sets the proxy's `baseUrlVariable` to `http://127.0.0.1:<port>` and its placeholder's
 * `variable` to its `value`. The port is declared by the front door, the standard's first required plugin, as the
 * `port` access of its manifest with mode `listen`: a number, or `setting:<key>` read from that plugin's settings.
 * That layer publishes a refusal instead when the proxy does not hold its `baseUrlVariable` or its placeholder's
 * `variable` or `value` as text, when the standard requires no plugin, or when the front door is not among the run's
 * plugins, has a manifest that cannot be read, declares no fixed port it listens on, or names a setting that is not
 * set or holds no port from 1 to 65535.
 *
 * `tick` is a SERVE run's alone, after its `started` answered OK, and REFUSED otherwise, since an ACTION run never
 * ticks. It acts only through its `now` and reads no clock: every `recordRunMilliseconds` of hosts' start's timers
 * it stamps the run record alive at `now` and runs processes' prune for this process at `now`, and every
 * `updateCheckMilliseconds` it runs updates' check at
 * `now`, its damaged versions those cache's locate could not run, the first tick taking both. Each timer is next due
 * at `now` plus its interval, so a late tick runs it once, and `next` is the earlier of the two. A prune that answers
 * that no holder is alive, or that another process whose stamp is fresh is recorded as the loader's host, ends the run
 * with a REFUSED tick, as do
 * `pruneFailuresBeforeExit` turns in a row whose stamp or prune was refused; a refused update check changes nothing.
 * Each failed turn, a refused check and each version a check refused are written as a warning to the loader's own
 * log, under the loader's id at `now`, as the archived host wrote them to stderr.
 * A tick holds the run's lock while it waits on the network, and a scope's operations never take that lock.
 *
 * `finished` is an ACTION run's alone, once, after its `started` answered OK. An action that answered, in a run given
 * a result path, has its result written there as compact JSON, `null` for a value of kind NONE, as a new file in a
 * results folder made when missing; `finished` answers MALFORMED for a result that is no well-formed value tree and
 * REFUSED when the results folder is not a real folder, a link to one included, or anything already stands at the
 * path, and the run then fails. `stop` ends the scopes, removes the run
 * record the plan wrote, ends the run and answers its exit code, all under the lock a start takes, so a start racing
 * it begins its scopes only after they ended. The exit code is 0 for a SERVE run that ends on a stop request or its
 * holders' word and for an action that answered, and 1 for an action that threw or never ran and for a run a call of
 * which answered other than OK before it ended, but the tick that ended it on its holders' word; a call after the run
 * ended leaves its exit code as it was.
 */
RUNTIME_DEFINITION_API const void *runtime_standard_host_definition_of(void);

#ifdef __cplusplus
}
#endif

#endif
