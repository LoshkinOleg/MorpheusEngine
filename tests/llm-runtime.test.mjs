import test from "node:test";
import assert from "node:assert/strict";
import { z } from "zod";
import { generateStructuredWithRetries, parseJsonObject } from "@morpheus/shared";

test("parseJsonObject extracts fenced json payload", () => {
  const parsed = parseJsonObject('```json\n{"ok":true,"value":2}\n```');
  assert.deepEqual(parsed, { ok: true, value: 2 });
});

test("generateStructuredWithRetries retries and succeeds", async () => {
  const schema = z.object({ ok: z.boolean() });
  let calls = 0;
  const result = await generateStructuredWithRetries({
    stageName: "test_stage",
    schema,
    basePrompt: "return ok",
    fallbackValue: { ok: false },
    providerName: () => "ollama",
    chat: async () => {
      calls += 1;
      if (calls === 1) return "not json";
      return '{"ok":true}';
    },
  });
  assert.equal(result.output.ok, true);
  assert.equal(result.conversation.usedFallback, false);
  assert.equal(result.conversation.attempts.length, 2);
});

test("generateStructuredWithRetries returns fallback for stub provider", async () => {
  const schema = z.object({ ok: z.boolean() });
  const result = await generateStructuredWithRetries({
    stageName: "test_stage",
    schema,
    basePrompt: "return ok",
    fallbackValue: { ok: false },
    providerName: () => "stub",
  });
  assert.equal(result.output.ok, false);
  assert.equal(result.conversation.usedFallback, true);
  assert.equal(result.warnings.length > 0, true);
});
