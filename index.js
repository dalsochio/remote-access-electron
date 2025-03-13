const { BrowserWindow, app, ipcMain } = require("electron");
const screenCapture = require("./build/Release/screen_capture.node");
const net = require("net");

let win;
let serviceClient;
let screenCaptureInterval;
let selectedMonitor;

function createWindow() {
  win = new BrowserWindow({
    useContentSize: true,
    resizable: true,
    width: 1920,
    height: 1080,
    center: true,
    show: false,
    webPreferences: {
      contextIsolation: false,
      nodeIntegration: true,
    },
  });

  win.loadFile("index.html");

  win.on("ready-to-show", () => {
    const monitors = screenCapture.listMonitors();

    win?.webContents?.send("monitors", monitors);

    win?.show();
  });
}

function startCapture() {
  if (screenCaptureInterval) {
    return;
  }

  screenCaptureInterval = setInterval(() => {
    const frame = screenCapture.getLatestFrame();

    win?.webContents?.send("frame", {
      data: frame.data,
      width: frame.width,
      height: frame.height,
      timestamp: Date.now(),
    });

    console.log("capturing " + Date.now());
  }, 1000 / 30);
}

function stopCapture() {
  clearInterval(screenCaptureInterval);
  screenCaptureInterval = null;
}

function switchMonitor(index) {
  screenCapture.switchMonitor(index);
}

app.whenReady().then(createWindow);

app.on("window-all-closed", () => {
  stopCapture();

  if (process.platform !== "darwin") {
    app.quit();
  }
});

app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindow();
  }
});

ipcMain.handle("startCapture", () => {
  startCapture();
});

ipcMain.handle("stopCapture", () => {
  stopCapture();
});

ipcMain.handle("switchMonitor", (event, index) => {
  selectedMonitor = index;
  switchMonitor(index);
});
