"use strict";

const POLL_MS = 2000;

// Fechas y números con formato español y hora peninsular
const DATE_FORMAT = new Intl.DateTimeFormat("es-ES", {
  timeZone: "Europe/Madrid", day: "2-digit", month: "2-digit", year: "numeric", hour: "2-digit", minute: "2-digit",
});
const formatDate = (unixSeconds) => DATE_FORMAT.format(new Date(unixSeconds * 1000));
const formatNumber = (n) => n.toLocaleString("es-ES");

// Textos de la conexión con Telegram (connectionState* de TDLib)
const CONNECTION = {
  connectionStateReady: ["Conectado", "ok"],
  connectionStateUpdating: ["Actualizando…", "warn"],
  connectionStateConnecting: ["Conectando…", "warn"],
  connectionStateConnectingToProxy: ["Conectando al proxy…", "warn"],
  connectionStateWaitingForNetwork: ["Esperando red…", "err"],
};

// Pasos del inicio de sesión (authorizationState* de TDLib)
const AUTH_STEPS = {
  authorizationStateWaitPhoneNumber: {
    endpoint: "/api/telegram/auth/phone", field: "phone_number",
    text: "Introduce el número de teléfono de tu cuenta de Telegram, con prefijo internacional.",
    label: "Número de teléfono", type: "tel", placeholder: "+34 600 000 000",
  },
  authorizationStateWaitCode: {
    endpoint: "/api/telegram/auth/code", field: "code",
    text: "Telegram te ha enviado un código de inicio de sesión (normalmente a la propia app de Telegram).",
    label: "Código", type: "text", placeholder: "12345", inputmode: "numeric",
  },
  authorizationStateWaitPassword: {
    endpoint: "/api/telegram/auth/password", field: "password",
    text: "Tu cuenta tiene verificación en dos pasos. Introduce tu contraseña.",
    label: "Contraseña", type: "password", placeholder: "",
  },
};

const AUTH_STATUS = {
  authorizationStateWaitTdlibParameters: ["Iniciando…", "warn"],
  authorizationStateWaitPhoneNumber: ["Sin iniciar sesión", "warn"],
  authorizationStateWaitCode: ["Esperando código", "warn"],
  authorizationStateWaitPassword: ["Esperando contraseña", "warn"],
  authorizationStateReady: ["Sesión iniciada", "ok"],
  authorizationStateLoggingOut: ["Cerrando sesión…", "warn"],
  authorizationStateClosing: ["Cerrando…", "warn"],
  authorizationStateClosed: ["Cerrada", "err"],
};

const CHAT_GROUPS = [
  ["channel", "Canales"],
  ["supergroup", "Supergrupos"],
  ["group", "Grupos"],
];

// Errores habituales de Telegram traducidos
function translateError(message) {
  const known = {
    PHONE_NUMBER_INVALID: "El número de teléfono no es válido.",
    PHONE_NUMBER_BANNED: "Este número de teléfono está bloqueado en Telegram.",
    PHONE_CODE_INVALID: "El código no es correcto.",
    PHONE_CODE_EXPIRED: "El código ha caducado. Vuelve a introducir el teléfono.",
    PASSWORD_HASH_INVALID: "La contraseña no es correcta.",
    API_ID_INVALID: "TELEGARRM_API_ID o TELEGARRM_API_HASH no son válidos.",
  };
  if (known[message]) return known[message];
  const flood = /retry after (\d+)/i.exec(message || "");
  if (flood) return `Demasiados intentos. Espera ${flood[1]} s antes de volver a probar.`;
  return message || "Error desconocido.";
}

const $ = (id) => document.getElementById(id);

function setStatus(id, text, cls) {
  const el = $(id);
  el.textContent = text;
  el.className = cls || "";
}

function showError(id, message) {
  $(id).textContent = message;
  $(id).hidden = !message;
}

// fetch con JSON; lanza Error con el mensaje de la API si la respuesta no es 2xx
async function api(path, options = {}) {
  const res = await fetch(path, {
    cache: "no-store",
    ...options,
    headers: options.body ? { "Content-Type": "application/json" } : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Error HTTP ${res.status}`);
  return data;
}

// ---------------------------------------------------------------------------
// Inicio de sesión
// ---------------------------------------------------------------------------

let currentAuthState = null;

function renderAuthForm(state, telegram) {
  const step = AUTH_STEPS[state];
  $("auth-section").hidden = !step;
  if (!step || state === currentAuthState) return;  // No borrar lo que se está escribiendo

  $("auth-text").textContent = step.text;
  $("auth-label").textContent = step.label;
  const input = $("auth-input");
  input.value = "";
  input.type = step.type;
  input.placeholder = step.placeholder;
  if (step.inputmode) input.setAttribute("inputmode", step.inputmode);
  else input.removeAttribute("inputmode");

  const hint = telegram.password_hint;
  $("auth-hint").hidden = !(state === "authorizationStateWaitPassword" && hint);
  $("auth-hint").textContent = hint ? `Pista: ${hint}` : "";
  showError("auth-error", "");
}

$("auth-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const step = AUTH_STEPS[currentAuthState];
  if (!step) return;

  const button = $("auth-submit");
  button.disabled = true;
  showError("auth-error", "");
  try {
    const raw = $("auth-input").value;
    // La contraseña se envía tal cual: puede empezar o acabar en espacio
    const value = step.type === "password" ? raw : raw.trim();
    await api(step.endpoint, { method: "POST", body: JSON.stringify({ [step.field]: value }) });
  } catch (e) {
    showError("auth-error", translateError(e.message));
  } finally {
    button.disabled = false;
    refresh();
  }
});

// ---------------------------------------------------------------------------
// Canales vigilados
// ---------------------------------------------------------------------------

let lastChannelsJson = null;
let chatsLoaded = false;

function channelMeta(channel) {
  const parts = [`${formatNumber(channel.message_count)} mensajes`, `${formatNumber(channel.file_count)} archivos`];
  if (channel.syncing) parts.push("Sincronizando…");
  else if (!channel.history_complete) parts.push("Historial pendiente");
  if (channel.last_sync_at) parts.push(`Última sincronización: ${formatDate(channel.last_sync_at)}`);
  return parts.join(" · ");
}

function renderChannels(channels) {
  const json = JSON.stringify(channels);
  if (json === lastChannelsJson) return;  // Evita rehacer la lista (y los botones) sin cambios
  lastChannelsJson = json;

  $("channels-empty").hidden = channels.length > 0;
  const list = $("channel-list");
  list.replaceChildren();
  for (const channel of channels) {
    const item = document.createElement("li");

    const info = document.createElement("div");
    const title = document.createElement("div");
    title.className = "channel-title";
    title.textContent = channel.title || `Chat ${channel.id}`;
    const meta = document.createElement("div");
    meta.className = "channel-meta";
    meta.textContent = channelMeta(channel);
    info.append(title, meta);

    const actions = document.createElement("div");
    actions.className = "channel-actions";
    const sync = document.createElement("button");
    sync.className = "secondary small";
    sync.textContent = "Sincronizar";
    sync.disabled = channel.syncing;
    sync.addEventListener("click", () => syncChannel(channel.id, sync));
    const remove = document.createElement("button");
    remove.className = "danger small";
    remove.textContent = "Quitar";
    remove.addEventListener("click", () => removeChannel(channel));
    actions.append(sync, remove);

    item.append(info, actions);
    list.append(item);
  }
}

async function loadChats() {
  const select = $("chat-select");
  try {
    const chats = await api("/api/telegram/chats");
    select.replaceChildren();
    const placeholder = new Option("Elige un canal o grupo…", "");
    placeholder.disabled = true;
    placeholder.selected = true;
    select.append(placeholder);

    for (const [type, label] of CHAT_GROUPS) {
      const group = document.createElement("optgroup");
      group.label = label;
      chats
        .filter((chat) => chat.type === type && !chat.watched)
        .sort((a, b) => a.title.localeCompare(b.title, "es", { sensitivity: "base" }))
        .forEach((chat) => group.append(new Option(chat.title, chat.id)));
      if (group.children.length) select.append(group);
    }
    select.disabled = false;
    $("add-submit").disabled = false;
  } catch (e) {
    chatsLoaded = false;  // Se reintenta en el siguiente sondeo
    select.replaceChildren(new Option("No se pudieron cargar los chats", ""));
    showError("add-error", e.message);
  }
}

async function refreshChannels() {
  try {
    renderChannels(await api("/api/channels"));
  } catch (e) {
    // El siguiente sondeo lo reintentará
  }
}

async function syncChannel(id, button) {
  button.disabled = true;
  try {
    await api(`/api/channels/${id}/sync`, { method: "POST" });
  } catch (e) {
    alert(e.message);
  }
  refreshChannels();
}

async function removeChannel(channel) {
  if (!confirm(`¿Dejar de vigilar «${channel.title}»? Se borrarán sus mensajes guardados (no los de Telegram).`)) return;
  try {
    await api(`/api/channels/${channel.id}`, { method: "DELETE" });
  } catch (e) {
    alert(e.message);
  }
  await refreshChannels();
  loadChats();
}

$("add-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const select = $("chat-select");
  if (!select.value) return;

  const button = $("add-submit");
  button.disabled = true;
  showError("add-error", "");
  try {
    // Los chat_id de TDLib caben de sobra en un número de JavaScript (< 2^53)
    await api("/api/channels", { method: "POST", body: JSON.stringify({ chat_id: Number(select.value) }) });
    await refreshChannels();
    await loadChats();
  } catch (e) {
    showError("add-error", e.message);
  } finally {
    button.disabled = false;
  }
});

// ---------------------------------------------------------------------------
// Sondeo del estado
// ---------------------------------------------------------------------------

async function refresh() {
  let data;
  try {
    data = await api("/api/status");
  } catch (e) {
    setStatus("st-service", "Sin respuesta del servidor", "err");
    return;
  }
  $("version").textContent = `v${data.version}`;
  setStatus("st-service", "En marcha", "ok");
  const dbOk = data.database && data.database.status === "ok";
  setStatus("st-db", dbOk ? "Conectada" : "Error", dbOk ? "ok" : "err");

  const tg = data.telegram || {};
  const conn = CONNECTION[tg.connection_state] || [tg.connection_state || "—", ""];
  setStatus("st-connection", conn[0], conn[1]);
  const auth = AUTH_STATUS[tg.authorization_state] || [tg.authorization_state || "—", "warn"];
  setStatus("st-auth", auth[0], auth[1]);

  renderAuthForm(tg.authorization_state, tg);
  currentAuthState = tg.authorization_state;

  const ready = tg.authorization_state === "authorizationStateReady";
  $("channels-section").hidden = !ready;
  if (ready) {
    refreshChannels();
    if (!chatsLoaded) {
      chatsLoaded = true;  // Evita cargas repetidas mientras la primera está en curso
      loadChats();
    }
  }
}

refresh();
setInterval(refresh, POLL_MS);
