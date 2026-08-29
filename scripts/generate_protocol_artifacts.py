#!/usr/bin/env python3
"""Generate every C++/TSV artifact derived from Bedrock protocol.json files.

The generator is deliberately dependency-free. proto.yml remains the human
source for minecraft-data, while the vendored protocol.json is its compiled
ProtoDef representation. The take_item_entity block is validated between both
forms before any artifact is written.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
MINECRAFT_DATA_ROOT = REPO_ROOT / "data" / "minecraft-data"
BEDROCK_ROOT = MINECRAFT_DATA_ROOT / "bedrock"
DATA_PATHS_FILE = MINECRAFT_DATA_ROOT / "dataPaths.json"
PROTOCOL_VERSIONS_FILE = BEDROCK_ROOT / "common" / "protocolVersions.json"


def natural_key(value: str) -> tuple[Any, ...]:
    return tuple(
        int(part) if part.isdigit() else part
        for part in re.split(r"(\d+)", value)
    )


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def compact_json(value: Any) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True)


def registered_versions() -> list[dict[str, Any]]:
    releases = [
        value
        for value in load_json(PROTOCOL_VERSIONS_FILE)
        if value.get("releaseType") == "release"
    ]
    seen: set[str] = set()
    for value in releases:
        version = str(value["minecraftVersion"])
        if version in seen:
            raise RuntimeError(f"duplicate registered Bedrock version: {version}")
        seen.add(version)
    return releases


def bedrock_data_paths() -> dict[str, Any]:
    return load_json(DATA_PATHS_FILE).get("bedrock", {})


def component_dir(version: str, component: str) -> str:
    entry = bedrock_data_paths().get(version, {})
    value = entry.get(component, f"bedrock/{version}")
    prefix = "bedrock/"
    if not isinstance(value, str) or not value.startswith(prefix):
        raise RuntimeError(
            f"Bedrock {component} path for {version} is not a Bedrock path: {value!r}"
        )
    return value[len(prefix):]


def protocol_source(version: str) -> tuple[str, Path]:
    source = component_dir(version, "protocol")
    path = BEDROCK_ROOT / source / "protocol.json"
    if not path.is_file():
        raise RuntimeError(
            f"protocol.json for {version} (source {source}) does not exist"
        )
    return source, path


def proto_source(version: str) -> tuple[str, Path]:
    source = component_dir(version, "proto")
    path = BEDROCK_ROOT / source / "proto.yml"
    if not path.is_file():
        raise RuntimeError(
            f"proto.yml for {version} (source {source}) does not exist"
        )
    return source, path


def generated_versions() -> list[str]:
    versions = {
        str(value["minecraftVersion"])
        for value in registered_versions()
    }
    for child in BEDROCK_ROOT.iterdir():
        if child.is_dir() and (child / "protocol.json").is_file():
            versions.add(child.name)
    return sorted(versions, key=natural_key)


def packet_fields(schema: Any) -> list[tuple[str, Any]]:
    if not (
        isinstance(schema, list)
        and len(schema) == 2
        and schema[0] == "container"
        and isinstance(schema[1], list)
    ):
        raise RuntimeError("packet_take_item_entity is not a ProtoDef container")
    fields: list[tuple[str, Any]] = []
    for field in schema[1]:
        if isinstance(field, dict) and isinstance(field.get("name"), str):
            fields.append((field["name"], field.get("type")))
    return fields


def take_item_json_fields(path: Path) -> list[tuple[str, Any]]:
    types = load_json(path).get("types", {})
    schema = types.get("packet_take_item_entity")
    if schema is None:
        raise RuntimeError(f"packet_take_item_entity missing from {path}")
    return packet_fields(schema)


def take_item_yaml_fields(path: Path) -> list[tuple[str, str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    start = next(
        (index for index, line in enumerate(lines) if line == "packet_take_item_entity:"),
        None,
    )
    if start is None:
        raise RuntimeError(f"packet_take_item_entity missing from {path}")

    fields: list[tuple[str, str]] = []
    field_pattern = re.compile(r"^\s+([A-Za-z_][A-Za-z0-9_]*):\s*(\S+)\s*$")
    for line in lines[start + 1:]:
        if line and not line[0].isspace():
            break
        match = field_pattern.match(line)
        if not match:
            continue
        name, field_type = match.groups()
        if not name.startswith("!"):
            fields.append((name, field_type))
    return fields


def validate_take_item_sources() -> None:
    expected = [
        ("runtime_entity_id", "varint64"),
        ("target", "varint64"),
    ]

    for child in sorted(BEDROCK_ROOT.iterdir(), key=lambda p: natural_key(p.name)):
        protocol = child / "protocol.json"
        if protocol.is_file():
            fields = take_item_json_fields(protocol)
            names = {name for name, _ in fields}
            if "runtime_entity_id" in names and fields != expected:
                raise RuntimeError(
                    f"{protocol}: expected {expected!r}, found {fields!r}"
                )

        proto = child / "proto.yml"
        if proto.is_file():
            fields = take_item_yaml_fields(proto)
            names = {name for name, _ in fields}
            if "runtime_entity_id" in names and fields != expected:
                raise RuntimeError(
                    f"{proto}: expected {expected!r}, found {fields!r}"
                )

    for release in registered_versions():
        version = str(release["minecraftVersion"])
        _, protocol = protocol_source(version)
        _, proto = proto_source(version)
        json_fields = take_item_json_fields(protocol)
        yaml_fields = take_item_yaml_fields(proto)
        if json_fields != expected or yaml_fields != expected:
            raise RuntimeError(
                f"{version}: take_item_entity mismatch: "
                f"protocol.json={json_fields!r}, proto.yml={yaml_fields!r}"
            )


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8", newline="\n")


def generate_packet_schema_tsv(output_root: Path) -> list[Path]:
    output_dir = output_root / "data" / "generated" / "packet-schema" / "bedrock"
    output_dir.mkdir(parents=True, exist_ok=True)
    expected_files: list[Path] = []

    for version in generated_versions():
        source, protocol_path = protocol_source(version)
        types = load_json(protocol_path).get("types", {})
        rows: list[tuple[str, list[tuple[str, str]]]] = []
        for type_name, schema in types.items():
            if not type_name.startswith("packet_") or type_name == "packet":
                continue
            fields: list[tuple[str, str]] = []
            if (
                isinstance(schema, list)
                and len(schema) == 2
                and schema[0] == "container"
                and isinstance(schema[1], list)
            ):
                for field in schema[1]:
                    if isinstance(field, dict):
                        fields.append((
                            str(field.get("name", "")),
                            compact_json(field.get("type", "")),
                        ))
            rows.append((type_name[len("packet_"):], fields))

        lines = [
            "# packet\tfield\ttype",
            f"# source_protocol_dir\t{source}",
            f"# source_protocol_json\tdata/minecraft-data/bedrock/{source}/protocol.json",
        ]
        for packet_name, fields in sorted(rows):
            for field_name, field_type in fields:
                lines.append(f"{packet_name}\t{field_name}\t{field_type}")
        output = output_dir / f"{version}.tsv"
        write_text(output, "\n".join(lines) + "\n")
        expected_files.append(output)

    for stale in output_dir.glob("*.tsv"):
        if stale not in expected_files:
            stale.unlink()
    return expected_files


def generate_protocol_type_tsv(output_root: Path) -> list[Path]:
    output_dir = output_root / "data" / "generated" / "protocol-types" / "bedrock"
    output_dir.mkdir(parents=True, exist_ok=True)
    expected_files: list[Path] = []

    for version in generated_versions():
        source, protocol_path = protocol_source(version)
        types = load_json(protocol_path).get("types", {})
        lines = [
            "# type\tjson",
            f"# source_protocol_dir\t{source}",
            f"# source_protocol_json\tdata/minecraft-data/bedrock/{source}/protocol.json",
        ]
        for type_name, schema in sorted(types.items()):
            lines.append(f"{type_name}\t{compact_json(schema)}")
        output = output_dir / f"{version}.tsv"
        write_text(output, "\n".join(lines) + "\n")
        expected_files.append(output)

    for stale in output_dir.glob("*.tsv"):
        if stale not in expected_files:
            stale.unlink()
    return expected_files


def raw_json_literal(value: str) -> str:
    delimiter = "BEDROCKJSON"
    if f"){delimiter}\"" in value:
        raise RuntimeError("generated JSON collides with raw string delimiter")
    return f'R"{delimiter}({value}){delimiter}"'


def generate_cpp_protocol_types(output_root: Path) -> list[Path]:
    releases = registered_versions()
    latest = str(releases[0]["minecraftVersion"])
    version_names = [str(value["minecraftVersion"]) for value in releases]

    version_types: dict[str, dict[str, Any]] = {}
    unique_schema_values: set[str] = set()
    for version in version_names:
        _, protocol_path = protocol_source(version)
        types = load_json(protocol_path).get("types", {})
        version_types[version] = types
        unique_schema_values.update(compact_json(schema) for schema in types.values())

    schemas = sorted(unique_schema_values)
    schema_indexes = {schema: index for index, schema in enumerate(schemas)}
    entries: list[tuple[str, str, int]] = []
    for version, types in version_types.items():
        for type_name, schema in types.items():
            entries.append((
                version,
                type_name,
                schema_indexes[compact_json(schema)],
            ))
    entries.sort(key=lambda value: (value[0], value[1]))

    header = f"""#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrock {{

inline constexpr std::string_view GENERATED_PROTOCOL_TYPES_VERSION = "{latest}";

std::optional<std::string> generatedProtocolTypeJson(
    const std::string& version,
    const std::string& name
);

// Compatibility lookup for callers that explicitly want the latest schema.
std::optional<std::string> generatedProtocolTypeJson(const std::string& name);

std::vector<std::string> generatedProtocolTypeVersions();

}} // namespace bedrock
"""

    cpp_lines = [
        "#include <bedrock/generated/GeneratedProtocolTypes.hpp>",
        "",
        "#include <algorithm>",
        "#include <cstdint>",
        "#include <iterator>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace bedrock {",
        "namespace {",
        "",
        "struct GeneratedProtocolTypeEntry {",
        "    std::string_view version;",
        "    std::string_view name;",
        "    uint32_t schemaIndex;",
        "};",
        "",
        "static const std::string_view SCHEMAS[] = {",
    ]
    cpp_lines.extend(f"    {raw_json_literal(schema)}," for schema in schemas)
    cpp_lines.extend([
        "};",
        "",
        "static const GeneratedProtocolTypeEntry TYPE_ENTRIES[] = {",
    ])
    cpp_lines.extend(
        f'    {{ "{version}", "{name}", {index}u }},'
        for version, name, index in entries
    )
    cpp_lines.extend([
        "};",
        "",
        "static const std::string_view VERSIONS[] = {",
    ])
    cpp_lines.extend(f'    "{version}",' for version in version_names)
    cpp_lines.extend([
        "};",
        "",
        "} // namespace",
        "",
        "std::optional<std::string> generatedProtocolTypeJson(",
        "    const std::string& version,",
        "    const std::string& name",
        ") {",
        "    const auto key = std::pair<std::string_view, std::string_view>(",
        "        version,",
        "        name",
        "    );",
        "    const auto found = std::lower_bound(",
        "        std::begin(TYPE_ENTRIES),",
        "        std::end(TYPE_ENTRIES),",
        "        key,",
        "        [](const GeneratedProtocolTypeEntry& entry, const auto& wanted) {",
        "            return std::pair(entry.version, entry.name) < wanted;",
        "        }",
        "    );",
        "    if (found == std::end(TYPE_ENTRIES) ||",
        "        found->version != version || found->name != name) {",
        "        return std::nullopt;",
        "    }",
        "    return std::string(SCHEMAS[found->schemaIndex]);",
        "}",
        "",
        "std::optional<std::string> generatedProtocolTypeJson(",
        "    const std::string& name",
        ") {",
        "    return generatedProtocolTypeJson(",
        "        std::string(GENERATED_PROTOCOL_TYPES_VERSION),",
        "        name",
        "    );",
        "}",
        "",
        "std::vector<std::string> generatedProtocolTypeVersions() {",
        "    std::vector<std::string> out;",
        "    out.reserve(std::size(VERSIONS));",
        "    for (const auto version : VERSIONS) out.emplace_back(version);",
        "    return out;",
        "}",
        "",
        "} // namespace bedrock",
        "",
    ])

    header_path = (
        output_root / "include" / "bedrock" / "generated" /
        "GeneratedProtocolTypes.hpp"
    )
    cpp_path = (
        output_root / "src" / "generated" / "GeneratedProtocolTypes.cpp"
    )
    write_text(header_path, header)
    write_text(cpp_path, "\n".join(cpp_lines))
    return [header_path, cpp_path]


def generate_packet_tables(output_root: Path) -> list[Path]:
    node = shutil.which("node")
    if not node:
        raise RuntimeError("node is required to generate protocol packet tables")
    output_root.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            node,
            str(REPO_ROOT / "tools" / "generate_protocol_tables.js"),
            "all",
            str(BEDROCK_ROOT),
        ],
        cwd=output_root,
        check=True,
    )
    return [
        output_root / "include" / "bedrock" / "generated" /
        "GeneratedProtocolTables.hpp",
        output_root / "src" / "generated" / "GeneratedProtocolTables.cpp",
    ]


def generate(output_root: Path, only: str = "all") -> list[Path]:
    validate_take_item_sources()
    generated: list[Path] = []
    if only in ("all", "packet-schema"):
        generated.extend(generate_packet_schema_tsv(output_root))
    if only in ("all", "protocol-types"):
        generated.extend(generate_protocol_type_tsv(output_root))
    if only in ("all", "cpp-types"):
        generated.extend(generate_cpp_protocol_types(output_root))
    if only in ("all", "packet-tables"):
        generated.extend(generate_packet_tables(output_root))
    return generated


def relative_generated_files(root: Path) -> list[Path]:
    files = [
        Path("include/bedrock/generated/GeneratedProtocolTables.hpp"),
        Path("include/bedrock/generated/GeneratedProtocolTypes.hpp"),
        Path("src/generated/GeneratedProtocolTables.cpp"),
        Path("src/generated/GeneratedProtocolTypes.cpp"),
    ]
    for directory in (
        Path("data/generated/packet-schema/bedrock"),
        Path("data/generated/protocol-types/bedrock"),
    ):
        files.extend(
            path.relative_to(root)
            for path in sorted((root / directory).glob("*.tsv"))
        )
    return sorted(files, key=lambda path: path.as_posix())


def compare_generated() -> None:
    with tempfile.TemporaryDirectory(prefix="bedrock-protocol-generated-") as temp:
        temporary_root = Path(temp)
        generate(temporary_root, "all")
        expected = relative_generated_files(temporary_root)
        actual = relative_generated_files(REPO_ROOT)
        if expected != actual:
            missing = sorted(set(expected) - set(actual))
            stale = sorted(set(actual) - set(expected))
            raise RuntimeError(
                "generated file list differs; "
                f"missing={missing!r}, stale={stale!r}"
            )

        mismatches: list[str] = []
        for relative in expected:
            wanted = (temporary_root / relative).read_text(encoding="utf-8")
            current = (REPO_ROOT / relative).read_text(encoding="utf-8")
            if wanted == current:
                continue
            diff = "".join(difflib.unified_diff(
                current.splitlines(keepends=True),
                wanted.splitlines(keepends=True),
                fromfile=str(relative),
                tofile=f"generated/{relative}",
                n=2,
            ))
            mismatches.append(diff[:8000])

        if mismatches:
            raise RuntimeError(
                f"{len(mismatches)} generated file(s) are stale\n" +
                "\n".join(mismatches[:5])
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-root",
        type=Path,
        default=REPO_ROOT,
        help="repository-shaped output root (default: current repository)",
    )
    parser.add_argument(
        "--only",
        choices=(
            "all",
            "packet-schema",
            "protocol-types",
            "cpp-types",
            "packet-tables",
        ),
        default="all",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="generate in a temporary directory and compare with the repository",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.check:
            compare_generated()
            print("[GENERATED-CHECK] all protocol artifacts are current")
        else:
            files = generate(args.output_root.resolve(), args.only)
            print(f"[GENERATED] wrote {len(files)} artifact(s)")
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"[GENERATED-ERROR] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
