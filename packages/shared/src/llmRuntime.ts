import fs from "node:fs";
import path from "node:path";
import dotenv from "dotenv";

export type ChatMessage = { role: "system" | "user" | "assistant"; content: string };

export type ConversationTrace = {
  attempts: Array<{
    attempt: number;
    requestMessages: ChatMessage[];
    rawResponse?: string;
    error?: string;
  }>;
  usedFallback: boolean;
  fallbackReason?: string;
};

export function loadBackendEnv(moduleDir: string): void {
  for (const candidate of [
    path.resolve(moduleDir, "..", "..", "backend", ".env"),
    path.resolve(process.cwd(), "apps", "backend", ".env"),
    path.resolve(process.cwd(), ".env"),
    path.resolve(moduleDir, "..", ".env"),
  ]) {
    if (fs.existsSync(candidate)) {
      dotenv.config({ path: candidate, override: true });
    }
  }
}

export function getProviderName(): "ollama" | "stub" {
  return (process.env.LLM_PROVIDER ?? "stub").toLowerCase() === "ollama" ? "ollama" : "stub";
}

export async function chatWithProvider(messages: ChatMessage[]): Promise<string> {
  if (getProviderName() === "stub") {
    const lastUser = [...messages].reverse().find((item) => item.role === "user");
    return `Stub response to: ${lastUser?.content ?? "empty input"}`;
  }

  const baseUrl = process.env.OLLAMA_BASE_URL ?? "http://localhost:11434";
  const model = process.env.OLLAMA_MODEL ?? "qwen2.5:7b-instruct";
  const timeoutMs = Number(process.env.OLLAMA_TIMEOUT_MS ?? "15000");
  const response = await fetch(`${baseUrl.replace(/\/$/, "")}/api/chat`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ model, messages, stream: false }),
    signal: AbortSignal.timeout(Number.isFinite(timeoutMs) && timeoutMs > 0 ? timeoutMs : 15000),
  });
  if (!response.ok) throw new Error(`Ollama HTTP ${response.status}: ${await response.text()}`);
  const data = (await response.json()) as { message?: { content?: string } };
  const content = data.message?.content?.trim();
  if (!content) throw new Error("No content in Ollama response.");
  return content;
}

export function parseJsonObject(text: string): unknown {
  const trimmed = text.trim();
  const fencedMatch = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  const candidate = fencedMatch ? fencedMatch[1] : trimmed;
  try {
    return JSON.parse(candidate);
  } catch {
    const start = candidate.indexOf("{");
    if (start < 0) throw new Error("No JSON object found in model response.");
    let depth = 0;
    let inString = false;
    let escaped = false;
    for (let index = start; index < candidate.length; index += 1) {
      const char = candidate[index];
      if (inString) {
        if (escaped) escaped = false;
        else if (char === "\\") escaped = true;
        else if (char === "\"") inString = false;
        continue;
      }
      if (char === "\"") {
        inString = true;
        continue;
      }
      if (char === "{") depth += 1;
      if (char === "}") {
        depth -= 1;
        if (depth === 0) return JSON.parse(candidate.slice(start, index + 1));
      }
    }
    throw new Error("Could not isolate JSON object from model response.");
  }
}

export async function generateStructuredWithRetries<T>(params: {
  stageName: string;
  schema: { parse(input: unknown): T };
  basePrompt: string;
  fallbackValue: T;
  normalizeParsed?: (parsed: unknown) => unknown;
  maxRetries?: number;
  systemPrompt?: string;
  chat?: (messages: ChatMessage[]) => Promise<string>;
  providerName?: () => "ollama" | "stub";
}): Promise<{ output: T; warnings: string[]; conversation: ConversationTrace }> {
  const warnings: string[] = [];
  const attempts: ConversationTrace["attempts"] = [];
  const maxRetries = params.maxRetries ?? 2;
  const runChat = params.chat ?? chatWithProvider;
  const readProvider = params.providerName ?? getProviderName;
  const systemPrompt =
    params.systemPrompt ?? "Return ONLY valid JSON. Do not add markdown, comments, or extra keys unless requested.";

  if (readProvider() === "stub") {
    const reason = `${params.stageName}: structured generation skipped because LLM provider is stub.`;
    warnings.push(reason);
    return {
      output: params.fallbackValue,
      warnings,
      conversation: {
        attempts: [
          {
            attempt: 1,
            requestMessages: [{ role: "system", content: `${params.stageName} skipped: ${reason}` }],
          },
        ],
        usedFallback: true,
        fallbackReason: reason,
      },
    };
  }

  let retryHint = "";
  for (let attempt = 0; attempt <= maxRetries; attempt += 1) {
    const requestMessages: ChatMessage[] = [
      { role: "system", content: systemPrompt },
      { role: "user", content: `${params.basePrompt}${retryHint}` },
    ];
    try {
      const raw = await runChat(requestMessages);
      const parsed = parseJsonObject(raw);
      const normalized = params.normalizeParsed ? params.normalizeParsed(parsed) : parsed;
      const output = params.schema.parse(normalized);
      attempts.push({ attempt: attempt + 1, requestMessages, rawResponse: raw });
      return {
        output,
        warnings,
        conversation: { attempts, usedFallback: false },
      };
    } catch (error) {
      const errorText = error instanceof Error ? error.message : String(error);
      attempts.push({ attempt: attempt + 1, requestMessages, error: errorText });
      if (attempt >= maxRetries) {
        warnings.push(`${params.stageName}: used fallback after retries (${errorText})`);
        return {
          output: params.fallbackValue,
          warnings,
          conversation: { attempts, usedFallback: true, fallbackReason: errorText },
        };
      }
      retryHint =
        "\n\nYour previous output was invalid.\n" +
        "Fix and return ONLY a valid JSON object for this task.\n" +
        `Validation/parsing error: ${errorText}`;
    }
  }

  warnings.push(`${params.stageName}: unexpected retry loop exit, used fallback.`);
  return {
    output: params.fallbackValue,
    warnings,
    conversation: { attempts, usedFallback: true, fallbackReason: "Unexpected retry loop exit." },
  };
}
