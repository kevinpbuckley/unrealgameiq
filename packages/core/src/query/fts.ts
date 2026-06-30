/**
 * Turn a free-text user query into a safe FTS5 MATCH expression.
 *
 * Raw user input can contain FTS5 operators (`"`, `*`, `:`, `(`, `NEAR`) that
 * throw on MATCH. We extract word tokens, quote each to neutralize operators,
 * and OR them for recall ("find anything about reloading"); bm25 handles ranking.
 */
export function toFtsQuery(input: string): string | null {
  const tokens = input.match(/[A-Za-z0-9_]+/g);
  if (!tokens || tokens.length === 0) return null;
  return tokens.map((t) => `"${t}"`).join(" OR ");
}
