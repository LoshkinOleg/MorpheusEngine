import type { PipelineStepEvent, TurnExecutionState } from "../api";
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
  selectedPipelineEvents: PipelineStepEvent[];
  selectedDebugEntry: { turn: number; trace: unknown } | null;
  selectedTrace: Record<string, unknown> | null;
  prettyStageName: (stage: string) => string;
  renderCollapsibleJson: (summary: string, value: unknown) => ReactNode;
  renderConversationPayload: (moduleName: string, payload: unknown) => ReactNode;
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
        </div>
        {props.stepExecution ? (
          <p className="metaLine">
            Step Mode Turn: {props.stepExecution.turn} | Cursor: {props.stepExecution.cursor} | Status:{" "}
            {props.stepExecution.completed ? "completed" : "waiting for next step"}
          </p>
        ) : null}
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
        {props.selectedPipelineEvents.length > 0 ? (
          <section className="debugCard debugCardWide">
            <h3>Pipeline Events</h3>
            <div className="conversationList">
              {props.selectedPipelineEvents.map((event) => (
                <details key={`pipeline-step-${event.stepNumber}`} className="conversationAttempt">
                  <summary>
                    {event.stepNumber}. {props.prettyStageName(event.stage)} ({event.endpoint}) - {event.status}
                  </summary>
                  <div className="conversationAttemptBody">
                    {props.renderCollapsibleJson("Show request JSON", event.request)}
                    {props.renderCollapsibleJson("Show response JSON", event.response ?? null)}
                    {props.renderCollapsibleJson("Show warnings", event.warnings ?? [])}
                  </div>
                </details>
              ))}
            </div>
          </section>
        ) : null}
        {props.selectedDebugEntry && props.selectedTrace ? (
          <div className="debugStructured">
            <section className="debugCard">
              <h3>Intent</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.intent ?? null)}
            </section>
            <section className="debugCard">
              <h3>Loremaster</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.loremaster ?? null)}
            </section>
            <section className="debugCard">
              <h3>Proposal</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.proposal ?? null)}
            </section>
            <section className="debugCard">
              <h3>Committed</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.committed ?? null)}
            </section>
            <section className="debugCard">
              <h3>Warnings</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.warnings ?? [])}
            </section>
            <section className="debugCard">
              <h3>Refusal</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.refusal ?? null)}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Narration Text</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace.narrationText ?? "")}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Raw Trace</h3>
              {props.renderCollapsibleJson("Show JSON", props.selectedTrace)}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Model Conversations</h3>
              {Object.entries(
                (props.selectedTrace.llmConversations as Record<string, unknown> | undefined) ?? {},
              ).length === 0 ? (
                <p className="metaLine">No model conversations captured for this turn.</p>
              ) : (
                <div className="conversationList">
                  {Object.entries(
                    (props.selectedTrace.llmConversations as Record<string, unknown> | undefined) ?? {},
                  ).map(([moduleName, payload]) => props.renderConversationPayload(moduleName, payload))}
                </div>
              )}
            </section>
          </div>
        ) : null}
      </article>
    </section>
  );
}
