import { join } from "node:path";
import { PARITY_DIRECTORY } from "./stage.mjs";

/** Where `cmake --build build --config Release` leaves the runtime library on this OS. */
export function binaryPath() {
  const build = join(PARITY_DIRECTORY, "..", "build", "bin");
  if (process.platform === "win32") {
    return join(build, "Release", "runtime.dll");
  }
  return join(build, process.platform === "darwin" ? "libruntime.dylib" : "libruntime.so");
}
