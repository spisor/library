import { isDeepStrictEqual } from "node:util";
import type { Divergence, ExpectedCase } from "../../stage.mjs";
import type { ObservedCase } from "./observe.js";

export interface Mismatch {
  case: string;
  at: string;
  observed: unknown;
  wanted: unknown;
  reason: string;
}

/**
 * @implNote A divergence is held in both directions: the library must still answer what the table
 * records, and basekit must still answer something else, so neither side drifts into or out of it
 * unnoticed.
 */
function held(name: string, at: string, observed: unknown, basekit: unknown, recorded: unknown): Mismatch | undefined {
  if (!isDeepStrictEqual(observed, recorded)) return { case: name, at, observed, wanted: recorded, reason: "differs from the recorded divergence" };
  if (isDeepStrictEqual(basekit, recorded)) return { case: name, at, observed, wanted: basekit, reason: "basekit agrees, so the recorded divergence is stale" };
  return undefined;
}

function agreed(name: string, at: string, observed: unknown, basekit: unknown): Mismatch | undefined {
  return isDeepStrictEqual(observed, basekit) ? undefined : { case: name, at, observed, wanted: basekit, reason: "differs from basekit" };
}

function answerMismatches(name: string, observed: ObservedCase, expected: ExpectedCase, divergences: readonly Divergence[]): Mismatch[] {
  return observed.answers.flatMap((answer, index) => {
    const divergence = divergences.find((entry) => entry.call === index);
    const at = `call ${index}`;
    const mismatch = divergence ? held(name, at, answer, expected.answers[index], divergence.answer) : agreed(name, at, answer, expected.answers[index]);
    return mismatch ? [mismatch] : [];
  });
}

function contentOf(files: Record<string, string>, path: string): string | null {
  return Object.hasOwn(files, path) ? files[path] : null;
}

function fileMismatches(name: string, observed: ObservedCase, expected: ExpectedCase, divergences: readonly Divergence[]): Mismatch[] {
  const recorded: Record<string, string | null> = Object.assign({}, ...divergences.map((entry) => entry.files ?? {}));
  const paths = [...new Set([...Object.keys(observed.files), ...Object.keys(expected.files), ...Object.keys(recorded)])].sort();
  return paths.flatMap((path) => {
    const at = `file ${path}`;
    const mine = contentOf(observed.files, path);
    const basekit = contentOf(expected.files, path);
    const mismatch = Object.hasOwn(recorded, path) ? held(name, at, mine, basekit, recorded[path]) : agreed(name, at, mine, basekit);
    return mismatch ? [mismatch] : [];
  });
}

/** Every way one case's run differs from basekit's, once the case's recorded divergences are applied. */
export function mismatchesOf(name: string, observed: ObservedCase, expected: ExpectedCase, divergences: readonly Divergence[]): Mismatch[] {
  return [...answerMismatches(name, observed, expected, divergences), ...fileMismatches(name, observed, expected, divergences)];
}
