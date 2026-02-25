import { OllamaProvider } from "./ollama.js";
import { StubLlmProvider, type LlmProvider } from "./llm.js";

function parseTimeoutMs(raw: string | undefined): number {
  const parsed = Number(raw);
  if (!Number.isFinite(parsed) || parsed <= 0) {
    return 15000;
  }
  return Math.floor(parsed);
}

export function createLlmProviderFromEnv(env = process.env): LlmProvider {
  const providerName = (env.LLM_PROVIDER ?? "stub").toLowerCase();

  if (providerName === "ollama") {
    return new OllamaProvider({
      baseUrl: env.OLLAMA_BASE_URL ?? "http://localhost:11434",
      model: env.OLLAMA_MODEL ?? "qwen2.5:7b-instruct",
      timeoutMs: parseTimeoutMs(env.OLLAMA_TIMEOUT_MS),
    });
  }

  return new StubLlmProvider();
}
