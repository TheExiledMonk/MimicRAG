from __future__ import annotations

from dataclasses import dataclass


CanonicalType = str


@dataclass
class TypeMapping:
    canonical: CanonicalType
    aliases: list[str]


_TYPE_TABLE = [
    TypeMapping("INT64", ["INTEGER", "INT", "BIGINT", "SMALLINT", "NUMBER"]),
    TypeMapping("FLOAT64", ["FLOAT", "DOUBLE", "REAL", "NUMERIC", "DECIMAL"]),
    TypeMapping("STRING", ["VARCHAR", "TEXT", "CHAR", "CLOB"]),
    TypeMapping("BOOL", ["BOOLEAN", "BOOL"]),
]


def canonical_type(name: str) -> CanonicalType:
    upper = name.strip().upper()
    for mapping in _TYPE_TABLE:
        if upper == mapping.canonical or upper in mapping.aliases:
            return mapping.canonical
    raise KeyError(f"unknown type '{name}'")


def type_table() -> list[TypeMapping]:
    return list(_TYPE_TABLE)
