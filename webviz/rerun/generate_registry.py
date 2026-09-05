#!/usr/bin/env python3
# Copyright (C) 2026 by Thun Lu. All rights reserved.

"""Generate field descriptors from the installed Rerun SDK declarations."""

import argparse
from pathlib import Path
import re


def declarations(path):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)


def generate(sdk, output):
    includes = set()
    fields = []
    aliases = {}
    enums = {}
    for path in sorted((sdk / "archetypes").glob("*.hpp")):
        source = declarations(path)
        name = re.search(r'ArchetypeName\[\]\s*=\s*"rerun\.archetypes\.([^"]+)"', source)
        if not name:
            raise ValueError(f"Missing archetype declaration: {path}")
        archetype = name[1]
        members = set(re.findall(r"std::optional<ComponentBatch>\s+(\w+)\s*;", source))
        descriptors = dict(re.findall(
            r"Descriptor_(\w+)\s*=\s*ComponentDescriptor\(\s*ArchetypeName,\s*\"[^\"]+\",\s*"
            r"Loggable<rerun::components::(\w+)>::ComponentType\s*\)", source))
        setters = {}
        for field, collection, component in re.findall(
                r"\bwith_(\w+)\s*\(\s*const\s+(Collection<)?rerun::components::(\w+)>?\s*&", source):
            if field in descriptors and component == descriptors[field]:
                setters[field] = bool(collection)
        if not members or members != descriptors.keys() or members != setters.keys():
            raise ValueError(f"Unsupported archetype declaration: {path}")
        includes.add(f"rerun/archetypes/{path.name}")
        for field in sorted(members):
            component = descriptors[field]
            fields.append(
                f'    {{"{archetype}", "{field}", "{component}", '
                f'&::rerun::archetypes::{archetype}::Descriptor_{field},\n'
                f'     &::rerun::Loggable<::rerun::components::{component}>::arrow_data_type, '
                f'{str(setters[field]).lower()}}},')

    for folder in ("components", "encodings"):
        for path in sorted((sdk / folder).glob("*.hpp")):
            source = declarations(path)
            for name, body in re.findall(r"enum class (\w+)\s*:\s*\w+\s*\{([^}]+)\}", source):
                if not re.search(rf"struct Loggable<(?:rerun::)?{folder}::{name}>", source):
                    continue
                includes.add(f"rerun/{folder}/{path.name}")
                enums[name] = (folder, [re.match(r"\s*(\w+)", item)[1]
                                       for item in body.split(",") if item.strip()])
            wrapper = re.search(r"struct (\w+)\s*\{", source)
            underlying = re.search(r"return Loggable<rerun::encodings::(\w+)>::arrow_data_type\(\)", source)
            if folder == "components" and wrapper and underlying:
                aliases[wrapper[1]] = underlying[1]

    enum_rows = []
    for name, (folder, values) in sorted(enums.items()):
        for alias in sorted({name} | {key for key, value in aliases.items() if value == name}):
            for value in values:
                enum_rows.append(f'    {{"{alias}", "{value}", '
                                 f'static_cast<uint64_t>(::rerun::{folder}::{name}::{value})}},')
    if not fields or not enum_rows:
        raise ValueError("No Rerun fields or enums found")
    text = '// Generated from Rerun SDK declarations by generate_registry.py.\n\n'
    text += '#include "rerun_schema.h"\n'
    text += ''.join(f'#include <{name}>\n' for name in sorted(includes))
    text += '\nnamespace vlink {\nnamespace webviz {\n\n'
    for name, data_type, rows in (("fields", "RerunField", fields), ("enums", "RerunEnum", enum_rows)):
        text += f'const std::vector<{data_type}>& rerun_{name}() {{\n'
        text += f'  static const std::vector<{data_type}> values = {{\n'
        text += '\n'.join(rows) + '\n  };\n  return values;\n}\n\n'
    text += '}  // namespace webviz\n}  // namespace vlink\n'
    output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    generate(args.sdk, args.output)
