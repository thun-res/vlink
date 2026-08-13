#!/usr/bin/env python3
"""Tests for tools/autosar/arxml_to_vlink_someip.py."""

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from itertools import product
from pathlib import Path
from typing import Dict, List, Optional, Tuple


AUTOSAR_NAMESPACE = "http://autosar.org/schema/r4.0"


def package_xml(name: str, elements: str) -> str:
    return textwrap.dedent(
        f"""\
        <?xml version="1.0" encoding="UTF-8"?>
        <AUTOSAR xmlns="{AUTOSAR_NAMESPACE}">
          <AR-PACKAGES>
            <AR-PACKAGE>
              <SHORT-NAME>{name}</SHORT-NAME>
              <ELEMENTS>
                {textwrap.dedent(elements).strip()}
              </ELEMENTS>
            </AR-PACKAGE>
          </AR-PACKAGES>
        </AUTOSAR>
        """
    ).lstrip()


UINT8_BASE = """
<SW-BASE-TYPE>
  <SHORT-NAME>uint8</SHORT-NAME>
  <BASE-TYPE-SIZE>8</BASE-TYPE-SIZE>
  <BASE-TYPE-ENCODING>NONE</BASE-TYPE-ENCODING>
  <NATIVE-DECLARATION>uint8</NATIVE-DECLARATION>
</SW-BASE-TYPE>
"""


def someip_length_deployment_xml(
    array_width: int = 2,
    string_width: int = 1,
    struct_width: int = 2,
    override_target: str = "/Deploy/Payload/fixed",
) -> str:
    return package_xml(
        "Deploy",
        UINT8_BASE
        + f"""
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Child</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>title</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>history</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ARRAY-SIZE>8</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>fixed</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ARRAY-SIZE>2</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>dynamic</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ARRAY-SIZE>8</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>child</SHORT-NAME><CATEGORY>TYPE_REFERENCE</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <IMPLEMENTATION-DATA-TYPE-REF>/Deploy/Child</IMPLEMENTATION-DATA-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <SERVICE-INTERFACE>
          <SHORT-NAME>Service</SHORT-NAME><EVENTS><VARIABLE-DATA-PROTOTYPE>
            <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Deploy/Payload</TYPE-TREF>
          </VARIABLE-DATA-PROTOTYPE></EVENTS>
        </SERVICE-INTERFACE>
        <AP-SOMEIP-TRANSFORMATION-PROPS>
          <SHORT-NAME>Defaults</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>
          <SIZE-OF-ARRAY-LENGTH-FIELD>{array_width}</SIZE-OF-ARRAY-LENGTH-FIELD>
          <SIZE-OF-STRING-LENGTH-FIELD>{string_width}</SIZE-OF-STRING-LENGTH-FIELD>
          <SIZE-OF-STRUCT-LENGTH-FIELD>{struct_width}</SIZE-OF-STRUCT-LENGTH-FIELD>
        </AP-SOMEIP-TRANSFORMATION-PROPS>
        <AP-SOMEIP-TRANSFORMATION-PROPS>
          <SHORT-NAME>NoArrayLength</SHORT-NAME>
          <SIZE-OF-ARRAY-LENGTH-FIELD>0</SIZE-OF-ARRAY-LENGTH-FIELD>
        </AP-SOMEIP-TRANSFORMATION-PROPS>
        <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
          <SHORT-NAME>EventDefaults</SHORT-NAME>
          <EVENT-REFS><EVENT-REF>/Deploy/Service/Event</EVENT-REF></EVENT-REFS>
          <TRANSFORMATION-PROPS-REF>/Deploy/Defaults</TRANSFORMATION-PROPS-REF>
        </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
        <SOMEIP-DATA-PROTOTYPE-TRANSFORMATION-PROPS>
          <SHORT-NAME>FixedOverride</SHORT-NAME><DATA-PROTOTYPES>
            <DATA-PROTOTYPE-IN-SERVICE-INTERFACE-REF><ELEMENT-IN-IMPL-DATATYPE>
              <PORT-INTERFACE-REF>/Deploy/Service</PORT-INTERFACE-REF>
              <ROOT-DATA-PROTOTYPE-REF>/Deploy/Service/Event</ROOT-DATA-PROTOTYPE-REF>
              <TARGET-DATA-PROTOTYPE-REF>{override_target}</TARGET-DATA-PROTOTYPE-REF>
            </ELEMENT-IN-IMPL-DATATYPE></DATA-PROTOTYPE-IN-SERVICE-INTERFACE-REF>
          </DATA-PROTOTYPES>
          <SOMEIP-TRANSFORMATION-PROPS-REF>/Deploy/NoArrayLength</SOMEIP-TRANSFORMATION-PROPS-REF>
        </SOMEIP-DATA-PROTOTYPE-TRANSFORMATION-PROPS>
        """,
    )


def service_element_deployment_xml(service_elements: str, mapping_refs: str) -> str:
    return package_xml(
        "ServiceDeploy",
        """
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <SERVICE-INTERFACE>
          <SHORT-NAME>Service</SHORT-NAME>
        """
        + service_elements
        + """
        </SERVICE-INTERFACE>
        <AP-SOMEIP-TRANSFORMATION-PROPS>
          <SHORT-NAME>Defaults</SHORT-NAME>
          <SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>
          <SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>
        </AP-SOMEIP-TRANSFORMATION-PROPS>
        <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
          <SHORT-NAME>Mapping</SHORT-NAME>
        """
        + mapping_refs
        + """
          <TRANSFORMATION-PROPS-REF>/ServiceDeploy/Defaults</TRANSFORMATION-PROPS-REF>
        </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
        """,
    )


def nested_alias_deployment_xml(array_width: int = 2, maximum: Optional[int] = None) -> str:
    maximum_xml = f"<ARRAY-SIZE>{maximum}</ARRAY-SIZE>" if maximum is not None else ""
    return package_xml(
        "Alias",
        UINT8_BASE
        + f"""
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Leaf</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              {maximum_xml}<ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
              <BASE-TYPE-REF>/Alias/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Plane</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>element</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
              <ARRAY-SIZE>2</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              <IMPLEMENTATION-DATA-TYPE-REF>/Alias/Leaf</IMPLEMENTATION-DATA-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Cube</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>element</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
              <ARRAY-SIZE>2</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              <IMPLEMENTATION-DATA-TYPE-REF>/Alias/Plane</IMPLEMENTATION-DATA-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>values</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
              <IMPLEMENTATION-DATA-TYPE-REF>/Alias/Cube</IMPLEMENTATION-DATA-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
          <VARIABLE-DATA-PROTOTYPE>
            <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Alias/Payload</TYPE-TREF>
          </VARIABLE-DATA-PROTOTYPE>
        </EVENTS></SERVICE-INTERFACE>
        <AP-SOMEIP-TRANSFORMATION-PROPS>
          <SHORT-NAME>Deployment</SHORT-NAME>
          <SIZE-OF-ARRAY-LENGTH-FIELD>{array_width}</SIZE-OF-ARRAY-LENGTH-FIELD>
        </AP-SOMEIP-TRANSFORMATION-PROPS>
        <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
          <SHORT-NAME>Mapping</SHORT-NAME>
          <EVENT-REFS><EVENT-REF>/Alias/Service/Event</EVENT-REF></EVENT-REFS>
          <TRANSFORMATION-PROPS-REF>/Alias/Deployment</TRANSFORMATION-PROPS-REF>
        </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
        """,
    )


class ArxmlToVlinkSomeipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        test_dir = Path(__file__).resolve().parent
        autosar_dir = test_dir.parent
        cls.repo_root = autosar_dir.parents[1]
        cls.tool = autosar_dir / "arxml_to_vlink_someip.py"
        cls.fixture = test_dir / "autosar_r25_11_someip_types.arxml"
        cls.features_fixture = test_dir / "autosar_r25_11_someip_features.arxml"
        cls.matrix_fixture = test_dir / "autosar_r25_11_someip_matrix.arxml"
        cls.advanced_fixture = test_dir / "autosar_r25_11_someip_advanced.arxml"
        cls.golden = test_dir / "generated_someip_types.h"
        cls.features_golden = test_dir / "generated_someip_features.h"
        cls.matrix_golden = test_dir / "generated_someip_matrix.h"
        cls.advanced_golden = test_dir / "generated_someip_advanced.h"

    def run_tool(self, files: Dict[str, str], *arguments: str) -> Tuple[subprocess.CompletedProcess, Path]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        paths: List[Path] = []
        for name, content in files.items():
            path = directory / name
            path.write_text(content, encoding="utf-8")
            paths.append(path)

        command = [sys.executable, str(self.tool), *(str(path) for path in paths), *arguments]
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        return result, directory

    @staticmethod
    def find_cpp_compiler() -> Optional[str]:
        return (
            os.environ.get("CXX")
            or shutil.which("c++")
            or shutil.which("cl")
            or shutil.which("clang-cl")
        )

    def find_autosar_schema(self) -> Optional[Path]:
        schema_value = os.environ.get("AUTOSAR_R25_11_XSD")
        if schema_value:
            schema = Path(schema_value)
            self.assertTrue(schema.is_file(), f"AUTOSAR_R25_11_XSD does not exist: {schema}")
            return schema
        candidate = (
            self.repo_root
            / "build-ai"
            / "arxml_someip_length"
            / "schema"
            / "extracted"
            / "AUTOSAR_00054.xsd"
        )
        return candidate if candidate.is_file() else None

    def compile_cpp(self, compiler: str, source: Path, *includes: Path) -> subprocess.CompletedProcess:
        compiler_path = compiler.strip('"')
        command = (
            [compiler_path]
            if Path(compiler_path).is_file()
            else shlex.split(compiler, posix=os.name != "nt")
        )
        command[0] = command[0].strip('"')
        executable = Path(command[0]).name.lower()
        if executable in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}:
            command.extend(("/nologo", "/std:c++17", "/Zs"))
            command.extend(f"/I{include}" for include in includes)
        else:
            command.extend(("-std=c++17", "-fsyntax-only"))
            for include in includes:
                command.extend(("-I", str(include)))
        command.append(str(source))
        return subprocess.run(command, check=False, capture_output=True, text=True)

    def compile_and_run_cpp(
        self, compiler: str, source: Path, linker_file: Path, runtime_dir: Path, *includes: Path
    ) -> Tuple[subprocess.CompletedProcess, Optional[subprocess.CompletedProcess]]:
        compiler_path = compiler.strip('"')
        command = (
            [compiler_path]
            if Path(compiler_path).is_file()
            else shlex.split(compiler, posix=os.name != "nt")
        )
        command[0] = command[0].strip('"')
        executable_name = Path(command[0]).name.lower()
        output = source.with_suffix(".exe" if os.name == "nt" else "")
        if executable_name in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}:
            command.extend(("/nologo", "/std:c++17", "/EHsc"))
            command.extend(f"/I{include}" for include in includes)
            command.extend((str(source), str(linker_file), f"/Fe:{output}"))
        else:
            command.append("-std=c++17")
            for include in includes:
                command.extend(("-I", str(include)))
            command.extend((str(source), str(linker_file), f"-Wl,-rpath,{runtime_dir}", "-o", str(output)))

        compiled = subprocess.run(command, check=False, capture_output=True, text=True)
        if compiled.returncode != 0:
            return compiled, None

        environment = os.environ.copy()
        if os.name == "nt":
            library_path = "PATH"
        elif sys.platform == "darwin":
            library_path = "DYLD_LIBRARY_PATH"
        else:
            library_path = "LD_LIBRARY_PATH"
        environment[library_path] = str(runtime_dir) + os.pathsep + environment.get(library_path, "")
        executed = subprocess.run(
            [str(output)], check=False, capture_output=True, text=True, env=environment
        )
        return compiled, executed

    def test_generates_scalars_enum_arrays_nested_types_and_bytes(self) -> None:
        arxml = package_xml(
            "Types",
            UINT8_BASE
            + """
            <SW-BASE-TYPE>
              <SHORT-NAME>uint16</SHORT-NAME>
              <BASE-TYPE-SIZE>16</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>NONE</BASE-TYPE-ENCODING>
              <NATIVE-DECLARATION>uint16</NATIVE-DECLARATION>
            </SW-BASE-TYPE>
            <SW-BASE-TYPE>
              <SHORT-NAME>sint32</SHORT-NAME>
              <BASE-TYPE-SIZE>32</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>2C</BASE-TYPE-ENCODING>
            </SW-BASE-TYPE>
            <COMPU-METHOD>
              <SHORT-NAME>ModeCompu</SHORT-NAME>
              <COMPU-INTERNAL-TO-PHYS>
                <COMPU-SCALES>
                  <COMPU-SCALE>
                    <LOWER-LIMIT>0</LOWER-LIMIT>
                    <UPPER-LIMIT>0</UPPER-LIMIT>
                    <COMPU-CONST><VT>off</VT></COMPU-CONST>
                  </COMPU-SCALE>
                  <COMPU-SCALE>
                    <LOWER-LIMIT>1</LOWER-LIMIT>
                    <UPPER-LIMIT>1</UPPER-LIMIT>
                    <COMPU-CONST><VT>active</VT></COMPU-CONST>
                  </COMPU-SCALE>
                </COMPU-SCALES>
              </COMPU-INTERNAL-TO-PHYS>
            </COMPU-METHOD>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Mode</SHORT-NAME>
              <CATEGORY>VALUE</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Types/uint16</BASE-TYPE-REF>
                <COMPU-METHOD-REF DEST="COMPU-METHOD">/Types/ModeCompu</COMPU-METHOD-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>FixedSamples</SHORT-NAME>
              <CATEGORY>ARRAY</CATEGORY>
              <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                <ARRAY-SIZE>3</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                  <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Types/uint16</BASE-TYPE-REF>
                </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Message</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>mode</SHORT-NAME><CATEGORY>TYPE_REFERENCE</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <IMPLEMENTATION-DATA-TYPE-REF>/Types/Mode</IMPLEMENTATION-DATA-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>samples</SHORT-NAME><CATEGORY>TYPE_REFERENCE</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <IMPLEMENTATION-DATA-TYPE-REF>/Types/FixedSamples</IMPLEMENTATION-DATA-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>payload</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                  <ARRAY-SIZE>4096</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF>/Types/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>label</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-TEXT-PROPS><ENCODING>UTF-8</ENCODING></SW-TEXT-PROPS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool(
            {"types.arxml": arxml},
            "--type",
            "Message",
            "--namespace",
            "vehicle::someip",
            "--byte-arrays-as-bytes",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("namespace vehicle::someip {", result.stdout)
        self.assertIn("// AUTOSAR type: /Types/Mode.", result.stdout)
        self.assertIn("enum class Mode : uint16_t {", result.stdout)
        self.assertIn("kOff = 0,", result.stdout)
        self.assertIn("kActive = 1,\n};", result.stdout)
        self.assertIn("// AUTOSAR type: /Types/FixedSamples.", result.stdout)
        self.assertIn("using FixedSamples = std::array<uint16_t, 3>;", result.stdout)
        self.assertIn("// AUTOSAR type: /Types/Message.", result.stdout)
        self.assertIn("vlink::Bytes payload{};", result.stdout)
        self.assertIn("std::string label{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(payload, 4U, 4096U)", result.stdout)

    def test_resolves_cross_file_application_mapping(self) -> None:
        implementation = package_xml(
            "Impl",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>ImplMessage</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <SHORT-NAME>value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                  <BASE-TYPE-REF>/Impl/uint8</BASE-TYPE-REF>
                </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        application = package_xml(
            "App",
            """
            <APPLICATION-RECORD-DATA-TYPE>
              <SHORT-NAME>AppMessage</SHORT-NAME>
              <ELEMENTS><APPLICATION-RECORD-ELEMENT>
                <SHORT-NAME>unusedModelField</SHORT-NAME>
                <TYPE-TREF>/Missing/ModelType</TYPE-TREF>
              </APPLICATION-RECORD-ELEMENT></ELEMENTS>
            </APPLICATION-RECORD-DATA-TYPE>
            <DATA-TYPE-MAPPING-SET>
              <SHORT-NAME>Mappings</SHORT-NAME><DATA-TYPE-MAPS><DATA-TYPE-MAPPING>
                <APPLICATION-DATA-TYPE-REF>/App/AppMessage</APPLICATION-DATA-TYPE-REF>
                <IMPLEMENTATION-DATA-TYPE-REF>/Impl/ImplMessage</IMPLEMENTATION-DATA-TYPE-REF>
              </DATA-TYPE-MAPPING></DATA-TYPE-MAPS>
            </DATA-TYPE-MAPPING-SET>
            """,
        ).replace(AUTOSAR_NAMESPACE, "http://autosar.org/3.2.2")
        result, _ = self.run_tool(
            {"implementation.arxml": implementation, "application.arxml": application},
            "--type",
            "/App/AppMessage",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("// Sources: application.arxml, implementation.arxml", result.stdout)
        self.assertIn("struct ImplMessage final {", result.stdout)
        self.assertIn("using AppMessage = ImplMessage;", result.stdout)
        self.assertNotIn("unusedModelField", result.stdout)

    def test_rejects_non_struct_top_level_prototype_without_deployment(self) -> None:
        arxml = package_xml(
            "Scalar",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Counter</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint32</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE>
                <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Scalar/Counter</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            """,
        )
        result, _ = self.run_tool(
            {"scalar.arxml": arxml}, "--prototype", "/Scalar/Service/Event"
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("top-level SOME/IP payload must resolve to a structure", result.stderr)

    def test_rejects_port_interface_scoped_type_mapping(self) -> None:
        mappings = package_xml(
            "Scoped",
            """
            <DATA-TYPE-MAPPING-SET>
              <SHORT-NAME>Mappings</SHORT-NAME>
            </DATA-TYPE-MAPPING-SET>
            """,
        )
        scoped = textwrap.dedent(
            f"""\
            <?xml version="1.0" encoding="UTF-8"?>
            <PORT-INTERFACE-TO-DATA-TYPE-MAPPING xmlns="{AUTOSAR_NAMESPACE}">
              <SHORT-NAME>ServiceMappings</SHORT-NAME>
              <DATA-TYPE-MAPPING-SET-REFS>
                <DATA-TYPE-MAPPING-SET-REF>/Scoped/Mappings</DATA-TYPE-MAPPING-SET-REF>
              </DATA-TYPE-MAPPING-SET-REFS>
              <PORT-INTERFACE-REF>/Scoped/Service</PORT-INTERFACE-REF>
            </PORT-INTERFACE-TO-DATA-TYPE-MAPPING>
            """
        ).lstrip()
        result, _ = self.run_tool(
            {"mappings.arxml": mappings, "scoped_mapping.arxml": scoped}
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("PORT-INTERFACE-TO-DATA-TYPE-MAPPING scope is not supported", result.stderr)

    def test_indexes_ar_packages_and_fragment_roots(self) -> None:
        wrapped = package_xml(
            "Roots",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Counter</SHORT-NAME><CATEGORY>UINT32</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        packages_start = wrapped.index("<AR-PACKAGES>")
        packages_end = wrapped.index("</AR-PACKAGES>") + len("</AR-PACKAGES>")
        fragment = """
        <APPLICATION-PRIMITIVE-DATA-TYPE>
          <SHORT-NAME>FragmentValue</SHORT-NAME><CATEGORY>SINT16</CATEGORY>
        </APPLICATION-PRIMITIVE-DATA-TYPE>
        """
        cases = (
            ("packages.arxml", wrapped[packages_start:packages_end], "/Roots/Counter", "uint32_t"),
            ("fragment.arxml", textwrap.dedent(fragment), "/FragmentValue", "int16_t"),
        )

        for name, arxml, reference, cpp_type in cases:
            with self.subTest(root=name):
                result, _ = self.run_tool({name: arxml}, "--type", reference)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"using {reference.rsplit('/', 1)[-1]} = {cpp_type};", result.stdout)

    def test_generates_adaptive_cpp_implementation_types(self) -> None:
        arxml = package_xml(
            "Adaptive",
            """
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>uint16_t</SHORT-NAME><CATEGORY>VALUE</CATEGORY><HEADER-FILE>cstdint</HEADER-FILE>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>bool</SHORT-NAME><CATEGORY>VALUE</CATEGORY><TYPE-EMITTER>FUNDAMENTAL_TYPE</TYPE-EMITTER>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>float</SHORT-NAME><CATEGORY>VALUE</CATEGORY><TYPE-EMITTER>FUNDAMENTAL_TYPE</TYPE-EMITTER>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>String</SHORT-NAME><CATEGORY>STRING</CATEGORY><HEADER-FILE>ara/core/string.h</HEADER-FILE>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Samples</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><ARRAY-SIZE>3</ARRAY-SIZE>
              <TEMPLATE-ARGUMENTS><CPP-TEMPLATE-ARGUMENT>
                <TEMPLATE-TYPE-REF DEST="STD-CPP-IMPLEMENTATION-DATA-TYPE">
                  /Adaptive/uint16_t
                </TEMPLATE-TYPE-REF>
              </CPP-TEMPLATE-ARGUMENT></TEMPLATE-ARGUMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>History</SHORT-NAME><CATEGORY>VECTOR</CATEGORY>
              <TEMPLATE-ARGUMENTS><CPP-TEMPLATE-ARGUMENT>
                <TEMPLATE-TYPE-REF DEST="STD-CPP-IMPLEMENTATION-DATA-TYPE">
                  /Adaptive/uint16_t
                </TEMPLATE-TYPE-REF>
              </CPP-TEMPLATE-ARGUMENT></TEMPLATE-ARGUMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>AdaptiveState</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>samples</SHORT-NAME><TYPE-REFERENCE><TYPE-REFERENCE-REF>
                    /Adaptive/Samples
                  </TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>history</SHORT-NAME><TYPE-REFERENCE><TYPE-REFERENCE-REF>
                    /Adaptive/History
                  </TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>name</SHORT-NAME><TYPE-REFERENCE><TYPE-REFERENCE-REF>
                    /Adaptive/String
                  </TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>valid</SHORT-NAME><TYPE-REFERENCE><TYPE-REFERENCE-REF>
                    /Adaptive/bool
                  </TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>temperature</SHORT-NAME><TYPE-REFERENCE><TYPE-REFERENCE-REF>
                    /Adaptive/float
                  </TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <SOMEIP-TRANSFORMATION-DESCRIPTION>
              <SHORT-NAME>SomeipTransformation</SHORT-NAME><ALIGNMENT>32</ALIGNMENT>
              <BYTE-ORDER>MOST-SIGNIFICANT-BYTE-FIRST</BYTE-ORDER>
            </SOMEIP-TRANSFORMATION-DESCRIPTION>
            """,
        )
        result, directory = self.run_tool({"adaptive.arxml": arxml}, "--type", "AdaptiveState")
        all_types, _ = self.run_tool({"adaptive.arxml": arxml})

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(all_types.returncode, 0, all_types.stderr)
        self.assertIn("using uint16_t_ = uint16_t;", all_types.stdout)
        self.assertNotIn("using uint16_t = uint16_t;", all_types.stdout)
        self.assertIn("using Samples = std::array<uint16_t, 3>;", result.stdout)
        self.assertIn("using History = std::vector<uint16_t>;", result.stdout)
        self.assertIn("using String = std::string;", result.stdout)
        self.assertNotIn("VLINK_SOMEIP_ALIGNMENT", result.stdout)
        self.assertIn("bool valid{};", result.stdout)
        self.assertIn("float temperature{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIELDS(samples, history, name, valid, temperature)", result.stdout)
        self.assertLess(
            result.stdout.index("float temperature{};"),
            result.stdout.index("VLINK_SOMEIP_FIELDS(samples, history, name, valid, temperature)"),
        )

        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "adaptive_generated.h"
            source = directory / "adaptive_generated.cc"
            header.write_text(all_types.stdout, encoding="utf-8")
            source.write_text(
                '#include "adaptive_generated.h"\n'
                "static_assert(vlink::Serializer::get_type_of<AdaptiveState>() == "
                "vlink::Serializer::kSomeipType);\n",
                encoding="utf-8",
            )
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_generates_application_value_custom_cpp_and_parameter_prototype(self) -> None:
        arxml = package_xml(
            "Formats",
            UINT8_BASE
            + """
            <APPLICATION-VALUE-DATA-TYPE>
              <SHORT-NAME>ApplicationValue</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Formats/uint8</BASE-TYPE-REF>
            </APPLICATION-VALUE-DATA-TYPE>
            <CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>CustomPayload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>value</SHORT-NAME>
                  <TYPE-REFERENCE-REF>/Formats/ApplicationValue</TYPE-REFERENCE-REF>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE>
            <PARAMETER-DATA-PROTOTYPE>
              <SHORT-NAME>DefaultParameter</SHORT-NAME><TYPE-TREF>/Formats/CustomPayload</TYPE-TREF>
              <INIT-VALUE><RECORD-VALUE-SPECIFICATION><FIELDS>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>9</VALUE></NUMERICAL-VALUE-SPECIFICATION>
              </FIELDS></RECORD-VALUE-SPECIFICATION></INIT-VALUE>
            </PARAMETER-DATA-PROTOTYPE>
            """,
        )
        result, _ = self.run_tool(
            {"formats.arxml": arxml}, "--prototype", "/Formats/DefaultParameter"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using ApplicationValue = uint8_t;", result.stdout)
        self.assertIn("struct CustomPayload final {", result.stdout)
        self.assertIn("ApplicationValue value{};", result.stdout)
        self.assertIn("[[nodiscard]] static CustomPayload make_default()", result.stdout)
        self.assertIn("static_cast<uint8_t>(9U)", result.stdout)

    def test_rejects_optional_adaptive_member_without_tlv_deployment(self) -> None:
        arxml = package_xml(
            "Adaptive",
            """
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>uint8_t</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>OptionalState</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>state</SHORT-NAME><IS-OPTIONAL>true</IS-OPTIONAL>
                  <TYPE-REFERENCE><TYPE-REFERENCE-REF>/Adaptive/uint8_t</TYPE-REFERENCE-REF></TYPE-REFERENCE>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"optional.arxml": arxml}, "--type", "OptionalState")

        self.assertEqual(result.returncode, 2)
        self.assertIn("optional members require a TLV data-ID deployment", result.stderr)

    def test_generates_dynamic_and_static_tlv_optional_members(self) -> None:
        arxml = package_xml(
            "Adaptive",
            UINT8_BASE
            + """
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>uint16_t</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>uint32_t</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Lookup</SHORT-NAME><CATEGORY>ASSOCIATIVE_MAP</CATEGORY>
              <TEMPLATE-ARGUMENTS>
                <CPP-TEMPLATE-ARGUMENT>
                  <CATEGORY>ASSOC_MAP_VALUE</CATEGORY><TEMPLATE-TYPE-REF>/Adaptive/uint16_t</TEMPLATE-TYPE-REF>
                </CPP-TEMPLATE-ARGUMENT>
                <CPP-TEMPLATE-ARGUMENT>
                  <CATEGORY>ASSOC_MAP_KEY</CATEGORY><TEMPLATE-TYPE-REF>/Adaptive/uint32_t</TEMPLATE-TYPE-REF>
                </CPP-TEMPLATE-ARGUMENT>
              </TEMPLATE-ARGUMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>OptionalState</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>label</SHORT-NAME><CATEGORY>STRING</CATEGORY><IS-OPTIONAL>true</IS-OPTIONAL>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>code</SHORT-NAME><BASE-TYPE-REF>/Adaptive/uint8</BASE-TYPE-REF>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>lookup</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY><IS-OPTIONAL>true</IS-OPTIONAL>
                  <TYPE-REFERENCE-REF>/Adaptive/Lookup</TYPE-REFERENCE-REF>
                </CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE>
                <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Adaptive/OptionalState</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>Defaults</SHORT-NAME>
              <SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>
              <SIZE-OF-STRING-LENGTH-FIELD>2</SIZE-OF-STRING-LENGTH-FIELD>
              <SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>
              <SIZE-OF-UNION-LENGTH-FIELD>2</SIZE-OF-UNION-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TLV-DATA-ID-DEFINITION-SET>
              <SHORT-NAME>DataIds</SHORT-NAME><TLV-DATA-ID-DEFINITIONS>
                <TLV-DATA-ID-DEFINITION>
                  <ID>1</ID><TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/Adaptive/OptionalState/label</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
                </TLV-DATA-ID-DEFINITION>
                <TLV-DATA-ID-DEFINITION>
                  <ID>2</ID><TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/Adaptive/OptionalState/code</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
                </TLV-DATA-ID-DEFINITION>
                <TLV-DATA-ID-DEFINITION>
                  <ID>3</ID><TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/Adaptive/OptionalState/lookup</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
                </TLV-DATA-ID-DEFINITION>
              </TLV-DATA-ID-DEFINITIONS>
            </TLV-DATA-ID-DEFINITION-SET>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>Mapping</SHORT-NAME>
              <EVENT-REFS><EVENT-REF>/Adaptive/Service/Event</EVENT-REF></EVENT-REFS>
              <TLV-DATA-ID-DEFINITION-REFS>
                <TLV-DATA-ID-DEFINITION-REF>/Adaptive/DataIds</TLV-DATA-ID-DEFINITION-REF>
              </TLV-DATA-ID-DEFINITION-REFS>
              <TRANSFORMATION-PROPS-REF>/Adaptive/Defaults</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, directory = self.run_tool(
            {"optional.arxml": arxml}, "--prototype", "/Adaptive/Service/Event", "--strict"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("std::optional<std::string> label{};", result.stdout)
        self.assertIn("std::optional<Lookup> lookup{};", result.stdout)
        self.assertIn("SOME/IP deployment: TLV data ID 1, optional, dynamic length", result.stdout)
        self.assertIn("SOME/IP deployment: TLV data ID 2, fixed-width wire type", result.stdout)
        self.assertIn("VLINK_SOMEIP_STRUCT_LENGTH(2U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_LENGTH(1, label, 2U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_LENGTH(2, code, 2U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_LENGTH(3, lookup, 2U)", result.stdout)
        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "tlv_generated.h"
            source = directory / "tlv_generated.cc"
            header.write_text(result.stdout, encoding="utf-8")
            source.write_text('#include "tlv_generated.h"\n', encoding="utf-8")
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

        static = arxml.replace(
            "<SHORT-NAME>Defaults</SHORT-NAME>",
            "<SHORT-NAME>Defaults</SHORT-NAME>"
            "<IS-DYNAMIC-LENGTH-FIELD-SIZE>false</IS-DYNAMIC-LENGTH-FIELD-SIZE>",
            1,
        )
        static_result, _ = self.run_tool(
            {"static.arxml": static}, "--prototype", "/Adaptive/Service/Event"
        )
        self.assertEqual(static_result.returncode, 0, static_result.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_STATIC_LENGTH(1, label, 2U)", static_result.stdout)

        mixed = arxml.replace(
            "</VARIABLE-DATA-PROTOTYPE>\n            </EVENTS>",
            "</VARIABLE-DATA-PROTOTYPE>"
            "<VARIABLE-DATA-PROTOTYPE><SHORT-NAME>StaticEvent</SHORT-NAME>"
            "<TYPE-TREF>/Adaptive/OptionalState</TYPE-TREF></VARIABLE-DATA-PROTOTYPE>\n            </EVENTS>",
            1,
        ).replace(
            "</AP-SOMEIP-TRANSFORMATION-PROPS>",
            "</AP-SOMEIP-TRANSFORMATION-PROPS>"
            "<AP-SOMEIP-TRANSFORMATION-PROPS><SHORT-NAME>StaticDefaults</SHORT-NAME>"
            "<IS-DYNAMIC-LENGTH-FIELD-SIZE>false</IS-DYNAMIC-LENGTH-FIELD-SIZE>"
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>"
            "<SIZE-OF-STRING-LENGTH-FIELD>2</SIZE-OF-STRING-LENGTH-FIELD>"
            "<SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>"
            "<SIZE-OF-UNION-LENGTH-FIELD>2</SIZE-OF-UNION-LENGTH-FIELD>"
            "</AP-SOMEIP-TRANSFORMATION-PROPS>",
            1,
        ).replace(
            "</TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>",
            "</TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>"
            "<TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>"
            "<SHORT-NAME>StaticMapping</SHORT-NAME><EVENT-REFS>"
            "<EVENT-REF>/Adaptive/Service/StaticEvent</EVENT-REF></EVENT-REFS>"
            "<TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/Adaptive/DataIds"
            "</TLV-DATA-ID-DEFINITION-REF></TLV-DATA-ID-DEFINITION-REFS>"
            "<TRANSFORMATION-PROPS-REF>/Adaptive/StaticDefaults</TRANSFORMATION-PROPS-REF>"
            "</TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>",
            1,
        )
        for prototypes in (
            ("/Adaptive/Service/Event", "/Adaptive/Service/StaticEvent"),
            ("/Adaptive/Service/StaticEvent", "/Adaptive/Service/Event"),
        ):
            with self.subTest(prototypes=prototypes):
                conflict, _ = self.run_tool(
                    {"mixed.arxml": mixed},
                    "--prototype",
                    prototypes[0],
                    "--prototype",
                    prototypes[1],
                )
                self.assertEqual(conflict.returncode, 2)
                self.assertIn("conflicting TLV modes across service elements", conflict.stderr)

    def test_generates_nested_tlv_structures_with_scoped_reused_ids(self) -> None:
        arxml = package_xml(
            "NestedTlv",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Child</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>code</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/NestedTlv/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Root</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>child</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <TYPE-REFERENCE-REF>/NestedTlv/Child</TYPE-REFERENCE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>inline</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
                  <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                    <SHORT-NAME>code</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                    <BASE-TYPE-REF>/NestedTlv/uint8</BASE-TYPE-REF>
                  </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>RootAlias</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
              <IMPLEMENTATION-DATA-TYPE-REF>/NestedTlv/Root</IMPLEMENTATION-DATA-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE><SHORT-NAME>Event</SHORT-NAME>
                <TYPE-TREF>/NestedTlv/RootAlias</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS><SHORT-NAME>Deployment</SHORT-NAME>
              <SIZE-OF-ARRAY-LENGTH-FIELD>1</SIZE-OF-ARRAY-LENGTH-FIELD>
              <SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>
              <SIZE-OF-STRUCT-LENGTH-FIELD>1</SIZE-OF-STRUCT-LENGTH-FIELD>
              <SIZE-OF-UNION-LENGTH-FIELD>1</SIZE-OF-UNION-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TLV-DATA-ID-DEFINITION-SET><SHORT-NAME>DataIds</SHORT-NAME><TLV-DATA-ID-DEFINITIONS>
              <TLV-DATA-ID-DEFINITION><ID>1</ID>
                <TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/NestedTlv/Root/child</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
              <TLV-DATA-ID-DEFINITION><ID>1</ID>
                <TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/NestedTlv/Child/code</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
              <TLV-DATA-ID-DEFINITION><ID>2</ID>
                <TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/NestedTlv/Root/inline</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
              <TLV-DATA-ID-DEFINITION><ID>1</ID>
                <TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>/NestedTlv/Root/inline/code</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
            </TLV-DATA-ID-DEFINITIONS></TLV-DATA-ID-DEFINITION-SET>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>Mapping</SHORT-NAME><EVENT-REFS>
                <EVENT-REF>/NestedTlv/Service/Event</EVENT-REF>
              </EVENT-REFS>
              <TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/NestedTlv/DataIds</TLV-DATA-ID-DEFINITION-REF>
              </TLV-DATA-ID-DEFINITION-REFS>
              <TRANSFORMATION-PROPS-REF>/NestedTlv/Deployment</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool(
            {"nested_tlv.arxml": arxml}, "--prototype", "/NestedTlv/Service/Event", "--strict"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("VLINK_SOMEIP_TLV_LENGTH("), 4)

    def test_infers_standard_scalar_matrix_and_generates_application_record(self) -> None:
        scalar_types = {
            "u8": ("uint8", "uint8_t"),
            "u16": ("uint16", "uint16_t"),
            "u32": ("uint32", "uint32_t"),
            "u64": ("uint64", "uint64_t"),
            "s8": ("sint8", "int8_t"),
            "s16": ("sint16", "int16_t"),
            "s32": ("sint32", "int32_t"),
            "s64": ("sint64", "int64_t"),
            "f32": ("float32", "float"),
            "f64": ("float64", "double"),
            "valid": ("boolean", "bool"),
        }
        fields = "\n".join(
            textwrap.dedent(
                f"""\
                <APPLICATION-RECORD-ELEMENT>
                  <SHORT-NAME>{name}</SHORT-NAME>
                  <TYPE-TREF>/AUTOSAR/PlatformTypes/{autosar_type}</TYPE-TREF>
                </APPLICATION-RECORD-ELEMENT>
                """
            )
            for name, (autosar_type, _) in scalar_types.items()
        )
        arxml = package_xml(
            "App",
            f"""
            <APPLICATION-RECORD-DATA-TYPE>
              <SHORT-NAME>Position</SHORT-NAME><ELEMENTS>
                {fields}
              </ELEMENTS>
            </APPLICATION-RECORD-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"app.arxml": arxml}, "--type", "Position")

        self.assertEqual(result.returncode, 0, result.stderr)
        for name, (autosar_type, cpp_type) in scalar_types.items():
            self.assertIn(f"{cpp_type} {name}{{}};", result.stdout)
            self.assertIn(f"missing standard type '/AUTOSAR/PlatformTypes/{autosar_type}'", result.stderr)

    def test_generates_official_application_array_element(self) -> None:
        arxml = package_xml(
            "App",
            """
            <APPLICATION-ARRAY-DATA-TYPE>
              <SHORT-NAME>FixedValues</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ELEMENT>
                <SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                <TYPE-TREF DEST="APPLICATION-PRIMITIVE-DATA-TYPE">
                  /AUTOSAR/PlatformTypes/uint16
                </TYPE-TREF>
                <ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                <MAX-NUMBER-OF-ELEMENTS>3</MAX-NUMBER-OF-ELEMENTS>
              </ELEMENT>
            </APPLICATION-ARRAY-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"application_array.arxml": arxml}, "--type", "FixedValues")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using FixedValues = std::array<uint16_t, 3>;", result.stdout)

    def test_validates_application_array_size_semantics(self) -> None:
        missing_size = package_xml(
            "App",
            """
            <APPLICATION-ARRAY-DATA-TYPE>
              <SHORT-NAME>Broken</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ELEMENT>
                <SHORT-NAME>element</SHORT-NAME>
                <TYPE-TREF>/AUTOSAR/PlatformTypes/uint8</TYPE-TREF>
                <ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              </ELEMENT>
            </APPLICATION-ARRAY-DATA-TYPE>
            """,
        )
        invalid_semantics = missing_size.replace(
            "FIXED-SIZE", "VENDOR-SPECIFIC"
        ).replace("</ELEMENT>", "<MAX-NUMBER-OF-ELEMENTS>3</MAX-NUMBER-OF-ELEMENTS></ELEMENT>")

        missing, _ = self.run_tool({"missing.arxml": missing_size}, "--type", "Broken")
        invalid, _ = self.run_tool({"invalid.arxml": invalid_semantics}, "--type", "Broken")

        self.assertEqual(missing.returncode, 2)
        self.assertIn("fixed-size array has no", missing.stderr)
        self.assertEqual(invalid.returncode, 2)
        self.assertIn("unsupported ARRAY-SIZE-SEMANTICS", invalid.stderr)

    def test_variable_array_initial_value_uses_size_as_maximum(self) -> None:
        arxml = package_xml(
            "Init",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>History</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <ARRAY-SIZE>3</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF>/Init/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>HistoryValue</SHORT-NAME>
              <TYPE-TREF>/Init/History</TYPE-TREF>
              <INIT-VALUE><ARRAY-VALUE-SPECIFICATION><ELEMENTS>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>1</VALUE></NUMERICAL-VALUE-SPECIFICATION>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>2</VALUE></NUMERICAL-VALUE-SPECIFICATION>
              </ELEMENTS></ARRAY-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>
            """,
        )
        accepted, _ = self.run_tool({"accepted.arxml": arxml}, "--prototype", "HistoryValue")
        too_many_xml = arxml.replace(
            "</ELEMENTS></ARRAY-VALUE-SPECIFICATION>",
            """<NUMERICAL-VALUE-SPECIFICATION><VALUE>3</VALUE></NUMERICAL-VALUE-SPECIFICATION>
               <NUMERICAL-VALUE-SPECIFICATION><VALUE>4</VALUE></NUMERICAL-VALUE-SPECIFICATION>
               </ELEMENTS></ARRAY-VALUE-SPECIFICATION>""",
        )
        rejected, _ = self.run_tool({"rejected.arxml": too_many_xml}, "--prototype", "HistoryValue")

        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("std::vector<uint8_t>{", accepted.stdout)
        self.assertIn("// AUTOSAR INIT-VALUE source: /Init/HistoryValue.", accepted.stdout)
        self.assertIn("inline History make_history_value_initial_value()", accepted.stdout)
        self.assertNotIn("make_default", accepted.stdout)
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("maximum is 3", rejected.stderr)

    def test_struct_initial_value_uses_static_make_default_and_rejects_ambiguity(self) -> None:
        arxml = package_xml(
            "Init",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Init/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>Defaults</SHORT-NAME><TYPE-TREF>/Init/Payload</TYPE-TREF>
              <INIT-VALUE><RECORD-VALUE-SPECIFICATION><FIELDS>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>7</VALUE></NUMERICAL-VALUE-SPECIFICATION>
              </FIELDS></RECORD-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>
            """,
        )
        generated, directory = self.run_tool({"default.arxml": arxml}, "--prototype", "Defaults")

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertIn("uint8_t value{};", generated.stdout)
        self.assertIn("// AUTOSAR INIT-VALUE source: /Init/Defaults.", generated.stdout)
        self.assertIn("[[nodiscard]] static Payload make_default()", generated.stdout)
        self.assertIn("return Payload{static_cast<uint8_t>(7U)};", generated.stdout)
        self.assertNotIn("inline Payload make_", generated.stdout)

        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "default.h"
            source = directory / "default.cc"
            header.write_text(generated.stdout, encoding="utf-8")
            source.write_text(
                '#include "default.h"\n'
                "void use_default() {\n"
                "  Payload plain{};\n"
                "  auto configured = Payload::make_default();\n"
                "  (void)plain;\n"
                "  (void)configured;\n"
                "}\n",
                encoding="utf-8",
            )
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

        second = arxml.replace(
            "</VARIABLE-DATA-PROTOTYPE>",
            """</VARIABLE-DATA-PROTOTYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>OtherDefaults</SHORT-NAME><TYPE-TREF>/Init/Payload</TYPE-TREF>
              <INIT-VALUE><RECORD-VALUE-SPECIFICATION><FIELDS>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>8</VALUE></NUMERICAL-VALUE-SPECIFICATION>
              </FIELDS></RECORD-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>""",
            1,
        )
        ambiguous, _ = self.run_tool(
            {"ambiguous.arxml": second},
            "--prototype",
            "Defaults",
            "--prototype",
            "OtherDefaults",
        )
        self.assertEqual(ambiguous.returncode, 2)
        self.assertIn("multiple prototype initial values cannot share make_default()", ambiguous.stderr)

    def test_text_initial_values_preserve_whitespace_and_empty_strings(self) -> None:
        arxml = package_xml(
            "Init",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>Label</SHORT-NAME><TYPE-TREF>/Init/Name</TYPE-TREF>
              <INIT-VALUE><TEXT-VALUE-SPECIFICATION>
                <VALUE>  a&quot;b\\c  </VALUE>
              </TEXT-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>Empty</SHORT-NAME><TYPE-TREF>/Init/Name</TYPE-TREF>
              <INIT-VALUE><TEXT-VALUE-SPECIFICATION><VALUE/></TEXT-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>
            """,
        )
        result, _ = self.run_tool(
            {"strings.arxml": arxml},
            "--prototype",
            "Label",
            "--prototype",
            "Empty",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('return "  a\\\"b\\\\c  ";', result.stdout)
        self.assertIn('return "";', result.stdout)

        bounded = arxml.replace(
            "<SHORT-NAME>Name</SHORT-NAME><CATEGORY>STRING</CATEGORY>",
            "<SHORT-NAME>Name</SHORT-NAME><CATEGORY>STRING</CATEGORY>"
            "<SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>"
            "<SW-TEXT-PROPS><SW-MAX-TEXT-SIZE>3</SW-MAX-TEXT-SIZE></SW-TEXT-PROPS>"
            "</SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>",
            1,
        )
        rejected, _ = self.run_tool({"bounded_string.arxml": bounded}, "--prototype", "Label")
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("string initial value has 9 code point(s), maximum is 3", rejected.stderr)

    def test_generates_inline_structure_and_multidimensional_array(self) -> None:
        arxml = package_xml(
            "Nested",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Container</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>child</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                    <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                      <SHORT-NAME>state</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                      <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                        <BASE-TYPE-REF>/Nested/uint8</BASE-TYPE-REF>
                      </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                    </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  </SUB-ELEMENTS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>matrix</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                  <ARRAY-SIZE>2</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                    <SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                    <ARRAY-SIZE>3</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                    <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                      <SHORT-NAME>cell</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                      <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                        <BASE-TYPE-REF>/Nested/uint8</BASE-TYPE-REF>
                      </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                    </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
                  </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE>
              <SHORT-NAME>Service</SHORT-NAME><EVENTS><VARIABLE-DATA-PROTOTYPE>
                <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Nested/Container</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE></EVENTS>
            </SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>Defaults</SHORT-NAME><SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>EventDefaults</SHORT-NAME>
              <EVENT-REFS><EVENT-REF>/Nested/Service/Event</EVENT-REF></EVENT-REFS>
              <TRANSFORMATION-PROPS-REF>/Nested/Defaults</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool({"nested.arxml": arxml}, "--prototype", "Event")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("struct Container_child final {", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIELDS(state)", result.stdout)
        self.assertIn("std::array<std::array<uint8_t, 3>, 2> matrix{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_ARRAY_LENGTH(matrix, 2U, 2U)", result.stdout)
        self.assertLess(
            result.stdout.index("struct Container_child final {"),
            result.stdout.index("struct Container final {"),
        )

    def test_disambiguates_duplicate_short_names(self) -> None:
        first = package_xml(
            "First",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Status</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint8</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        second = package_xml(
            "Second",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Status</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint16</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"first.arxml": first, "second.arxml": second})
        ambiguous, _ = self.run_tool(
            {"first.arxml": first, "second.arxml": second}, "--type", "Status"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using First_Status = uint8_t;", result.stdout)
        self.assertIn("using Second_Status = uint16_t;", result.stdout)
        self.assertEqual(ambiguous.returncode, 2)
        self.assertIn("ambiguous short reference 'Status'", ambiguous.stderr)

    def test_generated_symbols_are_globally_unique(self) -> None:
        first = package_xml(
            "First",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Status</SHORT-NAME><CATEGORY>UINT8</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        second = package_xml(
            "Second",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Status</SHORT-NAME><CATEGORY>UINT16</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>First_Status</SHORT-NAME><CATEGORY>UINT32</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"first.arxml": first, "second.arxml": second})

        self.assertEqual(result.returncode, 0, result.stderr)
        declarations = [line for line in result.stdout.splitlines() if line.startswith("using ")]
        names = [line.split()[1] for line in declarations]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("using First_Status = uint32_t;", result.stdout)
        self.assertIn("using First_Status_2 = uint8_t;", result.stdout)

    def test_renames_fields_that_conflict_with_someip_macro_members(self) -> None:
        arxml = package_xml(
            "Members",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>vlink_someip_endian</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>vlink_someip_struct_length</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>get_vlink_someip_fields</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>make_default</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>serialize</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>deserialize</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Members/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE>
                <SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Members/Payload</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>Deployment</SHORT-NAME>
              <BYTE-ORDER>MOST-SIGNIFICANT-BYTE-LAST</BYTE-ORDER>
              <SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>Mapping</SHORT-NAME>
              <EVENT-REFS><EVENT-REF>/Members/Service/Event</EVENT-REF></EVENT-REFS>
              <TRANSFORMATION-PROPS-REF>/Members/Deployment</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, directory = self.run_tool(
            {"members.arxml": arxml}, "--prototype", "/Members/Service/Event", "--strict"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("uint8_t vlink_someip_endian_2{};", result.stdout)
        self.assertIn("uint8_t vlink_someip_struct_length_2{};", result.stdout)
        self.assertIn("uint8_t get_vlink_someip_fields_2{};", result.stdout)
        self.assertIn("uint8_t make_default_2{};", result.stdout)
        self.assertIn("uint8_t serialize_2{};", result.stdout)
        self.assertIn("uint8_t deserialize_2{};", result.stdout)
        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "members_generated.h"
            source = directory / "members_generated.cc"
            header.write_text(result.stdout, encoding="utf-8")
            source.write_text('#include "members_generated.h"\n', encoding="utf-8")
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_absolute_reference_does_not_fall_back_to_matching_short_name(self) -> None:
        arxml = package_xml(
            "Actual",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Counter</SHORT-NAME><CATEGORY>UINT32</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <APPLICATION-RECORD-DATA-TYPE>
              <SHORT-NAME>Message</SHORT-NAME><ELEMENTS><APPLICATION-RECORD-ELEMENT>
                <SHORT-NAME>counter</SHORT-NAME><TYPE-TREF>/Wrong/Counter</TYPE-TREF>
              </APPLICATION-RECORD-ELEMENT></ELEMENTS>
            </APPLICATION-RECORD-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"absolute.arxml": arxml}, "--type", "Message")

        self.assertEqual(result.returncode, 2)
        self.assertIn("unresolved AUTOSAR reference '/Wrong/Counter'", result.stderr)

    def test_default_mode_skips_malformed_union_but_strict_mode_rejects_warning(self) -> None:
        arxml = package_xml(
            "Mixed",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Good</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF>/Mixed/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Choice</SHORT-NAME><CATEGORY>UNION</CATEGORY>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool({"mixed.arxml": arxml})
        strict, _ = self.run_tool({"mixed.arxml": arxml}, "--strict")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("struct Good final {", result.stdout)
        self.assertNotIn("struct Choice", result.stdout)
        self.assertIn("union requires value alternatives", result.stderr)
        self.assertEqual(strict.returncode, 2)
        self.assertIn("strict mode rejected", strict.stderr)

    def test_explicit_malformed_union_and_incompatible_deployment_fail(self) -> None:
        arxml = package_xml(
            "Deploy",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Choice</SHORT-NAME><CATEGORY>UNION</CATEGORY>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </IMPLEMENTATION-DATA-TYPE>
            <SOMEIP-ARRAY-DEPLOYMENT>
              <SHORT-NAME>ArrayDeployment</SHORT-NAME><ARRAY-LENGTH-FIELD-SIZE>64</ARRAY-LENGTH-FIELD-SIZE>
            </SOMEIP-ARRAY-DEPLOYMENT>
            """,
        )
        union_result, _ = self.run_tool({"deploy.arxml": arxml}, "--type", "Choice")
        strict_result, _ = self.run_tool({"deploy.arxml": arxml}, "--type", "Value", "--strict")

        self.assertEqual(union_result.returncode, 2)
        self.assertIn("union requires value alternatives", union_result.stderr)
        self.assertEqual(strict_result.returncode, 2)
        self.assertIn("ARRAY-LENGTH-FIELD-SIZE=64", strict_result.stderr)

    def test_generates_union_deployment(self) -> None:
        arxml = package_xml(
            "Union",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Choice</SHORT-NAME><CATEGORY>UNION</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>small</SHORT-NAME><CATEGORY>VALUE</CATEGORY><BASE-TYPE-REF>/Union/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>large</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint32</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>choice</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <IMPLEMENTATION-DATA-TYPE-REF>/Union/Choice</IMPLEMENTATION-DATA-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE><SHORT-NAME>Event</SHORT-NAME><TYPE-TREF>/Union/Payload</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>Defaults</SHORT-NAME>
              <SIZE-OF-UNION-LENGTH-FIELD>2</SIZE-OF-UNION-LENGTH-FIELD>
              <SIZE-OF-UNION-TYPE-SELECTOR-FIELD>1</SIZE-OF-UNION-TYPE-SELECTOR-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>Mapping</SHORT-NAME>
              <EVENT-REFS><EVENT-REF>/Union/Service/Event</EVENT-REF></EVENT-REFS>
              <TRANSFORMATION-PROPS-REF>/Union/Defaults</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool({"union.arxml": arxml}, "--prototype", "/Union/Service/Event")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using Choice = std::variant<uint8_t, uint32_t>;", result.stdout)
        self.assertIn("VLINK_SOMEIP_UNION(choice, 2U, 1U)", result.stdout)

        unframed = arxml.replace(
            "<SIZE-OF-UNION-LENGTH-FIELD>2</SIZE-OF-UNION-LENGTH-FIELD>",
            "<SIZE-OF-UNION-LENGTH-FIELD>0</SIZE-OF-UNION-LENGTH-FIELD>",
        )
        rejected, _ = self.run_tool(
            {"unframed.arxml": unframed}, "--prototype", "/Union/Service/Event"
        )
        equal_size, _ = self.run_tool(
            {"unframed.arxml": unframed.replace(
                "/AUTOSAR/PlatformTypes/uint32", "/Union/uint8"
            )},
            "--prototype",
            "/Union/Service/Event",
        )

        self.assertEqual(rejected.returncode, 2)
        self.assertIn("requires equal-size scalar or enum alternatives", rejected.stderr)
        self.assertEqual(equal_size.returncode, 0, equal_size.stderr)
        self.assertIn("VLINK_SOMEIP_UNION(choice, 0U, 1U)", equal_size.stdout)

        tlv = arxml.replace(
            "<AP-SOMEIP-TRANSFORMATION-PROPS>",
            "<TLV-DATA-ID-DEFINITION-SET><SHORT-NAME>DataIds</SHORT-NAME><TLV-DATA-ID-DEFINITIONS>"
            "<TLV-DATA-ID-DEFINITION><ID>1</ID><TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>"
            "/Union/Payload/choice</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>"
            "</TLV-DATA-ID-DEFINITION></TLV-DATA-ID-DEFINITIONS></TLV-DATA-ID-DEFINITION-SET>"
            "<AP-SOMEIP-TRANSFORMATION-PROPS>",
            1,
        ).replace(
            "<SHORT-NAME>Defaults</SHORT-NAME>",
            "<SHORT-NAME>Defaults</SHORT-NAME><SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>",
            1,
        ).replace(
            "<TRANSFORMATION-PROPS-REF>/Union/Defaults</TRANSFORMATION-PROPS-REF>",
            "<TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/Union/DataIds"
            "</TLV-DATA-ID-DEFINITION-REF></TLV-DATA-ID-DEFINITION-REFS>"
            "<TRANSFORMATION-PROPS-REF>/Union/Defaults</TRANSFORMATION-PROPS-REF>",
            1,
        )
        tlv_rejected, _ = self.run_tool(
            {"tlv_union_1.arxml": tlv}, "--prototype", "/Union/Service/Event"
        )
        tlv_default, _ = self.run_tool(
            {"tlv_union_4.arxml": tlv.replace(
                "<SIZE-OF-UNION-TYPE-SELECTOR-FIELD>1</SIZE-OF-UNION-TYPE-SELECTOR-FIELD>",
                "<SIZE-OF-UNION-TYPE-SELECTOR-FIELD>4</SIZE-OF-UNION-TYPE-SELECTOR-FIELD>",
            )},
            "--prototype",
            "/Union/Service/Event",
        )

        self.assertEqual(tlv_rejected.returncode, 2)
        self.assertIn(
            "non-default union type selector width cannot be expressed inside a TLV member",
            tlv_rejected.stderr,
        )
        self.assertEqual(tlv_default.returncode, 0, tlv_default.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_LENGTH(1, choice, 2U)", tlv_default.stdout)

    def test_generates_application_and_cpp_associative_maps(self) -> None:
        arxml = package_xml(
            "Maps",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Key</SHORT-NAME><CATEGORY>UINT16</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Text</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <APPLICATION-ASSOC-MAP-DATA-TYPE>
              <SHORT-NAME>ApplicationLookup</SHORT-NAME><CATEGORY>ASSOCIATIVE_MAP</CATEGORY>
              <KEY><TYPE-TREF>/Maps/Key</TYPE-TREF></KEY>
              <VALUE><TYPE-TREF>/Maps/Text</TYPE-TREF></VALUE>
            </APPLICATION-ASSOC-MAP-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>uint32_t</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>string</SHORT-NAME><CATEGORY>STRING</CATEGORY>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>CppLookup</SHORT-NAME><CATEGORY>ASSOCIATIVE_MAP</CATEGORY>
              <TEMPLATE-ARGUMENTS>
                <CPP-TEMPLATE-ARGUMENT>
                  <CATEGORY>ASSOC_MAP_VALUE</CATEGORY><TEMPLATE-TYPE-REF>/Maps/string</TEMPLATE-TYPE-REF>
                </CPP-TEMPLATE-ARGUMENT>
                <CPP-TEMPLATE-ARGUMENT>
                  <CATEGORY>ASSOC_MAP_KEY</CATEGORY><TEMPLATE-TYPE-REF>/Maps/uint32_t</TEMPLATE-TYPE-REF>
                </CPP-TEMPLATE-ARGUMENT>
              </TEMPLATE-ARGUMENTS>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool(
            {"maps.arxml": arxml}, "--type", "ApplicationLookup", "--type", "CppLookup"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using ApplicationLookup = std::map<Key, Text>;", result.stdout)
        self.assertIn("using CppLookup = std::map<uint32_t, string>;", result.stdout)

    def test_generates_official_someip_length_and_struct_length_deployments(self) -> None:
        result, directory = self.run_tool(
            {"deployment.arxml": someip_length_deployment_xml()},
            "--prototype",
            "/Deploy/Service/Event",
            "--byte-arrays-as-bytes",
            "--strict",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout.count("VLINK_SOMEIP_STRUCT_LENGTH(2U)"), 2)
        self.assertIn("VLINK_SOMEIP_LENGTH(fixed, 0U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(dynamic, 2U, 8U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(name, 1U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(title, 1U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(history, 2U, 8U)", result.stdout)
        self.assertNotIn("make_default", result.stdout)

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        header = directory / "deployment_generated.h"
        source = directory / "deployment_generated.cc"
        header.write_text(result.stdout, encoding="utf-8")
        source.write_text('#include "deployment_generated.h"\n', encoding="utf-8")
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_accepts_all_supported_someip_transformation_props_tags(self) -> None:
        base = someip_length_deployment_xml()
        tags = (
            "AP-SOMEIP-TRANSFORMATION-PROPS",
            "SOMEIP-TRANSFORMATION-PROPS",
        )

        for tag in tags:
            with self.subTest(tag=tag):
                arxml = base.replace("AP-SOMEIP-TRANSFORMATION-PROPS", tag)
                result, _ = self.run_tool(
                    {f"{tag.lower()}.arxml": arxml}, "--prototype", "Event", "--strict"
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("VLINK_SOMEIP_LENGTH_MAX(dynamic, 2U, 8U)", result.stdout)
                self.assertIn("VLINK_SOMEIP_LENGTH(name, 1U)", result.stdout)

    def test_default_someip_lengths_do_not_emit_redundant_macros(self) -> None:
        result, _ = self.run_tool(
            {
                "default_deployment.arxml": someip_length_deployment_xml(
                    array_width=4, string_width=4, struct_width=0
                )
            },
            "--prototype",
            "Event",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("VLINK_SOMEIP_STRUCT_LENGTH", result.stdout)
        self.assertNotIn("VLINK_SOMEIP_LENGTH(dynamic", result.stdout)
        self.assertNotIn("VLINK_SOMEIP_LENGTH(name", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(fixed, 0U)", result.stdout)

        for width in (1, 4):
            with self.subTest(struct_width=width):
                generated, _ = self.run_tool(
                    {f"struct_{width}.arxml": someip_length_deployment_xml(struct_width=width)},
                    "--prototype",
                    "Event",
                    "--strict",
                )
                self.assertEqual(generated.returncode, 0, generated.stderr)
                self.assertEqual(
                    generated.stdout.count(f"VLINK_SOMEIP_STRUCT_LENGTH({width}U)"), 2
                )

    def test_generates_someip_byte_order_and_rejects_unrepresentable_endian(self) -> None:
        base = someip_length_deployment_xml()
        little_xml = base.replace(
            "<ALIGNMENT>8</ALIGNMENT>",
            "<ALIGNMENT>8</ALIGNMENT><BYTE-ORDER>MOST-SIGNIFICANT-BYTE-LAST</BYTE-ORDER>",
            1,
        )
        big_xml = base.replace(
            "<ALIGNMENT>8</ALIGNMENT>",
            "<ALIGNMENT>8</ALIGNMENT><BYTE-ORDER>MOST-SIGNIFICANT-BYTE-FIRST</BYTE-ORDER>",
            1,
        )
        opaque_xml = base.replace(
            "<ALIGNMENT>8</ALIGNMENT>",
            "<ALIGNMENT>8</ALIGNMENT><BYTE-ORDER>OPAQUE</BYTE-ORDER>",
            1,
        )

        little, _ = self.run_tool({"little.arxml": little_xml}, "--prototype", "Event", "--strict")
        big, _ = self.run_tool({"big.arxml": big_xml}, "--prototype", "Event", "--strict")
        opaque, _ = self.run_tool({"opaque.arxml": opaque_xml}, "--prototype", "Event")

        self.assertEqual(little.returncode, 0, little.stderr)
        self.assertIn("VLINK_SOMEIP_ENDIAN_LITTLE", little.stdout)
        self.assertEqual(big.returncode, 0, big.stderr)
        self.assertIn("VLINK_SOMEIP_ENDIAN_BIG", big.stdout)
        self.assertEqual(opaque.returncode, 2)
        self.assertIn("BYTE-ORDER OPAQUE cannot be expressed", opaque.stderr)

    def test_rejects_default_and_explicit_little_endian_for_shared_cpp_type(self) -> None:
        arxml = package_xml(
            "EndianConflict",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>valid</SHORT-NAME><CATEGORY>BOOLEAN</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><FIELDS>
              <FIELD><SHORT-NAME>Default</SHORT-NAME><TYPE-TREF>/EndianConflict/Payload</TYPE-TREF></FIELD>
              <FIELD><SHORT-NAME>Little</SHORT-NAME><TYPE-TREF>/EndianConflict/Payload</TYPE-TREF></FIELD>
            </FIELDS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>LittleEndian</SHORT-NAME>
              <BYTE-ORDER>MOST-SIGNIFICANT-BYTE-LAST</BYTE-ORDER>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>LittleMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/EndianConflict/Service/Little</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/EndianConflict/LittleEndian</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool(
            {"endian_conflict.arxml": arxml},
            "--prototype",
            "/EndianConflict/Service/Default",
            "--prototype",
            "/EndianConflict/Service/Little",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("conflicting SOME/IP byte orders 'big' and 'little'", result.stderr)

    def test_rejects_mixed_per_field_someip_byte_order(self) -> None:
        arxml = someip_length_deployment_xml().replace(
            "<SHORT-NAME>Defaults</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>",
            "<SHORT-NAME>Defaults</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>"
            "<BYTE-ORDER>MOST-SIGNIFICANT-BYTE-LAST</BYTE-ORDER>",
            1,
        ).replace(
            "<SHORT-NAME>NoArrayLength</SHORT-NAME>",
            "<SHORT-NAME>NoArrayLength</SHORT-NAME>"
            "<BYTE-ORDER>MOST-SIGNIFICANT-BYTE-FIRST</BYTE-ORDER>",
            1,
        )
        result, _ = self.run_tool({"mixed_endian.arxml": arxml}, "--prototype", "Event")

        self.assertEqual(result.returncode, 2)
        self.assertIn("per-field SOME/IP BYTE-ORDER cannot be expressed", result.stderr)

    def test_rejects_per_field_someip_alignment(self) -> None:
        arxml = someip_length_deployment_xml().replace(
            "<SHORT-NAME>NoArrayLength</SHORT-NAME>",
            "<SHORT-NAME>NoArrayLength</SHORT-NAME><ALIGNMENT>16</ALIGNMENT>",
            1,
        )
        result, _ = self.run_tool({"field_alignment.arxml": arxml}, "--prototype", "Event")

        self.assertEqual(result.returncode, 2)
        self.assertIn("per-field SOME/IP ALIGNMENT cannot be expressed", result.stderr)

    def test_rejects_invalid_someip_length_widths(self) -> None:
        cases = (
            (someip_length_deployment_xml(array_width=3), "length width 3"),
            (someip_length_deployment_xml(string_width=0), "zero length-field width"),
            (someip_length_deployment_xml(struct_width=3), "structure length width 3"),
        )
        for index, (arxml, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                result, _ = self.run_tool(
                    {f"invalid_{index}.arxml": arxml}, "--prototype", "Event"
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected, result.stderr)

    def test_generates_multidimensional_array_length_matrix(self) -> None:
        template = someip_length_deployment_xml(override_target="/Deploy/Payload/matrix")
        matrix = """
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>matrix</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
              <ARRAY-SIZE>2</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                <ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                <TYPE-REFERENCE-REF>/Deploy/Child</TYPE-REFERENCE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
        """
        arxml = template.replace(
            "          </SUB-ELEMENTS>\n        </IMPLEMENTATION-DATA-TYPE>\n        <SERVICE-INTERFACE>",
            textwrap.indent(textwrap.dedent(matrix).strip(), "          ")
            + "\n          </SUB-ELEMENTS>\n        </IMPLEMENTATION-DATA-TYPE>\n        <SERVICE-INTERFACE>",
            1,
        )

        for width in (1, 2, 4):
            with self.subTest(width=width):
                deployed = arxml.replace(
                    "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>",
                    f"<SIZE-OF-ARRAY-LENGTH-FIELD>{width}</SIZE-OF-ARRAY-LENGTH-FIELD>",
                    1,
                ).replace(
                    "<SIZE-OF-ARRAY-LENGTH-FIELD>0</SIZE-OF-ARRAY-LENGTH-FIELD>",
                    f"<SIZE-OF-ARRAY-LENGTH-FIELD>{width}</SIZE-OF-ARRAY-LENGTH-FIELD>",
                    1,
                )
                result, _ = self.run_tool(
                    {f"matrix_{width}.arxml": deployed}, "--prototype", "Event", "--strict"
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                if width == 4:
                    self.assertNotIn("VLINK_SOMEIP_ARRAY_LENGTH(matrix", result.stdout)
                else:
                    self.assertIn(
                        f"VLINK_SOMEIP_ARRAY_LENGTH(matrix, {width}U, {width}U)", result.stdout
                    )
                self.assertIn("VLINK_SOMEIP_LENGTH(title, 1U)", result.stdout)
                if width == 4:
                    self.assertNotIn("VLINK_SOMEIP_LENGTH(history", result.stdout)
                else:
                    self.assertIn(f"VLINK_SOMEIP_LENGTH_MAX(history, {width}U, 8U)", result.stdout)

        rejected, _ = self.run_tool({"matrix_zero.arxml": arxml}, "--prototype", "Event")
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("/Deploy/Payload/matrix", rejected.stderr)
        self.assertIn("zero length-field width is only valid for fixed-size arrays", rejected.stderr)

        fixed = arxml.replace(
            "<SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>\n"
            "              <ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>",
            "<SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>\n"
            "              <ARRAY-SIZE>4</ARRAY-SIZE>"
            "<ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>",
            1,
        ).replace(
            "<TYPE-REFERENCE-REF>/Deploy/Child</TYPE-REFERENCE-REF>",
            "<BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>",
            1,
        )
        accepted, _ = self.run_tool({"matrix_fixed_zero.arxml": fixed}, "--prototype", "Event")
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("VLINK_SOMEIP_ARRAY_LENGTH(matrix, 0U, 0U)", accepted.stdout)

        bounded = arxml.replace(
            "<SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>",
            "<SHORT-NAME>row</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><ARRAY-SIZE>4</ARRAY-SIZE>",
            1,
        ).replace(
            "<SIZE-OF-ARRAY-LENGTH-FIELD>0</SIZE-OF-ARRAY-LENGTH-FIELD>",
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>",
            1,
        )
        rejected_maximum, _ = self.run_tool(
            {"matrix_bounded.arxml": bounded}, "--prototype", "Event", "--strict"
        )
        self.assertEqual(rejected_maximum.returncode, 2)
        self.assertIn("maximum-size multidimensional arrays cannot be expressed", rejected_maximum.stderr)

        nested_string = arxml.replace(
            "<TYPE-REFERENCE-REF>/Deploy/Child</TYPE-REFERENCE-REF>",
            """<SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>cell</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>""",
            1,
        )
        supported_string = nested_string.replace(
            "<SIZE-OF-ARRAY-LENGTH-FIELD>0</SIZE-OF-ARRAY-LENGTH-FIELD>",
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>",
            1,
        ).replace(
            "<SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>",
            "<SIZE-OF-STRING-LENGTH-FIELD>4</SIZE-OF-STRING-LENGTH-FIELD>",
            1,
        )
        supported, _ = self.run_tool(
            {"matrix_string_default.arxml": supported_string}, "--prototype", "Event", "--strict"
        )
        self.assertEqual(supported.returncode, 0, supported.stderr)
        self.assertIn("VLINK_SOMEIP_ARRAY_LENGTH(matrix, 2U, 2U)", supported.stdout)

        unsupported_string = nested_string.replace(
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>",
            "<SIZE-OF-ARRAY-LENGTH-FIELD>4</SIZE-OF-ARRAY-LENGTH-FIELD>",
            1,
        ).replace(
            "<SIZE-OF-ARRAY-LENGTH-FIELD>0</SIZE-OF-ARRAY-LENGTH-FIELD>",
            "<SIZE-OF-ARRAY-LENGTH-FIELD>4</SIZE-OF-ARRAY-LENGTH-FIELD>",
            1,
        )
        unsupported, _ = self.run_tool(
            {"matrix_string.arxml": unsupported_string}, "--prototype", "Event"
        )
        self.assertEqual(unsupported.returncode, 2)
        self.assertIn("nested string cannot be expressed", unsupported.stderr)

    def test_generates_three_dimensional_array_lengths_through_aliases(self) -> None:
        result, directory = self.run_tool(
            {"alias_cube.arxml": nested_alias_deployment_xml()},
            "--prototype",
            "Event",
            "--strict",
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using Leaf = std::vector<uint8_t>;", result.stdout)
        self.assertIn("using Plane = std::array<Leaf, 2>;", result.stdout)
        self.assertIn("using Cube = std::array<Plane, 2>;", result.stdout)
        self.assertIn("VLINK_SOMEIP_ARRAY_LENGTH(values, 2U, 2U, 2U)", result.stdout)

        bounded, _ = self.run_tool(
            {"bounded_alias_cube.arxml": nested_alias_deployment_xml(maximum=8)},
            "--prototype",
            "Event",
            "--strict",
        )
        self.assertEqual(bounded.returncode, 2)
        self.assertIn("maximum-size multidimensional arrays cannot be expressed", bounded.stderr)

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        header = directory / "alias_cube.h"
        source = directory / "alias_cube.cc"
        header.write_text(result.stdout, encoding="utf-8")
        source.write_text('#include "alias_cube.h"\n', encoding="utf-8")
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_generates_dynamic_and_static_tlv_multidimensional_arrays(self) -> None:
        arxml = nested_alias_deployment_xml().replace(
            "<SHORT-NAME>values</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>",
            "<SHORT-NAME>values</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY><IS-OPTIONAL>true</IS-OPTIONAL>",
            1,
        ).replace(
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>",
            "<SIZE-OF-ARRAY-LENGTH-FIELD>2</SIZE-OF-ARRAY-LENGTH-FIELD>"
            "<SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>",
            1,
        ).replace(
            "<TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>",
            "<TLV-DATA-ID-DEFINITION-SET><SHORT-NAME>DataIds</SHORT-NAME><TLV-DATA-ID-DEFINITIONS>"
            "<TLV-DATA-ID-DEFINITION><ID>1</ID><TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>"
            "/Alias/Payload/values</TLV-IMPLEMENTATION-DATA-TYPE-ELEMENT-REF>"
            "</TLV-DATA-ID-DEFINITION></TLV-DATA-ID-DEFINITIONS></TLV-DATA-ID-DEFINITION-SET>"
            "<TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>",
            1,
        ).replace(
            "<TRANSFORMATION-PROPS-REF>/Alias/Deployment</TRANSFORMATION-PROPS-REF>",
            "<TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/Alias/DataIds"
            "</TLV-DATA-ID-DEFINITION-REF></TLV-DATA-ID-DEFINITION-REFS>"
            "<TRANSFORMATION-PROPS-REF>/Alias/Deployment</TRANSFORMATION-PROPS-REF>",
            1,
        )
        dynamic, _ = self.run_tool({"dynamic.arxml": arxml}, "--prototype", "Event", "--strict")
        static, _ = self.run_tool(
            {"static.arxml": arxml.replace(
                "<SHORT-NAME>Deployment</SHORT-NAME>",
                "<SHORT-NAME>Deployment</SHORT-NAME>"
                "<IS-DYNAMIC-LENGTH-FIELD-SIZE>false</IS-DYNAMIC-LENGTH-FIELD-SIZE>",
                1,
            )},
            "--prototype",
            "Event",
            "--strict",
        )

        self.assertEqual(dynamic.returncode, 0, dynamic.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_ARRAY_LENGTH(1, values, 2U, 2U, 2U)", dynamic.stdout)
        self.assertEqual(static.returncode, 0, static.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_STATIC_ARRAY_LENGTH(1, values, 2U, 2U, 2U)", static.stdout)

    def test_validates_nested_bytes_array_length_boundary(self) -> None:
        supported, _ = self.run_tool(
            {"nested_bytes_default.arxml": nested_alias_deployment_xml(4)},
            "--prototype",
            "Event",
            "--byte-arrays-as-bytes",
            "--strict",
        )
        rejected, _ = self.run_tool(
            {"nested_bytes_nondefault.arxml": nested_alias_deployment_xml(2)},
            "--prototype",
            "Event",
            "--byte-arrays-as-bytes",
        )

        self.assertEqual(supported.returncode, 0, supported.stderr)
        self.assertIn("using Leaf = vlink::Bytes;", supported.stdout)
        self.assertNotIn("VLINK_SOMEIP_ARRAY_LENGTH(values", supported.stdout)
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("nested bytes cannot be expressed", rejected.stderr)

    def test_rejects_unmatched_fine_grained_someip_target(self) -> None:
        result, _ = self.run_tool(
            {
                "unmatched.arxml": someip_length_deployment_xml(
                    override_target="/Deploy/Payload/missing"
                )
            },
            "--prototype",
            "Event",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("do not match generated fields", result.stderr)

    def test_applies_deployment_automatically_for_selected_type(self) -> None:
        result, _ = self.run_tool(
            {"automatic.arxml": someip_length_deployment_xml()}, "--type", "/Deploy/Payload"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VLINK_SOMEIP_STRUCT_LENGTH(2U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(dynamic, 2U, 8U)", result.stdout)

    def test_applies_field_and_method_direction_deployment_mappings(self) -> None:
        cases = {
            "field": (
                service_element_deployment_xml(
                    """
                <FIELDS><FIELD>
                  <SHORT-NAME>State</SHORT-NAME><TYPE-TREF>/ServiceDeploy/Payload</TYPE-TREF>
                </FIELD></FIELDS>
                    """,
                    """
                <FIELD-REFS><FIELD-REF>/ServiceDeploy/Service/State</FIELD-REF></FIELD-REFS>
                    """,
                ),
                "/ServiceDeploy/Service/State",
            ),
            "method_call": (
                service_element_deployment_xml(
                    """
                <METHODS><CLIENT-SERVER-OPERATION>
                  <SHORT-NAME>SetState</SHORT-NAME><ARGUMENTS><ARGUMENT-DATA-PROTOTYPE>
                    <SHORT-NAME>request</SHORT-NAME><TYPE-TREF>/ServiceDeploy/Payload</TYPE-TREF>
                    <DIRECTION>IN</DIRECTION>
                  </ARGUMENT-DATA-PROTOTYPE></ARGUMENTS>
                </CLIENT-SERVER-OPERATION></METHODS>
                    """,
                    """
                <METHOD-CALL-REFS>
                  <METHOD-CALL-REF>/ServiceDeploy/Service/SetState</METHOD-CALL-REF>
                </METHOD-CALL-REFS>
                    """,
                ),
                "/ServiceDeploy/Service/SetState/request",
            ),
            "method_return": (
                service_element_deployment_xml(
                    """
                <METHODS><CLIENT-SERVER-OPERATION>
                  <SHORT-NAME>GetState</SHORT-NAME><ARGUMENTS><ARGUMENT-DATA-PROTOTYPE>
                    <SHORT-NAME>response</SHORT-NAME><TYPE-TREF>/ServiceDeploy/Payload</TYPE-TREF>
                    <DIRECTION>OUT</DIRECTION>
                  </ARGUMENT-DATA-PROTOTYPE></ARGUMENTS>
                </CLIENT-SERVER-OPERATION></METHODS>
                    """,
                    """
                <METHOD-RETURN-REFS>
                  <METHOD-RETURN-REF>/ServiceDeploy/Service/GetState</METHOD-RETURN-REF>
                </METHOD-RETURN-REFS>
                    """,
                ),
                "/ServiceDeploy/Service/GetState/response",
            ),
            "method_inout": (
                service_element_deployment_xml(
                    """
                <METHODS><CLIENT-SERVER-OPERATION>
                  <SHORT-NAME>ExchangeState</SHORT-NAME><ARGUMENTS><ARGUMENT-DATA-PROTOTYPE>
                    <SHORT-NAME>state</SHORT-NAME><TYPE-TREF>/ServiceDeploy/Payload</TYPE-TREF>
                    <DIRECTION>INOUT</DIRECTION>
                  </ARGUMENT-DATA-PROTOTYPE></ARGUMENTS>
                </CLIENT-SERVER-OPERATION></METHODS>
                    """,
                    """
                <METHOD-REFS>
                  <METHOD-REF>/ServiceDeploy/Service/ExchangeState</METHOD-REF>
                </METHOD-REFS>
                    """,
                ),
                "/ServiceDeploy/Service/ExchangeState/state",
            ),
        }
        for name, (arxml, prototype) in cases.items():
            with self.subTest(name=name):
                result, _ = self.run_tool(
                    {f"{name}.arxml": arxml}, "--prototype", prototype, "--strict"
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("VLINK_SOMEIP_STRUCT_LENGTH(2U)", result.stdout)
                self.assertIn("VLINK_SOMEIP_LENGTH(name, 1U)", result.stdout)

    def test_rejects_conflicting_structure_lengths_for_shared_cpp_type(self) -> None:
        arxml = package_xml(
            "Conflict",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><FIELDS>
              <FIELD><SHORT-NAME>First</SHORT-NAME><TYPE-TREF>/Conflict/Payload</TYPE-TREF></FIELD>
              <FIELD><SHORT-NAME>Second</SHORT-NAME><TYPE-TREF>/Conflict/Payload</TYPE-TREF></FIELD>
            </FIELDS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>OneByte</SHORT-NAME><SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>
              <SIZE-OF-STRUCT-LENGTH-FIELD>1</SIZE-OF-STRUCT-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>TwoBytes</SHORT-NAME><SIZE-OF-STRING-LENGTH-FIELD>2</SIZE-OF-STRING-LENGTH-FIELD>
              <SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>FirstMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/Conflict/Service/First</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/Conflict/OneByte</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>SecondMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/Conflict/Service/Second</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/Conflict/TwoBytes</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool({"conflict.arxml": arxml}, "--type", "Payload")

        self.assertEqual(result.returncode, 2)
        self.assertIn("conflicting SOME/IP structure length widths 1 and 2", result.stderr)

    def test_rejects_conflicting_nested_field_lengths_for_shared_cpp_type(self) -> None:
        arxml = package_xml(
            "NestedConflict",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Child</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>DefaultPayload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>child</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <TYPE-REFERENCE-REF>/NestedConflict/Child</TYPE-REFERENCE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>TwoBytePayload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>child</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <TYPE-REFERENCE-REF>/NestedConflict/Child</TYPE-REFERENCE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><FIELDS>
              <FIELD>
                <SHORT-NAME>Default</SHORT-NAME><TYPE-TREF>/NestedConflict/DefaultPayload</TYPE-TREF>
              </FIELD>
              <FIELD>
                <SHORT-NAME>TwoByte</SHORT-NAME><TYPE-TREF>/NestedConflict/TwoBytePayload</TYPE-TREF>
              </FIELD>
            </FIELDS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>TwoByteStrings</SHORT-NAME>
              <SIZE-OF-STRING-LENGTH-FIELD>2</SIZE-OF-STRING-LENGTH-FIELD>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>TwoByteMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/NestedConflict/Service/TwoByte</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/NestedConflict/TwoByteStrings</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, _ = self.run_tool(
            {"nested_conflict.arxml": arxml},
            "--prototype",
            "/NestedConflict/Service/Default",
            "--prototype",
            "/NestedConflict/Service/TwoByte",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("conflicting SOME/IP length widths 4 and 2", result.stderr)

    def test_rejects_conflicting_encoding_and_alignment_for_shared_cpp_type(self) -> None:
        arxml = package_xml(
            "Shared",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><FIELDS>
              <FIELD><SHORT-NAME>First</SHORT-NAME><TYPE-TREF>/Shared/Payload</TYPE-TREF></FIELD>
              <FIELD><SHORT-NAME>Second</SHORT-NAME><TYPE-TREF>/Shared/Payload</TYPE-TREF></FIELD>
            </FIELDS></SERVICE-INTERFACE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>FirstProps</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>
              <STRING-ENCODING>UTF-8</STRING-ENCODING>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>SecondProps</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>
              <STRING-ENCODING>UTF-16BE</STRING-ENCODING>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>FirstMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/Shared/Service/First</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/Shared/FirstProps</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>SecondMapping</SHORT-NAME>
              <FIELD-REFS><FIELD-REF>/Shared/Service/Second</FIELD-REF></FIELD-REFS>
              <TRANSFORMATION-PROPS-REF>/Shared/SecondProps</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        for prototypes in (
            ("/Shared/Service/First", "/Shared/Service/Second"),
            ("/Shared/Service/Second", "/Shared/Service/First"),
        ):
            with self.subTest(prototypes=prototypes):
                conflict, _ = self.run_tool(
                    {"encoding.arxml": arxml},
                    "--prototype",
                    prototypes[0],
                    "--prototype",
                    prototypes[1],
                )
                self.assertEqual(conflict.returncode, 2)
                self.assertIn("conflicting SOME/IP type deployments", conflict.stderr)

        alignment = arxml.replace("<STRING-ENCODING>UTF-16BE</STRING-ENCODING>",
                                  "<STRING-ENCODING>UTF-8</STRING-ENCODING>").replace(
            "<SHORT-NAME>SecondProps</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>",
            "<SHORT-NAME>SecondProps</SHORT-NAME><ALIGNMENT>16</ALIGNMENT>",
        )
        conflict, _ = self.run_tool(
            {"alignment.arxml": alignment},
            "--prototype",
            "/Shared/Service/First",
            "--prototype",
            "/Shared/Service/Second",
        )
        self.assertEqual(conflict.returncode, 2)
        self.assertIn("conflicting SOME/IP alignments 1 and 2 bytes", conflict.stderr)

    def test_ignores_unreferenced_someip_descriptions_and_accepts_alignment_override(self) -> None:
        arxml = package_xml(
            "Deploy",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Message</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <SOMEIP-TRANSFORMATION-DESCRIPTION>
              <SHORT-NAME>First</SHORT-NAME><ALIGNMENT>16</ALIGNMENT>
            </SOMEIP-TRANSFORMATION-DESCRIPTION>
            <SOMEIP-TRANSFORMATION-DESCRIPTION>
              <SHORT-NAME>Second</SHORT-NAME><ALIGNMENT>32</ALIGNMENT>
            </SOMEIP-TRANSFORMATION-DESCRIPTION>
            """,
        )
        ambiguous, _ = self.run_tool({"deploy.arxml": arxml}, "--type", "Message")
        selected, _ = self.run_tool(
            {"deploy.arxml": arxml}, "--type", "Message", "--alignment-bits", "16"
        )

        self.assertEqual(ambiguous.returncode, 0, ambiguous.stderr)
        self.assertNotIn("VLINK_SOMEIP_ALIGNMENT", ambiguous.stdout)
        self.assertEqual(selected.returncode, 0, selected.stderr)
        self.assertIn("VLINK_SOMEIP_ALIGNMENT(2U)", selected.stdout)

    def test_lists_types_and_rejects_overwriting_input(self) -> None:
        arxml = package_xml(
            "List",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Counter</SHORT-NAME><CATEGORY>UINT32</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        listed, directory = self.run_tool({"list.arxml": arxml}, "--list-types")
        input_path = directory / "list.arxml"
        expected = subprocess.run(
            [sys.executable, str(self.tool), str(input_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        output_path = directory / "generated.h"
        output_path.write_text("old\n", encoding="utf-8")
        if os.name != "nt":
            output_path.chmod(0o600)
        written = subprocess.run(
            [sys.executable, str(self.tool), str(input_path), "--output", str(output_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        new_output_path = directory / "new_generated.h"
        new_written = subprocess.run(
            [sys.executable, str(self.tool), str(input_path), "--output", str(new_output_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        overwrite = subprocess.run(
            [sys.executable, str(self.tool), str(input_path), "--output", str(input_path)],
            check=False,
            capture_output=True,
            text=True,
        )
        missing_parent = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(input_path),
                "--output",
                str(directory / "missing" / "generated.h"),
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn("/List/Counter\tAPPLICATION-PRIMITIVE-DATA-TYPE", listed.stdout)
        self.assertEqual(expected.returncode, 0, expected.stderr)
        self.assertEqual(written.returncode, 0, written.stderr)
        self.assertEqual(written.stdout, "")
        self.assertEqual(output_path.read_text(encoding="utf-8"), expected.stdout)
        self.assertEqual(new_written.returncode, 0, new_written.stderr)
        self.assertEqual(new_written.stdout, "")
        self.assertEqual(new_output_path.read_text(encoding="utf-8"), expected.stdout)
        if os.name != "nt":
            self.assertEqual(output_path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(new_output_path.stat().st_mode & 0o777, 0o644)
        self.assertEqual(overwrite.returncode, 2)
        self.assertIn("must not overwrite", overwrite.stderr)
        self.assertEqual(missing_parent.returncode, 2)
        self.assertIn("output directory does not exist", missing_parent.stderr)

    def test_lists_nested_data_prototypes(self) -> None:
        listed = subprocess.run(
            [sys.executable, str(self.tool), str(self.fixture), "--list-prototypes"],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn(
            "/VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent\tVARIABLE-DATA-PROTOTYPE",
            listed.stdout,
        )

    def test_rejects_out_of_range_prototype_initial_value(self) -> None:
        invalid = self.fixture.read_text(encoding="utf-8").replace(
            "<NUMERICAL-VALUE-SPECIFICATION><VALUE>7</VALUE></NUMERICAL-VALUE-SPECIFICATION>",
            "<NUMERICAL-VALUE-SPECIFICATION><VALUE>4294967296</VALUE></NUMERICAL-VALUE-SPECIFICATION>",
            1,
        )
        result, _ = self.run_tool(
            {"invalid_initial.arxml": invalid},
            "--prototype",
            "VehicleStateEvent",
            "--byte-arrays-as-bytes",
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("does not fit uint32_t", result.stderr)

    def test_rejects_invalid_composite_initial_values(self) -> None:
        base = self.fixture.read_text(encoding="utf-8")
        wrong_spec = base.replace(
            "<INIT-VALUE>\n                    <RECORD-VALUE-SPECIFICATION>",
            "<INIT-VALUE>\n                    <ARRAY-VALUE-SPECIFICATION>",
            1,
        ).replace(
            "                    </RECORD-VALUE-SPECIFICATION>\n                  </INIT-VALUE>",
            "                    </ARRAY-VALUE-SPECIFICATION>\n                  </INIT-VALUE>",
            1,
        )
        extra_record_field = base.replace(
            "<RECORD-VALUE-SPECIFICATION>\n                      <FIELDS>",
            """<RECORD-VALUE-SPECIFICATION>
                      <FIELDS>
                        <NUMERICAL-VALUE-SPECIFICATION><VALUE>0</VALUE></NUMERICAL-VALUE-SPECIFICATION>""",
            1,
        )
        short_fixed_array = base.replace(
            "                            <NUMERICAL-VALUE-SPECIFICATION><VALUE>40</VALUE>"
            "</NUMERICAL-VALUE-SPECIFICATION>\n",
            "",
            1,
        )
        cases = (
            (wrong_spec, "expected RECORD-VALUE-SPECIFICATION, got ARRAY-VALUE-SPECIFICATION"),
            (extra_record_field, "record initial value has 11 field(s), expected 10"),
            (short_fixed_array, "array initial value has 3 element(s), expected 4"),
        )

        for index, (arxml, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                result, _ = self.run_tool(
                    {f"composite_{index}.arxml": arxml},
                    "--prototype",
                    "VehicleStateEvent",
                    "--byte-arrays-as-bytes",
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected, result.stderr)

    def test_rejects_invalid_constant_initial_values(self) -> None:
        base = self.fixture.read_text(encoding="utf-8")
        constant_ref = (
            '<CONSTANT-REF DEST="CONSTANT-SPECIFICATION">'
            "/VLink/ServiceInterfaces/InitialSequence</CONSTANT-REF>"
        )
        missing_ref = base.replace(constant_ref, "<SHORT-LABEL>missing</SHORT-LABEL>", 1)
        wrong_target = base.replace(
            "/VLink/ServiceInterfaces/InitialSequence</CONSTANT-REF>",
            "/VLink/ImplementationTypes/VehicleState</CONSTANT-REF>",
            1,
        )
        missing_value = base.replace(
            """<VALUE-SPEC>
                <NUMERICAL-VALUE-SPECIFICATION><VALUE>7</VALUE></NUMERICAL-VALUE-SPECIFICATION>
              </VALUE-SPEC>""",
            "<VALUE-SPEC/>",
            1,
        )
        cyclic = base.replace(
            "<NUMERICAL-VALUE-SPECIFICATION><VALUE>7</VALUE></NUMERICAL-VALUE-SPECIFICATION>",
            f"<CONSTANT-REFERENCE>{constant_ref}</CONSTANT-REFERENCE>",
            1,
        )
        cases = (
            (missing_ref, "CONSTANT-REFERENCE has no CONSTANT-REF"),
            (wrong_target, "is not a CONSTANT-SPECIFICATION"),
            (missing_value, "constant has no VALUE-SPEC"),
            (cyclic, "cyclic CONSTANT-REFERENCE"),
        )

        for index, (arxml, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                result, _ = self.run_tool(
                    {f"constant_{index}.arxml": arxml},
                    "--prototype",
                    "VehicleStateEvent",
                    "--byte-arrays-as-bytes",
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected, result.stderr)

    def test_rejects_invalid_scalar_initial_values(self) -> None:
        base = self.fixture.read_text(encoding="utf-8")
        boolean = (
            "<NUMERICAL-VALUE-SPECIFICATION><VALUE>true</VALUE>"
            "</NUMERICAL-VALUE-SPECIFICATION>"
        )
        application_value = "<SW-VALUES-PHYS><VT>drive</VT></SW-VALUES-PHYS>"
        cases = (
            (base.replace(boolean, "<NUMERICAL-VALUE-SPECIFICATION/>", 1), "has no VALUE"),
            (
                base.replace(
                    boolean,
                    "<NUMERICAL-VALUE-SPECIFICATION><VALUE/></NUMERICAL-VALUE-SPECIFICATION>",
                    1,
                ),
                "scalar initial value is empty",
            ),
            (
                base.replace(
                    boolean,
                    "<ARRAY-VALUE-SPECIFICATION><ELEMENTS/></ARRAY-VALUE-SPECIFICATION>",
                    1,
                ),
                "unsupported scalar initial value specification",
            ),
            (base.replace(application_value, "<SW-VALUES-PHYS/>", 1), "exactly one V or VT"),
            (
                base.replace(
                    application_value,
                    "<SW-VALUES-PHYS><VT>drive</VT><V>1</V></SW-VALUES-PHYS>",
                    1,
                ),
                "exactly one V or VT",
            ),
            (base.replace(">drive<", ">flying<", 1), "unknown GearMode enumeration label"),
            (base.replace(">true<", ">maybe<", 1), "invalid boolean initial value"),
            (base.replace(">20.5<", ">invalid<", 1), "invalid floating-point initial value"),
            (base.replace(">20.5<", ">nan<", 1), "non-finite floating-point"),
            (base.replace("<VALUE>7</VALUE>", "<VALUE>invalid</VALUE>", 1), "invalid integer initial value"),
        )

        for index, (arxml, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                result, _ = self.run_tool(
                    {f"scalar_{index}.arxml": arxml},
                    "--prototype",
                    "VehicleStateEvent",
                    "--byte-arrays-as-bytes",
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected, result.stderr)

    def test_renders_64_bit_initial_value_boundaries(self) -> None:
        arxml = package_xml(
            "Boundary",
            """
            <SW-BASE-TYPE>
              <SHORT-NAME>uint64</SHORT-NAME><BASE-TYPE-SIZE>64</BASE-TYPE-SIZE>
              <NATIVE-DECLARATION>uint64</NATIVE-DECLARATION>
            </SW-BASE-TYPE>
            <SW-BASE-TYPE>
              <SHORT-NAME>sint64</SHORT-NAME><BASE-TYPE-SIZE>64</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>2C</BASE-TYPE-ENCODING>
            </SW-BASE-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Limits</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>maximum</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Boundary/uint64</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>minimum</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Boundary/sint64</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>Defaults</SHORT-NAME><TYPE-TREF>/Boundary/Limits</TYPE-TREF>
              <INIT-VALUE><RECORD-VALUE-SPECIFICATION><FIELDS>
                <NUMERICAL-VALUE-SPECIFICATION>
                  <VALUE>18446744073709551615</VALUE>
                </NUMERICAL-VALUE-SPECIFICATION>
                <NUMERICAL-VALUE-SPECIFICATION>
                  <VALUE>-9223372036854775808</VALUE>
                </NUMERICAL-VALUE-SPECIFICATION>
              </FIELDS></RECORD-VALUE-SPECIFICATION></INIT-VALUE>
            </VARIABLE-DATA-PROTOTYPE>
            """,
        )
        result, directory = self.run_tool(
            {"boundaries.arxml": arxml}, "--prototype", "Defaults"
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("static_cast<uint64_t>(18446744073709551615ULL)", result.stdout)
        self.assertIn("static_cast<int64_t>((-9223372036854775807LL - 1LL))", result.stdout)

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        header = directory / "boundaries.h"
        source = directory / "boundaries.cc"
        header.write_text(result.stdout, encoding="utf-8")
        source.write_text('#include "boundaries.h"\n', encoding="utf-8")
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_parses_autosar_octal_float_and_rejects_recursive_or_unmapped_fixed_string(self) -> None:
        arxml = package_xml(
            "Compat",
            UINT8_BASE
            + """
            <SW-BASE-TYPE>
              <SHORT-NAME>MyReal</SHORT-NAME><BASE-TYPE-SIZE>64</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>FLOAT</BASE-TYPE-ENCODING>
            </SW-BASE-TYPE>
            <SW-BASE-TYPE><SHORT-NAME>Utf8</SHORT-NAME><BASE-TYPE-SIZE>8</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>UTF-8</BASE-TYPE-ENCODING></SW-BASE-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>RealValue</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Compat/MyReal</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>OctalArray</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <ARRAY-SIZE>010</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <BASE-TYPE-REF>/Compat/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Node</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>child</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <IMPLEMENTATION-DATA-TYPE-REF>/Compat/Node</IMPLEMENTATION-DATA-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>FixedText</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <SW-TEXT-PROPS><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-MAX-TEXT-SIZE>4</SW-MAX-TEXT-SIZE><BASE-TYPE-REF>/Compat/Utf8</BASE-TYPE-REF>
                </SW-TEXT-PROPS>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        generated, _ = self.run_tool(
            {"compat.arxml": arxml}, "--type", "RealValue", "--type", "OctalArray"
        )
        recursive, _ = self.run_tool({"compat.arxml": arxml}, "--type", "Node")
        fixed_string, _ = self.run_tool({"compat.arxml": arxml}, "--type", "FixedText")

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertIn("using RealValue = double;", generated.stdout)
        self.assertIn("using OctalArray = std::array<uint8_t, 8>;", generated.stdout)
        self.assertEqual(recursive.returncode, 2)
        self.assertIn("recursive by-value", recursive.stderr)
        self.assertEqual(fixed_string.returncode, 2)
        self.assertIn("fixed string requires an Application-to-Implementation mapping", fixed_string.stderr)

    def test_generates_mapped_fixed_utf8_string(self) -> None:
        arxml = package_xml(
            "Fixed",
            UINT8_BASE
            + """
            <SW-BASE-TYPE><SHORT-NAME>Utf8</SHORT-NAME><BASE-TYPE-SIZE>8</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>UTF-8</BASE-TYPE-ENCODING></SW-BASE-TYPE>
            <SW-BASE-TYPE><SHORT-NAME>uint16</SHORT-NAME><BASE-TYPE-SIZE>16</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>NONE</BASE-TYPE-ENCODING></SW-BASE-TYPE>
            <SW-BASE-TYPE><SHORT-NAME>Utf16</SHORT-NAME><BASE-TYPE-SIZE>16</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>UTF-16</BASE-TYPE-ENCODING></SW-BASE-TYPE>
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <SW-TEXT-PROPS><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-MAX-TEXT-SIZE>4</SW-MAX-TEXT-SIZE>
                  <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Fixed/Utf8</BASE-TYPE-REF>
                </SW-TEXT-PROPS>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Name16</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <SW-TEXT-PROPS><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-MAX-TEXT-SIZE>3</SW-MAX-TEXT-SIZE>
                  <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Fixed/Utf16</BASE-TYPE-REF>
                </SW-TEXT-PROPS>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>NameStorage</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <ARRAY-SIZE>8</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Fixed/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Name16Storage</SHORT-NAME><CATEGORY>ARRAY</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>element</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <ARRAY-SIZE>4</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF DEST="SW-BASE-TYPE">/Fixed/uint16</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <APPLICATION-RECORD-DATA-TYPE><SHORT-NAME>Payload</SHORT-NAME><ELEMENTS>
              <APPLICATION-RECORD-ELEMENT><SHORT-NAME>name</SHORT-NAME>
                <TYPE-TREF DEST="APPLICATION-PRIMITIVE-DATA-TYPE">/Fixed/Name</TYPE-TREF>
              </APPLICATION-RECORD-ELEMENT>
              <APPLICATION-RECORD-ELEMENT><SHORT-NAME>name16</SHORT-NAME>
                <TYPE-TREF DEST="APPLICATION-PRIMITIVE-DATA-TYPE">/Fixed/Name16</TYPE-TREF>
              </APPLICATION-RECORD-ELEMENT>
            </ELEMENTS></APPLICATION-RECORD-DATA-TYPE>
            <DATA-TYPE-MAPPING-SET><SHORT-NAME>Mappings</SHORT-NAME><DATA-TYPE-MAPS><DATA-TYPE-MAP>
              <APPLICATION-DATA-TYPE-REF DEST="APPLICATION-PRIMITIVE-DATA-TYPE">/Fixed/Name</APPLICATION-DATA-TYPE-REF>
              <IMPLEMENTATION-DATA-TYPE-REF DEST="IMPLEMENTATION-DATA-TYPE">/Fixed/NameStorage</IMPLEMENTATION-DATA-TYPE-REF>
            </DATA-TYPE-MAP></DATA-TYPE-MAPS></DATA-TYPE-MAPPING-SET>
            <DATA-TYPE-MAPPING-SET><SHORT-NAME>Mappings16</SHORT-NAME><DATA-TYPE-MAPS><DATA-TYPE-MAP>
              <APPLICATION-DATA-TYPE-REF DEST="APPLICATION-PRIMITIVE-DATA-TYPE">/Fixed/Name16</APPLICATION-DATA-TYPE-REF>
              <IMPLEMENTATION-DATA-TYPE-REF DEST="IMPLEMENTATION-DATA-TYPE">/Fixed/Name16Storage</IMPLEMENTATION-DATA-TYPE-REF>
            </DATA-TYPE-MAP></DATA-TYPE-MAPS></DATA-TYPE-MAPPING-SET>
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS><VARIABLE-DATA-PROTOTYPE>
              <SHORT-NAME>Event</SHORT-NAME>
              <TYPE-TREF DEST="APPLICATION-RECORD-DATA-TYPE">/Fixed/Payload</TYPE-TREF>
            </VARIABLE-DATA-PROTOTYPE></EVENTS></SERVICE-INTERFACE>
            <TRANSFORMATION-PROPS-SET><SHORT-NAME>Props</SHORT-NAME><TRANSFORMATION-PROPSS>
              <AP-SOMEIP-TRANSFORMATION-PROPS><SHORT-NAME>Deployment</SHORT-NAME>
                <SIZE-OF-STRING-LENGTH-FIELD>0</SIZE-OF-STRING-LENGTH-FIELD>
              </AP-SOMEIP-TRANSFORMATION-PROPS>
            </TRANSFORMATION-PROPSS></TRANSFORMATION-PROPS-SET>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING><SHORT-NAME>Mapping</SHORT-NAME>
              <EVENT-REFS><EVENT-REF DEST="VARIABLE-DATA-PROTOTYPE">/Fixed/Service/Event</EVENT-REF></EVENT-REFS>
              <TRANSFORMATION-PROPS-REF DEST="AP-SOMEIP-TRANSFORMATION-PROPS">/Fixed/Props/Deployment</TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        result, directory = self.run_tool({"fixed.arxml": arxml}, "--prototype", "Event", "--strict")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("// AUTOSAR fixed wire size: 11 bytes.", result.stdout)
        self.assertIn("// AUTOSAR maximum text size: 4 code points.", result.stdout)
        self.assertIn("using Name = std::string;", result.stdout)
        self.assertIn("Name name{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIXED_STRING_MAX(name, 11U, 0U, 4U)", result.stdout)
        self.assertIn("// AUTOSAR fixed wire size: 10 bytes.", result.stdout)
        self.assertIn("// AUTOSAR maximum text size: 3 code points.", result.stdout)
        self.assertIn("using Name16 = std::u16string;", result.stdout)
        self.assertIn("Name16 name16{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIXED_UTF16_BE_MAX(name16, 10U, 0U, 3U)", result.stdout)
        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "fixed_generated.h"
            source = directory / "fixed_generated.cc"
            header.write_text(result.stdout, encoding="utf-8")
            source.write_text('#include "fixed_generated.h"\n', encoding="utf-8")
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

        definitions = """
            <TLV-DATA-ID-DEFINITION-SET><SHORT-NAME>DataIds</SHORT-NAME><TLV-DATA-ID-DEFINITIONS>
              <TLV-DATA-ID-DEFINITION><ID>1</ID>
                <TLV-RECORD-ELEMENT-REF>/Fixed/Payload/name</TLV-RECORD-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
              <TLV-DATA-ID-DEFINITION><ID>2</ID>
                <TLV-RECORD-ELEMENT-REF>/Fixed/Payload/name16</TLV-RECORD-ELEMENT-REF>
              </TLV-DATA-ID-DEFINITION>
            </TLV-DATA-ID-DEFINITIONS></TLV-DATA-ID-DEFINITION-SET>
        """
        tlv = arxml.replace(
            "<TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING><SHORT-NAME>Mapping</SHORT-NAME>",
            definitions
            + "<TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING><SHORT-NAME>Mapping</SHORT-NAME>",
        ).replace(
            "<SIZE-OF-STRING-LENGTH-FIELD>0</SIZE-OF-STRING-LENGTH-FIELD>",
            "<SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>"
            "<SIZE-OF-STRUCT-LENGTH-FIELD>1</SIZE-OF-STRUCT-LENGTH-FIELD>",
        ).replace(
            "<TRANSFORMATION-PROPS-REF DEST=\"AP-SOMEIP-TRANSFORMATION-PROPS\">",
            "<TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/Fixed/DataIds"
            "</TLV-DATA-ID-DEFINITION-REF></TLV-DATA-ID-DEFINITION-REFS>"
            "<TRANSFORMATION-PROPS-REF DEST=\"AP-SOMEIP-TRANSFORMATION-PROPS\">",
        )
        dynamic, _ = self.run_tool({"fixed_tlv.arxml": tlv}, "--prototype", "Event", "--strict")
        static, _ = self.run_tool(
            {"fixed_static_tlv.arxml": tlv.replace(
                "<SHORT-NAME>Deployment</SHORT-NAME>",
                "<SHORT-NAME>Deployment</SHORT-NAME><IS-DYNAMIC-LENGTH-FIELD-SIZE>false</IS-DYNAMIC-LENGTH-FIELD-SIZE>",
            )},
            "--prototype",
            "Event",
            "--strict",
        )

        self.assertEqual(dynamic.returncode, 0, dynamic.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_FIXED_STRING_MAX(1, name, 11U, 1U", dynamic.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_FIXED_STRING_MAX(2, name16, 10U, 1U", dynamic.stdout)
        self.assertEqual(static.returncode, 0, static.stderr)
        self.assertIn("VLINK_SOMEIP_TLV_STATIC_FIXED_STRING_MAX(1, name, 11U, 1U", static.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_STATIC_FIXED_STRING_MAX(2, name16, 10U, 1U", static.stdout)

    def test_rejects_cp_wire_models_that_cannot_be_expressed(self) -> None:
        cases = {
            "profile": """
              <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Payload</SHORT-NAME>
                <CATEGORY>STRUCTURE</CATEGORY><DYNAMIC-ARRAY-SIZE-PROFILE>VSA_LINEAR</DYNAMIC-ARRAY-SIZE-PROFILE>
              </IMPLEMENTATION-DATA-TYPE>
            """,
            "availability": """
              <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
                <IS-STRUCT-WITH-OPTIONAL-ELEMENT>true</IS-STRUCT-WITH-OPTIONAL-ELEMENT>
              </IMPLEMENTATION-DATA-TYPE>
            """,
            "wrapper": UINT8_BASE + """
              <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>size</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/CP/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>data</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                  <ARRAY-SIZE>4</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                  <BASE-TYPE-REF>/CP/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        }
        expected = {
            "profile": "variable-array size profiles are not supported",
            "availability": "availability-bitfield structures are not supported",
            "wrapper": "variable-array wrappers are not supported",
        }

        for name, body in cases.items():
            with self.subTest(model=name):
                result, _ = self.run_tool({f"{name}.arxml": package_xml("CP", body)}, "--type", "Payload")
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected[name], result.stderr)

        isignal = """
        <SOMEIP-TRANSFORMATION-I-SIGNAL-PROPS xmlns="http://autosar.org/schema/r4.0">
          <SHORT-NAME>Deployment</SHORT-NAME>
        </SOMEIP-TRANSFORMATION-I-SIGNAL-PROPS>
        """
        rejected, _ = self.run_tool({"isignal.arxml": textwrap.dedent(isignal)})
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("CP SOMEIP-TRANSFORMATION-I-SIGNAL-PROPS are not supported", rejected.stderr)

    def test_preserves_mapped_dynamic_string_maximum(self) -> None:
        arxml = package_xml(
            "Text",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE><SHORT-NAME>Name</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                <SW-TEXT-PROPS><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-MAX-TEXT-SIZE>12</SW-MAX-TEXT-SIZE>
                </SW-TEXT-PROPS>
              </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            <STD-CPP-IMPLEMENTATION-DATA-TYPE><SHORT-NAME>NameImpl</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <HEADER-FILE>ara/core/string.h</HEADER-FILE>
            </STD-CPP-IMPLEMENTATION-DATA-TYPE>
            <APPLICATION-RECORD-DATA-TYPE><SHORT-NAME>Payload</SHORT-NAME><ELEMENTS>
              <APPLICATION-RECORD-ELEMENT><SHORT-NAME>name</SHORT-NAME><TYPE-TREF>/Text/Name</TYPE-TREF>
              </APPLICATION-RECORD-ELEMENT>
            </ELEMENTS></APPLICATION-RECORD-DATA-TYPE>
            <DATA-TYPE-MAPPING-SET><SHORT-NAME>Mappings</SHORT-NAME><DATA-TYPE-MAPS><DATA-TYPE-MAP>
              <APPLICATION-DATA-TYPE-REF>/Text/Name</APPLICATION-DATA-TYPE-REF>
              <IMPLEMENTATION-DATA-TYPE-REF>/Text/NameImpl</IMPLEMENTATION-DATA-TYPE-REF>
            </DATA-TYPE-MAP></DATA-TYPE-MAPS></DATA-TYPE-MAPPING-SET>
            """,
        )
        result, _ = self.run_tool({"text.arxml": arxml}, "--type", "Payload")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("// AUTOSAR maximum text size: 12 code points.", result.stdout)
        self.assertIn("Name name{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(name, 4U, 12U)", result.stdout)

    def test_rejects_conflicting_mappings_and_ignores_unreferenced_ap_alignment(self) -> None:
        first = package_xml(
            "MapOne",
            """
            <DATA-TYPE-MAPPING-SET><SHORT-NAME>Mappings</SHORT-NAME><DATA-TYPE-MAPS>
              <DATA-TYPE-MAPPING>
                <APPLICATION-DATA-TYPE-REF>/App/Value</APPLICATION-DATA-TYPE-REF>
                <IMPLEMENTATION-DATA-TYPE-REF>/Impl/First</IMPLEMENTATION-DATA-TYPE-REF>
              </DATA-TYPE-MAPPING>
            </DATA-TYPE-MAPS></DATA-TYPE-MAPPING-SET>
            """,
        )
        second = package_xml(
            "MapTwo",
            """
            <DATA-TYPE-MAPPING-SET><SHORT-NAME>Mappings</SHORT-NAME><DATA-TYPE-MAPS>
              <DATA-TYPE-MAPPING>
                <APPLICATION-DATA-TYPE-REF>/App/Value</APPLICATION-DATA-TYPE-REF>
                <IMPLEMENTATION-DATA-TYPE-REF>/Impl/Second</IMPLEMENTATION-DATA-TYPE-REF>
              </DATA-TYPE-MAPPING>
            </DATA-TYPE-MAPS></DATA-TYPE-MAPPING-SET>
            """,
        )
        conflict, _ = self.run_tool({"first.arxml": first, "second.arxml": second})
        self.assertEqual(conflict.returncode, 2)
        self.assertIn("conflicting implementation mappings", conflict.stderr)

        unrelated = package_xml(
            "Align",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Message</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <BASE-TYPE-REF>/Align/uint8</BASE-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            <AP-SOMEIP-TRANSFORMATION-PROPS>
              <SHORT-NAME>Unused</SHORT-NAME><ALIGNMENT>32</ALIGNMENT>
            </AP-SOMEIP-TRANSFORMATION-PROPS>
            """,
        )
        generated, _ = self.run_tool({"alignment.arxml": unrelated}, "--type", "Message")
        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertNotIn("VLINK_SOMEIP_ALIGNMENT", generated.stdout)

    def test_rejects_ordered_type_members_split_across_files(self) -> None:
        member_template = """
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>{name}</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Split/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
          </SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        """
        first = package_xml("Split", UINT8_BASE + member_template.format(name="first"))
        second = package_xml("Split", member_template.format(name="second"))
        for files in (
            {"first.arxml": first, "second.arxml": second},
            {"second.arxml": second, "first.arxml": first},
        ):
            split, _ = self.run_tool(files, "--type", "/Split/Payload")
            self.assertEqual(split.returncode, 2)
            self.assertIn("cannot merge ordered 'SUB-ELEMENTS' members", split.stderr)

        members = """
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>{first}</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Split/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            <IMPLEMENTATION-DATA-TYPE-ELEMENT>
              <SHORT-NAME>{second}</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Split/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE-ELEMENT>
        """
        structure = """
        <IMPLEMENTATION-DATA-TYPE>
          <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
          <SUB-ELEMENTS>{members}</SUB-ELEMENTS>
        </IMPLEMENTATION-DATA-TYPE>
        """
        ordered = package_xml(
            "Split", UINT8_BASE + structure.format(members=members.format(first="first", second="second"))
        )
        reversed_order = package_xml(
            "Split", structure.format(members=members.format(first="second", second="first"))
        )
        for files in (
            {"ordered.arxml": ordered, "reversed.arxml": reversed_order},
            {"reversed.arxml": reversed_order, "ordered.arxml": ordered},
        ):
            split, _ = self.run_tool(files, "--type", "/Split/Payload")
            self.assertEqual(split.returncode, 2)
            self.assertIn("cannot merge ordered 'SUB-ELEMENTS' members", split.stderr)

        subset = package_xml("Split", member_template.format(name="first"))
        for files in (
            {"ordered.arxml": ordered, "subset.arxml": subset},
            {"subset.arxml": subset, "ordered.arxml": ordered},
        ):
            split, _ = self.run_tool(files, "--type", "/Split/Payload")
            self.assertEqual(split.returncode, 2)
            self.assertIn("cannot merge ordered 'SUB-ELEMENTS' members", split.stderr)

        package_start = first.index("<AR-PACKAGE>")
        package_end = first.index("</AR-PACKAGE>") + len("</AR-PACKAGE>")
        standalone, _ = self.run_tool(
            {"standalone.arxml": first[package_start:package_end]}, "--type", "/Split/Payload"
        )
        self.assertEqual(standalone.returncode, 0, standalone.stderr)

    def test_generates_cross_file_service_deployment(self) -> None:
        types = package_xml(
            "MultiTypes",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Payload</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>values</SHORT-NAME><CATEGORY>ARRAY</CATEGORY>
                  <ARRAY-SIZE>4</ARRAY-SIZE><ARRAY-SIZE-SEMANTICS>VARIABLE-SIZE</ARRAY-SIZE-SEMANTICS>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF DEST="SW-BASE-TYPE">/MultiTypes/uint8</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>label</SHORT-NAME><CATEGORY>STRING</CATEGORY>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        service = package_xml(
            "MultiService",
            """
            <SERVICE-INTERFACE><SHORT-NAME>Service</SHORT-NAME><EVENTS>
              <VARIABLE-DATA-PROTOTYPE><SHORT-NAME>Event</SHORT-NAME>
                <TYPE-TREF DEST="IMPLEMENTATION-DATA-TYPE">/MultiTypes/Payload</TYPE-TREF>
              </VARIABLE-DATA-PROTOTYPE>
            </EVENTS></SERVICE-INTERFACE>
            """,
        )
        deployment = package_xml(
            "MultiDeploy",
            """
            <TRANSFORMATION-PROPS-SET><SHORT-NAME>Props</SHORT-NAME><TRANSFORMATION-PROPSS>
              <AP-SOMEIP-TRANSFORMATION-PROPS><SHORT-NAME>PayloadDeployment</SHORT-NAME>
                <SIZE-OF-ARRAY-LENGTH-FIELD>1</SIZE-OF-ARRAY-LENGTH-FIELD>
                <SIZE-OF-STRING-LENGTH-FIELD>2</SIZE-OF-STRING-LENGTH-FIELD>
                <SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>
              </AP-SOMEIP-TRANSFORMATION-PROPS>
            </TRANSFORMATION-PROPSS></TRANSFORMATION-PROPS-SET>
            <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
              <SHORT-NAME>EventMapping</SHORT-NAME>
              <EVENT-REFS><EVENT-REF DEST="VARIABLE-DATA-PROTOTYPE">
                /MultiService/Service/Event
              </EVENT-REF></EVENT-REFS>
              <TRANSFORMATION-PROPS-REF DEST="AP-SOMEIP-TRANSFORMATION-PROPS">
                /MultiDeploy/Props/PayloadDeployment
              </TRANSFORMATION-PROPS-REF>
            </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
            """,
        )
        files = {
            "types.arxml": types,
            "service.arxml": service,
            "deployment.arxml": deployment,
        }
        generated, directory = self.run_tool(
            files, "--prototype", "/MultiService/Service/Event", "--strict"
        )
        reversed_generated, _ = self.run_tool(
            dict(reversed(tuple(files.items()))),
            "--prototype",
            "/MultiService/Service/Event",
            "--strict",
        )

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertEqual(reversed_generated.returncode, 0, reversed_generated.stderr)
        self.assertIn(
            "// SOME/IP structure deployment: 2-byte length field; 1-byte alignment; big-endian.",
            generated.stdout,
        )
        self.assertIn("// AUTOSAR source: /MultiTypes/Payload/values.", generated.stdout)
        self.assertIn("// AUTOSAR source: /MultiTypes/Payload/label.", generated.stdout)
        self.assertIn("std::vector<uint8_t> values{};", generated.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(values, 1U, 4U)", generated.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(label, 2U)", generated.stdout)
        self.assertIn("// AUTOSAR maximum elements: 4.", generated.stdout)
        self.assertIn("// SOME/IP deployment: 1-byte length field.", generated.stdout)
        self.assertEqual(generated.stdout, reversed_generated.stdout)

        compiler = self.find_cpp_compiler()
        if compiler:
            header = directory / "multi_file_generated.h"
            source = directory / "multi_file_generated.cc"
            header.write_text(generated.stdout, encoding="utf-8")
            source.write_text('#include "multi_file_generated.h"\n', encoding="utf-8")
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_splits_cross_file_dependencies_into_headers(self) -> None:
        leaf = package_xml(
            "Base",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Counter</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Base/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Leaf</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <SHORT-NAME>value</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                <IMPLEMENTATION-DATA-TYPE-REF>/Base/Counter</IMPLEMENTATION-DATA-TYPE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )

        def holder(package: str, name: str, member: str, target: str) -> str:
            return package_xml(
                package,
                f"""
                <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>{name}</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
                  <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                    <SHORT-NAME>{member}</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                    <IMPLEMENTATION-DATA-TYPE-REF>{target}</IMPLEMENTATION-DATA-TYPE-REF>
                  </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
                </IMPLEMENTATION-DATA-TYPE>
                """,
            )

        left = holder("Left", "LeftNode", "leaf", "/Base/Leaf")
        right = holder("Right", "RightNode", "leaf", "/Base/Leaf")
        root = package_xml(
            "Api",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Root</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>left</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <IMPLEMENTATION-DATA-TYPE-REF>/Left/LeftNode</IMPLEMENTATION-DATA-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>right</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <IMPLEMENTATION-DATA-TYPE-REF>/Right/RightNode</IMPLEMENTATION-DATA-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        files = {
            "leaf.arxml": leaf,
            "left.arxml": left,
            "right.arxml": right,
            "root.arxml": root,
        }
        missing_output, _ = self.run_tool(
            files, "--type", "/Api/Root", "--split-by", "type"
        )
        self.assertEqual(missing_output.returncode, 2)
        self.assertIn("split output requires --output", missing_output.stderr)

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        for name, content in files.items():
            (directory / name).write_text(content, encoding="utf-8")
        for split_by, expected in (
            ("type", {"counter.h", "leaf.h", "left_node.h", "right_node.h", "root.h"}),
            ("package", {"base.h", "left.h", "right.h", "api.h"}),
        ):
            with self.subTest(split_by=split_by):
                output = directory / f"generated_{split_by}"
                output.mkdir()
                command = [
                    sys.executable,
                    str(self.tool),
                    *(str(directory / name) for name in files),
                    "--type",
                    "/Api/Root",
                    "--namespace",
                    "multi",
                    "--split-by",
                    split_by,
                    "--output",
                    str(output),
                ]
                generated = subprocess.run(command, check=False, capture_output=True, text=True)
                self.assertEqual(generated.returncode, 0, generated.stderr)
                self.assertEqual(
                    {path.name for path in output.glob("*.h")},
                    expected | {"vlink_someip_types.h"},
                )
                left_header = output / ("left_node.h" if split_by == "type" else "left.h")
                dependency = "leaf.h" if split_by == "type" else "base.h"
                self.assertIn(f'#include "{dependency}"', left_header.read_text(encoding="utf-8"))
                base_header = output / ("leaf.h" if split_by == "type" else "base.h")
                base_content = base_header.read_text(encoding="utf-8")
                if split_by == "type":
                    self.assertIn('#include "counter.h"', base_content)
                else:
                    self.assertIn("using Counter = uint8_t;", base_content)
                    self.assertIn("struct Leaf final {", base_content)
                aggregate = output / "vlink_someip_types.h"
                compiler = self.find_cpp_compiler()
                if compiler:
                    for header in sorted(output.glob("*.h")):
                        source = output / f"compile_{header.stem}.cc"
                        source.write_text(f'#include "{header.name}"\n', encoding="utf-8")
                        compiled = self.compile_cpp(
                            compiler, source, output, self.repo_root / "include"
                        )
                        self.assertEqual(compiled.returncode, 0, compiled.stderr)
                self.assertTrue(aggregate.is_file())

    def test_rejects_cross_file_recursive_types_in_split_mode(self) -> None:
        first = package_xml(
            "First",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>A</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
              <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>b</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                <IMPLEMENTATION-DATA-TYPE-REF>/Second/B</IMPLEMENTATION-DATA-TYPE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        )
        second = package_xml(
            "Second",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>B</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
              <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>a</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                <IMPLEMENTATION-DATA-TYPE-REF>/First/A</IMPLEMENTATION-DATA-TYPE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        )
        result, _ = self.run_tool(
            {"first.arxml": first, "second.arxml": second},
            "--type",
            "/First/A",
            "--split-by",
            "type",
            "--output",
            ".",
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("recursive by-value", result.stderr)

    def test_rejects_package_level_header_cycle(self) -> None:
        first = package_xml(
            "First",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>FirstValue</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/First/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>UsesSecond</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
              <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>value</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                <IMPLEMENTATION-DATA-TYPE-REF>/Second/SecondValue</IMPLEMENTATION-DATA-TYPE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        )
        second = package_xml(
            "Second",
            UINT8_BASE.replace("uint8", "byte")
            + """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>SecondValue</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Second/byte</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>UsesFirst</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
              <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>value</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                <IMPLEMENTATION-DATA-TYPE-REF>/First/FirstValue</IMPLEMENTATION-DATA-TYPE-REF>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        )
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        (directory / "first.arxml").write_text(first, encoding="utf-8")
        (directory / "second.arxml").write_text(second, encoding="utf-8")
        command = [
            sys.executable,
            str(self.tool),
            str(directory / "first.arxml"),
            str(directory / "second.arxml"),
            "--type",
            "/First/UsesSecond",
            "--type",
            "/Second/UsesFirst",
        ]
        type_output = directory / "generated_type"
        type_output.mkdir()
        type_result = subprocess.run(
            [*command, "--split-by", "type", "--output", str(type_output)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(type_result.returncode, 0, type_result.stderr)

        output = directory / "generated_package"
        output.mkdir()
        result = subprocess.run(
            [
                *command,
                "--split-by",
                "package",
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("package split creates cyclic header dependencies", result.stderr)

    def test_keeps_inline_structures_in_their_autosar_package_header(self) -> None:
        arxml = package_xml(
            "Inline",
            UINT8_BASE
            + """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/Inline/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>Outer</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY><SUB-ELEMENTS>
              <IMPLEMENTATION-DATA-TYPE-ELEMENT><SHORT-NAME>inner</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
                <SUB-ELEMENTS><IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>value</SHORT-NAME><CATEGORY>TYPE-REFERENCE</CATEGORY>
                  <IMPLEMENTATION-DATA-TYPE-REF>/Inline/Value</IMPLEMENTATION-DATA-TYPE-REF>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT></SUB-ELEMENTS>
              </IMPLEMENTATION-DATA-TYPE-ELEMENT>
            </SUB-ELEMENTS></IMPLEMENTATION-DATA-TYPE>
            """,
        )
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = directory / "inline.arxml"
        source.write_text(arxml, encoding="utf-8")
        output = directory / "generated"
        output.mkdir()
        result = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(source),
                "--type",
                "/Inline/Outer",
                "--split-by",
                "package",
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            {path.name for path in output.glob("*.h")},
            {"inline_.h", "vlink_someip_types.h"},
        )
        content = (output / "inline_.h").read_text(encoding="utf-8")
        self.assertIn("using Value = uint8_t;", content)
        self.assertIn("struct Outer_inner final {", content)
        self.assertIn("struct Outer final {", content)

    def test_avoids_windows_reserved_split_header_names(self) -> None:
        arxml = package_xml(
            "CON",
            """
            <IMPLEMENTATION-DATA-TYPE><SHORT-NAME>AUX</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = directory / "reserved.arxml"
        source.write_text(arxml, encoding="utf-8")
        for split_by, header in (("type", "aux_.h"), ("package", "con_.h")):
            output = directory / split_by
            output.mkdir()
            result = subprocess.run(
                [
                    sys.executable,
                    str(self.tool),
                    str(source),
                    "--type",
                    "/CON/AUX",
                    "--split-by",
                    split_by,
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((output / header).is_file())

    def test_rejects_conflicting_split_type_definitions(self) -> None:
        first = package_xml(
            "Split",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>Value</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
              <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint8</BASE-TYPE-REF>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        different_tag = package_xml(
            "Split",
            """
            <APPLICATION-PRIMITIVE-DATA-TYPE>
              <SHORT-NAME>Value</SHORT-NAME><CATEGORY>UINT8</CATEGORY>
            </APPLICATION-PRIMITIVE-DATA-TYPE>
            """,
        )
        conflicting_leaf = first.replace("<CATEGORY>VALUE</CATEGORY>", "<CATEGORY>BOOLEAN</CATEGORY>")
        cases = (
            (different_tag, "different element types"),
            (conflicting_leaf, "conflicting 'CATEGORY' values"),
        )

        for index, (second, expected) in enumerate(cases):
            with self.subTest(expected=expected):
                result, _ = self.run_tool(
                    {"first.arxml": first, f"second_{index}.arxml": second}
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected, result.stderr)

    def test_rejects_unsupported_someip_deployments(self) -> None:
        base = someip_length_deployment_xml()
        legacy = base.replace(
            "<SHORT-NAME>Defaults</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>",
            "<SHORT-NAME>Defaults</SHORT-NAME><ALIGNMENT>8</ALIGNMENT>"
            "<IMPLEMENTS-LEGACY-STRING-SERIALIZATION>true</IMPLEMENTS-LEGACY-STRING-SERIALIZATION>",
            1,
        )
        result, _ = self.run_tool(
            {"legacy.arxml": legacy}, "--prototype", "/Deploy/Service/Event"
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("legacy SOME/IP string serialization", result.stderr)

        network = base.replace(
            "</DATA-PROTOTYPES>\n          <SOMEIP-TRANSFORMATION-PROPS-REF>/Deploy/NoArrayLength",
            "</DATA-PROTOTYPES>"
            "<NETWORK-REPRESENTATION><BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>"
            "</NETWORK-REPRESENTATION>"
            "<SOMEIP-TRANSFORMATION-PROPS-REF>/Deploy/NoArrayLength",
            1,
        )
        network_result, _ = self.run_tool(
            {"network.arxml": network}, "--prototype", "/Deploy/Service/Event"
        )
        self.assertEqual(network_result.returncode, 2)
        self.assertIn("NETWORK-REPRESENTATION", network_result.stderr)

        network_only = base.replace(
            """
        <TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>
          <SHORT-NAME>EventDefaults</SHORT-NAME>
          <EVENT-REFS><EVENT-REF>/Deploy/Service/Event</EVENT-REF></EVENT-REFS>
          <TRANSFORMATION-PROPS-REF>/Deploy/Defaults</TRANSFORMATION-PROPS-REF>
        </TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING>""",
            "",
        ).replace(
            "<SOMEIP-TRANSFORMATION-PROPS-REF>/Deploy/NoArrayLength"
            "</SOMEIP-TRANSFORMATION-PROPS-REF>",
            "<NETWORK-REPRESENTATION><BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>"
            "</NETWORK-REPRESENTATION>",
        )
        network_type, _ = self.run_tool(
            {"network_type.arxml": network_only}, "--type", "/Deploy/Payload", "--strict"
        )
        self.assertEqual(network_type.returncode, 2)
        self.assertIn("NETWORK-REPRESENTATION", network_type.stderr)

        contextual = base.replace(
            "<PORT-INTERFACE-REF>/Deploy/Service</PORT-INTERFACE-REF>",
            "<CONTEXT-DATA-PROTOTYPE-REFS><CONTEXT-DATA-PROTOTYPE-REF>"
            "/Deploy/Payload/child</CONTEXT-DATA-PROTOTYPE-REF>"
            "</CONTEXT-DATA-PROTOTYPE-REFS>"
            "<PORT-INTERFACE-REF>/Deploy/Service</PORT-INTERFACE-REF>",
            1,
        )
        contextual_result, _ = self.run_tool(
            {"context.arxml": contextual}, "--prototype", "/Deploy/Service/Event", "--strict"
        )
        self.assertEqual(contextual_result.returncode, 2)
        self.assertIn("CONTEXT-DATA-PROTOTYPE-REFS", contextual_result.stderr)

    def test_generates_utf16_deployment(self) -> None:
        for encoding, suffix in (("UTF-16BE", "BE"), ("UTF-16LE", "LE")):
            with self.subTest(encoding=encoding):
                arxml = someip_length_deployment_xml().replace(
                    "<SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>\n"
                    "        </AP-SOMEIP-TRANSFORMATION-PROPS>",
                    "<SIZE-OF-STRUCT-LENGTH-FIELD>2</SIZE-OF-STRUCT-LENGTH-FIELD>"
                    f"<STRING-ENCODING>{encoding}</STRING-ENCODING>\n"
                    "        </AP-SOMEIP-TRANSFORMATION-PROPS>",
                    1,
                )
                result, _ = self.run_tool(
                    {"encoding.arxml": arxml}, "--prototype", "/Deploy/Service/Event", "--strict"
                )

                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("std::u16string name{};", result.stdout)
                self.assertIn(f"VLINK_SOMEIP_UTF16_{suffix}(name, 1U)", result.stdout)
                self.assertIn("std::u16string title{};", result.stdout)
                self.assertIn(f"VLINK_SOMEIP_UTF16_{suffix}(title, 1U)", result.stdout)

    def test_reports_malformed_xml_without_traceback(self) -> None:
        result, _ = self.run_tool({"broken.arxml": "<AUTOSAR><AR-PACKAGES>"})

        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot parse", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_generated_header_is_valid_cpp17_someip_type(self) -> None:
        compiler = self.find_cpp_compiler()
        if not compiler:
            self.skipTest("no C++ compiler is available")

        arxml = package_xml(
            "Smoke",
            """
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>SmokeMessage</SHORT-NAME><CATEGORY>STRUCTURE</CATEGORY>
              <SUB-ELEMENTS>
                <IMPLEMENTATION-DATA-TYPE-ELEMENT>
                  <SHORT-NAME>counter</SHORT-NAME><CATEGORY>VALUE</CATEGORY>
                  <SW-DATA-DEF-PROPS><SW-DATA-DEF-PROPS-VARIANTS><SW-DATA-DEF-PROPS-CONDITIONAL>
                    <BASE-TYPE-REF>/AUTOSAR/PlatformTypes/uint32</BASE-TYPE-REF>
                  </SW-DATA-DEF-PROPS-CONDITIONAL></SW-DATA-DEF-PROPS-VARIANTS></SW-DATA-DEF-PROPS>
                </IMPLEMENTATION-DATA-TYPE-ELEMENT>
              </SUB-ELEMENTS>
            </IMPLEMENTATION-DATA-TYPE>
            """,
        )
        generated, directory = self.run_tool({"smoke.arxml": arxml})
        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertIn("#include <vlink/extension/someip_serializer.h>", generated.stdout)
        self.assertIn("#include <vlink/serializer.h>", generated.stdout)
        self.assertNotIn("vlink/impl/someip_serializer.h", generated.stdout)
        self.assertNotIn("vlink/vlink.h", generated.stdout)

        header = directory / "generated.h"
        source = directory / "smoke.cc"
        header.write_text(generated.stdout, encoding="utf-8")
        source.write_text(
            textwrap.dedent(
                """\
                #include "generated.h"

                static_assert(vlink::Serializer::get_type_of<SmokeMessage>() ==
                              vlink::Serializer::kSomeipType);
                """
            ),
            encoding="utf-8",
        )
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_tool_runs_in_isolated_python_without_third_party_packages(self) -> None:
        generated = subprocess.run(
            [
                sys.executable,
                "-I",
                "-S",
                str(self.tool),
                str(self.fixture),
                "--type",
                "/VLink/ImplementationTypes/VehicleState",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertIn("struct VehicleState final {", generated.stdout)

    def test_committed_r25_11_fixtures_validate_against_the_official_schema(self) -> None:
        schema = self.find_autosar_schema()
        if schema is None:
            self.skipTest("AUTOSAR_R25_11_XSD is not available")
        xmllint = shutil.which("xmllint")
        if not xmllint:
            self.skipTest("xmllint is not available")

        for fixture in (
            self.fixture,
            self.features_fixture,
            self.matrix_fixture,
            self.advanced_fixture,
        ):
            with self.subTest(fixture=fixture.name):
                validated = subprocess.run(
                    [xmllint, "--noout", "--schema", str(schema), str(fixture)],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(validated.returncode, 0, validated.stderr)

    def test_r25_11_deployment_matrix_generates_compiles_and_round_trips_108_samples(self) -> None:
        baseline = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.matrix_fixture),
                "--prototype",
                "/Matrix/ServiceInterfaces/MatrixService/SimpleEvent",
                "--namespace",
                "vlink::autosar::matrix",
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(baseline.returncode, 0, baseline.stderr)
        self.assertEqual(baseline.stdout, self.matrix_golden.read_text(encoding="utf-8"))

        compiler = self.find_cpp_compiler()
        if not compiler:
            self.skipTest("no C++ compiler is available")

        schema = self.find_autosar_schema()
        xmllint = shutil.which("xmllint")
        if schema is not None:
            self.assertIsNotNone(xmllint, "xmllint is required when an AUTOSAR R25-11 XSD is available")

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        fixture = self.matrix_fixture.read_text(encoding="utf-8")
        source_lines = [
            "#include <array>",
            "#include <cstdint>",
            "#include <cstring>",
            "#include <string>",
            "#include <vector>",
            "",
        ]
        checks: List[str] = []
        schema_validated = 0

        widths = (1, 2, 4)
        endian_modes = (
            ("big", "MOST-SIGNIFICANT-BYTE-FIRST"),
            ("little", "MOST-SIGNIFICANT-BYTE-LAST"),
        )
        for index, (array_width, string_width, struct_width, endian_mode, alignment) in enumerate(
            product(widths, widths, widths, endian_modes, (1, 2))
        ):
            endian_name, byte_order = endian_mode
            sample = (
                fixture.replace("<ALIGNMENT>8</ALIGNMENT>", f"<ALIGNMENT>{alignment * 8}</ALIGNMENT>")
                .replace(
                    "<BYTE-ORDER>MOST-SIGNIFICANT-BYTE-FIRST</BYTE-ORDER>",
                    f"<BYTE-ORDER>{byte_order}</BYTE-ORDER>",
                )
                .replace(
                    "<SIZE-OF-ARRAY-LENGTH-FIELD>1</SIZE-OF-ARRAY-LENGTH-FIELD>",
                    f"<SIZE-OF-ARRAY-LENGTH-FIELD>{array_width}</SIZE-OF-ARRAY-LENGTH-FIELD>",
                )
                .replace(
                    "<SIZE-OF-STRING-LENGTH-FIELD>1</SIZE-OF-STRING-LENGTH-FIELD>",
                    f"<SIZE-OF-STRING-LENGTH-FIELD>{string_width}</SIZE-OF-STRING-LENGTH-FIELD>",
                )
                .replace(
                    "<SIZE-OF-STRUCT-LENGTH-FIELD>1</SIZE-OF-STRUCT-LENGTH-FIELD>",
                    f"<SIZE-OF-STRUCT-LENGTH-FIELD>{struct_width}</SIZE-OF-STRUCT-LENGTH-FIELD>",
                )
            )
            sample_name = f"sample_{index:03d}"
            arxml = directory / f"{sample_name}.arxml"
            header = directory / f"{sample_name}.h"
            arxml.write_text(sample, encoding="utf-8")

            if schema is not None:
                validated = subprocess.run(
                    [xmllint, "--noout", "--schema", str(schema), str(arxml)],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(validated.returncode, 0, validated.stderr)
                schema_validated += 1

            namespace = f"vlink::autosar::matrix::{sample_name}"
            generated = subprocess.run(
                [
                    sys.executable,
                    str(self.tool),
                    str(arxml),
                    "--prototype",
                    "/Matrix/ServiceInterfaces/MatrixService/SimpleEvent",
                    "--namespace",
                    namespace,
                    "--strict",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(generated.returncode, 0, generated.stderr)
            header.write_text(generated.stdout, encoding="utf-8")
            source_lines.append(f'#include "{sample_name}.h"')

            body = bytearray()
            scalar_order = "big" if endian_name == "big" else "little"
            body.extend((0x1234).to_bytes(2, scalar_order))
            string_body = b"\xEF\xBB\xBFA\x00"
            body.extend(len(string_body).to_bytes(string_width, "big"))
            body.extend(string_body)
            body.extend(b"\x00" * (-(16 + struct_width + len(body)) % alignment))
            array_body = b"\x56\x78"
            body.extend(len(array_body).to_bytes(array_width, "big"))
            body.extend(array_body)
            body.extend(b"\x00" * (-(16 + struct_width + len(body)) % alignment))
            body.extend((0x9ABC).to_bytes(2, scalar_order))
            expected = len(body).to_bytes(struct_width, "big") + body
            expected_values = ", ".join(f"0x{value:02X}U" for value in expected)
            source_lines.extend(
                [
                    "",
                    f"static bool check_{sample_name}() {{",
                    f"  using Payload = {namespace}::SimplePayload;",
                    "  Payload source;",
                    "  source.sequence = 0x1234U;",
                    '  source.name = "A";',
                    "  source.values = {0x56U, 0x78U};",
                    "  source.tail = 0x9ABCU;",
                    "  vlink::Bytes encoded;",
                    "  if (!vlink::Serializer::serialize(source, encoded)) {",
                    "    return false;",
                    "  }",
                    f"  const std::array<uint8_t, {len(expected)}> expected = {{{expected_values}}};",
                    "  if (encoded.size() != expected.size() ||",
                    "      std::memcmp(encoded.data(), expected.data(), expected.size()) != 0) {",
                    "    return false;",
                    "  }",
                    "  Payload target;",
                    "  if (!vlink::Serializer::deserialize(encoded, target)) {",
                    "    return false;",
                    "  }",
                    "  if (target.sequence != source.sequence || target.name != source.name ||",
                    "      target.values != source.values || target.tail != source.tail) {",
                    "    return false;",
                    "  }",
                    "  const auto truncated = vlink::Bytes::shallow_copy(expected.data(), expected.size() - 1U);",
                    "  return !vlink::Serializer::deserialize(truncated, target);",
                    "}",
                ]
            )
            checks.append(f"  if (!check_{sample_name}()) {{ return {index + 1}; }}")

        self.assertEqual(len(checks), 108)
        if schema is not None:
            self.assertEqual(schema_validated, 108)
        source_lines.extend(["", "int main() {", *checks, "  return 0;", "}"])
        source = directory / "someip_matrix.cc"
        source.write_text("\n".join(source_lines) + "\n", encoding="utf-8")

        linker_value = os.environ.get("VLINK_AUTOSAR_TEST_LINKER_FILE")
        runtime_value = os.environ.get("VLINK_AUTOSAR_TEST_RUNTIME_DIR")
        if linker_value and runtime_value:
            compiled, executed = self.compile_and_run_cpp(
                compiler,
                source,
                Path(linker_value),
                Path(runtime_value),
                directory,
                self.repo_root / "include",
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertIsNotNone(executed)
            self.assertEqual(executed.returncode, 0, executed.stderr)
        else:
            compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_r25_11_advanced_features_generate_compile_and_round_trip(self) -> None:
        generated = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.advanced_fixture),
                "--prototype",
                "/Advanced/ServiceInterfaces/AdvancedService/Event",
                "--prototype",
                "/Advanced/ServiceInterfaces/AdvancedService/TlvEvent",
                "--namespace",
                "vlink::autosar::advanced",
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertEqual(generated.stdout, self.advanced_golden.read_text(encoding="utf-8"))
        self.assertIn("using RecordArray = std::array<RecordArray_element, 2>;", generated.stdout)

        compiler = self.find_cpp_compiler()
        if not compiler:
            self.skipTest("no C++ compiler is available")

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source = directory / "someip_advanced.cc"
        source.write_text(
            textwrap.dedent(
                """\
                #include <array>
                #include <cstdint>
                #include <cstring>
                #include <variant>

                #include "generated_someip_advanced.h"

                namespace {

                template <size_t SizeT>
                bool matches(const vlink::Bytes& value, const std::array<uint8_t, SizeT>& expected) {
                  return value.size() == expected.size() &&
                         std::memcmp(value.data(), expected.data(), expected.size()) == 0;
                }

                bool check_advanced_payload() {
                  using vlink::autosar::advanced::AdvancedPayload;

                  AdvancedPayload source;
                  source.title = u"A";
                  source.lookup.emplace(0x1234U, 0x01020304U);
                  source.samples = {0x1111U, 0x2222U};
                  source.choice = uint16_t{0x5678U};

                  vlink::Bytes encoded;
                  const std::array<uint8_t, 29> expected = {
                      0x00U, 0x1BU, 0x00U, 0x06U, 0xFEU, 0xFFU, 0x00U, 0x41U,
                      0x00U, 0x00U, 0x00U, 0x06U, 0x12U, 0x34U, 0x01U, 0x02U,
                      0x03U, 0x04U, 0x00U, 0x04U, 0x11U, 0x11U, 0x22U, 0x22U,
                      0x00U, 0x02U, 0x01U, 0x56U, 0x78U,
                  };
                  if (!vlink::Serializer::serialize(source, encoded) || !matches(encoded, expected)) {
                    return false;
                  }

                  AdvancedPayload target;
                  if (!vlink::Serializer::deserialize(encoded, target) || target.title != source.title ||
                      target.lookup != source.lookup || target.samples != source.samples ||
                      !std::holds_alternative<uint16_t>(target.choice) ||
                      std::get<uint16_t>(target.choice) != 0x5678U) {
                    return false;
                  }

                  const std::array<uint8_t, 30> excess = {
                      0x00U, 0x1CU, 0x00U, 0x06U, 0xFEU, 0xFFU, 0x00U, 0x41U,
                      0x00U, 0x00U, 0x00U, 0x06U, 0x12U, 0x34U, 0x01U, 0x02U,
                      0x03U, 0x04U, 0x00U, 0x05U, 0x11U, 0x11U, 0x22U, 0x22U,
                      0xAAU, 0x00U, 0x02U, 0x01U, 0x56U, 0x78U,
                  };
                  const auto excess_data = vlink::Bytes::shallow_copy(excess.data(), excess.size());
                  if (!vlink::Serializer::deserialize(excess_data, target) || target.samples != source.samples ||
                      !std::holds_alternative<uint16_t>(target.choice) ||
                      std::get<uint16_t>(target.choice) != 0x5678U) {
                    return false;
                  }

                  source.title = u"ABCDE";
                  if (vlink::Serializer::serialize(source, encoded)) {
                    return false;
                  }

                  const auto truncated = vlink::Bytes::shallow_copy(expected.data(), expected.size() - 1U);
                  return !vlink::Serializer::deserialize(truncated, target);
                }

                bool check_tlv_payload() {
                  using vlink::autosar::advanced::Cube;
                  using vlink::autosar::advanced::RecordArray;
                  using vlink::autosar::advanced::TlvPayload;

                  TlvPayload source;
                  source.label = "A";
                  source.code = 0x7FU;
                  source.lookup.emplace().emplace(0x1234U, 0x01020304U);
                  Cube cube{};
                  cube[0][0] = {0x01U, 0x02U};
                  cube[0][1] = {0x03U, 0x04U};
                  cube[1][0] = {0x05U, 0x06U};
                  cube[1][1] = {0x07U, 0x08U};
                  source.values = cube;
                  RecordArray records{};
                  records[0].value = 0x09U;
                  records[1].value = 0x0AU;
                  source.records = records;

                  vlink::Bytes encoded;
                  const std::array<uint8_t, 54> expected = {
                      0x00U, 0x34U, 0x50U, 0x01U, 0x05U, 0xEFU, 0xBBU, 0xBFU,
                      0x41U, 0x00U, 0x00U, 0x02U, 0x7FU, 0x50U, 0x03U, 0x06U,
                      0x12U, 0x34U, 0x01U, 0x02U, 0x03U, 0x04U, 0x50U, 0x04U,
                      0x14U, 0x00U, 0x08U, 0x00U, 0x02U, 0x01U, 0x02U, 0x00U,
                      0x02U, 0x03U, 0x04U, 0x00U, 0x08U, 0x00U, 0x02U, 0x05U,
                      0x06U, 0x00U, 0x02U, 0x07U, 0x08U, 0x50U, 0x05U, 0x06U,
                      0x00U, 0x01U, 0x09U, 0x00U, 0x01U, 0x0AU,
                  };
                  if (!vlink::Serializer::serialize(source, encoded) || !matches(encoded, expected)) {
                    return false;
                  }

                  TlvPayload target;
                  if (!vlink::Serializer::deserialize(encoded, target) || target.label != source.label ||
                      target.code != source.code || target.lookup != source.lookup || target.values != source.values ||
                      !target.records || (*target.records)[0].value != records[0].value ||
                      (*target.records)[1].value != records[1].value) {
                    return false;
                  }

                  TlvPayload required_only;
                  required_only.code = 0x7FU;
                  const std::array<uint8_t, 5> absent = {0x00U, 0x03U, 0x00U, 0x02U, 0x7FU};
                  target = source;
                  if (!vlink::Serializer::serialize(required_only, encoded) || !matches(encoded, absent) ||
                      !vlink::Serializer::deserialize(encoded, target) || target.label || target.lookup ||
                      target.values || target.records || target.code != required_only.code) {
                    return false;
                  }

                  const auto truncated = vlink::Bytes::shallow_copy(expected.data(), expected.size() - 1U);
                  return !vlink::Serializer::deserialize(truncated, target);
                }

                }  // namespace

                int main() { return check_advanced_payload() && check_tlv_payload() ? 0 : 1; }
                """
            ),
            encoding="utf-8",
        )

        linker_value = os.environ.get("VLINK_AUTOSAR_TEST_LINKER_FILE")
        runtime_value = os.environ.get("VLINK_AUTOSAR_TEST_RUNTIME_DIR")
        if linker_value and runtime_value:
            compiled, executed = self.compile_and_run_cpp(
                compiler,
                source,
                Path(linker_value),
                Path(runtime_value),
                self.advanced_golden.parent,
                self.repo_root / "include",
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            self.assertIsNotNone(executed)
            self.assertEqual(executed.returncode, 0, executed.stderr)
        else:
            compiled = self.compile_cpp(
                compiler, source, self.advanced_golden.parent, self.repo_root / "include"
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_r25_11_feature_fixture_generates_fixed_strings_with_deployment_comments(self) -> None:
        generated = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.features_fixture),
                "--prototype",
                "/Features/Service/Event",
                "--prototype",
                "/Features/Service/TlvEvent",
                "--prototype",
                "/Features/Service/StaticTlvEvent",
                "--namespace",
                "vlink::autosar::features",
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertIn("// AUTOSAR fixed wire size: 11 bytes.", generated.stdout)
        self.assertIn("// AUTOSAR maximum text size: 4 code points.", generated.stdout)
        self.assertIn("// SOME/IP deployment: 1-byte length field.", generated.stdout)
        self.assertIn("// SOME/IP deployment: 1-byte length field; UTF-16BE.", generated.stdout)
        self.assertIn("VLINK_SOMEIP_FIXED_STRING_MAX(text8, 11U, 1U, 4U)", generated.stdout)
        self.assertIn("VLINK_SOMEIP_FIXED_UTF16_BE_MAX(text16, 10U, 1U, 3U)", generated.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_FIXED_STRING_MAX(1, text8, 11U, 1U", generated.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_FIXED_STRING_MAX(2, text16, 10U, 1U", generated.stdout)
        self.assertIn("VLINK_SOMEIP_TLV_STATIC_FIXED_STRING_MAX(1, text8, 11U, 1U", generated.stdout)
        self.assertEqual(generated.stdout, self.features_golden.read_text(encoding="utf-8"))

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        header = directory / "someip_features_generated.h"
        source = directory / "features.cc"
        header.write_text(generated.stdout, encoding="utf-8")
        source.write_text('#include "someip_features_generated.h"\n', encoding="utf-8")
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_r25_11_fixture_reproduces_committed_header(self) -> None:
        generated = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.fixture),
                "--prototype",
                "/VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent",
                "--namespace",
                "vlink::autosar",
                "--byte-arrays-as-bytes",
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertEqual(generated.stdout, self.golden.read_text(encoding="utf-8"))
        self.assertIn("// AUTOSAR maximum text size: 7 code points.", generated.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(name, 2U, 7U)", generated.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(objects, 2U, 2U)", generated.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH_MAX(payload, 1U, 4U)", generated.stdout)
        self.assertIn(
            "// AUTOSAR INIT-VALUE source: /VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent.",
            generated.stdout,
        )
        self.assertIn("[[nodiscard]] static VehicleState make_default()", generated.stdout)
        self.assertNotIn("make_vehicle_state_event_initial_value", generated.stdout)

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        header = directory / "generated.h"
        source = directory / "make_default.cc"
        header.write_text(generated.stdout, encoding="utf-8")
        source.write_text(
            '#include "generated.h"\n'
            "auto make_state() { return vlink::autosar::VehicleState::make_default(); }\n",
            encoding="utf-8",
        )
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

        split_directory = directory / "split"
        split_directory.mkdir()
        split = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.fixture),
                "--prototype",
                "/VLink/ServiceInterfaces/VehicleStateService/VehicleStateEvent",
                "--namespace",
                "vlink::autosar",
                "--byte-arrays-as-bytes",
                "--strict",
                "--split-by",
                "type",
                "--output",
                str(split_directory),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(split.returncode, 0, split.stderr)
        split_source = split_directory / "make_default.cc"
        split_source.write_text(
            '#include "vlink_someip_types.h"\n'
            "auto make_state() { return vlink::autosar::VehicleState::make_default(); }\n",
            encoding="utf-8",
        )
        split_compiled = self.compile_cpp(
            compiler, split_source, split_directory, self.repo_root / "include"
        )
        self.assertEqual(split_compiled.returncode, 0, split_compiled.stderr)

    def test_r25_11_fixture_generates_all_types_in_strict_mode(self) -> None:
        generated = subprocess.run(
            [
                sys.executable,
                str(self.tool),
                str(self.fixture),
                "--namespace",
                "vlink::autosar_all",
                "--byte-arrays-as-bytes",
                "--strict",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(generated.returncode, 0, generated.stderr)
        self.assertEqual(generated.stderr, "")
        self.assertIn("using PayloadBytesApplication = vlink::Bytes;", generated.stdout)

        compiler = self.find_cpp_compiler()
        if not compiler:
            return
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        header = directory / "all_types.h"
        source = directory / "all_types.cc"
        header.write_text(generated.stdout, encoding="utf-8")
        source.write_text('#include "all_types.h"\n', encoding="utf-8")
        compiled = self.compile_cpp(compiler, source, directory, self.repo_root / "include")
        self.assertEqual(compiled.returncode, 0, compiled.stderr)


if __name__ == "__main__":
    unittest.main()
