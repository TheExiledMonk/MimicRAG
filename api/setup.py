from setuptools import Extension, setup
from pathlib import Path

root = Path(__file__).resolve().parent.parent
engine_src = root / "engine" / "src"

sources = [
    str(Path(__file__).parent / "mimicapi" / "_mimicdb.cpp"),
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
    str(engine_src / "array_codec.cpp"),
    str(engine_src / "simd_output.cpp"),
    str(engine_src / "simd_dispatch.cpp"),
]

core_sources = [
    str(Path(__file__).parent / "mimicapi" / "_mimicapi_core.cpp"),
    str(root / "api_cpp" / "src" / "mimicapi_core.cpp"),
]

ext = Extension(
    "mimicapi._mimicdb",
    sources=sources,
    include_dirs=[str(root / "engine" / "include")],
    language="c++",
    extra_compile_args=["-std=c++20"],
)

ext_core = Extension(
    "mimicapi._mimicapi_core",
    sources=core_sources + sources[1:],
    include_dirs=[
        str(root / "engine" / "include"),
        str(root / "api_cpp" / "include"),
    ],
    language="c++",
    extra_compile_args=["-std=c++20"],
)

mongo_sources = [
    str(Path(__file__).parent / "mimicapi" / "_mimicapi_mongo.cpp"),
    str(root / "api_cpp" / "src" / "mimicapi_mongo.cpp"),
    str(root / "api_cpp" / "src" / "mimicapi_core.cpp"),
]

ext_mongo = Extension(
    "mimicapi._mimicapi_mongo",
    sources=mongo_sources + sources[1:],
    include_dirs=[
        str(root / "engine" / "include"),
        str(root / "api_cpp" / "include"),
    ],
    language="c++",
    extra_compile_args=["-std=c++20"],
)

setup(
    name="mimicapi",
    version="0.0.0",
    packages=["mimicapi"],
    ext_modules=[ext, ext_core, ext_mongo],
)
