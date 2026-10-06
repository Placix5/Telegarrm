"use strict";

const STATUS_POLL_MS = 2000;
const CATALOG_POLL_MS = 15000;

// Fechas, números y tamaños con formato español, hora peninsular y unidades del SI (base 1000)
const DATE_FORMAT = new Intl.DateTimeFormat("es-ES", {
  timeZone: "Europe/Madrid", day: "2-digit", month: "2-digit", year: "numeric", hour: "2-digit", minute: "2-digit",
});
const SIZE_FORMAT = new Intl.NumberFormat("es-ES", { maximumFractionDigits: 1 });
const formatDate = (unixSeconds) => DATE_FORMAT.format(new Date(unixSeconds * 1000));
const formatNumber = (n) => n.toLocaleString("es-ES");
function formatSize(bytes) {
  const units = [["TB", 1e12], ["GB", 1e9], ["MB", 1e6], ["kB", 1e3]];
  for (const [unit, factor] of units) {
    if (bytes >= factor) return `${SIZE_FORMAT.format(bytes / factor)} ${unit}`;
  }
  return `${bytes} B`;
}
const plural = (n, one, many) => `${formatNumber(n)} ${n === 1 ? one : many}`;

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

const KIND_LABEL = { series: "Serie", movie: "Película" };

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

// Crea un elemento con clase y texto (el contenido de Telegram nunca pasa por innerHTML)
function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function setStatus(id, text, cls) {
  const node = $(id);
  node.textContent = text;
  node.className = cls || "";
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

// Para buscar sin distinguir mayúsculas ni acentos
const normalize = (text) => (text || "").normalize("NFD").replace(/[̀-ͯ]/g, "").toLowerCase();

// ---------------------------------------------------------------------------
// Navegación: #/catalogo, #/catalogo/<chat>/<ficha>, #/canales, #/estado
// ---------------------------------------------------------------------------

let telegramReady = false;
let routeShown = false;

function currentRoute() {
  const parts = location.hash.replace(/^#\/?/, "").split("/").filter(Boolean);
  if (parts[0] === "canales") return { view: "channels" };
  if (parts[0] === "estado") return { view: "status" };
  if (parts[0] === "catalogo" && parts.length === 3) return { view: "detail", chatId: parts[1], anchorId: parts[2] };
  return { view: "catalog" };
}

function showRoute() {
  routeShown = true;
  let route = currentRoute();
  // Sin sesión de Telegram solo tiene sentido la pantalla de estado (inicio de sesión)
  if (!telegramReady && route.view !== "status") route = { view: "status" };

  for (const view of ["catalog", "detail", "channels", "status"]) {
    $(`view-${view}`).hidden = view !== route.view;
  }
  const navView = route.view === "detail" ? "catalog" : route.view;
  document.querySelectorAll("nav a").forEach((link) => {
    if (link.dataset.view === navView) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  });

  if (route.view !== "detail") document.title = "Telegarrm";
  if (route.view === "catalog") loadCatalog();
  if (route.view === "detail") loadDetail(route.chatId, route.anchorId);
  if (route.view === "channels") {
    refreshChannels();
    if (!chatsLoaded) {
      chatsLoaded = true;  // Evita cargas repetidas mientras la primera está en curso
      loadChats();
    }
  }
}

window.addEventListener("hashchange", showRoute);

// ---------------------------------------------------------------------------
// Catálogo
// ---------------------------------------------------------------------------

let catalogItems = [];
let lastCatalogJson = null;

const itemPath = (item) => `${item.chat_id}/${item.anchor_id}`;
const posterUrl = (item) => `/api/catalog/${itemPath(item)}/poster`;

function posterElement(item) {
  const box = el("div", "poster");
  const initials = el("span", "poster-initials",
    item.title.split(/\s+/).filter(Boolean).slice(0, 2).map((word) => word[0]).join("").toUpperCase());
  if (item.has_poster) {
    const img = el("img");
    img.loading = "lazy";
    img.alt = "";
    img.src = posterUrl(item);
    // Si la portada falla, se quedan las iniciales
    img.addEventListener("error", () => img.replaceWith(initials));
    box.append(img);
  } else {
    box.append(initials);
  }
  return box;
}

function itemMeta(item) {
  const parts = [];
  if (item.year) parts.push(item.year);
  parts.push(KIND_LABEL[item.kind] || item.kind);
  if (item.kind === "series") parts.push(plural(item.seasons, "temporada", "temporadas"));
  if (item.quality) parts.push(item.quality);
  return parts.join(" · ");
}

function renderCatalog() {
  const query = normalize($("catalog-search").value.trim());
  const kind = $("catalog-kind").value;
  const visible = catalogItems
    .filter((item) => !kind || item.kind === kind)
    .filter((item) => !query || normalize([item.title, item.channel_title, ...item.genres].join(" ")).includes(query))
    .sort((a, b) => a.title.localeCompare(b.title, "es", { sensitivity: "base" }));

  const grid = $("catalog-grid");
  grid.replaceChildren();
  for (const item of visible) {
    const card = el("a", "card");
    card.href = `#/catalogo/${itemPath(item)}`;
    const body = el("div", "card-body");
    body.append(el("div", "card-title", item.title), el("div", "card-meta", itemMeta(item)));
    card.append(posterElement(item), body);
    grid.append(card);
  }

  if (!catalogItems.length) {
    $("catalog-summary").textContent =
      "El catálogo está vacío. Añade canales en la pestaña «Canales»; se rellenará al sincronizarlos.";
  } else {
    const total = catalogItems.reduce((sum, item) => sum + item.total_size, 0);
    $("catalog-summary").textContent = `${plural(visible.length, "título", "títulos")} de ${formatNumber(catalogItems.length)} · ${formatSize(total)} en total`;
  }
}

async function loadCatalog() {
  try {
    const items = await api("/api/catalog");
    const json = JSON.stringify(items);
    if (json === lastCatalogJson) return;  // Sin cambios: no se rehacen las tarjetas
    lastCatalogJson = json;
    catalogItems = items;
    renderCatalog();
  } catch (e) {
    $("catalog-summary").textContent = `No se pudo cargar el catálogo: ${e.message}`;
  }
}

$("catalog-search").addEventListener("input", renderCatalog);
$("catalog-kind").addEventListener("change", renderCatalog);

function episodeLabel(file) {
  const episode = String(file.episode).padStart(2, "0");
  const end = file.episode_end ? `-${String(file.episode_end).padStart(2, "0")}` : "";
  return `${file.season}x${episode}${end}`;
}

function filesTable(files, withEpisodes) {
  const table = el("table");
  const head = el("tr");
  if (withEpisodes) head.append(el("th", "", "Episodio"));
  head.append(el("th", "", withEpisodes ? "Título" : "Archivo"), el("th", "num", "Tamaño"));
  table.append(head);
  for (const file of files) {
    const row = el("tr");
    if (withEpisodes) row.append(el("td", "episode", episodeLabel(file)));
    const name = withEpisodes ? (file.episode_title || "") : file.file_name;
    const nameCell = el("td", "", name);
    nameCell.title = file.file_name;  // El nombre original, al pasar el ratón
    row.append(nameCell, el("td", "num", formatSize(file.size)));
    table.append(row);
  }
  return table;
}

async function loadDetail(chatId, anchorId) {
  const container = $("detail-content");
  container.replaceChildren(el("p", "hint", "Cargando…"));
  let item;
  try {
    item = await api(`/api/catalog/${chatId}/${anchorId}`);
  } catch (e) {
    container.replaceChildren(el("p", "err", e.message));
    return;
  }

  const info = el("div");
  const title = el("h2", "", item.title);
  const badges = el("div", "badges");
  for (const text of [KIND_LABEL[item.kind], item.year, item.quality, ...item.languages, ...item.genres]) {
    if (text) badges.append(el("span", "badge", String(text)));
  }
  const parts = item.kind === "series"
    ? [plural(item.seasons, "temporada", "temporadas"), plural(item.episodes, "episodio", "episodios")]
    : [plural(item.file_count, "archivo", "archivos")];
  parts.push(formatSize(item.total_size));
  const summary = el("p", "", parts.join(" · "));
  const channel = el("p", "hint", `Canal: ${item.channel_title}`);
  info.append(title, badges, summary, channel);

  if (item.description) {
    const details = el("details");
    details.append(el("summary", "", "Ficha original"), el("div", "description", item.description));
    info.append(details);
  }

  const detail = el("div", "detail");
  detail.append(posterElement(item), info);

  const files = el("div", "files");
  if (item.kind === "series") {
    const bySeason = new Map();
    const loose = [];
    for (const file of item.files) {
      if (file.episode === null) loose.push(file);
      else bySeason.set(file.season, [...(bySeason.get(file.season) || []), file]);
    }
    for (const [season, seasonFiles] of bySeason) {
      const size = seasonFiles.reduce((sum, file) => sum + file.size, 0);
      files.append(el("h3", "", `Temporada ${season} · ${plural(seasonFiles.length, "archivo", "archivos")} · ${formatSize(size)}`),
        filesTable(seasonFiles, true));
    }
    if (loose.length) files.append(el("h3", "", "Otros archivos"), filesTable(loose, false));
  } else {
    files.append(el("h3", "", "Archivos"), filesTable(item.files, false));
  }

  container.replaceChildren(detail, files);
  document.title = `${item.title} · Telegarrm`;
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
    const item = el("li");
    const info = el("div");
    info.append(el("div", "channel-title", channel.title || `Chat ${channel.id}`),
      el("div", "channel-meta", channelMeta(channel)));

    const actions = el("div", "channel-actions");
    const sync = el("button", "secondary small", "Sincronizar");
    sync.disabled = channel.syncing;
    sync.addEventListener("click", () => syncChannel(channel.id, sync));
    const remove = el("button", "danger small", "Quitar");
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
    chatsLoaded = false;  // Se reintenta la próxima vez que se abra la pestaña
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
  lastCatalogJson = null;  // El catálogo ha cambiado
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
    if (!routeShown) showRoute();
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
  if (ready !== telegramReady || !routeShown) {
    telegramReady = ready;
    showRoute();  // Al iniciar o perder la sesión cambia lo que se puede ver
  }
  if (ready && currentRoute().view === "channels") refreshChannels();
}

refresh();
setInterval(refresh, STATUS_POLL_MS);
setInterval(() => {
  if (telegramReady && currentRoute().view === "catalog") loadCatalog();
}, CATALOG_POLL_MS);
