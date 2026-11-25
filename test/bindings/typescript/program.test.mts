import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { describe, it } from "node:test";
import { coreLibraryPath, given, standardLibraryPath, weivelModule, weivelPackage } from "./weivel.mts";

const pluginSources = given("SPISOR_BINDINGS_PLUGINS");
const runtimeName = given("SPISOR_BINDINGS_RUNTIME_NAME");
const runBound = 60_000;

const { bundlePlugin, bundleProgram } = await weivelModule("bundle");

interface Ran {
  readonly exit: number | null;
  readonly output: string;
}

/** The environment a loader's launch gives the program: the home's places, and its user's home inside it. */
function placesOf(home: string): NodeJS.ProcessEnv {
  return {
    ...process.env,
    SPISOR_HOME: join(home, "portable"),
    SPISOR_STATE: join(home, "state"),
    SPISOR_CACHE: join(home, "cache"),
    SPISOR_SECRETS: join(home, "secrets"),
    HOME: join(home, "user"),
    USERPROFILE: join(home, "user"),
  };
}

/** Bundles each plugin's TypeScript entry into the entry its install names, as a plugin's build would. */
async function installEntries(home: string, plugins: readonly string[]): Promise<void> {
  for (const plugin of plugins) {
    const out = mkdtempSync(join(home, `bundle-${plugin}-`));
    try {
      copyFileSync(await bundlePlugin(join(pluginSources, plugin, "index.ts"), out), join(home, "cache", "bundles", plugin, "1.0.0", "index.js"));
    } finally {
      rmSync(out, { recursive: true, force: true });
    }
  }
}

/** Runs weivel/typescript's bundled host program over the home's runtime, `node <program> <runtime> <arguments>`. */
async function runProgram(label: string, home: string, programArguments: readonly string[]): Promise<Ran> {
  const programDirectory = join(weivelPackage, "programs", label);
  rmSync(programDirectory, { recursive: true, force: true });
  const { commandLine: program } = await bundleProgram(programDirectory, { nativeLibraries: { coreLibraryPath, standardLibraryPath } });
  const child = spawn(process.execPath, [program, join(home, "rt", runtimeName), ...programArguments], { env: placesOf(home), windowsHide: true });
  let output = "";
  child.stdout.on("data", (chunk: Buffer) => (output += chunk.toString()));
  child.stderr.on("data", (chunk: Buffer) => (output += chunk.toString()));
  const bound = setTimeout(() => child.kill(), runBound);
  try {
    const exit = await new Promise<number | null>((resolve, reject) => {
      child.on("error", reject);
      child.on("exit", resolve);
    });
    return { exit, output };
  } finally {
    clearTimeout(bound);
  }
}

function report(home: string, plugin: string): string | undefined {
  const path = join(home, `${plugin}-report.txt`);
  return existsSync(path) ? readFileSync(path, "utf8") : undefined;
}

const greetAction = ["javascript", "test-host", "action", "a", "greet", "{}"];
const greeted = "activated: hello from the manifest's defaults\ngreeted: world\n";

describe("weivel/typescript's bundled host program over runtime", () => {
  it("runs a TypeScript plugin that reads its own config through weivel.scope, then its action", async () => {
    const home = given("SPISOR_BINDINGS_HOME_ONE");
    await installEntries(home, ["a"]);

    const ran = await runProgram("one", home, greetAction);

    assert.equal(ran.exit, 0, ran.output);
    assert.equal(report(home, "a"), greeted, ran.output);
  });

  it("refuses the plugin whose manifest declares an unknown settings layer alone, and the run carries on", async () => {
    const home = given("SPISOR_BINDINGS_HOME_QUARANTINE");
    await installEntries(home, ["a", "b"]);

    const ran = await runProgram("quarantine", home, greetAction);

    assert.equal(ran.exit, 0, ran.output);
    assert.equal(report(home, "a"), greeted, ran.output);
    assert.equal(report(home, "b"), undefined, `b was activated despite its unknown layer: ${ran.output}`);
  });
});
