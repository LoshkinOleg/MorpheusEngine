import test from "node:test";
import assert from "node:assert/strict";
import { createRunLock } from "../apps/backend/dist/runLock.js";

test("run lock serializes tasks per runId", async () => {
  const withRunLock = createRunLock();
  const events = [];

  const first = withRunLock("run-a", async () => {
    events.push("first:start");
    await new Promise((resolve) => setTimeout(resolve, 25));
    events.push("first:end");
  });
  const second = withRunLock("run-a", async () => {
    events.push("second:start");
    events.push("second:end");
  });

  await Promise.all([first, second]);
  assert.deepEqual(events, ["first:start", "first:end", "second:start", "second:end"]);
});

test("run lock allows parallel tasks across different runs", async () => {
  const withRunLock = createRunLock();
  const events = [];

  await Promise.all([
    withRunLock("run-a", async () => {
      events.push("a:start");
      await new Promise((resolve) => setTimeout(resolve, 15));
      events.push("a:end");
    }),
    withRunLock("run-b", async () => {
      events.push("b:start");
      await new Promise((resolve) => setTimeout(resolve, 5));
      events.push("b:end");
    }),
  ]);

  assert.equal(events.includes("a:start"), true);
  assert.equal(events.includes("b:start"), true);
  assert.equal(events.includes("a:end"), true);
  assert.equal(events.includes("b:end"), true);
});
