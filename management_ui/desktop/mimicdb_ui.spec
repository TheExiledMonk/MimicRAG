# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path

from PyInstaller.utils.hooks import collect_submodules

spec_root = Path(SPECPATH)
repo_root = spec_root.parents[1]
ui_dist = repo_root / "management_ui" / "ui" / "dist"
launcher = repo_root / "management_ui" / "desktop" / "launcher.py"
runtime_hook = repo_root / "management_ui" / "desktop" / "pyi_runtime_hook.py"

datas = []
if ui_dist.exists():
    datas.append((str(ui_dist), "ui_dist"))

hiddenimports = (
    collect_submodules("uvicorn")
    + collect_submodules("fastapi")
    + collect_submodules("pydantic")
    + collect_submodules("webview")
    + ["gi", "optparse"]
)

a = Analysis(
    [str(launcher)],
    pathex=[str(repo_root)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[str(runtime_hook)],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=None,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=None)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="mimicdb-ui",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
)
