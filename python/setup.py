from __future__ import annotations

from pathlib import Path

from setuptools import find_packages, setup

try:
    from pybind11.setup_helpers import Pybind11Extension, build_ext
except Exception as exc:  # noqa: BLE001
    raise RuntimeError(
        "pybind11 is required to build the native extension. "
        "Install build deps with: python -m pip install -r requirements-build.txt"
    ) from exc

ROOT = Path(__file__).resolve().parent
CPP_DIR = Path("..") / "cpp"

ext_modules = [
    Pybind11Extension(
        "llm_structured._native",
        [
            "bindings/llm_structured_pybind.cpp",
            "../cpp/src/llm_structured.cpp",
        ],
        include_dirs=[
            "../cpp/include",
        ],
        cxx_std=17,
        # MSVC: make sure we compile as C++17.
        extra_compile_args=["/std:c++17"] if __import__("sys").platform.startswith("win") else [],
    )
]

setup(
    name="llm-structured",
    version="0.0.0",
    packages=find_packages(),
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    zip_safe=False,
)
