import { useEffect, useState } from "react";
import { fetchRunLogs, fetchSessions, openSessionSavedFolder, startRun, submitTurn } from "./api";

type ChatLine = {
  speaker: "system" | "you" | "engine";
  text: string;
};

const DEFAULT_PLAYER_ID = "entity.player.captain";
const DEFAULT_GAME_PROJECT_ID = "sandcrawler";
const LOG_POLL_INTERVAL_MS = 1500;

export function App() {
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
  const hasActiveSessionInList = runId ? sessions.some((session) => session.sessionId === runId) : false;
  const selectedDebugIndex = debugEntries.findIndex((entry) => entry.turn === selectedDebugTurn);
  const selectedDebugEntry =
    selectedDebugIndex >= 0 ? debugEntries[selectedDebugIndex] : null;
  const selectedTrace =
    selectedDebugEntry && typeof selectedDebugEntry.trace === "object" && selectedDebugEntry.trace !== null
      ? (selectedDebugEntry.trace as Record<string, unknown>)
      : null;

  const stepDebugTurn = (direction: "up" | "down") => {
    if (selectedDebugIndex < 0) return;
    if (direction === "up" && selectedDebugIndex > 0) {
      setSelectedDebugTurn(debugEntries[selectedDebugIndex - 1].turn);
      return;
    }
    if (direction === "down" && selectedDebugIndex < debugEntries.length - 1) {
      setSelectedDebugTurn(debugEntries[selectedDebugIndex + 1].turn);
    }
  };

  const applyLogsToView = (logs: Awaited<ReturnType<typeof fetchRunLogs>>) => {
    const chatFromLogs: ChatLine[] = logs.proseEntries.flatMap((entry) => [
      { speaker: "you", text: entry.playerText },
      { speaker: "engine", text: entry.engineText },
    ]);
    setMessages(chatFromLogs);
    setTurn(logs.nextTurn);

    const normalizedDebugEntries = logs.debugEntries.map((entry) => ({
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
      return latestTurn > prev ? latestTurn : prev;
    });
  };

  const syncSessionFromLogs = async (activeRunId: string) => {
    const logs = await fetchRunLogs(activeRunId);
    applyLogsToView(logs);
  };

  const startNewGame = async () => {
    if (isLoading) return;
    setIsLoading(true);
    setErrorText(null);
    try {
      const run = await startRun();
      setCurrentGameProjectId(run.gameProject);
      setRunId(run.runId);
      await syncSessionFromLogs(run.runId);
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
      fetchRunLogs(runId)
        .then((logs) => {
          applyLogsToView(logs);
        })
        .catch((error) => {
          const message = error instanceof Error ? error.message : String(error);
          setErrorText((prev) => prev ?? message);
        });
    }, LOG_POLL_INTERVAL_MS);

    return () => clearInterval(timer);
  }, [runId, isLoading]);

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
                await syncSessionFromLogs(selectedSessionId);
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

      <section className="splitPanels">
        <article className="panel">
          <h2>Game UI</h2>
          <p className="metaLine">
            Run: {runId ?? "not started"} | Turn: {turn}
          </p>
          <>
            <div className="log">
              {messages.length === 0 ? <p className="metaLine">No prose log entries yet.</p> : null}
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
                    await syncSessionFromLogs(activeRunId);
                    const sessionResponse = await fetchSessions(run.gameProject);
                    setSessions(sessionResponse.sessions);
                  }

                  await submitTurn({
                    runId: activeRunId,
                    turn,
                    playerInput,
                    playerId: DEFAULT_PLAYER_ID,
                  });
                  await syncSessionFromLogs(activeRunId);
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
        <article className="panel">
          <h2>Debug UI</h2>
          {debugEntries.length > 0 ? (
            <div className="debugControls">
              <label htmlFor="debug-turn-select">Turn:</label>
              <button
                type="button"
                onClick={() => stepDebugTurn("up")}
                disabled={selectedDebugIndex <= 0}
                title="Previous turn"
              >
                ↑
              </button>
              <select
                id="debug-turn-select"
                value={selectedDebugTurn ?? ""}
                onChange={(e) => setSelectedDebugTurn(Number(e.target.value))}
              >
                {debugEntries.map((entry) => (
                  <option key={entry.turn} value={entry.turn}>
                    {entry.turn}
                  </option>
                ))}
              </select>
              <button
                type="button"
                onClick={() => stepDebugTurn("down")}
                disabled={selectedDebugIndex < 0 || selectedDebugIndex >= debugEntries.length - 1}
                title="Next turn"
              >
                ↓
              </button>
            </div>
          ) : null}
          {debugEntries.length === 0 ? <p className="metaLine">No debug log entries yet.</p> : null}
          {selectedDebugEntry && selectedTrace ? (
            <div className="debugStructured">
              <section className="debugCard">
                <h3>Intent</h3>
                <pre>{JSON.stringify(selectedTrace.intent ?? null, null, 2)}</pre>
              </section>
              <section className="debugCard">
                <h3>Loremaster</h3>
                <pre>{JSON.stringify(selectedTrace.loremaster ?? null, null, 2)}</pre>
              </section>
              <section className="debugCard">
                <h3>Proposal</h3>
                <pre>{JSON.stringify(selectedTrace.proposal ?? null, null, 2)}</pre>
              </section>
              <section className="debugCard">
                <h3>Committed</h3>
                <pre>{JSON.stringify(selectedTrace.committed ?? null, null, 2)}</pre>
              </section>
              <section className="debugCard">
                <h3>Warnings</h3>
                <pre>{JSON.stringify(selectedTrace.warnings ?? [], null, 2)}</pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>Narration Text</h3>
                <pre>{JSON.stringify(selectedTrace.narrationText ?? "", null, 2)}</pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>Raw Trace</h3>
                <pre>{JSON.stringify(selectedTrace, null, 2)}</pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>Intent Extractor Conversation</h3>
                <pre>
                  {JSON.stringify(
                    (selectedTrace.llmConversations as Record<string, unknown> | undefined)
                      ?.intent_extractor ?? null,
                    null,
                    2,
                  )}
                </pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>Default Simulator Conversation</h3>
                <pre>
                  {JSON.stringify(
                    (selectedTrace.llmConversations as Record<string, unknown> | undefined)
                      ?.default_simulator ?? null,
                    null,
                    2,
                  )}
                </pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>Proser Conversation</h3>
                <pre>
                  {JSON.stringify(
                    (selectedTrace.llmConversations as Record<string, unknown> | undefined)
                      ?.proser ?? null,
                    null,
                    2,
                  )}
                </pre>
              </section>
              <section className="debugCard debugCardWide">
                <h3>All Model Conversations</h3>
                <pre>{JSON.stringify(selectedTrace.llmConversations ?? null, null, 2)}</pre>
              </section>
            </div>
          ) : null}
        </article>
      </section>
    </main>
  );
}
