import type { ProposedDiff } from "@morpheus/shared";

export interface RuntimeContext {
  runId: string;
  turn: number;
  playerId: string;
  input: string;
}

export interface PipelineModule {
  name: string;
  run(ctx: RuntimeContext): Promise<ProposedDiff>;
}
