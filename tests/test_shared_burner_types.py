"""Prevent divergent C layouts for globals shared across burner translation units."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SharedBurnerTypes(unittest.TestCase):
    def test_shared_layouts_have_one_definition(self):
        consumers = [ROOT / "main/burner/core/ws_server.c", ROOT / "main/burner/core/ws_server_internal.h"]
        for name, header in [("burner_status_t", "burner_status_types.h"),
                             ("burner_tf_list_buf_t", "burner_tf_list_types.h")]:
            for consumer in consumers:
                text = consumer.read_text(encoding="utf-8")
                self.assertIn(f'#include "{header}"', text)
                self.assertNotRegex(text, rf'\}}\s*{name}\s*;')
            definitions = []
            for path in (ROOT / "main").rglob("*"):
                if path.suffix in (".c", ".h"):
                    if re.search(rf'\}}\s*{name}\s*;', path.read_text(encoding="utf-8")):
                        definitions.append(path.name)
            self.assertEqual(definitions, [header])

    def test_status_preserves_mapper_field_before_file_fields(self):
        text = (ROOT / "main/burner/core/burner_status_types.h").read_text(encoding="utf-8")
        self.assertLess(text.index("probe_mapper_name["), text.index("rom_name["))

    def test_global_definitions_compile_against_their_extern_declarations(self):
        source = (ROOT / "main/burner/core/ws_server.c").read_text(encoding="utf-8")
        header = (ROOT / "main/burner/core/ws_server_internal.h").read_text(encoding="utf-8")
        self.assertTrue(source.startswith('#include "ws_server_internal.h"'))
        # Every shared struct/enum must come from the included declaration;
        # copying an identical definition is also forbidden (it can drift).
        pattern = r'typedef\s+(?:struct|enum)[^{;]*\{.*?\}\s*(\w+)\s*;'
        shared = set(re.findall(pattern, header, re.S))
        private = set(re.findall(pattern, source, re.S))
        self.assertFalse(shared & private, f"duplicated shared types: {shared & private}")

    def test_other_public_modules_include_their_header(self):
        for name in ("burner_task", "burner_status", "burner_backend", "burner_spi_backend"):
            text = (ROOT / f"main/burner/core/{name}.c").read_text(encoding="utf-8")
            self.assertIn('#include "ws_server_internal.h"', text)


if __name__ == "__main__":
    unittest.main()
