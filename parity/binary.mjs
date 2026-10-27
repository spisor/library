import { join } from "node:path";
import { PARITY_DIRECTORY } from "./stage.mjs";

/** Where `cmake --build build --config Release` leaves the runtime library on this OS. */
export function binaryPath() {
  const build = join(PARITY_DIRECTORY, "..", "build", "bin");
  return process.platform === "win32" ? join(build, "Release", "runtime.dll") : join(build, "libruntime.so");
}
