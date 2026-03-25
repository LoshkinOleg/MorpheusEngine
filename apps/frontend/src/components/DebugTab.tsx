import type { TurnExecutionState } from "../api";
import type { ReactNode } from "react";

interface DebugTabProps {
  runId: string | null;
  stepLoading: boolean;
  stepPlayerInput: string;
  onStepPlayerInputChange: (value: string) => void;
  onStartStepMode: () => void;
  onNextStepMode: () => void;
  onRunStepModeToEnd: () => void;
  stepExecution: TurnExecutionState | null;
  debugTurnOptions: number[];
  selectedDebugTurn: number | null;
  onStepDebugTurn: (direction: "up" | "down") => void;
  onSelectedDebugTurnChange: (value: number) => void;
  selectedDebugEntry: { turn: number; trace: unknown } | null;
  renderCollapsibleJson: (summary: string, value: unknown, options?: { open?: boolean }) => ReactNode;
  dumpTracesLoading: boolean;
  dumpTracesError: string | null;
  dumpTracesResult: { outputPath: string; traceCount: number } | null;
  onDumpTraces: () => void;
}

export function DebugTab(props: DebugTabProps) {
  return (
    <section className="splitPanels">
      <article className="panel">
        <h2>Debug UI</h2>
        <div className="moduleDebugActions">
          <input
            value={props.stepPlayerInput}
            onChange={(e) => props.onStepPlayerInputChange(e.target.value)}
            placeholder="Player input for stepped execution"
            disabled={props.stepLoading || !props.runId}
          />
          <button
            type="button"
            onClick={props.onStartStepMode}
            disabled={props.stepLoading || !props.runId}
            title="Begin a new stepped turn"
          >
            {props.stepLoading ? "Working..." : "Start Step Mode"}
          </button>
          <button
            type="button"
            onClick={props.onNextStepMode}
            disabled={props.stepLoading || !props.stepExecution || props.stepExecution.completed}
            title="Advance one pipeline stage"
          >
            Next Step
          </button>
          <button
            type="button"
            onClick={props.onRunStepModeToEnd}
            disabled={props.stepLoading || !props.stepExecution || props.stepExecution.completed}
            title="Advance all remaining stages"
          >
            Run To End
          </button>
          <button
            type="button"
            onClick={props.onDumpTraces}
            disabled={props.dumpTracesLoading || !props.runId}
            title="Write all recorded turn traces to the session saved folder"
          >
            {props.dumpTracesLoading ? "Dumping..." : "Dump All Turn Traces"}
          </button>
        </div>
        {props.stepExecution ? (
          <p className="metaLine">
            Step Mode Turn: {props.stepExecution.turn} | Cursor: {props.stepExecution.cursor} | Status:{" "}
            {props.stepExecution.completed ? "completed" : "waiting for next step"}
          </p>
        ) : null}
        {props.dumpTracesResult ? (
          <p className="metaLine">
            Dumped {props.dumpTracesResult.traceCount} traces to {props.dumpTracesResult.outputPath}
          </p>
        ) : null}
        {props.dumpTracesError ? <p className="statusError">Error: {props.dumpTracesError}</p> : null}
        {props.debugTurnOptions.length > 0 ? (
          <div className="debugControls">
            <label htmlFor="debug-turn-select">Turn:</label>
            <button
              type="button"
              onClick={() => props.onStepDebugTurn("up")}
              disabled={
                props.selectedDebugTurn === null ||
                props.debugTurnOptions.findIndex((value) => value === props.selectedDebugTurn) <= 0
              }
              title="Previous turn"
            >
              ↑
            </button>
            <select
              id="debug-turn-select"
              value={props.selectedDebugTurn ?? ""}
              onChange={(e) => props.onSelectedDebugTurnChange(Number(e.target.value))}
            >
              {props.debugTurnOptions.map((turnOption) => (
                <option key={turnOption} value={turnOption}>
                  {turnOption}
                </option>
              ))}
            </select>
            <button
              type="button"
              onClick={() => props.onStepDebugTurn("down")}
              disabled={
                props.selectedDebugTurn === null ||
                props.debugTurnOptions.findIndex((value) => value === props.selectedDebugTurn) < 0 ||
                props.debugTurnOptions.findIndex((value) => value === props.selectedDebugTurn) >=
                  props.debugTurnOptions.length - 1
              }
              title="Next turn"
            >
              ↓
            </button>
          </div>
        ) : null}
        {props.debugTurnOptions.length === 0 ? <p className="metaLine">No debug entries yet.</p> : null}
        {props.selectedDebugEntry ? (
          <section className="debugCard debugCardWide">
            <h3>Raw Trace</h3>
            {props.renderCollapsibleJson("Trace JSON", props.selectedDebugEntry.trace ?? null, { open: true })}
          </section>
        ) : null}
      </article>
    </section>
  );
}
