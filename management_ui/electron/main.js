import { app, BrowserWindow } from "electron";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";

const DEFAULT_URL = "http://127.0.0.1:8000/ui";

function serviceBinaryName() {
  return process.platform === "win32" ? "mimicdb-ui-service.exe" : "mimicdb-ui-service";
}

function resolveServiceBinary() {
  if (process.env.MIMICDB_UI_SERVICE_BIN) {
    return process.env.MIMICDB_UI_SERVICE_BIN;
  }
  if (app.isPackaged) {
    return path.join(process.resourcesPath, "service", serviceBinaryName());
  }
  const devPath = path.join(
    process.cwd(),
    "..",
    "desktop",
    "dist",
    serviceBinaryName(),
  );
  return devPath;
}

function waitForReady(url, timeoutMs) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const attempt = () => {
      fetch(url.replace("/ui", "/api/health"))
        .then((res) => {
          if (res.ok) {
            resolve();
            return;
          }
          throw new Error("not ready");
        })
        .catch(() => {
          if (Date.now() - start > timeoutMs) {
            reject(new Error("service startup timeout"));
            return;
          }
          setTimeout(attempt, 200);
        });
    };
    attempt();
  });
}

let serviceProcess = null;

async function createWindow() {
  const serviceBin = resolveServiceBinary();
  if (!existsSync(serviceBin)) {
    throw new Error(`service binary not found at ${serviceBin}`);
  }
  serviceProcess = spawn(serviceBin, [], {
    stdio: "inherit",
    env: {
      ...process.env,
      MIMICDB_UI_BIND_HOST: "127.0.0.1",
      MIMICDB_UI_BIND_PORT: "8000",
    },
  });

  await waitForReady(DEFAULT_URL, 15000);

  const win = new BrowserWindow({
    width: 1200,
    height: 800,
    backgroundColor: "#0b1c2d",
    webPreferences: {
      preload: path.join(import.meta.dirname, "preload.js"),
    },
  });

  win.loadURL(DEFAULT_URL);
}

app.whenReady().then(createWindow);

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow().catch((err) => {
      console.error(err);
      app.quit();
    });
  }
});

app.on("window-all-closed", () => {
  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("before-quit", () => {
  if (serviceProcess) {
    serviceProcess.kill();
    serviceProcess = null;
  }
});
