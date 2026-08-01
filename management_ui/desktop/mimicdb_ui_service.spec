# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path

from PyInstaller.utils.hooks import collect_submodules

spec_root = Path(SPECPATH)
repo_root = spec_root.parents[1]
entrypoint = repo_root / "management_ui" / "desktop" / "service_entry.py"

hiddenimports = (
    collect_submodules("uvicorn")
    + collect_submodules("fastapi")
    + collect_submodules("pydantic")
)

a = Analysis(
    [str(entrypoint)],
    pathex=[str(repo_root)],
    binaries=[],
    datas=[],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
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
    name="mimicdb-ui-service",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
)
