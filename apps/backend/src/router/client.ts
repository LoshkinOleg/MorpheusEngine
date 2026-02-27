import {
  ArbiterModuleResponseSchema,
  IntentModuleResponseSchema,
  LoreRetrieverModuleResponseSchema,
  LoremasterPostModuleResponseSchema,
  LoremasterPreModuleResponseSchema,
  ProserModuleResponseSchema,
  SimulatorModuleResponseSchema,
} from "@morpheus/shared";

const REQUEST_TIMEOUT_MS = Number(process.env.MODULE_REQUEST_TIMEOUT_MS ?? "20000");

async function postJson(baseUrl: string, routePath: string, payload: unknown): Promise<unknown> {
  const response = await fetch(`${baseUrl.replace(/\/$/, "")}${routePath}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(payload),
    signal: AbortSignal.timeout(REQUEST_TIMEOUT_MS),
  });
  const bodyText = await response.text();
  let parsed: unknown = {};
  try {
    parsed = bodyText ? JSON.parse(bodyText) : {};
  } catch {
    parsed = { raw: bodyText };
  }
  if (!response.ok) {
    throw new Error(`Module call failed ${baseUrl}${routePath}: HTTP ${response.status} ${JSON.stringify(parsed)}`);
  }
  return parsed;
}

export async function invokeIntent(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/invoke", payload);
  return IntentModuleResponseSchema.parse(parsed);
}

export async function invokeLoreRetrieve(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/retrieve", payload);
  return LoreRetrieverModuleResponseSchema.parse(parsed);
}

export async function invokeLorePre(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/pre", payload);
  return LoremasterPreModuleResponseSchema.parse(parsed);
}

export async function invokeLorePost(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/post", payload);
  return LoremasterPostModuleResponseSchema.parse(parsed);
}

export async function invokeSimulator(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/invoke", payload);
  return SimulatorModuleResponseSchema.parse(parsed);
}

export async function invokeArbiter(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/invoke", payload);
  return ArbiterModuleResponseSchema.parse(parsed);
}

export async function invokeProser(baseUrl: string, payload: unknown) {
  const parsed = await postJson(baseUrl, "/invoke", payload);
  return ProserModuleResponseSchema.parse(parsed);
}
