// ================= 檢視切換 =================
const VIEW_TITLES = {
  console: "控制台",
  wifi: "Wi-Fi 設定",
  stream: "影像串流",
  motion: "移動偵測",
  alarm: "告警紀錄",
  playback: "錄影回放",
  ota: "OTA 更新",
};
let currentView = "console";

document.querySelectorAll(".nav-item").forEach((item) => {
  item.addEventListener("click", () => {
    currentView = item.dataset.view;
    document
      .querySelectorAll(".nav-item")
      .forEach((i) => i.classList.toggle("active", i === item));
    document
      .querySelectorAll(".panel")
      .forEach((p) =>
        p.classList.toggle("active", p.id === "panel-" + currentView),
      );
    document.getElementById("viewTitle").textContent = VIEW_TITLES[currentView];
    if (currentView === "alarm") loadAlarms();
    if (currentView === "playback") loadRecordings();
    else stopPlayback(); // 離開回放頁即停止串流下載
  });
});

// ================= 系統狀態輪詢 =================
const statusDot = document.getElementById("statusDot");
const statusText = document.getElementById("statusText");
const footDot = document.getElementById("footDot");
const footText = document.getElementById("footText");
const stConn = document.getElementById("stConn");
const stSsid = document.getElementById("stSsid");
const stIp = document.getElementById("stIp");
const stCam = document.getElementById("stCam");
const stStorage = document.getElementById("stStorage");
const ledBtn = document.getElementById("ledBtn");
const ledBtnText = document.getElementById("ledBtnText");

let ledState = false;
let ledSyncing = false;

function setLedUI(on) {
  ledState = on;
  ledBtn.classList.toggle("on", on);
  ledBtn.setAttribute("aria-checked", on ? "true" : "false");
  ledBtnText.textContent = on ? "ON" : "OFF";
  ledBtnText.style.color = on ? "var(--ok)" : "";
}

async function refreshStatus() {
  try {
    const res = await fetch("/status");
    const data = await res.json();

    if (data.connected) {
      statusDot.className = "dot on";
      statusText.textContent = data.ssid + " | " + data.ip;
      footDot.className = "dot on";
      footText.textContent = "SYSTEM ONLINE";
    } else if (data.connecting) {
      statusDot.className = "dot busy";
      statusText.textContent = "Wi-Fi 連線中...";
      footDot.className = "dot busy";
      footText.textContent = "CONNECTING";
    } else {
      statusDot.className = "dot";
      statusText.textContent = "AP 模式 (未連上 Wi-Fi)";
      footDot.className = "dot";
      footText.textContent = "AP MODE";
    }

    stConn.textContent = data.connected
      ? "已連線"
      : data.connecting
        ? "連線中..."
        : "未連線";
    stConn.style.color = data.connected ? "var(--ok)" : "";
    stSsid.textContent = data.connected ? data.ssid : "--";
    stIp.textContent = data.connected ? data.ip : "--";
    stCam.textContent = data.camera ? "運作中" : "初始化失敗";
    stCam.style.color = data.camera ? "var(--ok)" : "var(--err)";
    stStorage.textContent = data.sd
      ? "SD 卡 (appsetting.json)"
      : "NVS / RAM (TFT 模式)";

    if (!ledSyncing) setLedUI(!!data.led);
  } catch (e) {
    statusDot.className = "dot err";
    statusText.textContent = "無法取得狀態";
    footDot.className = "dot err";
    footText.textContent = "OFFLINE";
  }
}

ledBtn.addEventListener("click", async () => {
  ledSyncing = true;
  const next = !ledState;
  setLedUI(next); // 先切畫面，失敗時下次輪詢會修正
  try {
    const res = await fetch("/led?state=" + (next ? "1" : "0"));
    const data = await res.json();
    setLedUI(!!data.led);
  } catch (e) {
    /* 下次輪詢會自動修正畫面 */
  }
  ledSyncing = false;
});

// ================= Wi-Fi 設定 =================
const netList = document.getElementById("netList");
const ssidInput = document.getElementById("ssidInput");
const passInput = document.getElementById("passInput");
const connectBtn = document.getElementById("connectBtn");
const connMsg = document.getElementById("connMsg");
const scanBtn = document.getElementById("scanBtn");
const clearBtn = document.getElementById("clearBtn");
const clearMsg = document.getElementById("clearMsg");

async function doScan() {
  scanBtn.disabled = true;
  scanBtn.textContent = "掃描中...";
  netList.innerHTML = "";
  try {
    const res = await fetch("/scan");
    const nets = await res.json();
    nets.sort((a, b) => b.rssi - a.rssi);
    nets.forEach((n) => {
      const li = document.createElement("li");
      li.textContent = (n.secure ? "🔒 " : "") + n.ssid;
      const rssi = document.createElement("span");
      rssi.className = "muted";
      rssi.textContent = n.rssi + " dBm";
      li.appendChild(rssi);
      li.addEventListener("click", () => {
        ssidInput.value = n.ssid;
        passInput.focus();
        [...netList.children].forEach((c) => c.classList.remove("selected"));
        li.classList.add("selected");
      });
      netList.appendChild(li);
    });
    if (nets.length === 0) {
      netList.innerHTML = '<li class="muted">找不到任何 Wi-Fi</li>';
    }
  } catch (e) {
    netList.innerHTML = '<li class="muted">掃描失敗</li>';
  }
  scanBtn.disabled = false;
  scanBtn.textContent = "掃描";
}

async function doConnect() {
  const ssid = ssidInput.value.trim();
  if (!ssid) {
    connMsg.className = "msg err";
    connMsg.textContent = "請輸入或選擇 SSID";
    return;
  }
  connectBtn.disabled = true;
  connMsg.className = "msg";
  connMsg.textContent = "連線中，請稍候...";
  try {
    const body = new URLSearchParams({ ssid, password: passInput.value });
    const res = await fetch("/connect", { method: "POST", body });
    const data = await res.json();
    if (data.ok) {
      connMsg.className = "msg ok";
      connMsg.textContent = "已送出連線請求，請稍候...";
    } else {
      connMsg.className = "msg err";
      connMsg.textContent = data.error || "連線請求失敗";
    }
  } catch (e) {
    connMsg.className = "msg err";
    connMsg.textContent = "連線請求失敗";
  }
  connectBtn.disabled = false;
}

async function doClearWifi() {
  if (!confirm("確定要清除已儲存的 Wi-Fi 設定並中斷連線？")) return;
  clearBtn.disabled = true;
  clearMsg.className = "msg";
  clearMsg.textContent = "清除中...";
  try {
    const res = await fetch("/clear", { method: "POST" });
    const data = await res.json();
    if (data.ok) {
      clearMsg.className = "msg ok";
      clearMsg.textContent = "已清除，請重新掃描並連線";
      doScan();
    } else {
      clearMsg.className = "msg err";
      clearMsg.textContent = "清除失敗";
    }
  } catch (e) {
    clearMsg.className = "msg err";
    clearMsg.textContent = "清除失敗";
  }
  clearBtn.disabled = false;
  refreshStatus();
}

scanBtn.addEventListener("click", doScan);
connectBtn.addEventListener("click", doConnect);
clearBtn.addEventListener("click", doClearWifi);

// ================= 影像擷取共用迴圈 =================
// 影像串流與移動偵測共用同一個 /capture 輪詢迴圈，
// 只有在這兩個頁面其中之一開啟時才抓影像，避免占用 ESP32。
const streamCanvas = document.getElementById("streamCanvas");
const streamCtx = streamCanvas.getContext("2d");
const streamHud = document.getElementById("streamHud");
const streamToggleBtn = document.getElementById("streamToggleBtn");
const motionCanvas = document.getElementById("motionCanvas");
const motionCtx = motionCanvas.getContext("2d");

let streamPaused = false;
let otaUploading = false;
let frameTimes = [];

streamToggleBtn.addEventListener("click", () => {
  streamPaused = !streamPaused;
  streamToggleBtn.textContent = streamPaused ? "繼續" : "暫停";
});

// ---------- 影像旋轉 ----------
// 影像串流與移動偵測共用同一個旋轉角度 (0/90/180/270)，
// 存在 localStorage，重新整理後保留設定。
const streamRotateBtn = document.getElementById("streamRotateBtn");
const motionRotateBtn = document.getElementById("motionRotateBtn");

let rotateDeg = Number(localStorage.getItem("rotateDeg")) || 0;
if (![0, 90, 180, 270].includes(rotateDeg)) rotateDeg = 0;

function updateRotateUI() {
  streamRotateBtn.textContent = "旋轉 " + rotateDeg + "°";
  motionRotateBtn.textContent = "旋轉 " + rotateDeg + "°";
}
updateRotateUI();

function cycleRotate() {
  rotateDeg = (rotateDeg + 90) % 360;
  localStorage.setItem("rotateDeg", rotateDeg);
  updateRotateUI();
  drawOverlay(); // 偵測區域以感光元件座標保存，旋轉後重畫對應的顯示位置
}
streamRotateBtn.addEventListener("click", cycleRotate);
motionRotateBtn.addEventListener("click", cycleRotate);

// 以畫布中心旋轉繪製影像 (畫布與影像皆為 240x240 正方形)
function drawRotated(ctx, bmp, w, h) {
  ctx.save();
  ctx.translate(w / 2, h / 2);
  ctx.rotate((rotateDeg * Math.PI) / 180);
  ctx.drawImage(bmp, -w / 2, -h / 2, w, h);
  ctx.restore();
}

async function frameLoop() {
  for (;;) {
    const wantStream =
      currentView === "stream" && !streamPaused && !otaUploading;
    const wantMotion = currentView === "motion" && !otaUploading;
    if (!wantStream && !wantMotion) {
      await new Promise((r) => setTimeout(r, 300));
      continue;
    }
    try {
      const res = await fetch("/capture", { cache: "no-store" });
      if (!res.ok) throw new Error("capture " + res.status);
      const blob = await res.blob();
      const bmp = await createImageBitmap(blob);

      if (wantStream) {
        drawRotated(streamCtx, bmp, streamCanvas.width, streamCanvas.height);
        const now = performance.now();
        frameTimes.push(now);
        frameTimes = frameTimes.filter((t) => now - t < 2000);
        streamHud.textContent = "FPS " + (frameTimes.length / 2).toFixed(1);
      }
      if (wantMotion) {
        drawRotated(motionCtx, bmp, motionCanvas.width, motionCanvas.height);
      }
      bmp.close();
    } catch (e) {
      if (wantStream) streamHud.textContent = "NO SIGNAL";
      if (wantMotion) motionHud.textContent = "NO SIGNAL";
      await new Promise((r) => setTimeout(r, 800));
    }
    await new Promise((r) => setTimeout(r, 60));
  }
}

// ================= 移動偵測 (偵測在 ESP32 裝置端進行) =================
const motionOverlay = document.getElementById("motionOverlay");
const overlayCtx = motionOverlay.getContext("2d");
const motionBadge = document.getElementById("motionBadge");
const motionHud = document.getElementById("motionHud");
const motionClearBtn = document.getElementById("motionClearBtn");
const sensRange = document.getElementById("sensRange");

// 偵測設定保存在 ESP32；region 一律用「感光元件方向」的 240x240 座標，
// 與韌體端一致，顯示時再依 rotateDeg 轉成畫布座標。
let region = null; // {x, y, w, h} 感光元件座標
let selecting = null; // 拖曳中的起點 {x, y} (畫布座標)
let deviceTriggered = false; // 由 /motion/status 輪詢更新
let motionSynced = false; // 是否已從裝置取回一次設定 (頁面重整後還原)

// 依 90° 倍數旋轉矩形座標 (size x size 正方形畫布)，負角度為反向旋轉
function rotateRect(r, deg, size) {
  switch (((deg % 360) + 360) % 360) {
    case 90:
      return { x: size - r.y - r.h, y: r.x, w: r.h, h: r.w };
    case 180:
      return { x: size - r.x - r.w, y: size - r.y - r.h, w: r.w, h: r.h };
    case 270:
      return { x: r.y, y: size - r.x - r.w, w: r.h, h: r.w };
    default:
      return { x: r.x, y: r.y, w: r.w, h: r.h };
  }
}

async function sendMotionConfig() {
  const body = new URLSearchParams({
    enabled: region ? "1" : "0",
    x: region ? Math.round(region.x) : 0,
    y: region ? Math.round(region.y) : 0,
    w: region ? Math.round(region.w) : 0,
    h: region ? Math.round(region.h) : 0,
    sens: sensRange.value,
  });
  try {
    await fetch("/motion/config", { method: "POST", body });
  } catch (e) {
    /* 失敗時下次輪詢會反映裝置實際設定 */
  }
}

function canvasPos(e) {
  const rect = motionOverlay.getBoundingClientRect();
  const x = ((e.clientX - rect.left) / rect.width) * motionOverlay.width;
  const y = ((e.clientY - rect.top) / rect.height) * motionOverlay.height;
  return {
    x: Math.max(0, Math.min(motionOverlay.width, x)),
    y: Math.max(0, Math.min(motionOverlay.height, y)),
  };
}

motionOverlay.addEventListener("pointerdown", (e) => {
  e.preventDefault();
  motionOverlay.setPointerCapture(e.pointerId);
  selecting = { start: canvasPos(e), cur: canvasPos(e) };
});
motionOverlay.addEventListener("pointermove", (e) => {
  if (!selecting) return;
  selecting.cur = canvasPos(e);
  drawOverlay();
});
motionOverlay.addEventListener("pointerup", (e) => {
  if (!selecting) return;
  const { start, cur } = selecting;
  selecting = null;
  const x = Math.min(start.x, cur.x),
    y = Math.min(start.y, cur.y);
  const w = Math.abs(cur.x - start.x),
    h = Math.abs(cur.y - start.y);
  if (w >= 12 && h >= 12) {
    // 框選畫在旋轉後的畫面上，反向旋轉回感光元件座標再交給韌體
    region = rotateRect({ x, y, w, h }, -rotateDeg, motionOverlay.width);
    sendMotionConfig();
  }
  drawOverlay();
});

motionClearBtn.addEventListener("click", () => {
  region = null;
  motionHud.textContent = "DIFF --%";
  sendMotionConfig();
  drawOverlay();
});

sensRange.addEventListener("change", () => {
  if (region) sendMotionConfig();
});

// ---------- Discord 推播通知設定 ----------
const webhookInput = document.getElementById("webhookInput");
const webhookSaveBtn = document.getElementById("webhookSaveBtn");
const webhookTestBtn = document.getElementById("webhookTestBtn");
const webhookMsg = document.getElementById("webhookMsg");

function setWebhookMsg(cls, text) {
  webhookMsg.className = "msg " + cls;
  webhookMsg.textContent = text;
}

async function loadNotifyConfig() {
  try {
    const res = await fetch("/notify/config");
    const d = await res.json();
    webhookInput.value = d.webhook || "";
  } catch (e) {
    /* 讀取失敗維持空白，可直接重新輸入 */
  }
}

webhookSaveBtn.addEventListener("click", async () => {
  const url = webhookInput.value.trim();
  try {
    const body = new URLSearchParams({ webhook: url });
    const res = await fetch("/notify/config", { method: "POST", body });
    const d = await res.json();
    if (d.ok) setWebhookMsg("ok", url ? "已儲存" : "已清除，推播停用");
    else setWebhookMsg("err", d.error || "儲存失敗");
  } catch (e) {
    setWebhookMsg("err", "儲存失敗");
  }
});

webhookTestBtn.addEventListener("click", async () => {
  setWebhookMsg("", "傳送中...");
  try {
    const res = await fetch("/notify/test", { method: "POST" });
    const d = await res.json();
    if (d.ok) setWebhookMsg("ok", "已排入傳送，請到 Discord 頻道確認");
    else setWebhookMsg("err", d.error || "測試失敗");
  } catch (e) {
    setWebhookMsg("err", "測試失敗");
  }
});

// ---------- 裝置端偵測狀態輪詢 ----------
// 不管停在哪個頁面都輪詢：偵測跑在 ESP32 上，新告警要即時跳通知。
let lastSeenAlarmId = null; // 第一次輪詢先當基準，之後 id 變大 = 新告警

async function pollMotion() {
  try {
    const res = await fetch("/motion/status");
    const d = await res.json();

    if (!motionSynced) {
      // 頁面剛載入：還原裝置上已生效的區域與靈敏度
      motionSynced = true;
      if (d.enabled) region = { x: d.x, y: d.y, w: d.w, h: d.h };
      sensRange.value = d.sens;
    }

    deviceTriggered = !!d.triggered;
    if (d.enabled) {
      const recTag = d.recording ? " · 錄影中" : "";
      motionHud.textContent =
        "DIFF " + Number(d.diff).toFixed(1) + "%" + (d.recording ? " REC" : "");
      motionBadge.className = deviceTriggered ? "badge alert" : "badge normal";
      motionBadge.textContent =
        (deviceTriggered ? "偵測到移動" : "監控中") + recTag;
    } else {
      motionBadge.className = "badge";
      motionBadge.textContent = "未設定區域";
    }
    drawOverlay();

    updateAlarmBadge(d.count);
    if (lastSeenAlarmId === null) {
      lastSeenAlarmId = d.lastId;
    } else if (d.lastId > lastSeenAlarmId) {
      lastSeenAlarmId = d.lastId;
      notifyAlarm();
      if (currentView === "alarm") loadAlarms();
    }
  } catch (e) {
    /* 離線狀態由 refreshStatus 顯示，這裡靜默重試 */
  }
}
setInterval(pollMotion, 1000);

function drawOverlay() {
  overlayCtx.clearRect(0, 0, motionOverlay.width, motionOverlay.height);

  if (selecting) {
    const { start, cur } = selecting;
    overlayCtx.strokeStyle = "rgba(0, 229, 255, 0.9)";
    overlayCtx.setLineDash([5, 4]);
    overlayCtx.lineWidth = 1.5;
    overlayCtx.strokeRect(
      Math.min(start.x, cur.x),
      Math.min(start.y, cur.y),
      Math.abs(cur.x - start.x),
      Math.abs(cur.y - start.y),
    );
    overlayCtx.setLineDash([]);
    return;
  }
  if (!region) return;

  // region 為感光元件座標，先轉成目前旋轉角度下的畫布座標
  const disp = rotateRect(region, rotateDeg, motionOverlay.width);
  const color = deviceTriggered ? "#ff3860" : "#2bff88";
  overlayCtx.strokeStyle = color;
  overlayCtx.shadowColor = color;
  overlayCtx.shadowBlur = 8;
  overlayCtx.lineWidth = 2;
  overlayCtx.strokeRect(disp.x, disp.y, disp.w, disp.h);
  overlayCtx.shadowBlur = 0;

  // 四角加強線，做出瞄準框效果
  const L = Math.min(14, disp.w / 3, disp.h / 3);
  overlayCtx.lineWidth = 4;
  const { x, y, w, h } = disp;
  const corners = [
    [
      [x, y + L],
      [x, y],
      [x + L, y],
    ],
    [
      [x + w - L, y],
      [x + w, y],
      [x + w, y + L],
    ],
    [
      [x + w, y + h - L],
      [x + w, y + h],
      [x + w - L, y + h],
    ],
    [
      [x + L, y + h],
      [x, y + h],
      [x, y + h - L],
    ],
  ];
  corners.forEach((pts) => {
    overlayCtx.beginPath();
    overlayCtx.moveTo(pts[0][0], pts[0][1]);
    overlayCtx.lineTo(pts[1][0], pts[1][1]);
    overlayCtx.lineTo(pts[2][0], pts[2][1]);
    overlayCtx.stroke();
  });
}

// ================= 告警紀錄 =================
const alarmListEl = document.getElementById("alarmListEl");
const alarmClearBtn = document.getElementById("alarmClearBtn");
const alarmNavBadge = document.getElementById("alarmNavBadge");
const toast = document.getElementById("toast");

function updateAlarmBadge(count) {
  alarmNavBadge.hidden = !count;
  alarmNavBadge.textContent = count;
}

function fmtAlarmTime(epoch) {
  // 裝置時間靠網頁載入時同步，未同步前的紀錄只有開機經過秒數
  if (epoch < 1000000000) return "時間未同步";
  return new Date(epoch * 1000).toLocaleString("zh-TW", { hour12: false });
}

async function loadAlarms() {
  try {
    const res = await fetch("/alarms");
    const list = await res.json();
    alarmListEl.innerHTML = "";
    if (!list.length) {
      alarmListEl.innerHTML =
        '<p class="muted" style="margin:0">目前沒有告警紀錄</p>';
      return;
    }
    list.forEach((a) => {
      const item = document.createElement("div");
      item.className = "alarm-item";

      const link = document.createElement("a");
      link.href = "/alarm/img?id=" + a.id;
      link.target = "_blank";
      const img = document.createElement("img");
      img.src = link.href;
      img.loading = "lazy";
      img.style.transform = "rotate(" + rotateDeg + "deg)"; // 與串流頁同角度顯示
      link.appendChild(img);

      const info = document.createElement("div");
      info.className = "alarm-info";
      const idEl = document.createElement("span");
      idEl.className = "alarm-id";
      idEl.textContent = "#" + a.id;
      const timeEl = document.createElement("span");
      timeEl.className = "alarm-time";
      timeEl.textContent = fmtAlarmTime(a.time);
      const noteEl = document.createElement("span");
      noteEl.className = "muted";
      noteEl.textContent = "移動偵測觸發";
      info.append(idEl, timeEl, noteEl);

      item.append(link, info);
      alarmListEl.appendChild(item);
    });
  } catch (e) {
    alarmListEl.innerHTML =
      '<p class="muted" style="margin:0">讀取告警紀錄失敗</p>';
  }
}

alarmClearBtn.addEventListener("click", async () => {
  if (!confirm("確定要清除全部告警紀錄？")) return;
  try {
    await fetch("/alarm/clear", { method: "POST" });
  } catch (e) {
    /* 清除失敗時列表重新載入會顯示實際內容 */
  }
  updateAlarmBadge(0);
  loadAlarms();
});

// ---------- 觸發通知：右下角 toast + 蜂鳴 ----------
let toastTimer = null;
function notifyAlarm() {
  toast.textContent =
    "⚠ 偵測到移動！已擷取告警影像 " +
    new Date().toLocaleTimeString("zh-TW", { hour12: false });
  toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    toast.hidden = true;
  }, 4000);
  beep();
}

let audioCtx = null;
function beep() {
  try {
    audioCtx =
      audioCtx || new (window.AudioContext || window.webkitAudioContext)();
    for (let i = 0; i < 2; i++) {
      const t = audioCtx.currentTime + i * 0.25;
      const o = audioCtx.createOscillator();
      const g = audioCtx.createGain();
      o.connect(g);
      g.connect(audioCtx.destination);
      o.frequency.value = 880;
      g.gain.setValueAtTime(0.12, t);
      g.gain.exponentialRampToValueAtTime(0.001, t + 0.18);
      o.start(t);
      o.stop(t + 0.2);
    }
  } catch (e) {
    /* 瀏覽器可能在使用者互動前禁止播放音效 */
  }
}

// ================= 錄影回放 =================
const recListEl = document.getElementById("recListEl");
const recCanvas = document.getElementById("recCanvas");
const recCtx = recCanvas.getContext("2d");
const recHud = document.getElementById("recHud");
const recStatus = document.getElementById("recStatus");
const recStopBtn = document.getElementById("recStopBtn");
const recRefreshBtn = document.getElementById("recRefreshBtn");

let playbackActive = false;
let playbackAbort = null;

// 檔名 20260716_143005_.avi -> 2026-07-16 14:30:05
function recNameToTime(name) {
  const m = name.match(/^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})_/);
  if (!m) return name;
  return m[1] + "-" + m[2] + "-" + m[3] + " " + m[4] + ":" + m[5] + ":" + m[6];
}

async function loadRecordings() {
  try {
    const res = await fetch("/recordings");
    const list = await res.json();
    list.sort((a, b) => (a.name < b.name ? 1 : -1)); // 檔名含日期時間，新到舊
    recListEl.innerHTML = "";
    if (!list.length) {
      recListEl.innerHTML =
        '<p class="muted" style="margin:0">SD 卡沒有錄影檔（或目前為 TFT 模式）</p>';
      return;
    }
    list.forEach((r) => {
      const item = document.createElement("div");
      item.className = "alarm-item";

      const info = document.createElement("div");
      info.className = "alarm-info";
      const timeEl = document.createElement("span");
      timeEl.className = "alarm-time";
      timeEl.textContent = recNameToTime(r.name);
      const metaEl = document.createElement("span");
      metaEl.className = "muted";
      metaEl.textContent = r.name + " · " + (r.size / 1048576).toFixed(1) + " MB";
      info.append(timeEl, metaEl);

      const btns = document.createElement("div");
      btns.style.cssText = "display:flex;gap:8px;margin-left:auto;flex:none";
      const playBtn = document.createElement("button");
      playBtn.textContent = "播放";
      playBtn.addEventListener("click", () => playRecording(r.name));
      const delBtn = document.createElement("button");
      delBtn.className = "danger";
      delBtn.textContent = "刪除";
      delBtn.addEventListener("click", async () => {
        if (!confirm("確定要刪除 " + r.name + "？")) return;
        try {
          const res2 = await fetch(
            "/rec/delete?name=" + encodeURIComponent(r.name),
            { method: "POST" },
          );
          const d = await res2.json();
          if (!d.ok) alert(d.error || "刪除失敗");
        } catch (e) {
          /* 重新整理列表會反映實際結果 */
        }
        loadRecordings();
      });
      btns.append(playBtn, delBtn);

      item.append(info, btns);
      recListEl.appendChild(item);
    });
  } catch (e) {
    recListEl.innerHTML =
      '<p class="muted" style="margin:0">讀取錄影清單失敗</p>';
  }
}

function stopPlayback() {
  playbackActive = false;
  if (playbackAbort) {
    playbackAbort.abort();
    playbackAbort = null;
  }
  recHud.textContent = "IDLE";
}

function findSeq(arr, seq, from) {
  outer: for (let i = from; i <= arr.length - seq.length; i++) {
    for (let j = 0; j < seq.length; j++) {
      if (arr[i + j] !== seq[j]) continue outer;
    }
    return i;
  }
  return -1;
}

// 串流下載 AVI 並掃描其中的 JPEG 幀 (FFD8FF ~ FFD9)，以 10fps 步調繪到畫布。
// 不解析 AVI 結構，MJPEG 每幀就是完整 JPEG，直接找標記最簡單可靠。
async function playRecording(name) {
  stopPlayback();
  playbackActive = true;
  playbackAbort = new AbortController();
  recStatus.textContent = "載入中：" + name;
  recHud.textContent = "PLAY 0.0s";
  recCtx.clearRect(0, 0, recCanvas.width, recCanvas.height);

  const SOI = [0xff, 0xd8, 0xff];
  const EOI = [0xff, 0xd9];
  let frames = 0;
  try {
    const res = await fetch("/rec/file?name=" + encodeURIComponent(name), {
      signal: playbackAbort.signal,
    });
    if (!res.ok) throw new Error("HTTP " + res.status);
    const reader = res.body.getReader();
    let buf = new Uint8Array(0);
    let next = performance.now();
    while (playbackActive) {
      const { done, value } = await reader.read();
      if (done) break;
      const nb = new Uint8Array(buf.length + value.length);
      nb.set(buf);
      nb.set(value, buf.length);
      buf = nb;

      for (;;) {
        const s = findSeq(buf, SOI, 0);
        if (s < 0) {
          if (buf.length > 2) buf = buf.slice(buf.length - 2); // 保留可能截斷的標記
          break;
        }
        const e = findSeq(buf, EOI, s + 3);
        if (e < 0) {
          if (s > 0) buf = buf.slice(s);
          break;
        }
        const jpg = buf.slice(s, e + 2);
        buf = buf.slice(e + 2);
        const bmp = await createImageBitmap(
          new Blob([jpg], { type: "image/jpeg" }),
        ).catch(() => null);
        if (bmp) {
          const wait = next - performance.now();
          if (wait > 0) await new Promise((r2) => setTimeout(r2, wait));
          next = Math.max(next + 100, performance.now() - 200); // 10fps
          drawRotated(recCtx, bmp, recCanvas.width, recCanvas.height);
          bmp.close();
          frames++;
          recHud.textContent = "PLAY " + (frames / 10).toFixed(1) + "s";
        }
        if (!playbackActive) break;
      }
    }
    if (playbackActive) {
      recStatus.textContent =
        "播放完畢：" + name + "（" + (frames / 10).toFixed(1) + " 秒）";
      recHud.textContent = "END";
    }
  } catch (e) {
    if (playbackActive) {
      recStatus.textContent = "播放失敗：" + name;
      recHud.textContent = "ERROR";
    }
  }
  playbackActive = false;
}

recStopBtn.addEventListener("click", () => {
  stopPlayback();
  recStatus.textContent = "已停止播放";
});
recRefreshBtn.addEventListener("click", loadRecordings);

// ================= OTA 更新 =================
const otaFile = document.getElementById("otaFile");
const otaTarget = document.getElementById("otaTarget");
const otaBtn = document.getElementById("otaBtn");
const otaBar = document.getElementById("otaBar");
const otaMsg = document.getElementById("otaMsg");

function setOtaMsg(cls, text) {
  otaMsg.className = "msg " + cls;
  otaMsg.textContent = text;
}

function doOtaUpload() {
  const file = otaFile.files[0];
  if (!file) {
    setOtaMsg("err", "請先選擇 .bin 檔");
    return;
  }
  if (!file.name.toLowerCase().endsWith(".bin")) {
    setOtaMsg("err", "檔案格式需為 .bin");
    return;
  }
  const target = otaTarget.value; // firmware | spiffs
  const targetName = target === "spiffs" ? "SPIFFS 網頁資料" : "韌體";
  if (
    !confirm(
      "確定要上傳「" +
        file.name +
        "」更新" +
        targetName +
        "？更新完成後裝置會重新啟動。",
    )
  )
    return;

  otaUploading = true; // 暫停影像輪詢，把頻寬與 WebServer 讓給上傳
  otaBtn.disabled = true;
  otaFile.disabled = true;
  otaTarget.disabled = true;
  otaBar.style.width = "0%";
  setOtaMsg("", "上傳中，請勿關閉電源...");

  // 用 XHR 而非 fetch，才能取得上傳進度
  const xhr = new XMLHttpRequest();
  xhr.open("POST", "/update?target=" + encodeURIComponent(target));
  xhr.timeout = 180000;

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      otaBar.style.width = ((e.loaded / e.total) * 100).toFixed(1) + "%";
    }
  };

  const done = (cls, text) => {
    setOtaMsg(cls, text);
    otaUploading = false;
    otaBtn.disabled = false;
    otaFile.disabled = false;
    otaTarget.disabled = false;
  };

  xhr.onload = () => {
    let data = null;
    try {
      data = JSON.parse(xhr.responseText);
    } catch (e) {
      /* 非 JSON 視為失敗 */
    }
    if (data && data.ok) {
      otaBar.style.width = "100%";
      setOtaMsg("ok", "更新成功，裝置重新啟動中，約 10 秒後自動重新整理...");
      setTimeout(() => location.reload(), 10000);
    } else {
      done(
        "err",
        "更新失敗：" + (data && data.error ? data.error : "HTTP " + xhr.status),
      );
    }
  };
  xhr.onerror = () => done("err", "上傳失敗，請確認與裝置的連線後重試");
  xhr.ontimeout = () => done("err", "上傳逾時，請重試");

  const fd = new FormData();
  fd.append("update", file, file.name);
  xhr.send(fd);
}

otaBtn.addEventListener("click", doOtaUpload);

// ================= 啟動 =================
// 把瀏覽器時間 + 時區同步給 ESP32 (AP 模式沒有 NTP)，告警紀錄/推播才有真實時間
function syncTime() {
  fetch("/time", {
    method: "POST",
    body: new URLSearchParams({
      epoch: Math.floor(Date.now() / 1000),
      tz: -new Date().getTimezoneOffset(), // 分鐘，UTC+8 = 480
    }),
  }).catch(() => {});
}
syncTime();
setInterval(syncTime, 10 * 60 * 1000);

refreshStatus();
setInterval(refreshStatus, 2000);
doScan();
loadNotifyConfig();
pollMotion();
frameLoop();
