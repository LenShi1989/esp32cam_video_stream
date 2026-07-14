// ================= 檢視切換 =================
const VIEW_TITLES = {
  console: "控制台",
  wifi: "Wi-Fi 設定",
  stream: "影像串流",
  motion: "移動偵測",
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
const ledBtn = document.getElementById("ledBtn");
const ledBtnText = document.getElementById("ledBtnText");

let ledState = false;
let ledSyncing = false;

function setLedUI(on) {
  ledState = on;
  ledBtn.classList.toggle("on", on);
  ledBtnText.textContent = on ? "ON" : "OFF";
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
        streamCtx.drawImage(bmp, 0, 0, streamCanvas.width, streamCanvas.height);
        const now = performance.now();
        frameTimes.push(now);
        frameTimes = frameTimes.filter((t) => now - t < 2000);
        streamHud.textContent = "FPS " + (frameTimes.length / 2).toFixed(1);
      }
      if (wantMotion) {
        motionCtx.drawImage(bmp, 0, 0, motionCanvas.width, motionCanvas.height);
        detectMotion(bmp);
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

// ================= 移動偵測 =================
const motionOverlay = document.getElementById("motionOverlay");
const overlayCtx = motionOverlay.getContext("2d");
const motionBadge = document.getElementById("motionBadge");
const motionHud = document.getElementById("motionHud");
const motionClearBtn = document.getElementById("motionClearBtn");
const sensRange = document.getElementById("sensRange");

const SAMPLE = 48; // 偵測區域縮到 48x48 再比較，降低運算量
const sampleCanvas = document.createElement("canvas");
sampleCanvas.width = SAMPLE;
sampleCanvas.height = SAMPLE;
const sampleCtx = sampleCanvas.getContext("2d", { willReadFrequently: true });

let region = null; // {x, y, w, h} 以 240x240 畫布座標表示
let selecting = null; // 拖曳中的起點 {x, y}
let prevGray = null; // 上一幀偵測區域的灰階資料
let triggeredUntil = 0; // 觸發後紅框保持到此時間戳

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
    region = { x, y, w, h };
    prevGray = null;
    triggeredUntil = 0;
  }
  drawOverlay();
});

motionClearBtn.addEventListener("click", () => {
  region = null;
  prevGray = null;
  triggeredUntil = 0;
  motionHud.textContent = "DIFF --%";
  drawOverlay();
});

function detectMotion(bmp) {
  if (!region) {
    motionBadge.className = "badge";
    motionBadge.textContent = "未設定區域";
    drawOverlay();
    return;
  }

  // 只取框選區域，縮到 SAMPLE x SAMPLE 後轉灰階與上一幀比較
  const sx = (region.x / motionCanvas.width) * bmp.width;
  const sy = (region.y / motionCanvas.height) * bmp.height;
  const sw = (region.w / motionCanvas.width) * bmp.width;
  const sh = (region.h / motionCanvas.height) * bmp.height;
  sampleCtx.drawImage(bmp, sx, sy, sw, sh, 0, 0, SAMPLE, SAMPLE);
  const img = sampleCtx.getImageData(0, 0, SAMPLE, SAMPLE).data;

  const gray = new Uint8Array(SAMPLE * SAMPLE);
  for (let i = 0; i < gray.length; i++) {
    const p = i * 4;
    gray[i] = (img[p] * 3 + img[p + 1] * 4 + img[p + 2]) >> 3;
  }

  if (prevGray) {
    let changed = 0;
    for (let i = 0; i < gray.length; i++) {
      if (Math.abs(gray[i] - prevGray[i]) > 28) changed++;
    }
    const ratio = (changed / gray.length) * 100;
    motionHud.textContent = "DIFF " + ratio.toFixed(1) + "%";
    // 靈敏度滑桿 1(最靈敏)~15：變動像素比例門檻 %
    const threshold = 16 - Number(sensRange.value);
    if (ratio > threshold) {
      triggeredUntil = performance.now() + 1500;
    }
  }
  prevGray = gray;

  const triggered = performance.now() < triggeredUntil;
  motionBadge.className = triggered ? "badge alert" : "badge normal";
  motionBadge.textContent = triggered ? "偵測到移動" : "監控中";
  drawOverlay();
}

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

  const triggered = performance.now() < triggeredUntil;
  const color = triggered ? "#ff3860" : "#2bff88";
  overlayCtx.strokeStyle = color;
  overlayCtx.shadowColor = color;
  overlayCtx.shadowBlur = 8;
  overlayCtx.lineWidth = 2;
  overlayCtx.strokeRect(region.x, region.y, region.w, region.h);
  overlayCtx.shadowBlur = 0;

  // 四角加強線，做出瞄準框效果
  const L = Math.min(14, region.w / 3, region.h / 3);
  overlayCtx.lineWidth = 4;
  const { x, y, w, h } = region;
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
refreshStatus();
setInterval(refreshStatus, 2000);
doScan();
frameLoop();
