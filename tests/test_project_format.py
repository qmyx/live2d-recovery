"""Run with: python tests/test_project_format.py PATH_TO_PROJECT_FIXTURE_EXE.

Uses only generated data and the Python standard library. The fixture executable
links recovery_core; Cubism Editor is not needed for these serialization checks.
"""

import io
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile


def contents(data):
    if data[:4] != b"CAFF":
        raise ValueError("Missing CAFF signature")
    mask = struct.unpack_from(">I", data, 14)[0]
    count = struct.unpack_from(">I", data, 54)[0] ^ mask
    cursor = 58
    entries = {}
    for _ in range(count):
        strings = []
        for _ in range(2):
            length = data[cursor] ^ (mask & 255)
            cursor += 1
            strings.append(bytes(value ^ (mask & 255) for value in data[cursor:cursor + length]).decode("utf-8"))
            cursor += length
        high, low, size = (struct.unpack_from(">I", data, cursor + offset)[0] ^ mask for offset in (0, 4, 8))
        cursor += 22
        start = (high << 32) | low
        if start + size > len(data):
            raise ValueError("Container entry is outside its file")
        entries[strings[0]] = bytes(value ^ (mask & 255) for value in data[start:start + size])
    with zipfile.ZipFile(io.BytesIO(entries["main.xml"])) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP CRC mismatch")
        return archive.read("contents"), entries


class ProjectFormatTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="recovery-fixture-")
        cls.path = Path(cls.directory.name) / "synthetic.cmo3"
        result = subprocess.run([str(FIXTURE), str(cls.path)], capture_output=True,
                                encoding="utf-8", errors="replace", timeout=60)
        if result.returncode or "PROJECT_FIXTURE_OK" not in result.stdout:
            raise AssertionError(result.stdout + result.stderr)
        cls.xml, cls.assets = contents(cls.path.read_bytes())
        cls.root = ET.fromstring(cls.xml)
        cls.identities = {element.get("xs.id"): element for element in cls.root.iter() if element.get("xs.id")}

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_shared_identity_and_references(self):
        definitions = [element.get("xs.id") for element in self.root.iter() if element.get("xs.id")]
        self.assertEqual(len(definitions), len(set(definitions)))
        self.assertEqual(len(definitions), len(self.root.find("shared")))
        for element in self.root.iter():
            if element.get("xs.ref"):
                self.assertEqual(element.tag, self.identities[element.get("xs.ref")].tag)

    def test_material_ownership_cycle(self):
        for mesh in self.root.findall("./shared/CArtMeshSource"):
            extension_ref = mesh.find("./ACDrawableSource/ACParameterControllableSource/carray_list/CTextureInputExtension")
            extension = self.identities[extension_ref.get("xs.ref")]
            self.assertEqual(extension.find("./ACExtension/CArtMeshSource").get("xs.ref"), mesh.get("xs.id"))
            texture_input_ref = extension.find("./carray_list/CTextureInput_ModelImage")
            texture_input = self.identities[texture_input_ref.get("xs.ref")]
            self.assertEqual(texture_input.find("./ACTextureInput/CTextureInputExtension").get("xs.ref"), extension.get("xs.id"))

    def test_first_grid_axis_varies_fastest(self):
        mesh = next(element for element in self.root.findall("./shared/CArtMeshSource")
                    if element.find("./ACDrawableSource/CDrawableId").get("idstr") == "SyntheticMesh")
        grid_reference = mesh.find("./ACDrawableSource/ACParameterControllableSource/KeyformGridSource")
        grid = self.identities[grid_reference.get("xs.ref")]
        indexes = [[int(value.text) for value in form.findall("./KeyformGridAccessKey/array_list/KeyOnParameter/i")]
                   for form in grid.findall("./array_list/KeyformOnGrid")]
        self.assertEqual(indexes, [[0, 0], [1, 0], [2, 0], [0, 1], [1, 1], [2, 1]])
        targets = mesh.findall("./ACDrawableSource/ACParameterControllableSource/KeyFormMorphTargetSet/carray_list/KeyFormMorphTarget")
        self.assertEqual(sorted(float(target.find("f").text) for target in targets), [-1, 1])

    def test_rotation_unit_conversion_once_per_chain(self):
        forms = self.root.findall("./shared/CRotationDeformerSource/carray_list/CRotationDeformerForm")
        self.assertEqual(len(forms), 2)
        for form in forms:
            self.assertAlmostEqual(float(form.get("scale")), 1, places=6)

    def test_effects_and_combined_parameters(self):
        effects = self.root.findall("./main/CModelSource/CModelInfo/CEffectParameterGroups/hash_map/entry")
        self.assertEqual({entry.find("EffectParameterType").get("v") for entry in effects}, {"EYE_BLINK", "LIPSYNC"})
        for entry in effects:
            self.assertEqual(len(entry.find("carray_list")), 1)
        parameters = self.root.findall("./main/CModelSource/CParameterSourceSet/carray_list/CParameterSource")
        combined = {parameter.find("CParameterId").get("idstr"):
                    parameter.find("b[@xs.n='combined']").text for parameter in parameters}
        self.assertEqual(combined, {"ParamMove": "true", "ParamTurn": "false", "ParamShape": "false"})

    def test_equal_draw_orders_preserve_interleaved_parts(self):
        parts = self.root.findall("./shared/CPartSource")
        meshes = self.root.findall("./shared/CArtMeshSource")
        by_guid = {}
        for part in parts:
            by_guid[part.find("CPartGuid").get("xs.ref")] = part
        for mesh in meshes:
            by_guid[mesh.find("./ACDrawableSource/CDrawableGuid").get("xs.ref")] = mesh
        root_part = next(part for part in parts if part.find("CPartId").get("idstr") == "__RootPart__")

        def render_sequence(part):
            result = []
            children = part.find("carray_list[@xs.n='_childGuids']")
            for child in reversed(children):
                source = by_guid.get(child.get("xs.ref"))
                if source is None:  # Deformers and glue do not draw a mesh themselves.
                    continue
                if source.tag == "CPartSource":
                    result.extend(render_sequence(source))
                else:
                    result.append(source.find("./ACDrawableSource/CDrawableId").get("idstr"))
            return result

        self.assertEqual(render_sequence(root_part), [
            "RootBefore", "PartBefore", "SyntheticMeshB", "SyntheticMesh", "PartAfter", "RootAfter"])

    def test_unrepresentable_part_order_is_refused(self):
        path = Path(self.directory.name) / "unrepresentable.cmo3"
        result = subprocess.run([str(FIXTURE), str(path), "--noncontiguous-parts"],
                                capture_output=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("NONCONTIGUOUS_PARTS_REFUSED", result.stdout)
        self.assertFalse(path.exists())

    def test_embedded_assets_and_escaped_names(self):
        self.assertEqual(set(self.assets), {"main.xml", "texture_0.png"})
        self.assertTrue(self.assets["texture_0.png"].startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(self.root.find("./main/CModelSource/s[@xs.n='name']").text, "Synthetic · 模型 <测试>")
        self.assertNotIn(self.directory.name.encode(), self.xml)
        self.assertNotIn(b"not-shipped-input.model3.json", self.xml)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    FIXTURE = Path(sys.argv.pop()).resolve()
    unittest.main()
