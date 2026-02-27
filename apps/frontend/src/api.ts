const API_BASE_URL = import.meta.env.VITE_API_BASE_URL ?? "http://localhost:8787";
const MODULE_INTENT_URL = import.meta.env.VITE_MODULE_INTENT_URL ?? "http://localhost:8791";
const MODULE_LOREMASTER_URL = import.meta.env.VITE_MODULE_LOREMASTER_URL ?? "http://localhost:8792";
const MODULE_SIMULATOR_URL =
  import.meta.env.VITE_MODULE_DEFAULT_SIMULATOR_URL ?? "http://localhost:8793";
const MODULE_ARBITER_URL = import.meta.env.VITE_MODULE_ARBITER_URL ?? "http://localhost:8795";
const MODULE_PROSER_URL = import.meta.env.VITE_MODULE_PROSER_URL ?? "http://localhost:8794";

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

export interface RunStateResponse {
  runId: string;
  gameProjectId: string;
  messages: Array<{
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

export interface PipelineStepEvent {
  stepNumber: number;
  stage: string;
  endpoint: string;
  status: "ok" | "error" | "skipped";
  request: unknown;
  response?: unknown;
  warnings: string[];
  error?: string;
  startedAt: string;
  finishedAt: string;
}

export interface TurnExecutionState {
  runId: string;
  turn: number;
  mode: "normal" | "step";
  cursor: number;
  completed: boolean;
  createdAt: string;
  updatedAt: string;
  playerInput: string;
  playerId: string;
  requestId: string;
  gameProjectId: string;
  result: {
    narrationText?: string;
    warnings: string[];
  };
}

export interface PipelineStateResponse {
  runId: string;
  turn: number;
  execution: TurnExecutionState | null;
  events: PipelineStepEvent[];
}

async function requestJson<T>(path: string, init?: RequestInit): Promise<T> {
  return requestJsonAbsolute<T>(`${API_BASE_URL}${path}`, init);
}

async function requestJsonAbsolute<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, {
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

export async function fetchRunState(runId: string): Promise<RunStateResponse> {
  return requestJson<RunStateResponse>(`/run/${encodeURIComponent(runId)}/state`);
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

export async function startTurnStep(payload: TurnRequest): Promise<{
  runId: string;
  turn: number;
  execution: TurnExecutionState;
  pipelineEvents: PipelineStepEvent[];
}> {
  return requestJson("/turn/step/start", {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function nextTurnStep(payload: { runId: string; turn: number }): Promise<{
  runId: string;
  turn: number;
  execution: TurnExecutionState;
  pipelineEvents: PipelineStepEvent[];
  result: unknown;
}> {
  return requestJson("/turn/step/next", {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function fetchTurnPipeline(runId: string, turn: number): Promise<PipelineStateResponse> {
  return requestJson(`/run/${encodeURIComponent(runId)}/turn/${turn}/pipeline`);
}

export async function invokeIntentModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_INTENT_URL}/invoke`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeLoreRetrieveModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_LOREMASTER_URL}/retrieve`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeLorePreModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_LOREMASTER_URL}/pre`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeSimulatorModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_SIMULATOR_URL}/invoke`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeLorePostModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_LOREMASTER_URL}/post`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeProserModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_PROSER_URL}/invoke`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}

export async function invokeArbiterModule(payload: unknown): Promise<unknown> {
  return requestJsonAbsolute<unknown>(`${MODULE_ARBITER_URL}/invoke`, {
    method: "POST",
    body: JSON.stringify(payload),
  });
}
