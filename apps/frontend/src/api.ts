const API_BASE_URL = import.meta.env.VITE_API_BASE_URL ?? "http://localhost:8787";

interface ApiErrorEnvelope {
  error?: {
    code?: string;
    message?: string;
    requestId?: string;
    details?: unknown;
  };
}

export interface StartRunResponse {
  runId: string;
  gameProject: string;
}

export interface SessionsResponse {
  gameProjectId: string;
  sessions: Array<{
    sessionId: string;
    createdAt: string;
  }>;
}

export interface TurnRequest {
  runId: string;
  turn: number;
  playerInput: string;
  playerId: string;
}

export interface TurnResponse {
  intent: unknown;
  loremaster?: unknown;
  proposal: unknown;
  committed: unknown;
  warnings: string[];
  narrationText: string;
}

export interface RunLogsResponse {
  runId: string;
  gameProjectId: string;
  proseEntries: Array<{
    turn: number;
    playerText: string;
    engineText: string;
  }>;
  debugEntries: Array<{
    timestamp: string;
    turn: number;
    trace: unknown;
  }>;
  nextTurn: number;
}

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API_BASE_URL}${path}`, {
    ...init,
    headers: {
      "content-type": "application/json",
      ...(init?.headers ?? {}),
    },
  });

  const text = await response.text();
  const body = text ? (JSON.parse(text) as unknown) : {};

  if (!response.ok) {
    const envelope = body as ApiErrorEnvelope;
    const message =
      envelope.error?.message ??
      `Request failed with status ${response.status}`;
    throw new Error(message);
  }

  return body as T;
}

export async function startRun(): Promise<StartRunResponse> {
  return requestJson<StartRunResponse>("/run/start", {
    method: "POST",
    body: JSON.stringify({}),
  });
}

export async function submitTurn(payload: TurnRequest): Promise<TurnResponse> {
  return requestJson<TurnResponse>("/turn", {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function fetchRunLogs(runId: string): Promise<RunLogsResponse> {
  return requestJson<RunLogsResponse>(`/run/${encodeURIComponent(runId)}/logs`);
}

export async function fetchSessions(gameProjectId: string): Promise<SessionsResponse> {
  return requestJson<SessionsResponse>(`/game_projects/${encodeURIComponent(gameProjectId)}/sessions`);
}

export async function openSessionSavedFolder(runId: string): Promise<{ ok: true; runId: string }> {
  return requestJson<{ ok: true; runId: string }>(
    `/run/${encodeURIComponent(runId)}/open-saved-folder`,
    {
      method: "POST",
      body: JSON.stringify({}),
    },
  );
}
