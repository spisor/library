import { readFileSync } from "node:fs";
import { openLibrary, parseDeclaration } from "@weivel/typescript/host";
import { describe, expect, it } from "vitest";
import { binaryPath } from "../../binary.mjs";
import { DECLARATION_FILE, caseNames, currentPlatform, declaredCallCount, readDivergences, readExpected, stageCase, workDirectory } from "../../stage.mjs";
import type { Divergence } from "../../stage.mjs";
import { mismatchesOf } from "./compare.js";
import type { Mismatch } from "./compare.js";
import { observeCase } from "./observe.js";

const IMPLEMENTATION = "typescript";

describe("the runtime library through the TypeScript host", () => {
  it("answers every case as basekit does", () => {
    const divergences = readDivergences().filter((entry) => entry.implementations.includes(IMPLEMENTATION));
    const names = caseNames();
    expect(names.length, "no cases were found, so the run would hold the library to nothing").toBeGreaterThan(0);
    expect(readDivergences().map((entry) => entry.case).filter((name) => !names.includes(name)), "a divergence names a case the run does not have").toEqual([]);
    const declaration = parseDeclaration(readFileSync(DECLARATION_FILE, "utf8"));
    const library = openLibrary(binaryPath(), declaration);
    const mismatches: Mismatch[] = [];
    const held = new Set<Divergence>();
    let calls = 0;

    try {
      for (const name of names) {
        const expected = readExpected(name);
        expect(expected.platform, `${name} was answered by basekit on another platform`).toBe(currentPlatform());
        const staged = stageCase(name, workDirectory(IMPLEMENTATION));
        expect(expected.answers.length, `${name} has a stale expected file`).toBe(staged.calls.length);
        const observed = observeCase(library, declaration, staged);
        calls += observed.answers.length;
        const ofCase = divergences.filter((entry) => entry.case === name);
        for (const entry of ofCase) if (entry.call === undefined || entry.call < staged.calls.length) held.add(entry);
        mismatches.push(...mismatchesOf(name, observed, expected, ofCase));
      }
    } finally {
      library.close();
    }

    console.log(`typescript: ${names.length} cases, ${calls} calls, ${held.size} recorded divergences held, ${mismatches.length} mismatches`);
    expect(calls, "the run made fewer or more calls than the case files declare").toBe(declaredCallCount(names));
    expect(divergences.filter((entry) => !held.has(entry)), "a divergence names no call the case makes").toEqual([]);
    expect(mismatches).toEqual([]);
  });
});
