#!/usr/bin/env python3
"""Compatibility wrapper for the unified protocol artifact generator."""

from generate_protocol_artifacts import REPO_ROOT, generate


if __name__ == "__main__":
    files = generate(REPO_ROOT, "protocol-types")
    print(f"[GENERATED] wrote {len(files)} protocol type TSV file(s)")
