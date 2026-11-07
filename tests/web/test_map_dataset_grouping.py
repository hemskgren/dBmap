"""Mirror map dataset discovery grouping (scratch metadata → Helicopter option)."""

from __future__ import annotations


def datasets_from_scratch_list(objects: list[dict]) -> list[dict]:
    by_dataset: dict[str, dict] = {}
    for item in objects:
        meta = item.get("metadata") or {}
        if meta.get("kind") != "map-layer" or not meta.get("dataset"):
            continue
        dataset_id = str(meta["dataset"])
        entry = by_dataset.setdefault(
            dataset_id,
            {
                "id": dataset_id,
                "title": meta.get("dataset_title") or dataset_id,
                "layers": [],
            },
        )
        entry["layers"].append(
            {
                "objectId": item["id"],
                "layer": meta.get("layer") or "",
                "title": meta.get("title") or meta.get("layer") or item["id"],
            }
        )
        if meta.get("dataset_title"):
            entry["title"] = meta["dataset_title"]
    return sorted(by_dataset.values(), key=lambda row: row["title"])


def test_helicopter_layers_group_into_one_dataset() -> None:
    datasets = datasets_from_scratch_list(
        [
            {
                "id": "route-id",
                "metadata": {
                    "kind": "map-layer",
                    "dataset": "helicopter",
                    "dataset_title": "Helicopter",
                    "layer": "route",
                    "title": "Helicopter ground-truth route",
                },
            },
            {
                "id": "analysis-id",
                "metadata": {
                    "kind": "map-layer",
                    "dataset": "helicopter",
                    "dataset_title": "Helicopter",
                    "layer": "analysis",
                    "title": "Helicopter acoustic analysis (simulated)",
                },
            },
        ]
    )
    assert len(datasets) == 1
    assert datasets[0]["id"] == "helicopter"
    assert datasets[0]["title"] == "Helicopter"
    layers = {layer["layer"]: layer["objectId"] for layer in datasets[0]["layers"]}
    assert layers == {"route": "route-id", "analysis": "analysis-id"}


def test_expired_or_unrelated_objects_are_ignored() -> None:
    assert datasets_from_scratch_list([]) == []
    assert (
        datasets_from_scratch_list(
            [{"id": "x", "metadata": {"kind": "other", "dataset": "helicopter"}}]
        )
        == []
    )
