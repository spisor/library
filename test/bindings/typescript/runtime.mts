import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { coreLibraryPath, given, runtimeServiceIds, standardLibraryPath, weivelModule } from "./weivel.mts";

export const runtimeLibraryPath = given("SPISOR_BINDINGS_RUNTIME");
const loaderId = "test-host";

const { Host } = await weivelModule("host");
const { EdgeSource } = await weivelModule("core");
const { loadStandard } = await weivelModule("standard");

export interface Bound {
  readonly [operation: string]: (argument?: object) => Promise<unknown>;
}

export interface Declaration {
  readonly services: readonly { readonly id: string; readonly description?: string; readonly functions: readonly { readonly symbol: string }[] }[];
}

export interface Plan {
  readonly mode: number;
  readonly plugins: readonly { readonly directory: string; readonly manifest: string; readonly entry: string }[];
  readonly action_plugin: string | null;
}

export interface Scope {
  readonly config: Bound;
  release(): void;
}

/** runtime loaded on a host given the standard's check, a SERVE run of the loader started, its first plugin scoped. */
export interface OpenedRuntime {
  readonly declaration: Declaration;
  readonly runtimeHost: Bound;
  readonly plan: Plan;
  readonly scope: Scope;
  close(): Promise<void>;
}

type Standard = ReturnType<typeof loadStandard>;
type DescriptionCheck = (description: object, definition: object | null) => unknown;

function createHost(descriptionCheck: DescriptionCheck): any {
  return Host.create({
    edgeSource: EdgeSource.Capabilities,
    coreLibraryPath,
    descriptionCheck,
    log: { debug() {}, info() {}, warn() {}, error() {} },
  });
}

function readDeclaration(standard: Standard): Declaration {
  const read = standard.readLibrary(readFileSync(join(dirname(runtimeLibraryPath), "library.json"), "utf8"));
  if (read.value === undefined) {
    throw new Error(`the standard refused runtime's declaration: ${read.refusal?.detail}`);
  }
  return read.value as Declaration;
}

/**
 * What loading runtime's declaration with the service `id`'s definition function pointed at `symbol` threw, or
 * undefined when it loaded. With `answersChecked` false the host's check passes the description the standard's
 * check alone and never asks whether it answers the definition, so what refuses the swap then is the binding's own.
 */
export async function refusalOfServing(id: string, symbol: string, answersChecked: boolean): Promise<string | undefined> {
  const standard = loadStandard(standardLibraryPath);
  const host = createHost((description, definition) => standard.checkDescription(description, answersChecked ? definition : null));
  const declaration = structuredClone(readDeclaration(standard)) as any;
  const service = declaration.services.find((candidate: { id: string }) => candidate.id === id);
  const served = service.functions.find((declared: { symbol: string }) => declared.symbol === `runtime_${id}_definition_of`);
  served.symbol = symbol;
  try {
    host.loadLibrary(declaration, runtimeLibraryPath, { withheld: [runtimeServiceIds.host, runtimeServiceIds.scope] });
    return undefined;
  } catch (refused) {
    return (refused as Error).message;
  } finally {
    await host.destroy();
  }
}

/** Loads runtime from its declaration as the host program does, keeping its standard services for the caller. */
export async function openRuntime(): Promise<OpenedRuntime> {
  const standard = loadStandard(standardLibraryPath);
  const host = createHost((description, definition) => standard.checkDescription(description, definition));
  const declaration = readDeclaration(standard);
  const described = host.loadLibrary(declaration, runtimeLibraryPath, { withheld: [runtimeServiceIds.host, runtimeServiceIds.scope] })
    .described as ReadonlyMap<string, Bound>;
  const runtimeHost = described.get(runtimeServiceIds.host)!;
  const plan = (await runtimeHost.start!({ arguments: ["typescript", loaderId] })) as Plan;
  const scope = (await described.get(runtimeServiceIds.scope)!.open!({ manifest_path: plan.plugins[0]!.manifest })) as Scope;
  return {
    declaration,
    runtimeHost,
    plan,
    scope,
    async close() {
      scope.release();
      await runtimeHost.stop!();
      await host.destroy();
    },
  };
}
