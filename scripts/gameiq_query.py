#!/usr/bin/env python3
"""Read-only Game IQ query CLI: answers index queries WITHOUT the Unreal Editor.

The editor toolset (GameIQ.GameIQService) needs the editor open, and in a multi-agent project one
agent holds the editor at a time. This script opens <Project>/.gameiq/index.db READ-ONLY with the
Python standard library (sqlite3 ships FTS5 and JSON1) and prints the same JSON envelope the
toolset returns: {"result": ..., "index": {...freshness...}}.

    python Plugins/GameIQ/scripts/gameiq_query.py search "reload ammo" --kind blueprint
    python Plugins/GameIQ/scripts/gameiq_query.py entity asset:/Game/UI/WBP_Hud
    python Plugins/GameIQ/scripts/gameiq_query.py references asset:/Game/Audio/VO_REX_01 --direction in
    python Plugins/GameIQ/scripts/gameiq_query.py children asset:/Game/Maps/L_Main --class Light
    python Plugins/GameIQ/scripts/gameiq_query.py impact cpp:CaseSubsystem
    python Plugins/GameIQ/scripts/gameiq_query.py stats unused
    python Plugins/GameIQ/scripts/gameiq_query.py stats largest:Texture2D
    python Plugins/GameIQ/scripts/gameiq_query.py changes HEAD~3            # since a commit, a snapshot id or an ISO time
    python Plugins/GameIQ/scripts/gameiq_query.py doctor
    python Plugins/GameIQ/scripts/gameiq_query.py sql "SELECT kind, COUNT(*) FROM entities GROUP BY kind"

The project is found by walking up from the current directory to the first folder holding
.gameiq/index.db; pass --project <dir> or --db <path> to point elsewhere. Nothing is ever written.
"""

import argparse
import hashlib
import json
import math
import os
import re
import sqlite3
import subprocess
import sys
from datetime import datetime, timezone
from fnmatch import fnmatchcase
from pathlib import Path

DOC_EXTS = {".md", ".markdown", ".mdx", ".txt", ".rst", ".text", ".html", ".htm", ".rtf", ".docx", ".doc", ".pdf"}
SKIP_DIRS = {"Intermediate", "Binaries", "Saved", "DerivedDataCache", "node_modules"}
UE_TICKS_AT_UNIX_EPOCH = 621355968000000000  # FDateTime ticks (100 ns since 0001-01-01) at 1970-01-01

SEVERITY = {
    "inherits": 10, "implements": 9, "calls": 8, "binds-input": 7, "uses-skeleton": 8, "uses-material": 7,
    "uses-texture": 5, "overrides-parameter": 6, "casts-to": 6, "placed-in-level": 5, "instance-of": 5,
    "loads-by-path": 6, "plays-anim": 5, "plays-audio": 5, "plays-sequence": 5,
    "plays-on": 4, "depends-on": 4, "references": 3, "describes": 2, "constrains": 2, "illustrates": 2,
}

CPP_CAVEAT = ("C++ 'calls' edges are best-effort static extraction (same-class, Super::, qualified, and "
              "property/parameter-typed calls); dynamic dispatch, delegates, and calls through local variables "
              "are not modeled. Treat missing C++ callers as 'unknown', not 'none' — confirm with a grep of Source/.")


# ---- opening -------------------------------------------------------------------------------

def find_db(args):
    if args.db:
        return Path(args.db).resolve()
    start = Path(args.project).resolve() if args.project else Path.cwd().resolve()
    for folder in [start, *start.parents]:
        candidate = folder / ".gameiq" / "index.db"
        if candidate.is_file():
            return candidate
    sys.exit(json.dumps({"error": f"no .gameiq/index.db found at or above {start} — pass --project or --db, "
                                  "or build the index (GameIQ.Rebuild / scripts/build-index.ps1)"}))


def open_ro(path):
    con = sqlite3.connect(Path(path).resolve().as_uri() + "?mode=ro", uri=True, timeout=5)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA query_only=1")
    return con


def meta(con, key):
    row = con.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
    return row["value"] if row else None


def source_of_producer(producer):
    p = producer.split("@", 1)[0]
    p = p[len("gameiq-"):] if p.startswith("gameiq-") else p
    p = p[len("ue-"):] if p.startswith("ue-") else p
    return "blueprints" if p == "bp" else p


def by_source(con):
    out = {}
    for r in con.execute("SELECT key, value FROM meta WHERE key LIKE 'producerIngestAtIso:%' ORDER BY key"):
        out[source_of_producer(r["key"][len("producerIngestAtIso:"):])] = r["value"]
    return out


def envelope(con, payload, note=None):
    out = {"result": payload, "index": {
        "lastIngestAtIso": meta(con, "lastIngestAtIso"),
        "lastGeneratedAtIso": meta(con, "lastGeneratedAtIso"),
        "projectName": meta(con, "projectName"),
        "bySource": by_source(con),
        "servedBy": "gameiq_query.py (read-only, no editor)",
    }}
    if note:
        out["note"] = note
    return out


def parse_json(raw):
    if not raw:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return raw


def entity_json(r):
    return {
        "id": r["id"], "kind": r["kind"], "name": r["name"], "path": r["path"], "parent": r["parent"],
        "source": r["source"], "summary": r["summary"], "detail": parse_json(r["detail"]),
        "authority": r["authority"] or "extracted-fact",
    }


def entity_exists(con, eid):
    return con.execute("SELECT 1 FROM entities WHERE id = ?", (eid,)).fetchone() is not None


def has_reflection_prefix(name):
    return len(name) >= 2 and name[1].isupper() and name[0] in "AUFISTE"


def resolve_id(con, eid):
    """Exact id, else the UE-prefix alias forms for cpp ids (mirrors ResolveEntityId)."""
    if entity_exists(con, eid):
        return eid
    if not eid.startswith("cpp:"):
        return None
    body = eid[4:]
    head, sep, tail = body.partition("::")
    tail = sep + tail
    if has_reflection_prefix(head) and entity_exists(con, "cpp:" + head[1:] + tail):
        return "cpp:" + head[1:] + tail
    for p in "AUFISTE":
        if entity_exists(con, "cpp:" + p + head + tail):
            return "cpp:" + p + head + tail
    return None


def resolve_id_or_path(con, value):
    eid = resolve_id(con, value)
    if eid:
        return eid
    row = con.execute("SELECT id FROM entities WHERE path = ?1 OR id = 'asset:' || ?1 "
                      "ORDER BY CASE WHEN kind = 'blueprint' THEN 0 ELSE 1 END, id LIMIT 1", (value,)).fetchone()
    return row["id"] if row else None


def not_found(con, eid):
    name = re.split(r"[:/]", eid)[-1]
    if has_reflection_prefix(name):
        name = name[1:]
    hints = [r["id"] for r in con.execute(
        "SELECT id FROM entities WHERE name LIKE '%' || ? || '%' ORDER BY length(name) LIMIT 5", (name,))] if name else []
    payload = {"error": f"entity not found: {eid}"}
    if hints:
        payload["didYouMean"] = hints
    return envelope(con, payload, "Unknown entity id — take exact ids from search results. "
                                  "If the whole index looks empty, run doctor.")


# ---- search ----------------------------------------------------------------------------------

def word_tokens(text):
    return re.findall(r"[A-Za-z0-9_]+", text)


def fts_query(text, op, prefix=False):
    return f" {op} ".join(f'"{t}"' + ("*" if prefix else "") for t in word_tokens(text))


def split_camel(s):
    out = []
    for i, c in enumerate(s):
        if i > 0 and c.isupper() and not s[i - 1].isupper() and s[i - 1] != " ":
            out.append(" ")
        out.append(c)
    return "".join(out)


def root_folder(path):
    if not path or not path.startswith("/Game/"):
        return ""
    nxt = path.find("/", 6)
    return path if nxt < 0 else path[:nxt]


SEARCH_SQL = """
WITH hits AS (
  SELECT c.id AS chunkId, c.entity_id AS entityId, c.kind AS chunkKind,
         bm25(chunks_fts, 1.0, 0.6)
           * (CASE ce.kind WHEN 'level-actor' THEN 0.35 ELSE 1.0 END)
           * (CASE c.kind WHEN 'cpp-body' THEN 0.7 ELSE 1.0 END)
           * (CASE WHEN json_extract(ce.detail,'$.assetClass') IN
               ('Texture2D','TextureCube','Texture2DArray','VirtualTexture2D',
                'MaterialInstanceConstant','MaterialFunction') THEN 0.5 ELSE 1.0 END) AS score,
         snippet(chunks_fts, 0, '[', ']', ' ... ', 12) AS snip
  FROM chunks_fts JOIN chunks c ON c.rowid = chunks_fts.rowid
  JOIN entities ce ON ce.id = c.entity_id
  WHERE chunks_fts MATCH ?1
), best AS (
  SELECT *, ROW_NUMBER() OVER (PARTITION BY entityId ORDER BY score) AS rn FROM hits
)
SELECT b.chunkId, b.chunkKind, b.score, b.snip,
       (SELECT COUNT(*) FROM edges x WHERE x.dst = e.id) AS inRefs, e.*
FROM best b JOIN entities e ON e.id = b.entityId
WHERE b.rn = 1 AND (?2 = '' OR e.kind = ?2)
AND (?5 = '' OR e.id LIKE 'asset:' || ?5 || '%' OR e.path LIKE ?5 || '%')
ORDER BY b.score LIMIT ?3 OFFSET ?4
"""


def run_fts(con, match, kind, prefix_path, limit):
    hits = []
    for r in con.execute(SEARCH_SQL, (match, kind, limit, 0, prefix_path)):
        hits.append({"entity": entity_json(r), "score": r["score"], "snippet": r["snip"],
                     "matchedChunkKind": r["chunkKind"], "inboundRefs": r["inRefs"], "externalRefs": 0})
    for h in hits:
        if h["inboundRefs"] <= 0:
            continue
        root = root_folder(h["entity"]["path"])
        if not root:
            h["externalRefs"] = h["inboundRefs"]
            continue
        h["externalRefs"] = con.execute(
            "SELECT COUNT(*) FROM edges x LEFT JOIN entities se ON se.id = x.src WHERE x.dst = ?1 AND "
            "(se.id IS NULL OR NOT (COALESCE(se.path,'') LIKE ?2 || '%' OR se.id LIKE 'asset:' || ?2 || '%'))",
            (h["entity"]["id"], root)).fetchone()[0]
    return hits


def rerank(query, hits):
    tokens = [t.lower() for t in word_tokens(query) if len(t) >= 3]
    for h in hits:
        if tokens:
            name = h["entity"]["name"] or ""
            hay = (name + " " + split_camel(name.replace("_", " "))).lower()
            h["score"] *= 1.0 + 0.8 * sum(1 for t in tokens if t in hay) / len(tokens)
        h["score"] *= 1.0 + 0.12 * math.log2(1.0 + max(0, h["externalRefs"]))
        if h["inboundRefs"] >= 3 and h["externalRefs"] == 0:
            h["score"] *= 0.55
    hits.sort(key=lambda h: h["score"])
    return hits


def search_hits(con, query, kind="", limit=20, offset=0, path_prefix=""):
    fetch = min((offset + limit) * 3, 300)
    tiers = [fts_query(query, "AND"), fts_query(query, "OR"), fts_query(query, "OR", prefix=True)]
    if not tiers[0]:
        return []
    for match in dict.fromkeys(tiers):  # dedupe, keep order
        hits = rerank(query, run_fts(con, match, kind, path_prefix, fetch))[offset:offset + limit]
        if hits:
            return hits
    return []


def cmd_search(con, a):
    return envelope(con, search_hits(con, a.query, a.kind, a.limit, a.offset, a.path_prefix))


# ---- entity / children / references / impact -------------------------------------------------

def edges_json(con, sql, eid, cap):
    return [{"src": r["src"], "dst": r["dst"], "type": r["type"], "attrs": parse_json(r["attrs"])}
            for r in con.execute(sql, (eid, cap))]


def cmd_entity(con, a):
    eid = resolve_id(con, a.id)
    if not eid:
        return not_found(con, a.id)
    ent = entity_json(con.execute("SELECT * FROM entities WHERE id = ?", (eid,)).fetchone())
    if eid != a.id:
        ent["resolvedFrom"] = a.id
    cap = a.cap
    count = lambda sql: con.execute(sql, (eid,)).fetchone()[0]
    counts = {
        "outgoing": count("SELECT COUNT(*) FROM edges WHERE src = ?"),
        "incoming": count("SELECT COUNT(*) FROM edges WHERE dst = ?"),
        "children": count("SELECT COUNT(*) FROM entities WHERE parent = ?"),
        "chunks": count("SELECT COUNT(*) FROM chunks WHERE entity_id = ?"),
    }
    children = [entity_json(r) for r in con.execute(
        "SELECT * FROM (SELECT e.*, ROW_NUMBER() OVER (PARTITION BY COALESCE(json_extract(e.detail,'$.class'), e.kind) "
        "ORDER BY e.id) AS rr, COUNT(*) OVER (PARTITION BY COALESCE(json_extract(e.detail,'$.class'), e.kind)) AS clsN "
        "FROM entities e WHERE e.parent = ?) ORDER BY rr, clsN, id LIMIT ?", (eid, cap))]
    payload = {
        "entity": ent,
        "outgoing": edges_json(con, "SELECT * FROM edges WHERE src = ? LIMIT ?", eid, cap),
        "incoming": edges_json(con, "SELECT * FROM edges WHERE dst = ? LIMIT ?", eid, cap),
        "children": children,
        "chunks": [{"id": r["id"], "kind": r["kind"], "text": r["text"]} for r in con.execute(
            "SELECT id, kind, text FROM chunks WHERE entity_id = ? LIMIT ?", (eid, cap))],
        "counts": counts,
    }
    by_class = [{"class": r[0], "count": r[1]} for r in con.execute(
        "SELECT COALESCE(json_extract(detail,'$.class'), kind) AS class, COUNT(*) AS count FROM entities "
        "WHERE parent = ? GROUP BY class ORDER BY count DESC", (eid,))]
    if by_class:
        payload["childrenByClass"] = by_class
    payload["truncated"] = any(v > cap for v in counts.values())
    return envelope(con, payload)


def cmd_children(con, a):
    eid = resolve_id(con, a.id)
    if not eid:
        return not_found(con, a.id)
    where = ("WHERE parent = ?1 AND (?2 = '' OR COALESCE(json_extract(detail,'$.class'), kind) LIKE '%' || ?2 || '%')")
    total = con.execute("SELECT COUNT(*) FROM entities " + where, (eid, a.class_filter)).fetchone()[0]
    rows = [entity_json(r) for r in con.execute(
        "SELECT * FROM entities " + where + " ORDER BY id LIMIT ?3 OFFSET ?4", (eid, a.class_filter, a.limit, a.offset))]
    return envelope(con, {"parent": eid, "classFilter": a.class_filter, "total": total, "offset": a.offset,
                          "children": rows})


def walk(con, eid, outgoing, depth, edge_type, kind, max_rows):
    a, b = ("src", "dst") if outgoing else ("dst", "src")
    sql = f"""
    WITH RECURSIVE walk(id, via, depth) AS (
      SELECT e.{b}, e.type, 1 FROM edges e WHERE e.{a} = ?1 AND (?2 = '' OR e.type = ?2)
      UNION
      SELECT e.{b}, e.type, w.depth + 1 FROM edges e JOIN walk w ON e.{a} = w.id
      WHERE w.depth < ?3 AND (?2 = '' OR e.type = ?2)
    ), best AS (SELECT id, via, depth, ROW_NUMBER() OVER (PARTITION BY id ORDER BY depth) AS rn FROM walk)
    SELECT b.via AS viaType, b.depth AS hopDepth, e.* FROM best b JOIN entities e ON e.id = b.id
    WHERE b.rn = 1 AND b.id <> ?1 AND (?4 = '' OR e.kind = ?4) ORDER BY b.depth, e.id LIMIT ?5"""
    return [(entity_json(r), r["viaType"], r["hopDepth"]) for r in con.execute(sql, (eid, edge_type, depth, kind, max_rows))]


def cmd_references(con, a):
    eid = resolve_id(con, a.id)
    if not eid:
        return not_found(con, a.id)
    seen, rows = {eid}, []
    for direction, outgoing in (("in", False), ("out", True)):
        if a.direction not in (direction, "both"):
            continue
        for ent, via, depth in walk(con, eid, outgoing, max(1, a.depth), a.edge_type, a.kind, a.offset + a.limit):
            if ent["id"] in seen:
                continue
            seen.add(ent["id"])
            rows.append({"entity": ent, "viaType": via, "depth": depth, "direction": direction})
    rows.sort(key=lambda r: r["depth"])
    return envelope(con, rows[a.offset:a.offset + a.limit], CPP_CAVEAT if eid.startswith("cpp:") else None)


def cmd_impact(con, a):
    eid = resolve_id(con, a.id)
    if not eid:
        return not_found(con, a.id)
    rows, by_kind = [], {}
    for ent, via, depth in walk(con, eid, False, 4, "", "", 5000):
        rows.append({"entity": ent, "viaType": via, "depth": depth, "severity": SEVERITY.get(via, 1) / max(1, depth)})
        by_kind[ent["kind"]] = by_kind.get(ent["kind"], 0) + 1
    rows.sort(key=lambda r: -r["severity"])
    payload = {"impacted": rows[:a.limit], "totalDependents": len(rows),
               "byKind": dict(sorted(by_kind.items(), key=lambda kv: -kv[1])), "truncated": len(rows) > a.limit}
    if eid.startswith("cpp:"):
        payload["note"] = CPP_CAVEAT
    return envelope(con, payload)


# ---- stats -----------------------------------------------------------------------------------

def rows(con, sql, params=()):
    return [dict(r) for r in con.execute(sql, params)]


def stats_unused(con):
    candidates = rows(con, """
        SELECT e.id AS id, e.kind AS kind, e.name AS name, e.path AS path,
               COALESCE(json_extract(e.detail,'$.assetClass'),'') AS assetClass,
               json_extract(e.detail,'$.diskBytes') AS diskBytes
        FROM entities e WHERE e.kind IN ('asset','blueprint') AND NOT EXISTS (
          SELECT 1 FROM edges x LEFT JOIN entities s ON s.id = x.src
          WHERE x.dst = e.id AND x.src <> x.dst
          AND x.type NOT IN ('describes','illustrates','loads-by-path')
          AND COALESCE(s.authority,'extracted-fact') <> 'stated-intent'
          AND COALESCE(s.parent,'') <> e.id) ORDER BY e.id""")
    exact, patterns = {}, []
    for r in con.execute("SELECT src, dst, COALESCE(attrs,'') AS attrs FROM edges WHERE type = 'loads-by-path'"):
        attrs = parse_json(r["attrs"])
        via = attrs.get("via") if isinstance(attrs, dict) else None
        label = f"{via or r['dst']} ({r['src']})"
        if r["dst"].startswith("asset-pattern:"):
            patterns.append((r["dst"][len("asset-pattern:"):].lower(), label))
        else:
            exact[r["dst"]] = label
    unused, by_path, by_class = [], [], {}
    for c in candidates:
        package = (c["id"][6:] if c["id"].startswith("asset:") else c["path"]).lower()
        matched = exact.get(c["id"]) or next((lbl for pat, lbl in patterns if fnmatchcase(package, pat)), None)
        row = {k: v for k, v in c.items() if v not in (None, "")}
        if matched:
            row["matchedBy"] = matched
            by_path.append(row)
        else:
            unused.append(row)
            key = c["assetClass"] or c["kind"]
            by_class[key] = by_class.get(key, 0) + 1
    return {
        "unused": unused[:500], "possiblyLoadedByPath": by_path[:500],
        "counts": {"unused": len(unused), "possiblyLoadedByPath": len(by_path),
                   "unusedByClass": dict(sorted(by_class.items(), key=lambda kv: -kv[1]))},
        "truncated": len(unused) > 500 or len(by_path) > 500,
        "note": "'unused' = no inbound edge except self-edges, design-doc links and the asset's own child entities. "
                "'possiblyLoadedByPath' = only a C++ /Game/... string literal (or a %s format/folder literal) names it: "
                "check the code before deleting. Maps, startup assets and assets loaded through the Asset Manager or "
                "config (.ini) paths can still show as unused; confirm with references and a grep of Config/.",
    }


def stats_largest(con, class_filter):
    top, by_class, by_folder, total, n = [], {}, {}, 0, 0
    for r in con.execute(
            "SELECT id, name, path, detail, json_extract(detail,'$.diskBytes') AS diskBytes, "
            "COALESCE(json_extract(detail,'$.assetClass'), kind) AS assetClass FROM entities "
            "WHERE kind IN ('asset','blueprint') AND json_extract(detail,'$.diskBytes') IS NOT NULL "
            "AND (?1 = '' OR json_extract(detail,'$.assetClass') = ?1) ORDER BY json_extract(detail,'$.diskBytes') DESC",
            (class_filter,)):
        size = int(r["diskBytes"])
        n += 1
        total += size
        c = by_class.setdefault(r["assetClass"], [0, 0]); c[0] += 1; c[1] += size
        parts = [p for p in (r["path"] or "").split("/") if p]
        folder = "/" + "/".join(parts[:3]) if len(parts) >= 4 else "/" + "/".join(parts[:2])
        f = by_folder.setdefault(folder, [0, 0]); f[0] += 1; f[1] += size
        if len(top) < 50:
            row = {"id": r["id"], "name": r["name"], "assetClass": r["assetClass"], "diskBytes": size}
            detail = parse_json(r["detail"])
            if isinstance(detail, dict) and isinstance(detail.get("facts"), dict):
                row["facts"] = detail["facts"]
            top.append(row)
    rollup = lambda m, key: [{key: k, "count": v[0], "diskBytes": v[1]}
                             for k, v in sorted(m.items(), key=lambda kv: -kv[1][1])[:40]]
    return {"classFilter": class_filter, "largest": top, "byClass": rollup(by_class, "assetClass"),
            "byFolder": rollup(by_folder, "folder"), "assetsWithSize": n, "totalDiskBytes": total,
            "note": "Sizes are uncooked on-disk package sizes, not cooked sizes. Use 'largest:<AssetClass>' for one class."}


def cmd_stats(con, a):
    f = a.facet or "overview"
    if f == "kinds":
        return envelope(con, rows(con, "SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC"))
    if f == "edges":
        return envelope(con, rows(con, "SELECT type, COUNT(*) AS count FROM edges GROUP BY type ORDER BY count DESC"))
    if f == "unused":
        return envelope(con, stats_unused(con))
    if f == "largest" or f.startswith("largest:"):
        return envelope(con, stats_largest(con, f.partition(":")[2].strip()))
    if f == "largest-deps":
        return envelope(con, rows(con, "SELECT e.id AS id, e.name AS name, e.kind AS kind, COUNT(x.dst) AS outDeps "
                                       "FROM entities e JOIN edges x ON x.src = e.id GROUP BY e.id ORDER BY outDeps DESC LIMIT 50"))
    if f == "authority":
        return envelope(con, rows(con, "SELECT COALESCE(authority,'extracted-fact') AS authority, COUNT(*) AS count "
                                       "FROM entities GROUP BY authority ORDER BY count DESC"))
    if f == "doc-types":
        return envelope(con, rows(con, "SELECT json_extract(detail,'$.docType') AS docType, COUNT(*) AS count "
                                       "FROM entities WHERE kind='document' GROUP BY docType ORDER BY count DESC"))
    return envelope(con, {
        "totalEntities": con.execute("SELECT COUNT(*) FROM entities").fetchone()[0],
        "byKind": rows(con, "SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC"),
        "byEdgeType": rows(con, "SELECT type, COUNT(*) AS count FROM edges GROUP BY type ORDER BY count DESC"),
        "projectName": meta(con, "projectName"), "lastIngestAtIso": meta(con, "lastIngestAtIso"),
        "lastGeneratedAtIso": meta(con, "lastGeneratedAtIso"), "bySource": by_source(con),
    })


# ---- doctor ----------------------------------------------------------------------------------

def load_config(root):
    try:
        return json.loads((Path(root) / "gameiq.config.json").read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return {}


def is_excluded(name, rel, excludes):
    for e in excludes:
        if "*" in e or "?" in e:
            pats = [e]
            for suffix in ("/**", "/*"):
                if e.endswith(suffix):
                    pats.append(e[:-len(suffix)])
            if any(fnmatchcase(rel.lower(), p.lower()) or fnmatchcase(name.lower(), p.lower()) for p in pats):
                return True
            continue
        if name == e or rel == e or rel.startswith(e + "/"):
            return True
    return False


def skip_dir(full, name):
    """Dot-dirs, build output and linked git worktrees (a .git FILE pointing into worktrees/).
    A submodule's .git file points into modules/ and is kept, as in GameIQWalk::IsLinkedWorktree."""
    if name.startswith(".") or name in SKIP_DIRS:
        return True
    marker = Path(full) / ".git"
    if not marker.is_file():
        return False
    try:
        text = marker.read_text(encoding="utf-8", errors="replace").replace("\\", "/")
    except OSError:
        return False
    return text.lstrip().startswith("gitdir:") and "/worktrees/" in text


def walk_files(root, exts):
    """Mirror of GameIQWalk::WalkFiles (dot-dirs, worktrees, build output and config excludes skipped)."""
    root = Path(root)
    excludes = ["Plugins/GameIQ"] + [str(e).replace("\\", "/").rstrip() for e in load_config(root).get("exclude", [])]
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = Path(dirpath).relative_to(root).as_posix()
        rel_dir = "" if rel_dir == "." else rel_dir
        keep = []
        for d in dirnames:
            rel = f"{rel_dir}/{d}" if rel_dir else d
            if not skip_dir(Path(dirpath) / d, d) and not is_excluded(d, rel, excludes):
                keep.append(d)
        dirnames[:] = keep
        for f in filenames:
            rel = f"{rel_dir}/{f}" if rel_dir else f
            if Path(f).suffix.lower() in exts and not is_excluded(f, rel, excludes):
                out.append(rel)
    return out


def docs_fingerprint(root):
    """Mirror of GameIQDocsFingerprint::Compute: MD5 over 'relpath|size|ueTicks' lines, sorted."""
    docs_path = (load_config(root).get("docsPath") or "").strip().replace("\\", "/")
    docs_root = Path(root) / docs_path if docs_path and not Path(docs_path).is_absolute() else Path(docs_path or root)
    if not docs_root.is_dir():
        return hashlib.md5(b"").hexdigest()
    rels = [r for r in walk_files(docs_root, DOC_EXTS) if not r.startswith("Plugins/") and not r.endswith(".caption.txt")]
    rels.sort(key=str.lower)
    lines = []
    for rel in rels:
        st = (docs_root / rel).stat()
        # UE's GetStatData reports the mtime in whole seconds (measured against the C++ fingerprint).
        ticks = (st.st_mtime_ns // 1_000_000_000) * 10_000_000 + UE_TICKS_AT_UNIX_EPOCH
        lines.append(f"{rel}|{st.st_size}|{ticks}\n")
    return hashlib.md5("".join(lines).encode("ascii", "replace")).hexdigest()


def disk_packages(root):
    content = Path(root) / "Content"
    out = set()
    if not content.is_dir():
        return out
    for dirpath, _, filenames in os.walk(content):
        for f in filenames:
            if f.lower().endswith((".uasset", ".umap")):
                rel = (Path(dirpath) / f).relative_to(content).as_posix()
                if "__ExternalActors__/" in rel or "__ExternalObjects__/" in rel:
                    continue
                out.add("/Game/" + rel.rsplit(".", 1)[0])
    return out


def folder_of(package):
    parts = [p for p in package.split("/") if p]
    return "/" + "/".join(parts[:3]) if len(parts) >= 4 else ("/" + "/".join(parts[:2]) if len(parts) >= 3 else "/Game")


def cmd_doctor(con, a):
    root = Path(a.db_path).parent.parent
    problems, warnings, payload = [], [], {}
    payload["schemaVersion"] = con.execute("PRAGMA user_version").fetchone()[0]
    missing = [t for t in ("meta", "entities", "edges", "chunks", "chunks_fts")
               if not con.execute("SELECT 1 FROM sqlite_master WHERE name = ?", (t,)).fetchone()]
    if missing:
        problems.append(f"missing tables: {', '.join(missing)} — the index is broken; run GameIQ.Rebuild")
    integrity = con.execute("PRAGMA quick_check(1)").fetchone()[0]
    payload["integrity"] = integrity
    if integrity.lower() != "ok":
        problems.append(f"the index file is damaged ({integrity[:200]}) — run GameIQ.Rebuild full")
    if not missing:
        counts = {t: con.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0] for t in ("entities", "edges", "chunks")}
        payload.update(counts)
        if counts["entities"] == 0:
            problems.append("the index has zero entities — run GameIQ.Rebuild")
        payload["byProducer"] = rows(con, "SELECT COALESCE(producer,'(none)') AS producer, COUNT(*) AS count "
                                          "FROM entities GROUP BY producer ORDER BY count DESC")
        indexed = {r[0] for r in con.execute(
            "SELECT substr(id, 7) FROM entities WHERE id LIKE 'asset:/Game/%' AND parent IS NULL")}
        on_disk = disk_packages(root)
        miss = sorted(p for p in on_disk if p not in indexed)
        gone = sorted(p for p in indexed if on_disk and p not in on_disk
                      and "/__ExternalActors__/" not in p and "/__ExternalObjects__/" not in p)
        by_folder = {}
        for p in miss:
            by_folder[folder_of(p)] = by_folder.get(folder_of(p), 0) + 1
        payload["diskVsIndex"] = {
            "diskAssets": len(on_disk), "indexedAssets": len(indexed), "missingFromIndex": len(miss),
            "goneFromDisk": len(gone),
            "missingByFolder": [{"folder": k, "missing": v} for k, v in sorted(by_folder.items(), key=lambda kv: -kv[1])[:25]],
            "missingSample": miss[:20], "goneSample": gone[:20],
        }
        if miss:
            problems.append(f"{len(miss)} of {len(on_disk)} assets on disk are not in the index (see diskVsIndex.missingByFolder) "
                            "— open the editor (the startup catch-up scan adds them) or run GameIQ.Rebuild")
        if gone:
            problems.append(f"{len(gone)} indexed assets no longer exist on disk — the startup catch-up scan or GameIQ.Rebuild removes them")
    stored = root / ".gameiq" / "extract" / "docs.fingerprint"
    if stored.is_file():
        changed = docs_fingerprint(root) != stored.read_text(encoding="utf-8-sig").strip()
        payload["docsChangedSinceExtract"] = changed
        if changed:
            problems.append("design docs changed since the last docs extraction — the editor reindexes them automatically "
                            "(Auto Reindex Docs); otherwise run GameIQ.ReindexDocs")
    agent_files = []
    for name in ("CLAUDE.md", "AGENTS.md", "GEMINI.md", ".github/copilot-instructions.md"):
        p = root / name
        if p.is_file():
            text = p.read_text(encoding="utf-8", errors="replace")
            agent_files.append({"file": name, "hasGameIQGuide": "<!-- BEGIN GameIQ agent guidance -->" in text
                                or "AGENTS.md.sample" in text})
    git_repo = (root / ".git").exists()
    gitignore = root / ".gitignore"
    ignored = False
    if gitignore.is_file():
        for line in gitignore.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip().lstrip("/")
            for suffix in ("/**", "/*", "/"):
                if line.endswith(suffix):
                    line = line[:-len(suffix)]
            ignored |= line in (".gameiq", "**/.gameiq")
    payload["agentGuide"] = {"agentFiles": agent_files, "present": any(f["hasGameIQGuide"] for f in agent_files),
                             "gitRepo": git_repo, "gitignoresIndex": ignored}
    if not payload["agentGuide"]["present"]:
        warnings.append("no agent file carries the GameIQ guide block — run GameIQ.GenerateAgentConfig in the editor")
    if git_repo and not ignored:
        warnings.append(".gameiq/ is not in .gitignore — GameIQ.GenerateAgentConfig adds it")
    payload["bySource"] = by_source(con)
    payload["healthy"] = not problems
    payload["problems"] = problems
    payload["warnings"] = warnings
    return envelope(con, payload)


# ---- changes ---------------------------------------------------------------------------------

def manifests(snap_dir):
    out = []
    for p in sorted(Path(snap_dir).glob("*.json"), reverse=True):
        try:
            m = json.loads(p.read_text(encoding="utf-8-sig"))
            if "id" in m:
                out.append(m)
        except ValueError:
            pass
    return out


def parse_iso(text):
    try:
        t = datetime.fromisoformat(text.replace("Z", "+00:00"))
        return t if t.tzinfo else t.replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def git(root, *args):
    try:
        r = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, timeout=20)
        return r.stdout.strip() if r.returncode == 0 else None
    except OSError:
        return None


def newest_at_or_before(snaps, when):
    for m in snaps:
        t = parse_iso(m.get("createdAtIso", ""))
        if t and t <= when:
            return m
    return None


def resolve_state(db_path, value):
    """'current', a snapshot id, an ISO time or a git ref -> (db path, manifest)."""
    if not value or value.lower() == "current":
        return db_path, {"id": "current"}
    snap_dir = Path(db_path).parent / "snapshots"
    snaps = manifests(snap_dir)
    manifest = next((m for m in snaps if m["id"] == value), None)
    resolved = None
    if not manifest and "-" in value and ":" in value and parse_iso(value):
        manifest = newest_at_or_before(snaps, parse_iso(value))
        resolved = f"time {value}"
        if not manifest:
            raise SystemExit(json.dumps({"error": f"no snapshot exists at or before {value}"}))
    if not manifest:
        root = Path(db_path).parent.parent
        sha = git(root, "rev-parse", "--verify", "--quiet", value + "^{commit}") if not value.startswith("-") else None
        if not sha:
            raise SystemExit(json.dumps({"error": f"'{value}' is not a snapshot id, an ISO time, or a git ref git can resolve here"}))
        manifest = next((m for m in snaps if m.get("gitHead") == sha), None)
        resolved = f"git {value} ({sha[:10]})"
        if not manifest:
            ct = git(root, "show", "-s", "--format=%ct", sha)
            if ct:
                manifest = newest_at_or_before(snaps, datetime.fromtimestamp(int(ct), tz=timezone.utc))
            resolved += " — no snapshot at that commit; using the newest one taken before its commit time"
        if not manifest:
            raise SystemExit(json.dumps({"error": f"no snapshot exists at or before commit {sha[:10]} — the editor takes one "
                                                  "each time git HEAD moves (Snapshot Per Commit)"}))
    path = snap_dir / (manifest["id"] + ".db")
    if not path.is_file():
        raise SystemExit(json.dumps({"error": f"snapshot database is missing: {manifest['id']}"}))
    manifest = dict(manifest)
    if resolved:
        manifest["resolvedFrom"] = resolved
    return path, manifest


def load_state(path):
    c = open_ro(path)
    ents = {r["id"]: dict(r) for r in c.execute(
        "SELECT id, kind, name, path, COALESCE(parent,'') AS parent, source, COALESCE(summary,'') AS summary, "
        "COALESCE(detail,'') AS detail, COALESCE(authority,'extracted-fact') AS authority FROM entities")}
    edges = {(r["src"], r["dst"], r["type"]): r["attrs"] or "" for r in c.execute("SELECT src, dst, type, attrs FROM edges")}
    chunks = {r["id"]: (r["entity_id"], r["kind"], r["text"]) for r in c.execute("SELECT id, entity_id, kind, text FROM chunks")}
    ingest = c.execute("SELECT value FROM meta WHERE key='lastIngestAtIso'").fetchone()
    c.close()
    return ents, edges, chunks, ingest[0] if ingest else None


def cmd_changes(con, a):
    before_path, before_m = resolve_state(a.db_path, a.since)
    after_path, after_m = resolve_state(a.db_path, a.until)
    be, bed, bch, bi = load_state(before_path)
    ae, aed, ach, ai = load_state(after_path)
    match = lambda e: e is not None and (not a.kind or e["kind"].lower() == a.kind.lower()) and (
        not a.path_prefix or e["path"].lower().startswith(a.path_prefix.lower())
        or e["id"].lower().startswith(("asset:" + a.path_prefix).lower()))
    summary = lambda e: {k: e[k] for k in ("id", "kind", "name", "path")}
    fp = lambda e: (e["kind"], e["source"], e["summary"], e["detail"], e["authority"])
    out = []
    deleted = {i for i in be if i not in ae}
    created = {i for i in ae if i not in be}
    del_by_fp, new_by_fp = {}, {}
    for i in deleted:
        del_by_fp.setdefault(fp(be[i]), []).append(i)
    for i in created:
        new_by_fp.setdefault(fp(ae[i]), []).append(i)
    for key, olds in del_by_fp.items():
        news = new_by_fp.get(key, [])
        if len(olds) == 1 and len(news) == 1 and (match(be[olds[0]]) or match(ae[news[0]])):
            out.append({"type": "renamed", "before": summary(be[olds[0]]), "after": summary(ae[news[0]])})
            deleted.discard(olds[0]); created.discard(news[0])
    semantic = {"bp-variable": "blueprint-variable", "bp-component": "blueprint-component", "bp-function": "blueprint-function"}
    for i in sorted(deleted):
        if match(be[i]):
            out.append({"type": f"{semantic[be[i]['kind']]}-deleted" if be[i]["kind"] in semantic else "deleted", "entity": summary(be[i])})
    for i in sorted(created):
        if match(ae[i]):
            out.append({"type": f"{semantic[ae[i]['kind']]}-created" if ae[i]["kind"] in semantic else "created", "entity": summary(ae[i])})
    for i in sorted(set(be) & set(ae)):
        b, n = be[i], ae[i]
        fields = [f for f in ("kind", "name", "path", "parent", "source", "summary", "detail", "authority") if b[f] != n[f]]
        if fields and (match(b) or match(n)):
            kind = b["kind"].lower()
            t = f"{semantic[kind]}-changed" if kind in semantic else ("blueprint-structure-changed" if "blueprint" in kind else "entity-modified")
            out.append({"type": t, "before": summary(b), "after": summary(n), "fields": fields})
    def edge_ctx(src, dst):
        return match(ae.get(src) or be.get(src)) or match(ae.get(dst) or be.get(dst))
    for k in sorted(set(bed) | set(aed)):
        if k in bed and k not in aed:
            t = "relationship-removed"
        elif k in aed and k not in bed:
            t = "relationship-added"
        elif bed[k] != aed[k]:
            t = "relationship-metadata-changed"
        else:
            continue
        if edge_ctx(k[0], k[1]):
            out.append({"type": t, "src": k[0], "dst": k[1], "edgeType": k[2], "attrs": aed.get(k, bed.get(k))})
    snip = lambda s: s if len(s) <= 2000 else s[:2000] + "\n...[truncated]"
    for cid in sorted(set(bch) | set(ach)):
        b, n = bch.get(cid), ach.get(cid)
        if b and n and b[2] == n[2]:
            continue
        ent_id = (n or b)[0]
        ent = ae.get(ent_id) or be.get(ent_id)
        if not match(ent):
            continue
        bp = "blueprint" in (ent["kind"] if ent else "")
        change = {"entityId": ent_id, "chunkId": cid, "chunkKind": (n or b)[1]}
        if b and not n:
            change.update(type="blueprint-logic-removed" if bp else "content-removed", before=snip(b[2]))
        elif n and not b:
            change.update(type="blueprint-logic-added" if bp else "content-added", after=snip(n[2]))
        else:
            change.update(type="blueprint-logic-changed" if bp else "content-changed", before=snip(b[2]), after=snip(n[2]))
        out.append(change)
    out.sort(key=lambda c: json.dumps(c, sort_keys=True))
    page = out[a.offset:a.offset + a.limit]
    return envelope(con, {"since": before_m, "until": after_m, "sinceLastIngestAtIso": bi, "untilLastIngestAtIso": ai,
                          "changes": page, "total": len(out), "offset": a.offset,
                          "truncated": a.offset + len(page) < len(out),
                          "provenance": "semantic comparison of GameIQ index entities, edges, and chunks"})


# What the `sql` command may do: read rows and call functions. Everything else is denied by the
# SQLite authorizer, so ATTACH (opens another file), VACUUM INTO (writes a file), PRAGMA (can turn
# query_only off) and every INSERT/UPDATE/DELETE/DDL fail even though the file is opened mode=ro.
SQL_ALLOWED = {sqlite3.SQLITE_SELECT, sqlite3.SQLITE_READ, sqlite3.SQLITE_FUNCTION, sqlite3.SQLITE_RECURSIVE}
SQL_SCHEMA_PRAGMAS = {"table_info", "table_xinfo", "table_list", "index_list", "index_info", "index_xinfo"}
SQL_VALUE_PRAGMAS = {"user_version", "schema_version", "page_count", "page_size", "freelist_count",
                     "journal_mode", "encoding", "data_version"}


def sql_authorizer(action, arg1, arg2, _db, _trigger):
    if action in SQL_ALLOWED:
        return sqlite3.SQLITE_OK
    if action == sqlite3.SQLITE_PRAGMA:
        name = (arg1 or "").lower()
        # Schema pragmas take a table/index name and only read; value pragmas are allowed without a
        # value (with one they set it). FTS5 issues "PRAGMA data_version" internally.
        if name in SQL_SCHEMA_PRAGMAS or (name in SQL_VALUE_PRAGMAS and arg2 is None):
            return sqlite3.SQLITE_OK
    return sqlite3.SQLITE_DENY


def cmd_sql(con, a):
    con.set_authorizer(sql_authorizer)
    try:
        return envelope(con, rows(con, a.statement))
    finally:
        con.set_authorizer(None)


# ---- main ------------------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description="Read-only Game IQ queries over .gameiq/index.db (no editor needed).")
    p.add_argument("--project", help="project folder (default: walk up from the current directory)")
    p.add_argument("--db", help="explicit index.db path")
    p.add_argument("--compact", action="store_true", help="print single-line JSON")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("search", help="full-text search (stemmed, camelCase-aware, ranked like the toolset)")
    s.add_argument("query"); s.add_argument("--kind", default=""); s.add_argument("--limit", type=int, default=20)
    s.add_argument("--offset", type=int, default=0); s.add_argument("--path-prefix", default="")
    s.set_defaults(fn=cmd_search)

    s = sub.add_parser("entity", help="one entity with edges, children and chunks")
    s.add_argument("id"); s.add_argument("--cap", type=int, default=50); s.set_defaults(fn=cmd_entity)

    s = sub.add_parser("children", help="page an entity's children, optionally by class")
    s.add_argument("id"); s.add_argument("--class", dest="class_filter", default="")
    s.add_argument("--limit", type=int, default=50); s.add_argument("--offset", type=int, default=0)
    s.set_defaults(fn=cmd_children)

    s = sub.add_parser("references", help="who uses this / what this uses")
    s.add_argument("id"); s.add_argument("--direction", choices=("in", "out", "both"), default="both")
    s.add_argument("--depth", type=int, default=1); s.add_argument("--edge-type", default="")
    s.add_argument("--kind", default=""); s.add_argument("--limit", type=int, default=200)
    s.add_argument("--offset", type=int, default=0); s.set_defaults(fn=cmd_references)

    s = sub.add_parser("impact", help="what could break if this changes")
    s.add_argument("id"); s.add_argument("--limit", type=int, default=200); s.set_defaults(fn=cmd_impact)

    s = sub.add_parser("stats", help="overview | kinds | edges | unused | largest[:Class] | largest-deps | authority | doc-types")
    s.add_argument("facet", nargs="?", default="overview"); s.set_defaults(fn=cmd_stats)

    s = sub.add_parser("changes", help="what changed since a snapshot id, git ref (HEAD~3, a sha, a branch) or ISO time")
    s.add_argument("since"); s.add_argument("until", nargs="?", default="current")
    s.add_argument("--kind", default=""); s.add_argument("--path-prefix", default="")
    s.add_argument("--limit", type=int, default=200); s.add_argument("--offset", type=int, default=0)
    s.set_defaults(fn=cmd_changes)

    s = sub.add_parser("doctor", help="index health, freshness per source, disk-versus-index gap, agent-guide check")
    s.set_defaults(fn=cmd_doctor)

    s = sub.add_parser("sql", help="run one read-only SELECT against the index")
    s.add_argument("statement"); s.set_defaults(fn=cmd_sql)

    a = p.parse_args(argv)
    a.db_path = find_db(a)
    con = open_ro(a.db_path)
    try:
        result = a.fn(con, a)
    except sqlite3.Error as e:
        result = {"error": f"sqlite: {e}"}
    finally:
        con.close()
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=None if a.compact else 2, default=str))
    return 0 if "error" not in result else 1


if __name__ == "__main__":
    sys.exit(main())
