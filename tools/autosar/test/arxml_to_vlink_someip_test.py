#!/usr/bin/env python3
"""Tests for tools/autosar/arxml_to_vlink_someip.py."""

import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Dict, List, Tuple


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


class ArxmlToVlinkSomeipTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        test_dir = Path(__file__).resolve().parent
        autosar_dir = test_dir.parent
        cls.repo_root = autosar_dir.parents[1]
        cls.tool = autosar_dir / "arxml_to_vlink_someip.py"
        cls.fixture = test_dir / "autosar_r25_11_someip_types.arxml"
        cls.golden = test_dir / "generated_someip_types.h"

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
        self.assertIn("enum class Mode : uint16_t {", result.stdout)
        self.assertIn("kOff = 0,", result.stdout)
        self.assertIn("kActive = 1,\n};", result.stdout)
        self.assertIn("using FixedSamples = std::array<uint16_t, 3>;", result.stdout)
        self.assertIn("vlink::Bytes payload{};", result.stdout)
        self.assertIn("std::string label{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIELDS(mode, samples, payload, label)", result.stdout)

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
        self.assertIn("struct ImplMessage final {", result.stdout)
        self.assertIn("using AppMessage = ImplMessage;", result.stdout)
        self.assertNotIn("unusedModelField", result.stdout)

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
        self.assertIn("VLINK_SOMEIP_ALIGNMENT(4U)", result.stdout)
        self.assertIn("bool valid{};", result.stdout)
        self.assertIn("float temperature{};", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIELDS(samples, history, name, valid, temperature)", result.stdout)
        self.assertLess(
            result.stdout.index("float temperature{};"),
            result.stdout.index("VLINK_SOMEIP_ALIGNMENT(4U)"),
        )
        self.assertLess(
            result.stdout.index("VLINK_SOMEIP_ALIGNMENT(4U)"),
            result.stdout.index("VLINK_SOMEIP_FIELDS(samples, history, name, valid, temperature)"),
        )

        compiler = os.environ.get("CXX") or shutil.which("c++")
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
            compiled = subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-fsyntax-only",
                    "-I",
                    str(directory),
                    "-I",
                    str(self.repo_root / "include"),
                    str(source),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_rejects_optional_adaptive_member_that_requires_tlv(self) -> None:
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
        self.assertIn("optional members require TLV", result.stderr)

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
        self.assertEqual(rejected.returncode, 2)
        self.assertIn("maximum is 3", rejected.stderr)

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
            """,
        )
        result, _ = self.run_tool({"nested.arxml": arxml}, "--type", "Container")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("struct Container_child final {", result.stdout)
        self.assertIn("VLINK_SOMEIP_FIELDS(state)", result.stdout)
        self.assertIn("std::array<std::array<uint8_t, 3>, 2> matrix{};", result.stdout)
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

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("using First_Status = uint8_t;", result.stdout)
        self.assertIn("using Second_Status = uint16_t;", result.stdout)

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
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if compiler:
            header = directory / "members_generated.h"
            source = directory / "members_generated.cc"
            header.write_text(result.stdout, encoding="utf-8")
            source.write_text('#include "members_generated.h"\n', encoding="utf-8")
            compiled = subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-fsyntax-only",
                    "-I",
                    str(directory),
                    "-I",
                    str(self.repo_root / "include"),
                    str(source),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
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

    def test_default_mode_skips_union_but_strict_mode_rejects_warning(self) -> None:
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
        self.assertIn("does not support union", result.stderr)
        self.assertEqual(strict.returncode, 2)
        self.assertIn("strict mode rejected", strict.stderr)

    def test_explicit_union_and_incompatible_deployment_fail(self) -> None:
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
              <SHORT-NAME>ArrayDeployment</SHORT-NAME><ARRAY-LENGTH-FIELD-SIZE>16</ARRAY-LENGTH-FIELD-SIZE>
            </SOMEIP-ARRAY-DEPLOYMENT>
            """,
        )
        union_result, _ = self.run_tool({"deploy.arxml": arxml}, "--type", "Choice")
        strict_result, _ = self.run_tool({"deploy.arxml": arxml}, "--type", "Value", "--strict")

        self.assertEqual(union_result.returncode, 2)
        self.assertIn("does not support union", union_result.stderr)
        self.assertEqual(strict_result.returncode, 2)
        self.assertIn("ARRAY-LENGTH-FIELD-SIZE=16", strict_result.stderr)

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
        self.assertIn("VLINK_SOMEIP_LENGTH(dynamic, 2U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(name, 1U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(title, 1U)", result.stdout)
        self.assertIn("VLINK_SOMEIP_LENGTH(history, 2U)", result.stdout)
        self.assertNotIn("make_event_initial_value", result.stdout)

        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            return
        header = directory / "deployment_generated.h"
        source = directory / "deployment_generated.cc"
        header.write_text(result.stdout, encoding="utf-8")
        source.write_text('#include "deployment_generated.h"\n', encoding="utf-8")
        compiled = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-fsyntax-only",
                "-I",
                str(directory),
                "-I",
                str(self.repo_root / "include"),
                str(source),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)

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
        self.assertIn("VLINK_SOMEIP_LENGTH(dynamic, 2U)", result.stdout)

    def test_applies_field_and_method_direction_deployment_mappings(self) -> None:
        cases = {
            "field": service_element_deployment_xml(
                """
                <FIELDS><FIELD>
                  <SHORT-NAME>State</SHORT-NAME><TYPE-TREF>/ServiceDeploy/Payload</TYPE-TREF>
                </FIELD></FIELDS>
                """,
                """
                <FIELD-REFS><FIELD-REF>/ServiceDeploy/Service/State</FIELD-REF></FIELD-REFS>
                """,
            ),
            "method_call": service_element_deployment_xml(
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
            "method_return": service_element_deployment_xml(
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
            "method_inout": service_element_deployment_xml(
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
        }
        for name, arxml in cases.items():
            with self.subTest(name=name):
                result, _ = self.run_tool(
                    {f"{name}.arxml": arxml}, "--type", "Payload", "--strict"
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

    def test_requires_alignment_override_for_multiple_someip_deployments(self) -> None:
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

        self.assertEqual(ambiguous.returncode, 2)
        self.assertIn("multiple SOME/IP alignments", ambiguous.stderr)
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
        overwrite = subprocess.run(
            [sys.executable, str(self.tool), str(input_path), "--output", str(input_path)],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn("/List/Counter\tAPPLICATION-PRIMITIVE-DATA-TYPE", listed.stdout)
        self.assertEqual(overwrite.returncode, 2)
        self.assertIn("must not overwrite", overwrite.stderr)

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

    def test_parses_autosar_octal_float_and_rejects_recursive_or_fixed_string(self) -> None:
        arxml = package_xml(
            "Compat",
            UINT8_BASE
            + """
            <SW-BASE-TYPE>
              <SHORT-NAME>MyReal</SHORT-NAME><BASE-TYPE-SIZE>64</BASE-TYPE-SIZE>
              <BASE-TYPE-ENCODING>FLOAT</BASE-TYPE-ENCODING>
            </SW-BASE-TYPE>
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
            <IMPLEMENTATION-DATA-TYPE>
              <SHORT-NAME>FixedText</SHORT-NAME><CATEGORY>STRING</CATEGORY>
              <SW-TEXT-PROPS><ENCODING>UTF-8</ENCODING>
                <ARRAY-SIZE-SEMANTICS>FIXED-SIZE</ARRAY-SIZE-SEMANTICS>
              </SW-TEXT-PROPS>
            </IMPLEMENTATION-DATA-TYPE>
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
        self.assertIn("fixed-length strings", fixed_string.stderr)

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

    def test_merges_split_types_and_indexes_standalone_package(self) -> None:
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
        split, _ = self.run_tool(
            {"first.arxml": first, "second.arxml": second}, "--type", "/Split/Payload"
        )

        self.assertEqual(split.returncode, 0, split.stderr)
        self.assertIn("uint8_t first{};", split.stdout)
        self.assertIn("uint8_t second{};", split.stdout)

        package_start = first.index("<AR-PACKAGE>")
        package_end = first.index("</AR-PACKAGE>") + len("</AR-PACKAGE>")
        standalone, _ = self.run_tool(
            {"standalone.arxml": first[package_start:package_end]}, "--type", "/Split/Payload"
        )
        self.assertEqual(standalone.returncode, 0, standalone.stderr)

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

        tlv = base.replace(
            "<SHORT-NAME>EventDefaults</SHORT-NAME>",
            "<SHORT-NAME>EventDefaults</SHORT-NAME>"
            "<TLV-DATA-ID-DEFINITION-REFS><TLV-DATA-ID-DEFINITION-REF>/Deploy/Tlv</TLV-DATA-ID-DEFINITION-REF>"
            "</TLV-DATA-ID-DEFINITION-REFS>",
            1,
        )
        tlv_result, _ = self.run_tool(
            {"tlv.arxml": tlv}, "--prototype", "/Deploy/Service/Event"
        )
        self.assertEqual(tlv_result.returncode, 2)
        self.assertIn("TLV data-ID deployment", tlv_result.stderr)

        network = base.replace(
            "<SHORT-NAME>FixedOverride</SHORT-NAME>",
            "<SHORT-NAME>FixedOverride</SHORT-NAME>"
            "<NETWORK-REPRESENTATION><BASE-TYPE-REF>/Deploy/uint8</BASE-TYPE-REF>"
            "</NETWORK-REPRESENTATION>",
            1,
        )
        network_result, _ = self.run_tool(
            {"network.arxml": network}, "--prototype", "/Deploy/Service/Event"
        )
        self.assertEqual(network_result.returncode, 2)
        self.assertIn("NETWORK-REPRESENTATION", network_result.stderr)

    def test_reports_malformed_xml_without_traceback(self) -> None:
        result, _ = self.run_tool({"broken.arxml": "<AUTOSAR><AR-PACKAGES>"})

        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot parse", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_generated_header_is_valid_cpp17_someip_type(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("c++")
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
        compiled = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-fsyntax-only",
                "-I",
                str(directory),
                "-I",
                str(self.repo_root / "include"),
                str(source),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
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

        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            return
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        header = directory / "all_types.h"
        source = directory / "all_types.cc"
        header.write_text(generated.stdout, encoding="utf-8")
        source.write_text('#include "all_types.h"\n', encoding="utf-8")
        compiled = subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-fsyntax-only",
                "-I",
                str(directory),
                "-I",
                str(self.repo_root / "include"),
                str(source),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stderr)


if __name__ == "__main__":
    unittest.main()
