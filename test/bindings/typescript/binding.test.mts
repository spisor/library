import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";
import { CopiedDescription, withMemberMoved } from "./moved-member.mts";
import { openRuntime, refusalOfServing, runtimeLibraryPath, type OpenedRuntime } from "./runtime.mts";
import { koffi, runtimeServiceIds, weivelModule } from "./weivel.mts";

const serveMode = 1;
const textKind = 1;
const numberKind = 2;

const { bindDefinition } = await weivelModule("host");

interface SettingValue {
  readonly kind: number;
  readonly text: string | null;
  readonly number: number;
}

let opened: OpenedRuntime;

before(async () => {
  opened = await openRuntime();
});

after(async () => {
  await opened?.close();
});

async function setting(key: string): Promise<SettingValue> {
  return ((await opened.scope.config.get!({ key })) as { value: SettingValue }).value;
}

describe("runtime bound from its descriptions by weivel/typescript", () => {
  it("answers weivel.host's start as a plan of nested structs naming the installed plugin", () => {
    assert.equal(opened.plan.mode, serveMode);
    assert.equal(opened.plan.plugins.length, 1);
    assert.match(opened.plan.plugins[0]!.manifest, /[\\/]a[\\/]1\.0\.0[\\/]plugin\.json$/);
    assert.match(opened.plan.plugins[0]!.entry, /[\\/]index\.js$/);
    assert.equal(opened.plan.action_plugin, null);
  });

  it("reads a plugin's own config back through the scope weivel.scope opened for it", async () => {
    const greeting = await setting("greeting");
    const depth = await setting("nested.depth");

    assert.equal(greeting.kind, textKind);
    assert.equal(greeting.text, "hello from the manifest's defaults");
    assert.equal(depth.kind, numberKind);
    assert.equal(depth.number, 2);
  });

  it("answers every setting as a counted array of structs each holding its value", async () => {
    const all = (await opened.scope.config.all!()) as { settings: { key: string; value: SettingValue }[] };

    assert.deepEqual(
      all.settings.map((each) => each.key),
      ["greeting", "nested.depth", "report"],
    );
    assert.match(all.settings[2]!.value.text!, /a-report\.txt$/);
  });

  it("refuses a description whose member offset disagrees with the platform's layout, and binds it unmoved", () => {
    const binary = koffi.load(runtimeLibraryPath);
    const copies = new CopiedDescription();
    try {
      const service = opened.declaration.services.find((candidate) => candidate.id === runtimeServiceIds.host)!;
      const definition = binary.func(service.functions[0]!.symbol, "void *", [])();
      const description = binary.func(service.description!, "void *", [])();

      const unmoved = withMemberMoved(copies, description, "weivel_standard_host_plan", "plugins", 0);
      const moved = withMemberMoved(copies, description, "weivel_standard_host_plan", "plugins", 8);

      assert.deepEqual(Object.keys(bindDefinition(definition, unmoved)), Object.keys(opened.runtimeHost));
      assert.throws(() => bindDefinition(definition, moved), /weivel_standard_host_plan\.plugins is described at offset/);
    } finally {
      copies.free();
      binary.unload();
    }
  });

  it("refuses paths served by identity's definition, which only the check that a description answers its definition sees", async () => {
    const answers = /was refused by the check, status 2 at answered_by: answered_by answers another definition/;

    assert.match((await refusalOfServing("paths", "runtime_identity_definition_of", true))!, answers);
    assert.equal(await refusalOfServing("paths", "runtime_identity_definition_of", false), undefined);
  });

  it("refuses identity served by paths' definition, as its first shape alone already did", async () => {
    assert.match((await refusalOfServing("identity", "runtime_paths_definition_of", true))!, /status 2 at answered_by/);
    assert.match((await refusalOfServing("identity", "runtime_paths_definition_of", false))!, /below its first shape/);
  });
});
