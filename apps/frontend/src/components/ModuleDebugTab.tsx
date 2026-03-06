import type { ReactNode } from "react";

type ModuleDebugTarget = "intent" | "loremaster" | "simulator" | "arbiter" | "proser";
type LoremasterEndpoint = "retrieve" | "pre" | "post";

interface ModuleDebugTabProps {
  moduleDebugTarget: ModuleDebugTarget;
  onModuleDebugTargetChange: (target: ModuleDebugTarget) => void;
  moduleDebugLoremasterEndpoint: LoremasterEndpoint;
  onModuleDebugLoremasterEndpointChange: (endpoint: LoremasterEndpoint) => void;
  currentGameProjectId: string;
  onCurrentGameProjectIdChange: (value: string) => void;
  moduleDebugRunId: string;
  onModuleDebugRunIdChange: (value: string) => void;
  moduleDebugTurn: number;
  onModuleDebugTurnChange: (value: number) => void;
  moduleDebugPlayerId: string;
  onModuleDebugPlayerIdChange: (value: string) => void;
  moduleDebugInput: string;
  onModuleDebugInputChange: (value: string) => void;
  moduleDebugProposalJson: string;
  onModuleDebugProposalJsonChange: (value: string) => void;
  moduleDebugLoading: boolean;
  onRunModuleDebug: () => void;
  moduleDebugError: string | null;
  moduleDebugResult: unknown;
  renderCollapsibleJson: (summary: string, value: unknown) => ReactNode;
  renderConversationPayload: (moduleName: string, payload: unknown) => ReactNode;
}

export function ModuleDebugTab(props: ModuleDebugTabProps) {
  return (
    <section className="splitPanels">
      <article className="panel">
        <h2>Module Debug UI</h2>
        <p className="metaLine">Call modules directly without running the full backend turn pipeline.</p>
        <div className="moduleDebugGrid">
          <label>
            Module
            <select
              value={props.moduleDebugTarget}
              onChange={(e) => props.onModuleDebugTargetChange(e.target.value as ModuleDebugTarget)}
            >
              <option value="intent">Intent Extractor</option>
              <option value="loremaster">Loremaster</option>
              <option value="simulator">Default Simulator</option>
              <option value="arbiter">Arbiter</option>
              <option value="proser">Proser</option>
            </select>
          </label>
          {props.moduleDebugTarget === "loremaster" ? (
            <label>
              Loremaster Endpoint
              <select
                value={props.moduleDebugLoremasterEndpoint}
                onChange={(e) => props.onModuleDebugLoremasterEndpointChange(e.target.value as LoremasterEndpoint)}
              >
                <option value="retrieve">/retrieve</option>
                <option value="pre">/pre</option>
                <option value="post">/post</option>
              </select>
            </label>
          ) : null}
          <label>
            Game Project
            <input
              value={props.currentGameProjectId}
              onChange={(e) => props.onCurrentGameProjectIdChange(e.target.value)}
              disabled={props.moduleDebugLoading}
            />
          </label>
          <label>
            Run ID
            <input
              value={props.moduleDebugRunId}
              onChange={(e) => props.onModuleDebugRunIdChange(e.target.value)}
              disabled={props.moduleDebugLoading}
            />
          </label>
          <label>
            Turn
            <input
              type="number"
              value={props.moduleDebugTurn}
              onChange={(e) => props.onModuleDebugTurnChange(Number(e.target.value))}
              disabled={props.moduleDebugLoading}
            />
          </label>
          <label>
            Player ID
            <input
              value={props.moduleDebugPlayerId}
              onChange={(e) => props.onModuleDebugPlayerIdChange(e.target.value)}
              disabled={props.moduleDebugLoading}
            />
          </label>
        </div>
        <label className="moduleDebugLabel">
          Player Input
          <textarea
            value={props.moduleDebugInput}
            onChange={(e) => props.onModuleDebugInputChange(e.target.value)}
            rows={4}
            disabled={props.moduleDebugLoading}
          />
        </label>
        <label className="moduleDebugLabel">
          Proposal JSON (used for Loremaster `/post` and Proser)
          <textarea
            value={props.moduleDebugProposalJson}
            onChange={(e) => props.onModuleDebugProposalJsonChange(e.target.value)}
            rows={8}
            disabled={props.moduleDebugLoading}
          />
        </label>
        <div className="moduleDebugActions">
          <button type="button" onClick={props.onRunModuleDebug} disabled={props.moduleDebugLoading}>
            {props.moduleDebugLoading ? "Running..." : "Run Module"}
          </button>
        </div>
        {props.moduleDebugError ? <p className="statusError">Error: {props.moduleDebugError}</p> : null}
        {props.moduleDebugResult && typeof props.moduleDebugResult === "object" ? (
          <div className="debugStructured">
            <section className="debugCard">
              <h3>Meta</h3>
              {props.renderCollapsibleJson(
                "Show JSON",
                (props.moduleDebugResult as Record<string, unknown>).meta ?? null,
              )}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Output</h3>
              {props.renderCollapsibleJson(
                "Show JSON",
                (props.moduleDebugResult as Record<string, unknown>).output ?? null,
              )}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Module Conversation</h3>
              {((props.moduleDebugResult as Record<string, unknown>).debug as Record<string, unknown> | undefined)
                ?.llmConversation ? (
                <div className="conversationList">
                  {props.renderConversationPayload(
                    props.moduleDebugTarget === "loremaster"
                      ? `loremaster_${props.moduleDebugLoremasterEndpoint}`
                      : props.moduleDebugTarget,
                    ((props.moduleDebugResult as Record<string, unknown>).debug as Record<string, unknown>)
                      .llmConversation,
                  )}
                </div>
              ) : (
                <p className="metaLine">No `llmConversation` in this module response.</p>
              )}
            </section>
            <section className="debugCard debugCardWide">
              <h3>Raw Module Response</h3>
              {props.renderCollapsibleJson("Show JSON", props.moduleDebugResult)}
            </section>
          </div>
        ) : (
          <section className="debugCard debugCardWide">
            <h3>Module Output</h3>
            {props.renderCollapsibleJson("Show JSON", props.moduleDebugResult ?? "No module output yet.")}
          </section>
        )}
      </article>
    </section>
  );
}
