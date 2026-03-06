export function createRunLock() {
  const runLockQueues = new Map<string, Array<() => void>>();

  async function acquireRunLock(runId: string): Promise<() => void> {
    const queue = runLockQueues.get(runId);
    if (!queue) {
      runLockQueues.set(runId, []);
      return () => {
        const waiters = runLockQueues.get(runId);
        if (!waiters) return;
        const next = waiters.shift();
        if (next) next();
        else runLockQueues.delete(runId);
      };
    }

    await new Promise<void>((resolve) => {
      queue.push(resolve);
    });
    return () => {
      const waiters = runLockQueues.get(runId);
      if (!waiters) return;
      const next = waiters.shift();
      if (next) next();
      else runLockQueues.delete(runId);
    };
  }

  return async function withRunLock<T>(runId: string, fn: () => Promise<T>): Promise<T> {
    const release = await acquireRunLock(runId);
    try {
      return await fn();
    } finally {
      release();
    }
  };
}
