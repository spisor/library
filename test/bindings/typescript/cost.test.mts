import assert from "node:assert/strict";
import { it } from "node:test";
import { openRuntime } from "./runtime.mts";
import { coreLibraryPath, given, weivelModule } from "./weivel.mts";

const echoLibraryPath = given("SPISOR_BINDINGS_ECHO");

const { benchmarkLibraryCall, sampleInterleaved, summarise } = await weivelModule("benchmarks");

/**
 * How config.get is sampled: far fewer calls than the echo benchmark makes, since every call reads the plugin's
 * settings files afresh and so costs a file read where echo costs a copy.
 */
const configGetSampling = { warmupCalls: 200, callsPerBatch: 20, batches: 101 };

it("measures config.get through the generic binding beside weivel's own echo benchmark, judged by nothing", async () => {
  const opened = await openRuntime();
  try {
    const key = { key: "greeting" };

    const configGet = summarise(sampleInterleaved({ configGet: () => opened.scope.config.get!(key) }, configGetSampling).configGet).median as number;
    const echo = (await benchmarkLibraryCall({ echoLibraryPath, coreLibraryPath })) as { bareCall: { median: number }; weivelCall: { median: number } };

    console.log(
      `spisor-bindings-measurement language=typescript platform=${process.platform}-${process.arch} ` +
        `config.get=${configGet.toFixed(0)}ns echo=${echo.weivelCall.median.toFixed(0)}ns ` +
        `bare-echo=${echo.bareCall.median.toFixed(0)}ns ratio=${(configGet / echo.weivelCall.median).toFixed(1)}`,
    );
    assert.ok(configGet > 0 && echo.weivelCall.median > 0);
  } finally {
    await opened.close();
  }
});
