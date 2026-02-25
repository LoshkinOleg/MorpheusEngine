import fs from "node:fs";
import path from "node:path";

export interface GameProjectManifest {
  id: string;
  title: string;
  engineVersion: string;
  entryState: {
    playerId: string;
    anchorId: string;
  };
  moduleBindings: Record<string, string>;
}

export function loadGameProjectManifest(gameProjectId: string): GameProjectManifest {
  const manifestPath = path.resolve(
    process.cwd(),
    "..",
    "..",
    "game_projects",
    gameProjectId,
    "manifest.json",
  );

  const raw = fs.readFileSync(manifestPath, "utf8");
  return JSON.parse(raw) as GameProjectManifest;
}
