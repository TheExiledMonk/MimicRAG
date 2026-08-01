from __future__ import annotations

from pathlib import Path
import sys
import threading
import time

import uvicorn
import webview
import webbrowser

repo_root = Path(__file__).resolve().parents[2]
if str(repo_root) not in sys.path:
    sys.path.insert(0, str(repo_root))

from management_ui.service.app import app
from management_ui.service.config import ServiceConfig


class ServerThread(threading.Thread):
    def __init__(self, host: str, port: int) -> None:
        super().__init__(daemon=True)
        self._config = uvicorn.Config(
            app,
            host=host,
            port=port,
            log_level="info",
        )
        self._server = uvicorn.Server(self._config)

    def run(self) -> None:
        self._server.run()


def main() -> None:
    config = ServiceConfig()
    server = ServerThread(config.bind_host, config.bind_port)
    server.start()

    url = f"http://{config.bind_host}:{config.bind_port}/ui"
    webview.create_window(
        "MimicDB Management UI",
        url=url,
        width=1200,
        height=800,
    )

    for _ in range(50):
        if server._server.started:  # type: ignore[attr-defined]
            break
        time.sleep(0.1)

    try:
        webview.start()
    except webview.errors.WebViewException as exc:
        print(f"webview failed: {exc}")
        print("opening system browser instead...")
        webbrowser.open(url)
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
