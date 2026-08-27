from setuptools import Extension, find_packages, setup
from pathlib import Path

root = Path(__file__).resolve().parent.parent

sources = [
    "mimicapi/_mimicdb.cpp",
    "../engine/src/bitmap.cpp",
    "../engine/src/field_vector.cpp",
    "../engine/src/dataset.cpp",
    "../engine/src/mask.cpp",
    "../engine/src/predicate.cpp",
    "../engine/src/segment.cpp",
    "../engine/src/scan.cpp",
    "../engine/src/metrics.cpp",
    "../engine/src/aggregate.cpp",
    "../engine/src/hash.cpp",
    "../engine/src/schema.cpp",
    "../engine/src/dictionary.cpp",
    "../engine/src/array_codec.cpp",
    "../engine/src/compression.cpp",
    "../engine/src/simd_output.cpp",
    "../engine/src/simd_dispatch.cpp",
    "../engine/src/vector_search.cpp",
    "../engine/src/vector_gpu.cpp",
    "../engine/src/vector_ivf.cpp",
]

core_sources = [
    "mimicapi/_mimicapi_core.cpp",
    "../api_cpp/src/mimicapi_core.cpp",
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
    "mimicapi/_mimicapi_mongo.cpp",
    "../api_cpp/src/mimicapi_mongo.cpp",
    "../api_cpp/src/mimicapi_core.cpp",
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
    packages=find_packages(),
    ext_modules=[ext, ext_core, ext_mongo],
)
