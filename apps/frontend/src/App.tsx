import { useEffect, useState } from "react";
import {
  fetchTurnPipeline,
  fetchRunState,
  fetchSessions,
  invokeArbiterModule,
  invokeIntentModule,
  invokeLorePostModule,
  invokeLorePreModule,
  invokeLoreRetrieveModule,
  nextTurnStep,
  invokeProserModule,
  invokeSimulatorModule,
  openSessionSavedFolder,
  startTurnStep,
  startRun,
  submitTurn,
  type PipelineStepEvent,
  type TurnExecutionState,
} from "./api";

type ChatLine = {
  speaker: "system" | "you" | "engine";
  text: string;
};

const DEFAULT_PLAYER_ID = "entity.player.captain";
const DEFAULT_GAME_PROJECT_ID = "sandcrawler";
const STATE_POLL_INTERVAL_MS = 1500;
type ActiveTab = "game" | "debug" | "module_debug";
type ModuleDebugTarget =
  | "intent"
  | "loremaster"
  | "simulator"
  | "arbiter"
  | "proser";
type LoremasterEndpoint = "retrieve" | "pre" | "post";

type ConversationTrace = {
  attempts?: Array<{
    attempt?: number;
    requestMessages?: Array<{ role?: string; content?: string }>;
    rawResponse?: string;
    error?: string;
  }>;
  usedFallback?: boolean;
  fallbackReason?: string;
};

function asConversationTrace(value: unknown): ConversationTrace | null {
  if (!value || typeof value !== "object") return null;
  return value as ConversationTrace;
}

function toTitleCase(text: string): string {
  return text
    .replace(/_/g, " ")
    .split(" ")
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(" ");
}

function prettyStageName(stage: string): string {
  if (stage === "world_state_update") return "World State Update";
  if (stage === "frontend_input") return "Front End Input";
  return toTitleCase(stage);
}

function tryParseJsonText(text: string): unknown {
  const trimmed = text.trim();
  if (!(trimmed.startsWith("{") || trimmed.startsWith("["))) return text;
  try {
    return JSON.parse(trimmed) as unknown;
  } catch {
    return text;
  }
}

function normalizeJsonForDisplay(value: unknown): unknown {
  if (typeof value === "string") return tryParseJsonText(value);
  if (Array.isArray(value)) return value.map((item) => normalizeJsonForDisplay(item));
  if (value && typeof value === "object") {
    const next: Record<string, unknown> = {};
    for (const [key, nested] of Object.entries(value as Record<string, unknown>)) {
      next[key] = normalizeJsonForDisplay(nested);
    }
    return next;
  }
  return value;
}

function renderCollapsibleJson(summary: string, value: unknown) {
  const normalized = normalizeJsonForDisplay(value);
  return (
    <details className="jsonCollapse">
      <summary>{summary}</summary>
      <pre>{JSON.stringify(normalized, null, 2)}</pre>
    </details>
  );
}

export function App() {
  const [activeTab, setActiveTab] = useState<ActiveTab>("game");
  const [messages, setMessages] = useState<ChatLine[]>([]);
  const [input, setInput] = useState("");
  const [runId, setRunId] = useState<string | null>(null);
  const [turn, setTurn] = useState(1);
  const [isLoading, setIsLoading] = useState(false);
  const [errorText, setErrorText] = useState<string | null>(null);
  const [debugEntries, setDebugEntries] = useState<Array<{ turn: number; trace: unknown }>>([]);
  const [selectedDebugTurn, setSelectedDebugTurn] = useState<number | null>(null);
  const [currentGameProjectId, setCurrentGameProjectId] = useState(DEFAULT_GAME_PROJECT_ID);
  const [sessions, setSessions] = useState<Array<{ sessionId: string; createdAt: string }>>([]);
  const [moduleDebugTarget, setModuleDebugTarget] = useState<ModuleDebugTarget>("intent");
  const [moduleDebugLoremasterEndpoint, setModuleDebugLoremasterEndpoint] =
    useState<LoremasterEndpoint>("pre");
  const [moduleDebugInput, setModuleDebugInput] = useState("Look around.");
  const [moduleDebugPlayerId, setModuleDebugPlayerId] = useState(DEFAULT_PLAYER_ID);
  const [moduleDebugRunId, setModuleDebugRunId] = useState("debug-session");
  const [moduleDebugTurn, setModuleDebugTurn] = useState(1);
  const [moduleDebugProposalJson, setModuleDebugProposalJson] = useState(
    JSON.stringify(
      {
        moduleName: "default_simulator",
        operations: [
          {
            op: "observation",
            scope: "view:player",
            payload: {
              observerId: DEFAULT_PLAYER_ID,
              text: "You scan the dune horizon from the crawler deck.",
              turn: 1,
            },
            reason: "Manual module debug proposal",
          },
        ],
      },
      null,
      2,
    ),
  );
  const [moduleDebugResult, setModuleDebugResult] = useState<unknown>(null);
  const [moduleDebugError, setModuleDebugError] = useState<string | null>(null);
  const [moduleDebugLoading, setModuleDebugLoading] = useState(false);
  const [stepPlayerInput, setStepPlayerInput] = useState("Look around.");
  const [stepExecution, setStepExecution] = useState<TurnExecutionState | null>(null);
  const [stepPipelineEvents, setStepPipelineEvents] = useState<PipelineStepEvent[]>([]);
  const [stepLoading, setStepLoading] = useState(false);
  const hasActiveSessionInList = runId ? sessions.some((session) => session.sessionId === runId) : false;
  const selectedDebugIndex = debugEntries.findIndex((entry) => entry.turn === selectedDebugTurn);
  const selectedDebugEntry =
    selectedDebugIndex >= 0 ? debugEntries[selectedDebugIndex] : null;
  const selectedTrace =
    selectedDebugEntry && typeof selectedDebugEntry.trace === "object" && selectedDebugEntry.trace !== null
      ? (selectedDebugEntry.trace as Record<string, unknown>)
      : null;
  const selectedTracePipelineEvents = Array.isArray(selectedTrace?.pipelineEvents)
    ? (selectedTrace.pipelineEvents as PipelineStepEvent[])
    : [];
  const selectedPipelineEvents =
    stepExecution && selectedDebugTurn === stepExecution.turn && stepPipelineEvents.length > 0
      ? stepPipelineEvents
      : selectedTracePipelineEvents;
  const debugTurnOptions = [
    ...new Set([
      ...debugEntries.map((entry) => entry.turn),
      ...(stepExecution ? [stepExecution.turn] : []),
    ]),
  ].sort((a, b) => a - b);

  const stepDebugTurn = (direction: "up" | "down") => {
    if (selectedDebugTurn === null) return;
    const selectedIndex = debugTurnOptions.findIndex((value) => value === selectedDebugTurn);
    if (selectedIndex < 0) return;
    if (direction === "up" && selectedIndex > 0) {
      setSelectedDebugTurn(debugTurnOptions[selectedIndex - 1]);
      return;
    }
    if (direction === "down" && selectedIndex < debugTurnOptions.length - 1) {
      setSelectedDebugTurn(debugTurnOptions[selectedIndex + 1]);
    }
  };

  const buildModuleDebugContext = () => ({
    requestId: `frontend-${Date.now()}`,
    runId: moduleDebugRunId.trim() || runId || "debug-session",
    gameProjectId: currentGameProjectId,
    turn: Number.isInteger(moduleDebugTurn) && moduleDebugTurn > 0 ? moduleDebugTurn : 1,
    playerId: moduleDebugPlayerId.trim() || DEFAULT_PLAYER_ID,
    playerInput: moduleDebugInput,
  });

  const parseProposalForDebug = () => {
    try {
      return JSON.parse(moduleDebugProposalJson) as unknown;
    } catch {
      throw new Error("Proposal JSON is invalid.");
    }
  };

  const renderConversationPayload = (moduleName: string, payload: unknown) => {
    const convo = asConversationTrace(payload);
    const renderCollapsibleText = (summary: string, text: string) => (
      <details className="jsonCollapse">
        <summary>{summary}</summary>
        <pre>{text}</pre>
      </details>
    );
    return (
      <section key={moduleName} className="conversationModule">
        <header className="conversationModuleHeader">
          <strong>{toTitleCase(moduleName)}</strong>
          <span>
            Attempts: {convo?.attempts?.length ?? 0}
            {convo?.usedFallback ? " | Used fallback" : " | No fallback"}
          </span>
        </header>
        {convo?.usedFallback ? (
          <p className="conversationFallback">Fallback reason: {convo.fallbackReason ?? "unknown"}</p>
        ) : null}
        {(convo?.attempts ?? []).map((attempt, idx) => (
          <details key={`${moduleName}-attempt-${idx}`} className="conversationAttempt">
            <summary>
              Attempt {attempt.attempt ?? idx + 1}
              {attempt.error ? " - failed" : " - ok"}
            </summary>
            <div className="conversationAttemptBody">
              {(attempt.requestMessages ?? []).map((message, messageIdx) => (
                <div key={`${moduleName}-attempt-${idx}-msg-${messageIdx}`} className="conversationMessage">
                  <p className="conversationRole">{message.role ?? "unknown"}</p>
                  {renderCollapsibleText("Show message content", message.content ?? "")}
                </div>
              ))}
              {attempt.rawResponse ? (
                <div className="conversationResponse">
                  <p className="conversationRole">raw response</p>
                  {renderCollapsibleJson("Show raw response", attempt.rawResponse)}
                </div>
              ) : null}
              {attempt.error ? (
                <div className="conversationError">
                  <p className="conversationRole">error</p>
                  {renderCollapsibleText("Show error", attempt.error)}
                </div>
              ) : null}
            </div>
          </details>
        ))}
      </section>
    );
  };

  const runModuleDebug = async () => {
    setModuleDebugLoading(true);
    setModuleDebugError(null);
    try {
      const context = buildModuleDebugContext();
      const intentResponse = (await invokeIntentModule({ context })) as {
        output?: unknown;
      };
      const intent = intentResponse.output;
      if (!intent) throw new Error("Intent module did not return output.");

      if (moduleDebugTarget === "intent") {
        setModuleDebugResult(intentResponse);
        return;
      }

      const retrievalResponse = (await invokeLoreRetrieveModule({
        context,
        intent,
      })) as { output?: unknown };
      const lore = retrievalResponse.output;
      if (!lore) throw new Error("Lore retrieval module did not return output.");

      const preResponse = (await invokeLorePreModule({
        context,
        intent,
        lore,
      })) as { output?: unknown };
      const loremasterPre = preResponse.output;
      if (!loremasterPre) throw new Error("Loremaster pre module did not return output.");

      if (moduleDebugTarget === "loremaster") {
        if (moduleDebugLoremasterEndpoint === "retrieve") {
          setModuleDebugResult(retrievalResponse);
          return;
        }
        if (moduleDebugLoremasterEndpoint === "pre") {
          setModuleDebugResult(preResponse);
          return;
        }
        const postResponse = (await invokeLorePostModule({
          context,
          intent,
          lore,
          proposal: parseProposalForDebug(),
        })) as { output?: unknown };
        setModuleDebugResult(postResponse);
        return;
      }

      const simulatorResponse = (await invokeSimulatorModule({
        context,
        intent,
        lore,
        loremasterPre,
      })) as { output?: unknown };
      const simulatorProposal = simulatorResponse.output;
      if (!simulatorProposal) throw new Error("Simulator module did not return output.");

      if (moduleDebugTarget === "simulator") {
        setModuleDebugResult(simulatorResponse);
        return;
      }

      const postForArbiter = (await invokeLorePostModule({
        context,
        intent,
        lore,
        proposal: parseProposalForDebug(),
      })) as { output?: unknown };
      const lorePostForArbiter = postForArbiter.output;
      if (!lorePostForArbiter) throw new Error("Loremaster post module did not return output.");

      const arbiterResponse = (await invokeArbiterModule({
        context,
        intent,
        lore,
        loremasterPre,
        proposal: simulatorProposal,
        lorePost: lorePostForArbiter,
      })) as { output?: unknown };
      if (moduleDebugTarget === "arbiter") {
        setModuleDebugResult(arbiterResponse);
        return;
      }

      const proposal = parseProposalForDebug();
      const postResponse = (await invokeLorePostModule({
        context,
        intent,
        lore,
        proposal,
      })) as { output?: unknown };
      const lorePost = postResponse.output;
      if (!lorePost) throw new Error("Loremaster post module did not return output.");

      const proposalForProser = proposal as { operations?: unknown };
      const committed = {
        turn: context.turn,
        operations: Array.isArray(proposalForProser.operations) ? proposalForProser.operations : [],
        summary: "Manual committed diff for module debug",
      };

      const proserResponse = await invokeProserModule({
        context,
        committed,
        lore,
        lorePost,
      });
      setModuleDebugResult(proserResponse);
    } catch (error) {
      setModuleDebugError(error instanceof Error ? error.message : String(error));
    } finally {
      setModuleDebugLoading(false);
    }
  };

  const startStepMode = async () => {
    if (!runId || stepLoading) return;
    setStepLoading(true);
    setErrorText(null);
    try {
      const response = await startTurnStep({
        runId,
        turn,
        playerInput: stepPlayerInput,
        playerId: DEFAULT_PLAYER_ID,
      });
      setStepExecution(response.execution);
      setStepPipelineEvents(response.pipelineEvents);
      setSelectedDebugTurn(response.turn);
      await syncSessionFromState(runId);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setErrorText(message);
    } finally {
      setStepLoading(false);
    }
  };

  const nextStepMode = async () => {
    if (!runId || !stepExecution || stepLoading) return;
    setStepLoading(true);
    setErrorText(null);
    try {
      const response = await nextTurnStep({
        runId,
        turn: stepExecution.turn,
      });
      setStepExecution(response.execution);
      setStepPipelineEvents(response.pipelineEvents);
      if (response.execution.completed) {
        await syncSessionFromState(runId);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setErrorText(message);
    } finally {
      setStepLoading(false);
    }
  };

  const runStepModeToEnd = async () => {
    if (!runId || !stepExecution || stepExecution.completed || stepLoading) return;
    setStepLoading(true);
    setErrorText(null);
    try {
      let latestExecution = stepExecution;
      let guard = 0;
      while (!latestExecution.completed && guard < 16) {
        const response = await nextTurnStep({
          runId,
          turn: latestExecution.turn,
        });
        latestExecution = response.execution;
        setStepExecution(response.execution);
        setStepPipelineEvents(response.pipelineEvents);
        guard += 1;
      }
      if (latestExecution.completed) {
        await syncSessionFromState(runId);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setErrorText(message);
    } finally {
      setStepLoading(false);
    }
  };

  const applyStateToView = (state: Awaited<ReturnType<typeof fetchRunState>>) => {
    const previousLatestTurn =
      debugEntries.length > 0 ? debugEntries[debugEntries.length - 1].turn : null;
    const chatFromState: ChatLine[] = state.messages.flatMap((entry) => [
      { speaker: "you", text: entry.playerText },
      { speaker: "engine", text: entry.engineText },
    ]);
    setMessages(chatFromState);
    setTurn(state.nextTurn);

    const normalizedDebugEntries = state.debugEntries.map((entry) => ({
      turn: entry.turn,
      trace: entry.trace,
    }));
    setDebugEntries(normalizedDebugEntries);

    if (normalizedDebugEntries.length === 0) {
      setSelectedDebugTurn(null);
      return;
    }

    const latestTurn = normalizedDebugEntries[normalizedDebugEntries.length - 1].turn;
    setSelectedDebugTurn((prev) => {
      if (prev === null) return latestTurn;
      if (!normalizedDebugEntries.some((entry) => entry.turn === prev)) return latestTurn;
      const userWasOnLatest = previousLatestTurn !== null && prev === previousLatestTurn;
      if (userWasOnLatest && latestTurn > previousLatestTurn) return latestTurn;
      return prev;
    });
  };

  const syncSessionFromState = async (activeRunId: string) => {
    const state = await fetchRunState(activeRunId);
    applyStateToView(state);
  };

  const startNewGame = async () => {
    if (isLoading) return;
    setIsLoading(true);
    setErrorText(null);
    try {
      const run = await startRun();
      setCurrentGameProjectId(run.gameProject);
      setRunId(run.runId);
      await syncSessionFromState(run.runId);
      const sessionResponse = await fetchSessions(run.gameProject);
      setSessions(sessionResponse.sessions);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setErrorText(message);
    } finally {
      setIsLoading(false);
    }
  };

  useEffect(() => {
    fetchSessions(currentGameProjectId)
      .then((response) => {
        setSessions(response.sessions);
      })
      .catch((error) => {
        const message = error instanceof Error ? error.message : String(error);
        setErrorText((prev) => prev ?? message);
      });
  }, [currentGameProjectId]);

  useEffect(() => {
    if (!runId) return;

    const timer = setInterval(() => {
      if (isLoading) return;
      fetchRunState(runId)
        .then((state) => {
          applyStateToView(state);
        })
        .catch((error) => {
          const message = error instanceof Error ? error.message : String(error);
          setErrorText((prev) => prev ?? message);
        });
    }, STATE_POLL_INTERVAL_MS);

    return () => clearInterval(timer);
  }, [runId, isLoading]);

  useEffect(() => {
    if (!runId || selectedDebugTurn === null) return;
    fetchTurnPipeline(runId, selectedDebugTurn)
      .then((pipelineState) => {
        if (pipelineState.execution) {
          setStepExecution((prev) => {
            if (prev && prev.turn !== pipelineState.execution?.turn) return prev;
            return pipelineState.execution;
          });
        }
        if (pipelineState.turn === selectedDebugTurn) {
          setStepPipelineEvents(pipelineState.events);
        }
      })
      .catch(() => {
        // Ignore read failures for turns that do not have pipeline state yet.
      });
  }, [runId, selectedDebugTurn]);

  return (
    <main className="layout">
      <header className="topbar">
        <h1>Morpheus Engine</h1>
        <div className="topbarControls">
          <select
            value={runId ?? ""}
            disabled={isLoading}
            onChange={async (e) => {
              const selectedSessionId = e.target.value;
              if (!selectedSessionId) return;
              setIsLoading(true);
              setErrorText(null);
              try {
                setRunId(selectedSessionId);
                await syncSessionFromState(selectedSessionId);
              } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                setErrorText(message);
              } finally {
                setIsLoading(false);
              }
            }}
          >
            {!runId ? (
              <option value="" disabled>
                Select session
              </option>
            ) : null}
            {runId && !hasActiveSessionInList ? (
              <option value={runId}>
                {runId.slice(0, 8)} - active session
              </option>
            ) : null}
            {sessions.map((session) => (
              <option key={session.sessionId} value={session.sessionId}>
                {session.sessionId.slice(0, 8)} - {new Date(session.createdAt).toLocaleString()}
              </option>
            ))}
          </select>
          <button
            disabled={isLoading || !runId}
            onClick={async () => {
              if (!runId) return;
              setErrorText(null);
              try {
                await openSessionSavedFolder(runId);
              } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                setErrorText(message);
              }
            }}
          >
            Open Session Folder
          </button>
          <button onClick={() => void startNewGame()} disabled={isLoading}>
            {isLoading ? "Loading..." : "New Game"}
          </button>
        </div>
      </header>

      <section className="tabBar">
        <button
          type="button"
          className={activeTab === "game" ? "tabButton tabButtonActive" : "tabButton"}
          onClick={() => setActiveTab("game")}
        >
          Game
        </button>
        <button
          type="button"
          className={activeTab === "debug" ? "tabButton tabButtonActive" : "tabButton"}
          onClick={() => setActiveTab("debug")}
        >
          Debug
        </button>
        <button
          type="button"
          className={activeTab === "module_debug" ? "tabButton tabButtonActive" : "tabButton"}
          onClick={() => setActiveTab("module_debug")}
        >
          Module Debug
        </button>
      </section>

      {activeTab === "game" ? (
        <section className="splitPanels">
          <article className="panel">
            <h2>Game UI</h2>
            <p className="metaLine">
              Run: {runId ?? "not started"} | Turn: {turn}
            </p>
            <>
              <div className="log">
                {messages.length === 0 ? <p className="metaLine">No turn data yet.</p> : null}
                {messages.map((line, idx) => (
                  <p key={`${idx}-${line.speaker}-${line.text}`}>
                    {line.speaker === "you" ? "You" : line.speaker === "engine" ? "Engine" : "System"}:{" "}
                    {line.speaker === "you" && line.text.trim().length === 0 ? "(wait / pass turn)" : line.text}
                  </p>
                ))}
              </div>
              {errorText ? <p className="statusError">Error: {errorText}</p> : null}
              <form
                className="chatInput"
                onSubmit={async (e) => {
                  e.preventDefault();
                  if (!input.length || isLoading) return;

                  const playerInput = input;
                  setInput("");
                  setErrorText(null);
                  setIsLoading(true);

                  try {
                    let activeRunId = runId;
                    if (!activeRunId) {
                      const run = await startRun();
                      setCurrentGameProjectId(run.gameProject);
                      activeRunId = run.runId;
                      setRunId(run.runId);
                      await syncSessionFromState(activeRunId);
                      const sessionResponse = await fetchSessions(run.gameProject);
                      setSessions(sessionResponse.sessions);
                    }

                    await submitTurn({
                      runId: activeRunId,
                      turn,
                      playerInput,
                      playerId: DEFAULT_PLAYER_ID,
                    });
                    await syncSessionFromState(activeRunId);
                  } catch (error) {
                    const message = error instanceof Error ? error.message : String(error);
                    setErrorText(message);
                  } finally {
                    setIsLoading(false);
                  }
                }}
              >
                <input
                  value={input}
                  onChange={(e) => setInput(e.target.value)}
                  placeholder="Describe your action (or send spaces to pass turn)..."
                  disabled={isLoading}
                />
                <button type="submit" disabled={isLoading}>
                  {isLoading ? "Thinking..." : "Send"}
                </button>
              </form>
            </>
          </article>
        </section>
      ) : null}

      {activeTab === "debug" ? (
        <section className="splitPanels">
          <article className="panel">
            <h2>Debug UI</h2>
            <div className="moduleDebugActions">
              <input
                value={stepPlayerInput}
                onChange={(e) => setStepPlayerInput(e.target.value)}
                placeholder="Player input for stepped execution"
                disabled={stepLoading || !runId}
              />
              <button
                type="button"
                onClick={() => void startStepMode()}
                disabled={stepLoading || !runId}
                title="Begin a new stepped turn"
              >
                {stepLoading ? "Working..." : "Start Step Mode"}
              </button>
              <button
                type="button"
                onClick={() => void nextStepMode()}
                disabled={stepLoading || !stepExecution || stepExecution.completed}
                title="Advance one pipeline stage"
              >
                Next Step
              </button>
              <button
                type="button"
                onClick={() => void runStepModeToEnd()}
                disabled={stepLoading || !stepExecution || stepExecution.completed}
                title="Advance all remaining stages"
              >
                Run To End
              </button>
            </div>
            {stepExecution ? (
              <p className="metaLine">
                Step Mode Turn: {stepExecution.turn} | Cursor: {stepExecution.cursor} | Status:{" "}
                {stepExecution.completed ? "completed" : "waiting for next step"}
              </p>
            ) : null}
            {debugTurnOptions.length > 0 ? (
              <div className="debugControls">
                <label htmlFor="debug-turn-select">Turn:</label>
                <button
                  type="button"
                  onClick={() => stepDebugTurn("up")}
                  disabled={
                    selectedDebugTurn === null ||
                    debugTurnOptions.findIndex((value) => value === selectedDebugTurn) <= 0
                  }
                  title="Previous turn"
                >
                  ↑
                </button>
                <select
                  id="debug-turn-select"
                  value={selectedDebugTurn ?? ""}
                  onChange={(e) => setSelectedDebugTurn(Number(e.target.value))}
                >
                  {debugTurnOptions.map((turnOption) => (
                    <option key={turnOption} value={turnOption}>
                      {turnOption}
                    </option>
                  ))}
                </select>
                <button
                  type="button"
                  onClick={() => stepDebugTurn("down")}
                  disabled={
                    selectedDebugTurn === null ||
                    debugTurnOptions.findIndex((value) => value === selectedDebugTurn) < 0 ||
                    debugTurnOptions.findIndex((value) => value === selectedDebugTurn) >=
                      debugTurnOptions.length - 1
                  }
                  title="Next turn"
                >
                  ↓
                </button>
              </div>
            ) : null}
            {debugTurnOptions.length === 0 ? <p className="metaLine">No debug entries yet.</p> : null}
            {selectedPipelineEvents.length > 0 ? (
              <section className="debugCard debugCardWide">
                <h3>Pipeline Events</h3>
                <div className="conversationList">
                  {selectedPipelineEvents.map((event) => (
                    <details key={`pipeline-step-${event.stepNumber}`} className="conversationAttempt">
                      <summary>
                        {event.stepNumber}. {prettyStageName(event.stage)} ({event.endpoint}) - {event.status}
                      </summary>
                      <div className="conversationAttemptBody">
                        {renderCollapsibleJson("Show request JSON", event.request)}
                        {renderCollapsibleJson("Show response JSON", event.response ?? null)}
                        {renderCollapsibleJson("Show warnings", event.warnings ?? [])}
                      </div>
                    </details>
                  ))}
                </div>
              </section>
            ) : null}
            {selectedDebugEntry && selectedTrace ? (
              <div className="debugStructured">
                <section className="debugCard">
                  <h3>Intent</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.intent ?? null)}
                </section>
                <section className="debugCard">
                  <h3>Loremaster</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.loremaster ?? null)}
                </section>
                <section className="debugCard">
                  <h3>Proposal</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.proposal ?? null)}
                </section>
                <section className="debugCard">
                  <h3>Committed</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.committed ?? null)}
                </section>
                <section className="debugCard">
                  <h3>Warnings</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.warnings ?? [])}
                </section>
                <section className="debugCard">
                  <h3>Refusal</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.refusal ?? null)}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Narration Text</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace.narrationText ?? "")}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Raw Trace</h3>
                  {renderCollapsibleJson("Show JSON", selectedTrace)}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Model Conversations</h3>
                  {Object.entries(
                    (selectedTrace.llmConversations as Record<string, unknown> | undefined) ?? {},
                  ).length === 0 ? (
                    <p className="metaLine">No model conversations captured for this turn.</p>
                  ) : (
                    <div className="conversationList">
                      {Object.entries(
                        (selectedTrace.llmConversations as Record<string, unknown> | undefined) ?? {},
                      ).map(([moduleName, payload]) => renderConversationPayload(moduleName, payload))}
                    </div>
                  )}
                </section>
              </div>
            ) : null}
          </article>
        </section>
      ) : null}

      {activeTab === "module_debug" ? (
        <section className="splitPanels">
          <article className="panel">
            <h2>Module Debug UI</h2>
            <p className="metaLine">Call modules directly without running the full backend turn pipeline.</p>
            <div className="moduleDebugGrid">
              <label>
                Module
                <select
                  value={moduleDebugTarget}
                  onChange={(e) => setModuleDebugTarget(e.target.value as ModuleDebugTarget)}
                >
                  <option value="intent">Intent Extractor</option>
                  <option value="loremaster">Loremaster</option>
                  <option value="simulator">Default Simulator</option>
                  <option value="arbiter">Arbiter</option>
                  <option value="proser">Proser</option>
                </select>
              </label>
              {moduleDebugTarget === "loremaster" ? (
                <label>
                  Loremaster Endpoint
                  <select
                    value={moduleDebugLoremasterEndpoint}
                    onChange={(e) =>
                      setModuleDebugLoremasterEndpoint(e.target.value as LoremasterEndpoint)
                    }
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
                  value={currentGameProjectId}
                  onChange={(e) => setCurrentGameProjectId(e.target.value)}
                  disabled={moduleDebugLoading}
                />
              </label>
              <label>
                Run ID
                <input
                  value={moduleDebugRunId}
                  onChange={(e) => setModuleDebugRunId(e.target.value)}
                  disabled={moduleDebugLoading}
                />
              </label>
              <label>
                Turn
                <input
                  type="number"
                  value={moduleDebugTurn}
                  onChange={(e) => setModuleDebugTurn(Number(e.target.value))}
                  disabled={moduleDebugLoading}
                />
              </label>
              <label>
                Player ID
                <input
                  value={moduleDebugPlayerId}
                  onChange={(e) => setModuleDebugPlayerId(e.target.value)}
                  disabled={moduleDebugLoading}
                />
              </label>
            </div>
            <label className="moduleDebugLabel">
              Player Input
              <textarea
                value={moduleDebugInput}
                onChange={(e) => setModuleDebugInput(e.target.value)}
                rows={4}
                disabled={moduleDebugLoading}
              />
            </label>
            <label className="moduleDebugLabel">
              Proposal JSON (used for Loremaster `/post` and Proser)
              <textarea
                value={moduleDebugProposalJson}
                onChange={(e) => setModuleDebugProposalJson(e.target.value)}
                rows={8}
                disabled={moduleDebugLoading}
              />
            </label>
            <div className="moduleDebugActions">
              <button type="button" onClick={() => void runModuleDebug()} disabled={moduleDebugLoading}>
                {moduleDebugLoading ? "Running..." : "Run Module"}
              </button>
            </div>
            {moduleDebugError ? <p className="statusError">Error: {moduleDebugError}</p> : null}
            {moduleDebugResult && typeof moduleDebugResult === "object" ? (
              <div className="debugStructured">
                <section className="debugCard">
                  <h3>Meta</h3>
                  {renderCollapsibleJson("Show JSON", (moduleDebugResult as Record<string, unknown>).meta ?? null)}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Output</h3>
                  {renderCollapsibleJson("Show JSON", (moduleDebugResult as Record<string, unknown>).output ?? null)}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Module Conversation</h3>
                  {((moduleDebugResult as Record<string, unknown>).debug as Record<string, unknown> | undefined)
                    ?.llmConversation ? (
                    <div className="conversationList">
                      {renderConversationPayload(
                        moduleDebugTarget === "loremaster"
                          ? `loremaster_${moduleDebugLoremasterEndpoint}`
                          : moduleDebugTarget,
                        ((moduleDebugResult as Record<string, unknown>).debug as Record<string, unknown>)
                          .llmConversation,
                      )}
                    </div>
                  ) : (
                    <p className="metaLine">No `llmConversation` in this module response.</p>
                  )}
                </section>
                <section className="debugCard debugCardWide">
                  <h3>Raw Module Response</h3>
                  {renderCollapsibleJson("Show JSON", moduleDebugResult)}
                </section>
              </div>
            ) : (
              <section className="debugCard debugCardWide">
                <h3>Module Output</h3>
                {renderCollapsibleJson(
                  "Show JSON",
                  moduleDebugResult ?? "No module output yet.",
                )}
              </section>
            )}
          </article>
        </section>
      ) : null}
    </main>
  );
}
