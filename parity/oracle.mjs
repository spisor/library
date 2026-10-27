import { existsSync, readFileSync, realpathSync } from "node:fs";
import { registerHooks } from "node:module";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import {
  caseNames,
  homeFiles,
  isHandEdit,
  logTime,
  normalised,
  performHandEdit,
  siblingPath,
  stageCase,
  workDirectory,
  writeExpected,
} from "./stage.mjs";

const BASEKIT_SOURCE = join(siblingPath("basekit"), "src");
const PATH_VARIABLES = { repos: "HUB_REPOS_SUBDIR", plugin: "HUB_PLUGIN_SUBDIR", cache: "HUB_CACHE_SUBDIR", config: "HUB_CONFIG_SUBDIR" };
const CLEARED_VARIABLES = ["CORE_LOG_CONSOLE", "NO_COLOR"];

function sourceFor(specifier, parentURL) {
  const emitted = new URL(specifier, parentURL);
  if (!specifier.endsWith(".js")) return emitted;
  const source = new URL(`${specifier.slice(0, -3)}.ts`, parentURL);
  return existsSync(fileURLToPath(source)) ? source : emitted;
}

/** Each declared function of a `.d.ts` as a no-op export, so a module importing it loads. */
function stubFor(missing) {
  const declarations = fileURLToPath(missing).replace(/\.js$/, ".d.ts");
  if (!existsSync(declarations)) return undefined;
  const names = [...readFileSync(declarations, "utf8").matchAll(/export declare function (\w+)/g)].map((match) => match[1]);
  const body = names.map((name) => `export function ${name}() {}`).join("\n");
  return `data:text/javascript,${encodeURIComponent(body)}`;
}

/**
 * @implNote basekit's full build needs its TeaVM and Gradle toolchain, while `plugin-runtime.ts`
 * reaches only Node built-ins and its own sources, so the oracle runs that source at the pinned
 * commit through Node's type stripping. Its imports name the `.js` its build would emit, which this
 * hook maps back to the `.ts` beside it. The query of the importing module is carried to every
 * relative import, so `?case=<name>` gives each case a fresh instance of the whole graph, its caches
 * and import-time `START_TIME` included. One module is stubbed, and only because it cannot load:
 * `generated/core.teavm.js` is TeaVM output a source checkout lacks, so its declared functions
 * become no-ops. They only redact what basekit writes under `events/`, which is never compared.
 */
function resolveEmittedNamesToSource() {
  registerHooks({
    resolve(specifier, context, nextResolve) {
      const parent = context.parentURL ? new URL(context.parentURL) : undefined;
      if (!specifier.startsWith(".") || !parent?.pathname.endsWith(".ts")) return nextResolve(specifier, context);
      const target = sourceFor(specifier, parent);
      target.search = parent.search;
      if (!existsSync(fileURLToPath(target))) {
        const stub = stubFor(target);
        if (stub) return { url: stub, shortCircuit: true };
      }
      return nextResolve(target.href, context);
    },
  });
}

/** A `Date` whose clock reads `now.value` when constructed without arguments, so basekit's times are the case's. */
function installFakeClock() {
  const RealDate = Date;
  const now = { value: RealDate.now() };
  class CaseDate extends RealDate {
    constructor(...parts) {
      if (parts.length === 0) super(now.value);
      else super(...parts);
    }

    static now() {
      return now.value;
    }
  }
  globalThis.Date = CaseDate;
  return now;
}

/**
 * Points basekit's process facts at the case: `HOME` at the staged home's parent, so no real app
 * registry is found, `HUB_*` cleared but for the case's `names`, which basekit reads from the
 * `HUB_<KIND>_SUBDIR` variables, and the case's own environment.
 */
function enterEnvironment(staged) {
  const saved = { ...process.env };
  for (const name of Object.keys(process.env)) {
    if (name.toUpperCase().startsWith("HUB_") || CLEARED_VARIABLES.includes(name.toUpperCase())) delete process.env[name];
  }
  process.env.HOME = staged.root;
  process.env.USERPROFILE = staged.root;
  for (const [kind, variable] of Object.entries(PATH_VARIABLES)) {
    if (typeof staged.names?.[kind] === "string") process.env[variable] = staged.names[kind];
  }
  Object.assign(process.env, staged.environment);
  return () => {
    for (const name of Object.keys(process.env)) delete process.env[name];
    Object.assign(process.env, saved);
  };
}

function withConsoleCaptured(work) {
  const lines = [];
  const original = console.error;
  console.error = (...parts) => lines.push(parts.join(" "));
  try {
    const answer = work();
    return lines.length > 0 ? { ...answer, console: lines.join("\n") } : answer;
  } finally {
    console.error = original;
  }
}

function present(member, value) {
  return value === undefined ? {} : { [member]: value };
}

/** A fresh basekit for one case, with the `from` its own set reports caught from its change hook. */
async function loadBasekit(staged) {
  const query = `?case=${encodeURIComponent(staged.name)}`;
  const { createPluginRuntime } = await import(pathToFileURL(join(BASEKIT_SOURCE, "plugin-runtime.ts")).href + query);
  const config = await import(pathToFileURL(join(BASEKIT_SOURCE, "config.ts")).href + query);
  const changes = [];
  config.setConfigChangeHook((_source, _key, change) => changes.push(change));
  return { runtime: createPluginRuntime(staged.manifest, staged.home), changes };
}

async function basekitAnswer(basekit, call) {
  const { runtime, changes } = basekit;
  const argument = call.argument;
  switch (call.function) {
    case "paths.resolve":
      return runtime.paths;
    case "config.all":
      return { values: runtime.config.all() };
    case "config.get":
      return present("value", runtime.config.get(argument.key));
    case "config.set":
      changes.length = 0;
      await runtime.config.set(argument.key, argument.value);
      return present("previous", changes[0]?.from);
    case "log.write":
      return withConsoleCaptured(() => {
        runtime.log[argument.level](argument.message);
        return {};
      });
    default:
      throw new Error(`a case calls ${call.function}, which the runtime library does not declare`);
  }
}

/** The basekit source as the loader names it in a stack, through the link and as it resolves. */
const BASEKIT_SOURCE_URLS = [pathToFileURL(BASEKIT_SOURCE).href, pathToFileURL(realpathSync(BASEKIT_SOURCE)).href];

/**
 * @implNote Only an error whose stack runs through basekit's own source is basekit's answer: the
 * oracle's own mistakes are thrown outside it and stop the run.
 */
function isThrownByBasekit(error) {
  const stack = error instanceof Error && typeof error.stack === "string" ? error.stack : "";
  return BASEKIT_SOURCE_URLS.some((url) => stack.includes(url));
}

async function answerOf(basekit, call, staged) {
  try {
    return normalised(JSON.parse(JSON.stringify(await basekitAnswer(basekit, call))), staged);
  } catch (error) {
    if (isThrownByBasekit(error)) return { thrown: true };
    throw error;
  }
}

async function answerCase(name, clock) {
  const staged = stageCase(name, workDirectory("oracle"));
  const leave = enterEnvironment(staged);
  try {
    clock.value = Date.parse(staged.clock.start);
    const basekit = await loadBasekit(staged);
    const answers = [];
    let logIndex = 0;
    for (const call of staged.calls) {
      if (isHandEdit(call)) {
        answers.push(performHandEdit(staged, call));
        continue;
      }
      if (call.function === "log.write") clock.value = Date.parse(logTime(staged, logIndex++));
      answers.push(await answerOf(basekit, call, staged));
    }
    writeExpected(name, answers, homeFiles(staged));
  } finally {
    leave();
  }
}

resolveEmittedNamesToSource();
const clock = installFakeClock();
const names = caseNames();
if (names.length === 0) throw new Error("the oracle found no cases, so it would hold the library to nothing");
for (const name of names) await answerCase(name, clock);
console.log(`basekit answered ${names.length} cases`);
