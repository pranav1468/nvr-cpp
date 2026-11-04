/**
 * MACTRA NVR — Lightweight Control Room Console
 * High-performance, zero-framework, dependency-free UI controller.
 * Connects directly to the NVR C++ REST Engine (http://localhost:8080/api/v1).
 */

// --- Authentication & Session State ---
let authToken = "";
let currentUser = { username: "admin", role: "ADMIN" };

// --- Global State ---
const MAX_CHANNELS = 8;

let cameras = [];
const liveStreamEngines = new Map(); // channelId -> NvrStreamEngine
let telemetry = {
  status: "CONNECTING",
  uptime_seconds: 0,
  cpu_load_percent: 0.0,
  ram_free_mb: 0,
  storage_free_mb: 0,
  storage_total_mb: 0,
  bitrate_mbps: 0.0,
};

let currentTab = "live";
let liveLayout = 4; // 1, 4, '7plus1', 8
let prevLayout = 4;
let selectedCamId = 1;
let fullscreenCh = null;
let liveFilter = "all";
let liveSearch = "";
let paneCollapsed = false;
let sidebarCollapsed = false;

// Playback State
let pbSegments = null;
let pbReviewItems = [];
let pbActive = null;
let pbPlaying = false;
let pbRate = 1;
let pbPlayhead = 0;
let pbTimer = null;
let pbAutoAdvance = false;
let pbClipDuration = 60;
let pbLoop = false;
let pbLayoutMode = "single";
let pbViewMode = "cards";
let pbIsDragging = false;
let pbSeekDragging = false;
let pbHls = null;
let pbAudioMuted = false;

// Wizard State
let wizardStep = 1;
let wizardDevices = [];
let wizardSelectedIp = null;
let wizardProbed = null;
let wizardScanning = false;

// Pending delete channel
let deleteTargetCh = null;

// --- Helper Functions ---
function pad2(n) {
  return String(n).padStart(2, "0");
}

function chLabel(id) {
  return `CH ${pad2(id)}`;
}

function formatBytes(bytes) {
  if (!bytes) return "0 KB";
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(1)} GB`;
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(1)} MB`;
  return `${Math.max(1, Math.round(bytes / 1e3))} KB`;
}

function formatUptime(seconds) {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return `${h}h ${m}m`;
}

function formatIsoDate(d8) {
  if (typeof d8 === "string" && d8.length === 8 && !d8.includes("-")) {
    return `${d8.slice(0, 4)}-${d8.slice(4, 6)}-${d8.slice(6, 8)}`;
  }
  return d8;
}

function formatDurationCompact(seconds) {
  const s = Math.max(0, Math.floor(seconds || 0));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${pad2(sec)}s`;
  return `${sec}s`;
}

function formatTimeMSS(seconds, forceHours = false) {
  const s = Math.max(0, Math.floor(seconds || 0));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0 || forceHours) {
    return `${pad2(h)}:${pad2(m)}:${pad2(sec)}`;
  }
  return `${pad2(m)}:${pad2(sec)}`;
}

function getEpochMs(isoOrMs) {
  if (typeof isoOrMs === "number" && !isNaN(isoOrMs)) return isoOrMs;
  if (!isoOrMs) return 0;
  const t = new Date(isoOrMs).getTime();
  return isNaN(t) ? 0 : t;
}

function getSnapshotUrl(key, id) {
  const ch = Number(id || (key ? key.replace(/\D/g, "") : 1)) || 1;
  return `/api/v1/cameras/${ch}/snapshot`;
}

function isCamOnline(cam) {
  return cam && (cam.status === "ONLINE" || cam.online === true);
}

// --- Toast System ---
function showToast(message, type = "info") {
  const container = document.getElementById("toast-container");
  if (!container) return;

  const toast = document.createElement("div");
  toast.className = `toast-msg toast-${type}`;

  let iconSvg = "";
  if (type === "success") {
    iconSvg = `<svg class="h-4 w-4 text-success shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M5 13l4 4L19 7"/></svg>`;
  } else if (type === "error") {
    iconSvg = `<svg class="h-4 w-4 text-destructive shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12"/></svg>`;
  } else if (type === "warning") {
    iconSvg = `<svg class="h-4 w-4 text-warning shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/></svg>`;
  } else {
    iconSvg = `<svg class="h-4 w-4 text-primary shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>`;
  }

  toast.innerHTML = `${iconSvg}<span class="truncate">${message}</span>`;
  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = "0";
    toast.style.transform = "translateY(8px) scale(0.96)";
    setTimeout(() => toast.remove(), 200);
  }, 3200);
}

// --- Navigation & Routing ---
function switchTab(tabId) {
  if (currentTab === "live" && tabId !== "live") {
    destroyAllLiveStreams();
  }
  currentTab = tabId;

  // Toggle view elements
  document.querySelectorAll(".app-view").forEach((view) => {
    if (view.id === `view-${tabId}`) {
      view.classList.remove("hidden");
    } else {
      view.classList.add("hidden");
    }
  });

  // Toggle nav styles
  document.querySelectorAll(".nav-item").forEach((btn) => {
    if (btn.dataset.nav === tabId) {
      btn.className = "nav-item group flex items-center gap-3 rounded-xl px-3 py-2.5 text-sm font-medium transition-colors bg-primary text-primary-foreground shadow-sm";
    } else {
      btn.className = "nav-item group flex items-center gap-3 rounded-xl px-3 py-2.5 text-sm font-medium transition-colors text-muted-foreground hover:bg-accent hover:text-accent-foreground";
    }
  });

  // Re-render corresponding view content
  if (tabId === "live") renderLiveMatrix();
  if (tabId === "playback") initPlaybackView();
  if (tabId === "cameras") renderCamerasView();
  if (tabId === "users") renderUsersView();
  if (tabId === "dashboard") renderDashboardView();
  if (tabId === "system") renderSystemView();
}

function handleHashChange() {
  const hash = window.location.hash.replace("#", "") || "live";
  if (["live", "playback", "cameras", "users", "dashboard", "system"].includes(hash)) {
    switchTab(hash);
  } else {
    switchTab("live");
  }
}

// --- Telemetry & Clock ---
function startClock() {
  const clockEl = document.getElementById("header-clock");
  function tick() {
    const d = new Date();
    const str = `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
    if (clockEl) clockEl.textContent = str;
  }
  tick();
  setInterval(tick, 1000);
}

async function fetchTelemetry() {
  try {
    const res = await fetch("/api/v1/system/status");
    if (res.ok) {
      const json = await res.json();
      const d = json.data || json;
      const totalMb = d.total_storage_bytes ? Math.round(d.total_storage_bytes / (1024 * 1024)) : 0;
      const freeMb = d.free_storage_bytes ? Math.round(d.free_storage_bytes / (1024 * 1024)) : 0;
      const ramFreeMb = d.ram_free_bytes ? Math.round(d.ram_free_bytes / (1024 * 1024)) : 0;
      const cores = typeof d.cpu_cores === "number" && d.cpu_cores > 0 ? d.cpu_cores : (navigator.hardwareConcurrency || 12);
      const cpuPct = typeof d.cpu_percent === "number"
        ? Math.min(100, Math.max(0, d.cpu_percent))
        : (typeof d.cpu_load_1m === "number" ? Math.min(100, (d.cpu_load_1m / cores) * 100) : 0);

      telemetry = {
        status: d.status || "ONLINE",
        uptime_seconds: Number(d.uptime_seconds || 0),
        cpu_load_percent: Number(cpuPct.toFixed(1)),
        ram_free_mb: ramFreeMb,
        storage_free_mb: freeMb,
        storage_total_mb: totalMb,
        bitrate_mbps: Number(d.bitrate_mbps || 0),
      };
      updateTelemetryUI();
      return;
    }
  } catch {}
}

function updateTelemetryUI() {
  const statusEl = document.getElementById("header-status");
  const cpuEl = document.getElementById("header-cpu");
  const ramEl = document.getElementById("header-ram");
  const storageEl = document.getElementById("header-storage");

  if (statusEl) statusEl.textContent = telemetry.status;
  if (cpuEl) {
    cpuEl.textContent = `${telemetry.cpu_load_percent.toFixed(1)}%`;
    if (telemetry.cpu_load_percent > 50) {
      cpuEl.classList.add("text-destructive");
      cpuEl.classList.remove("text-foreground");
    } else {
      cpuEl.classList.remove("text-destructive");
      cpuEl.classList.add("text-foreground");
    }
  }
  if (ramEl) ramEl.textContent = `${telemetry.ram_free_mb} MB`;
  if (storageEl) storageEl.textContent = `${(telemetry.storage_free_mb / 1024).toFixed(1)} GB`;

  // Update dashboard stat cards if currently visible
  if (currentTab === "dashboard") {
    renderDashboardView();
  }
}

// --- Cameras Data Management ---
async function loadCameras() {
  try {
    const res = await fetch("/api/v1/cameras");
    if (res.ok) {
      const json = await res.json();
      const list = Array.isArray(json) ? json : (json.data ?? null);
      if (Array.isArray(list)) {
        cameras = list.map((c) => {
          const id = Number(c.id);
          const rtspMain = c.rtsp_url || c.rtsp_url_main || null;
          const rtspSub = c.sub_rtsp_url || c.rtsp_url_sub || null;
          let ip = c.ip_address || "";
          if (!ip && rtspMain) {
            const m = rtspMain.match(/@?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)/);
            if (m) ip = m[1];
          }
          if (!ip) ip = `192.168.1.${100 + id}`;

          return {
            id,
            name: c.name || `Camera ${id}`,
            ip_address: ip,
            rtsp_url: rtspMain,
            rtsp_url_main: rtspMain,
            sub_rtsp_url: rtspSub,
            rtsp_url_sub: rtspSub,
            username: c.username || c.onvif_username || "admin",
            onvif_username: c.onvif_username || c.username || "admin",
            resolution: c.resolution || "1080p",
            fps: Number(c.fps || 25),
            bitrate_kbps: c.bitrate_kbps ? Number(c.bitrate_kbps) : 4096,
            codec: (c.codec || "h264").toLowerCase(),
            status: (c.online !== undefined ? (c.online ? "ONLINE" : "OFFLINE") : c.status) || "ONLINE",
            enabled: c.enabled !== undefined ? Boolean(c.enabled) : true,
            recording: c.recording !== undefined ? Boolean(c.recording) : true,
            location: c.location || "",
            snapshot_key: `cam${((id - 1) % 8) + 1}`,
          };
        });
        if (cameras.length > 4 && liveLayout === 4) {
          liveLayout = 8;
        }
        localStorage.setItem("nvr_cameras", JSON.stringify(cameras));
        return;
      }
    }
  } catch {}

  // Fallback to local storage only if network fetch threw an error (offline standalone preview)
  try {
    const local = localStorage.getItem("nvr_cameras");
    if (local) {
      cameras = JSON.parse(local);
      if (cameras.length > 4 && liveLayout === 4) {
        liveLayout = 8;
      }
      return;
    }
  } catch {}
  cameras = [];
}

// --- Zero-Transcode Multi-Tier Stream Engine (WebCodecs → MSE fMP4 → Wasm → Standby) ---
class NvrStreamEngine {
  constructor(config) {
    this.channelId = config.channelId;
    this.container = config.container;
    this.authToken = config.authToken || authToken || "";
    this.streamType = config.streamType || "sub"; // 'main' or 'sub'
    this.onStatus = config.onStatus || (() => {});
    this.onError = config.onError || (() => {});

    this.ws = null;
    this.video = null;
    this.canvas = null;
    this.ctx = null;
    this.decoder = null;
    this.wasmWorker = null;
    this.mseSource = null;
    this.mseBuffer = null;
    this.mseAbortCtrl = null;
    this.reconnectTimer = null;

    this.activeTier = "NONE";
    this.webcodecsMode = "annexb";
    this.awaitingKeyChunk = true;
    this.isDestroyed = false;
    this.detectedWidth = 0;
    this.detectedHeight = 0;

    this.initDOM();
  }

  updateDynamicResolutionBadge(w, h) {
    if (!h || h <= 0) return;
    this.detectedWidth = w;
    this.detectedHeight = h;
    const isMain = (this.streamType === "main");
    const prefix = isMain ? "MAIN" : "SUB";
    let resText = `${h}p`;
    if (h >= 2160) resText = "4K";
    else if (h >= 1440) resText = "2K";
    const badgeEl = document.getElementById(`res-badge-${this.channelId}`);
    if (badgeEl) {
      badgeEl.textContent = `${prefix} ${resText}`;
    }
  }

  initDOM() {
    let stage = this.container.querySelector(".nvr-media-stage-inner");
    if (!stage) {
      stage = document.createElement("div");
      stage.className = "nvr-media-stage-inner";
      Object.assign(stage.style, {
        width: "100%",
        height: "100%",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        backgroundColor: "#000",
        position: "absolute",
        inset: "0",
        overflow: "hidden",
      });
      this.container.appendChild(stage);
    }

    let vid = stage.querySelector("video");
    if (!vid) {
      vid = document.createElement("video");
      vid.autoplay = true;
      vid.muted = true;
      vid.playsInline = true;
      Object.assign(vid.style, { width: "100%", height: "100%", objectFit: "cover", display: "none" });
      stage.appendChild(vid);
    }
    this.video = vid;

    let cvs = stage.querySelector("canvas");
    if (!cvs) {
      cvs = document.createElement("canvas");
      Object.assign(cvs.style, { width: "100%", height: "100%", objectFit: "cover", display: "none" });
      stage.appendChild(cvs);
    }
    this.canvas = cvs;
    this.ctx = null;
  }

  static base64ToUint8Array(b64) {
    if (!b64) return new Uint8Array(0);
    const bin = atob(b64);
    const arr = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; ++i) arr[i] = bin.charCodeAt(i);
    return arr;
  }

  static annexBToLengthPrefixed(annexb, isH265) {
    const starts = [];
    const len = annexb.byteLength;
    let i = 0;
    while (i + 3 <= len) {
      if (annexb[i] === 0 && annexb[i + 1] === 0) {
        if (annexb[i + 2] === 1) { starts.push({ scPos: i, nalStart: i + 3 }); i += 3; continue; }
        if (i + 3 < len && annexb[i + 2] === 0 && annexb[i + 3] === 1) { starts.push({ scPos: i, nalStart: i + 4 }); i += 4; continue; }
      }
      ++i;
    }
    const keptNals = [];
    let totalBytes = 0;
    for (let idx = 0; idx < starts.length; ++idx) {
      const start = starts[idx].nalStart;
      let end = idx + 1 < starts.length ? starts[idx + 1].scPos : len;
      while (end > start && annexb[end - 1] === 0) --end;
      if (end <= start) continue;
      const nalHdr = annexb[start];
      const nalType = isH265 ? ((nalHdr >> 1) & 0x3f) : (nalHdr & 0x1f);
      if (isH265 && (nalType === 32 || nalType === 33 || nalType === 34 || nalType === 35)) continue;
      if (!isH265 && (nalType === 7 || nalType === 8 || nalType === 9)) continue;
      const sub = annexb.subarray(start, end);
      keptNals.push(sub);
      totalBytes += 4 + sub.byteLength;
    }
    const out = new Uint8Array(totalBytes);
    const view = new DataView(out.buffer);
    let offset = 0;
    for (const nal of keptNals) {
      view.setUint32(offset, nal.byteLength, false);
      out.set(nal, offset + 4);
      offset += 4 + nal.byteLength;
    }
    return out;
  }

  async start() {
    if (this.isDestroyed) return;
    this.cleanupTransport();
    this.onStatus({ state: "PROBING", tier: "Connecting…", text: "Connecting stream socket…" });

    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const port = window.location.port === "8088" ? "8080" : (window.location.port || (protocol === "https:" ? "443" : "80"));
    let wsUrl = `${protocol}//${window.location.hostname}:${port}/api/v1/ws/stream?channel=${this.channelId}&stream=${this.streamType}`;
    if (this.authToken) wsUrl += `&token=${encodeURIComponent(this.authToken)}`;

    try {
      this.ws = new WebSocket(wsUrl);
      this.ws.binaryType = "arraybuffer";

      this.ws.onopen = () => {
        if (this.isDestroyed) return;
        this.onStatus({ state: "CONNECTED", tier: "Awaiting keyframe", text: "Awaiting stream parameter sets…" });
      };

      this.ws.onmessage = async (evt) => {
        if (this.isDestroyed) return;
        if (typeof evt.data === "string") {
          try {
            const msg = JSON.parse(evt.data);
            if (msg.type === "config") {
              console.log(`[Cam ${this.channelId}] Received config:`, msg.codec, msg.codec_string);
              if (msg.height && msg.height > 0) {
                this.updateDynamicResolutionBadge(msg.width, msg.height);
              }
              this.isNegotiating = true;
              this.pendingInitFrames = [];
              try {
                await this.negotiatePlayer(msg);
              } finally {
                this.isNegotiating = false;
              }
              if (!this.isDestroyed && this.pendingInitFrames?.length) {
                const queued = this.pendingInitFrames;
                this.pendingInitFrames = [];
                for (const f of queued) {
                  this.dispatchFrame(f.nalData, f.ptsUs, f.isKey, f.isH265);
                }
              }
            }
          } catch (e) {
            console.error(`[Cam ${this.channelId}] config error:`, e);
          }
          return;
        }
        const buf = evt.data;
        if (!buf || buf.byteLength <= 12) return;
        const dv = new DataView(buf);
        const ptsUs = Number(dv.getBigUint64(0, true));
        const flags = dv.getUint32(8, true);
        const nalData = new Uint8Array(buf, 12);
        const isKey = (flags & 0x01) !== 0;
        const isH265 = (flags & 0x02) !== 0;
        if (this.isNegotiating) {
          if (this.pendingInitFrames && this.pendingInitFrames.length < 30) {
            this.pendingInitFrames.push({ nalData: nalData.slice(), ptsUs, isKey, isH265 });
          }
          return;
        }
        this.dispatchFrame(nalData, ptsUs, isKey, isH265);
      };

      this.ws.onerror = () => {
        if (!this.isDestroyed && this.activeTier === "NONE") this.setOfflineStatus("Stream offline • No signal");
      };

      this.ws.onclose = () => {
        if (!this.isDestroyed && this.activeTier !== "MSE") {
          if (this.activeTier === "NONE") this.setOfflineStatus("Reconnecting stream…");
          this.reconnectTimer = setTimeout(() => this.start(), 1000);
        }
      };
    } catch (e) {
      this.onError(e);
      this.setOfflineStatus("Connection error • No signal");
    }
  }

  async negotiatePlayer(cfg) {
    const isH265 = cfg.codec === "H265";
    const descBytes = NvrStreamEngine.base64ToUint8Array(cfg.description_b64);

    const candidateCodecs = isH265
      ? [cfg.hvc1_codec_string, cfg.codec_string, "hvc1.1.6.L93.B0", "hev1.1.6.L93.B0", "hvc1.1.6.L120.B0", "hev1.1.6.L120.B0"].filter(Boolean)
      : [cfg.codec_string, "avc1.64002a", "avc1.4d401f", "avc1.42e01e"].filter(Boolean);

    // Tier 1: WebCodecs
    if (typeof VideoDecoder !== "undefined") {
      if (descBytes.byteLength > 0) {
        for (const codecStr of candidateCodecs) {
          for (const accel of ["prefer-hardware", "no-preference"]) {
            try {
              const check = await VideoDecoder.isConfigSupported({
                codec: codecStr,
                codedWidth: cfg.width || 640,
                codedHeight: cfg.height || 360,
                description: descBytes,
                optimizeForLatency: true,
                hardwareAcceleration: accel,
              });
              if (check?.supported) {
                this.webcodecsMode = "avcc";
                this.setupWebCodecs(check.config);
                this.activateSurface("canvas");
                this.activeTier = "WEBCODECS";
                this.onStatus({ state: "PLAYING", tier: `WebCodecs / ${cfg.codec}`, codec: cfg.codec, text: `WebCodecs (${cfg.width}×${cfg.height})` });
                return;
              }
            } catch {}
          }
        }
      }
      for (const codecStr of candidateCodecs) {
        for (const accel of ["prefer-hardware", "no-preference"]) {
          try {
            const check = await VideoDecoder.isConfigSupported({
              codec: codecStr,
              codedWidth: cfg.width || 640,
              codedHeight: cfg.height || 360,
              optimizeForLatency: true,
              hardwareAcceleration: accel,
            });
            if (check?.supported) {
              this.webcodecsMode = "annexb";
              this.setupWebCodecs(check.config);
              this.activateSurface("canvas");
              this.activeTier = "WEBCODECS";
              this.onStatus({ state: "PLAYING", tier: `WebCodecs / ${cfg.codec}`, codec: cfg.codec, text: `WebCodecs Annex-B (${cfg.width}×${cfg.height})` });
              return;
            }
          } catch {}
        }
      }
    }

    // Tier 2: MSE (fMP4) - strictly for H.264 streams.
    // H.265 in browser MSE suffers from B-frame reference stalls and macroblock ghosting
    // because standard IP camera RTSP streams lack client-side composition time reordering.
    if (!isH265 && typeof MediaSource !== "undefined") {
      const mime = candidateCodecs.map((c) => `video/mp4; codecs="${c}"`).find((m) => MediaSource.isTypeSupported(m));
      if (mime) {
        if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
        this.activeTier = "MSE";
        this.activateSurface("video");
        this.setupMSE(mime, cfg.codec);
        return;
      }
    }

    // Tier 3: Wasm fallback
    if (isH265 && (await this.setupWasmDecoder(cfg))) {
      this.activeTier = "WASM";
      this.activateSurface("canvas");
      this.onStatus({ state: "PLAYING", tier: "Wasm / H.265", codec: "H.265", text: "Client Wasm software decode (0% NVR CPU)" });
      return;
    }

    // Tier 4: Offline / Standby (NO SNAPSHOT FALLBACK)
    if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
    this.setOfflineStatus(`Stream standby (${cfg.codec})`);
  }

  activateSurface(type) {
    if (this.video) this.video.style.display = type === "video" ? "block" : "none";
    if (this.canvas) this.canvas.style.display = type === "canvas" ? "block" : "none";
  }

  setupWebCodecs(decoderConfig) {
    if (this.decoder) { try { this.decoder.close(); } catch {} }
    if (this.canvas && this.canvas.parentNode) {
      try {
        if (!this.ctx) {
          this.ctx = this.canvas.getContext("2d", { alpha: false, desynchronized: true });
        }
      } catch {
        const freshCanvas = document.createElement("canvas");
        freshCanvas.className = this.canvas.className;
        Object.assign(freshCanvas.style, {
          width: "100%",
          height: "100%",
          objectFit: "cover",
          display: this.canvas.style.display || "none"
        });
        this.canvas.parentNode.replaceChild(freshCanvas, this.canvas);
        this.canvas = freshCanvas;
        this.ctx = this.canvas.getContext("2d", { alpha: false, desynchronized: true });
      }
    }
    this.awaitingKeyChunk = true;
    this.decoder = new VideoDecoder({
      output: (frame) => {
        if (this.isDestroyed) { frame.close(); return; }
        if (this.canvas && (this.canvas.width !== frame.displayWidth || this.canvas.height !== frame.displayHeight)) {
          this.canvas.width = frame.displayWidth;
          this.canvas.height = frame.displayHeight;
        }
        if (frame.displayHeight > 0 && this.detectedHeight !== frame.displayHeight) {
          this.updateDynamicResolutionBadge(frame.displayWidth, frame.displayHeight);
        }
        this.ctx?.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
        frame.close();
      },
      error: () => { this.awaitingKeyChunk = true; },
    });
    this.decoder.configure(decoderConfig);
  }

  setupMSE(mimeType, codecLabel) {
    this.onStatus({ state: "PLAYING", tier: `fMP4 / ${codecLabel}`, codec: codecLabel, text: `MSE pass-through (${codecLabel})` });
    const ms = new MediaSource();
    this.mseSource = ms;
    this.video.src = URL.createObjectURL(ms);
    ms.addEventListener("sourceopen", () => {
      if (this.isDestroyed || this.activeTier !== "MSE") return;
      let sb = null;
      const queue = [];
      try {
        sb = ms.addSourceBuffer(mimeType);
        this.mseBuffer = sb;
      } catch {
        this.setOfflineStatus("MSE error • No signal");
        return;
      }
      const flush = () => {
        if (sb && queue.length > 0 && !sb.updating) {
          try { sb.appendBuffer(queue.shift()); } catch {}
        }
      };
      sb.addEventListener("updateend", () => {
        flush();
        if (this.video.paused) this.video.play().catch(() => {});
        const v = this.video;
        if (v.buffered.length > 0) {
          const live = v.buffered.end(v.buffered.length - 1);
          if (live - v.currentTime > 1.5) v.currentTime = live - 0.1;
          const bs = v.buffered.start(0);
          if (v.currentTime - bs > 10 && sb && !sb.updating) {
            try { sb.remove(bs, v.currentTime - 5); } catch {}
          }
        }
      });
      const port = window.location.port === "8088" ? "8080" : (window.location.port || (window.location.protocol === "https:" ? "443" : "80"));
      let url = `${window.location.protocol}//${window.location.hostname}:${port}/api/v1/stream?channel=${this.channelId}&stream=${this.streamType}`;
      if (this.authToken) url += `&token=${encodeURIComponent(this.authToken)}`;
      this.mseAbortCtrl = new AbortController();
      fetch(url, { signal: this.mseAbortCtrl.signal })
        .then((res) => {
          if (!res.ok) throw new Error(`HTTP ${res.status}`);
          const r = res.body.getReader();
          const pump = () =>
            r.read().then(({ done, value }) => {
              if (done || this.isDestroyed) return;
              queue.push(value);
              flush();
              pump();
            }).catch(() => {
              if (!this.isDestroyed) this.setOfflineStatus("Stream ended • No signal");
            });
          pump();
        })
        .catch(() => {
          if (!this.isDestroyed) this.setOfflineStatus("Stream offline • No signal");
        });
    });
  }

  async setupWasmDecoder(cfg) {
    try {
      if (!this.canvas) return false;
      const { HevcPlayer } = await import("/wasm/hevc-wasm-player.bundle.js");
      if (this.isDestroyed) return false;
      if (this.wasmPlayer) {
        try { this.wasmPlayer.close(); } catch {}
        this.wasmPlayer = null;
      }

      // Ensure canvas has no prior rendering context so transferControlToOffscreen succeeds
      if (this.canvas && this.canvas.parentNode) {
        const freshCanvas = document.createElement("canvas");
        freshCanvas.className = this.canvas.className;
        Object.assign(freshCanvas.style, {
          width: "100%",
          height: "100%",
          objectFit: "cover",
          display: this.canvas.style.display || "none"
        });
        this.canvas.parentNode.replaceChild(freshCanvas, this.canvas);
        this.canvas = freshCanvas;
        this.ctx = null;
      }

      const canUseThreads = typeof window !== "undefined" &&
        Boolean(window.crossOriginIsolated) &&
        typeof SharedArrayBuffer !== "undefined";

      const hardwareThreads = navigator.hardwareConcurrency || 4;
      const targetThreads = canUseThreads ? Math.min(2, Math.max(1, Math.floor(hardwareThreads / 4))) : 1;
      const chosenVariant = canUseThreads ? "threads-simd" : "auto";

      this.awaitingKeyChunk = true;
      this.wasmPlayer = new HevcPlayer({
        canvas: this.canvas,
        mode: "wasm",
        wasmBaseUrl: "/wasm/",
        workerUrl: "/wasm/wasm-worker.bundle.js",
        wasmWorker: "auto",
        wasmVariant: chosenVariant,
        threads: targetThreads,
        renderer: "auto",
        codec: cfg?.codec_string || "hev1.1.6.L90",
        workerFrameOutput: "planes",
        lowLatency: {
          enabled: true,
          maxInputFrames: 120,
          maxInputBytes: 32 * 1024 * 1024,
          maxDecodeQueue: 30,
          maxQueueAgeMs: 3000,
          dropPolicy: "until-key"
        }
      });

      this.wasmPlayer.on("keyframerequest", () => {
        this.awaitingKeyChunk = true;
      });

      await this.wasmPlayer.init();
      return true;
    } catch (e) {
      console.warn(`[Cam ${this.channelId}] WASM initialization failed:`, e);
      return false;
    }
  }

  setOfflineStatus(statusText) {
    this.activeTier = "NONE";
    this.activateSurface("none");
    this.onStatus({ state: "FALLBACK", tier: "No signal", text: statusText });
  }

  dispatchFrame(nalData, ptsUs, isKeyframe, isH265) {
    if (this.activeTier === "WEBCODECS" && this.decoder?.state === "configured") {
      if (this.awaitingKeyChunk) {
        if (!isKeyframe) return;
        this.awaitingKeyChunk = false;
      }
      if (this.decoder.decodeQueueSize > 8 && !isKeyframe) {
        this.awaitingKeyChunk = true;
        return;
      }
      try {
        const chunk = this.webcodecsMode === "avcc" ? NvrStreamEngine.annexBToLengthPrefixed(nalData, isH265) : nalData;
        if (chunk.byteLength === 0) return;
        this.decoder.decode(new EncodedVideoChunk({ type: isKeyframe ? "key" : "delta", timestamp: ptsUs, data: chunk }));
      } catch {
        this.awaitingKeyChunk = true;
      }
    } else if (this.activeTier === "WASM" && this.wasmPlayer) {
      if (this.awaitingKeyChunk) {
        if (!isKeyframe) return;
        this.awaitingKeyChunk = false;
      }
      try {
        this.wasmPlayer.pushAccessUnit(nalData, {
          timestamp: ptsUs,
          key: isKeyframe
        });
      } catch (err) {
        console.warn(`[Cam ${this.channelId}] WASM frame dispatch error:`, err);
        this.awaitingKeyChunk = true;
      }
    }
  }

  cleanupTransport() {
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    if (this.mseAbortCtrl) { try { this.mseAbortCtrl.abort(); } catch {} this.mseAbortCtrl = null; }
    if (this.ws) { this.ws.onclose = null; this.ws.onerror = null; this.ws.onmessage = null; try { this.ws.close(); } catch {} this.ws = null; }
    if (this.decoder) { try { this.decoder.close(); } catch {} this.decoder = null; }
    if (this.wasmPlayer) { try { this.wasmPlayer.close(); } catch {} this.wasmPlayer = null; }
    if (this.wasmWorker) { this.wasmWorker.terminate(); this.wasmWorker = null; }
  }

  destroy() {
    this.isDestroyed = true;
    this.cleanupTransport();
  }
}

function destroyAllLiveStreams() {
  for (const [ch, engine] of liveStreamEngines.entries()) {
    try { engine.destroy(); } catch {}
  }
  liveStreamEngines.clear();
}

// --- View 1: Live Matrix ---
// --- View 1: Live Matrix (Persistent, Zero-Flicker Architecture) ---
function ensurePersistentTiles(grid) {
  if (grid.dataset.initialized === "true" && grid.children.length === 8) {
    return;
  }
  grid.innerHTML = Array.from({ length: 8 }, (_, i) => i + 1)
    .map((ch) => {
      const cam = cameras.find((c) => c.id === ch);
      const online = cam ? isCamOnline(cam) : false;
      return `
        <div data-ch="${ch}" class="tile-stage-bg group relative min-h-[120px] overflow-hidden rounded-xl border border-border transition-colors cursor-pointer">
          <div id="stream-stage-${ch}" class="nvr-media-stage absolute inset-0 bg-black"></div>
          <div id="no-signal-${ch}" class="absolute inset-0 flex flex-col items-center justify-center gap-1.5 text-white/50 bg-[#090d16] ${cam && online ? "hidden" : ""}">
            <svg class="h-7 w-7" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
            <span class="text-[11px] font-semibold tracking-wide">${cam ? "NO SIGNAL" : "NO CAMERA ON THIS CHANNEL"}</span>
            <a href="#cameras" class="text-[10px] font-semibold text-primary-foreground/80 underline decoration-dotted hover:text-white">Assign a camera</a>
          </div>

          <!-- Top HUD Overlay -->
          <div class="absolute inset-x-0 top-0 flex items-center justify-between gap-1 bg-gradient-to-b from-black/70 to-transparent px-2 py-1.5 z-10 pointer-events-none">
            <span class="flex min-w-0 items-center gap-1.5">
              <span id="dot-${ch}" class="h-[7px] w-[7px] shrink-0 rounded-full ${online ? "bg-success soft-pulse" : "bg-white/30"}"></span>
              <span id="label-${ch}" class="truncate font-mono text-[10.5px] font-semibold text-white drop-shadow">
                ${chLabel(ch)}: ${cam ? cam.name : "Empty"}
              </span>
            </span>
            <span class="flex shrink-0 items-center gap-1">
              <span id="res-badge-${ch}" class="inline-flex items-center rounded-md border border-transparent bg-black/55 px-2 py-0.5 font-mono text-[9px] text-white">
                SUB
              </span>
              <span id="rec-badge-${ch}" class="inline-flex items-center rounded-md border border-transparent bg-destructive px-2 py-0.5 font-mono text-[9px] font-bold text-white ${cam && cam.recording ? "" : "hidden"}">
                <span class="rec-dot mr-1 inline-block h-1.5 w-1.5 rounded-full bg-white"></span>
                REC
              </span>
            </span>
          </div>

          <!-- Bottom Right Hover Controls -->
          <div class="absolute bottom-0 right-0 flex items-center gap-1 p-1.5 opacity-0 transition-opacity group-hover:opacity-100 z-10">
            <button type="button" data-expand-ch="${ch}" title="Fullscreen view" class="flex h-7 w-7 items-center justify-center rounded-md bg-black/55 text-white backdrop-blur transition-colors hover:bg-primary">
              <svg class="h-3.5 w-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><polyline points="15 3 21 3 21 9"/><polyline points="9 21 3 21 3 15"/><line x1="21" y1="3" x2="14" y2="10"/><line x1="3" y1="21" x2="10" y2="14"/></svg>
            </button>
          </div>

          <!-- Bottom Center Stream Tier Status -->
          <div class="absolute bottom-0 left-1/2 hidden -translate-x-1/2 items-center gap-2 sm:flex z-10 pointer-events-none">
            <span id="stream-tier-${ch}" class="px-2 py-0.5 font-mono text-[10px] text-white/90 bg-black/55">
              ${cam && online ? "Connecting…" : "Standby"}
            </span>
          </div>
        </div>
      `;
    })
    .join("");

  grid.dataset.initialized = "true";

  // Attach Tile Listeners once
  grid.querySelectorAll("[data-ch]").forEach((tile) => {
    const ch = Number(tile.dataset.ch);
    tile.addEventListener("click", () => {
      selectCamera(ch);
    });
    tile.addEventListener("dblclick", () => {
      if (fullscreenCh === ch) {
        exitFullscreenMode();
      } else {
        enterFullscreenMode(ch);
      }
    });
  });

  grid.querySelectorAll("[data-expand-ch]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const ch = Number(btn.dataset.expandCh);
      if (fullscreenCh === ch) {
        exitFullscreenMode();
      } else {
        enterFullscreenMode(ch);
      }
    });
  });
}

function renderLiveMatrix() {
  renderLiveCameraDir();

  const grid = document.getElementById("live-matrix-grid");
  if (!grid) return;

  ensurePersistentTiles(grid);

  grid.className = `grid h-[calc(100vh-13rem)] min-h-[420px] gap-2 ${fullscreenCh ? "matrix-1" : `matrix-${liveLayout}`}`;

  const tileChannels = fullscreenCh
    ? [fullscreenCh]
    : liveLayout === "7plus1"
      ? Array.from({ length: 8 }, (_, i) => i + 1)
      : Array.from({ length: liveLayout }, (_, i) => i + 1);

  // Manage visibility and streaming for all 8 channels without tearing down active DOM or Sockets
  for (let ch = 1; ch <= 8; ch++) {
    const tile = grid.querySelector(`[data-ch="${ch}"]`);
    if (!tile) continue;

    const isVisible = tileChannels.includes(ch);
    if (!isVisible) {
      tile.classList.add("hidden");
      tile.classList.remove("tile-master");
      continue;
    }

    tile.classList.remove("hidden");
    const isMaster = (liveLayout === "7plus1" && ch === 1 && !fullscreenCh);
    if (isMaster) tile.classList.add("tile-master");
    else tile.classList.remove("tile-master");

    if (selectedCamId === ch) {
      tile.classList.add("border-primary", "ring-2", "ring-primary/40");
      tile.classList.remove("border-border");
    } else {
      tile.classList.remove("border-primary", "ring-2", "ring-primary/40");
      tile.classList.add("border-border");
    }

    const cam = cameras.find((c) => c.id === ch);
    const online = cam ? isCamOnline(cam) : false;
    // To preserve zero-latency switching and avoid Wasm 1080p decode bottlenecks,
    // live grid and fullscreen operate on the high-definition SUB stream (720p).
    // The MAIN stream remains dedicated to zero-transcode high-bitrate recording.
    const isMain = false;
    const targetStream = "sub";
    const isEnabled = cam && (cam.enabled !== false);

    // Update HUD elements
    const labelEl = document.getElementById(`label-${ch}`);
    if (labelEl) labelEl.textContent = `${chLabel(ch)}: ${cam ? cam.name : "Empty"}`;

    const dotEl = document.getElementById(`dot-${ch}`);
    if (dotEl) dotEl.className = `h-[7px] w-[7px] shrink-0 rounded-full ${online ? "bg-success soft-pulse" : "bg-white/30"}`;

    const resBadgeEl = document.getElementById(`res-badge-${ch}`);
    if (resBadgeEl) {
      const existing = liveStreamEngines.get(ch);
      const prefix = isMain ? "MAIN" : "SUB";
      if (existing && existing.detectedHeight > 0) {
        let resText = `${existing.detectedHeight}p`;
        if (existing.detectedHeight >= 2160) resText = "4K";
        else if (existing.detectedHeight >= 1440) resText = "2K";
        resBadgeEl.textContent = `${prefix} ${resText}`;
      } else {
        resBadgeEl.textContent = prefix;
      }
    }

    const noSigEl = document.getElementById(`no-signal-${ch}`);
    if (noSigEl) {
      if (cam && online) noSigEl.classList.add("hidden");
      else noSigEl.classList.remove("hidden");
    }

    const stageEl = document.getElementById(`stream-stage-${ch}`);
    if (stageEl && isEnabled) {
      const existing = liveStreamEngines.get(ch);
      if (!existing) {
        // First-time mount: start stream
        const engine = new NvrStreamEngine({
          channelId: ch,
          container: stageEl,
          streamType: targetStream,
          onStatus: (st) => {
            const badgeEl = document.getElementById(`stream-tier-${ch}`);
            if (badgeEl) {
              badgeEl.textContent = st.tier || st.text;
              badgeEl.className = `px-2 py-0.5 font-mono text-[10px] text-white/90 ${
                st.state === "PLAYING" ? "bg-success/80" : st.state === "FALLBACK" ? "bg-warning/80" : "bg-black/55"
              }`;
            }
          },
          onError: () => {
            const badgeEl = document.getElementById(`stream-tier-${ch}`);
            if (badgeEl) {
              badgeEl.textContent = "Error";
              badgeEl.className = "px-2 py-0.5 font-mono text-[10px] text-white/90 bg-destructive/80";
            }
          },
        });
        liveStreamEngines.set(ch, engine);
        engine.start();
      } else if (existing.streamType !== targetStream) {
        // Stream type changed (e.g. sub to main for fullscreen, or main to sub on exit)
        try { existing.destroy(); } catch {}
        const engine = new NvrStreamEngine({
          channelId: ch,
          container: stageEl,
          streamType: targetStream,
          onStatus: (st) => {
            const badgeEl = document.getElementById(`stream-tier-${ch}`);
            if (badgeEl) {
              badgeEl.textContent = st.tier || st.text;
              badgeEl.className = `px-2 py-0.5 font-mono text-[10px] text-white/90 ${
                st.state === "PLAYING" ? "bg-success/80" : st.state === "FALLBACK" ? "bg-warning/80" : "bg-black/55"
              }`;
            }
          },
          onError: () => {
            const badgeEl = document.getElementById(`stream-tier-${ch}`);
            if (badgeEl) {
              badgeEl.textContent = "Error";
              badgeEl.className = "px-2 py-0.5 font-mono text-[10px] text-white/90 bg-destructive/80";
            }
          },
        });
        liveStreamEngines.set(ch, engine);
        engine.start();
      }
      // If existing engine already matches targetStream: DO NOTHING.
      // Stream continues playing without a single dropped frame!
    } else {
      // Only tear down engine if camera was explicitly disabled or removed
      const existing = liveStreamEngines.get(ch);
      if (existing && !isEnabled) {
        try { existing.destroy(); } catch {}
        liveStreamEngines.delete(ch);
      }
    }
  }
}

function selectCamera(id) {
  selectedCamId = id;

  // 1. Highlight directory item
  const listEl = document.getElementById("live-camera-list");
  if (listEl) {
    listEl.querySelectorAll("[data-dir-cam]").forEach((btn) => {
      const camId = Number(btn.dataset.dirCam);
      if (camId === id) {
        btn.classList.add("bg-accent");
        btn.classList.remove("hover:bg-secondary");
      } else {
        btn.classList.remove("bg-accent");
        btn.classList.add("hover:bg-secondary");
      }
    });
  }

  // 2. Highlight matrix tile
  const grid = document.getElementById("live-matrix-grid");
  if (grid) {
    grid.querySelectorAll("[data-ch]").forEach((tile) => {
      const ch = Number(tile.dataset.ch);
      if (ch === id) {
        tile.classList.add("border-primary", "ring-2", "ring-primary/40");
        tile.classList.remove("border-border");
      } else {
        tile.classList.remove("border-primary", "ring-2", "ring-primary/40");
        tile.classList.add("border-border");
      }
    });
  }
}

function setPaneCollapsed(collapsed) {
  paneCollapsed = collapsed;
  const pane = document.getElementById("live-camera-pane");
  const expandBtn = document.getElementById("btn-expand-cam-pane");
  if (!pane) return;

  if (collapsed) {
    pane.classList.remove("w-72", "border-border");
    pane.classList.add("w-0", "border-transparent");
    if (expandBtn) {
      expandBtn.classList.remove("hidden");
      expandBtn.classList.add("inline-flex");
    }
  } else {
    pane.classList.remove("w-0", "border-transparent");
    pane.classList.add("w-72", "border-border");
    if (expandBtn) {
      expandBtn.classList.add("hidden");
      expandBtn.classList.remove("inline-flex");
    }
  }
}

function updateLayoutButtonsUI() {
  document.querySelectorAll(".layout-btn").forEach((btn) => {
    const l = btn.dataset.layout === "7plus1" ? "7plus1" : Number(btn.dataset.layout);
    const active = !fullscreenCh && liveLayout === l;
    if (active) {
      btn.className = "layout-btn flex items-center gap-1.5 rounded-lg px-2.5 py-1.5 text-[11px] font-bold transition-colors bg-primary text-primary-foreground shadow-sm";
    } else {
      btn.className = "layout-btn flex items-center gap-1.5 rounded-lg px-2.5 py-1.5 text-[11px] font-bold transition-colors text-secondary-foreground hover:bg-accent";
    }
  });
}

function applyLayout(l) {
  liveLayout = l;
  fullscreenCh = null;
  prevLayout = l;
  updateLayoutButtonsUI();
  const exitBtn = document.getElementById("btn-exit-fullscreen");
  if (exitBtn) {
    exitBtn.classList.add("hidden");
    exitBtn.classList.remove("inline-flex");
  }
  const cnt = (l === "7plus1" || l === 8) ? 8 : (l === 1 ? 1 : 4);
  fetch("/api/v1/layout", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ count: cnt, layout_count: cnt, layout: l })
  }).catch(() => {});
  fetch("/api/v1/layout/fullscreen", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ exit: true, enabled: false })
  }).catch(() => {});
  renderLiveMatrix();
}

function renderLiveCameraDir() {
  const countEl = document.getElementById("live-dir-count");
  if (countEl) countEl.textContent = `${cameras.length} / ${MAX_CHANNELS}`;

  const listEl = document.getElementById("live-camera-list");
  if (!listEl) return;

  const q = liveSearch.trim().toLowerCase();
  const filtered = cameras.filter((c) => {
    if (q && !(`${c.name}`.toLowerCase().includes(q) || c.ip_address.toLowerCase().includes(q))) return false;
    if (liveFilter === "online" && !isCamOnline(c)) return false;
    if (liveFilter === "rec" && !c.recording) return false;
    return true;
  });

  if (filtered.length === 0) {
    listEl.innerHTML = `
      <li class="p-4 text-center text-xs text-muted-foreground">
        No cameras match.
        <br />
        <a href="#cameras" class="mt-1 inline-block font-semibold text-primary hover:underline">+ Discover ONVIF</a>
      </li>
    `;
    return;
  }

  listEl.innerHTML = filtered
    .map((cam) => {
      const selected = selectedCamId === cam.id;
      const online = isCamOnline(cam);

      return `
      <li>
        <button type="button" data-dir-cam="${cam.id}" class="flex w-full items-center gap-2.5 rounded-xl p-2 text-left transition-colors ${selected ? "bg-accent" : "hover:bg-secondary"}">
          <img src="${getSnapshotUrl(cam.snapshot_key, cam.id)}" alt="${cam.name}" class="h-11 w-16 shrink-0 rounded-lg object-cover" />
          <div class="min-w-0">
            <div class="truncate text-[12.5px] font-semibold text-foreground">${cam.name}</div>
            <div class="flex items-center gap-1.5 font-mono text-[10.5px] text-muted-foreground">
              <span class="h-1.5 w-1.5 rounded-full ${online ? "bg-success" : "bg-warning"}"></span>
              ${chLabel(cam.id)} · ${cam.resolution}
            </div>
          </div>
        </button>
      </li>
    `;
    })
    .join("");

  listEl.querySelectorAll("[data-dir-cam]").forEach((btn) => {
    const id = Number(btn.dataset.dirCam);
    btn.addEventListener("click", () => {
      selectCamera(id);
    });
    btn.addEventListener("dblclick", () => {
      enterFullscreenMode(id);
    });
  });
}

function enterFullscreenMode(ch) {
  prevLayout = liveLayout;
  fullscreenCh = ch;
  selectedCamId = ch;
  updateLayoutButtonsUI();
  const exitBtn = document.getElementById("btn-exit-fullscreen");
  if (exitBtn) {
    exitBtn.classList.remove("hidden");
    exitBtn.classList.add("inline-flex");
  }
  fetch("/api/v1/layout/fullscreen", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ channel_id: ch })
  }).catch(() => {});
  renderLiveMatrix();
  selectCamera(ch);
}

function exitFullscreenMode() {
  fullscreenCh = null;
  liveLayout = prevLayout;
  updateLayoutButtonsUI();
  const exitBtn = document.getElementById("btn-exit-fullscreen");
  if (exitBtn) {
    exitBtn.classList.add("hidden");
    exitBtn.classList.remove("inline-flex");
  }
  fetch("/api/v1/layout/fullscreen", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ exit: true, enabled: false })
  }).catch(() => {});
  renderLiveMatrix();
  if (selectedCamId) selectCamera(selectedCamId);
}

// --- View 2: Recordings Playback & Activity Review ---
async function initPlaybackView() {
  // Populate channel dropdown
  const chSelect = document.getElementById("pb-channel");
  if (chSelect) {
    chSelect.innerHTML = `<option value="all">All channels</option>` + cameras.map((c) => `<option value="${c.id}">${chLabel(c.id)} · ${c.name}</option>`).join("");
  }

  // Fetch available recording dates on disk
  const dateInput = document.getElementById("pb-date");
  try {
    const res = await fetch("/api/v1/recordings/dates");
    if (res.ok) {
      const json = await res.json();
      const dates = Array.isArray(json.data) ? json.data : [];
      if (dates.length > 0 && dateInput && (!dateInput.value || dateInput.dataset.autoSet !== "false")) {
        dateInput.value = formatIsoDate(dates[0]);
      }
    }
  } catch (err) {}

  if (dateInput && !dateInput.value) {
    dateInput.value = new Date().toISOString().slice(0, 10);
  }

  // Auto-search footage so the activity cards feed loads immediately
  searchRecordings();
}

function setPlaybackViewMode(mode) {
  pbViewMode = mode;

  const btnCards = document.getElementById("btn-pb-mode-cards");
  const btnPlayer = document.getElementById("btn-pb-mode-player");
  const btnTable = document.getElementById("btn-pb-mode-table");

  const cardsView = document.getElementById("pb-cards-view");
  const playerView = document.getElementById("pb-player-view");
  const tableView = document.getElementById("pb-table-view");

  const activeBtnClass = "inline-flex h-7 items-center justify-center gap-1.5 rounded-md px-2.5 text-xs font-semibold bg-primary text-primary-foreground shadow-sm transition-colors";
  const inactiveBtnClass = "inline-flex h-7 items-center justify-center gap-1.5 rounded-md px-2.5 text-xs font-semibold text-muted-foreground transition-colors hover:text-foreground";

  if (btnCards) btnCards.className = mode === "cards" ? activeBtnClass : inactiveBtnClass;
  if (btnPlayer) btnPlayer.className = mode === "player" ? activeBtnClass : inactiveBtnClass;
  if (btnTable) btnTable.className = mode === "table" ? activeBtnClass : inactiveBtnClass;

  if (cardsView) {
    if (mode === "cards") cardsView.classList.remove("hidden");
    else cardsView.classList.add("hidden");
  }
  if (playerView) {
    if (mode === "player") playerView.classList.remove("hidden");
    else playerView.classList.add("hidden");
  }
  if (tableView) {
    if (mode === "table") tableView.classList.remove("hidden");
    else tableView.classList.add("hidden");
  }
}

async function searchRecordings() {
  const ch = document.getElementById("pb-channel")?.value || "all";
  const day = document.getElementById("pb-date")?.value || new Date().toISOString().slice(0, 10);
  const from = document.getElementById("pb-from")?.value || "00:00";
  const to = document.getElementById("pb-to")?.value || "23:59";
  const trigger = document.getElementById("pb-trigger")?.value || "all";

  showToast("Searching recordings index...", "info");

  try {
    const params = new URLSearchParams();
    if (ch !== "all") params.set("channel_id", ch);
    if (day) params.set("day", day);
    if (from) params.set("from", from);
    if (to) params.set("to", to);
    if (trigger !== "all") params.set("trigger", trigger);

    const res = await fetch(`/api/v1/recordings?${params.toString()}`);
    if (res.ok) {
      const json = await res.json();
      const list = Array.isArray(json) ? json : (json.data ?? []);
      pbSegments = Array.isArray(list) ? list : [];
      pbReviewItems = Array.isArray(json.review_items) ? json.review_items : [];

      renderPlaybackUI();

      if (pbReviewItems.length > 0 || pbSegments.length > 0) {
        showToast(`${pbReviewItems.length} activity items (${pbSegments.length} segments) loaded`, "success");
      } else {
        showToast("No recorded footage found on storage disk for this query", "info");
      }
      return;
    }
  } catch (err) {
    showToast("Failed to query recordings index from engine", "error");
  }

  pbSegments = [];
  pbReviewItems = [];
  renderPlaybackUI();
}

function openReviewItemInPlayer(item) {
  if (!item) return;

  let targetSeg = null;
  if (Array.isArray(item.segments) && item.segments.length > 0) {
    const firstRef = item.segments[0];
    targetSeg = (pbSegments || []).find((s) => s.id === firstRef.id) || firstRef;
  }
  if (!targetSeg && item.first_segment) {
    targetSeg = item.first_segment;
  }
  if (!targetSeg) {
    targetSeg = (pbSegments || []).find((s) => s.channel_id === item.channel_id);
  }

  if (targetSeg) {
    setPlaybackViewMode("player");
    startSegmentPlayback(targetSeg, 0, item);
    const stage = document.getElementById("pb-stage-container");
    if (stage) {
      stage.scrollIntoView({ behavior: "smooth", block: "center" });
    }
  } else {
    showToast("No playable segment found for this activity item", "error");
  }
}

function renderReviewCards() {
  const countEl = document.getElementById("pb-review-count");
  const gridEl = document.getElementById("pb-review-grid");
  const emptyEl = document.getElementById("pb-review-empty");

  if (!gridEl) return;

  if (!pbReviewItems || pbReviewItems.length === 0) {
    if (countEl) countEl.textContent = "0 activity events found";
    if (gridEl) {
      gridEl.innerHTML = "";
      gridEl.classList.add("hidden");
    }
    if (emptyEl) emptyEl.classList.remove("hidden");
    return;
  }

  if (emptyEl) emptyEl.classList.add("hidden");
  if (gridEl) gridEl.classList.remove("hidden");
  if (countEl) {
    countEl.textContent = `${pbReviewItems.length} activity items (${pbSegments ? pbSegments.length : 0} segmented clips on disk)`;
  }

  gridEl.innerHTML = pbReviewItems
    .map((item, idx) => {
      const cam = cameras.find((c) => c.id === item.channel_id);
      const camName = cam ? cam.name : `Camera ${pad2(item.channel_id)}`;
      const durationStr = formatDurationCompact(item.duration_seconds);
      const startD = new Date(item.start_time);
      const endD = new Date(item.end_time);
      const startValid = !isNaN(startD.getTime());
      const endValid = !isNaN(endD.getTime());
      const timeStr = (startValid && endValid)
        ? `${startD.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })} – ${endD.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}`
        : `${item.start_time.slice(11, 16)} – ${item.end_time.slice(11, 16)}`;
      const dateStr = startValid ? startD.toLocaleDateString() : item.start_time.slice(0, 10);
      const fallbackSnapshot = getSnapshotUrl(null, item.channel_id);
      const thumbUrl = item.thumbnail_url || fallbackSnapshot;
      const previewVideoUrl = item.video_url || "";

      let triggerBadge = `<span class="rounded bg-primary/80 px-1.5 py-0.5 text-[10px] font-semibold text-primary-foreground shadow-sm">Continuous</span>`;
      if (item.trigger_type === "event") {
        triggerBadge = `<span class="rounded bg-destructive/90 px-1.5 py-0.5 text-[10px] font-bold text-destructive-foreground flex items-center gap-1 shadow-sm"><span class="h-1.5 w-1.5 rounded-full bg-white animate-pulse"></span>AI Event</span>`;
      } else if (item.trigger_type === "motion") {
        triggerBadge = `<span class="rounded bg-amber-600/90 px-1.5 py-0.5 text-[10px] font-bold text-white flex items-center gap-1 shadow-sm"><span class="h-1.5 w-1.5 rounded-full bg-white"></span>Motion</span>`;
      }

      return `
        <div data-review-idx="${idx}" class="pb-review-card group relative flex flex-col overflow-hidden rounded-xl border border-border/70 bg-card shadow-sm transition-all duration-200 hover:shadow-md hover:border-primary/50 cursor-pointer">
          <div class="pb-stage-box relative aspect-video w-full overflow-hidden bg-black/90">
            <img src="${thumbUrl}" alt="${camName}" class="pb-card-thumb absolute inset-0 h-full w-full object-cover transition-transform duration-300 group-hover:scale-105" onerror="this.onerror=null;this.src='${fallbackSnapshot}';" />
            ${
              previewVideoUrl
                ? `<video preload="none" muted playsinline loop data-preview-src="${previewVideoUrl}" class="pb-card-preview-video absolute inset-0 h-full w-full object-cover hidden pointer-events-none"></video>`
                : ""
            }
            <div class="pb-card-scrub-bar absolute bottom-0 left-0 right-0 h-1 bg-white/30 hidden pointer-events-none">
              <div class="pb-card-scrub-fill h-full bg-primary" style="width: 0%"></div>
            </div>

            <!-- Top Badges -->
            <div class="absolute top-2 left-2 flex items-center gap-1.5 pointer-events-none">
              <span class="rounded bg-black/75 px-1.5 py-0.5 font-mono text-[10px] font-bold text-white">${chLabel(item.channel_id)}</span>
              ${item.is_locked ? '<span class="rounded bg-destructive px-1.5 py-0.5 text-[10px] font-bold text-white shadow">🔒 Locked</span>' : ''}
            </div>

            <div class="absolute top-2 right-2 pointer-events-none">
              ${triggerBadge}
            </div>

            <!-- Bottom Badges -->
            <span class="pb-card-scrub-time hidden absolute bottom-2 left-2 rounded bg-black/85 px-2 py-0.5 font-mono text-[10px] font-semibold text-white backdrop-blur-sm pointer-events-none">
              00:00
            </span>

            <span class="absolute bottom-2 right-2 rounded bg-black/85 px-2 py-0.5 font-mono text-[11px] font-semibold text-white shadow backdrop-blur-sm pointer-events-none">
              ${durationStr}
            </span>
          </div>

          <!-- Metadata -->
          <div class="flex flex-col p-3 gap-1 bg-card">
            <div class="flex items-start justify-between gap-2">
              <div class="overflow-hidden">
                <h4 class="font-display text-sm font-semibold text-foreground truncate" title="${camName}">
                  ${camName}
                </h4>
                <p class="text-[11px] text-muted-foreground font-mono truncate">
                  ${dateStr} · ${timeStr}
                </p>
              </div>
              <span class="shrink-0 rounded-md bg-secondary px-2 py-0.5 text-[10px] font-mono text-secondary-foreground font-medium">
                ${item.segment_count || 1} seg
              </span>
            </div>

            <div class="mt-1 flex items-center justify-between text-[11px] text-muted-foreground border-t border-border/40 pt-2">
              <span class="font-mono">${formatBytes(item.file_size_bytes)}</span>
              <span class="inline-flex items-center gap-1 font-medium text-primary text-[11px] transition-transform group-hover:translate-x-0.5">
                Play timeline
                <svg class="h-3 w-3" fill="none" viewBox="0 0 24 24" stroke="currentColor"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/></svg>
              </span>
            </div>
          </div>
        </div>
      `;
    })
    .join("");

  // Attach interactive preview and scrubbing listeners to cards
  gridEl.querySelectorAll(".pb-review-card").forEach((card) => {
    const idx = Number(card.dataset.reviewIdx);
    const item = pbReviewItems[idx];
    if (!item) return;

    const stageBox = card.querySelector(".pb-stage-box");
    const previewVid = card.querySelector(".pb-card-preview-video");
    const scrubBar = card.querySelector(".pb-card-scrub-bar");
    const scrubFill = card.querySelector(".pb-card-scrub-fill");
    const scrubTime = card.querySelector(".pb-card-scrub-time");
    let previewTimer = null;

    card.addEventListener("mouseenter", () => {
      if (previewTimer) clearTimeout(previewTimer);
      previewTimer = setTimeout(() => {
        if (previewVid && previewVid.dataset.previewSrc) {
          if (!previewVid.src) {
            previewVid.src = previewVid.dataset.previewSrc;
          }
          previewVid.classList.remove("hidden");
          previewVid.currentTime = 0;
          previewVid.play().catch(() => {});
        }
      }, 180);
    });

    if (stageBox) {
      stageBox.addEventListener("mousemove", (e) => {
        const rect = stageBox.getBoundingClientRect();
        const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
        const seekSec = ratio * (item.duration_seconds || 60);

        if (scrubBar) scrubBar.classList.remove("hidden");
        if (scrubFill) scrubFill.style.width = `${ratio * 100}%`;
        if (scrubTime) {
          scrubTime.classList.remove("hidden");
          scrubTime.textContent = formatDurationCompact(seekSec);
        }

        if (previewVid && previewVid.duration) {
          previewVid.currentTime = Math.min(previewVid.duration, ratio * previewVid.duration);
        }
      });
    }

    card.addEventListener("mouseleave", () => {
      if (previewTimer) clearTimeout(previewTimer);
      if (previewVid) {
        previewVid.pause();
        previewVid.classList.add("hidden");
      }
      if (scrubBar) scrubBar.classList.add("hidden");
      if (scrubTime) scrubTime.classList.add("hidden");
    });

    card.addEventListener("click", () => {
      openReviewItemInPlayer(item);
    });
  });
}

function renderPlaybackUI() {
  renderReviewCards();

  const countEl = document.getElementById("pb-segments-count");
  if (countEl) {
    countEl.textContent = pbSegments === null ? "Press Search footage to load the index" : `${pbSegments.length} segments found`;
  }

  // Render Table
  const tbody = document.getElementById("pb-table-body");
  if (tbody) {
    if (!pbSegments || pbSegments.length === 0) {
      tbody.innerHTML = `
        <tr>
          <td colspan="8" class="p-4 align-middle h-24 text-center text-sm text-muted-foreground">
            ${pbSegments === null ? "No search performed yet." : "No recorded segments match the selected time range and filters."}
          </td>
        </tr>
      `;
    } else {
      tbody.innerHTML = pbSegments
        .map((seg) => {
          const isCurrent = pbActive?.id === seg.id;
          return `
          <tr data-seg-id="${seg.id}" class="border-b transition-colors hover:bg-muted/50 data-[state=selected]:bg-muted ${isCurrent ? "bg-muted/80 font-medium" : ""}">
            <td class="p-4 align-middle font-mono text-xs">#${seg.id}</td>
            <td class="p-4 align-middle">
              <span class="inline-flex items-center rounded-md border border-transparent bg-secondary px-2.5 py-0.5 font-mono text-[10px] text-secondary-foreground font-semibold">
                ${chLabel(seg.channel_id)}
              </span>
            </td>
            <td class="p-4 align-middle font-mono text-xs">${seg.start_time.slice(0, 19).replace("T", " ")}</td>
            <td class="p-4 align-middle font-mono text-xs">${seg.end_time.slice(11, 19)}</td>
            <td class="p-4 align-middle font-mono text-xs">${seg.duration_seconds}s</td>
            <td class="p-4 align-middle font-mono text-xs">${formatBytes(seg.file_size_bytes)}</td>
            <td class="p-4 align-middle">
              <button type="button" data-lock-seg="${seg.id}" class="inline-flex h-7 items-center justify-center gap-1 rounded-md px-2 text-[11px] font-medium transition-colors ${
                seg.is_locked ? "bg-destructive text-destructive-foreground hover:bg-destructive/90" : "bg-secondary text-secondary-foreground hover:bg-accent"
              }">
                <svg class="h-3 w-3" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
                  ${
                    seg.is_locked
                      ? '<rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0110 0v4"/>'
                      : '<rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 019.9-1"/>'
                  }
                </svg>
                ${seg.is_locked ? "Protected" : "Normal"}
              </button>
            </td>
            <td class="p-4 align-middle">
              <div class="flex gap-1.5">
                <button type="button" data-play-seg="${seg.id}" class="inline-flex h-7 items-center justify-center gap-1 rounded-md bg-primary px-2.5 text-[11px] font-medium text-primary-foreground shadow hover:bg-primary/90">
                  <svg class="h-3 w-3" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><polygon points="5 3 19 12 5 21 5 3"/></svg>
                  Play
                </button>
                <button type="button" data-dl-seg="${seg.id}" class="inline-flex h-7 w-7 items-center justify-center rounded-md bg-secondary text-secondary-foreground hover:bg-accent" title="Download MP4">
                  <svg class="h-3 w-3" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1m-4-4l-4 4m0 0l-4-4m4 4V4"/></svg>
                </button>
              </div>
            </td>
          </tr>
        `;
        })
        .join("");

      // Attach row listeners
      tbody.querySelectorAll("[data-lock-seg]").forEach((btn) => {
        btn.addEventListener("click", () => {
          const id = Number(btn.dataset.lockSeg);
          toggleRecordingLock(id);
        });
      });

      tbody.querySelectorAll("[data-play-seg]").forEach((btn) => {
        btn.addEventListener("click", () => {
          const id = Number(btn.dataset.playSeg);
          const seg = pbSegments.find((s) => s.id === id);
          if (seg) {
            setPlaybackViewMode("player");
            startSegmentPlayback(seg);
          }
        });
      });

      tbody.querySelectorAll("[data-dl-seg]").forEach((btn) => {
        btn.addEventListener("click", () => {
          const id = Number(btn.dataset.dlSeg);
          const seg = pbSegments.find((s) => s.id === id);
          if (seg) {
            const link = document.createElement("a");
            link.href = seg.video_url || `/api/v1/recordings/video?id=${seg.id}&download=true`;
            link.download = `channel_${seg.channel_id}_segment_${seg.id}.mp4`;
            document.body.appendChild(link);
            link.click();
            document.body.removeChild(link);
            showToast(`Downloading recording #${seg.id}`, "success");
          }
        });
      });
    }
  }

  setPlaybackViewMode(pbViewMode);
}

async function toggleRecordingLock(segId) {
  const seg = (pbSegments || []).find((s) => s.id === segId);
  if (!seg) return;

  try {
    await fetch(`/api/v1/recordings/${segId}/lock`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ is_locked: !seg.is_locked, file_path: seg.file_path }),
    });
  } catch {}

  seg.is_locked = !seg.is_locked;
  renderPlaybackUI();
  showToast(`Footage #${segId} ${seg.is_locked ? "locked against circular retention" : "unlocked"}`, "success");
}

function getConsecutiveSegment(currentSeg, forward = true) {
  if (!currentSeg || !pbSegments || pbSegments.length === 0) return null;
  const sameChannelSegs = pbSegments.filter((s) => s.channel_id === currentSeg.channel_id);
  sameChannelSegs.sort((a, b) => getEpochMs(a.start_time_ms || a.start_time) - getEpochMs(b.start_time_ms || b.start_time));
  const idx = sameChannelSegs.findIndex((s) => s.id === currentSeg.id);
  if (idx === -1) return null;
  if (forward && idx + 1 < sameChannelSegs.length) {
    return sameChannelSegs[idx + 1];
  } else if (!forward && idx > 0) {
    return sameChannelSegs[idx - 1];
  }
  return null;
}

function highlightActiveSegmentRow(segId) {
  document.querySelectorAll("#pb-table-body tr").forEach((tr) => {
    if (tr.dataset.segId === String(segId)) {
      tr.classList.add("bg-muted/80", "font-medium");
    } else {
      tr.classList.remove("bg-muted/80", "font-medium");
    }
  });
}

function syncAllChannelsToTime(targetMs) {
  const currentTargetMs = (targetMs && targetMs > 86400000)
    ? targetMs
    : (pbActive ? getEpochMs(pbActive.start_time_ms || pbActive.start_time) + (targetMs || 0) * 1000 : 0);

  for (let ch = 1; ch <= MAX_CHANNELS; ch++) {
    const vid = document.getElementById(`pb-sync-vid-${ch}`);
    if (!vid) continue;
    const seg = (pbSegments || []).find((s) => {
      if (s.channel_id !== ch) return false;
      const sStart = getEpochMs(s.start_time_ms || s.start_time);
      const sEnd = getEpochMs(s.end_time_ms || s.end_time);
      return currentTargetMs >= sStart && currentTargetMs <= sEnd;
    });
    if (seg) {
      const url = seg.video_url || `/api/v1/recordings/video?path=${encodeURIComponent(seg.file_path)}`;
      const fullUrl = new URL(url, window.location.origin).href;
      if (vid.src !== fullUrl) {
        vid.src = url;
      }
      const sStart = getEpochMs(seg.start_time_ms || seg.start_time);
      const offset = Math.max(0, (currentTargetMs - sStart) / 1000);
      vid.currentTime = offset;
      vid.playbackRate = pbRate;
      if (pbPlaying) vid.play().catch(() => {});
      else vid.pause();
    } else {
      vid.pause();
    }
  }
}

function startSegmentPlayback(seg, seekOffsetSec = 0, activityItem = null) {
  if (!seg) return;
  pbActive = seg;
  pbClipDuration = (activityItem && activityItem.duration_seconds) ? activityItem.duration_seconds : (seg.duration_seconds || 60);
  pbPlayhead = getEpochMs(seg.start_time_ms || seg.start_time);
  pbPlayhead = segStart + seekOffsetSec;
  pbPlaying = true;

  document.getElementById("pb-placeholder")?.classList.add("hidden");
  document.getElementById("pb-top-overlay")?.classList.remove("hidden");
  document.getElementById("pb-controls-overlay")?.classList.remove("hidden");

  const cam = cameras.find((c) => c.id === seg.channel_id);
  const camName = cam ? cam.name : chLabel(seg.channel_id);
  const dateObj = new Date(seg.start_time);
  const localTimeStr = isNaN(dateObj.getTime()) ? seg.start_time : dateObj.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const titleEl = document.getElementById("pb-watching-title");
  if (titleEl) {
    titleEl.textContent = `${camName} · ${localTimeStr} (${formatDurationCompact(pbClipDuration)})`;
  }



  if (pbHls) {
    try { pbHls.destroy(); } catch (_) {}
    pbHls = null;
  }

  const videoEl = document.getElementById("pb-video");
  if (videoEl) {
    if (pbLayoutMode === "single") {
      videoEl.classList.remove("hidden");
      document.getElementById("pb-sync-grid")?.classList.add("hidden");
    }
    const videoUrl = seg.video_url || (seg.file_path ? `/api/v1/recordings/video?path=${encodeURIComponent(seg.file_path)}` : "");
    if (videoUrl) {
      const fullUrl = new URL(videoUrl, window.location.origin).href;
      if (videoEl.src !== fullUrl) {
        videoEl.src = videoUrl;
      }
      videoEl.playbackRate = pbRate;
      videoEl.muted = pbAudioMuted;
      if (seekOffsetSec > 0) {
        videoEl.currentTime = seekOffsetSec;
      }
      videoEl.play().catch((err) => {
        // Browser autoplay restriction: if unmuted autoplay is rejected, fallback to muted
        if (err.name === "NotAllowedError" && !videoEl.muted) {
          videoEl.muted = true;
          pbAudioMuted = true;
          updateAudioUI();
          videoEl.play().catch(() => {});
        }
      });
    }
  }

  if (pbLayoutMode === "grid4") {
    syncAllChannelsToTime(pbPlayhead);
  }

  // Enable playback buttons
  ["btn-pb-back10s", "btn-pb-playpause", "btn-pb-fwd10s", "btn-pb-download", "btn-pb-fullscreen"].forEach((id) => {
    const el = document.getElementById(id);
    if (el) el.removeAttribute("disabled");
  });

  updatePlayPauseUI();
  updatePlayheadUI();
  highlightActiveSegmentRow(seg.id);

  showToast(`Playing segment #${seg.id} — ${chLabel(seg.channel_id)}`, "info");
}

function updatePlayheadUI() {
  const videoEl = document.getElementById("pb-video");
  const cur = videoEl ? videoEl.currentTime : 0;
  const dur = (videoEl && videoEl.duration && !isNaN(videoEl.duration) && videoEl.duration > 0)
                ? videoEl.duration
                : (pbClipDuration || 60);

  const pct = Math.min(100, Math.max(0, (cur / dur) * 100));

  const seekFill = document.getElementById("pb-seek-fill");
  const seekThumb = document.getElementById("pb-seek-thumb");
  if (seekFill) seekFill.style.width = `${pct}%`;
  if (seekThumb) seekThumb.style.left = `${pct}%`;

  const hasHours = dur >= 3600;
  const counter = document.getElementById("pb-time-counter");
  if (counter) counter.textContent = `${formatTimeMSS(cur, hasHours)} / ${formatTimeMSS(dur, hasHours)}`;
}

function updatePlayPauseUI() {
  const icon = document.getElementById("icon-pb-play");
  const badge = document.getElementById("pb-badge-state");

  if (icon) {
    icon.innerHTML = pbPlaying
      ? '<rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/>'
      : '<polygon points="5 3 19 12 5 21 5 3"/>';
  }
  if (badge) {
    badge.innerHTML = `<span class="${pbPlaying ? "rec-dot" : ""} mr-1 inline-block h-1.5 w-1.5 rounded-full bg-white"></span>${pbPlaying ? "PLAYING" : "PAUSED"}`;
    badge.className = pbPlaying
      ? "inline-flex items-center rounded-md border border-transparent bg-destructive/90 px-2.5 py-0.5 font-mono text-[10px] font-bold text-white shadow"
      : "inline-flex items-center rounded-md border border-white/20 bg-black/60 px-2.5 py-0.5 font-mono text-[10px] font-bold text-white/80 shadow";
  }
}

function updateAudioUI() {
  const iconOn = document.getElementById("icon-pb-audio-on");
  const iconOff = document.getElementById("icon-pb-audio-off");
  if (iconOn && iconOff) {
    if (pbAudioMuted) {
      iconOn.classList.add("hidden");
      iconOff.classList.remove("hidden");
    } else {
      iconOn.classList.remove("hidden");
      iconOff.classList.add("hidden");
    }
  }
}



// --- View 3: Cameras Management ---
function renderCamerasView() {
  const grid = document.getElementById("cam-grid");
  if (!grid) return;

  const freeCount = MAX_CHANNELS - cameras.length;
  const addBtn = document.getElementById("btn-cam-add-manual");
  if (addBtn) {
    if (freeCount <= 0) {
      addBtn.setAttribute("disabled", "true");
      addBtn.classList.add("opacity-50", "pointer-events-none");
    } else {
      addBtn.removeAttribute("disabled");
      addBtn.classList.remove("opacity-50", "pointer-events-none");
    }
  }

  if (cameras.length === 0) {
    grid.innerHTML = `
      <div class="col-span-full rounded-xl border border-border/70 bg-card p-14 text-center shadow-card">
        <svg class="mx-auto h-10 w-10 text-muted-foreground" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
        <h3 class="mt-3 font-display text-lg font-semibold">No cameras added yet</h3>
        <p class="mx-auto mt-1 max-w-md text-sm text-muted-foreground">
          Discover ONVIF compliant cameras on the local subnet or configure an RTSP stream manually.
        </p>
        <button type="button" id="btn-empty-onvif" class="mt-4 inline-flex h-9 items-center justify-center gap-1.5 rounded-md bg-primary px-4 text-xs font-medium text-primary-foreground shadow hover:bg-primary/90">
          <svg class="h-3.5 w-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg>
          Run ONVIF network discovery
        </button>
      </div>
    `;
    document.getElementById("btn-empty-onvif")?.addEventListener("click", openOnvifWizard);
    return;
  }

  grid.innerHTML = cameras
    .map((cam) => {
      const online = isCamOnline(cam);

      return `
      <div class="overflow-hidden rounded-xl border border-border/70 bg-card shadow-card">
        <div class="tile-stage-bg relative aspect-video overflow-hidden">
          ${
            online
              ? `
            <img src="${getSnapshotUrl(cam.snapshot_key, cam.id)}" alt="${cam.name}" class="tile-drift absolute inset-0 h-full w-full object-cover" />
          `
              : `
            <div class="absolute inset-0 flex flex-col items-center justify-center gap-1.5 text-white/50">
              <svg class="h-8 w-8" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
              <span class="text-xs font-semibold">No signal</span>
            </div>
          `
          }
          <div class="absolute left-2 top-2">
            <span class="inline-flex items-center rounded-md border border-transparent bg-black/50 px-2.5 py-0.5 font-mono text-[10px] text-white font-semibold">
              ${chLabel(cam.id)}
            </span>
          </div>
          <div class="absolute right-2 top-2 flex items-center gap-1">
            <span class="inline-flex items-center rounded-md border border-transparent ${
              online ? "bg-success text-white" : "bg-muted text-muted-foreground"
            } px-2.5 py-0.5 font-mono text-[10px] font-semibold">
              ${online ? "ONLINE" : "OFFLINE"}
            </span>
            ${
              cam.recording
                ? `
              <span class="inline-flex items-center rounded-md border border-transparent bg-destructive px-2 py-0.5 font-mono text-[9px] font-bold text-white">
                <span class="rec-dot mr-1 inline-block h-1.5 w-1.5 rounded-full bg-white"></span>
                REC
              </span>
            `
                : ""
            }
          </div>
        </div>

        <div class="space-y-2.5 p-3.5">
          <div class="flex items-center justify-between gap-2">
            <h3 class="truncate text-sm font-semibold text-foreground">${cam.name}</h3>
            <span class="shrink-0 font-mono text-[11px] text-muted-foreground">${cam.ip_address}</span>
          </div>

          <div class="grid grid-cols-2 gap-x-3 gap-y-1 text-[11.5px] text-muted-foreground">
            <div>
              Resolution: <span class="font-mono font-semibold text-foreground">${cam.resolution}</span>
            </div>
            <div>
              Frame rate: <span class="font-mono font-semibold text-foreground">${cam.fps} FPS</span>
            </div>
            <div>
              Sub stream: <span class="inline-flex items-center rounded-full bg-success px-1.5 py-0 text-[9px] font-semibold text-white">Active</span>
            </div>
            <div>
              Bitrate: <span class="font-mono font-semibold text-foreground">${cam.bitrate_kbps ? `${cam.bitrate_kbps}k` : "Auto"}</span>
            </div>
          </div>

          <p class="truncate font-mono text-[10.5px] text-muted-foreground" title="${cam.rtsp_url_sub || cam.rtsp_url_main || ""}">
            RTSP: ${cam.rtsp_url_sub || cam.rtsp_url_main || "Not configured"}
          </p>

          <div class="flex flex-wrap gap-1.5 pt-1">
            <button type="button" data-rec-cam="${cam.id}" class="inline-flex h-7 items-center justify-center gap-1.5 rounded-md ${cam.is_recording !== false ? "bg-red-500/15 text-destructive border border-destructive/30 hover:bg-red-500/25" : "bg-secondary text-secondary-foreground hover:bg-secondary/80"} px-2 text-[11px] font-medium shadow-sm transition-colors" title="${cam.is_recording !== false ? "Stop recording channel" : "Start recording channel"}">
              <span class="h-2 w-2 rounded-full ${cam.is_recording !== false ? "bg-destructive animate-pulse" : "bg-muted-foreground"}"></span>
              <span>${cam.is_recording !== false ? "REC active" : "REC idle"}</span>
            </button>
            <button type="button" data-hw-cam="${cam.id}" class="inline-flex h-7 items-center justify-center gap-1.5 rounded-md bg-secondary px-2 text-[11px] font-medium text-secondary-foreground shadow-sm transition-colors hover:bg-secondary/80">
              <svg class="h-3.5 w-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="16" height="16" x="4" y="4" rx="2"/><rect width="6" height="6" x="9" y="9" rx="1"/><path d="M15 2v2"/><path d="M15 20v2"/><path d="M2 15h2"/><path d="M2 9h2"/><path d="M20 15h2"/><path d="M20 9h2"/><path d="M9 2v2"/><path d="M9 20v2"/></svg>
              HW config
            </button>
            <button type="button" data-restart-cam="${cam.id}" class="inline-flex h-7 w-7 items-center justify-center rounded-md bg-secondary p-0 text-[11px] font-medium text-secondary-foreground shadow-sm transition-colors hover:bg-secondary/80" title="Restart RTSP ingress session">
              <svg class="h-3.5 w-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/></svg>
            </button>
            <button type="button" data-edit-cam="${cam.id}" class="inline-flex h-7 items-center justify-center gap-1.5 rounded-md bg-secondary px-2 text-[11px] font-medium text-secondary-foreground shadow-sm transition-colors hover:bg-secondary/80">
              <svg class="h-3.5 w-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"/><path d="m15 5 4 4"/></svg>
              Edit
            </button>
            <button type="button" data-delete-cam="${cam.id}" class="inline-flex h-7 items-center justify-center gap-1.5 rounded-md bg-destructive px-2 text-[11px] font-medium text-destructive-foreground shadow-sm transition-colors hover:bg-destructive/90">
              <svg class="h-3.5 w-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 1-2h6c1 0 1 1 1 2v2"/><line x1="10" x2="10" y1="11" y2="17"/><line x1="14" x2="14" y1="11" y2="17"/></svg>
              Delete
            </button>
          </div>
        </div>
      </div>
    `;
    })
    .join("");

  // Attach card listeners
  grid.querySelectorAll("[data-rec-cam]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const id = Number(btn.dataset.recCam);
      const cam = cameras.find((c) => c.id === id);
      if (!cam) return;
      const currentlyRecording = cam.is_recording !== false;
      const action = currentlyRecording ? "stop" : "start";
      showToast(`${currentlyRecording ? "Stopping" : "Starting"} recording for ${chLabel(id)}...`, "info");
      try {
        const res = await fetch(`/api/v1/cameras/${id}/record/${action}`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ channel_id: id, mode: "CONTINUOUS" })
        });
        const d = await res.json();
        if (d.success) {
          cam.is_recording = !currentlyRecording;
          showToast(`Recording ${currentlyRecording ? "stopped" : "started"} for ${chLabel(id)}`, "success");
          renderCamerasView();
        } else {
          showToast(`Recording control failed: ${d.message || "Engine error"}`, "error");
        }
      } catch (err) {
        showToast(`Request failed: ${err.message}`, "error");
      }
    });
  });

  grid.querySelectorAll("[data-edit-cam]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = Number(btn.dataset.editCam);
      openEditModal(id);
    });
  });

  grid.querySelectorAll("[data-hw-cam]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = Number(btn.dataset.hwCam);
      openHwModal(id);
    });
  });

  grid.querySelectorAll("[data-restart-cam]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const id = Number(btn.dataset.restartCam);
      showToast(`Restarting RTSP ingress session for ${chLabel(id)}...`, "info");
      try {
        await fetch(`/api/v1/cameras/${id}/restart`, { method: "POST" });
        showToast(`Ingress session for ${chLabel(id)} restarted`, "success");
      } catch {
        showToast(`Restart triggered for ${chLabel(id)}`, "success");
      }
    });
  });

  grid.querySelectorAll("[data-delete-cam]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const id = Number(btn.dataset.deleteCam);
      openDeleteModal(id);
    });
  });
}

// --- View 4: Dashboard ---
function renderDashboardView() {
  const activeCount = cameras.filter((c) => c.enabled && isCamOnline(c)).length;
  const usedMb = Math.max(0, telemetry.storage_total_mb - telemetry.storage_free_mb);
  const usedPct = Math.min(100, Math.max(4, (usedMb / telemetry.storage_total_mb) * 100));

  const statusEl = document.getElementById("dash-core-status");
  const uptimeEl = document.getElementById("dash-core-uptime");
  const cpuEl = document.getElementById("dash-cpu-pct");
  const ramEl = document.getElementById("dash-ram-free");
  const storageEl = document.getElementById("dash-storage-free");
  const activeCamsEl = document.getElementById("dash-active-cams");
  const storageNums = document.getElementById("dash-storage-numbers");
  const storageBar = document.getElementById("dash-storage-bar");
  const bitrateEl = document.getElementById("dash-bitrate");

  if (statusEl) statusEl.textContent = telemetry.status;
  if (uptimeEl) uptimeEl.textContent = `Uptime: ${formatUptime(telemetry.uptime_seconds)}`;
  if (cpuEl) cpuEl.textContent = `${telemetry.cpu_load_percent.toFixed(1)}%`;
  if (ramEl) ramEl.textContent = `${telemetry.ram_free_mb} MB`;
  if (storageEl) storageEl.textContent = `${(telemetry.storage_free_mb / 1024).toFixed(1)} GB`;
  if (activeCamsEl) activeCamsEl.textContent = `${activeCount} / ${cameras.length || 8}`;
  if (storageNums) storageNums.textContent = `${(usedMb / 1024).toFixed(1)} GB / ${(telemetry.storage_total_mb / 1024).toFixed(1)} GB`;
  if (storageBar) storageBar.style.width = `${usedPct}%`;
  if (bitrateEl) bitrateEl.textContent = `Ingest ${telemetry.bitrate_mbps.toFixed(1)} Mbps`;

  // Channels preview grid
  const channelsGrid = document.getElementById("dash-channels-grid");
  if (channelsGrid) {
    channelsGrid.innerHTML = cameras
      .map((cam) => {
        const online = isCamOnline(cam);
        return `
        <div class="overflow-hidden rounded-xl border border-border/70 bg-card shadow-card">
          <div class="tile-stage-bg relative aspect-video overflow-hidden">
            <img src="${getSnapshotUrl(cam.snapshot_key, cam.id)}" alt="${cam.name}" class="tile-drift h-full w-full object-cover" />
            <div class="absolute left-2 top-2">
              <span class="inline-flex items-center rounded-md border border-transparent bg-black/50 px-2.5 py-0.5 font-mono text-[10px] text-white font-semibold">${chLabel(cam.id)}</span>
            </div>
            <div class="absolute right-2 top-2">
              <span class="inline-flex items-center rounded-md border border-transparent ${online ? "bg-success text-white" : "bg-muted text-muted-foreground"} px-2.5 py-0.5 font-mono text-[10px] font-semibold">
                ${online ? "LIVE SUB" : "OFFLINE"}
              </span>
            </div>
            <div class="absolute bottom-1.5 left-2 font-mono text-[10px] text-white/90 drop-shadow">
              ${cam.resolution} · ${cam.fps} FPS
            </div>
          </div>
          <div class="flex items-center justify-between gap-2 p-3">
            <div class="min-w-0">
              <div class="truncate text-[13px] font-semibold text-foreground">${cam.name}</div>
              <div class="truncate font-mono text-[11px] text-muted-foreground">${cam.ip_address} · ${cam.codec.toUpperCase()}</div>
            </div>
            <a href="#live" class="inline-flex h-8 items-center justify-center gap-1.5 rounded-md bg-secondary px-3 text-xs font-medium text-secondary-foreground hover:bg-accent">
              <svg class="h-3.5 w-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/><path stroke-linecap="round" stroke-linejoin="round" d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z"/></svg>
              Watch live
            </a>
          </div>
        </div>
      `;
      })
      .join("");
  }
}

// --- View 5: System & Storage ---
async function renderSystemView() {
  const box = document.getElementById("trace-box");
  const badge = document.getElementById("trace-events-badge");

  // Load storage path and disk telemetry
  try {
    const sRes = await fetch("/api/v1/storage");
    if (sRes.ok) {
      const sJson = await sRes.json();
      const sData = sJson.data ?? sJson;
      const recPath = sData.recording_path || "./recordings";
      const pathLabel = document.getElementById("sys-storage-path");
      if (pathLabel) pathLabel.textContent = `${recPath} (dedicated disk)`;
      const pathInput = document.getElementById("input-storage-path");
      if (pathInput && document.activeElement !== pathInput) {
        pathInput.value = recPath;
      }

      const diskHealth = document.getElementById("sys-disk-health");
      if (diskHealth) {
        const freeGb = (sData.free_storage_bytes / (1024 * 1024 * 1024)).toFixed(1);
        const totalGb = (sData.total_storage_bytes / (1024 * 1024 * 1024)).toFixed(1);
        diskHealth.textContent = `${freeGb} GB Free / ${totalGb} GB (${sData.is_writable ? "Writable" : "Read-Only"})`;
      }
    }
  } catch {}

  try {
    const res = await fetch("/api/v1/diagnostics/trace?max_count=128");
    if (res.ok) {
      const json = await res.json();
      const data = json.data ?? json;
      if (data && Array.isArray(data.breadcrumbs) && data.breadcrumbs.length > 0) {
        const lines = data.breadcrumbs.map((b) => {
          const dateStr = b.timestamp_us ? new Date(Math.floor(b.timestamp_us / 1000)).toISOString().slice(11, 23) : new Date().toISOString().slice(11, 23);
          const lvl = (b.level ?? "INFO").padEnd(5);
          const tag = b.tag ? `[${b.tag}]` : "";
          const loc = b.file ? ` (${b.file.split("/").pop()}:${b.line ?? 0})` : "";
          return `[${dateStr}] [${lvl}] ${tag} ${b.message ?? ""}${loc}`;
        });
        if (box) {
          box.textContent = lines.join("\n");
          box.scrollTop = box.scrollHeight;
        }
        if (badge) {
          const proc = data.process;
          badge.textContent = `${lines.length} events${proc?.rss_kb ? ` · RSS: ${(proc.rss_kb / 1024).toFixed(1)} MB (${proc.threads ?? 0} threads)` : ""}`;
        }
        return;
      }
    }
  } catch {}

  if (box) {
    box.textContent = "Diagnostics tracer idle — awaiting camera ingress events and system telemetry.";
  }
  if (badge) {
    badge.textContent = "0 events · Inactive";
  }
}

async function updateStoragePath() {
  const input = document.getElementById("input-storage-path");
  const errBox = document.getElementById("change-storage-error");
  if (errBox) errBox.classList.add("hidden");

  if (!input) return;
  const newPath = input.value.trim();
  if (!newPath) {
    if (errBox) {
      errBox.textContent = "Please enter a valid storage path";
      errBox.classList.remove("hidden");
    }
    showToast("Please enter a valid storage path", "error");
    return;
  }

  const btn = document.getElementById("btn-apply-storage-path");
  if (btn) {
    btn.disabled = true;
    btn.textContent = "Applying...";
  }

  try {
    const res = await fetch("/api/v1/storage/path", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: newPath })
    });
    const data = await res.json();
    if (res.ok && (data.success || data.recording_path)) {
      const appliedPath = data.data?.recording_path || data.recording_path || newPath;
      showToast("Recording storage path updated to " + appliedPath, "success");
      const pathLabel = document.getElementById("sys-storage-path");
      if (pathLabel) pathLabel.textContent = `${appliedPath} (dedicated disk)`;
      closeModal("modal-change-storage");
      await renderSystemView();
      await fetchTelemetry();
    } else {
      const msg = data.message || data.error || "Failed to update storage path";
      if (errBox) {
        errBox.textContent = msg;
        errBox.classList.remove("hidden");
      }
      showToast(msg, "error");
    }
  } catch (err) {
    if (errBox) {
      errBox.textContent = err.message;
      errBox.classList.remove("hidden");
    }
    showToast("Network error updating storage path: " + err.message, "error");
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = "Apply Path";
    }
  }
}

// --- Modals Implementation ---
function openModal(id) {
  const modal = document.getElementById(id);
  if (modal) modal.classList.add("open");
}

function closeModal(id) {
  const modal = document.getElementById(id);
  if (modal) modal.classList.remove("open");
}

// --- User Header Display ---
function updateUserHeader() {
  const uEl = document.getElementById("header-username");
  const rEl = document.getElementById("header-user-role");

  if (currentUser && currentUser.username) {
    if (uEl) uEl.textContent = currentUser.username;
    if (rEl) {
      rEl.textContent = currentUser.role || "ADMIN";
      rEl.className = "rounded bg-primary/20 px-1.5 py-0.5 text-[9px] font-bold text-primary";
    }
  }
}

async function renderUsersView() {
  await loadUsersTable();
}

async function loadUsersTable() {
  const tbody = document.getElementById("users-table-body");
  if (!tbody) return;

  try {
    const res = await fetch("/api/v1/users");
    if (!res.ok) return;
    const json = await res.json();
    const users = Array.isArray(json.data) ? json.data : (Array.isArray(json) ? json : (json.data?.users || json.users || []));
    tbody.innerHTML = "";

    // Calculate KPI metrics
    let total = 0, admins = 0, operators = 0, viewers = 0;
    users.forEach((u) => {
      total++;
      const r = (u.role || "").toUpperCase();
      if (r === "ADMIN") admins++;
      else if (r === "OPERATOR") operators++;
      else if (r === "VIEWER") viewers++;
    });

    const kpiTotal = document.getElementById("kpi-total-users");
    const kpiAdmin = document.getElementById("kpi-admin-users");
    const kpiOp = document.getElementById("kpi-operator-users");
    const kpiVw = document.getElementById("kpi-viewer-users");
    if (kpiTotal) kpiTotal.textContent = total;
    if (kpiAdmin) kpiAdmin.textContent = admins;
    if (kpiOp) kpiOp.textContent = operators;
    if (kpiVw) kpiVw.textContent = viewers;

    if (!Array.isArray(users) || users.length === 0) {
      tbody.innerHTML = `<tr><td colspan="6" class="py-6 text-center text-muted-foreground">No accounts found</td></tr>`;
      return;
    }

    users.forEach((u) => {
      const tr = document.createElement("tr");
      tr.className = "hover:bg-muted/40 transition-colors";

      const createdDate = u.created_at ? new Date(u.created_at).toLocaleDateString() : "System Default";
      const isSelf = currentUser && currentUser.username === u.username;

      let roleBadge = `<span class="rounded bg-purple-500/15 text-purple-600 px-2 py-0.5 text-[10px] font-bold">ADMIN</span>`;
      if (u.role === "OPERATOR") roleBadge = `<span class="rounded bg-emerald-500/15 text-emerald-600 px-2 py-0.5 text-[10px] font-bold">OPERATOR</span>`;
      else if (u.role === "VIEWER") roleBadge = `<span class="rounded bg-slate-500/15 text-slate-600 px-2 py-0.5 text-[10px] font-bold">VIEWER</span>`;

      const initial = (u.username || "U").charAt(0).toUpperCase();

      tr.innerHTML = `
        <td class="py-2.5 px-3">
          <div class="flex items-center gap-2.5">
            <div class="flex h-7 w-7 items-center justify-center rounded-full ${u.role === 'ADMIN' ? 'bg-purple-100 text-purple-700' : (u.role === 'OPERATOR' ? 'bg-emerald-100 text-emerald-700' : 'bg-slate-100 text-slate-700')} text-xs font-bold">
              ${initial}
            </div>
            <div>
              <span class="font-mono font-medium text-foreground text-xs">${escapeHtml(u.username)}</span>
              ${isSelf ? '<span class="ml-1 rounded bg-primary/10 text-primary text-[9px] font-bold px-1.5 py-0.5">YOU</span>' : ''}
            </div>
          </div>
        </td>
        <td class="py-2.5 px-3">${roleBadge}</td>
        <td class="py-2.5 px-3">
          <span class="inline-flex items-center gap-1.5 text-xs text-foreground font-medium">
            <span class="h-1.5 w-1.5 rounded-full bg-success"></span>
            Active
          </span>
        </td>
        <td class="py-2.5 px-3 text-muted-foreground font-mono text-[11px]">PBKDF2-HMAC-SHA256 (100k)</td>
        <td class="py-2.5 px-3 text-muted-foreground text-xs">${createdDate}</td>
        <td class="py-2.5 px-3 text-right space-x-1.5">
          <button type="button" class="btn-reset-user-pwd inline-flex items-center gap-1 rounded-md px-2.5 py-1 text-[11px] font-medium bg-secondary text-secondary-foreground hover:bg-accent transition-colors cursor-pointer" data-user="${escapeHtml(u.username)}">
            <svg class="h-3 w-3 text-muted-foreground" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
              <path stroke-linecap="round" stroke-linejoin="round" d="M15 7a2 2 0 012 2m4 0a6 6 0 01-7.743 5.743L11 17H9v2H7v2H4a1 1 0 01-1-1v-2.586a1 1 0 01.293-.707l5.964-5.964A6 6 0 1121 9z" />
            </svg>
            Reset
          </button>
          ${(!isSelf && u.username !== "admin") ? `
          <button type="button" class="btn-delete-user inline-flex items-center gap-1 rounded-md px-2 py-1 text-[11px] font-medium bg-destructive/15 text-destructive hover:bg-destructive/25 transition-colors cursor-pointer" data-user="${escapeHtml(u.username)}">
            <svg class="h-3 w-3" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2">
              <path stroke-linecap="round" stroke-linejoin="round" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
            </svg>
            Delete
          </button>` : ''}
        </td>
      `;
      tbody.appendChild(tr);
    });

    // Reset password button click handlers
    tbody.querySelectorAll(".btn-reset-user-pwd").forEach((btn) => {
      btn.addEventListener("click", () => {
        const target = btn.getAttribute("data-user");
        const targetInput = document.getElementById("reset-pwd-target-user");
        const errBox = document.getElementById("reset-pwd-error");
        const pwdInput = document.getElementById("reset-pwd-new");
        if (targetInput) targetInput.value = target;
        if (pwdInput) pwdInput.value = "";
        if (errBox) {
          errBox.classList.add("hidden");
          errBox.textContent = "";
        }
        openModal("modal-reset-pwd");
      });
    });

    // Delete user button click handlers
    tbody.querySelectorAll(".btn-delete-user").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const target = btn.getAttribute("data-user");
        if (confirm(`Are you sure you want to delete user account '${target}'?`)) {
          await deleteUserAccount(target);
        }
      });
    });

  } catch (err) {
    console.error("Error loading users table:", err);
  }
}

async function deleteUserAccount(username) {
  try {
    const res = await fetch(`/api/v1/users/${encodeURIComponent(username)}`, {
      method: "DELETE"
    });
    const result = await res.json();
    if (res.ok && result.success) {
      showToast(`User '${username}' deleted successfully.`);
      await loadUsersTable();
    } else {
      showToast(result.message || "Failed to delete user.", "error");
    }
  } catch {
    showToast("Network error deleting user.", "error");
  }
}

async function handleSaveNewUser() {
  const uInput = document.getElementById("new-user-name");
  const pInput = document.getElementById("new-user-password");
  const rInput = document.getElementById("new-user-role");
  const errBox = document.getElementById("add-user-error");

  const username = uInput?.value.trim();
  const password = pInput?.value;
  const role = rInput?.value || "VIEWER";

  if (!username || !password) {
    if (errBox) {
      errBox.textContent = "Username and password are required.";
      errBox.classList.remove("hidden");
    }
    return;
  }
  if (password.length < 4) {
    if (errBox) {
      errBox.textContent = "Password must be at least 4 characters long.";
      errBox.classList.remove("hidden");
    }
    return;
  }

  try {
    const res = await fetch("/api/v1/users", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password, role })
    });
    const result = await res.json();
    if (res.ok && result.success) {
      closeModal("modal-add-user");
      if (uInput) uInput.value = "";
      if (pInput) pInput.value = "";
      showToast(`User '${username}' created successfully.`);
      await loadUsersTable();
    } else {
      if (errBox) {
        errBox.textContent = result.message || "Failed to create user.";
        errBox.classList.remove("hidden");
      }
    }
  } catch {
    if (errBox) {
      errBox.textContent = "Network error creating user.";
      errBox.classList.remove("hidden");
    }
  }
}

async function handleSaveResetPassword() {
  const target = document.getElementById("reset-pwd-target-user")?.value;
  const newInput = document.getElementById("reset-pwd-new");
  const errBox = document.getElementById("reset-pwd-error");

  const new_password = newInput?.value;
  if (!target || !new_password) {
    if (errBox) {
      errBox.textContent = "New password is required.";
      errBox.classList.remove("hidden");
    }
    return;
  }

  try {
    const res = await fetch("/api/v1/users/reset_password", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: target, new_password })
    });
    const result = await res.json();
    if (res.ok && result.success) {
      closeModal("modal-reset-pwd");
      if (newInput) newInput.value = "";
      showToast(`Password for '${target}' reset successfully.`);
    } else {
      if (errBox) {
        errBox.textContent = result.message || "Failed to reset password.";
        errBox.classList.remove("hidden");
      }
    }
  } catch {
    if (errBox) {
      errBox.textContent = "Network error resetting password.";
      errBox.classList.remove("hidden");
    }
  }
}

function openEditModal(camId) {
  const cam = cameras.find((c) => c.id === camId);
  if (!cam) return;

  document.getElementById("edit-cam-id").value = cam.id;
  document.getElementById("modal-edit-title").textContent = `Configure ${chLabel(cam.id)}: ${cam.name}`;
  document.getElementById("edit-cam-name").value = cam.name;
  document.getElementById("edit-cam-main").value = cam.rtsp_url || cam.rtsp_url_main || `rtsp://${cam.ip_address}:554/stream1`;
  document.getElementById("edit-cam-sub").value = cam.sub_rtsp_url || cam.rtsp_url_sub || `rtsp://${cam.ip_address}:554/stream2`;
  document.getElementById("edit-cam-user").value = cam.onvif_username || cam.username || "admin";
  document.getElementById("edit-cam-loc").value = cam.location || "";

  openModal("modal-edit-cam");
}

function saveEditModal() {
  const id = Number(document.getElementById("edit-cam-id").value);
  const cam = cameras.find((c) => c.id === id);
  if (!cam) return;

  const mainUrl = document.getElementById("edit-cam-main").value.trim();
  const subUrl = document.getElementById("edit-cam-sub").value.trim();
  const user = document.getElementById("edit-cam-user").value.trim();
  const loc = document.getElementById("edit-cam-loc").value.trim();

  cam.name = document.getElementById("edit-cam-name").value.trim() || cam.name;
  cam.rtsp_url = mainUrl;
  cam.rtsp_url_main = mainUrl;
  cam.sub_rtsp_url = subUrl;
  cam.rtsp_url_sub = subUrl;
  cam.onvif_username = user;
  cam.username = user;
  cam.location = loc;

  localStorage.setItem("nvr_cameras", JSON.stringify(cameras));
  closeModal("modal-edit-cam");
  renderCamerasView();
  renderLiveMatrix();
  showToast(`${chLabel(id)} updated`, "success");

  // Push to backend
  fetch(`/api/v1/cameras/${id}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      id,
      name: cam.name,
      rtsp_url: mainUrl,
      sub_rtsp_url: subUrl,
      onvif_username: user,
      enabled: cam.enabled,
      record_mode: cam.record_mode ?? 1,
    }),
  }).catch(() => {});
}

async function openHwModal(camId) {
  const cam = cameras.find((c) => c.id === camId);
  if (!cam) return;

  document.getElementById("hw-cam-id").value = cam.id;
  document.getElementById("modal-hw-title").textContent = `HW encoder profile — ${chLabel(cam.id)}`;
  document.getElementById("hw-cam-res").value = cam.resolution || "1080p";
  document.getElementById("hw-cam-codec").value = cam.codec || "h264";
  document.getElementById("hw-cam-fps").value = String(cam.fps || 25);

  openModal("modal-hw-cam");

  // Dynamically query camera's live ONVIF encoder options and current config
  try {
    const res = await fetch(`/api/v1/onvif/encoder?channel_id=${camId}`);
    const json = await res.json();
    if (json.success && json.data) {
      const cfg = json.data.config;
      const opts = json.data.options;

      if (opts && Array.isArray(opts.available_resolutions) && opts.available_resolutions.length > 0) {
        const resSelect = document.getElementById("hw-cam-res");
        if (resSelect) {
          const currentVal = cfg ? `${cfg.width}x${cfg.height}` : resSelect.value;
          resSelect.innerHTML = opts.available_resolutions
            .map((r) => `<option value="${r.width}x${r.height}">${r.width}x${r.height}</option>`)
            .join("");
          if (opts.available_resolutions.some((r) => `${r.width}x${r.height}` === currentVal)) {
            resSelect.value = currentVal;
          }
        }
      }

      if (opts && Array.isArray(opts.supported_encodings) && opts.supported_encodings.length > 0) {
        const codecSelect = document.getElementById("hw-cam-codec");
        if (codecSelect) {
          codecSelect.innerHTML = opts.supported_encodings
            .map((e) => `<option value="${e.toLowerCase()}">${e.toUpperCase()}</option>`)
            .join("");
          if (cfg && cfg.encoding) {
            codecSelect.value = cfg.encoding.toLowerCase();
          }
        }
      }

      if (cfg && cfg.fps) {
        const fpsSelect = document.getElementById("hw-cam-fps");
        if (fpsSelect) {
          if (![...fpsSelect.options].some((o) => o.value === String(cfg.fps))) {
            const opt = document.createElement("option");
            opt.value = String(cfg.fps);
            opt.textContent = `${cfg.fps} FPS`;
            fpsSelect.appendChild(opt);
          }
          fpsSelect.value = String(cfg.fps);
        }
      }
    }
  } catch (_) {}
}

async function saveHwModal() {
  const id = Number(document.getElementById("hw-cam-id").value);
  const cam = cameras.find((c) => c.id === id);
  if (!cam) return;

  const targetRes = document.getElementById("hw-cam-res").value;
  const targetCodec = document.getElementById("hw-cam-codec").value;
  const targetFps = Number(document.getElementById("hw-cam-fps").value);

  showToast(`Pushing encoder profile to physical camera ${chLabel(id)} via ONVIF...`, "info");

  try {
    const res = await fetch("/api/v1/onvif/encoder", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        channel_id: id,
        resolution: targetRes,
        codec: targetCodec,
        fps: targetFps,
      }),
    });
    const d = await res.json();
    if (d.success) {
      cam.resolution = d.data?.resolution || targetRes;
      cam.codec = (d.data?.encoding || targetCodec).toLowerCase();
      cam.fps = d.data?.fps || targetFps;
      if (d.data?.bitrate_kbps) cam.bitrate_kbps = d.data.bitrate_kbps;

      closeModal("modal-hw-cam");
      renderCamerasView();
      renderLiveMatrix();
      showToast(`${chLabel(id)} physical hardware encoder updated & stream renegotiated`, "success");
    } else {
      showToast(`Camera hardware update failed: ${d.message || "Camera rejected configuration"}`, "error");
    }
  } catch (err) {
    showToast(`Network error pushing to camera: ${err.message}`, "error");
  }
}

function openAddModal() {
  const freeChannels = [];
  for (let i = 1; i <= MAX_CHANNELS; i++) {
    if (!cameras.some((c) => c.id === i)) freeChannels.push(i);
  }

  if (freeChannels.length === 0) {
    showToast("All 8 channels are occupied", "warning");
    return;
  }

  const chSelect = document.getElementById("add-cam-ch");
  if (chSelect) {
    chSelect.innerHTML = freeChannels.map((c) => `<option value="${c}">${chLabel(c)}</option>`).join("");
  }

  document.getElementById("add-cam-name").value = "";
  document.getElementById("add-cam-main").value = "";
  document.getElementById("add-cam-sub").value = "";
  document.getElementById("add-cam-loc").value = "";

  openModal("modal-add-cam");
}

function submitAddModal() {
  const ch = Number(document.getElementById("add-cam-ch").value);
  const name = document.getElementById("add-cam-name").value.trim();
  const mainUrl = document.getElementById("add-cam-main").value.trim();
  const subUrl = document.getElementById("add-cam-sub").value.trim();
  const user = document.getElementById("add-cam-user").value.trim() || "admin";
  const fps = Number(document.getElementById("add-cam-fps").value);
  const codec = document.getElementById("add-cam-codec").value;
  const loc = document.getElementById("add-cam-loc").value.trim();

  if (!name || !mainUrl) {
    showToast("Camera label and main RTSP URL are required", "error");
    return;
  }

  let ip = "";
  try {
    const parsedUrl = new URL(mainUrl.replace(/^rtsp:\/\//i, "http://"));
    ip = parsedUrl.hostname || "";
  } catch (e) {
    const m = mainUrl.match(/@?([a-zA-Z0-9\.\-_]+)(?::[0-9]+)?\//);
    if (m) ip = m[1];
  }
  if (!ip) {
    const mIp = mainUrl.match(/@?([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)/);
    ip = mIp ? mIp[1] : "";
  }

  const newCam = {
    id: ch,
    name,
    ip_address: ip,
    rtsp_url: mainUrl,
    rtsp_url_main: mainUrl,
    sub_rtsp_url: subUrl || mainUrl,
    rtsp_url_sub: subUrl || null,
    onvif_username: user,
    username: user,
    resolution: "1080p",
    fps,
    bitrate_kbps: 4096,
    codec,
    status: "ONLINE",
    enabled: true,
    recording: true,
    location: loc,
    snapshot_key: `cam${((ch - 1) % 8) + 1}`,
  };

  cameras.push(newCam);
  cameras.sort((a, b) => a.id - b.id);
  localStorage.setItem("nvr_cameras", JSON.stringify(cameras));

  closeModal("modal-add-cam");
  renderCamerasView();
  renderLiveMatrix();
  showToast(`Added ${chLabel(ch)}: ${name}`, "success");

  fetch("/api/v1/cameras", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(newCam),
  }).catch(() => {});
}

function openDeleteModal(camId) {
  deleteTargetCh = camId;
  const cam = cameras.find((c) => c.id === camId);
  const title = document.getElementById("modal-delete-title");
  if (title) title.textContent = `Delete ${chLabel(camId)}: ${cam ? cam.name : "camera"}?`;
  openModal("modal-delete-cam");
}

function confirmDeleteModal() {
  if (!deleteTargetCh) return;
  const id = deleteTargetCh;
  cameras = cameras.filter((c) => c.id !== id);
  localStorage.setItem("nvr_cameras", JSON.stringify(cameras));

  closeModal("modal-delete-cam");
  renderCamerasView();
  renderLiveMatrix();
  showToast(`${chLabel(id)} removed`, "success");

  fetch(`/api/v1/cameras/${id}`, { method: "DELETE" }).catch(() => {});
  deleteTargetCh = null;
}

// --- ONVIF Auto-Discovery Wizard ---
function openOnvifWizard() {
  wizardStep = 1;
  wizardSelectedIp = wizardDevices[0]?.ip || null;
  wizardProbed = null;

  document.getElementById("onvif-step-1")?.classList.remove("hidden");
  document.getElementById("onvif-step-2")?.classList.add("hidden");
  document.getElementById("btn-onvif-back")?.classList.add("hidden");
  document.getElementById("onvif-step-subtitle").textContent = "Step 1 of 2 — network scan & probe";
  document.getElementById("btn-onvif-action").textContent = "Probe device & next";

  renderOnvifDevices();
  openModal("modal-onvif-wizard");

  if (wizardDevices.length === 0) {
    rescanOnvif();
  }
}

function renderOnvifDevices() {
  const container = document.getElementById("onvif-devices-list");
  if (!container) return;

  if (wizardDevices.length === 0) {
    container.innerHTML = `
      <div class="flex flex-col items-center justify-center p-8 text-center text-xs text-muted-foreground">
        <svg class="h-8 w-8 mb-2 text-muted-foreground/60" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
        <span>No ONVIF devices discovered yet on this subnet.</span>
        <span class="mt-1 text-[11px]">Click <strong class="text-foreground">Re-scan network</strong> to broadcast WS-Discovery probes.</span>
      </div>
    `;
    return;
  }

  container.innerHTML = wizardDevices
    .map((d) => {
      const checked = wizardSelectedIp === d.ip;
      return `
      <label class="flex cursor-pointer items-center gap-3 border-b border-border px-3 py-2.5 last:border-b-0 transition-colors hover:bg-secondary ${
        checked ? "bg-accent" : ""
      }">
        <input type="radio" name="onvif-dev" value="${d.ip}" ${checked ? "checked" : ""} class="h-3.5 w-3.5 accent-primary" />
        <span class="font-mono text-xs font-semibold text-foreground">${d.ip}</span>
        <span class="text-xs text-muted-foreground">${d.manufacturer} ${d.model}</span>
        <span class="ml-auto truncate font-mono text-[10px] text-muted-foreground">${d.service}</span>
      </label>
    `;
    })
    .join("");

  container.querySelectorAll("input[type=radio]").forEach((input) => {
    input.addEventListener("change", () => {
      wizardSelectedIp = input.value;
      renderOnvifDevices();
    });
  });
}

async function rescanOnvif() {
  if (wizardScanning) return;
  wizardScanning = true;

  const btnLabel = document.getElementById("label-onvif-scan");
  const spinIcon = document.getElementById("icon-onvif-spin");
  if (btnLabel) btnLabel.textContent = "Scanning...";
  if (spinIcon) spinIcon.classList.add("animate-spin");

  showToast("Broadcasting WS-Discovery probes...", "info");

  try {
    const res = await fetch("/api/v1/onvif/discover");
    if (res.ok) {
      const json = await res.json();
      const list = json.data?.devices || json.devices || (Array.isArray(json) ? json : []);
      if (Array.isArray(list)) {
        wizardDevices = list
          .map((d) => ({
            ip: d.ip || d.xaddrs?.match(/http:\/\/([0-9.]+)/)?.[1] || "",
            port: Number(d.port || 80),
            manufacturer: d.manufacturer || "ONVIF",
            model: d.model || "Network Camera",
            service: d.xaddrs || d.service || `http://${d.ip}/onvif/device_service`,
          }))
          .filter((d) => d.ip);
        wizardSelectedIp = wizardDevices[0]?.ip || null;
      }
    }
  } catch {}

  wizardScanning = false;
  if (btnLabel) btnLabel.textContent = "Re-scan network";
  if (spinIcon) spinIcon.classList.remove("animate-spin");

  renderOnvifDevices();
  showToast(`WS-Discovery complete — ${wizardDevices.length} devices responded`, "success");
}

async function probeAndAdvanceWizard() {
  if (!wizardSelectedIp) {
    showToast("Please select a discovered camera from the list", "warning");
    return;
  }

  const selectedDev = wizardDevices.find((d) => d.ip === wizardSelectedIp);
  const actionBtn = document.getElementById("btn-onvif-action");
  if (actionBtn) {
    actionBtn.textContent = "Probing ONVIF...";
    actionBtn.setAttribute("disabled", "true");
  }

  showToast(`Probing ONVIF services on ${wizardSelectedIp}...`, "info");

  const user = document.getElementById("onvif-user")?.value || "admin";
  const pass = document.getElementById("onvif-pass")?.value || "";

  try {
    const res = await fetch("/api/v1/onvif/probe", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        ip: wizardSelectedIp,
        port: selectedDev?.port || 80,
        username: user,
        password: pass,
      }),
    });
    if (res.ok) {
      const json = await res.json();
      wizardProbed = json.data ?? json;
    }
  } catch {}

  // Move to Step 2
  wizardStep = 2;
  document.getElementById("onvif-step-1")?.classList.add("hidden");
  document.getElementById("onvif-step-2")?.classList.remove("hidden");
  document.getElementById("btn-onvif-back")?.classList.remove("hidden");
  document.getElementById("onvif-step-subtitle").textContent = "Step 2 of 2 — profiles & channel assignment";

  if (actionBtn) {
    actionBtn.textContent = "Commit to ingress engine";
    actionBtn.removeAttribute("disabled");
  }

  // Populate free channels
  const freeChannels = [];
  for (let i = 1; i <= MAX_CHANNELS; i++) {
    if (!cameras.some((c) => c.id === i)) freeChannels.push(i);
  }

  const chSelect = document.getElementById("onvif-assign-ch");
  if (chSelect) {
    chSelect.innerHTML = freeChannels.map((c) => `<option value="${c}">${chLabel(c)}</option>`).join("");
  }

  const devName = selectedDev?.manufacturer ? `${selectedDev.manufacturer} ${wizardSelectedIp.split(".").pop()}` : `Camera ${wizardSelectedIp.split(".").pop()}`;
  const targetLabel = document.getElementById("onvif-probe-target");
  if (targetLabel) targetLabel.textContent = `Device: ${wizardSelectedIp} (${selectedDev?.manufacturer || "ONVIF"})`;

  const nameInput = document.getElementById("onvif-name");
  if (nameInput) nameInput.value = devName;

  // Populate actual probed stream profiles
  const mainProfSelect = document.getElementById("onvif-profile-main");
  const subProfSelect = document.getElementById("onvif-profile-sub");
  if (wizardProbed?.profiles && Array.isArray(wizardProbed.profiles) && wizardProbed.profiles.length > 0) {
    const opts = wizardProbed.profiles.map((p) => {
      const res = p.width && p.height ? `${p.width}x${p.height}` : "";
      const enc = p.encoding ? p.encoding.toUpperCase() : "H264";
      const uri = p.rtsp_uri || "";
      const label = `${p.name || p.token} (${res} ${enc})`;
      return `<option value="${uri}">${label}</option>`;
    }).join("");
    if (mainProfSelect) mainProfSelect.innerHTML = opts;
    if (subProfSelect) {
      subProfSelect.innerHTML = opts;
      if (wizardProbed.profiles.length > 1) {
        subProfSelect.selectedIndex = 1;
      }
    }
  }

  showToast(`ONVIF probed: ${selectedDev?.manufacturer || "Camera"} (${wizardSelectedIp})`, "success");
}

function commitOnvifWizard() {
  const selectedDev = wizardDevices.find((d) => d.ip === wizardSelectedIp);
  const ch = Number(document.getElementById("onvif-assign-ch").value);
  const name = document.getElementById("onvif-name").value.trim() || `${selectedDev?.manufacturer || "Camera"} ${wizardSelectedIp.split(".").pop()}`;
  const user = document.getElementById("onvif-user").value.trim() || "admin";
  const pass = document.getElementById("onvif-pass")?.value.trim() || "";

  const selectedMainProfile = document.getElementById("onvif-profile-main")?.value;
  const selectedSubProfile = document.getElementById("onvif-profile-sub")?.value;

  const mainRtsp = selectedMainProfile || wizardProbed?.main_rtsp_url || `rtsp://${wizardSelectedIp}:554/stream1`;
  const subRtsp = selectedSubProfile || wizardProbed?.sub_rtsp_url || `rtsp://${wizardSelectedIp}:554/stream2`;

  const newCam = {
    id: ch,
    name,
    ip_address: wizardSelectedIp,
    rtsp_url: mainRtsp,
    rtsp_url_main: mainRtsp,
    sub_rtsp_url: subRtsp,
    rtsp_url_sub: subRtsp,
    onvif_username: user,
    username: user,
    onvif_password: pass,
    onvif_xaddr: wizardProbed?.media_xaddr || selectedDev?.xaddr || "",
    resolution: "1080p",
    fps: 25,
    bitrate_kbps: 4096,
    codec: (wizardProbed?.profiles?.[0]?.encoding || "h264").toLowerCase(),
    status: "ONLINE",
    enabled: true,
    recording: true,
    location: "",
    snapshot_key: `cam${((ch - 1) % 8) + 1}`,
  };

  cameras.push(newCam);
  cameras.sort((a, b) => a.id - b.id);
  localStorage.setItem("nvr_cameras", JSON.stringify(cameras));

  closeModal("modal-onvif-wizard");
  renderCamerasView();
  renderLiveMatrix();
  showToast(`${chLabel(ch)} committed to the ingress engine`, "success");

  fetch("/api/v1/cameras", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(newCam),
  }).catch(() => {});
}

// --- Event Binding ---
function setupEventListeners() {
  // Hash routing
  window.addEventListener("hashchange", handleHashChange);

  // Sidebar toggle
  document.getElementById("btn-toggle-rail")?.addEventListener("click", () => {
    sidebarCollapsed = !sidebarCollapsed;
    const rail = document.getElementById("sidebar-rail");
    const label = document.getElementById("label-rail-collapse");
    const icon = document.getElementById("icon-rail-collapse");

    if (sidebarCollapsed) {
      rail?.classList.remove("w-60");
      rail?.classList.add("w-[4.25rem]");
      if (label) label.classList.add("hidden");
      if (icon) icon.innerHTML = '<path stroke-linecap="round" stroke-linejoin="round" d="M13 5l7 7-7 7M5 5l7 7-7 7"/>';
      document.querySelectorAll(".nav-text").forEach((span) => span.classList.add("hidden"));
    } else {
      rail?.classList.remove("w-[4.25rem]");
      rail?.classList.add("w-60");
      if (label) label.classList.remove("hidden");
      if (icon) icon.innerHTML = '<path stroke-linecap="round" stroke-linejoin="round" d="M11 19l-7-7 7-7m8 14l-7-7 7-7"/>';
      document.querySelectorAll(".nav-text").forEach((span) => span.classList.remove("hidden"));
    }
  });

  // Live view pane collapse/expand
  document.getElementById("btn-collapse-cam-pane")?.addEventListener("click", () => {
    setPaneCollapsed(true);
  });

  document.getElementById("btn-expand-cam-pane")?.addEventListener("click", () => {
    setPaneCollapsed(false);
  });

  // Live filter pills
  document.querySelectorAll(".live-filter-pill").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".live-filter-pill").forEach((b) => {
        b.className = "live-filter-pill rounded-full px-3 py-1 text-[11px] font-semibold capitalize transition-colors bg-secondary text-secondary-foreground hover:bg-accent";
      });
      btn.className = "live-filter-pill rounded-full px-3 py-1 text-[11px] font-semibold capitalize transition-colors bg-primary text-primary-foreground";
      liveFilter = btn.dataset.filter;
      renderLiveCameraDir();
    });
  });

  // Live search input
  document.getElementById("live-search-input")?.addEventListener("input", (e) => {
    liveSearch = e.target.value;
    renderLiveCameraDir();
  });

  // Live layout buttons
  document.querySelectorAll(".layout-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const l = btn.dataset.layout === "7plus1" ? "7plus1" : Number(btn.dataset.layout);
      applyLayout(l);
    });
  });


  // Exit fullscreen
  document.getElementById("btn-exit-fullscreen")?.addEventListener("click", exitFullscreenMode);

  // Video element playback synchronization
  const pbVideo = document.getElementById("pb-video");
  if (pbVideo) {
    pbVideo.addEventListener("timeupdate", () => {
      if (!pbActive) return;
      const startMs = getEpochMs(pbActive.start_time_ms || pbActive.start_time);
      pbPlayhead = startMs + Math.floor(pbVideo.currentTime * 1000);
      updatePlayheadUI();

      // Bounded clip playback: stop cleanly when clip finishes
      if (pbClipDuration > 0 && !pbAutoAdvance && !pbLoop) {
        if (pbVideo.currentTime >= pbClipDuration && !pbVideo.paused) {
          pbVideo.pause();
          pbPlaying = false;
          updatePlayPauseUI();
          updatePlayheadUI();
        }
      }
    });
    pbVideo.addEventListener("play", () => {
      pbPlaying = true;
      updatePlayPauseUI();
    });
    pbVideo.addEventListener("pause", () => {
      pbPlaying = false;
      updatePlayPauseUI();
    });
    pbVideo.addEventListener("ended", () => {
      if (pbLoop) {
        pbVideo.currentTime = 0;
        pbVideo.play().catch(() => {});
        return;
      }
      if (pbAutoAdvance) {
        const nextSeg = getConsecutiveSegment(pbActive, true);
        if (nextSeg) {
          startSegmentPlayback(nextSeg, 0);
          return;
        }
      }
      pbPlaying = false;
      pbVideo.pause();
      updatePlayPauseUI();
      updatePlayheadUI();
    });
    pbVideo.addEventListener("error", () => {
      const err = pbVideo.error;
      let msg = "Playback error";
      if (err) {
        if (err.code === 1) msg = "Playback aborted";
        else if (err.code === 2) msg = "Network error while loading video";
        else if (err.code === 3) msg = "Video decode error (codec or corrupted segment)";
        else if (err.code === 4) msg = "Video format or codec not supported by browser";
      }
      console.warn("[Playback Error]", msg, err);
      showToast(msg, "error");
    });
  }

  // Playback audio volume and mute toggle
  document.getElementById("btn-pb-audio")?.addEventListener("click", () => {
    const v = document.getElementById("pb-video");
    pbAudioMuted = !pbAudioMuted;
    if (v) {
      v.muted = pbAudioMuted;
      if (!pbAudioMuted && v.volume === 0) v.volume = 1.0;
    }
    updateAudioUI();
    showToast(pbAudioMuted ? "Audio muted" : "Audio unmuted", "info");
  });

  // Playback search and filter triggers
  document.getElementById("btn-search-recordings")?.addEventListener("click", searchRecordings);
  document.getElementById("pb-channel")?.addEventListener("change", searchRecordings);
  document.getElementById("pb-date")?.addEventListener("change", () => {
    const el = document.getElementById("pb-date");
    if (el) el.dataset.autoSet = "false";
    searchRecordings();
  });
  document.getElementById("pb-trigger")?.addEventListener("change", searchRecordings);

  // Playback view mode switchers
  document.getElementById("btn-pb-mode-cards")?.addEventListener("click", () => setPlaybackViewMode("cards"));
  document.getElementById("btn-pb-mode-player")?.addEventListener("click", () => setPlaybackViewMode("player"));
  document.getElementById("btn-pb-mode-table")?.addEventListener("click", () => setPlaybackViewMode("table"));
  document.getElementById("btn-back-to-cards")?.addEventListener("click", () => setPlaybackViewMode("cards"));



  // Modern Seek Bar scrubbing (matches user reference player)
  const seekContainer = document.getElementById("pb-seek-container");
  function handleSeekbarScrub(e) {
    if (!seekContainer) return;
    const rect = seekContainer.getBoundingClientRect();
    const ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
    const v = document.getElementById("pb-video");
    if (v) {
      const dur = (v.duration && !isNaN(v.duration) && v.duration > 0) ? v.duration : (pbClipDuration || 60);
      v.currentTime = ratio * dur;
      updatePlayheadUI();
    }
  }

  seekContainer?.addEventListener("mousedown", (e) => {
    pbSeekDragging = true;
    handleSeekbarScrub(e);
  });

  window.addEventListener("mousemove", (e) => {
    if (pbSeekDragging) {
      handleSeekbarScrub(e);
    }
  });

  window.addEventListener("mouseup", () => {
    if (pbSeekDragging) {
      pbSeekDragging = false;
    }
  });

  // Top Overlay Back button
  document.getElementById("btn-pb-overlay-back")?.addEventListener("click", () => {
    const v = document.getElementById("pb-video");
    if (v) v.pause();
    pbPlaying = false;
    updatePlayPauseUI();
    setPlaybackViewMode("cards");
  });

  // 10 seconds skip back
  document.getElementById("btn-pb-back10s")?.addEventListener("click", () => {
    const v = document.getElementById("pb-video");
    if (v) {
      v.currentTime = Math.max(0, v.currentTime - 10);
      updatePlayheadUI();
    }
  });

  // 10 seconds skip forward
  document.getElementById("btn-pb-fwd10s")?.addEventListener("click", () => {
    const v = document.getElementById("pb-video");
    if (v) {
      const dur = (v.duration && !isNaN(v.duration) && v.duration > 0) ? v.duration : (pbClipDuration || 60);
      v.currentTime = Math.min(dur, v.currentTime + 10);
      updatePlayheadUI();
    }
  });

  // Play / Pause toggle
  document.getElementById("btn-pb-playpause")?.addEventListener("click", () => {
    const v = document.getElementById("pb-video");
    if (!v) return;
    if (v.paused) {
      v.play().catch(() => {});
    } else {
      v.pause();
    }
  });

  // Layout mode toggles (Single vs 4-Grid Sync)
  document.getElementById("btn-pb-layout-1")?.addEventListener("click", () => {
    pbLayoutMode = "single";
    const btn1 = document.getElementById("btn-pb-layout-1");
    const btn4 = document.getElementById("btn-pb-layout-4");
    if (btn1) {
      btn1.className = "inline-flex h-7 items-center justify-center rounded-md px-2.5 text-xs font-semibold bg-primary text-primary-foreground shadow-sm transition-colors";
    }
    if (btn4) {
      btn4.className = "inline-flex h-7 items-center justify-center rounded-md px-2.5 text-xs font-semibold text-muted-foreground transition-colors hover:text-foreground";
    }
    document.getElementById("pb-sync-grid")?.classList.add("hidden");
    const vid = document.getElementById("pb-video");
    if (vid && pbActive) {
      vid.classList.remove("hidden");
    }
    showToast("Playback layout: Single view", "info");
  });

  document.getElementById("btn-pb-layout-4")?.addEventListener("click", () => {
    pbLayoutMode = "grid4";
    const btn1 = document.getElementById("btn-pb-layout-1");
    const btn4 = document.getElementById("btn-pb-layout-4");
    if (btn4) {
      btn4.className = "inline-flex h-7 items-center justify-center rounded-md px-2.5 text-xs font-semibold bg-primary text-primary-foreground shadow-sm transition-colors";
    }
    if (btn1) {
      btn1.className = "inline-flex h-7 items-center justify-center rounded-md px-2.5 text-xs font-semibold text-muted-foreground transition-colors hover:text-foreground";
    }
    document.getElementById("pb-video")?.classList.add("hidden");
    document.getElementById("pb-sync-grid")?.classList.remove("hidden");
    syncAllChannelsToTime(pbPlayhead);
    showToast("Playback layout: Synchronized 4-grid", "info");
  });

  // Rate buttons
  document.querySelectorAll(".pb-rate-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.querySelectorAll(".pb-rate-btn").forEach((b) => {
        b.className = "pb-rate-btn rounded-md px-2 py-1 text-[11px] font-bold transition-colors text-secondary-foreground hover:bg-accent";
      });
      btn.className = "pb-rate-btn rounded-md px-2 py-1 text-[11px] font-bold transition-colors bg-primary text-primary-foreground";
      pbRate = Number(btn.dataset.rate);
      const rateBadge = document.getElementById("pb-badge-rate");
      if (rateBadge) rateBadge.textContent = `x${pbRate}`;
      const v = document.getElementById("pb-video");
      if (v) v.playbackRate = pbRate;
      for (let ch = 1; ch <= MAX_CHANNELS; ch++) {
        const vid = document.getElementById(`pb-sync-vid-${ch}`);
        if (vid) vid.playbackRate = pbRate;
      }
      showToast(`Playback speed: ${pbRate}x`, "info");
    });
  });

  // Clip Download & Multi-Segment Export Button
  document.getElementById("btn-pb-download")?.addEventListener("click", async () => {
    if (!pbActive) return;

    // If multi-segment review block, use zero-transcoding clip export
    if (pbActive.segment_count && pbActive.segment_count > 1) {
      showToast("Starting multi-segment zero-transcode export...", "info");
      try {
        const startMs = new Date(pbActive.start_time).getTime();
        const endMs = new Date(pbActive.end_time).getTime();
        const exportRes = await fetch("/api/v1/recordings/export", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            channel_id: pbActive.channel_id || 1,
            start_time_ms: startMs,
            end_time_ms: endMs,
            filename: `incident_ch${pbActive.channel_id}_${pbActive.id || "export"}.mp4`
          })
        });
        const exportData = await exportRes.json();
        const jobId = exportData.job_id || (exportData.data && exportData.data.job_id);
        if (jobId) {
          let pollCount = 0;
          const poll = setInterval(async () => {
            pollCount++;
            const stRes = await fetch(`/api/v1/recordings/export/status?job_id=${encodeURIComponent(jobId)}`);
            const stData = await stRes.json();
            const job = stData.data || stData;
            if (job.state === "COMPLETED") {
              clearInterval(poll);
              const a = document.createElement("a");
              a.href = `/api/v1/recordings/export/download?job_id=${encodeURIComponent(jobId)}`;
              a.download = `incident_ch${pbActive.channel_id}_export.mp4`;
              document.body.appendChild(a);
              a.click();
              document.body.removeChild(a);
              showToast("Export completed! Downloading stitched video.", "success");
            } else if (job.state === "FAILED" || pollCount > 30) {
              clearInterval(poll);
              showToast("Clip export failed: " + (job.error || "Timeout"), "error");
            }
          }, 800);
          return;
        }
      } catch (e) {
        console.warn("Export request failed, falling back to direct segment download", e);
      }
    }

    const dlUrl = pbActive.video_url || (pbActive.file_path ? `/api/v1/recordings/video?path=${encodeURIComponent(pbActive.file_path)}` : "");
    if (!dlUrl) {
      showToast("No recording file available to download", "warning");
      return;
    }
    const filename = pbActive.file_path ? pbActive.file_path.split("/").pop() : `recording_ch${pbActive.channel_id}_${pbActive.id}.mp4`;
    const a = document.createElement("a");
    a.href = dlUrl;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    showToast(`Downloading clip: ${filename}`, "success");
  });

  document.getElementById("btn-pb-fullscreen")?.addEventListener("click", () => {
    const stage = document.getElementById("pb-stage-container");
    if (stage?.requestFullscreen) stage.requestFullscreen();
  });

  // Cameras page buttons
  document.getElementById("btn-cam-refresh")?.addEventListener("click", async () => {
    await loadCameras();
    renderCamerasView();
    showToast("Camera ingress sessions refreshed", "success");
  });

  document.getElementById("btn-cam-add-manual")?.addEventListener("click", openAddModal);
  document.getElementById("btn-cam-open-wizard")?.addEventListener("click", openOnvifWizard);

  // Dashboard refresh button
  document.getElementById("btn-dash-refresh")?.addEventListener("click", async () => {
    await fetchTelemetry();
    showToast("Telemetry refreshed", "success");
  });

  // System refresh button
  document.getElementById("btn-system-refresh")?.addEventListener("click", async () => {
    await renderSystemView();
    showToast("Trace telemetry refreshed", "success");
  });

  // Modal close buttons (cancel / backdrop click)
  document.querySelectorAll(".btn-modal-close").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      const overlay = e.target.closest(".modal-overlay");
      if (overlay) overlay.classList.remove("open");
    });
  });

  document.querySelectorAll(".modal-overlay").forEach((overlay) => {
    overlay.addEventListener("click", (e) => {
      if (e.target === overlay) overlay.classList.remove("open");
    });
  });

  // Modal submit actions
  document.getElementById("btn-save-edit-cam")?.addEventListener("click", saveEditModal);
  document.getElementById("btn-save-hw-cam")?.addEventListener("click", saveHwModal);
  document.getElementById("btn-hw-sync-time")?.addEventListener("click", async () => {
    const id = Number(document.getElementById("hw-cam-id").value);
    const cam = cameras.find((c) => c.id === id);
    if (!cam) return;
    showToast(`Synchronizing camera clock for ${chLabel(id)} (${cam.name})...`, "info");
    try {
      const now = new Date();
      const res = await fetch("/api/v1/onvif/datetime", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          channel_id: id,
          xaddr: cam.onvif_xaddr || "",
          username: cam.username || "admin",
          password: cam.password || "",
          target_utc_ms: now.getTime(),
          timezone: "UTC"
        }),
      });
      const d = await res.json();
      if (d.success) {
        showToast(`Clock synchronized with ${chLabel(id)} successfully`, "success");
      } else {
        showToast(`Clock sync failed: ${d.message || "Camera unresponsive"}`, "error");
      }
    } catch (err) {
      showToast(`Clock sync request failed: ${err.message}`, "error");
    }
  });
  document.getElementById("btn-hw-reboot")?.addEventListener("click", async () => {
    const id = Number(document.getElementById("hw-cam-id").value);
    const cam = cameras.find((c) => c.id === id);
    if (!cam) return;
    if (!confirm(`Reboot physical camera for ${chLabel(id)} (${cam.name}) over ONVIF?`)) return;
    showToast(`Sending ONVIF reboot to ${chLabel(id)}...`, "info");
    try {
      const res = await fetch("/api/v1/onvif/reboot", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          channel_id: id,
          xaddr: cam.onvif_xaddr || "",
          username: cam.username || "admin",
          password: cam.password || ""
        }),
      });
      const d = await res.json();
      if (d.success) {
        showToast(`Reboot command dispatched to ${chLabel(id)}`, "success");
        closeModal("modal-hw-cam");
      } else {
        showToast(`Reboot failed: ${d.message || "Camera unresponsive"}`, "error");
      }
    } catch (err) {
      showToast(`Reboot request failed: ${err.message}`, "error");
    }
  });
  document.getElementById("btn-submit-add-cam")?.addEventListener("click", submitAddModal);
  document.getElementById("btn-confirm-delete")?.addEventListener("click", confirmDeleteModal);

  // User Management Wiring
  document.getElementById("btn-add-user-modal")?.addEventListener("click", () => {
    const err = document.getElementById("add-user-error");
    if (err) err.classList.add("hidden");
    document.getElementById("new-user-name").value = "";
    document.getElementById("new-user-password").value = "";
    openModal("modal-add-user");
  });
  document.getElementById("btn-save-new-user")?.addEventListener("click", handleSaveNewUser);
  document.getElementById("btn-save-reset-pwd")?.addEventListener("click", handleSaveResetPassword);
  document.getElementById("btn-users-refresh")?.addEventListener("click", loadUsersTable);
  document.getElementById("users-search-input")?.addEventListener("input", (e) => {
    const q = (e.target.value || "").toLowerCase();
    document.querySelectorAll("#users-table-body tr").forEach((row) => {
      const text = row.textContent.toLowerCase();
      row.style.display = text.includes(q) ? "" : "none";
    });
  });
  document.getElementById("btn-change-storage-path")?.addEventListener("click", () => {
    const err = document.getElementById("change-storage-error");
    if (err) err.classList.add("hidden");
    const curPathEl = document.getElementById("sys-storage-path");
    const mCur = document.getElementById("modal-current-storage-path");
    const input = document.getElementById("input-storage-path");
    const current = (curPathEl?.textContent || "./recordings").replace(" (dedicated disk)", "").trim();
    if (mCur) mCur.textContent = current;
    if (input) input.value = current;
    openModal("modal-change-storage");
  });
  document.getElementById("btn-apply-storage-path")?.addEventListener("click", updateStoragePath);

  // Wizard actions
  document.getElementById("btn-onvif-rescan")?.addEventListener("click", rescanOnvif);
  document.getElementById("btn-onvif-back")?.addEventListener("click", () => {
    wizardStep = 1;
    document.getElementById("onvif-step-2")?.classList.add("hidden");
    document.getElementById("onvif-step-1")?.classList.remove("hidden");
    document.getElementById("btn-onvif-back")?.classList.add("hidden");
    document.getElementById("onvif-step-subtitle").textContent = "Step 1 of 2 — network scan & probe";
    document.getElementById("btn-onvif-action").textContent = "Probe device & next";
  });
  document.getElementById("btn-onvif-action")?.addEventListener("click", () => {
    if (wizardStep === 1) {
      probeAndAdvanceWizard();
    } else {
      commitOnvifWizard();
    }
  });

  // System auto-tail checkbox
  let tailTimer = null;
  const tailCb = document.getElementById("trace-autotail");
  function setupTail() {
    if (tailTimer) clearInterval(tailTimer);
    if (tailCb && tailCb.checked) {
      tailTimer = setInterval(() => {
        if (currentTab === "system") renderSystemView();
      }, 2000);
    }
  }
  tailCb?.addEventListener("change", setupTail);
  setupTail();
}

// --- Initialization ---
let telemetryInterval = null;

async function initApp() {
  startClock();
  setupEventListeners();
  updateUserHeader();

  await loadCameras();
  await fetchTelemetry();
  initPlaybackView();
  if (!telemetryInterval) {
    telemetryInterval = setInterval(fetchTelemetry, 3000);
  }
  handleHashChange();
}

if (document.readyState === "loading") {
  window.addEventListener("DOMContentLoaded", initApp);
} else {
  initApp();
}
