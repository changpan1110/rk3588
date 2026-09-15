const form = document.getElementById("configForm");
const adminState = document.getElementById("adminState");
const adminStateText = document.getElementById("adminStateText");
const actionMessage = document.getElementById("actionMessage");
const transport = document.getElementById("transport");
const playerLink = document.getElementById("playerLink");
const openPlayerButton = document.getElementById("openPlayerButton");
const saveButton = document.getElementById("saveButton");
const saveUpdateButton = document.getElementById("saveUpdateButton");
let loadedConfig = null;
let playerUrl = "";
const requestedPlayerOrigin = new URLSearchParams(window.location.search).get("player");

function effectivePlayerUrl() {
  if (requestedPlayerOrigin) {
    try {
      const url = new URL(requestedPlayerOrigin);
      if (url.protocol === "http:" || url.protocol === "https:") {
        return `${url.origin}/`;
      }
    } catch {
      // Fall back to the URL returned by the gateway.
    }
  }
  return playerUrl;
}

function setAdminState(state, message) {
  adminState.dataset.state = state;
  adminStateText.textContent = message;
}

function setActionMessage(message, tone = "") {
  actionMessage.textContent = message;
  actionMessage.dataset.tone = tone;
}

function setValue(id, value) {
  document.getElementById(id).value = value ?? "";
}

function restartFieldLabels(fields) {
  const labels = {
    "web.port": "网页端口",
    "gateway.port": "WebSocket 端口",
    "gateway.debug": "详细控制日志",
    mavlink: "MAVLink 参数",
  };
  return fields.map(field => labels[field] || field).join("、");
}

function fillForm(config) {
  setValue("videoUrl", config.video.url);
  setValue("webPort", config.web.port);
  setValue("gatewayPort", config.gateway.port);
  document.getElementById("gatewayDebug").checked = Boolean(config.gateway.debug);
  setValue("transport", config.mavlink.transport);
  setValue("udpMode", config.mavlink.udp.mode);
  setValue("udpHost", config.mavlink.udp.host);
  setValue("udpPort", config.mavlink.udp.port);
  setValue("tcpMode", config.mavlink.tcp.mode);
  setValue("tcpHost", config.mavlink.tcp.host);
  setValue("tcpPort", config.mavlink.tcp.port);
  setValue("serialDevice", config.mavlink.serial.device);
  setValue("serialBaud", config.mavlink.serial.baud);
  updateTransportPanel();
}

function updateTransportPanel() {
  const selected = transport.value;
  document.querySelectorAll("[data-transport-panel]").forEach(panel => {
    const active = panel.dataset.transportPanel === selected;
    panel.hidden = !active;
    panel.querySelectorAll("input, select").forEach(input => { input.disabled = !active; });
  });
}

function numberValue(id) {
  const value = Number.parseInt(document.getElementById(id).value, 10);
  return Number.isInteger(value) ? value : null;
}

function collectConfig() {
  const next = JSON.parse(JSON.stringify(loadedConfig));
  next.video.url = document.getElementById("videoUrl").value.trim();
  next.web.port = numberValue("webPort");
  next.gateway.port = numberValue("gatewayPort");
  next.gateway.debug = document.getElementById("gatewayDebug").checked;
  next.mavlink.transport = transport.value;
  next.mavlink.udp.mode = document.getElementById("udpMode").value;
  next.mavlink.udp.host = document.getElementById("udpHost").value.trim();
  next.mavlink.udp.port = numberValue("udpPort");
  next.mavlink.tcp.mode = document.getElementById("tcpMode").value;
  next.mavlink.tcp.host = document.getElementById("tcpHost").value.trim();
  next.mavlink.tcp.port = numberValue("tcpPort");
  next.mavlink.serial.device = document.getElementById("serialDevice").value.trim();
  next.mavlink.serial.baud = numberValue("serialBaud");
  return next;
}

async function readError(response) {
  try {
    const body = await response.json();
    return body.detail || body.error || `请求失败：HTTP ${response.status}`;
  } catch {
    return `请求失败：HTTP ${response.status}`;
  }
}

async function loadConfig() {
  setAdminState("loading", "正在读取配置");
  setActionMessage("");
  try {
    const response = await fetch("/api/admin/config", { cache: "no-store" });
    if (!response.ok) throw new Error(await readError(response));
    const payload = await response.json();
    loadedConfig = payload.config;
    playerUrl = payload.player_url;
    playerLink.href = effectivePlayerUrl();
    fillForm(loadedConfig);
    setAdminState("ok", "配置已读取");
  } catch (error) {
    setAdminState("error", "读取失败");
    setActionMessage(error.message || "无法读取配置", "error");
  }
}

function notifyPlayer() {
  const targetUrl = effectivePlayerUrl();
  if (!targetUrl) return;
  const target = new URL(targetUrl);
  const message = { type: "rk3588-config-updated" };
  if (window.opener && !window.opener.closed) {
    window.opener.postMessage(message, target.origin);
    window.opener.focus();
    setActionMessage("已通知播放器刷新配置", "ok");
    return;
  }
  window.open(target.toString(), "rk3588-player");
  setActionMessage("播放器已打开", "ok");
}

async function saveConfig(updatePlayer = false) {
  if (!loadedConfig) return;
  if (!form.reportValidity()) return;
  saveButton.disabled = true;
  saveUpdateButton.disabled = true;
  setAdminState("loading", "正在保存");
  setActionMessage("");
  try {
    const response = await fetch("/api/admin/config", {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(collectConfig()),
    });
    if (!response.ok) throw new Error(await readError(response));
    const payload = await response.json();
    loadedConfig = payload.config;
    playerUrl = payload.player_url;
    playerLink.href = effectivePlayerUrl();
    fillForm(loadedConfig);
    setAdminState("ok", "配置已保存");
    if (payload.restart_required) {
      setActionMessage(`已保存。${restartFieldLabels(payload.restart_fields)}需要重启服务`, "warning");
    } else {
      setActionMessage("已保存，播放器配置已更新", "ok");
    }
    if (updatePlayer) notifyPlayer();
  } catch (error) {
    setAdminState("error", "保存失败");
    setActionMessage(error.message || "无法保存配置", "error");
  } finally {
    saveButton.disabled = false;
    saveUpdateButton.disabled = false;
  }
}

transport.addEventListener("change", updateTransportPanel);
form.addEventListener("submit", event => { event.preventDefault(); saveConfig(false); });
document.getElementById("saveUpdateButton").addEventListener("click", () => saveConfig(true));
document.getElementById("reloadConfigButton").addEventListener("click", loadConfig);
openPlayerButton.addEventListener("click", notifyPlayer);
loadConfig();
