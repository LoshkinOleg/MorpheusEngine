export interface ChatMessage {
  role: "system" | "user" | "assistant";
  content: string;
}

export interface LlmProvider {
  readonly name: string;
  chat(messages: ChatMessage[]): Promise<string>;
}

export class LlmProviderError extends Error {
  readonly causeText?: string;

  constructor(message: string, causeText?: string) {
    super(message);
    this.name = "LlmProviderError";
    this.causeText = causeText;
  }
}

export class StubLlmProvider implements LlmProvider {
  readonly name = "stub";

  async chat(messages: ChatMessage[]): Promise<string> {
    const lastUserMessage = [...messages].reverse().find((m) => m.role === "user");
    return `Stub response to: ${lastUserMessage?.content ?? "empty input"}`;
  }
}
