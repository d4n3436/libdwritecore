// Deliberately plain. Nothing here changes how text is drawn, so both sides
// run the defaults and what differs is the platform.
//
// The content size is set here and not over the DevTools protocol. Electron
// does not implement Chromium's Browser domain, and setContentSize sets the
// inner size exactly, which is the size the comparison is defined in.
const { app, BrowserWindow, nativeTheme } = require('electron');

// Neither side has a GPU worth using, and Electron aborts when its GPU
// process cannot start. Disabling it here keeps both sides identical.
app.disableHardwareAcceleration();

// Animated images are captured at whatever frame the two machines happen to
// have reached, which shows up as a difference no font change can fix.
// kImageAnimationPolicyNoAnimation holds every image on its first frame.
app.commandLine.appendSwitch('blink-settings', 'imageAnimationPolicy=2');

// prefers-color-scheme follows the OS theme of the session's own user, and
// the two sides do not run as the same user. Pinned, so both answer alike.
nativeTheme.themeSource = process.env.DWC_THEME || 'dark';

const W = parseInt(process.env.DWC_W || '1920', 10);
const H = parseInt(process.env.DWC_H || '1080', 10);

app.whenReady().then(() => {
  const win = new BrowserWindow({
    // Frameless and fixed, because a captioned resizable window cannot be
    // sized past its screen, and the hidden session the Windows side runs in
    // has a small one.
    show: true, frame: false, resizable: false, backgroundColor: '#ffffff',
    webPreferences: { backgroundThrottling: false }
  });
  win.setContentSize(W, H);
  // setContentSize keeps the top-left corner where the default-sized window
  // was placed, so a large content size runs off the screen.
  win.center();
  // Centering clamps a window that is larger than its screen back down to
  // it, so the size is stated again and takes effect from the centered
  // corner. A plain size change is not clamped.
  win.setContentSize(W, H);
  win.loadURL(process.env.DWC_URL || 'about:blank');
  // A renderer that dies, which a long webfont sweep can OOM, leaves the
  // DevTools page target stale: /json still answers but every WebSocket to
  // the dead target closes on connect, and Electron implements neither
  // Target.createTarget nor /json/new, so nothing outside this process can
  // bring a page back. Reloading revives the target in place.
  win.webContents.on('render-process-gone', (event, details) => {
    console.error('renderer gone (' + details.reason + '), reloading');
    setTimeout(() => { if (!win.isDestroyed()) win.webContents.reload(); }, 500);
  });
  win.webContents.on('unresponsive', () => {
    console.error('renderer unresponsive, reloading');
    win.webContents.forcefullyCrashRenderer();
  });
});
app.on('window-all-closed', () => app.quit());
