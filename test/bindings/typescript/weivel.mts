import { createRequire } from "node:module";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

/** A path the CMake registration hands every bindings test, refused when it is missing. */
export function given(name: string): string {
  const value = process.env[name];
  if (value === undefined || value === "") {
    throw new Error(`${name} is not set; the bindings tests run under ctest, which sets it`);
  }
  return value;
}

/** weivel/typescript at its pinned commit, compiled into the build tree with its own dependencies beside it. */
export const weivelPackage = given("SPISOR_BINDINGS_WEIVEL_TYPESCRIPT");
export const coreLibraryPath = given("SPISOR_BINDINGS_CORE");
export const standardLibraryPath = given("SPISOR_BINDINGS_STANDARD");

/** A module of the weivel package, as its own `dist` holds it; the tests type only what they read. */
export async function weivelModule(module: string): Promise<any> {
  return import(pathToFileURL(join(weivelPackage, "dist", module, "index.js")).href);
}

/** The package's own koffi, the one its binding calls through. */
export const koffi: any = createRequire(join(weivelPackage, "package.json"))("koffi");

export const runtimeServiceIds = { host: "weivel.host", scope: "weivel.scope" } as const;
