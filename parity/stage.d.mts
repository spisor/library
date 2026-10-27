export interface ParityCall {
  function: string;
  argument: Record<string, unknown>;
}

export interface CaseManifest {
  id: string;
  config?: Record<string, unknown>;
}

export interface CaseClock {
  start: string;
  times: string[];
}

export interface StagedCase {
  name: string;
  root: string;
  home: string;
  manifest: CaseManifest;
  names?: Record<string, unknown>;
  environment: Record<string, string>;
  clock: CaseClock;
  calls: ParityCall[];
}

export interface ExpectedCase {
  platform: string;
  answers: unknown[];
  files: Record<string, string>;
}

export interface Divergence {
  case: string;
  call?: number;
  answer?: unknown;
  files?: Record<string, string | null>;
  implementations: string[];
  because: string;
}

export interface DeclaredFunctions {
  services: { id: string; functions: { name: string; symbol: string }[] }[];
}

export const PARITY_DIRECTORY: string;
export const CASES_DIRECTORY: string;
export const EXPECTED_DIRECTORY: string;
export const DIVERGENCES_FILE: string;
export const DECLARATION_FILE: string;
export function siblingPath(name: string): string;
export function workDirectory(driver: string): string;
export function currentPlatform(): string;
export function caseNames(): string[];
export function declaredCallCount(names: readonly string[]): number;
export function stageCase(name: string, workRoot: string): StagedCase;
export function isHandEdit(call: ParityCall): boolean;
export function performHandEdit(staged: StagedCase, call: ParityCall): Record<string, never>;
export function logTime(staged: StagedCase, logIndex: number): string;
export function libraryArgument(staged: StagedCase, call: ParityCall, logIndex: number): Record<string, unknown>;
export function symbolOf(declaration: DeclaredFunctions, functionName: string): string;
export function normalised(value: unknown, staged: StagedCase): unknown;
export function failureAnswer(functionName: string, detail: string, fix: string, staged: StagedCase): unknown;
export function homeFiles(staged: StagedCase): Record<string, string>;
export function readExpected(name: string): ExpectedCase;
export function writeExpected(name: string, answers: unknown[], files: Record<string, string>): void;
export function readDivergences(): Divergence[];
