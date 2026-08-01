import site
import sys
from pathlib import Path


def _add_site_dir(path: Path) -> None:
    if path.is_dir() and str(path) not in sys.path:
        site.addsitedir(str(path))


# PyInstaller bundles exclude system dist-packages. Add them back for GTK (gi).
_add_site_dir(Path("/usr/lib/python3/dist-packages"))
_add_site_dir(Path(f"/usr/lib/python{sys.version_info.major}.{sys.version_info.minor}/dist-packages"))
