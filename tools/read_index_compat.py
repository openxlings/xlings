#!/usr/bin/env python3
"""Read an index tree's `index-compat.json` and print its `requires` block.

The block declares which client versions a published index snapshot needs:

    { "requires": { "xlings": { "min": "2026.8.4.1" } } }

It is a map of consumer name -> {min, max}. xlings evaluates only the "xlings"
key and routes clients that do not satisfy it to an older snapshot; every other
key is carried through untouched for whichever consumer owns it (#476).

Validated here rather than in the client, because a malformed contract should
stop a publish rather than reach every machine that runs `xlings update`.
"""
import json
import sys

# The pointer this ends up in is fetched on every `xlings update`. A publisher
# must not be able to make that fetch expensive for everyone.
MAX_BYTES = 4096


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: read_index_compat.py <index-compat.json>", file=sys.stderr)
        return 2
    try:
        with open(sys.argv[1], encoding="utf-8") as handle:
            doc = json.load(handle)
    except Exception as error:  # noqa: BLE001 - any parse failure is fatal here
        print(f"index-compat.json is not valid JSON: {error}", file=sys.stderr)
        return 1

    requires = doc.get("requires", {})
    if not isinstance(requires, dict):
        print("index-compat.json: `requires` must be an object", file=sys.stderr)
        return 1

    for consumer, bound in requires.items():
        if not isinstance(bound, dict):
            print(f"index-compat.json: requires.{consumer} must be an object",
                  file=sys.stderr)
            return 1
        for key, value in bound.items():
            if key not in ("min", "max"):
                # Not fatal for foreign consumers -- xlings does not own their
                # vocabulary -- but a typo'd bound on OUR key silently means
                # "no constraint", which is the failure mode worth catching.
                if consumer == "xlings":
                    print(f"index-compat.json: requires.xlings.{key} is not a "
                          f"bound; expected min/max", file=sys.stderr)
                    return 1
                continue
            if not isinstance(value, str) or not value:
                print(f"index-compat.json: requires.{consumer}.{key} must be a "
                      f"non-empty string", file=sys.stderr)
                return 1

    blob = json.dumps(requires, separators=(",", ":"), ensure_ascii=False)
    if len(blob.encode("utf-8")) > MAX_BYTES:
        print(f"index-compat.json: `requires` is {len(blob)} bytes, over the "
              f"{MAX_BYTES} limit", file=sys.stderr)
        return 1

    print(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
