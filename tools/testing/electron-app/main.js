// Deliberately plain. No command-line switches, no window flags beyond the
// size, so both sides run the defaults and what differs is the platform.
//
// The content size is set here and not over the DevTools protocol. Electron
// does not implement Chromium's Browser domain, and setContentSize sets the
// inner size exactly, which is the size the comparison is defined in.
const { app, BrowserWindow } = require('electron');

// Neither side has a GPU worth using, and Electron aborts when its GPU
// process cannot start. Disabling it here keeps both sides identical.
app.disableHardwareAcceleration();

const W = parseInt(process.env.DWC_W || '900', 10);
const H = parseInt(process.env.DWC_H || '700', 10);

app.whenReady().then(() => {
  const win = new BrowserWindow({
    show: true, backgroundColor: '#ffffff',
    webPreferences: { backgroundThrottling: false }
  });
  win.setContentSize(W, H);
  // setContentSize keeps the top-left corner where the default-sized window
  // was placed, so a large content size runs off the screen.
  win.center();
  win.loadURL(process.env.DWC_URL || 'about:blank');
});
app.on('window-all-closed', () => app.quit());
