#!/usr/bin/env python3
"""Versioned standing-null instrument identity, independent of release source identity.

The release receipt still hashes all source/tests/hooks. A null instead binds the
measurement entry points, their transitive local imports (including dormant
function imports), the trusted watchdog, and this scope definition. Editing an
unrelated correctness battery cannot change the comparison instrument.

Runtime cell bytes/parameters, geometry, environment and tool/server binary hashes
remain independently mandatory in abba_evidence. Makefiles build those identified
binaries before measurement; they are not measurement inputs. No old broad hash
can substitute for this new manifest. Nonstandard unresolvable imports and local
symlinks fail closed instead of hiding an executable dependency outside the scope.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import stat
import sys


SCHEMA = 1
SCOPE = "tomokv-abba-instrument-v1"
ROOTS = ("tests/abba_instrument.py", "tests/abbagate.py", "tests/abba_experiments.py",
         "tests/gate_history.py", "tests/legacy_reorder_witness.py")


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode()


def sha256(value):
    return hashlib.sha256(value).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_file(root, relative):
    path = root / relative
    require(path.is_relative_to(root) and all(not parent.is_symlink()
            for parent in [path, *path.parents] if parent.is_relative_to(root)),
            f"symlinked instrument dependency: {relative}")
    before = path.stat()
    require(stat.S_ISREG(before.st_mode), f"nonregular instrument dependency: {relative}")
    content = path.read_bytes()
    after = path.stat()
    identity = lambda s: (s.st_dev, s.st_ino, s.st_mode, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    require(identity(before) == identity(after), f"instrument dependency changed while reading: {relative}")
    return content, "100755" if before.st_mode & 0o111 else "100644"


def python_identity():
    # Paths may differ between otherwise identical installations. Bytes and the
    # interpreter's complete build/version string are the executable identity.
    return dict(implementation=sys.implementation.name, version=sys.version,
                executable_sha256=sha256(Path(sys.executable).resolve().read_bytes()))


def local_modules(root, name, *, relative_to=None):
    bases = (relative_to,) if relative_to is not None else (Path("tests"), Path("."))
    parts = name.split(".") if name else []
    for base in bases:
        module = base.joinpath(*parts)
        candidates = (module.with_suffix(".py"), module / "__init__.py") if parts else (module / "__init__.py",)
        found = next((candidate for candidate in candidates if (root / candidate).is_file()), None)
        if found is None:
            continue
        result = {found.as_posix()}
        for size in range(1, len(parts)):
            initializer = base.joinpath(*parts[:size]) / "__init__.py"
            if (root / initializer).is_file():
                result.add(initializer.as_posix())
        return result
    return set()


def instrument_fingerprint(root):
    root = Path(root).resolve()
    pending, entries = set(ROOTS), {}
    while pending:
        relative = pending.pop()
        if relative in entries:
            continue
        content, mode = read_file(root, relative)
        entries[relative] = dict(path=relative, mode=mode, sha256=sha256(content))
        tree = ast.parse(content, filename=relative)
        for node in ast.walk(tree):
            imports = []
            if isinstance(node, ast.Import):
                imports = [(alias.name, None, True) for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                base = Path(relative).parent
                for _ in range(max(0, node.level - 1)):
                    base = base.parent
                base = base if node.level else None
                imports = [(node.module or "", base, True)]
                imports += [(".".join(filter(None, (node.module, alias.name))), base, False)
                            for alias in node.names if alias.name != "*"]
            elif isinstance(node, ast.Call) and (
                    isinstance(node.func, ast.Name) and node.func.id == "__import__" or
                    isinstance(node.func, ast.Attribute) and node.func.attr == "import_module"):
                require(node.args and isinstance(node.args[0], ast.Constant) and
                        isinstance(node.args[0].value, str),
                        f"unresolved dynamic instrument import in {relative}")
                imports = [(node.args[0].value, None, True)]
            for name, base, required in imports:
                found = local_modules(root, name, relative_to=base)
                if not found and required:
                    require(base is None and name.split(".")[0] in sys.stdlib_module_names,
                            f"unresolved nonstandard instrument import {name!r} in {relative}")
                pending.update(found - entries.keys())
    payload = dict(schema=SCHEMA, scope=SCOPE, roots=list(ROOTS), python=python_identity(),
                   entries=[entries[name] for name in sorted(entries)])
    return {**payload, "sha256": sha256(canonical(payload))}


def validate_fingerprint(value):
    import re
    require(isinstance(value, dict) and set(value) == {"schema", "scope", "roots", "python", "entries", "sha256"},
            "missing/versionless ABBA instrument fingerprint; recollect the standing null")
    require(value["schema"] == SCHEMA and value["scope"] == SCOPE and value["roots"] == list(ROOTS),
            "unsupported ABBA instrument fingerprint scope/version")
    runtime = value["python"]
    require(isinstance(runtime, dict) and set(runtime) == {"implementation", "version", "executable_sha256"} and
            all(isinstance(item, str) and item for item in runtime.values()) and
            re.fullmatch(r"[0-9a-f]{64}", runtime["executable_sha256"]), "missing Python instrument identity")
    rows = value["entries"]
    require(isinstance(rows, list) and rows, "empty instrument dependency manifest")
    paths = []
    for row in rows:
        require(isinstance(row, dict) and set(row) == {"path", "mode", "sha256"}, "invalid instrument dependency")
        path = row["path"]
        require(isinstance(path, str) and not Path(path).is_absolute() and ".." not in Path(path).parts and
                path.endswith(".py") and row["mode"] in ("100644", "100755") and
                re.fullmatch(r"[0-9a-f]{64}", row["sha256"]), "invalid instrument dependency identity")
        paths.append(path)
    require(paths == sorted(set(paths)) and set(ROOTS) <= set(paths), "missing/duplicate instrument roots/dependencies")
    payload = {key: item for key, item in value.items() if key != "sha256"}
    require(value["sha256"] == sha256(canonical(payload)), "instrument fingerprint digest differs from its manifest")
    return value["sha256"]


def self_test():
    import copy
    import shutil
    import subprocess
    import tempfile
    import unittest
    from unittest import mock
    from gate_receipt import harness_fingerprint

    root = Path(__file__).resolve().parents[1]
    actual = instrument_fingerprint(root)

    class Controls(unittest.TestCase):
        def setUp(self):
            (root / "build").mkdir(exist_ok=True)
            self.temporary = tempfile.TemporaryDirectory(dir=root / "build")
            self.addCleanup(self.temporary.cleanup)
            self.root = Path(self.temporary.name)
            for entry in actual["entries"]:
                target = self.root / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / entry["path"], target)
            subprocess.run(["git", "init", "-q", str(self.root)], check=True)
            self.before = instrument_fingerprint(self.root)

        def test_unrelated_correctness_hooks_and_makefile_leave_null_identity_unchanged(self):
            broad = harness_fingerprint(self.root)
            for relative in ("tests/unrelated_correctness.py", "tests/gate.sh", "Makefile", ".githooks/pre-push"):
                with self.subTest(relative=relative):
                    path = self.root / relative
                    path.parent.mkdir(exist_ok=True)
                    path.write_text("# changed ordinary correctness/build/push behavior\n")
                    self.assertEqual(instrument_fingerprint(self.root), self.before)
                    self.assertNotEqual(harness_fingerprint(self.root), broad)

        def test_imported_helper_bytes_modes_and_python_identity_change_fingerprint(self):
            path = self.root / "tests/_lib.py"
            original = path.read_bytes()
            path.write_bytes(original + b"\n# changed RESP helper\n")
            self.assertNotEqual(instrument_fingerprint(self.root)["sha256"], self.before["sha256"])
            path.write_bytes(original)
            original_mode = path.stat().st_mode
            path.chmod(original_mode ^ stat.S_IXUSR)
            self.assertNotEqual(instrument_fingerprint(self.root)["sha256"], self.before["sha256"])
            path.chmod(original_mode)
            with mock.patch(__name__ + ".python_identity", return_value={**self.before["python"], "version": "another build"}):
                self.assertNotEqual(instrument_fingerprint(self.root)["sha256"], self.before["sha256"])

        def test_new_dormant_import_and_its_transitive_dependency_are_bound(self):
            entry = self.root / "tests/abbagate.py"
            with entry.open("a") as stream:
                stream.write("\ndef future_measurement_hook():\n    import extra_instrument_helper\n")
            first = self.root / "tests/extra_instrument_helper.py"
            second = self.root / "tests/another_instrument_helper.py"
            first.write_text("import another_instrument_helper\n")
            second.write_text("value = 1\n")
            before = instrument_fingerprint(self.root)
            paths = {entry["path"] for entry in before["entries"]}
            self.assertIn("tests/extra_instrument_helper.py", paths)
            self.assertIn("tests/another_instrument_helper.py", paths)
            second.write_text("value = 2\n")
            self.assertNotEqual(instrument_fingerprint(self.root)["sha256"], before["sha256"])

        def test_missing_symlinked_and_unresolved_dependencies_fail_closed(self):
            path = self.root / "tests/gate_history.py"
            original = path.read_bytes()
            path.unlink()
            # A removed root can be reached directly or through another module's
            # import first; either route must reject the incomplete instrument.
            with self.assertRaises((FileNotFoundError, ValueError)):
                instrument_fingerprint(self.root)
            path.symlink_to(root / "tests/gate_history.py")
            with self.assertRaisesRegex(ValueError, "symlinked"):
                instrument_fingerprint(self.root)
            path.unlink()
            path.write_bytes(original + b"\nimport missing_instrument_dependency\n")
            with self.assertRaisesRegex(ValueError, "unresolved nonstandard"):
                instrument_fingerprint(self.root)

        def test_old_hash_only_unknown_version_and_tampered_manifest_are_rejected(self):
            self.assertEqual(validate_fingerprint(self.before), self.before["sha256"])
            broken = [None, {"sha256": self.before["sha256"]}]
            for field, value in (("schema", 2), ("roots", []), ("sha256", "0" * 64)):
                changed = copy.deepcopy(self.before)
                changed[field] = value
                broken.append(changed)
            changed = copy.deepcopy(self.before)
            changed["entries"][0]["sha256"] = "0" * 64
            broken.append(changed)
            for value in broken:
                with self.subTest(value=value), self.assertRaises(ValueError):
                    validate_fingerprint(value)

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        raise SystemExit(self_test())
    print(json.dumps(instrument_fingerprint(Path(__file__).resolve().parents[1]), indent=2))
