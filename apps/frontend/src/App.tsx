import { useCallback, useEffect, useState } from "react";
import {
  fetchTurnPipeline,
  fetchRunState,
  fetchSessions,
  dumpAllTurnTraces,
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
import { DebugTab } from "./components/DebugTab";
import { GameTab } from "./components/GameTab";
import { ModuleDebugTab } from "./components/ModuleDebugTab";

type ChatLine = {
  speaker: "system" | "you" | "engine";
  text: string;
};

const DEFAULT_PLAYER_ID = "player.captain";
const DEFAULT_GAME_PROJECT_ID = "sandcrawler";

function loreRetrievalToFlavorExcerpts(lore: {
  evidence?: Array<{ source: string; excerpt: string }>;
}): Array<{ subject: string; data: string; source: string }> {
  const out: Array<{ subject: string; data: string; source: string }> = [];
  for (const item of lore.evidence ?? []) {
    if (!item.excerpt?.trim()) continue;
    const idx = item.excerpt.indexOf(": ");
    if (idx > 0) {
      out.push({
        subject: item.excerpt.slice(0, idx).trim() || "lore",
        data: item.excerpt.slice(idx + 2).trim(),
        source: item.source?.trim() || "lore",
      });
    } else {
      out.push({
        subject: "retrieved",
        data: item.excerpt.trim(),
        source: item.source?.trim() || "lore",
      });
    }
  }
  return out;
}
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

function renderCollapsibleJson(summary: string, value: unknown, options?: { open?: boolean }) {
  const normalized = normalizeJsonForDisplay(value);
  return (
    <details className="jsonCollapse" open={options?.open}>
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
  const [dumpTracesLoading, setDumpTracesLoading] = useState(false);
  const [dumpTracesError, setDumpTracesError] = useState<string | null>(null);
  const [dumpTracesResult, setDumpTracesResult] = useState<{
    outputPath: string;
    traceCount: number;
  } | null>(null);
  const hasActiveSessionInList = runId ? sessions.some((session) => session.sessionId === runId) : false;
  const selectedDebugIndex = debugEntries.findIndex((entry) => entry.turn === selectedDebugTurn);
  const selectedDebugEntry =
    selectedDebugIndex >= 0 ? debugEntries[selectedDebugIndex] : null;
  const debugTurnOptions = [
    ...new Set([
      ...debugEntries.map((entry) => entry.turn),
      ...(stepExecution ? [stepExecution.turn] : []),
    ]),
  ].sort((a, b) => a - b);

  const dumpTraces = async () => {
    if (!runId || dumpTracesLoading) return;
    setDumpTracesLoading(true);
    setDumpTracesError(null);
    setDumpTracesResult(null);
    try {
      const result = await dumpAllTurnTraces(runId);
      setDumpTracesResult({ outputPath: result.outputPath, traceCount: result.traceCount });
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      setDumpTracesError(message);
    } finally {
      setDumpTracesLoading(false);
    }
  };

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

      const flavorExcerpts = loreRetrievalToFlavorExcerpts(
        lore as { evidence?: Array<{ source: string; excerpt: string }> },
      );
      const intentForProser = intent as {
        candidates: Array<{ actorId: string; intent: string; params?: Record<string, unknown> }>;
      };
      const proserResponse = await invokeProserModule({
        context,
        committed,
        validatedIntent: { candidates: intentForProser.candidates },
        lore: {
          decisionKeys: [],
          decisionExcerpts: [],
          flavorKeys: flavorExcerpts.map((row) => row.subject),
          flavorExcerpts,
        },
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

  const applyStateToView = useCallback((state: Awaited<ReturnType<typeof fetchRunState>>) => {
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
    setDebugEntries((previousEntries) => {
      const previousLatestTurn =
        previousEntries.length > 0 ? previousEntries[previousEntries.length - 1].turn : null;

      if (normalizedDebugEntries.length === 0) {
        setSelectedDebugTurn(null);
        return normalizedDebugEntries;
      }

      const latestTurn = normalizedDebugEntries[normalizedDebugEntries.length - 1].turn;
      setSelectedDebugTurn((prev) => {
        if (prev === null) return latestTurn;
        if (!normalizedDebugEntries.some((entry) => entry.turn === prev)) return latestTurn;
        const userWasOnLatest = previousLatestTurn !== null && prev === previousLatestTurn;
        if (userWasOnLatest && latestTurn > previousLatestTurn) return latestTurn;
        return prev;
      });
      return normalizedDebugEntries;
    });
  }, []);

  const syncSessionFromState = useCallback(async (activeRunId: string) => {
    const state = await fetchRunState(activeRunId);
    applyStateToView(state);
  }, [applyStateToView]);

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
  }, [runId, isLoading, applyStateToView]);

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
        <GameTab
          runId={runId}
          turn={turn}
          messages={messages}
          errorText={errorText}
          input={input}
          isLoading={isLoading}
          onInputChange={setInput}
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
        />
      ) : null}

      {activeTab === "debug" ? (
        <DebugTab
          runId={runId}
          stepLoading={stepLoading}
          stepPlayerInput={stepPlayerInput}
          onStepPlayerInputChange={setStepPlayerInput}
          onStartStepMode={() => void startStepMode()}
          onNextStepMode={() => void nextStepMode()}
          onRunStepModeToEnd={() => void runStepModeToEnd()}
          stepExecution={stepExecution}
          debugTurnOptions={debugTurnOptions}
          selectedDebugTurn={selectedDebugTurn}
          onStepDebugTurn={stepDebugTurn}
          onSelectedDebugTurnChange={setSelectedDebugTurn}
          selectedDebugEntry={selectedDebugEntry}
          renderCollapsibleJson={renderCollapsibleJson}
          dumpTracesLoading={dumpTracesLoading}
          dumpTracesError={dumpTracesError}
          dumpTracesResult={dumpTracesResult}
          onDumpTraces={() => void dumpTraces()}
        />
      ) : null}

      {activeTab === "module_debug" ? (
        <ModuleDebugTab
          moduleDebugTarget={moduleDebugTarget}
          onModuleDebugTargetChange={setModuleDebugTarget}
          moduleDebugLoremasterEndpoint={moduleDebugLoremasterEndpoint}
          onModuleDebugLoremasterEndpointChange={setModuleDebugLoremasterEndpoint}
          currentGameProjectId={currentGameProjectId}
          onCurrentGameProjectIdChange={setCurrentGameProjectId}
          moduleDebugRunId={moduleDebugRunId}
          onModuleDebugRunIdChange={setModuleDebugRunId}
          moduleDebugTurn={moduleDebugTurn}
          onModuleDebugTurnChange={setModuleDebugTurn}
          moduleDebugPlayerId={moduleDebugPlayerId}
          onModuleDebugPlayerIdChange={setModuleDebugPlayerId}
          moduleDebugInput={moduleDebugInput}
          onModuleDebugInputChange={setModuleDebugInput}
          moduleDebugProposalJson={moduleDebugProposalJson}
          onModuleDebugProposalJsonChange={setModuleDebugProposalJson}
          moduleDebugLoading={moduleDebugLoading}
          onRunModuleDebug={() => void runModuleDebug()}
          moduleDebugError={moduleDebugError}
          moduleDebugResult={moduleDebugResult}
          renderCollapsibleJson={renderCollapsibleJson}
          renderConversationPayload={renderConversationPayload}
        />
      ) : null}
    </main>
  );
}
