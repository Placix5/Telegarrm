"use strict";

const STATUS_POLL_MS = 2000;
const CATALOG_POLL_MS = 15000;
const DETAIL_POLL_MS = 30000;

// Fechas, números y tamaños con formato español, hora peninsular y unidades del SI (base 1000)
const DATE_FORMAT = new Intl.DateTimeFormat("es-ES", {
  timeZone: "Europe/Madrid", day: "2-digit", month: "2-digit", year: "numeric", hour: "2-digit", minute: "2-digit",
});
const SIZE_FORMAT = new Intl.NumberFormat("es-ES", { maximumFractionDigits: 1 });
const formatDate = (unixSeconds) => DATE_FORMAT.format(new Date(unixSeconds * 1000));
const formatDay = (unixSeconds) => formatDate(unixSeconds).split(",")[0];

// "hace 5 min", "hace 3 h", "ayer", "hace 4 días" o la fecha (días según la hora de Madrid)
function relativeTime(unixSeconds) {
  const now = Date.now() / 1000;
  const seconds = now - unixSeconds;
  if (seconds < 60) return "ahora mismo";
  if (seconds < 3600) return `hace ${Math.floor(seconds / 60)} min`;
  if (formatDay(unixSeconds) === formatDay(now)) return `hace ${Math.floor(seconds / 3600)} h`;
  if (formatDay(unixSeconds) === formatDay(now - 86400)) return "ayer";
  if (seconds < 7 * 86400) return `hace ${Math.ceil(seconds / 86400)} días`;
  return formatDay(unixSeconds);
}
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
const MAX_QUALITY = [["", "La mejor calidad"], ["1080p", "Hasta 1080p"], ["720p", "Hasta 720p"]];
const RECENT_COUNT = 12;     // Obras en «Añadidas recientemente»
const TOAST_MS = 15000;      // Lo que dura un aviso si no se toca

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
// Navegación: #/catalogo, #/catalogo/<chat>/<ficha>, #/descargas, #/actividad, #/canales, #/ajustes, #/estado
// ---------------------------------------------------------------------------

let telegramReady = false;
let routeShown = false;

function currentRoute() {
  const parts = location.hash.replace(/^#\/?/, "").split("/").filter(Boolean);
  if (parts[0] === "canales") return { view: "channels" };
  if (parts[0] === "descargas") return { view: "downloads" };
  if (parts[0] === "actividad") return { view: "activity" };
  if (parts[0] === "ajustes") return { view: "settings" };
  if (parts[0] === "estado") return { view: "status" };
  if (parts[0] === "catalogo" && parts.length === 3) return { view: "detail", chatId: parts[1], anchorId: parts[2] };
  return { view: "catalog" };
}

function showRoute() {
  routeShown = true;
  let route = currentRoute();
  // Sin sesión de Telegram solo tiene sentido la pantalla de estado (inicio de sesión)
  if (!telegramReady && route.view !== "status") route = { view: "status" };

  for (const view of ["catalog", "detail", "downloads", "activity", "channels", "settings", "status"]) {
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
  if (route.view === "downloads") renderDownloads();
  if (route.view === "activity") loadActivity();
  if (route.view === "settings") loadSettings();
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

// "2160p" se muestra como "4K", que es como lo llaman los canales
const qualityLabel = (quality) => (quality === "2160p" ? "4K" : quality || "Calidad desconocida");

// Etiqueta de una versión: "4K HDR · REMUX"
function versionLabel(release) {
  return [qualityLabel(release.quality) + (release.hdr ? " HDR" : ""), ...(release.tags || [])].join(" · ");
}

function itemMeta(item) {
  const parts = [];
  if (item.year) parts.push(item.year);
  parts.push(KIND_LABEL[item.kind] || item.kind);
  if (item.kind === "series") parts.push(plural(item.seasons, "temporada", "temporadas"));
  if (item.qualities.length) parts.push(item.qualities.map(qualityLabel).join(" · "));
  return parts.join(" · ");
}

// Tarjeta de una obra; en «Añadidas recientemente», con cuándo se publicó lo último
function catalogCard(item, withWhen) {
  const card = el("a", "card");
  card.href = `#/catalogo/${itemPath(item)}`;
  const body = el("div", "card-body");
  body.append(el("div", "card-title", item.title), el("div", "card-meta", itemMeta(item)));
  if (withWhen) body.append(el("div", "card-when", relativeTime(item.updated_at)));
  const poster = posterElement(item);
  if (item.airing) poster.append(el("span", "ribbon", "En emisión"));
  if (item.followed) poster.append(el("span", "ribbon follow", "Siguiendo"));
  card.append(poster, body);
  return card;
}

function renderCatalog() {
  const query = normalize($("catalog-search").value.trim());
  const kind = $("catalog-kind").value;
  const byTitle = (a, b) => a.title.localeCompare(b.title, "es", { sensitivity: "base" });
  const sort = $("catalog-sort").value === "recent" ? (a, b) => b.updated_at - a.updated_at || byTitle(a, b) : byTitle;
  const visible = catalogItems
    .filter((item) => !kind || (kind === "airing" ? item.airing : kind === "followed" ? item.followed : item.kind === kind))
    .filter((item) => !query || normalize([item.title, ...item.alternate_titles, item.channel_title, ...item.genres,
      item.tmdb ? item.tmdb.title : "", item.tmdb ? item.tmdb.original_title : ""].join(" ")).includes(query))
    .sort(sort);

  const grid = $("catalog-grid");
  grid.replaceChildren(...visible.map((item) => catalogCard(item, false)));

  // Añadidas recientemente (D-043): lo publicado hace menos tiempo, sin búsqueda y con el filtro de tipo
  const recent = catalogItems
    .filter((item) => !kind || (kind === "airing" ? item.airing : kind === "followed" ? item.followed : item.kind === kind))
    .sort((a, b) => b.updated_at - a.updated_at)
    .slice(0, RECENT_COUNT);
  $("catalog-recent").hidden = Boolean(query) || recent.length === 0;
  $("recent-row").replaceChildren(...recent.map((item) => catalogCard(item, true)));

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
$("catalog-sort").addEventListener("change", renderCatalog);

function episodeLabel(release) {
  const episode = String(release.episode).padStart(2, "0");
  const end = release.episode_end ? `-${String(release.episode_end).padStart(2, "0")}` : "";
  return `${release.season}x${episode}${end}`;
}

// Versiones de un episodio: "1080p · 1,2 GB" y su botón de descarga
// Calidad comprobada en el propio archivo (D-042): la versión con esa calidad; si no, la del nombre
function realRelease(release) {
  const probe = release.probe;
  return probe && probe.quality ? Object.assign({}, release, { quality: probe.quality, hdr: probe.hdr }) : release;
}

const isProbed = (release) => Boolean(release.probe && release.probe.quality);

function renderQuality(node, release) {
  const real = realRelease(release);
  const named = versionLabel(release);
  node.textContent = versionLabel(real);
  if (isProbed(release)) {
    node.title = `Calidad comprobada en el propio archivo${versionLabel(real) !== named ? ` (el nombre decía ${named})` : ""}`;
  } else {
    node.title = release.probe && release.probe.error ? `No se pudo comprobar: ${release.probe.error}`
      : "Calidad según el nombre o la ficha (sin comprobar)";
  }
}

// Etiqueta de versión y, en el mismo hueco, un ✓ si la calidad está comprobada o un botón «?» que la
// comprueba leyendo solo el principio del archivo
function qualityControls(release) {
  const label = el("strong", "quality-label");
  const mark = el("span", "probe-mark", "✓");
  const button = el("button", "probe secondary small", "?");
  const update = () => {
    renderQuality(label, release);
    mark.title = label.title;
    mark.hidden = !isProbed(release);
    button.hidden = isProbed(release);
    button.title = release.probe && release.probe.error
      ? `${label.title}. Pulsa para volver a intentarlo.`
      : "Comprobar la calidad real (lee solo el principio del archivo, sin descargarlo)";
  };
  button.addEventListener("click", async () => {
    button.disabled = true;
    button.textContent = "…";
    detailBusy++;
    try {
      release.probe = await api(`/api/releases/${release.chat_id}/${release.message_id}/probe`, { method: "POST" });
    } catch (e) {
      release.probe = { error: e.message };
    }
    detailBusy--;
    button.disabled = false;
    button.textContent = "?";
    update();
  });
  update();
  return [label, mark, button];
}

// Comprueba de una en una las versiones sin comprobar y después rehace la ficha (agrupa por calidad real)
function probeAllButton(releases, onDone) {
  const pending = releases.filter((release) => !release.probe);
  const button = el("button", "secondary small", "Comprobar calidades");
  button.title = "Lee el principio de cada archivo para saber su calidad real, sin descargarlo";
  button.hidden = pending.length === 0;
  button.addEventListener("click", async () => {
    button.disabled = true;
    detailBusy++;
    for (let i = 0; i < pending.length; i++) {
      button.textContent = `Comprobando ${i + 1} de ${pending.length}…`;
      try {
        pending[i].probe = await api(`/api/releases/${pending[i].chat_id}/${pending[i].message_id}/probe`, { method: "POST" });
      } catch (e) {
        pending[i].probe = { error: e.message };
      }
    }
    detailBusy--;
    onDone();
  });
  return button;
}

function versionsElement(releases) {
  const box = el("div", "versions");
  for (const release of releases) {
    const chip = el("span", "version");
    chip.append(...qualityControls(release), el("span", "", formatSize(release.size)), downloadButton(release, true));
    const parts = release.parts.length > 1 ? ` (${release.parts.length} partes)` : "";
    chip.title = `${release.name}${parts}`;
    box.append(chip);
  }
  return box;
}

// Tabla con su fila de cabecera. kind da nombre al diseño para móviles (ver style.css).
function table(headers, kind) {
  const node = el("table", kind);
  const head = el("tr", "head");
  for (const [text, cls] of headers) head.append(el("th", cls || "", text));
  node.append(head);
  return node;
}

// En pantallas estrechas, una tabla nunca ensancha la página: se desplaza dentro de su caja
function scrollBox(node) {
  const box = el("div", "table-scroll");
  box.append(node);
  return box;
}

// Calidad para mostrar de un vídeo de la biblioteca: "720p", "4K HDR"
const diskLabel = (video) => (video.quality ? qualityLabel(video.quality) + (video.hdr ? " HDR" : "") : "calidad desconocida");

// Episodios que ya están en la biblioteca (D-041): "temporada:episodio" -> calidad
function episodesOnDisk(item) {
  const episodes = new Map();
  for (const video of item.on_disk || []) {
    if (video.episode === null) continue;
    for (let episode = video.episode; episode <= (video.episode_end || video.episode); episode++) {
      episodes.set(`${video.season}:${episode}`, diskLabel(video));
    }
  }
  return episodes;
}

// Series: una fila por episodio con todas sus versiones; título y sinopsis de TMDB si los hay
function episodesTable(releases, onDisk) {
  const node = table([["Episodio"], ["Título"], ["Versiones"]], "episodes");
  const byEpisode = new Map();
  for (const release of releases) {
    const key = episodeLabel(release);
    byEpisode.set(key, [...(byEpisode.get(key) || []), release]);
  }
  for (const [label, versions] of byEpisode) {
    const row = el("tr");
    const titleCell = el("td");
    const title = versions.map((release) => release.episode_title).find(Boolean) || "";
    titleCell.append(el("div", "", title));
    const overview = versions.map((release) => release.episode_overview).find(Boolean);
    if (overview) {
      const text = el("div", "episode-overview", overview);
      text.title = overview;
      titleCell.append(text);
    }
    const stored = onDisk.get(`${versions[0].season}:${versions[0].episode}`);
    if (stored) titleCell.append(el("div", "on-disk", `✓ En tu biblioteca · ${stored}`));
    const cell = el("td");
    cell.append(versionsElement(versions));
    row.append(el("td", "episode", label), titleCell, cell);
    node.append(row);
  }
  return scrollBox(node);
}

// Botones para descargar una temporada entera en una versión: el primer archivo de cada episodio
// con esa versión (los archivos ya vienen ordenados de mejor a peor)
function seasonActions(releases, onProbed) {
  const byVersion = new Map();
  for (const release of releases) {
    const label = versionLabel(realRelease(release));
    const episodes = byVersion.get(label) || new Map();
    if (!episodes.has(episodeLabel(release))) episodes.set(episodeLabel(release), release);
    byVersion.set(label, episodes);
  }
  const box = el("div", "season-actions");
  box.append(el("span", "hint", "Temporada completa:"));
  for (const [label, episodes] of byVersion) {
    const chosen = [...episodes.values()];
    const size = chosen.reduce((sum, release) => sum + release.size, 0);
    // "Almacenar en 1080p (12 GB)"; sin calidad conocida, "Almacenar (calidad desconocida, 12 GB)"
    const text = label.startsWith("Calidad desconocida")
      ? `Almacenar (${label.toLowerCase()}, ${formatSize(size)})` : `Almacenar en ${label} (${formatSize(size)})`;
    const button = el("button", "secondary small", text);
    button.addEventListener("click", async () => {
      const pending = chosen.filter((release) => !activeDownload(release));
      if (!pending.length) return;
      const ok = await confirmDialog({
        title: "Almacenar la temporada",
        text: "Se pondrán en cola los episodios de esta versión que no estén ya descargados ni en la cola.",
        facts: [["Episodios", formatNumber(pending.length)],
          ["Tamaño", formatSize(pending.reduce((sum, release) => sum + release.size, 0))], ["Versión", label]],
        confirmText: "Descargar",
      });
      if (!ok) return;
      button.disabled = true;
      for (const release of pending) await enqueue(release);
      button.disabled = false;
    });
    box.append(button);
  }
  box.append(probeAllButton(releases, onProbed));
  return box;
}

// Películas y archivos sueltos: una fila por versión
function versionsTable(releases) {
  const node = table([["Versión"], ["Archivo"], ["Publicado"], ["Tamaño", "num"], [""]], "versions-table");
  for (const release of releases) {
    const row = el("tr");
    const name = el("td", "", release.name);
    name.title = release.parts.map((part) => part.file_name).join("\n");
    const parts = release.parts.length > 1 ? ` · ${release.parts.length} partes` : "";
    const action = el("td");
    action.append(downloadButton(release, false));
    const version = el("td", "version-cell");
    version.append(...qualityControls(release));
    row.append(version, name, el("td", "", formatDate(release.date).split(",")[0]),
      el("td", "num", formatSize(release.size) + parts), action);
    node.append(row);
  }
  return scrollBox(node);
}

// Ficha abierta: la obra (para los avisos de novedades), su ruta y su JSON (para saber si ha cambiado)
let currentDetail = null;
let detailRoute = null;
let detailJson = null;
let detailBusy = 0;               // Comprobando calidades: no se redibuja hasta que termine
let seriesQualityChoice = null;   // Calidad elegida en «Serie completa» (se conserva al redibujar)
let detailDownloads = "";         // Estado de las descargas de la obra abierta (si cambia, se redibuja)

async function loadDetail(chatId, anchorId) {
  currentDetail = null;  // Hasta que cargue, no es la ficha de ninguna obra
  detailRoute = { chatId, anchorId };
  detailJson = null;
  seriesQualityChoice = null;
  const container = $("detail-content");
  container.replaceChildren(el("p", "hint", "Cargando…"));
  let item;
  try {
    item = await api(`/api/catalog/${chatId}/${anchorId}`);
  } catch (e) {
    container.replaceChildren(el("p", "err", e.message));
    return;
  }
  renderDetail(item);
}

// Vuelve a pedir la ficha abierta y, si algo ha cambiado (descargas que terminan, episodios nuevos,
// calidades comprobadas...), la redibuja en su sitio: sin «Cargando…» ni saltos (D-045)
async function refreshDetail() {
  const route = detailRoute;
  if (!route || !currentDetail || currentRoute().view !== "detail") return;
  let item;
  try {
    item = await api(`/api/catalog/${route.chatId}/${route.anchorId}`);
  } catch (e) {
    return;
  }
  // Mientras tanto no ha cambiado de ficha ni se está usando algo que el redibujo estropearía
  const active = document.activeElement;
  const choosing = active && active.tagName === "SELECT" && $("detail-content").contains(active);
  if (route !== detailRoute || detailBusy || choosing || $("dialog").open) return;
  if (JSON.stringify(item) === detailJson) return;
  const container = $("detail-content");
  const scroll = window.scrollY;
  const details = container.querySelector("details");
  const open = details ? details.open : false;
  renderDetail(item);
  const newDetails = container.querySelector("details");
  if (newDetails) newDetails.open = open;
  window.scrollTo(0, scroll);
}

// Resalta un momento las filas de unos archivos (los que acaban de publicarse)
function flashReleases(messageIds) {
  for (const id of messageIds) {
    const button = document.querySelector(`#detail-content button[data-message="${id}"]`);
    const row = button && button.closest("tr");
    if (row) {
      row.classList.remove("flash");
      void row.offsetWidth;  // Reinicia la animación si ya estaba
      row.classList.add("flash");
      setTimeout(() => row.classList.remove("flash"), 3000);
    }
  }
}

function renderDetail(item) {
  currentDetail = item;
  detailJson = JSON.stringify(item);
  detailDownloads = detailDownloadState();
  const container = $("detail-content");
  const info = el("div");
  const title = el("h2", "", item.title);
  const badges = el("div", "badges");
  if (item.airing) badges.append(el("span", "badge airing", "En emisión"));
  for (const text of [KIND_LABEL[item.kind], item.year, ...item.qualities.map(qualityLabel), item.hdr ? "HDR" : null,
    ...item.languages, ...item.genres]) {
    if (text) badges.append(el("span", "badge", String(text)));
  }
  info.append(title, badges, followControls(item));
  if (item.library) info.append(seriesBox(item));
  const tmdb = item.tmdb;
  if (tmdb && tmdb.original_title && normalize(tmdb.original_title) !== normalize(item.title)) {
    info.append(el("p", "hint", `Título original: ${tmdb.original_title}`));
  }
  if (item.alternate_titles.length) {
    info.append(el("p", "hint", `También: ${item.alternate_titles.join(" · ")}`));
  }
  // Sinopsis de TMDB; si no hay, la de la ficha de Telegram
  const synopsis = item.overview || item.synopsis;
  if (synopsis) info.append(el("p", "synopsis", synopsis));
  const links = el("div", "links");
  const ids = item.external_ids;
  if (ids && tmdb) links.append(externalLink(`https://www.themoviedb.org/${tmdb.type}/${ids.tmdb}`, "Ver en TMDB"));
  if (ids && ids.imdb) links.append(externalLink(`https://www.imdb.com/title/${ids.imdb}/`, "IMDb"));
  if (links.children.length) info.append(links);

  const parts = item.kind === "series"
    ? [plural(item.seasons, "temporada", "temporadas"), plural(item.episodes, "episodio", "episodios")]
    : [plural(item.release_count, "versión", "versiones")];
  parts.push(formatSize(item.total_size));
  info.append(el("p", "", parts.join(" · ")));
  const origin = [`Canal: ${item.channel_title}`];
  if (item.topics.length) origin.push(`Temas: ${item.topics.join(", ")}`);
  info.append(el("p", "hint", origin.join(" · ")));

  if (item.description) {
    const details = el("details");
    details.append(el("summary", "", "Ficha original"), el("div", "description", item.description));
    info.append(details);
  }

  const detail = el("div", "detail");
  detail.append(posterElement(item), info);

  const files = el("div", "files");
  const onDisk = episodesOnDisk(item);
  const reload = () => refreshDetail();
  if (item.kind === "movie" && (item.on_disk || []).length) {
    info.append(el("p", "on-disk", `✓ En tu biblioteca: ${item.on_disk.map(diskLabel).join(", ")}`));
  }
  if (item.kind === "series") {
    const bySeason = new Map();
    const loose = [];
    for (const release of item.releases) {
      if (release.episode === null) loose.push(release);
      else bySeason.set(release.season, [...(bySeason.get(release.season) || []), release]);
    }
    for (const [season, releases] of bySeason) {
      const episodes = new Set(releases.map(episodeLabel)).size;
      const size = releases.reduce((sum, release) => sum + release.size, 0);
      files.append(el("h3", "", `Temporada ${season} · ${plural(episodes, "episodio", "episodios")} · ${formatSize(size)}`),
        seasonActions(releases, reload), episodesTable(releases, onDisk));
    }
    if (loose.length) files.append(el("h3", "", "Otros archivos"), versionsTable(loose));
  } else {
    const actions = el("div", "season-actions");
    actions.append(probeAllButton(item.releases, reload));
    files.append(el("h3", "", "Versiones"), actions, versionsTable(item.releases));
  }

  container.replaceChildren(detail, files);
  document.title = `${item.title} · Telegarrm`;
  updateDownloadButtons();
}

// "mensaje:estado" de las descargas de los archivos de la ficha abierta
function detailDownloadState() {
  if (!currentDetail) return "";
  return currentDetail.releases.map((release) => {
    const download = downloadsByKey.get(releaseKey(release.chat_id, release.message_id));
    return download ? `${release.message_id}:${download.status}` : "";
  }).filter(Boolean).join(",");
}

function qualitySelect(value, onChange) {
  const select = el("select");
  for (const [option, text] of MAX_QUALITY) {
    const node = el("option", "", text);
    node.value = option;
    select.append(node);
  }
  select.value = value;
  select.setAttribute("aria-label", "Calidad máxima");
  select.addEventListener("change", () => onChange(select));
  return select;
}

async function setFollowQuality(follow, select) {
  try {
    const updated = await api(`/api/follows/${follow.id}`, { method: "PUT", body: JSON.stringify({ max_quality: select.value }) });
    follow.max_quality = updated.max_quality;
  } catch (e) {
    toastError(e.message);
    select.value = follow.max_quality;
  }
}

function confirmUnfollow(title) {
  return confirmDialog({
    title: `Dejar de seguir «${title}»`,
    text: "No se descargará nada más de forma automática. Lo que ya tienes se queda en la biblioteca.",
    confirmText: "Dejar de seguir",
    danger: true,
  });
}

// Botón «Seguir» de la ficha, con la calidad máxima y lo que hace
function followControls(item) {
  const box = el("div", "follow-box");
  const series = item.kind === "series";
  const render = () => {
    const follow = item.follow;
    const button = el("button", follow ? "secondary small" : "small", follow ? "✓ Siguiendo" : "Seguir");
    if (follow) button.title = "Dejar de seguir";
    button.addEventListener("click", async () => {
      if (follow && !(await confirmUnfollow(item.title))) return;
      button.disabled = true;
      try {
        if (follow) {
          await api(`/api/follows/${follow.id}`, { method: "DELETE" });
          item.follow = null;
        } else {
          item.follow = await api("/api/follows", {
            method: "POST", body: JSON.stringify({ chat_id: item.chat_id, anchor_id: item.anchor_id, max_quality: "" }),
          });
        }
        lastCatalogJson = null;  // La marca «Siguiendo» del catálogo ha cambiado
      } catch (e) {
        toastError(e.message);
      }
      render();
    });
    box.replaceChildren(button);
    if (follow) box.append(qualitySelect(follow.max_quality, (select) => setFollowQuality(follow, select)));
    const what = series
      ? "los episodios nuevos se descargan solos y, si llega una versión mejor, sustituye a la que tengas."
      : "si se publica una versión nueva, se descarga sola; si ya la tienes, solo las mejores, que sustituyen a la anterior.";
    const text = follow ? `Desde el ${formatDay(follow.created_at)}, ${what}` : `Al seguirla, ${what}`;
    box.append(el("p", "hint", text));
  };
  render();
  return box;
}

// Serie completa (D-040): cuántos episodios se tienen y botón para descargar los que faltan, en la
// mejor versión de cada uno dentro de la calidad elegida
function seriesBox(item) {
  const library = item.library;
  const box = el("div", "series-box");
  if (library.owned >= library.episodes) {
    box.append(el("p", "hint ok", `✓ Tienes los ${plural(library.episodes, "episodio conocido", "episodios conocidos")} (descargados o en cola).`));
    return box;
  }
  const summary = library.owned
    ? `Tienes ${formatNumber(library.owned)} de ${plural(library.episodes, "episodio conocido", "episodios conocidos")} (descargados o en cola).`
    : `${plural(library.episodes, "episodio conocido", "episodios conocidos")}; no tienes ninguno.`;
  const button = el("button", "small", "");
  const select = qualitySelect(seriesQualityChoice !== null ? seriesQualityChoice : item.follow ? item.follow.max_quality : "",
    () => {
      seriesQualityChoice = select.value;
      update();
    });
  const update = () => {
    const missing = library.missing[select.value];
    button.disabled = !missing.episodes;
    if (!missing.episodes) {
      button.textContent = "Los que faltan solo están en más calidad";
      return;
    }
    const size = formatSize(missing.size);
    if (!library.owned) button.textContent = `Descargar la serie completa (${plural(missing.episodes, "episodio", "episodios")}, ${size})`;
    else if (missing.episodes === 1) button.textContent = `Descargar el episodio que falta (${size})`;
    else button.textContent = `Descargar los ${formatNumber(missing.episodes)} episodios que faltan (${size})`;
  };
  button.addEventListener("click", async () => {
    const missing = library.missing[select.value];
    const quality = select.value ? `La mejor versión de cada episodio, hasta ${select.value}`
      : "La mejor versión de cada episodio";
    const facts = [["Episodios", formatNumber(missing.episodes) +
      (missing.files !== missing.episodes ? ` (en ${plural(missing.files, "archivo", "archivos")})` : "")],
    ["Tamaño", formatSize(missing.size)], ["Calidad", quality]];
    if (library.owned) facts.push(["Ya tienes", `${formatNumber(library.owned)} de ${plural(library.episodes, "episodio", "episodios")}`]);
    const ok = await confirmDialog({
      title: library.owned ? "Descargar los episodios que faltan" : "Descargar la serie completa",
      text: `Se pondrán en cola de «${item.title}» los episodios que no tienes. Se descargan de uno en uno y, al terminar cada uno, pasa a la biblioteca.`,
      facts,
      confirmText: "Descargar",
    });
    if (!ok) return;
    button.disabled = true;
    try {
      await api(`/api/catalog/${item.chat_id}/${item.anchor_id}/download`, {
        method: "POST", body: JSON.stringify({ max_quality: select.value }),
      });
    } catch (e) {
      toastError(e.message);
    }
    await refreshDownloads();
    refreshDetail();  // Vuelve a contar lo que falta
  });
  update();
  box.append(el("p", "hint", summary), select, button);
  return box;
}

function externalLink(url, text) {
  const link = el("a", "", text);
  link.href = url;
  link.target = "_blank";
  link.rel = "noopener";
  return link;
}

// ---------------------------------------------------------------------------
// Descargas
// ---------------------------------------------------------------------------

const DOWNLOAD_STATUS = {
  queued: "En cola", downloading: "Descargando", importing: "Importando", completed: "En la biblioteca",
  failed: "Falló", cancelled: "Cancelada", replaced: "Sustituida",
};
const FINISHED = ["completed", "replaced"];
const ACTIVE_STATUSES = ["queued", "downloading", "importing", "completed"];
const IN_PROGRESS = ["queued", "downloading", "importing"];

let downloadList = [];
let downloadsByKey = new Map();  // "chat:mensaje" de la primera parte -> descarga más reciente

const releaseKey = (chatId, messageId) => `${chatId}:${messageId}`;
const percent = (d) => (d.total_size > 0 ? Math.floor((100 * d.downloaded_size) / d.total_size) : 0);

function formatDuration(seconds) {
  if (!isFinite(seconds) || seconds <= 0) return "";
  const minutes = Math.ceil(seconds / 60);
  if (minutes < 60) return `${minutes} min`;
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`;
}

function activeDownload(release) {
  const download = downloadsByKey.get(releaseKey(release.chat_id, release.message_id));
  return download && ACTIVE_STATUSES.includes(download.status) ? download : null;
}

// Botón de descarga de un archivo lógico; su texto sigue el estado de la cola
function downloadButton(release, compact) {
  const button = el("button", compact ? "download" : "download wide", "");
  button.dataset.chat = release.chat_id;
  button.dataset.message = release.message_id;
  button.dataset.compact = compact ? "1" : "";
  button.addEventListener("click", async () => {
    const download = downloadsByKey.get(releaseKey(release.chat_id, release.message_id));
    button.disabled = true;
    if (download && download.status === "failed") await downloadAction(download.id, "retry", "POST");
    else await enqueue(release);
  });
  applyButtonState(button);
  return button;
}

function applyButtonState(button) {
  const download = downloadsByKey.get(releaseKey(button.dataset.chat, button.dataset.message));
  const compact = button.dataset.compact === "1";
  let text = compact ? "Descargar" : "Almacenar en disco";
  let disabled = false;
  if (download && download.status === "queued") { text = "En cola"; disabled = true; }
  if (download && download.status === "downloading") { text = `Descargando ${percent(download)} %`; disabled = true; }
  if (download && download.status === "importing") {
    text = download.import_percent !== null && download.import_percent < 100 ? `Importando ${download.import_percent} %` : "Importando…";
    disabled = true;
  }
  if (download && download.status === "completed") { text = "En la biblioteca"; disabled = true; }
  if (download && download.status === "failed") text = "Reintentar";
  button.textContent = text;
  button.disabled = disabled;
  button.className = `download${compact ? "" : " wide"}${disabled ? " secondary" : ""}`;
  // Mientras descarga, el propio botón muestra el progreso
  button.style.background = download && download.status === "downloading"
    ? `linear-gradient(to right, var(--accent-soft) ${percent(download)}%, transparent ${percent(download)}%)`
    : "";
}

function updateDownloadButtons() {
  document.querySelectorAll("button[data-message]").forEach(applyButtonState);
}

async function enqueue(release) {
  try {
    await api("/api/downloads", { method: "POST", body: JSON.stringify({ chat_id: release.chat_id, message_id: release.message_id }) });
  } catch (e) {
    toastError(e.message);
  }
  await refreshDownloads();
}

async function downloadAction(id, action, method) {
  try {
    await api(action ? `/api/downloads/${id}/${action}` : `/api/downloads/${id}`, { method });
  } catch (e) {
    toastError(e.message);
  }
  await refreshDownloads();
}

function downloadTitle(d) {
  const episode = d.episode ? ` · ${episodeLabel(d)}` : "";
  return `${d.title}${episode}`;
}

function renderDownloads() {
  const list = $("download-list");
  list.replaceChildren();
  $("downloads-empty").hidden = downloadList.length > 0;
  $("downloads-note").hidden = downloadList.length === 0;

  for (const d of downloadList) {
    const item = el("li");
    const head = el("div", "download-head");
    const info = el("div");
    info.append(el("div", "download-title", downloadTitle(d)),
      el("div", "download-meta", `${versionLabel(d)} · ${d.name}`));
    const status = el("div", `status-${d.status}`, DOWNLOAD_STATUS[d.status] || d.status);
    head.append(info, status);
    item.append(head);

    // Mientras importa, la barra muestra la descompresión
    const importing = d.status === "importing";
    const shown = importing ? (d.import_percent === null ? 0 : d.import_percent) : percent(d);
    if (d.status !== "cancelled" && !FINISHED.includes(d.status)) {
      const bar = el("div", "progress");
      const fill = el("div");
      fill.style.width = `${shown}%`;
      bar.append(fill);
      item.append(bar);
    }
    const meta = [];
    if (importing) {
      meta.push(d.import_percent !== null && d.import_percent < 100 ? `Descomprimiendo: ${d.import_percent} %` : "Moviendo a la biblioteca…");
    } else if (!FINISHED.includes(d.status)) {
      meta.push(`${formatSize(d.downloaded_size)} de ${formatSize(d.total_size)} (${percent(d)} %)`);
    } else {
      meta.push(formatSize(d.total_size));
    }
    if (d.status === "downloading" && d.bytes_per_second > 0) {
      meta.push(`${SIZE_FORMAT.format(d.bytes_per_second / 1e6)} MB/s`);
      const left = formatDuration((d.total_size - d.downloaded_size) / d.bytes_per_second);
      if (left) meta.push(`quedan ${left}`);
    }
    meta.push(`añadida el ${formatDate(d.created_at)}${d.origin === "auto" ? " por el seguimiento" : ""}`);
    item.append(el("div", "download-meta", meta.join(" · ")));
    if (d.status === "replaced") item.append(el("div", "download-meta", "Sustituida por una versión mejor"));
    else if (d.library_path) item.append(el("div", "download-meta", `📁 ${d.library_path}`));
    if (d.error) item.append(el("div", "err", d.error));

    const actions = el("div", "channel-actions");
    actions.style.marginTop = "0.5rem";
    const action = (text, cls, handler) => {
      const button = el("button", `${cls} small`, text);
      button.addEventListener("click", handler);
      actions.append(button);
    };
    if (IN_PROGRESS.includes(d.status)) {
      action("Cancelar", "danger", async () => {
        const ok = await confirmDialog({
          title: "Cancelar la descarga",
          text: `${downloadTitle(d)}: se borrará lo descargado hasta ahora.`,
          confirmText: "Cancelar la descarga",
          cancelText: "Seguir descargando",
          danger: true,
        });
        if (ok) downloadAction(d.id, "cancel", "POST");
      });
    }
    if (d.status === "failed" || d.status === "cancelled") action("Reintentar", "secondary", () => downloadAction(d.id, "retry", "POST"));
    if (["completed", "replaced", "failed", "cancelled"].includes(d.status)) action("Quitar de la lista", "secondary", () => downloadAction(d.id, "", "DELETE"));
    if (actions.children.length) item.append(actions);
    list.append(item);
  }
}

async function refreshDownloads() {
  try {
    downloadList = await api("/api/downloads");
  } catch (e) {
    return;
  }
  downloadsByKey = new Map();
  for (const d of downloadList) {  // La lista viene de la más reciente a la más antigua
    const key = releaseKey(d.chat_id, d.message_id);
    if (!downloadsByKey.has(key)) downloadsByKey.set(key, d);
  }
  const active = downloadList.filter((d) => IN_PROGRESS.includes(d.status)).length;
  $("downloads-count").textContent = active;
  $("downloads-count").hidden = active === 0;
  if (currentRoute().view === "downloads") renderDownloads();
  updateDownloadButtons();
  // Si una descarga de la obra abierta cambia de estado (entra en cola, termina, falla...), la ficha se
  // redibuja: «En tu biblioteca», «Tienes X de Y episodios»...
  if (currentRoute().view === "detail" && currentDetail && detailDownloadState() !== detailDownloads) {
    detailDownloads = detailDownloadState();
    refreshDetail();
  }
}

// ---------------------------------------------------------------------------
// Seguimiento y actividad
// ---------------------------------------------------------------------------

// Icono y estilo de cada tipo de entrada del historial
const ACTIVITY_STYLE = {
  queued_episode: ["+", "queued"], queued_movie: ["+", "queued"], queued_upgrade: ["↑", "queued"],
  completed: ["✓", "done"], upgraded: ["↑", "done"], replaced: ["↻", "done"], failed: ["!", "failed"],
  follow: ["●", "user"], unfollow: ["○", "user"],
};

let activityEntries = [];
let activityExpanded = false;  // Con «Ver más» pulsado no se refresca (perdería las páginas cargadas)

function renderFollows(follows) {
  const list = $("follow-list");
  list.replaceChildren();
  $("follows-empty").hidden = follows.length > 0;
  for (const follow of follows) {
    const item = el("li");
    const info = el("div");
    const title = el(follow.found ? "a" : "span", "channel-title", follow.title);
    if (follow.found) title.href = `#/catalogo/${follow.chat_id}/${follow.anchor_id}`;
    const meta = [KIND_LABEL[follow.kind] || follow.kind];
    if (follow.year) meta.push(follow.year);
    if (follow.airing) meta.push("en emisión");
    meta.push(`desde el ${formatDay(follow.created_at)}`);
    if (!follow.found) meta.push("ya no está en el catálogo");
    info.append(title, el("div", "channel-meta", meta.join(" · ")));
    const actions = el("div", "channel-actions");
    const remove = el("button", "danger small", "Dejar de seguir");
    remove.addEventListener("click", async () => {
      if (!(await confirmUnfollow(follow.title))) return;
      try {
        await api(`/api/follows/${follow.id}`, { method: "DELETE" });
        lastCatalogJson = null;
      } catch (e) {
        toastError(e.message);
      }
      loadActivity();
    });
    actions.append(qualitySelect(follow.max_quality, (select) => setFollowQuality(follow, select)), remove);
    item.append(info, actions);
    list.append(item);
  }
}

function renderActivity() {
  const list = $("activity-list");
  list.replaceChildren();
  $("activity-empty").hidden = activityEntries.length > 0;
  for (const entry of activityEntries) {
    const [icon, cls] = ACTIVITY_STYLE[entry.type] || ["·", "user"];
    const item = el("li", `activity-${cls}`);
    const text = el("div", "", entry.message);
    const meta = el("div", "activity-meta", formatDate(entry.at));
    if (entry.item) {
      const link = el("a", "", "Ver ficha");
      link.href = `#/catalogo/${entry.item.chat_id}/${entry.item.anchor_id}`;
      meta.append(" · ", link);
    }
    item.append(el("span", "activity-icon", icon), text, meta);
    list.append(item);
  }
  // Páginas de 100 (ver /api/activity): si la última vino llena, puede haber más
  $("activity-more").hidden = activityEntries.length === 0 || activityEntries.length % 100 !== 0;
}

async function loadActivity() {
  try {
    const [follows, entries] = await Promise.all([api("/api/follows"), activityExpanded ? null : api("/api/activity")]);
    renderFollows(follows);
    if (entries) {
      activityEntries = entries;
      renderActivity();
    }
  } catch (e) {
    $("activity-empty").textContent = `No se pudo cargar la actividad: ${e.message}`;
    $("activity-empty").hidden = false;
  }
}

$("activity-more").addEventListener("click", async () => {
  const last = activityEntries[activityEntries.length - 1];
  try {
    const more = await api(`/api/activity?before=${last.id}`);
    activityExpanded = true;
    activityEntries = activityEntries.concat(more);
    renderActivity();
    if (more.length < 100) $("activity-more").hidden = true;
  } catch (e) {
    toastError(e.message);
  }
});

// ---------------------------------------------------------------------------
// Novedades y avisos emergentes (D-043)
// ---------------------------------------------------------------------------

let lastEventId = null;  // Última novedad vista (null hasta el primer sondeo: lo anterior no se avisa)

// Aviso en la esquina: no roba el foco, se cierra solo y se puede cerrar o seguir su acción
function showToast(title, text, actionText, onAction, kind) {
  const toast = el("div", kind ? `toast ${kind}` : "toast");
  toast.setAttribute("role", "status");
  const body = el("div", "toast-body");
  body.append(el("strong", "", title), el("p", "", text));
  const close = () => {
    clearTimeout(timer);
    toast.classList.add("leaving");
    setTimeout(() => toast.remove(), 200);
  };
  if (actionText) {
    const action = el("button", "small", actionText);
    action.addEventListener("click", () => {
      close();
      onAction();
    });
    body.append(action);
  }
  const closeButton = el("button", "toast-close", "×");
  closeButton.setAttribute("aria-label", "Cerrar aviso");
  closeButton.addEventListener("click", close);
  toast.append(body, closeButton);
  $("toasts").append(toast);
  let timer = setTimeout(close, TOAST_MS);
  // Mientras el ratón está encima no se cierra
  toast.addEventListener("mouseenter", () => clearTimeout(timer));
  toast.addEventListener("mouseleave", () => {
    timer = setTimeout(close, TOAST_MS / 3);
  });
}

// Aviso de error en la esquina (en lugar de los avisos del navegador)
function toastError(message) {
  showToast("No se ha podido completar", message, null, null, "error");
}

// Confirmación con el estilo de la página (D-044), en lugar de la del navegador: título, explicación y
// un resumen de lo que se va a hacer. Devuelve una promesa con true si se acepta. Esc o pulsar fuera cancela.
function confirmDialog({ title, text, facts = [], confirmText = "Aceptar", cancelText = "Cancelar", danger = false }) {
  const dialog = $("dialog");
  if (dialog.open) dialog.close("cancel");  // Uno cada vez: el anterior se da por cancelado
  $("dialog-title").textContent = title;
  const body = $("dialog-body");
  body.replaceChildren();
  if (text) body.append(el("p", "", text));
  if (facts.length) {
    const list = el("dl", "dialog-facts");
    for (const [key, value] of facts) list.append(el("dt", "", key), el("dd", "", value));
    body.append(list);
  }
  const ok = $("dialog-ok");
  ok.textContent = confirmText;
  ok.className = danger ? "danger-solid" : "";
  $("dialog-cancel").textContent = cancelText;
  dialog.returnValue = "";
  return new Promise((resolve) => {
    dialog.addEventListener("close", () => resolve(dialog.returnValue === "ok"), { once: true });
    dialog.showModal();
    // En lo que no tiene vuelta atrás, el foco empieza en «Cancelar»
    (danger ? $("dialog-cancel") : ok).focus();
  });
}

// Pulsar fuera del cuadro (en el fondo) es cancelar
$("dialog").addEventListener("click", (event) => {
  if (event.target === $("dialog")) $("dialog").close("cancel");
});

// "el 1x03 (720p)", "los episodios 1x03 y 1x04", "una versión 4K HDR"
function describeNews(event) {
  const label = (release) => (release.quality ? ` (${versionLabel(release)})` : "");
  const episodes = event.releases.filter((release) => release.episode !== null);
  if (episodes.length === 1) return `Acaba de publicarse el ${episodeLabel(episodes[0])}${label(episodes[0])}.`;
  if (episodes.length > 1 && episodes.length <= 4) {
    const labels = episodes.map(episodeLabel);
    return `Acaban de publicarse los episodios ${labels.slice(0, -1).join(", ")} y ${labels[labels.length - 1]}.`;
  }
  if (episodes.length > 4) return `Acaban de publicarse ${episodes.length} episodios.`;
  if (event.releases.length === 1) {
    return event.releases[0].quality ? `Acaba de publicarse una versión ${versionLabel(event.releases[0])}.`
      : "Acaba de publicarse una versión nueva.";
  }
  return `Acaban de publicarse ${event.releases.length} versiones nuevas.`;
}

// Con cada sondeo del estado: si hay novedades de la obra abierta, se avisa
async function checkEvents(lastId) {
  if (lastEventId === null || lastId < lastEventId) {
    lastEventId = lastId;  // Primera vez o el servicio se ha reiniciado
    return;
  }
  if (lastId === lastEventId) return;
  const after = lastEventId;
  lastEventId = lastId;
  let events;
  try {
    events = await api(`/api/events?after=${after}`);
  } catch (e) {
    return;
  }
  const route = currentRoute();
  for (const event of events) {
    if (route.view === "detail" && currentDetail && event.item.chat_id === currentDetail.chat_id &&
        event.item.anchor_id === currentDetail.anchor_id) {
      await refreshDetail();  // Ya aparece en la ficha, resaltado un momento
      flashReleases(event.releases.map((release) => release.message_id));
      showToast(`Novedad en «${event.item.title}»`, `${describeNews(event)} Ya aparece en la ficha.`);
    }
  }
  if (events.length && route.view === "catalog") loadCatalog();
}

// ---------------------------------------------------------------------------
// Ajustes
// ---------------------------------------------------------------------------

const PATH_FIELDS = ["download_dir", "movies_dir", "series_dir"];

function setFieldStatus(field, text, cls) {
  const node = $(`status-${field}`);
  node.textContent = text || "";
  node.className = `field-status ${cls || ""}`;
}

function renderSettings(data) {
  for (const field of PATH_FIELDS) $(`set-${field}`).value = data[field] || "";
  $("set-download_dir").placeholder = `${data.default_download_dir} (predeterminado)`;
  $("set-min_free_gb").value = Math.round(data.min_free_bytes / 1e9);
  $("set-keep_replaced").checked = data.keep_replaced;

  for (const field of PATH_FIELDS) {
    const check = data.checks[field];
    if (!check) setFieldStatus(field, "Sin definir: la importación no llevará nada aquí hasta que la elijas.", "hint");
    else if (check.ok) setFieldStatus(field, `✓ Se puede escribir · ${formatSize(check.free_bytes)} libres`, "ok");
    else setFieldStatus(field, check.error, "err");
  }
  setFieldStatus("min_free_gb", "");

  const fs = $("settings-filesystem");
  fs.hidden = data.same_filesystem === null;
  fs.className = data.same_filesystem ? "hint ok" : "hint warn";
  fs.textContent = data.same_filesystem
    ? "✓ Llevar lo descargado del búfer a la biblioteca será instantáneo (un simple renombrado)."
    : "⚠ El búfer y las bibliotecas no comparten disco (o están en carpetas autorizadas por separado en el servicio): cada archivo se copiará y se escribirá dos veces. Ponlos bajo una misma carpeta, como /srv/media.";

  $("settings-warning").textContent = data.warning || "";
  $("settings-warning").hidden = !data.warning;
  $("restart-notice").hidden = !data.restart_required;
}

async function loadSettings() {
  try {
    renderSettings(await api("/api/settings"));
  } catch (e) {
    $("settings-warning").textContent = `No se pudieron cargar los ajustes: ${e.message}`;
    $("settings-warning").hidden = false;
  }
}

$("settings-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const message = $("settings-message");
  const button = $("settings-save");
  const body = { min_free_gb: Number($("set-min_free_gb").value), keep_replaced: $("set-keep_replaced").checked };
  for (const field of PATH_FIELDS) body[field] = $(`set-${field}`).value.trim();

  button.disabled = true;
  message.hidden = true;
  try {
    const res = await fetch("/api/settings", {
      method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      for (const [field, error] of Object.entries(data.fields || {})) setFieldStatus(field, error, "err");
      throw new Error(data.error || `Error HTTP ${res.status}`);
    }
    renderSettings(data);
    message.textContent = "Ajustes guardados.";
    message.className = "error-box ok";
  } catch (e) {
    message.textContent = e.message;
    message.className = "error-box err";
  } finally {
    message.hidden = false;
    button.disabled = false;
  }
});

$("restart-button").addEventListener("click", async () => {
  const button = $("restart-button");
  button.disabled = true;
  button.textContent = "Reiniciando…";
  try {
    await api("/api/restart", { method: "POST" });
  } catch (e) {
    // Si el servidor se cierra antes de responder, también vale
  }
  // Esperar a que el servicio vuelva (systemd lo arranca de nuevo en unos segundos)
  for (let attempt = 0; attempt < 60; attempt++) {
    await new Promise((resolve) => setTimeout(resolve, 1000));
    try {
      await api("/api/status");
      break;
    } catch (e) {
      // Aún no ha vuelto
    }
  }
  button.disabled = false;
  button.textContent = "Reiniciar ahora";
  loadSettings();
});

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
    toastError(e.message);
  }
  refreshChannels();
}

async function removeChannel(channel) {
  const ok = await confirmDialog({
    title: `Dejar de vigilar «${channel.title}»`,
    text: "Se borrarán los mensajes que Telegarrm tiene guardados de este canal (no los de Telegram) y sus obras saldrán del catálogo. Lo descargado se queda en la biblioteca.",
    confirmText: "Dejar de vigilar",
    danger: true,
  });
  if (!ok) return;
  try {
    await api(`/api/channels/${channel.id}`, { method: "DELETE" });
  } catch (e) {
    toastError(e.message);
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
  if (ready) refreshDownloads();
  if (data.events) checkEvents(data.events.last_id);
}

refresh();
setInterval(refresh, STATUS_POLL_MS);
setInterval(() => {
  if (telegramReady && currentRoute().view === "catalog") loadCatalog();
  if (telegramReady && currentRoute().view === "activity") loadActivity();
}, CATALOG_POLL_MS);
// La ficha abierta, por si cambia algo que no avisa (ej. un archivo borrado a mano de la biblioteca)
setInterval(() => {
  if (telegramReady && currentRoute().view === "detail") refreshDetail();
}, DETAIL_POLL_MS);
