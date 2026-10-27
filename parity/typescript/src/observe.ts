import { isPluginError } from "@weivel/typescript/engine";
import type { LoadedLibrary } from "@weivel/typescript/host";
import { failureAnswer, homeFiles, isHandEdit, libraryArgument, normalised, performHandEdit, symbolOf } from "../../stage.mjs";
import type { DeclaredFunctions, ParityCall, StagedCase } from "../../stage.mjs";

export interface ObservedCase {
  answers: unknown[];
  files: Record<string, string>;
}

/** A refusal the library itself answered, which the bridge labels with the service and function before the library's own message. */
function isLibraryFailure(error: unknown, call: ParityCall): error is { detail: string; fix: string } {
  return isPluginError(error) && error.detail.startsWith(`${call.function}: `);
}

function observedAnswer(library: LoadedLibrary, declaration: DeclaredFunctions, staged: StagedCase, call: ParityCall, logIndex: number): unknown {
  try {
    const answer = library.call(symbolOf(declaration, call.function), libraryArgument(staged, call, logIndex));
    return normalised(answer, staged);
  } catch (error) {
    if (isLibraryFailure(error, call)) return failureAnswer(call.function, error.detail, error.fix, staged);
    throw error;
  }
}

/** Every call of one staged case through the library, in order, and the files they left. */
export function observeCase(library: LoadedLibrary, declaration: DeclaredFunctions, staged: StagedCase): ObservedCase {
  const answers: unknown[] = [];
  let logIndex = 0;
  for (const call of staged.calls) {
    if (isHandEdit(call)) {
      answers.push(performHandEdit(staged, call));
      continue;
    }
    answers.push(observedAnswer(library, declaration, staged, call, logIndex));
    if (call.function === "log.write") logIndex += 1;
  }
  return { answers, files: homeFiles(staged) };
}
