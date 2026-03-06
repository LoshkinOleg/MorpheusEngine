import type { FormEvent } from "react";

type ChatLine = {
  speaker: "system" | "you" | "engine";
  text: string;
};

interface GameTabProps {
  runId: string | null;
  turn: number;
  messages: ChatLine[];
  errorText: string | null;
  input: string;
  isLoading: boolean;
  onInputChange: (next: string) => void;
  onSubmit: (e: FormEvent<HTMLFormElement>) => void;
}

export function GameTab(props: GameTabProps) {
  return (
    <section className="splitPanels">
      <article className="panel">
        <h2>Game UI</h2>
        <p className="metaLine">
          Run: {props.runId ?? "not started"} | Turn: {props.turn}
        </p>
        <>
          <div className="log">
            {props.messages.length === 0 ? <p className="metaLine">No turn data yet.</p> : null}
            {props.messages.map((line, idx) => (
              <p key={`${idx}-${line.speaker}-${line.text}`}>
                {line.speaker === "you" ? "You" : line.speaker === "engine" ? "Engine" : "System"}:{" "}
                {line.speaker === "you" && line.text.trim().length === 0 ? "(wait / pass turn)" : line.text}
              </p>
            ))}
          </div>
          {props.errorText ? <p className="statusError">Error: {props.errorText}</p> : null}
          <form className="chatInput" onSubmit={props.onSubmit}>
            <input
              value={props.input}
              onChange={(e) => props.onInputChange(e.target.value)}
              placeholder="Describe your action (or send spaces to pass turn)..."
              disabled={props.isLoading}
            />
            <button type="submit" disabled={props.isLoading}>
              {props.isLoading ? "Thinking..." : "Send"}
            </button>
          </form>
        </>
      </article>
    </section>
  );
}
