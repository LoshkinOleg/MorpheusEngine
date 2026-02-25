import test from "node:test";
import assert from "node:assert/strict";

const OLLAMA_BASE_URL = process.env.OLLAMA_BASE_URL ?? "http://localhost:11434";
const OLLAMA_MODEL = process.env.OLLAMA_MODEL ?? "qwen2.5:7b-instruct";

test("ollama is reachable and reports models", async () => {
  const response = await fetch(`${OLLAMA_BASE_URL}/api/tags`, {
    signal: AbortSignal.timeout(5000),
  });

  assert.equal(
    response.ok,
    true,
    `Expected ${OLLAMA_BASE_URL}/api/tags to return 2xx, got ${response.status}`,
  );

  const body = await response.json();
  assert.equal(Array.isArray(body.models), true, "Expected Ollama tags response to include models[]");
});

test("qwen model is available in ollama", async () => {
  const response = await fetch(`${OLLAMA_BASE_URL}/api/tags`, {
    signal: AbortSignal.timeout(5000),
  });
  assert.equal(response.ok, true, `Could not query ${OLLAMA_BASE_URL}/api/tags`);

  const body = await response.json();
  const models = Array.isArray(body.models) ? body.models : [];
  const names = models.map((m) => m?.model).filter(Boolean);
  const hasRequestedModel = names.includes(OLLAMA_MODEL);

  assert.equal(
    hasRequestedModel,
    true,
    `Model "${OLLAMA_MODEL}" not found in Ollama. Available models: ${names.join(", ") || "(none)"}`,
  );
});
