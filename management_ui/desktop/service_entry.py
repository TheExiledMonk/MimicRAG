from __future__ import annotations

import uvicorn

from management_ui.service.app import app
from management_ui.service.config import ServiceConfig


def main() -> None:
    config = ServiceConfig()
    uvicorn.run(
        app,
        host=config.bind_host,
        port=config.bind_port,
        log_level="info",
    )


if __name__ == "__main__":
    main()
