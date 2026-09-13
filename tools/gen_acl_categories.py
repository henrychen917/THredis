#!/usr/bin/env python3
"""Generate TomoKV's ACL category table from vanilla Redis 7.4 sources."""

import argparse
import difflib
import pathlib
import re
import sys


EXPECTED_CATEGORIES = [
    "keyspace", "read", "write", "set", "sortedset", "list", "hash", "string",
    "bitmap", "hyperloglog", "geo", "stream", "pubsub", "admin", "fast", "slow",
    "blocking", "dangerous", "connection", "transaction", "scripting",
]

COMMAND_SOURCES = [
    "src/cmd/t_string.cc",
    "src/cmd/t_hash.cc",
    "src/cmd/t_hash_ttl.cc",
    "src/cmd/t_list.cc",
    "src/cmd/t_set.cc",
    "src/cmd/t_zset.cc",
    "src/cmd/t_zset_ops.cc",
    "src/cmd/geo.cc",
    "src/cmd/t_stream.cc",
    "src/cmd/t_stream_groups.cc",
    "src/cmd/t_server.cc",
    "src/cmd/scripting.cc",
    "src/cmd/functions.cc",
    "src/cmd/server_tail.cc",
    "src/cmd/slowlog.cc",
    "src/cmd/lcs.cc",
    "src/cmd/cmdgap.cc",
    "src/cmd/pfdebug.cc",
]


def macro_arguments(line: str, marker: str) -> list[str]:
    start = line.index(marker) + len(marker)
    depth = 1
    quote = False
    escaped = False
    end = None
    for offset, char in enumerate(line[start:], start):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                end = offset
                break
    if end is None:
        raise ValueError(f"unterminated {marker} invocation")

    fields = []
    field_start = start
    quote = False
    escaped = False
    depths = {"(": 0, "[": 0, "{": 0}
    closing = {")": "(", "]": "[", "}": "{"}
    for offset, char in enumerate(line[start:end], start):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char in depths:
            depths[char] += 1
        elif char in closing:
            depths[closing[char]] -= 1
        elif char == "," and not any(depths.values()):
            fields.append(line[field_start:offset].strip())
            field_start = offset + 1
    fields.append(line[field_start:end].strip())
    return fields


def redis_categories(redis_root: pathlib.Path) -> tuple[list[tuple[str, int]], dict[str, int]]:
    server_h = (redis_root / "src/server.h").read_text()
    bits = {
        token: int(bit)
        for token, bit in re.findall(
            r"^#define ACL_CATEGORY_([A-Z]+) \(1ULL<<(\d+)\)$", server_h, re.MULTILINE
        )
    }
    acl_c = (redis_root / "src/acl.c").read_text()
    table = acl_c.split("ACLDefaultCommandCategories[]", 1)[1].split("{NULL,0}", 1)[0]
    ordered = []
    token_bits = {}
    for name, token in re.findall(r'\{"([a-z]+)", ACL_CATEGORY_([A-Z]+)\}', table):
        if token not in bits:
            raise ValueError(f"Redis category {token} has no bit definition")
        ordered.append((name, bits[token]))
        token_bits[token] = bits[token]
    if [name for name, _ in ordered] != EXPECTED_CATEGORIES or [bit for _, bit in ordered] != list(range(21)):
        raise ValueError("Redis category names or bit positions no longer match the 7.4 contract")
    return ordered, token_bits


def redis_commands(redis_root: pathlib.Path, token_bits: dict[str, int]) -> dict[str, int]:
    commands_def = (redis_root / "src/commands.def").read_text()
    body = commands_def.split("struct COMMAND_STRUCT redisCommandTable[] = {", 1)[1]
    body = body.split("{0}", 1)[0]
    result = {}
    for line in body.splitlines():
        if "MAKE_CMD(" not in line:
            continue
        fields = macro_arguments(line, "MAKE_CMD(")
        if len(fields) != 21:
            raise ValueError(f"expected 21 MAKE_CMD fields, found {len(fields)}")
        name_match = re.fullmatch(r'"([a-z0-9_-]+)"', fields[0])
        if not name_match:
            raise ValueError(f"unexpected Redis command name {fields[0]}")
        name = name_match.group(1).upper()
        flags = set(re.findall(r"CMD_[A-Z_]+", fields[15]))
        category_tokens = re.findall(r"ACL_CATEGORY_([A-Z]+)", fields[16])
        mask = 0
        for token in category_tokens:
            if token not in token_bits:
                raise ValueError(f"unknown Redis ACL category token {token} on {name}")
            mask |= 1 << token_bits[token]

        # setImplicitACLCategories(), redis 7.4 server.c:3008-3025.
        if "CMD_WRITE" in flags:
            mask |= 1 << token_bits["WRITE"]
        if "CMD_READONLY" in flags and not (mask & (1 << token_bits["SCRIPTING"])):
            mask |= 1 << token_bits["READ"]
        if "CMD_ADMIN" in flags:
            mask |= (1 << token_bits["ADMIN"]) | (1 << token_bits["DANGEROUS"])
        if "CMD_PUBSUB" in flags:
            mask |= 1 << token_bits["PUBSUB"]
        if "CMD_FAST" in flags:
            mask |= 1 << token_bits["FAST"]
        if "CMD_BLOCKING" in flags:
            mask |= 1 << token_bits["BLOCKING"]
        if not (mask & (1 << token_bits["FAST"])):
            mask |= 1 << token_bits["SLOW"]
        if name in result:
            raise ValueError(f"duplicate top-level Redis command {name}")
        result[name] = mask
    return result


def tomo_commands(repo_root: pathlib.Path) -> list[str]:
    result = []
    for relative in COMMAND_SOURCES:
        source = (repo_root / relative).read_text()
        match = re.search(r"static const CommandSpec kTable\[\] = \{(.*?)\n\};", source, re.DOTALL)
        if not match:
            raise ValueError(f"cannot find CommandSpec table in {relative}")
        result.extend(re.findall(r'^\s*\{"([A-Z][A-Z0-9_-]*)"\s*,', match.group(1), re.MULTILINE))
    duplicates = sorted({name for name in result if result.count(name) > 1})
    if duplicates:
        raise ValueError(f"duplicate TomoKV commands: {', '.join(duplicates)}")
    return result


def generated_text(repo_root: pathlib.Path, redis_root: pathlib.Path) -> str:
    categories, token_bits = redis_categories(redis_root)
    redis = redis_commands(redis_root, token_bits)
    tomo = tomo_commands(repo_root)
    missing = sorted(set(tomo) - set(redis))
    if missing:
        raise ValueError(f"TomoKV commands missing from Redis 7.4: {', '.join(missing)}")

    lines = [
        "// Generated by tools/gen_acl_categories.py from vanilla Redis 7.4; do not edit.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace tomo {",
        "",
        "struct AclCategoryDefinition {",
        "    const char* name;",
        "    uint64_t bit;",
        "};",
        "",
        "inline constexpr AclCategoryDefinition kAclCategories[] = {",
    ]
    for name, bit in categories:
        lines.append(f'    {{"{name}", uint64_t{{1}} << {bit}}},')
    lines.extend([
        "};",
        "inline constexpr std::size_t kAclCategoryCount =",
        "    sizeof(kAclCategories) / sizeof(kAclCategories[0]);",
        "",
        "struct AclCommandCategoryDefinition {",
        "    const char* name;",
        "    uint64_t categories;",
        "};",
        "",
        "inline constexpr AclCommandCategoryDefinition kAclCommandCategories[] = {",
    ])
    names_by_bit = {bit: name for name, bit in categories}
    for name in sorted(tomo):
        mask = redis[name]
        labels = " ".join(names_by_bit[bit] for bit in range(21) if mask & (1 << bit))
        lines.append(f'    {{"{name}", 0x{mask:016x}ULL}}, // {labels}')
    lines.extend([
        "};",
        "inline constexpr std::size_t kAclCommandCategoryCount =",
        "    sizeof(kAclCommandCategories) / sizeof(kAclCommandCategories[0]);",
        "",
        "}  // namespace tomo",
        "",
    ])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-root", default="/tmp/claude-1000/redis74")
    parser.add_argument("--repo-root", default=str(pathlib.Path(__file__).resolve().parents[1]))
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--output")
    group.add_argument("--check")
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root)
    output = generated_text(repo_root, pathlib.Path(args.redis_root))
    if args.check:
        target = pathlib.Path(args.check)
        actual = target.read_text() if target.exists() else ""
        if actual != output:
            sys.stderr.writelines(difflib.unified_diff(
                actual.splitlines(True), output.splitlines(True),
                fromfile=str(target), tofile="generated",
            ))
            return 1
        return 0
    if args.output:
        pathlib.Path(args.output).write_text(output)
    else:
        sys.stdout.write(output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"gen_acl_categories.py: {error}", file=sys.stderr)
        raise SystemExit(1)
