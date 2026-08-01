BUILD_DIR ?= build
RAG_BUILD_DIR ?= build-release

.PHONY: all build rag-build clean test bench bench-cpp py-ext ui-build ui-binary ui-service-bin ui-electron

all: build py-ext ui-build

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

rag-build:
	cmake -S . -B $(RAG_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DMIMICRAG_ENABLE_LLAMA=ON -DMIMICRAG_LLAMA_GPU=auto -DMIMICDB_NATIVE_ARCH=ON
	cmake --build $(RAG_BUILD_DIR) -j --target mimicrag_server

py-ext:
	cd api && ../.venv/bin/python setup.py build_ext --inplace

ui-build:
	cd management_ui/ui && npm run build

PYINSTALLER ?= .venv/bin/pyinstaller
UI_SPEC ?= management_ui/desktop/mimicdb_ui.spec
UI_SERVICE_SPEC ?= management_ui/desktop/mimicdb_ui_service.spec

ui-binary: ui-build
	$(PYINSTALLER) $(UI_SPEC)

ui-service-bin:
	$(PYINSTALLER) $(UI_SERVICE_SPEC)

ui-electron: ui-build ui-service-bin
	cd management_ui/electron && npm install && npm run dist

clean:
	rm -rf $(BUILD_DIR)

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: py-ext
	PYTHONPATH=api .venv/bin/python benchmarks/bench_suite.py $(ARGS)
	PYTHONPATH=api .venv/bin/python benchmarks/bench_cpp_core.py $(CPP_CORE_ARGS)

bench-cpp: py-ext
	PYTHONPATH=api .venv/bin/python benchmarks/bench_cpp_core.py $(CPP_CORE_ARGS)
