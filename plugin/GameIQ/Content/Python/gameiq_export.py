"""
Game IQ — Tier 1 asset extractor (Python, in-editor).

Produces "what each asset *is*" summaries via UE's reflection/editor API and writes
an ExtractorOutput JSON the Game IQ core ingests. This pairs with the headless C++
`GameIQExport` commandlet (Tier 0: identity + dependency graph): the commandlet is
fast and editor-less; this fills in typed per-asset detail and search chunks.

Run inside the editor (Output Log > Python) or headless:
    UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="gameiq_export.py"

Tier 2 (Blueprint/material *logic* as pseudocode) is intentionally NOT here: the
faithful path is C++ `FEdGraphUtilities::ExportNodesToText` + a renderer (design
§5.1), not Python graph walking. This module sticks to Tier 1, which UE's
reflection export makes nearly free.
"""

from __future__ import annotations

import json
import os
import datetime

import unreal  # provided by the UE Python environment

SCHEMA_VERSION = 1
PRODUCER = "gameiq-ue-python@0.1.0"


def _project_root() -> str:
    return unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())


def _out_dir() -> str:
    d = os.path.join(_project_root(), ".gameiq", "extract")
    os.makedirs(d, exist_ok=True)
    return d


def _safe(getter, default=None):
    """Best-effort property read — UE Python getters vary by version/type."""
    try:
        return getter()
    except Exception:
        return default


def _tier1_detail(asset) -> dict:
    """A small, defensive per-type recipe. Anything unknown falls back to class+name."""
    detail = {}
    try:
        if isinstance(asset, unreal.Texture2D):
            detail["width"] = _safe(asset.blueprint_get_size_x)
            detail["height"] = _safe(asset.blueprint_get_size_y)
            detail["compression"] = str(_safe(lambda: asset.get_editor_property("compression_settings")))
            detail["srgb"] = _safe(lambda: asset.get_editor_property("srgb"))
        elif isinstance(asset, unreal.StaticMesh):
            detail["num_lods"] = _safe(asset.get_num_lods)
            detail["num_triangles_lod0"] = _safe(lambda: asset.get_num_triangles(0))
        elif isinstance(asset, unreal.MaterialInterface):
            detail["material"] = True
        elif isinstance(asset, unreal.DataTable):
            row_struct = _safe(lambda: asset.get_editor_property("row_struct"))
            detail["row_struct"] = str(row_struct) if row_struct else None
    except Exception as exc:  # never let one asset abort the run
        detail["extract_error"] = str(exc)
    return detail


def export() -> str:
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    all_assets = ar.get_all_assets()

    entities = []
    chunks = []

    for data in all_assets:
        package = str(data.package_name)
        if not package.startswith("/Game"):
            continue
        try:
            asset = data.get_asset()
        except Exception:
            asset = None
        if asset is None:
            continue

        asset_class = str(data.asset_class_path.asset_name)
        ent_id = "asset:%s" % package
        detail = _tier1_detail(asset)
        summary = "%s %s" % (asset_class, str(data.asset_name))

        entities.append({
            "id": ent_id,
            "kind": "asset",
            "name": str(data.asset_name),
            "path": package,
            "source": "asset",
            "summary": summary,
            "detail": dict(detail, assetClass=asset_class),
        })

        # one recipe-summary chunk so the asset is searchable by its properties
        body = "\n".join("%s=%s" % (k, v) for k, v in detail.items() if v is not None)
        chunks.append({
            "id": "%s#summary" % ent_id,
            "entityId": ent_id,
            "kind": "recipe-summary",
            "text": "%s\n%s" % (summary, body),
        })

    output = {
        "schemaVersion": SCHEMA_VERSION,
        "generatedAtIso": datetime.datetime.utcnow().isoformat() + "Z",
        "producer": PRODUCER,
        "project": {"name": unreal.SystemLibrary.get_game_name(), "root": _project_root()},
        "entities": entities,
        "edges": [],
        "chunks": chunks,
    }

    out_file = os.path.join(_out_dir(), "assets.json")
    with open(out_file, "w", encoding="utf-8") as fh:
        json.dump(output, fh, indent=2)
    unreal.log("Game IQ: wrote %d Tier 1 asset entities to %s" % (len(entities), out_file))
    return out_file


if __name__ == "__main__":
    export()
