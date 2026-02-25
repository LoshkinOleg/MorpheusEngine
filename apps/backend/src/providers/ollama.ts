import type { ChatMessage, LlmProvider } from "./llm.js";
import { LlmProviderError } from "./llm.js";

interface OllamaChatResponse {
  message?: {
    role?: string;
    content?: string;
  };
}

export interface OllamaProviderOptions {
  baseUrl: string;
  model: string;
  timeoutMs: number;
}

export class OllamaProvider implements LlmProvider {
  readonly name = "ollama";
  private readonly baseUrl: string;
  private readonly model: string;
  private readonly timeoutMs: number;

  constructor(options: OllamaProviderOptions) {
    this.baseUrl = options.baseUrl.replace(/\/$/, "");
    this.model = options.model;
    this.timeoutMs = options.timeoutMs;
  }

  async chat(messages: ChatMessage[]): Promise<string> {
    if (!messages.length) {
      throw new LlmProviderError("OllamaProvider requires at least one message.");
    }

    let response: Response;
    try {
      response = await fetch(`${this.baseUrl}/api/chat`, {
        method: "POST",
        headers: {
          "content-type": "application/json",
        },
        body: JSON.stringify({
          model: this.model,
          messages,
          stream: false,
        }),
        signal: AbortSignal.timeout(this.timeoutMs),
      });
    } catch (error) {
      throw new LlmProviderError(
        `Failed to reach Ollama at ${this.baseUrl}.`,
        error instanceof Error ? error.message : String(error),
      );
    }

    if (!response.ok) {
      const errorBody = await response.text().catch(() => "");
      throw new LlmProviderError(
        `Ollama returned HTTP ${response.status}.`,
        errorBody || response.statusText,
      );
    }

    const data = (await response.json()) as OllamaChatResponse;
    const content = data.message?.content?.trim();

    if (!content) {
      throw new LlmProviderError("Ollama response did not include assistant message content.");
    }

    return content;
  }
}
