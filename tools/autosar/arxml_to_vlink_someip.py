#!/usr/bin/env python3
"""Generate VLink SOME/IP C++ payload types from AUTOSAR ARXML files."""

import argparse
import copy
import math
import re
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

TYPE_TAGS = {
    "APPLICATION-ARRAY-DATA-TYPE",
    "APPLICATION-PRIMITIVE-DATA-TYPE",
    "APPLICATION-RECORD-DATA-TYPE",
    "APPLICATION-VALUE-DATA-TYPE",
    "CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE",
    "IMPLEMENTATION-DATA-TYPE",
    "STD-CPP-IMPLEMENTATION-DATA-TYPE",
}

PROTOTYPE_TAGS = {
    "ARGUMENT-DATA-PROTOTYPE",
    "PARAMETER-DATA-PROTOTYPE",
    "VARIABLE-DATA-PROTOTYPE",
}

SERVICE_PROTOTYPE_TAGS = PROTOTYPE_TAGS | {"FIELD"}

SOMEIP_PROPS_TAGS = {
    "AP-SOMEIP-TRANSFORMATION-PROPS",
    "SOMEIP-TRANSFORMATION-DESCRIPTION",
    "SOMEIP-TRANSFORMATION-PROPS",
}

NESTED_INDEX_TAGS = SERVICE_PROTOTYPE_TAGS | SOMEIP_PROPS_TAGS | {"CLIENT-SERVER-OPERATION"}

FRAGMENT_INDEX_TAGS = TYPE_TAGS | NESTED_INDEX_TAGS | {
    "COMPU-METHOD",
    "CONSTANT-SPECIFICATION",
    "DATA-TYPE-MAPPING-SET",
    "TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING",
    "SW-BASE-TYPE",
}

DATA_TYPE_REF_TAGS = {
    "APPLICATION-DATA-TYPE-REF",
    "IMPLEMENTATION-DATA-TYPE-REF",
    "TYPE-REFERENCE-REF",
    "TYPE-TREF",
}

VLINK_SOMEIP_MEMBER_NAMES = {
    "get_serialized_size",
    "make_default",
    "is_vlink_someip_type",
    "vlink_someip_alignment",
    "vlink_someip_endian",
    "get_vlink_someip_fields",
    "vlink_someip_struct_length",
}

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char16_t", "char32_t", "class", "compl", "concept", "const",
    "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
    "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
    "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast",
    "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
    "while", "xor", "xor_eq",
}


class GeneratorError(Exception):
    """Base class for user-facing generation errors."""


class UnsupportedType(GeneratorError):
    """Raised when an AUTOSAR type cannot use VLINK_SOMEIP_FIELDS safely."""


class UnresolvedReference(UnsupportedType):
    """Raised when an AUTOSAR reference is absent from all input documents."""


@dataclass(frozen=True)
class XmlItem:
    ref: str
    element: ET.Element
    source: Path


@dataclass(frozen=True)
class TypeExpr:
    kind: str
    value: str = ""
    element: Optional["TypeExpr"] = None
    size: Optional[int] = None


@dataclass(frozen=True)
class StructField:
    name: str
    type_expr: TypeExpr
    source_ref: str
    length_width: Optional[int] = None
    array_dimensions: int = 1


@dataclass
class TypeDecl:
    ref: str
    name: str
    kind: str
    alias: Optional[TypeExpr] = None
    fields: List[StructField] = field(default_factory=list)
    enum_underlying: str = ""
    enum_values: List[Tuple[str, int]] = field(default_factory=list)
    struct_length_width: Optional[int] = None
    alignment: Optional[int] = None
    endian: Optional[str] = None
    endian_explicit: bool = False


@dataclass(frozen=True)
class InitialValue:
    ref: str
    name: str
    type_ref: str
    value_spec: Optional[ET.Element]


@dataclass(frozen=True)
class SomeipProps:
    array_width: Optional[int]
    string_width: Optional[int]
    struct_width: Optional[int]
    endian: Optional[str]
    alignment: Optional[int]
    legacy_string: Optional[bool]
    string_encoding: Optional[str]

    def with_defaults(self, defaults: Optional["SomeipProps"]) -> "SomeipProps":
        if defaults is None:
            return self
        return SomeipProps(
            self.array_width if self.array_width is not None else defaults.array_width,
            self.string_width if self.string_width is not None else defaults.string_width,
            self.struct_width if self.struct_width is not None else defaults.struct_width,
            self.endian if self.endian is not None else defaults.endian,
            self.alignment if self.alignment is not None else defaults.alignment,
            self.legacy_string if self.legacy_string is not None else defaults.legacy_string,
            self.string_encoding if self.string_encoding is not None else defaults.string_encoding,
        )

    def effective_profile(self) -> Tuple[int, int, int, str, int]:
        return (
            4 if self.array_width is None else self.array_width,
            4 if self.string_width is None else self.string_width,
            0 if self.struct_width is None else self.struct_width,
            "big" if self.endian is None else self.endian,
            1 if self.alignment is None else self.alignment,
        )


def local_name(tag: str) -> str:
    """Return an XML tag name without its namespace."""
    return tag.rsplit("}", 1)[-1]


def normalize_ref(value: str) -> str:
    parts = [part for part in value.strip().split("/") if part]
    return "/" + "/".join(parts)


def direct_child(element: ET.Element, name: str) -> Optional[ET.Element]:
    for child in element:
        if local_name(child.tag) == name:
            return child
    return None


def direct_text(element: ET.Element, name: str) -> str:
    child = direct_child(element, name)
    return (child.text or "").strip() if child is not None else ""


def descendants(element: ET.Element, names: Iterable[str]) -> Iterable[ET.Element]:
    wanted = set(names)
    for child in element.iter():
        if child is element:
            continue
        if local_name(child.tag) in wanted:
            yield child


def elements_including_self(element: ET.Element, names: Iterable[str]) -> Iterable[ET.Element]:
    wanted = set(names)
    for child in element.iter():
        if local_name(child.tag) in wanted:
            yield child


def descendant_texts(element: ET.Element, names: Iterable[str]) -> Iterable[str]:
    for child in descendants(element, names):
        value = (child.text or "").strip()
        if value:
            yield value


def first_descendant(element: ET.Element, names: Iterable[str]) -> Optional[ET.Element]:
    return next(iter(descendants(element, names)), None)


def first_descendant_text(element: ET.Element, names: Iterable[str]) -> str:
    child = first_descendant(element, names)
    return (child.text or "").strip() if child is not None else ""


def scoped_descendants(element: ET.Element, names: Iterable[str]) -> Iterable[ET.Element]:
    """Yield descendants without entering nested AUTOSAR member containers."""
    wanted = set(names)
    pending = list(reversed(list(element)))
    while pending:
        child = pending.pop()
        tag = local_name(child.tag)
        if tag in wanted:
            yield child
        if tag not in {"ELEMENTS", "SUB-ELEMENTS"}:
            pending.extend(reversed(list(child)))


def first_scoped_descendant(element: ET.Element, names: Iterable[str]) -> Optional[ET.Element]:
    return next(iter(scoped_descendants(element, names)), None)


def first_scoped_descendant_text(element: ET.Element, names: Iterable[str]) -> str:
    child = first_scoped_descendant(element, names)
    return (child.text or "").strip() if child is not None else ""


def sanitize_identifier(value: str, fallback: str = "unnamed") -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", value.strip())
    result = re.sub(r"_+", "_", result).strip("_") or fallback
    if result[0].isdigit():
        result = "_" + result
    is_native_typedef = re.fullmatch(r"u?int(?:8|16|32|64)_t|size_t", result) is not None
    if result in CPP_KEYWORDS or is_native_typedef or result.startswith("__") or re.match(r"^_[A-Z]", result):
        result += "_"
    return result


def parse_integer(value: str) -> int:
    text = value.strip().replace("_", "")
    if not text:
        raise ValueError("empty integer")
    octal = re.fullmatch(r"([+-]?)(0[0-7]+)", text)
    if octal is not None:
        magnitude = int(octal.group(2), 8)
        return -magnitude if octal.group(1) == "-" else magnitude
    try:
        return int(text, 0)
    except ValueError:
        return int(text, 10)


def normalized_category(element: ET.Element) -> str:
    return direct_text(element, "CATEGORY").upper().replace("_", "-")


def positive_size(value: str, context: str, description: str) -> int:
    try:
        size = parse_integer(value)
    except ValueError as error:
        raise UnsupportedType(f"{context}: invalid {description} '{value}'") from error
    if size <= 0:
        raise UnsupportedType(f"{context}: {description} must be positive")
    return size


def unique_name(base: str, used: Set[str]) -> str:
    name = base
    suffix = 2
    while name in used:
        name = f"{base}_{suffix}"
        suffix += 1
    used.add(name)
    return name


def snake_case(value: str) -> str:
    result = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    return sanitize_identifier(result.lower(), "value")


def escape_cpp_string(value: str) -> str:
    escaped: List[str] = []
    for char in value:
        codepoint = ord(char)
        if char == "\\":
            escaped.append("\\\\")
        elif char == '"':
            escaped.append('\\"')
        elif char == "\n":
            escaped.append("\\n")
        elif char == "\r":
            escaped.append("\\r")
        elif char == "\t":
            escaped.append("\\t")
        elif codepoint < 32 or codepoint == 127:
            escaped.append(f"\\{codepoint:03o}")
        else:
            escaped.append(char)
    return '"' + "".join(escaped) + '"'


class ArxmlModel:
    """Namespace-independent index of one or more AUTOSAR documents."""

    def __init__(self, paths: Sequence[Path]) -> None:
        self.items: Dict[str, XmlItem] = {}
        self._items_by_short_name: Dict[str, List[XmlItem]] = {}
        self.roots: List[Tuple[Path, ET.Element]] = []
        self.application_mappings: Dict[str, str] = {}
        self.someip_props: Dict[str, SomeipProps] = {}
        self.legacy_alignments: List[Tuple[Path, int]] = []
        self.service_props: Dict[str, str] = {}
        self.field_props: Dict[Tuple[str, str], str] = {}
        self.unsupported_service_deployments: Dict[str, str] = {}
        self.unsupported_field_deployments: Dict[Tuple[str, str], str] = {}
        self.warnings: List[str] = []
        self._load(paths)
        self._read_type_mappings()
        self._read_someip_deployments()

    def _load(self, paths: Sequence[Path]) -> None:
        for path in paths:
            try:
                root = ET.parse(path).getroot()
            except (OSError, ET.ParseError) as error:
                raise GeneratorError(f"cannot parse '{path}': {error}") from error
            self.roots.append((path, root))

            root_tag = local_name(root.tag)
            top_packages: List[ET.Element] = []
            if root_tag == "AUTOSAR":
                container = direct_child(root, "AR-PACKAGES")
                if container is not None:
                    top_packages = [
                        package for package in container if local_name(package.tag) == "AR-PACKAGE"
                    ]
            elif root_tag == "AR-PACKAGES":
                top_packages = [
                    package for package in root if local_name(package.tag) == "AR-PACKAGE"
                ]
            elif root_tag == "AR-PACKAGE":
                top_packages = [root]
            for package in top_packages:
                self._index_package(path, package, ())

            if not top_packages:
                self._index_fragment(path, root)

    def _index_package(self, source: Path, package: ET.Element, prefix: Tuple[str, ...]) -> None:
        pending = [(package, prefix)]
        while pending:
            current, current_prefix = pending.pop()
            package_name = direct_text(current, "SHORT-NAME")
            if not package_name:
                self.warnings.append(f"{source}: ignored AR-PACKAGE without SHORT-NAME")
                continue
            package_path = current_prefix + (package_name,)

            elements = direct_child(current, "ELEMENTS")
            if elements is not None:
                for element in elements:
                    short_name = direct_text(element, "SHORT-NAME")
                    if short_name:
                        element_path = package_path + (short_name,)
                        self._add_item(source, element, element_path)
                        self._index_nested(source, element, element_path)

            subpackages = direct_child(current, "AR-PACKAGES")
            if subpackages is not None:
                children = [child for child in subpackages if local_name(child.tag) == "AR-PACKAGE"]
                pending.extend((child, package_path) for child in reversed(children))

    def _index_fragment(self, source: Path, root: ET.Element) -> None:
        for element in root.iter():
            if local_name(element.tag) not in FRAGMENT_INDEX_TAGS:
                continue
            short_name = direct_text(element, "SHORT-NAME")
            if short_name:
                self._add_item(source, element, (short_name,))

    def _index_nested(self, source: Path, element: ET.Element, prefix: Tuple[str, ...]) -> None:
        pending = [(child, prefix) for child in reversed(list(element))]
        while pending:
            child, parent_prefix = pending.pop()
            short_name = direct_text(child, "SHORT-NAME")
            child_prefix = parent_prefix + (short_name,) if short_name else parent_prefix
            if local_name(child.tag) in NESTED_INDEX_TAGS and short_name:
                self._add_item(source, child, child_prefix)
            pending.extend((nested, child_prefix) for nested in reversed(list(child)))

    def _add_item(self, source: Path, element: ET.Element, parts: Tuple[str, ...]) -> None:
        ref = normalize_ref("/".join(parts))
        previous = self.items.get(ref)
        if previous is not None:
            if local_name(previous.element.tag) != local_name(element.tag):
                raise GeneratorError(
                    f"duplicate AUTOSAR reference '{ref}' has different element types in "
                    f"'{previous.source}' and '{source}'"
                )
            self._merge_split_element(previous.element, element, ref, previous.source, source)
            return
        item = XmlItem(ref, element, source)
        self.items[ref] = item
        self._items_by_short_name.setdefault(parts[-1], []).append(item)

    @classmethod
    def _merge_split_element(
        cls,
        target: ET.Element,
        incoming: ET.Element,
        ref: str,
        target_source: Path,
        incoming_source: Path,
    ) -> None:
        for name, value in incoming.attrib.items():
            previous = target.attrib.get(name)
            if previous is not None and previous != value:
                raise GeneratorError(
                    f"split AUTOSAR reference '{ref}' has conflicting attribute '{name}' in "
                    f"'{target_source}' and '{incoming_source}'"
                )
            target.attrib[name] = value

        target_text = (target.text or "").strip()
        incoming_text = (incoming.text or "").strip()
        if target_text and incoming_text and target_text != incoming_text:
            raise GeneratorError(
                f"split AUTOSAR reference '{ref}' has conflicting values in "
                f"'{target_source}' and '{incoming_source}'"
            )
        if not target_text and incoming_text:
            target.text = incoming.text

        for child in incoming:
            tag = local_name(child.tag)
            candidates = [candidate for candidate in target if local_name(candidate.tag) == tag]
            short_name = direct_text(child, "SHORT-NAME")
            if short_name:
                match = next(
                    (candidate for candidate in candidates if direct_text(candidate, "SHORT-NAME") == short_name),
                    None,
                )
                if match is None:
                    target.append(copy.deepcopy(child))
                else:
                    cls._merge_split_element(match, child, f"{ref}/{short_name}", target_source, incoming_source)
                continue

            if list(child):
                if len(candidates) > 1:
                    raise GeneratorError(
                        f"split AUTOSAR reference '{ref}' has ambiguous '{tag}' containers in "
                        f"'{target_source}' and '{incoming_source}'"
                    )
                if candidates:
                    cls._merge_split_element(candidates[0], child, ref, target_source, incoming_source)
                else:
                    target.append(copy.deepcopy(child))
                continue

            child_text = (child.text or "").strip()
            identical = any(
                (candidate.text or "").strip() == child_text and candidate.attrib == child.attrib
                for candidate in candidates
            )
            if identical:
                continue
            parent_tag = local_name(target.tag)
            if candidates and not (tag.endswith("-REF") and parent_tag.endswith("-REFS")):
                raise GeneratorError(
                    f"split AUTOSAR reference '{ref}' has conflicting '{tag}' values in "
                    f"'{target_source}' and '{incoming_source}'"
                )
            target.append(copy.deepcopy(child))

    def _read_type_mappings(self) -> None:
        for source, root in self.roots:
            for mapping in elements_including_self(root, {"DATA-TYPE-MAP", "DATA-TYPE-MAPPING"}):
                application_ref = first_descendant_text(mapping, {"APPLICATION-DATA-TYPE-REF"})
                implementation_ref = first_descendant_text(mapping, {"IMPLEMENTATION-DATA-TYPE-REF"})
                if not application_ref or not implementation_ref:
                    continue
                application_ref = normalize_ref(application_ref)
                implementation_ref = normalize_ref(implementation_ref)
                previous = self.application_mappings.get(application_ref)
                if previous is not None and previous != implementation_ref:
                    raise GeneratorError(
                        f"{source}: application type '{application_ref}' has conflicting implementation "
                        f"mappings '{previous}' and '{implementation_ref}'"
                    )
                self.application_mappings[application_ref] = implementation_ref

    @staticmethod
    def _optional_integer(element: ET.Element, name: str, context: str) -> Optional[int]:
        value = direct_text(element, name)
        if not value:
            return None
        try:
            return parse_integer(value)
        except ValueError as error:
            raise GeneratorError(f"{context}: invalid {name} '{value}'") from error

    @staticmethod
    def _optional_endian(element: ET.Element, context: str) -> Optional[str]:
        value = direct_text(element, "BYTE-ORDER")
        if not value:
            return None
        normalized = value.upper().replace("_", "-")
        if normalized in {"MOST-SIGNIFICANT-BYTE-FIRST", "BIG-ENDIAN"}:
            return "big"
        if normalized in {"MOST-SIGNIFICANT-BYTE-LAST", "LITTLE-ENDIAN"}:
            return "little"
        if normalized == "OPAQUE":
            return "opaque"
        raise GeneratorError(f"{context}: unsupported SOME/IP BYTE-ORDER '{value}'")

    @staticmethod
    def _optional_boolean(element: ET.Element, name: str, context: str) -> Optional[bool]:
        value = direct_text(element, name)
        if not value:
            return None
        normalized = value.strip().lower()
        if normalized in {"1", "true"}:
            return True
        if normalized in {"0", "false"}:
            return False
        raise GeneratorError(f"{context}: invalid {name} '{value}'")

    @classmethod
    def _optional_alignment(cls, element: ET.Element, context: str) -> Optional[int]:
        alignment_bits = cls._optional_integer(element, "ALIGNMENT", context)
        if alignment_bits is None:
            return None
        return alignment_bytes(alignment_bits)

    def _read_someip_deployments(self) -> None:
        for item in self.items.values():
            if local_name(item.element.tag) not in SOMEIP_PROPS_TAGS:
                continue
            self.someip_props[item.ref] = SomeipProps(
                self._optional_integer(item.element, "SIZE-OF-ARRAY-LENGTH-FIELD", item.ref),
                self._optional_integer(item.element, "SIZE-OF-STRING-LENGTH-FIELD", item.ref),
                self._optional_integer(item.element, "SIZE-OF-STRUCT-LENGTH-FIELD", item.ref),
                self._optional_endian(item.element, item.ref),
                self._optional_alignment(item.element, item.ref),
                self._optional_boolean(
                    item.element, "IMPLEMENTS-LEGACY-STRING-SERIALIZATION", item.ref
                ),
                direct_text(item.element, "STRING-ENCODING") or None,
            )
            if local_name(item.element.tag) == "SOMEIP-TRANSFORMATION-DESCRIPTION":
                alignment = self._optional_integer(item.element, "ALIGNMENT", item.ref)
                if alignment is not None:
                    self.legacy_alignments.append((item.source, alignment))

        for source, root in self.roots:
            for mapping in elements_including_self(
                root, {"TRANSFORMATION-PROPS-TO-SERVICE-INTERFACE-ELEMENT-MAPPING"}
            ):
                props_ref = direct_text(mapping, "TRANSFORMATION-PROPS-REF")
                if not props_ref:
                    continue
                prototype_refs = list(descendant_texts(mapping, {"EVENT-REF", "FIELD-REF"}))
                call_refs = list(descendant_texts(mapping, {"METHOD-CALL-REF"}))
                return_refs = list(descendant_texts(mapping, {"METHOD-RETURN-REF"}))
                method_refs = list(descendant_texts(mapping, {"METHOD-REF"}))
                for prototype_ref in prototype_refs:
                    self._add_service_mapping(prototype_ref, props_ref, source)
                for operation_ref in call_refs:
                    self._add_method_mapping(operation_ref, props_ref, source, {"IN", "INOUT"})
                for operation_ref in return_refs:
                    self._add_method_mapping(operation_ref, props_ref, source, {"OUT", "INOUT"})
                for operation_ref in method_refs:
                    self._add_method_mapping(operation_ref, props_ref, source, None)

                if first_descendant(mapping, {"TLV-DATA-ID-DEFINITION-REFS"}) is not None:
                    reason = "TLV data-ID deployment is not supported by VLink's fixed SOME/IP codec"
                    for prototype_ref in prototype_refs:
                        self._mark_service_unsupported(prototype_ref, reason)
                    for operation_ref in call_refs:
                        self._mark_method_unsupported(operation_ref, source, {"IN", "INOUT"}, reason)
                    for operation_ref in return_refs:
                        self._mark_method_unsupported(operation_ref, source, {"OUT", "INOUT"}, reason)
                    for operation_ref in method_refs:
                        self._mark_method_unsupported(operation_ref, source, None, reason)

            for deployment in elements_including_self(
                root, {"SOMEIP-DATA-PROTOTYPE-TRANSFORMATION-PROPS"}
            ):
                props_ref = direct_text(deployment, "SOMEIP-TRANSFORMATION-PROPS-REF")
                for reference in descendants(
                    deployment, {"DATA-PROTOTYPE-IN-SERVICE-INTERFACE-REF"}
                ):
                    self._add_field_deployment(reference, props_ref or None, deployment, source)

    def _mark_service_unsupported(self, prototype_ref: str, reason: str) -> None:
        prototype = normalize_ref(prototype_ref)
        self.unsupported_service_deployments.setdefault(prototype, reason)

    def _mark_method_unsupported(
        self,
        operation_ref: str,
        source: Path,
        directions: Optional[Set[str]],
        reason: str,
    ) -> None:
        operation = self.resolve(operation_ref, str(source))
        for argument in descendants(operation.element, {"ARGUMENT-DATA-PROTOTYPE"}):
            direction = direct_text(argument, "DIRECTION").upper().replace("_", "")
            if directions is not None and direction not in directions:
                continue
            short_name = direct_text(argument, "SHORT-NAME")
            if short_name:
                self._mark_service_unsupported(f"{operation.ref}/{short_name}", reason)

    def _add_service_mapping(self, prototype_ref: str, props_ref: str, source: Path) -> None:
        prototype = normalize_ref(prototype_ref)
        props = normalize_ref(props_ref)
        previous = self.service_props.get(prototype)
        if previous is not None and previous != props:
            raise GeneratorError(
                f"{source}: service element '{prototype}' has conflicting SOME/IP transformation props "
                f"'{previous}' and '{props}'"
            )
        self.service_props[prototype] = props

    def _add_method_mapping(
        self,
        operation_ref: str,
        props_ref: str,
        source: Path,
        directions: Optional[Set[str]],
    ) -> None:
        operation = self.resolve(operation_ref, str(source))
        if local_name(operation.element.tag) != "CLIENT-SERVER-OPERATION":
            raise GeneratorError(f"{source}: '{operation_ref}' is not a CLIENT-SERVER-OPERATION")
        for argument in descendants(operation.element, {"ARGUMENT-DATA-PROTOTYPE"}):
            direction = direct_text(argument, "DIRECTION").upper().replace("_", "")
            if directions is not None and direction not in directions:
                continue
            short_name = direct_text(argument, "SHORT-NAME")
            if short_name:
                self._add_service_mapping(f"{operation.ref}/{short_name}", props_ref, source)

    def _add_field_deployment(
        self,
        reference: ET.Element,
        props_ref: Optional[str],
        deployment: ET.Element,
        source: Path,
    ) -> None:
        root_ref = first_descendant_text(reference, {"ROOT-DATA-PROTOTYPE-REF"})
        target_ref = first_descendant_text(reference, {"TARGET-DATA-PROTOTYPE-REF"})
        if not root_ref:
            self.warnings.append(
                f"{source}: ignored SOME/IP data-prototype transformation props without ROOT-DATA-PROTOTYPE-REF"
            )
            return
        root = normalize_ref(root_ref)
        target = normalize_ref(target_ref) if target_ref else root
        key = (root, target)
        if first_descendant(deployment, {"NETWORK-REPRESENTATION"}) is not None:
            self.unsupported_field_deployments.setdefault(
                key,
                "NETWORK-REPRESENTATION is not supported because it can change the SOME/IP wire type",
            )
        if not props_ref:
            return
        props = normalize_ref(props_ref)
        previous = self.field_props.get(key)
        if previous is not None and previous != props:
            raise GeneratorError(
                f"{source}: data prototype '{target}' has conflicting SOME/IP transformation props "
                f"'{previous}' and '{props}'"
            )
        self.field_props[key] = props

    def prototype_type_ref(self, prototype_ref: str) -> str:
        item = self.resolve(prototype_ref, prototype_ref)
        if local_name(item.element.tag) not in SERVICE_PROTOTYPE_TAGS:
            raise GeneratorError(f"'{prototype_ref}' does not reference an AUTOSAR data prototype")
        reference = first_scoped_descendant_text(item.element, DATA_TYPE_REF_TAGS)
        if not reference:
            raise GeneratorError(f"{item.ref}: data prototype has no type reference")
        type_item = self.resolve(reference, item.ref)
        if local_name(type_item.element.tag) not in TYPE_TAGS:
            raise GeneratorError(f"{item.ref}: prototype type '{reference}' is not a supported data type")
        return type_item.ref

    def resolve(self, reference: str, context: str) -> XmlItem:
        normalized = normalize_ref(reference)
        exact = self.items.get(normalized)
        if exact is not None:
            return exact

        raw_reference = reference.strip()
        if raw_reference.startswith("/") or "/" in raw_reference:
            raise UnresolvedReference(f"{context}: unresolved AUTOSAR reference '{reference}'")

        short_name = normalized.rsplit("/", 1)[-1]
        matches = self._items_by_short_name.get(short_name, [])
        if not matches:
            raise UnresolvedReference(f"{context}: unresolved AUTOSAR reference '{reference}'")
        if len(matches) > 1:
            refs = ", ".join(sorted(item.ref for item in matches))
            raise UnsupportedType(f"{context}: ambiguous short reference '{reference}'; candidates: {refs}")
        return matches[0]

    def type_items(self) -> List[XmlItem]:
        return sorted(
            (item for item in self.items.values() if local_name(item.element.tag) in TYPE_TAGS),
            key=lambda item: item.ref,
        )

    def prototype_items(self) -> List[XmlItem]:
        return sorted(
            (item for item in self.items.values() if local_name(item.element.tag) in SERVICE_PROTOTYPE_TAGS),
            key=lambda item: item.ref,
        )

def alignment_bytes(alignment_bits: int) -> int:
    if alignment_bits <= 0 or alignment_bits % 8 != 0:
        raise GeneratorError(
            f"SOME/IP alignment must be a positive whole number of bytes, got {alignment_bits} bits"
        )
    result = alignment_bits // 8
    if result not in {1, 2, 4, 8, 16, 32}:
        raise GeneratorError(
            f"SOME/IP alignment {alignment_bits} bits is not supported; expected 8, 16, 32, 64, 128, or 256"
        )
    return result


def resolve_someip_alignment(model: ArxmlModel, override_bits: Optional[int]) -> Optional[int]:
    if override_bits is not None:
        return alignment_bytes(override_bits)
    values = {value for _, value in model.legacy_alignments}
    if len(values) > 1:
        rendered = ", ".join(str(value) for value in sorted(values))
        raise GeneratorError(
            f"multiple SOME/IP alignments ({rendered} bits) are present; select one with --alignment-bits"
        )
    return alignment_bytes(next(iter(values))) if values else None


class SomeipGenerator:
    """Translate indexed AUTOSAR data types to VLink SOME/IP declarations."""

    def __init__(
        self, model: ArxmlModel, byte_arrays_as_bytes: bool, alignment_override: Optional[int]
    ) -> None:
        self.model = model
        self.byte_arrays_as_bytes = byte_arrays_as_bytes
        self.alignment_override = alignment_override
        self.declarations: Dict[str, TypeDecl] = {}
        self.warnings: List[str] = list(model.warnings)
        self._building: Set[str] = set()
        self._built: Set[str] = set()
        self._field_deployment_widths: Dict[str, int] = {}
        self.initial_values: List[InitialValue] = []
        self.symbols = self._make_symbols(model.type_items())

    @staticmethod
    def _make_symbols(items: Sequence[XmlItem]) -> Dict[str, str]:
        groups: Dict[str, List[XmlItem]] = {}
        for item in items:
            symbol_props = direct_child(item.element, "SYMBOL-PROPS")
            symbol = direct_text(symbol_props, "SYMBOL") if symbol_props is not None else ""
            if not symbol:
                symbol = first_scoped_descendant_text(item.element, {"SYMBOL"})
            type_name = symbol or direct_text(item.element, "SHORT-NAME")
            groups.setdefault(sanitize_identifier(type_name, "Type"), []).append(item)

        symbols: Dict[str, str] = {}
        used: Set[str] = set()
        for base_name, group in sorted(groups.items()):
            if len(group) == 1:
                symbols[group[0].ref] = base_name
                used.add(base_name)

        for base_name, group in sorted(groups.items()):
            if len(group) == 1:
                continue
            for item in sorted(group, key=lambda value: value.ref):
                parts = [sanitize_identifier(part, "Package") for part in item.ref.strip("/").split("/")]
                candidate = base_name
                for depth in range(2, len(parts) + 1):
                    candidate = "_".join(parts[-depth:])
                    if candidate not in used:
                        break
                symbols[item.ref] = unique_name(candidate, used)
        return symbols

    def select(self, selectors: Sequence[str]) -> List[str]:
        if not selectors:
            return [item.ref for item in self.model.type_items()]

        selected: List[str] = []
        for selector in selectors:
            item = self.model.resolve(selector, "--type")
            if local_name(item.element.tag) not in TYPE_TAGS:
                raise GeneratorError(f"--type '{selector}' does not reference an AUTOSAR data type")
            if item.ref not in selected:
                selected.append(item.ref)
        return selected

    def select_initial_values(self, selectors: Sequence[str]) -> List[InitialValue]:
        selected: List[InitialValue] = []
        for selector in selectors:
            item = self.model.resolve(selector, "--prototype")
            if local_name(item.element.tag) not in SERVICE_PROTOTYPE_TAGS:
                raise GeneratorError(f"--prototype '{selector}' does not reference an AUTOSAR data prototype")

            type_ref = self.model.prototype_type_ref(item.ref)
            init_value = direct_child(item.element, "INIT-VALUE")
            value_spec = next(iter(init_value), None) if init_value is not None else None
            name = direct_text(item.element, "SHORT-NAME")
            selected.append(InitialValue(item.ref, name, type_ref, value_spec))
        self.initial_values = selected
        return selected

    def build(self, roots: Sequence[str], explicit: bool) -> List[str]:
        emitted_roots: List[str] = []
        for ref in roots:
            try:
                self._build_decl(ref)
                emitted_roots.append(ref)
            except UnsupportedType as error:
                if explicit:
                    raise
                self.warnings.append(f"skipped '{ref}': {error}")
        self._validate_deployments()
        return emitted_roots

    def apply_deployments(
        self, roots: Sequence[str], prototype_selectors: Sequence[str]
    ) -> None:
        selected_prototypes: List[str] = []
        if prototype_selectors:
            selected_prototypes = [
                self.model.resolve(selector, "--prototype").ref for selector in prototype_selectors
            ]
        else:
            candidates = set(self.model.service_props)
            candidates.update(root_ref for root_ref, _ in self.model.field_props)
            for prototype_ref in sorted(candidates):
                try:
                    type_ref = self.model.prototype_type_ref(prototype_ref)
                except GeneratorError as error:
                    self.warnings.append(str(error))
                    continue
                mapped_type_ref = self.model.application_mappings.get(type_ref, type_ref)
                if type_ref in roots or mapped_type_ref in roots:
                    selected_prototypes.append(prototype_ref)

        for prototype_ref in selected_prototypes:
            unsupported = self.model.unsupported_service_deployments.get(prototype_ref)
            if unsupported is not None:
                raise GeneratorError(f"{prototype_ref}: {unsupported}")
            type_ref = self.model.prototype_type_ref(prototype_ref)
            if type_ref not in self.declarations:
                continue
            defaults_ref = self.model.service_props.get(prototype_ref)
            has_deployment = defaults_ref is not None or any(
                root_ref == prototype_ref for root_ref, _ in self.model.field_props
            )
            if has_deployment:
                resolved_root = self._resolve_alias(TypeExpr("ref", type_ref))
                root_declaration = (
                    self.declarations.get(resolved_root.value) if resolved_root.kind == "ref" else None
                )
                if root_declaration is None or root_declaration.kind != "struct":
                    raise GeneratorError(
                        f"{prototype_ref}: top-level SOME/IP payload must resolve to a structure"
                    )
            defaults = self._resolve_props(defaults_ref, prototype_ref) if defaults_ref else None
            overrides: Dict[str, SomeipProps] = {}
            for (root_ref, target), props_ref in self.model.field_props.items():
                if root_ref != prototype_ref:
                    continue
                target_ref = self._mapped_deployment_target(target)
                props = self._resolve_props(props_ref, target)
                previous = overrides.get(target_ref)
                if previous is not None and previous != props:
                    raise GeneratorError(
                        f"{prototype_ref}: conflicting SOME/IP length deployments for '{target_ref}'"
                    )
                overrides[target_ref] = props
            for (root_ref, target), reason in self.model.unsupported_field_deployments.items():
                if root_ref == prototype_ref:
                    raise GeneratorError(f"{target}: {reason}")
            self._apply_root_deployment(type_ref, prototype_ref, defaults, overrides)

    @staticmethod
    def _merge_props(
        override: Optional[SomeipProps], defaults: Optional[SomeipProps]
    ) -> Optional[SomeipProps]:
        if override is None:
            return defaults
        return override.with_defaults(defaults)

    def _resolve_props(self, props_ref: Optional[str], context: str) -> SomeipProps:
        if not props_ref:
            raise GeneratorError(f"{context}: missing SOME/IP transformation props reference")
        item = self.model.resolve(props_ref, context)
        props = self.model.someip_props.get(item.ref)
        if props is None:
            raise GeneratorError(
                f"{context}: '{props_ref}' is not a supported SOME/IP transformation props element"
            )
        if props.legacy_string:
            raise GeneratorError(
                f"{context}: legacy SOME/IP string serialization is not supported"
            )
        if props.string_encoding:
            encoding = props.string_encoding.upper().replace("_", "-")
            if "UTF-8" not in encoding and "UTF8" not in encoding:
                raise GeneratorError(
                    f"{context}: SOME/IP string encoding '{props.string_encoding}' is not supported"
                )
        return props

    def _mapped_deployment_target(self, target_ref: str) -> str:
        for application_ref, implementation_ref in sorted(
            self.model.application_mappings.items(), key=lambda value: len(value[0]), reverse=True
        ):
            if target_ref == application_ref or target_ref.startswith(application_ref + "/"):
                return implementation_ref + target_ref[len(application_ref) :]
        return target_ref

    def _apply_root_deployment(
        self,
        root_ref: str,
        prototype_ref: str,
        defaults: Optional[SomeipProps],
        overrides: Dict[str, SomeipProps],
    ) -> None:
        visited_profiles: Set[Tuple[str, int, int, int, str, int]] = set()
        matched_overrides: Set[str] = set()

        def visit(expression: TypeExpr, inherited: Optional[SomeipProps], context_ref: str) -> None:
            if expression.kind == "ref":
                declaration = self.declarations.get(expression.value)
                if declaration is None:
                    return
                if declaration.kind == "alias":
                    visit(declaration.alias, inherited, context_ref)
                    return
                if declaration.kind != "struct":
                    return
                self._record_struct_width(declaration, inherited, context_ref, prototype_ref)
                self._record_struct_alignment(declaration, inherited, context_ref, prototype_ref)
                profile = self._effective_length_profile(inherited)
                visit_key = (expression.value, *profile)
                if visit_key in visited_profiles:
                    return
                visited_profiles.add(visit_key)
                updated: List[StructField] = []
                for member in declaration.fields:
                    override = overrides.get(member.source_ref)
                    props = self._merge_props(override, inherited)
                    if override is not None:
                        matched_overrides.add(member.source_ref)
                    width = self._width_for_expression(member.type_expr, props, member.source_ref)
                    dimensions = self._array_dimensions(member.type_expr, props, width, member.source_ref)
                    if self._length_kind(member.type_expr) in {"array", "bytes", "string", "vector"}:
                        self._record_field_width(
                            member.source_ref, 4 if width is None else width, prototype_ref
                        )
                    updated.append(
                        StructField(
                            member.name,
                            member.type_expr,
                            member.source_ref,
                            width if width != 4 else None,
                            dimensions,
                        )
                    )
                    visit(member.type_expr, props, member.source_ref)
                declaration.fields = updated
                return
            if expression.element is not None:
                visit(expression.element, inherited, context_ref)

        root_expression = TypeExpr("ref", root_ref)
        root_override = overrides.get(prototype_ref)
        root_props = self._merge_props(root_override, defaults)
        if root_override is not None:
            matched_overrides.add(prototype_ref)
        root_kind = self._length_kind(root_expression)
        if root_kind in {"array", "bytes", "string", "vector"} and root_props is not None:
            width = self._width_for_expression(root_expression, root_props, prototype_ref)
            if width not in {None, 4}:
                raise GeneratorError(
                    f"{prototype_ref}: top-level {root_kind} length width {width} cannot be expressed by "
                    "VLINK_SOMEIP_LENGTH; use a structure field"
                )
        self._record_root_endian(root_expression, root_props, prototype_ref)
        root_endian = (
            root_props.endian if root_props is not None and root_props.endian is not None else "big"
        )
        for target_ref, props in overrides.items():
            if target_ref == prototype_ref or props.endian is None:
                continue
            if props.endian == "opaque" or props.endian != root_endian:
                raise GeneratorError(
                    f"{target_ref}: per-field SOME/IP BYTE-ORDER cannot be expressed by "
                    "VLINK_SOMEIP_ENDIAN; the macro applies to the complete top-level payload"
                )
        visit(root_expression, root_props, prototype_ref)

        unmatched = sorted(set(overrides) - matched_overrides)
        if unmatched:
            rendered = ", ".join(unmatched)
            raise GeneratorError(
                f"{prototype_ref}: SOME/IP data-prototype deployment target(s) do not match generated fields: "
                f"{rendered}"
            )

    @staticmethod
    def _effective_length_profile(props: Optional[SomeipProps]) -> Tuple[int, int, int, str, int]:
        if props is None:
            return 4, 4, 0, "big", 1
        return props.effective_profile()

    def _record_struct_alignment(
        self,
        declaration: TypeDecl,
        props: Optional[SomeipProps],
        context: str,
        prototype_ref: str,
    ) -> None:
        value = self.alignment_override
        if value is None and props is not None:
            value = props.alignment
        if value is None:
            return
        previous = declaration.alignment
        if previous is not None and previous != value:
            raise GeneratorError(
                f"{declaration.ref}: conflicting SOME/IP alignments {previous} and {value} bytes "
                f"across service elements; latest is '{prototype_ref}'"
            )
        declaration.alignment = value

    def _record_root_endian(
        self, expression: TypeExpr, props: Optional[SomeipProps], prototype_ref: str
    ) -> None:
        endian = "big" if props is None or props.endian is None else props.endian
        explicit = props is not None and props.endian is not None
        if endian == "opaque":
            raise GeneratorError(
                f"{prototype_ref}: SOME/IP BYTE-ORDER OPAQUE cannot be expressed by VLINK_SOMEIP_ENDIAN"
            )

        resolved = self._resolve_alias(expression)
        declaration = self.declarations.get(resolved.value) if resolved.kind == "ref" else None
        if declaration is None or declaration.kind != "struct":
            if endian == "little":
                raise GeneratorError(
                    f"{prototype_ref}: little-endian top-level '{self._length_kind(expression)}' cannot use "
                    "VLINK_SOMEIP_ENDIAN; generate a structure payload"
                )
            return

        previous = declaration.endian
        if previous is not None and previous != endian:
            raise GeneratorError(
                f"{declaration.ref}: conflicting SOME/IP byte orders '{previous}' and '{endian}' "
                f"across service elements; latest is '{prototype_ref}'"
            )
        declaration.endian = endian
        declaration.endian_explicit = declaration.endian_explicit or explicit

    def _record_struct_width(
        self,
        declaration: TypeDecl,
        props: Optional[SomeipProps],
        context: str,
        prototype_ref: str,
    ) -> None:
        width = 0 if props is None or props.struct_width is None else props.struct_width
        if width not in {0, 1, 2, 4}:
            raise GeneratorError(
                f"{context}: unsupported SOME/IP structure length width {width}; expected 0, 1, 2, or 4"
            )
        previous = declaration.struct_length_width
        if previous is not None and previous != width:
            raise GeneratorError(
                f"{declaration.ref}: conflicting SOME/IP structure length widths {previous} and {width} "
                f"across service elements; latest is '{prototype_ref}'"
            )
        declaration.struct_length_width = width

    def _width_for_expression(
        self, expression: TypeExpr, props: Optional[SomeipProps], context: str
    ) -> Optional[int]:
        if props is None:
            return None
        kind = self._length_kind(expression)
        if kind not in {"array", "bytes", "string", "vector"}:
            return None
        width = props.string_width if kind == "string" else props.array_width
        if width is None:
            return None
        if width not in {0, 1, 2, 4}:
            raise GeneratorError(f"{context}: unsupported SOME/IP {kind} length width {width}; expected 0, 1, 2, or 4")
        if width == 0 and kind != "array":
            raise GeneratorError(f"{context}: zero length-field width is only valid for fixed-size arrays")
        return width

    def _resolve_alias(self, expression: TypeExpr) -> TypeExpr:
        resolved = expression
        visited: Set[str] = set()
        while resolved.kind == "ref" and resolved.value not in visited:
            visited.add(resolved.value)
            declaration = self.declarations.get(resolved.value)
            if declaration is None or declaration.kind != "alias" or declaration.alias is None:
                break
            resolved = declaration.alias
        return resolved

    def _length_kind(self, expression: TypeExpr) -> str:
        resolved = self._resolve_alias(expression)
        if resolved.kind != "ref":
            return resolved.kind
        declaration = self.declarations.get(resolved.value)
        return declaration.kind if declaration is not None else "ref"

    def _array_dimensions(
        self, expression: TypeExpr, props: Optional[SomeipProps], width: Optional[int], context: str
    ) -> int:
        resolved = self._resolve_alias(expression)
        dimensions = 0
        while resolved.kind in {"array", "vector"} and resolved.element is not None:
            if width == 0 and resolved.kind == "vector":
                raise GeneratorError(f"{context}: zero length-field width is only valid for fixed-size arrays")
            dimensions += 1
            resolved = self._resolve_alias(resolved.element)
        if dimensions > 0 and resolved.kind == "bytes" and width not in {None, 4}:
            raise GeneratorError(
                f"{context}: non-default length width for nested bytes cannot be expressed by "
                "VLINK_SOMEIP_ARRAY_LENGTH"
            )
        if dimensions > 0 and resolved.kind == "string" and props is not None and props.string_width not in {None, 4}:
            raise GeneratorError(
                f"{context}: non-default length width for nested string cannot be expressed by "
                "VLINK_SOMEIP_ARRAY_LENGTH"
            )
        if width in {None, 4}:
            return 1
        return max(dimensions, 1)

    def _record_field_width(self, field_ref: str, width: int, prototype_ref: str) -> None:
        previous = self._field_deployment_widths.get(field_ref)
        if previous is not None and previous != width:
            raise GeneratorError(
                f"{field_ref}: conflicting SOME/IP length widths {previous} and {width} across service elements; "
                f"latest is '{prototype_ref}'"
            )
        self._field_deployment_widths[field_ref] = width

    def _build_decl(self, ref: str) -> TypeDecl:
        if ref in self._building:
            raise UnsupportedType(f"recursive by-value type dependency involving '{ref}' is not supported")

        existing = self.declarations.get(ref)
        if ref in self._built and existing is not None:
            return existing

        if existing is not None:
            self._building.add(ref)
            try:
                for dependency in self._dependencies(existing):
                    self._build_decl(dependency)
                self._built.add(ref)
                return existing
            finally:
                self._building.discard(ref)

        item = self.model.resolve(ref, ref)
        tag = local_name(item.element.tag)
        if tag == "SW-BASE-TYPE":
            raise UnsupportedType(f"'{ref}' is a base type and does not produce a declaration")
        if tag not in TYPE_TAGS:
            raise UnsupportedType(f"'{ref}' has unsupported category '{tag}'")

        self._building.add(ref)
        try:
            declaration = self._parse_decl(item)
            self.declarations[ref] = declaration
            for dependency in self._dependencies(declaration):
                self._build_decl(dependency)
            self._built.add(ref)
            return declaration
        except Exception:
            self.declarations.pop(ref, None)
            self._built.discard(ref)
            raise
        finally:
            self._building.discard(ref)

    def _parse_decl(self, item: XmlItem) -> TypeDecl:
        tag = local_name(item.element.tag)
        name = self.symbols[item.ref]
        mapped_ref = self.model.application_mappings.get(item.ref)
        if tag.startswith("APPLICATION-") and mapped_ref:
            target = self.model.resolve(mapped_ref, item.ref)
            if local_name(target.element.tag) not in TYPE_TAGS:
                raise UnsupportedType(f"{item.ref}: mapped implementation reference '{mapped_ref}' is not a data type")
            return TypeDecl(item.ref, name, "alias", alias=TypeExpr("ref", target.ref))

        if tag == "IMPLEMENTATION-DATA-TYPE":
            return self._parse_implementation_type(item, name)
        if tag in {"CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE", "STD-CPP-IMPLEMENTATION-DATA-TYPE"}:
            return self._parse_cpp_implementation_type(item, name)
        if tag in {"APPLICATION-PRIMITIVE-DATA-TYPE", "APPLICATION-VALUE-DATA-TYPE"}:
            expression = self._parse_value_expr(item.element, item.ref)
            return self._value_decl(item, name, expression)
        if tag == "APPLICATION-RECORD-DATA-TYPE":
            fields = self._parse_record_fields(item.element, item.ref, "APPLICATION-RECORD-ELEMENT")
            return self._struct_decl(item.ref, name, fields)
        if tag == "APPLICATION-ARRAY-DATA-TYPE":
            expression = self._parse_application_array(item.element, item.ref)
            return TypeDecl(item.ref, name, "alias", alias=expression)
        raise UnsupportedType(f"{item.ref}: unsupported data type '{tag}'")

    def _parse_cpp_implementation_type(self, item: XmlItem, name: str) -> TypeDecl:
        category = normalized_category(item.element)
        if category in {"STRUCTURE", "RECORD"}:
            fields = self._parse_record_fields(item.element, item.ref, "CPP-IMPLEMENTATION-DATA-TYPE-ELEMENT")
            return self._struct_decl(item.ref, name, fields)
        if category in {"ARRAY", "VECTOR"}:
            reference = first_scoped_descendant_text(
                item.element, {"TEMPLATE-TYPE-REF", "TYPE-REFERENCE-REF", "TYPE-TREF"}
            )
            if not reference:
                raise UnsupportedType(f"{item.ref}: {category.lower()} has no template element type")
            element_expr = self._expr_for_reference(reference, item.ref)
            size_text = first_scoped_descendant_text(
                item.element, {"ARRAY-SIZE", "MAX-NUMBER-OF-ELEMENTS"}
            )
            if category == "VECTOR":
                max_size = positive_size(size_text, item.ref, "maximum vector size") if size_text else None
                if self.byte_arrays_as_bytes and self._is_uint8_expr(element_expr):
                    return TypeDecl(item.ref, name, "alias", alias=TypeExpr("bytes", size=max_size))
                return TypeDecl(
                    item.ref, name, "alias", alias=TypeExpr("vector", element=element_expr, size=max_size)
                )

            size = positive_size(size_text, item.ref, "array size")
            return TypeDecl(item.ref, name, "alias", alias=TypeExpr("array", element=element_expr, size=size))
        if category in {"STRING", "TEXT", "UTF-8", "UTF8"}:
            self._check_string_encoding(item.element, item.ref)
            return TypeDecl(item.ref, name, "alias", alias=TypeExpr("string"))
        if category in {"TYPE-REFERENCE", "TYPE_REFERENCE", "REFERENCE"}:
            reference = first_scoped_descendant_text(item.element, {"TYPE-REFERENCE-REF", "TYPE-TREF"})
            if not reference:
                raise UnsupportedType(f"{item.ref}: type reference has no target")
            return TypeDecl(item.ref, name, "alias", alias=self._expr_for_reference(reference, item.ref))
        if category in {"MAP", "UNION", "VARIANT", "OPTIONAL"}:
            raise UnsupportedType(f"{item.ref}: VLink's fixed SOME/IP deployment does not support {category.lower()}")
        if category in {"VALUE", "BOOLEAN", "BOOL"}:
            expression = self._builtin_reference_expr(direct_text(item.element, "SHORT-NAME"))
            if expression is None:
                expression = self._parse_value_expr(item.element, item.ref)
            return self._value_decl(item, name, expression)
        raise UnsupportedType(f"{item.ref}: unsupported C++ implementation category '{category or '<empty>'}'")

    def _parse_implementation_type(self, item: XmlItem, name: str) -> TypeDecl:
        category = normalized_category(item.element)
        sub_elements = self._sub_elements(item.element, "IMPLEMENTATION-DATA-TYPE-ELEMENT")

        if category in {"STRUCTURE", "RECORD"} or (not category and len(sub_elements) > 1):
            fields = self._parse_record_fields(item.element, item.ref, "IMPLEMENTATION-DATA-TYPE-ELEMENT")
            return self._struct_decl(item.ref, name, fields)
        if category in {"ARRAY", "VECTOR"}:
            if len(sub_elements) != 1:
                raise UnsupportedType(f"{item.ref}: array must contain exactly one implementation element")
            return TypeDecl(item.ref, name, "alias", alias=self._parse_array_element(sub_elements[0], item.ref))
        if category in {"TYPE-REFERENCE", "TYPE_REFERENCE", "REFERENCE"}:
            reference = first_descendant_text(item.element, {"IMPLEMENTATION-DATA-TYPE-REF", "TYPE-TREF"})
            if not reference:
                raise UnsupportedType(f"{item.ref}: type reference has no target")
            target = self.model.resolve(reference, item.ref)
            return TypeDecl(item.ref, name, "alias", alias=self._expr_for_item(target, item.ref))
        if category in {"UNION", "VARIANT", "BITFIELD", "BIT-FIELD"}:
            raise UnsupportedType(f"{item.ref}: VLink's fixed SOME/IP deployment does not support {category.lower()}")

        expression = self._parse_value_expr(item.element, item.ref)
        return self._value_decl(item, name, expression)

    def _value_decl(self, item: XmlItem, name: str, expression: TypeExpr) -> TypeDecl:
        if expression.kind == "scalar" and expression.value.startswith("uint"):
            enum_values = self._parse_enum_values(item.element, item.ref)
            if enum_values:
                width = int(re.search(r"(8|16|32|64)", expression.value).group(1))
                if any(value < 0 or value >= 1 << width for _, value in enum_values):
                    self.warnings.append(
                        f"{item.ref}: enum constants do not fit {expression.value}; generated an integer alias"
                    )
                    return TypeDecl(item.ref, name, "alias", alias=expression)
                return TypeDecl(
                    item.ref,
                    name,
                    "enum",
                    enum_underlying=expression.value,
                    enum_values=enum_values,
                )
        return TypeDecl(item.ref, name, "alias", alias=expression)

    def _struct_decl(self, ref: str, name: str, fields: List[StructField]) -> TypeDecl:
        if not fields:
            raise UnsupportedType(f"{ref}: empty structures cannot use VLINK_SOMEIP_FIELDS")
        return TypeDecl(ref, name, "struct", fields=fields)

    @staticmethod
    def _sub_elements(element: ET.Element, element_tag: str) -> List[ET.Element]:
        result: List[ET.Element] = []
        for container_name in ("SUB-ELEMENTS", "ELEMENTS"):
            container = direct_child(element, container_name)
            if container is not None:
                result.extend(child for child in container if local_name(child.tag) == element_tag)
        return result

    def _parse_record_fields(self, element: ET.Element, context: str, element_tag: str) -> List[StructField]:
        members = self._sub_elements(element, element_tag)
        fields: List[StructField] = []
        used_names: Set[str] = set(VLINK_SOMEIP_MEMBER_NAMES)
        for index, member in enumerate(members, 1):
            short_name = direct_text(member, "SHORT-NAME") or f"field_{index}"
            base_name = sanitize_identifier(short_name, f"field_{index}")
            name = unique_name(base_name, used_names)

            optional = first_scoped_descendant_text(member, {"IS-OPTIONAL"}).lower()
            if optional in {"1", "true"}:
                raise UnsupportedType(
                    f"{context}/{short_name}: optional members require TLV and are not supported by "
                    "VLink's fixed deployment"
                )

            category = normalized_category(member)
            member_context = f"{context}/{short_name}"
            has_array_size = first_scoped_descendant(
                member, {"ARRAY-SIZE", "MAX-NUMBER-OF-ELEMENTS"}
            ) is not None
            if category in {"STRUCTURE", "RECORD"}:
                expression = self._parse_inline_structure(member, member_context)
            elif category in {"ARRAY", "VECTOR"} or has_array_size:
                expression = self._parse_array_element(member, f"{context}/{short_name}")
            else:
                expression = self._parse_value_expr(member, member_context)
            fields.append(StructField(name, expression, member_context))
        return fields

    def _parse_inline_structure(self, element: ET.Element, context: str) -> TypeExpr:
        fields = self._parse_record_fields(element, context, "IMPLEMENTATION-DATA-TYPE-ELEMENT")
        if not fields:
            raise UnsupportedType(f"{context}: inline structure has no fields")

        parent_ref, short_name = context.rsplit("/", 1)
        parent_name = self.symbols.get(parent_ref, sanitize_identifier(parent_ref.rsplit("/", 1)[-1], "Type"))
        base_name = parent_name + "_" + sanitize_identifier(short_name, "Field")
        used_names = set(self.symbols.values())
        name = unique_name(base_name, used_names)

        self.symbols[context] = name
        self.declarations[context] = TypeDecl(context, name, "struct", fields=fields)
        return TypeExpr("ref", context)

    def _parse_application_array(self, element: ET.Element, context: str) -> TypeExpr:
        array_element = direct_child(element, "ELEMENT")
        if array_element is None:
            array_element = first_descendant(element, {"APPLICATION-ARRAY-ELEMENT"})
        if array_element is None:
            raise UnsupportedType(f"{context}: application array has no element")
        return self._parse_array_element(array_element, context)

    def _parse_array_element(self, element: ET.Element, context: str) -> TypeExpr:
        reference = first_scoped_descendant_text(
            element,
            {
                "APPLICATION-DATA-TYPE-REF",
                "BASE-TYPE-REF",
                "IMPLEMENTATION-DATA-TYPE-REF",
                "TEMPLATE-TYPE-REF",
                "TYPE-REFERENCE-REF",
                "TYPE-TREF",
            },
        )
        if reference:
            value_expr = self._expr_for_reference(reference, context)
        else:
            nested_elements = self._sub_elements(element, "IMPLEMENTATION-DATA-TYPE-ELEMENT")
            if len(nested_elements) > 1:
                raise UnsupportedType(f"{context}: array element contains multiple nested implementation elements")
            if nested_elements:
                nested = nested_elements[0]
                nested_category = normalized_category(nested)
                if nested_category in {"STRUCTURE", "RECORD"}:
                    value_expr = self._parse_inline_structure(nested, context + "/element")
                elif nested_category in {"ARRAY", "VECTOR"}:
                    value_expr = self._parse_array_element(nested, context + "/element")
                else:
                    value_expr = self._parse_value_expr(nested, context + "/element")
            else:
                value_expr = self._parse_value_expr(element, context)

        semantics = first_scoped_descendant_text(element, {"ARRAY-SIZE-SEMANTICS"}).upper().replace("_", "-")
        size_text = first_scoped_descendant_text(element, {"ARRAY-SIZE", "MAX-NUMBER-OF-ELEMENTS"})
        fixed_semantics = {"FIXED-SIZE", "FIXED", "STATIC"}
        variable_semantics = {"VARIABLE-SIZE", "VARIABLE", "DYNAMIC"}
        if semantics and semantics not in fixed_semantics | variable_semantics:
            raise UnsupportedType(f"{context}: unsupported ARRAY-SIZE-SEMANTICS '{semantics}'")
        if semantics in fixed_semantics and not size_text:
            raise UnsupportedType(f"{context}: fixed-size array has no ARRAY-SIZE or MAX-NUMBER-OF-ELEMENTS")
        if semantics in variable_semantics or not size_text:
            max_size = positive_size(size_text, context, "maximum array size") if size_text else None
            if self.byte_arrays_as_bytes and self._is_uint8_expr(value_expr):
                return TypeExpr("bytes", size=max_size)
            return TypeExpr("vector", element=value_expr, size=max_size)

        size = positive_size(size_text, context, "array size")
        return TypeExpr("array", element=value_expr, size=size)

    def _parse_value_expr(self, element: ET.Element, context: str) -> TypeExpr:
        category = normalized_category(element)
        if category in {"STRING", "TEXT", "UTF-8", "UTF8"}:
            self._check_string_encoding(element, context)
            return TypeExpr("string")
        if category in {"UNION", "VARIANT", "BITFIELD", "BIT-FIELD"}:
            raise UnsupportedType(f"{context}: VLink's fixed SOME/IP deployment does not support {category.lower()}")

        reference = first_scoped_descendant_text(element, DATA_TYPE_REF_TAGS)
        if reference:
            return self._expr_for_reference(reference, context)

        base_reference = first_scoped_descendant_text(element, {"BASE-TYPE-REF", "SW-BASE-TYPE-REF"})
        if base_reference:
            return self._expr_for_reference(base_reference, context, base_only=True)

        if category in {"BOOLEAN", "BOOL"}:
            return TypeExpr("scalar", "bool")
        primitive = self._scalar_from_text(category, 0)
        if primitive:
            return TypeExpr("scalar", primitive)
        raise UnsupportedType(f"{context}: no resolvable AUTOSAR base or referenced data type")

    def _expr_for_item(self, item: XmlItem, context: str) -> TypeExpr:
        tag = local_name(item.element.tag)
        if tag == "SW-BASE-TYPE":
            return self._base_type_expr(item, context)
        if tag in {"CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE", "STD-CPP-IMPLEMENTATION-DATA-TYPE"}:
            category = normalized_category(item.element)
            if category in {"VALUE", "BOOLEAN", "BOOL"}:
                expression = self._builtin_reference_expr(direct_text(item.element, "SHORT-NAME"))
                if expression is not None:
                    return expression
        if tag in TYPE_TAGS:
            return TypeExpr("ref", item.ref)
        raise UnsupportedType(f"{context}: reference '{item.ref}' targets unsupported '{tag}'")

    def _expr_for_reference(self, reference: str, context: str, base_only: bool = False) -> TypeExpr:
        try:
            item = self.model.resolve(reference, context)
        except UnresolvedReference:
            inferred = self._builtin_reference_expr(reference)
            if inferred is None:
                raise
            self.warnings.append(
                f"{context}: inferred '{self._render_expr(inferred)}' from missing standard type '{reference}'"
            )
            return inferred

        if base_only and local_name(item.element.tag) != "SW-BASE-TYPE":
            raise UnsupportedType(f"{context}: base type reference '{reference}' is not SW-BASE-TYPE")
        return self._expr_for_item(item, context)

    def _is_uint8_expr(self, expression: TypeExpr, seen: Optional[Set[str]] = None) -> bool:
        if expression == TypeExpr("scalar", "uint8_t"):
            return True
        if expression.kind != "ref":
            return False

        visited = set() if seen is None else seen
        if expression.value in visited:
            return False
        visited.add(expression.value)

        declaration = self.declarations.get(expression.value)
        if declaration is not None:
            if declaration.kind == "enum" or declaration.alias is None:
                return False
            return self._is_uint8_expr(declaration.alias, visited)

        mapped_ref = self.model.application_mappings.get(expression.value)
        if mapped_ref:
            mapped_item = self.model.resolve(mapped_ref, expression.value)
            return self._is_uint8_expr(self._expr_for_item(mapped_item, expression.value), visited)

        item = self.model.resolve(expression.value, expression.value)
        tag = local_name(item.element.tag)
        if tag == "SW-BASE-TYPE":
            return self._base_type_expr(item, item.ref) == TypeExpr("scalar", "uint8_t")
        if tag not in {
            "APPLICATION-PRIMITIVE-DATA-TYPE",
            "APPLICATION-VALUE-DATA-TYPE",
            "CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE",
            "IMPLEMENTATION-DATA-TYPE",
            "STD-CPP-IMPLEMENTATION-DATA-TYPE",
        }:
            return False
        if self._parse_enum_values(item.element, item.ref):
            return False

        category = normalized_category(item.element)
        if tag in {"CUSTOM-CPP-IMPLEMENTATION-DATA-TYPE", "STD-CPP-IMPLEMENTATION-DATA-TYPE"}:
            if category not in {"VALUE", "BOOLEAN", "BOOL", "TYPE-REFERENCE", "REFERENCE"}:
                return False
            builtin = self._builtin_reference_expr(direct_text(item.element, "SHORT-NAME"))
            if builtin is not None:
                return self._is_uint8_expr(builtin, visited)
        try:
            target = self._parse_value_expr(item.element, item.ref)
        except UnsupportedType:
            return False
        return self._is_uint8_expr(target, visited)

    def _builtin_reference_expr(self, reference: str) -> Optional[TypeExpr]:
        short_name = normalize_ref(reference).rsplit("/", 1)[-1]
        upper = re.sub(r"[^A-Z0-9]+", "", short_name.upper())
        if any(token in upper for token in ("UTF-16", "UTF16", "UCS-2", "UCS2")):
            return None
        if upper in {"UTF8", "UTF8STRING", "STRING", "STRINGUTF8"}:
            return TypeExpr("string")
        if upper in {"FLOAT", "FLOAT32"}:
            return TypeExpr("scalar", "float")
        if upper in {"DOUBLE", "FLOAT64"}:
            return TypeExpr("scalar", "double")
        scalar = self._scalar_from_text(short_name, 0)
        return TypeExpr("scalar", scalar) if scalar else None

    def _base_type_expr(self, item: XmlItem, context: str) -> TypeExpr:
        encoding = first_descendant_text(item.element, {"BASE-TYPE-ENCODING"})
        native = first_descendant_text(item.element, {"NATIVE-DECLARATION"})
        short_name = direct_text(item.element, "SHORT-NAME")
        combined = " ".join(value for value in (encoding, native, short_name) if value)
        upper = combined.upper().replace("_", "-")
        if any(token in upper for token in ("UTF-16", "UTF16", "UCS-2", "UCS2")):
            raise UnsupportedType(f"{context}: UTF-16/UCS-2 strings are not supported by VLink's SOME/IP codec")
        if any(token in upper for token in ("UTF-8", "UTF8")):
            return TypeExpr("string")

        size_text = first_descendant_text(item.element, {"BASE-TYPE-SIZE"})
        try:
            bits = parse_integer(size_text) if size_text else 0
        except ValueError as error:
            raise UnsupportedType(f"{context}: invalid BASE-TYPE-SIZE '{size_text}'") from error
        scalar = self._scalar_from_text(encoding, bits) if encoding else ""
        if not scalar:
            scalar = self._scalar_from_text(" ".join((native, short_name)), bits)
        if not scalar:
            raise UnsupportedType(
                f"{context}: cannot map SW-BASE-TYPE '{item.ref}' (encoding='{encoding}', size='{size_text}')"
            )
        return TypeExpr("scalar", scalar)

    @staticmethod
    def _scalar_from_text(value: str, bits: int) -> str:
        normalized = re.sub(r"[^A-Z0-9]+", "", value.upper())
        if "BOOLEAN" in normalized or normalized == "BOOL":
            return "bool"
        if any(token in normalized for token in ("FLOAT64", "DOUBLE", "IEEE75464")):
            return "double"
        if any(token in normalized for token in ("FLOAT32", "SINGLE", "IEEE75432")):
            return "float"
        if normalized == "FLOAT":
            return "double" if bits == 64 else "float" if bits == 32 else ""
        if "IEEE754" in normalized:
            return "double" if bits == 64 else "float" if bits == 32 else ""

        width = bits
        if not width:
            width_match = re.search(r"(8|16|32|64)(?:T)?$", normalized)
            width = int(width_match.group(1)) if width_match else 0
        if width not in {8, 16, 32, 64}:
            return ""

        signed = any(
            token in normalized for token in ("SINT", "SIGNED", "TWOSCOMPLEMENT", "2SCOMPLEMENT")
        ) or normalized.startswith("2C")
        if normalized.startswith("INT") and not normalized.startswith("INTEGERUNSIGNED"):
            signed = True
        if any(token in normalized for token in ("UINT", "UNSIGNED")):
            signed = False
        return f"{'int' if signed else 'uint'}{width}_t"

    @staticmethod
    def _check_string_encoding(element: ET.Element, context: str) -> None:
        encoding = first_descendant_text(element, {"STRING-ENCODING", "ENCODING"})
        if encoding and "UTF-8" not in encoding.upper() and "UTF8" not in encoding.upper():
            raise UnsupportedType(f"{context}: string encoding '{encoding}' is not UTF-8")
        semantics = first_descendant_text(element, {"ARRAY-SIZE-SEMANTICS"})
        if semantics.upper().replace("_", "-") in {"FIXED", "FIXED-SIZE", "STATIC"}:
            raise UnsupportedType(
                f"{context}: fixed-length strings are not supported by VLink's SOME/IP codec"
            )

    def _parse_enum_values(self, element: ET.Element, context: str) -> List[Tuple[str, int]]:
        compu_ref = first_descendant_text(element, {"COMPU-METHOD-REF"})
        if not compu_ref:
            return []
        compu_item = self.model.resolve(compu_ref, context)
        if local_name(compu_item.element.tag) != "COMPU-METHOD":
            return []

        values: List[Tuple[str, int]] = []
        used: Set[str] = set()
        for scale in descendants(compu_item.element, {"COMPU-SCALE"}):
            lower = first_descendant_text(scale, {"LOWER-LIMIT"})
            upper = first_descendant_text(scale, {"UPPER-LIMIT"})
            label = first_descendant_text(scale, {"SYMBOL"})
            if not label:
                label = first_descendant_text(scale, {"VT"})
            if not label:
                label = first_descendant_text(scale, {"SHORT-LABEL"})
            if not lower or not label or (upper and upper != lower):
                continue
            try:
                numeric = parse_integer(lower)
            except ValueError:
                continue
            name = sanitize_identifier(label, "Value")
            if not name.startswith("k"):
                name = "k" + name[0].upper() + name[1:]
            name = unique_name(name, used)
            values.append((name, numeric))
        return values

    def _render_initial_factory(self, initial: InitialValue, function_name: str) -> List[str]:
        expression = TypeExpr("ref", initial.type_ref)
        initializer = self._render_initial_expr(expression, initial.value_spec, initial.ref)
        return_statement = ("return " + initializer + ";").splitlines()
        return [f"inline {self._render_expr(expression)} {function_name}() {{"] + [
            "  " + line for line in return_statement
        ] + ["}"]

    def _render_initial_expr(self, expression: TypeExpr, value_spec: ET.Element, context: str) -> str:
        value_spec = self._resolve_constant_value(value_spec, context)
        if expression.kind == "ref":
            declaration = self.declarations.get(expression.value)
            if declaration is None:
                raise GeneratorError(f"{context}: initial value references an unbuilt type '{expression.value}'")
            if declaration.kind == "alias":
                return self._render_initial_expr(declaration.alias, value_spec, context)
            if declaration.kind == "enum":
                return self._render_enum_initial(declaration, value_spec, context)

            fields = self._composite_value_specs(value_spec, "RECORD-VALUE-SPECIFICATION", "FIELDS", context)
            if len(fields) != len(declaration.fields):
                raise GeneratorError(
                    f"{context}: record initial value has {len(fields)} field(s), expected {len(declaration.fields)}"
                )
            values = [
                self._render_initial_expr(member.type_expr, field_spec, f"{context}/{member.name}")
                for member, field_spec in zip(declaration.fields, fields)
            ]
            return self._render_braced_initial(declaration.name, values)
        if expression.kind in {"array", "vector", "bytes"}:
            elements = self._composite_value_specs(value_spec, "ARRAY-VALUE-SPECIFICATION", "ELEMENTS", context)
            if expression.kind == "array" and expression.size is not None and len(elements) != expression.size:
                raise GeneratorError(
                    f"{context}: array initial value has {len(elements)} element(s), expected {expression.size}"
                )
            if expression.kind in {"vector", "bytes"} and expression.size is not None:
                if len(elements) > expression.size:
                    raise GeneratorError(
                        f"{context}: variable-size initial value has {len(elements)} element(s), "
                        f"maximum is {expression.size}"
                    )
            if expression.kind == "bytes":
                return self._render_bytes_initial(elements, context)
            values = [
                self._render_initial_expr(expression.element, element, f"{context}[{index}]")
                for index, element in enumerate(elements)
            ]
            return self._render_braced_initial(self._render_expr(expression), values)
        if expression.kind == "string":
            return escape_cpp_string(self._scalar_value_text(value_spec, context, allow_empty=True))
        if expression.kind == "scalar":
            return self._render_scalar_initial(expression.value, self._scalar_value_text(value_spec, context), context)
        raise GeneratorError(f"{context}: unsupported initial value target '{expression.kind}'")

    @staticmethod
    def _render_braced_initial(cpp_type: str, values: Sequence[str]) -> str:
        if not values:
            return f"{cpp_type}{{}}"
        single_line = f"{cpp_type}{{{', '.join(values)}}}"
        if all("\n" not in value for value in values) and len(single_line) <= 110:
            return single_line
        body = ",\n".join("  " + value.replace("\n", "\n  ") for value in values)
        return f"{cpp_type}{{\n{body},\n}}"

    def _resolve_constant_value(
        self, value_spec: ET.Element, context: str, seen: Optional[Set[str]] = None
    ) -> ET.Element:
        if local_name(value_spec.tag) != "CONSTANT-REFERENCE":
            return value_spec
        reference = first_descendant_text(value_spec, {"CONSTANT-REF"})
        if not reference:
            raise GeneratorError(f"{context}: CONSTANT-REFERENCE has no CONSTANT-REF")
        constant = self.model.resolve(reference, context)
        if local_name(constant.element.tag) != "CONSTANT-SPECIFICATION":
            raise GeneratorError(f"{context}: '{reference}' is not a CONSTANT-SPECIFICATION")
        visited = set() if seen is None else seen
        if constant.ref in visited:
            raise GeneratorError(f"{context}: cyclic CONSTANT-REFERENCE involving '{constant.ref}'")
        visited.add(constant.ref)
        container = direct_child(constant.element, "VALUE-SPEC")
        nested = next(iter(container), None) if container is not None else None
        if nested is None:
            raise GeneratorError(f"{constant.ref}: constant has no VALUE-SPEC")
        return self._resolve_constant_value(nested, constant.ref, visited)

    @staticmethod
    def _composite_value_specs(
        value_spec: ET.Element, expected_tag: str, container_tag: str, context: str
    ) -> List[ET.Element]:
        tag = local_name(value_spec.tag)
        if tag != expected_tag:
            raise GeneratorError(f"{context}: expected {expected_tag}, got {tag}")
        container = direct_child(value_spec, container_tag)
        return list(container) if container is not None else []

    @staticmethod
    def _scalar_value_text(value_spec: ET.Element, context: str, allow_empty: bool = False) -> str:
        tag = local_name(value_spec.tag)
        if tag in {"NUMERICAL-VALUE-SPECIFICATION", "TEXT-VALUE-SPECIFICATION"}:
            value_element = direct_child(value_spec, "VALUE")
            if value_element is None:
                raise GeneratorError(f"{context}: scalar initial value has no VALUE")
            raw_value = value_element.text or ""
            value = raw_value if allow_empty else raw_value.strip()
        elif tag == "APPLICATION-VALUE-SPECIFICATION":
            values = [
                (element.text or "") if allow_empty else (element.text or "").strip()
                for element in descendants(value_spec, {"V", "VT"})
            ]
            if len(values) != 1:
                raise GeneratorError(f"{context}: application scalar initial value must contain exactly one V or VT")
            value = values[0]
        else:
            raise GeneratorError(f"{context}: unsupported scalar initial value specification '{tag}'")
        if not value and not allow_empty:
            raise GeneratorError(f"{context}: scalar initial value is empty")
        return value

    def _render_enum_initial(self, declaration: TypeDecl, value_spec: ET.Element, context: str) -> str:
        value = self._scalar_value_text(value_spec, context)
        try:
            numeric = parse_integer(value)
        except ValueError:
            wanted = sanitize_identifier(value, "Value").lower()
            for name, _ in declaration.enum_values:
                label = name[1:] if name.startswith("k") else name
                if label.lower() == wanted:
                    return f"{declaration.name}::{name}"
            raise GeneratorError(f"{context}: unknown {declaration.name} enumeration label '{value}'")

        for name, enum_value in declaration.enum_values:
            if enum_value == numeric:
                return f"{declaration.name}::{name}"
        underlying = self._render_scalar_initial(declaration.enum_underlying, value, context)
        return f"static_cast<{declaration.name}>({underlying})"

    @staticmethod
    def _render_scalar_initial(cpp_type: str, value: str, context: str) -> str:
        if cpp_type == "bool":
            normalized = value.strip().lower()
            if normalized in {"1", "true"}:
                return "true"
            if normalized in {"0", "false"}:
                return "false"
            raise GeneratorError(f"{context}: invalid boolean initial value '{value}'")
        if cpp_type in {"float", "double"}:
            try:
                numeric = float(value)
            except ValueError as error:
                raise GeneratorError(f"{context}: invalid floating-point initial value '{value}'") from error
            if not math.isfinite(numeric):
                raise GeneratorError(f"{context}: non-finite floating-point initial values are not supported")
            rendered = format(numeric, ".9g" if cpp_type == "float" else ".17g")
            if "." not in rendered and "e" not in rendered.lower():
                rendered += ".0"
            return rendered + ("F" if cpp_type == "float" else "")

        match = re.fullmatch(r"(u?)int(8|16|32|64)_t", cpp_type)
        if match is None:
            raise GeneratorError(f"{context}: unsupported scalar C++ type '{cpp_type}'")
        try:
            numeric = parse_integer(value)
        except ValueError as error:
            raise GeneratorError(f"{context}: invalid integer initial value '{value}'") from error
        unsigned = bool(match.group(1))
        bits = int(match.group(2))
        minimum = 0 if unsigned else -(1 << (bits - 1))
        maximum = (1 << bits) - 1 if unsigned else (1 << (bits - 1)) - 1
        if numeric < minimum or numeric > maximum:
            raise GeneratorError(f"{context}: initial value {numeric} does not fit {cpp_type}")

        if bits == 64 and unsigned:
            literal = f"{numeric}ULL"
        elif bits == 64 and numeric == minimum:
            literal = f"(-{maximum}LL - 1LL)"
        elif bits == 64:
            literal = f"{numeric}LL"
        elif unsigned:
            literal = f"{numeric}U"
        else:
            literal = str(numeric)
        return f"static_cast<{cpp_type}>({literal})"

    def _render_bytes_initial(self, elements: Sequence[ET.Element], context: str) -> str:
        if not elements:
            return "vlink::Bytes{}"
        values = [
            self._render_scalar_initial(
                "uint8_t",
                self._scalar_value_text(element, f"{context}[{index}]"),
                f"{context}[{index}]",
            )
            for index, element in enumerate(elements)
        ]
        return self._render_braced_initial("vlink::Bytes", values)

    @staticmethod
    def _dependencies(declaration: TypeDecl) -> Set[str]:
        result: Set[str] = set()

        def visit(expression: TypeExpr) -> None:
            if expression.kind == "ref":
                result.add(expression.value)
            if expression.element is not None:
                visit(expression.element)

        if declaration.alias is not None:
            visit(declaration.alias)
        for member in declaration.fields:
            visit(member.type_expr)
        return result

    def _validate_deployments(self) -> None:
        for source, root in self.model.roots:
            for element in root.iter():
                tag = local_name(element.tag)
                text = (element.text or "").strip()
                if not text:
                    continue
                upper = text.upper().replace("_", "-")
                if "STRING-ENCODING" in tag and "UTF-8" not in upper and "UTF8" not in upper:
                    self.warnings.append(
                        f"{source}: {tag}={text} is incompatible with VLink's dynamic UTF-8 strings"
                    )
                elif "LENGTH-FIELD-SIZE" in tag:
                    try:
                        size = parse_integer(text)
                    except ValueError:
                        continue
                    is_structure = "STRUCT" in tag
                    accepted_sizes = {0} if is_structure else {4, 32}
                    if size not in accepted_sizes:
                        expected = "0" if is_structure else "4 bytes/32 bits"
                        self.warnings.append(
                            f"{source}: {tag}={text} is incompatible with VLink; expected {expected}"
                        )

    def render(self, roots: Sequence[str], namespace: str, inputs: Sequence[Path]) -> str:
        ordered: List[str] = []
        visited: Set[str] = set()

        def append(ref: str) -> None:
            if ref in visited or ref not in self.declarations:
                return
            visited.add(ref)
            for dependency in sorted(self._dependencies(self.declarations[ref])):
                append(dependency)
            ordered.append(ref)

        for ref in roots:
            append(ref)

        lines = [
            "// Generated by tools/autosar/arxml_to_vlink_someip.py. Do not edit.",
            "// Sources: " + ", ".join(path.name for path in inputs),
            "",
            "#pragma once",
            "",
            "#include <array>",
            "#include <cstdint>",
            "#include <string>",
            "#include <vector>",
            "",
            "#include <vlink/serializer.h>",
            "",
        ]

        namespace_parts = [sanitize_identifier(part, "generated") for part in namespace.split("::") if part]
        if namespace_parts:
            lines.append(f"namespace {'::'.join(namespace_parts)} {{")
            lines.append("")

        struct_defaults: Dict[str, InitialValue] = {}
        standalone_initials: List[InitialValue] = []
        for initial in self.initial_values:
            if initial.value_spec is None:
                continue
            resolved = self._resolve_alias(TypeExpr("ref", initial.type_ref))
            declaration = self.declarations.get(resolved.value) if resolved.kind == "ref" else None
            if declaration is None or declaration.kind != "struct":
                standalone_initials.append(initial)
                continue
            if resolved.value in struct_defaults:
                raise GeneratorError(
                    f"{declaration.ref}: multiple prototype initial values cannot share make_default()"
                )
            struct_defaults[resolved.value] = initial

        for ref in ordered:
            declaration = self.declarations[ref]
            lines.extend(self._render_decl(declaration, struct_defaults.get(ref)))
            lines.append("")

        used_factory_names: Set[str] = {self.declarations[ref].name for ref in ordered}
        for initial in standalone_initials:
            base_name = f"make_{snake_case(initial.name)}_initial_value"
            function_name = unique_name(base_name, used_factory_names)
            lines.extend(self._render_initial_factory(initial, function_name))
            lines.append("")

        if namespace_parts:
            lines.append(f"}}  // namespace {'::'.join(namespace_parts)}")
            lines.append("")
        return "\n".join(lines)

    def _render_decl(self, declaration: TypeDecl, initial: Optional[InitialValue] = None) -> List[str]:
        if declaration.kind == "alias":
            return [f"using {declaration.name} = {self._render_expr(declaration.alias)};"]
        if declaration.kind == "enum":
            lines = [f"enum class {declaration.name} : {declaration.enum_underlying} {{"]
            for name, value in declaration.enum_values:
                lines.append(f"  {name} = {self._render_enum_literal(declaration.enum_underlying, value)},")
            lines.append("};")
            return lines

        lines = [f"struct {declaration.name} final {{"]
        for member in declaration.fields:
            lines.append(f"  {self._render_expr(member.type_expr)} {member.name}{{}};")
        lines.append("")
        alignment = declaration.alignment or self.alignment_override
        if alignment is not None and alignment > 1:
            lines.append(f"  VLINK_SOMEIP_ALIGNMENT({alignment}U)")
        if declaration.endian == "big" and declaration.endian_explicit:
            lines.append("  VLINK_SOMEIP_ENDIAN_BIG")
        elif declaration.endian == "little":
            lines.append("  VLINK_SOMEIP_ENDIAN_LITTLE")
        if declaration.struct_length_width not in {None, 0}:
            lines.append(f"  VLINK_SOMEIP_STRUCT_LENGTH({declaration.struct_length_width}U)")
        field_names = ", ".join(self._render_field(member) for member in declaration.fields)
        macro = f"  VLINK_SOMEIP_FIELDS({field_names})"
        if len(macro) <= 120:
            lines.append(macro)
        else:
            lines.append("  VLINK_SOMEIP_FIELDS(")
            for index, member in enumerate(declaration.fields):
                comma = "," if index + 1 < len(declaration.fields) else ""
                lines.append(f"      {self._render_field(member)}{comma}")
            lines.append("  )")
        if initial is not None:
            initializer = self._render_initial_expr(TypeExpr("ref", initial.type_ref), initial.value_spec, initial.ref)
            lines.append("")
            lines.append(f"  [[nodiscard]] static {declaration.name} make_default() {{")
            lines.extend("    " + line for line in ("return " + initializer + ";").splitlines())
            lines.append("  }")
        lines.append("};")
        return lines

    @staticmethod
    def _render_enum_literal(cpp_type: str, value: int) -> str:
        match = re.fullmatch(r"(u?)int(8|16|32|64)_t", cpp_type)
        if match is None:
            return str(value)
        unsigned = bool(match.group(1))
        bits = int(match.group(2))
        if unsigned and bits == 64:
            return f"{value}ULL"
        if unsigned:
            return str(value)
        if bits == 64 and value == -(1 << 63):
            return "(-9223372036854775807LL - 1LL)"
        return f"{value}{'LL' if bits == 64 else ''}"

    @staticmethod
    def _render_field(member: StructField) -> str:
        if member.length_width is None:
            return member.name
        if member.array_dimensions > 1:
            widths = ", ".join(f"{member.length_width}U" for _ in range(member.array_dimensions))
            return f"VLINK_SOMEIP_ARRAY_LENGTH({member.name}, {widths})"
        return f"VLINK_SOMEIP_LENGTH({member.name}, {member.length_width}U)"

    def _render_expr(self, expression: Optional[TypeExpr]) -> str:
        if expression is None:
            raise GeneratorError("internal error: missing type expression")
        if expression.kind == "scalar":
            return expression.value
        if expression.kind == "string":
            return "std::string"
        if expression.kind == "bytes":
            return "vlink::Bytes"
        if expression.kind == "ref":
            if expression.value not in self.symbols:
                raise GeneratorError(f"internal error: missing C++ symbol for '{expression.value}'")
            return self.symbols[expression.value]
        if expression.kind == "vector":
            return f"std::vector<{self._render_expr(expression.element)}>"
        if expression.kind == "array":
            return f"std::array<{self._render_expr(expression.element)}, {expression.size}>"
        raise GeneratorError(f"internal error: unknown type expression '{expression.kind}'")


def write_output(path: str, content: str, inputs: Sequence[Path]) -> None:
    if path == "-":
        sys.stdout.write(content)
        return

    output = Path(path)
    resolved_output = output.resolve()
    if any(resolved_output == source.resolve() for source in inputs):
        raise GeneratorError("output path must not overwrite an input ARXML file")
    if not output.parent.exists():
        raise GeneratorError(f"output directory does not exist: '{output.parent}'")

    temporary_name = ""
    existing_mode = output.stat().st_mode & 0o777 if output.exists() else None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=output.parent, prefix=output.name + ".", delete=False
        ) as temporary:
            temporary.write(content)
            temporary_name = temporary.name
        temporary_path = Path(temporary_name)
        temporary_path.chmod(existing_mode if existing_mode is not None else 0o644)
        temporary_path.replace(output)
    finally:
        if temporary_name:
            try:
                Path(temporary_name).unlink()
            except FileNotFoundError:
                pass


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate C++ types using VLINK_SOMEIP_FIELDS from AUTOSAR ARXML data types.",
        epilog=(
            "The generated wire format follows VLink's non-TLV deployment: configurable payload byte order, "
            "AUTOSAR alignment, 0/1/2/4-byte container and structure lengths, and dynamic UTF-8 strings."
        ),
    )
    parser.add_argument("arxml", nargs="+", type=Path, help="input ARXML file; pass multiple files for cross-file refs")
    parser.add_argument("-o", "--output", default="-", help="output C++ header, or '-' for stdout (default)")
    parser.add_argument("-n", "--namespace", default="", help="generated C++ namespace, for example vehicle::someip")
    parser.add_argument(
        "-t",
        "--type",
        action="append",
        default=[],
        metavar="REF",
        help="generate one type by absolute AUTOSAR ref or unique SHORT-NAME; repeatable",
    )
    parser.add_argument(
        "--prototype",
        action="append",
        default=[],
        metavar="REF",
        help="generate a data prototype type and, when INIT-VALUE exists, its default initializer; repeatable",
    )
    parser.add_argument("--list-types", action="store_true", help="list indexed data types and exit")
    parser.add_argument(
        "--list-prototypes", action="store_true", help="list indexed data prototypes and exit"
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="treat skipped or incompatible deployment warnings as errors",
    )
    parser.add_argument(
        "--byte-arrays-as-bytes",
        action="store_true",
        help="generate variable-size uint8 arrays as vlink::Bytes instead of std::vector<uint8_t>",
    )
    parser.add_argument(
        "--alignment-bits",
        type=int,
        metavar="BITS",
        help="override SOME/IP alignment in AUTOSAR bits when inputs contain multiple deployments",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = create_parser().parse_args(argv)
    try:
        model = ArxmlModel(args.arxml)
        if args.list_types:
            for item in model.type_items():
                print(f"{item.ref}\t{local_name(item.element.tag)}")
            return 0
        if args.list_prototypes:
            for item in model.prototype_items():
                print(f"{item.ref}\t{local_name(item.element.tag)}")
            return 0

        alignment = resolve_someip_alignment(model, args.alignment_bits)
        generator = SomeipGenerator(model, args.byte_arrays_as_bytes, alignment)
        initial_values = generator.select_initial_values(args.prototype)
        if args.type:
            roots = generator.select(args.type)
        elif initial_values:
            roots = []
        else:
            roots = generator.select([])
        for initial in initial_values:
            if initial.type_ref not in roots:
                roots.append(initial.type_ref)
        emitted_roots = generator.build(roots, bool(args.type or args.prototype))
        if not emitted_roots:
            raise GeneratorError("no compatible AUTOSAR data types were found")
        generator.apply_deployments(emitted_roots, args.prototype)

        for warning in generator.warnings:
            print(f"warning: {warning}", file=sys.stderr)
        if args.strict and generator.warnings:
            raise GeneratorError(f"strict mode rejected {len(generator.warnings)} warning(s)")

        content = generator.render(emitted_roots, args.namespace, args.arxml)
        write_output(args.output, content, args.arxml)
        return 0
    except RecursionError:
        print("error: ARXML nesting is too deep", file=sys.stderr)
        return 2
    except (GeneratorError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
