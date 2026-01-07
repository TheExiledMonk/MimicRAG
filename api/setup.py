from setuptools import Extension, setup
from pathlib import Path

root = Path(__file__).resolve().parent.parent
engine_src = root / "engine" / "src"

sources = [
    str(Path(__file__).parent / "pcdb" / "_pcdb.cpp"),
    str(engine_src / "bitmap.cpp"),
    str(engine_src / "field_vector.cpp"),
    str(engine_src / "dataset.cpp"),
    str(engine_src / "mask.cpp"),
    str(engine_src / "predicate.cpp"),
    str(engine_src / "segment.cpp"),
    str(engine_src / "scan.cpp"),
    str(engine_src / "metrics.cpp"),
    str(engine_src / "aggregate.cpp"),
    str(engine_src / "hash.cpp"),
    str(engine_src / "schema.cpp"),
    str(engine_src / "dictionary.cpp"),
]

ext = Extension(
    "pcdb._pcdb",
    sources=sources,
    include_dirs=[str(root / "engine" / "include")],
    language="c++",
    extra_compile_args=["-std=c++20"],
)

setup(
    name="pcdb",
    version="0.0.0",
    packages=["pcdb"],
    ext_modules=[ext],
)
