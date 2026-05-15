#!/usr/bin/env python3
"""
htc_reference_model.py — Independent reference model for htc (Battery 25 Q1).

This model shares NO code with the C implementation.  It maintains a simple
dict[hash, value] and replays operation traces, returning the same abstract
results that the C htc table should produce.

Usage:
    python3 htc_reference_model.py < trace.json

Trace format (JSON lines):
    {"op": "insert", "hash": 42, "value": 100}
    {"op": "find", "hash": 42}
    {"op": "remove", "hash": 42}
    {"op": "update", "hash": 42, "value": 200}
    {"op": "upsert", "hash": 42, "value": 300}
    {"op": "grow"}
    {"op": "reseed"}
    {"op": "checksum"}

Each line produces a result line.
"""

import json
import sys

HTC_OK = 0
HTC_ERR_DUPLICATE = 1
HTC_ERR_NOT_FOUND = 2
HTC_ERR_OOM = 3
HTC_ERR_PATHOLOGICAL = 4


class ReferenceModel:
    """Simple dict-based reference model.  No buckets, remap, stash, or arena."""

    def __init__(self):
        self.map = {}  # hash -> value

    def insert(self, h, v):
        if h in self.map:
            return HTC_ERR_DUPLICATE
        self.map[h] = v
        return HTC_OK

    def find(self, h):
        if h in self.map:
            return HTC_OK, self.map[h]
        return HTC_ERR_NOT_FOUND, None

    def remove(self, h):
        if h not in self.map:
            return HTC_ERR_NOT_FOUND
        del self.map[h]
        return HTC_OK

    def update(self, h, v):
        if h not in self.map:
            return HTC_ERR_NOT_FOUND
        self.map[h] = v
        return HTC_OK

    def upsert(self, h, v):
        self.map[h] = v
        return HTC_OK

    def grow(self):
        """Structural operations preserve abstract state."""
        pass

    def reseed(self):
        """Seed changes only affect placement, not identity."""
        pass

    def checksum(self):
        """Layout-independent checksum: XOR of hash ^ value for all live entries."""
        cs = 0
        for h, v in sorted(self.map.items()):
            cs ^= h
            cs ^= v
            cs = ((cs << 7) | (cs >> 57)) & 0xFFFFFFFFFFFFFFFF
        return cs

    def size(self):
        return len(self.map)


def main():
    model = ReferenceModel()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as e:
            print(f'{{"error": "json parse: {e}"}}')
            continue

        op = obj.get("op")
        h = obj.get("hash", 0)
        v = obj.get("value", 0)

        if op == "insert":
            r = model.insert(h, v)
            print(json.dumps({"op": "insert", "hash": h, "result": r, "size": model.size()}))
        elif op == "find":
            r, val = model.find(h)
            if r == HTC_OK:
                print(json.dumps({"op": "find", "hash": h, "result": r, "value": val}))
            else:
                print(json.dumps({"op": "find", "hash": h, "result": r}))
        elif op == "remove":
            r = model.remove(h)
            print(json.dumps({"op": "remove", "hash": h, "result": r, "size": model.size()}))
        elif op == "update":
            r = model.update(h, v)
            print(json.dumps({"op": "update", "hash": h, "result": r}))
        elif op == "upsert":
            r = model.upsert(h, v)
            print(json.dumps({"op": "upsert", "hash": h, "result": r, "size": model.size()}))
        elif op == "grow":
            model.grow()
            print(json.dumps({"op": "grow", "result": HTC_OK}))
        elif op == "reseed":
            model.reseed()
            print(json.dumps({"op": "reseed", "result": HTC_OK}))
        elif op == "checksum":
            cs = model.checksum()
            print(json.dumps({"op": "checksum", "checksum": cs, "size": model.size()}))
        else:
            print(json.dumps({"error": f"unknown op: {op}"}))


if __name__ == "__main__":
    main()
