import type { ComplexWorkletFunction } from './commonTypes';
import {
  getValueUnpackerCode,
  setupCallGuard,
  setupConsole,
  setupCoreFunctions,
} from './initializers';
import { makeShareableCloneRecursive } from './shareables';

export type WorkletRuntime = {
  __hostObjectWorkletRuntime: never;
  readonly name: string;
};

export function createWorkletRuntime(
  name: string,
  initializer?: ComplexWorkletFunction<[], void>
) {
  const runtime = global._createWorkletRuntime(name, getValueUnpackerCode());

  runOnRuntimeSync(runtime, () => {
    'worklet';
    setupCoreFunctions();
    setupCallGuard();
    setupConsole();
  });

  if (initializer !== undefined) {
    runOnRuntimeSync(runtime, initializer);
  }

  return runtime;
}

export function runOnRuntimeSync(
  runtime: WorkletRuntime,
  worklet: ComplexWorkletFunction<[], void>
) {
  'worklet';
  global._runOnRuntime(runtime, makeShareableCloneRecursive(worklet));
}
