#!/usr/bin/env python3
"""Real gc-merge regressions for late-linked global-layout/lifetime APIs.

Run with Python unittest, using the tools found on PATH. Set EJIT_LIPO_SCRIPT
to a saved lipo.py to reproduce a prior version without editing the worktree.
Optionally set EJIT_LIPO_RUNTIME_ARCHIVE to validate a built runtime archive.
No SDK, board execution, or application-object retention is tested here.
"""

import os
import tempfile
import unittest
from pathlib import Path

import test_lipo_gc_roots as support


OPTIONAL_APIS = (
    "ejit_init_global_layout",
    "ejit_get_global_layout_status",
    "ejit_refresh_global_layout",
    "ejit_shutdown_checked",
)


def undefined_symbols(nm, archive):
    output = support.run([nm, "-g", "--undefined-only", archive]).stdout
    return {line.split()[-1] for line in output.splitlines() if line.split()}


class GlobalLayoutRootsTest(unittest.TestCase):
    def setUp(self):
        self.clang = support.find_tool("clang-18", "clang")
        self.ar = support.find_tool("llvm-ar", "ar")
        self.nm = support.find_tool("llvm-nm", "nm")
        self.ld = support.find_tool("ld.lld")
        self.directory = tempfile.TemporaryDirectory(prefix="ejit-lipo-layout-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        script = os.environ.get("EJIT_LIPO_SCRIPT")
        previous = support.SCRIPT
        if script:
            support.SCRIPT = Path(script).resolve()
        try:
            self.lipo = support.load_lipo()
        finally:
            support.SCRIPT = previous

    def merge_and_check(self, archive, present, name, control=False):
        expected = set(present)
        self.assertTrue(expected.issubset(support.defined_symbols(self.nm, archive)))
        merged, log = support.gc_merge(
            self.lipo, self.root, archive, self.ar, self.nm, self.ld, name)
        defined = support.defined_symbols(self.nm, merged)
        undefined = undefined_symbols(self.nm, merged)
        self.assertIn("ejit_init", defined, log)
        self.assertEqual(set(OPTIONAL_APIS) & defined, expected, log)
        self.assertFalse(set(OPTIONAL_APIS) & undefined,
                         f"missing optional APIs became undefined roots: {undefined}; {log}")
        if control:
            self.assertNotIn("deliberately_unrooted", defined, log)

    def test_current_archive_retains_all_four_exports(self):
        archive = support.build_archive(
            self.root, self.clang, self.ar, OPTIONAL_APIS, "current")
        self.merge_and_check(archive, OPTIONAL_APIS, "current", control=True)

    def test_old_archive_does_not_gain_undefined_optional_roots(self):
        archive = support.build_archive(
            self.root, self.clang, self.ar, (), "old")
        self.merge_and_check(archive, (), "old", control=True)

    def test_partial_archives_retain_only_their_defined_apis(self):
        # Exercise every API independently, plus the init/status-only generation.
        # Unconditionally rooting all four would retain new archives but break
        # compatibility by adding undefined symbols to these older inputs.
        subsets = [(api,) for api in OPTIONAL_APIS] + [OPTIONAL_APIS[:2]]
        for index, present in enumerate(subsets):
            with self.subTest(defined=present):
                name = f"partial{index}"
                archive = support.build_archive(
                    self.root, self.clang, self.ar, present, name)
                self.merge_and_check(archive, present, name, control=True)

    @unittest.skipUnless(os.environ.get("EJIT_LIPO_RUNTIME_ARCHIVE"),
                         "set EJIT_LIPO_RUNTIME_ARCHIVE for built-runtime coverage")
    def test_existing_built_runtime_retains_all_four_exports(self):
        archive = Path(os.environ["EJIT_LIPO_RUNTIME_ARCHIVE"]).resolve()
        self.assertTrue(archive.is_file(), str(archive))
        self.merge_and_check(archive, OPTIONAL_APIS, "runtime")


if __name__ == "__main__":
    unittest.main()
