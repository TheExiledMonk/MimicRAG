from __future__ import annotations

from dataclasses import dataclass
import datetime
import math


@dataclass
class FunctionDef:
    name: str
    func: callable
    arity: tuple[int, int] | None = None


def _check_arity(values: list, arity: tuple[int, int] | None) -> None:
    if arity is None:
        return
    min_args, max_args = arity
    if not (min_args <= len(values) <= max_args):
        raise ValueError(f"function expects {min_args}-{max_args} args")


def _lower(values: list):
    _check_arity(values, (1, 1))
    return str(values[0]).lower()


def _upper(values: list):
    _check_arity(values, (1, 1))
    return str(values[0]).upper()


def _length(values: list):
    _check_arity(values, (1, 1))
    return len(str(values[0]))


def _substring(values: list):
    _check_arity(values, (2, 3))
    text = str(values[0])
    start = int(values[1]) - 1
    if len(values) == 2:
        return text[start:]
    length = int(values[2])
    return text[start:start + length]


def _trim(values: list):
    _check_arity(values, (1, 1))
    return str(values[0]).strip()


def _abs(values: list):
    _check_arity(values, (1, 1))
    return abs(float(values[0]))


def _round(values: list):
    _check_arity(values, (1, 2))
    number = float(values[0])
    if len(values) == 2:
        return round(number, int(values[1]))
    return round(number)


def _floor(values: list):
    _check_arity(values, (1, 1))
    return math.floor(float(values[0]))


def _ceil(values: list):
    _check_arity(values, (1, 1))
    return math.ceil(float(values[0]))


def _coalesce(values: list):
    _check_arity(values, (1, 1024))
    for val in values:
        if val is not None:
            return val
    return None


def _current_date(values: list):
    _check_arity(values, (0, 0))
    return datetime.date.today().isoformat()


def _current_timestamp(values: list):
    _check_arity(values, (0, 0))
    return datetime.datetime.utcnow().isoformat() + "Z"


_REGISTRY = {
    "LOWER": FunctionDef("LOWER", _lower, (1, 1)),
    "UPPER": FunctionDef("UPPER", _upper, (1, 1)),
    "LENGTH": FunctionDef("LENGTH", _length, (1, 1)),
    "SUBSTRING": FunctionDef("SUBSTRING", _substring, (2, 3)),
    "SUBSTR": FunctionDef("SUBSTRING", _substring, (2, 3)),
    "TRIM": FunctionDef("TRIM", _trim, (1, 1)),
    "ABS": FunctionDef("ABS", _abs, (1, 1)),
    "ROUND": FunctionDef("ROUND", _round, (1, 2)),
    "FLOOR": FunctionDef("FLOOR", _floor, (1, 1)),
    "CEIL": FunctionDef("CEIL", _ceil, (1, 1)),
    "CEILING": FunctionDef("CEIL", _ceil, (1, 1)),
    "COALESCE": FunctionDef("COALESCE", _coalesce, (1, 1024)),
    "CURRENT_DATE": FunctionDef("CURRENT_DATE", _current_date, (0, 0)),
    "CURRENT_TIMESTAMP": FunctionDef("CURRENT_TIMESTAMP", _current_timestamp, (0, 0)),
}


def get_function(name: str) -> FunctionDef:
    key = name.upper()
    if key not in _REGISTRY:
        raise KeyError(f"unknown function '{name}'")
    return _REGISTRY[key]


def evaluate(name: str, values: list):
    func = get_function(name)
    return func.func(values)
