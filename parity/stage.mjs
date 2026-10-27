import { copyFileSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const PARITY_DIRECTORY = dirname(fileURLToPath(import.meta.url));
/** `PARITY_CASES` points a driver at another case set, which is how the suite is shown to refuse an empty one. */
export const CASES_DIRECTORY = process.env.PARITY_CASES ? resolve(process.env.PARITY_CASES) : join(PARITY_DIRECTORY, "cases");
export const EXPECTED_DIRECTORY = join(PARITY_DIRECTORY, "expected");
export const DIVERGENCES_FILE = join(PARITY_DIRECTORY, "divergences.json");
export const DECLARATION_FILE = join(PARITY_DIRECTORY, "..", "runtime", "library.json");
const DEFAULT_CLOCK = Object.freeze({ start: "2026-10-27T19:34:53.123Z", times: [] });
const COMPARED_DIRECTORIES = ["logs", "config"];

export function siblingPath(name) {
  return join(PARITY_DIRECTORY, "..", "siblings", name);
}

export function workDirectory(driver) {
  return join(PARITY_DIRECTORY, "work", driver);
}

export function currentPlatform() {
  return `${process.platform}-${process.arch}`;
}

export function caseNames() {
  return readdirSync(CASES_DIRECTORY, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => entry.name)
    .sort();
}

function definitionOf(name) {
  return JSON.parse(readFileSync(join(CASES_DIRECTORY, name, "case.json"), "utf8"));
}

/** How many calls the case files declare, counted apart from any run so a run that skips one is caught. */
export function declaredCallCount(names) {
  return names.reduce((total, name) => total + definitionOf(name).calls.length, 0);
}

/**
 * Lays one case out afresh under the driver's work directory: its fixture files copied byte for
 * byte into the home.
 */
export function stageCase(name, workRoot) {
  const caseDirectory = join(CASES_DIRECTORY, name);
  const definition = definitionOf(name);
  const root = resolve(workRoot, name);
  const home = join(root, "home");
  rmSync(root, { recursive: true, force: true });
  mkdirSync(home, { recursive: true });
  for (const file of definition.home ?? []) {
    const target = join(home, file);
    mkdirSync(dirname(target), { recursive: true });
    copyFileSync(join(caseDirectory, "home", file), target);
  }
  return {
    name,
    root,
    home,
    manifest: definition.manifest,
    names: definition.names,
    environment: definition.environment ?? {},
    clock: definition.clock ?? DEFAULT_CLOCK,
    calls: definition.calls,
  };
}

/** The harness's own step between library calls, an edit a person makes by hand, answered `{}` by everyone. */
export function isHandEdit(call) {
  return call.function === "file.write";
}

export function performHandEdit(staged, call) {
  const target = join(staged.home, call.argument.path);
  mkdirSync(dirname(target), { recursive: true });
  writeFileSync(target, call.argument.text);
  return {};
}

/** The time of the case's next log call, `start` when the case names no more times. */
export function logTime(staged, logIndex) {
  return staged.clock.times[logIndex] ?? staged.clock.start;
}

/**
 * The argument the library is called with for one case call: the call's own argument with the
 * home, the names and the manifest the case holds, and for a log call the clock and `NO_COLOR`.
 */
export function libraryArgument(staged, call, logIndex) {
  const common = { home: staged.home, ...(staged.names !== undefined ? { names: staged.names } : {}) };
  if (call.function === "paths.resolve") return { ...common, ...call.argument };
  const plugin = { ...common, plugin: staged.manifest.id, ...(staged.manifest.config !== undefined ? { config: staged.manifest.config } : {}) };
  if (call.function !== "log.write") return { ...plugin, ...call.argument };
  return {
    ...plugin,
    start: staged.clock.start,
    time: logTime(staged, logIndex),
    noColor: staged.environment.NO_COLOR !== undefined,
    ...call.argument,
  };
}

/** The symbol a case's `service.function` names in the declaration. */
export function symbolOf(declaration, functionName) {
  const [serviceId, name] = functionName.split(".");
  const found = declaration.services.find((service) => service.id === serviceId)?.functions.find((entry) => entry.name === name);
  if (!found) throw new Error(`the declaration has no ${functionName}`);
  return found.symbol;
}

/**
 * An answer with the case's own directories written back as their tokens and every backslash as a
 * slash, so it reads the same wherever and on whichever OS the case was laid out.
 */
export function normalised(value, staged) {
  if (typeof value === "string") return value.split(staged.home).join("{home}").split(staged.root).join("{root}").split("\\").join("/");
  if (Array.isArray(value)) return value.map((element) => normalised(element, staged));
  if (value !== null && typeof value === "object") {
    return Object.fromEntries(Object.entries(value).map(([key, element]) => [key, normalised(element, staged)]));
  }
  return value;
}

/** A refusal as every implementation records it: the library's message without the bridge's `service.function: ` label, and its fix. */
export function failureAnswer(functionName, detail, fix, staged) {
  const label = `${functionName}: `;
  const message = detail.startsWith(label) ? detail.slice(label.length) : detail;
  return normalised({ failure: { message, fix } }, staged);
}

function filesUnder(directory, home, into) {
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) filesUnder(path, home, into);
    else into[relative(home, path).split("\\").join("/")] = readFileSync(path, "utf8");
  }
}

/**
 * Every file under the home's log and config directories, by home-relative path.
 *
 * @implNote Only these, as spec section 5.2 rules: basekit also writes its bus and activity under
 * `events/`, which the runtime never writes. A renamed config directory is listed too.
 */
export function homeFiles(staged) {
  const files = {};
  const directories = new Set(COMPARED_DIRECTORIES);
  if (typeof staged.names?.config === "string") directories.add(staged.names.config);
  for (const directory of directories) {
    const path = join(staged.home, directory);
    if (existsSync(path) && statSync(path).isDirectory()) filesUnder(path, staged.home, files);
  }
  return files;
}

export function readExpected(name) {
  return JSON.parse(readFileSync(join(EXPECTED_DIRECTORY, `${name}.json`), "utf8"));
}

export function writeExpected(name, answers, files) {
  mkdirSync(EXPECTED_DIRECTORY, { recursive: true });
  writeFileSync(join(EXPECTED_DIRECTORY, `${name}.json`), JSON.stringify({ platform: currentPlatform(), answers, files }, null, 2) + "\n");
}

export function readDivergences() {
  const divergences = JSON.parse(readFileSync(DIVERGENCES_FILE, "utf8")).divergences;
  const repeated = repeatedCalls(divergences);
  if (repeated.length > 0) throw new Error(`divergences.json records a call more than once: ${repeated.join(", ")}`);
  return divergences;
}

/** The `case/call` pairs divergences.json names more than once, since a driver could hold only one of them. */
export function repeatedCalls(divergences) {
  const seen = new Set();
  const repeated = new Set();
  for (const entry of divergences) {
    if (entry.call === undefined) continue;
    const pair = `${entry.case}/${entry.call}`;
    if (seen.has(pair)) repeated.add(pair);
    seen.add(pair);
  }
  return [...repeated];
}
