#!/usr/bin/env python3
"""Regression test for late-bound EJIT bound-pointer lipo GC roots."""

import importlib.util
import os
import tempfile
import types
import unittest
from unittest import mock


LIPO_PATH = os.path.join(os.path.dirname(__file__), "lipo.py")
SPEC = importlib.util.spec_from_file_location("ejit_lipo", LIPO_PATH)
LIPO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIPO)

BOUND_PTR_ROOTS = {
    "ejit_taskpool_compile_or_get_bound",
    "ejit_taskpool_compile_or_get_bound_v",
}


class LipoBoundPtrRootTest(unittest.TestCase):
    def test_bound_pointer_apis_are_passed_to_ld_as_roots(self):
        with tempfile.TemporaryDirectory() as directory:
            input_a = os.path.join(directory, "libejit.a")
            output_a = os.path.join(directory, "libejit_gc.a")
            with open(input_a, "wb") as stream:
                stream.write(b"archive")

            ld_commands = []
            defined_output = "".join(
                f"00000000 T {name}\n" for name in sorted(BOUND_PTR_ROOTS))

            def fake_run(command, **kwargs):
                if command[:2] == ["ar", "x"]:
                    object_path = os.path.join(kwargs["cwd"], "runtime.o")
                    with open(object_path, "wb") as stream:
                        stream.write(b"object")
                elif command and command[0] == "nm":
                    return types.SimpleNamespace(
                        returncode=0, stdout=defined_output, stderr="")
                elif command and command[0] == "fake-ld":
                    ld_commands.append(command)
                    output_index = command.index("-o") + 1
                    with open(command[output_index], "wb") as stream:
                        stream.write(b"linked")
                elif command[:2] == ["ar", "crs"]:
                    with open(command[2], "wb") as stream:
                        stream.write(b"archive")
                return types.SimpleNamespace(returncode=0, stdout="", stderr="")

            args = types.SimpleNamespace(
                input=input_a, output=output_a, build_dir=directory,
                ld="fake-ld")
            with mock.patch.object(LIPO.sp, "run", side_effect=fake_run), \
                 mock.patch.object(LIPO, "_try_strip_arm_mapping_symbols"), \
                 mock.patch.object(LIPO, "_try_remove_group"), \
                 mock.patch.object(LIPO, "_readelf_sections",
                                   return_value={".symtab", ".strtab"}), \
                 mock.patch.object(LIPO, "_readelf_is_relocatable"), \
                 mock.patch.object(LIPO, "_check_required_sections"), \
                 mock.patch.object(LIPO, "_check_root_definitions"):
                LIPO.doit_gc_merge(args)

            self.assertEqual(len(ld_commands), 1)
            ld_command = ld_commands[0]
            roots = {
                ld_command[index + 1]
                for index, value in enumerate(ld_command[:-1])
                if value == "-u"
            }
            self.assertTrue(BOUND_PTR_ROOTS.issubset(roots))


if __name__ == "__main__":
    unittest.main()
