#include "catalog.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "media_parser.hpp"

namespace {

using Message = DbManager::Message;
using Release = Catalog::Release;

bool isBlank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

// Valor más repetido (el primero en aparecer si hay empate); vacío si no hay ninguno
std::string mostCommon(const std::vector<std::string>& values) {
    std::string best;
    std::size_t bestCount = 0;
    for (const std::string& value : values) {
        if (value.empty()) {
            continue;
        }
        const auto count = static_cast<std::size_t>(std::count(values.begin(), values.end(), value));
        if (count > bestCount) {
            best = value;
            bestCount = count;
        }
    }
    return best;
}

void appendUnique(std::vector<std::string>& values, const std::vector<std::string>& more) {
    for (const std::string& value : more) {
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
            values.push_back(value);
        }
    }
}

// Tipo que sugiere el nombre del tema: "Películas 4K" -> movie, "Series en emisión" -> series
std::string kindFromTopic(const std::string& topicName) {
    const std::string key = media::titleKey(topicName);
    if (key.find("pelicul") != std::string::npos || key.find("movie") != std::string::npos ||
        key.find("cine") != std::string::npos) {
        return "movie";
    }
    if (key.find("serie") != std::string::npos) {
        return "series";
    }
    return "";
}

bool isAiringTopic(const std::string& topicName) {
    return media::titleKey(topicName).find("emision") != std::string::npos;
}

// Nombre para mostrar de un archivo troceado: "Peli (4K HDR).zip" -> "Peli (4K HDR)"
std::string displayName(const std::string& base) {
    static const std::string kExtensions[] = {".zip", ".rar", ".7z", ".mkv", ".mp4", ".avi"};
    for (const std::string& extension : kExtensions) {
        if (base.size() > extension.size() &&
            std::equal(extension.rbegin(), extension.rend(), base.rbegin(),
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                                   std::tolower(static_cast<unsigned char>(b)); })) {
            return base.substr(0, base.size() - extension.size());
        }
    }
    return base;
}

// Resultado del análisis de un mensaje con archivo
struct ParsedFile {
    std::string fileName;  // Si cambia (mensaje editado), se vuelve a analizar
    bool media = false;
    bool archive = false;
    media::PartInfo part;
    std::string quality;
    bool hdr = false;
    std::vector<std::string> tags;
    long tmdbId = 0;
    std::optional<media::EpisodeInfo> episode;
    std::string titleKey;  // Obra a la que parece pertenecer según su nombre ("" = no se sabe)
};

struct ParsedFicha {
    std::string caption;
    media::Ficha ficha;
};

}  // namespace

// Análisis de cada mensaje de un canal. Las expresiones regulares son lo más lento de la
// reconstrucción (unos 4,5 s para 13 000 mensajes en la Pi): cada mensaje se analiza una vez.
struct Catalog::ParseCache {
    std::unordered_map<std::int64_t, ParsedFile> files;
    std::unordered_map<std::int64_t, ParsedFicha> fichas;
    // Las partes de un archivo troceado (sin pie) comparten el análisis de su nombre base
    std::unordered_map<std::string, ParsedFile> bases;

    const ParsedFile& file(const Message& message) {
        const std::string name = message.fileName.value_or("");
        const auto it = files.find(message.messageId);
        if (it != files.end() && it->second.fileName == name) {
            return it->second;
        }
        ParsedFile parsed;
        parsed.media = message.fileSize && media::isMediaFile(name, message.mimeType.value_or(""));
        if (parsed.media) {
            const media::PartInfo part = media::splitParts(name);
            if (part.number > 0 && isBlank(message.text)) {
                const auto base = bases.find(part.base);
                parsed = base != bases.end() ? base->second : (bases[part.base] = analyze(part.base, ""));
            } else {
                parsed = analyze(name, message.text);
            }
            parsed.media = true;
            parsed.archive = media::isArchive(name);
            parsed.part = part;
        }
        parsed.fileName = name;
        return files[message.messageId] = std::move(parsed);
    }

    // Análisis caro (expresiones regulares) de un nombre y su pie
    static ParsedFile analyze(const std::string& name, const std::string& caption) {
        ParsedFile parsed;
        const std::string text = caption.empty() ? name : name + " " + caption;
        parsed.quality = media::detectQuality(text);
        parsed.hdr = media::detectHdr(text);
        parsed.tags = media::detectTags(text);
        parsed.tmdbId = media::detectTmdbId(text).value_or(0);
        // El nombre del fichero manda; si no tiene marcador, se prueba con el pie
        parsed.episode = media::parseEpisode(name);
        if (!parsed.episode && !caption.empty()) {
            parsed.episode = media::parseEpisode(caption);
        }
        parsed.titleKey = parsed.episode ? media::titleKey(parsed.episode->seriesName)
                                         : media::titleKey(media::cleanTitle(media::splitParts(name).base));
        return parsed;
    }

    const media::Ficha& ficha(const Message& message) {
        const auto it = fichas.find(message.messageId);
        if (it != fichas.end() && it->second.caption == message.text) {
            return it->second.ficha;
        }
        return (fichas[message.messageId] = ParsedFicha{message.text, media::parseFicha(message.text)}).ficha;
    }
};

// Una ficha y los archivos de su tema que la siguen
struct Catalog::Block {
    std::int64_t chatId = 0;
    std::int64_t anchor = 0;  // La ficha; si no hay ficha, el primer archivo
    std::int64_t topicId = 0;
    std::string topicName;
    std::string channelTitle;
    bool hasFicha = false;
    std::int64_t poster = 0;
    std::string caption;
    media::Ficha ficha;
    std::vector<Release> releases;
    std::vector<std::string> fileSeriesNames;  // Nombre de la serie según cada archivo
    std::vector<std::string> matchKeys;        // Títulos con los que debe encajar un archivo para unirse
    // Calculados al cerrar el bloque
    std::string kind;
    std::string title;
    std::vector<std::string> keys;  // Claves de título (principal y alternativos) para unir bloques
};

namespace {

using Block = Catalog::Block;
using Item = Catalog::Item;

bool keysCompatible(const std::string& a, const std::string& b);

// "1x01 - Ultimate Spiderman.mkv", "1x02 - Ultimate Spiderman.mkv"...: el texto tras el marcador
// es el nombre de la serie si se repite en más de la mitad de los episodios (y en dos como mínimo;
// con un solo episodio no se puede saber si es la serie o el título del episodio)
std::string repeatedEpisodeTitle(const std::vector<Release>& releases) {
    std::map<std::string, std::pair<int, std::string>> counts;  // Clave -> (episodios, texto original)
    int episodes = 0;
    for (const Release& release : releases) {
        if (release.episode == 0) {
            continue;
        }
        ++episodes;
        const std::string key = media::titleKey(release.episodeTitle);
        if (!key.empty()) {
            auto& entry = counts[key];
            if (entry.first++ == 0) {
                entry.second = release.episodeTitle;
            }
        }
    }
    for (const auto& [key, entry] : counts) {
        if (entry.first >= 2 && entry.first * 2 > episodes) {
            return entry.second;
        }
    }
    return "";
}

void finishBlock(Block& block) {
    const media::Ficha& ficha = block.ficha;
    for (Release& release : block.releases) {
        // Las partes se publican a veces desordenadas (part06, part05...): se ordenan por número
        std::stable_sort(release.parts.begin(), release.parts.end(), [](const Catalog::Part& a, const Catalog::Part& b) {
            return std::make_pair(a.number, a.messageId) < std::make_pair(b.number, b.messageId);
        });
        // Fichas de series en emisión: "Temporada 4" + "Episodio 8" y archivos sin marcador
        if (release.episode == 0 && ficha.episode > 0) {
            release.season = ficha.season > 0 ? ficha.season : 1;
            release.episode = ficha.episode;
            release.episodeEnd = ficha.episodeEnd;
        }
        if (release.quality.empty()) {
            release.quality = ficha.quality;
        }
        release.hdr = release.hdr || ficha.hdr;
    }

    const bool hasEpisodes = std::any_of(block.releases.begin(), block.releases.end(),
                                         [](const Release& release) { return release.episode > 0; });
    const std::string hint = kindFromTopic(block.topicName);
    block.kind = (hasEpisodes || ficha.season > 0) ? "series" : (hint.empty() ? "movie" : hint);

    // Título: ficha > nombre en los ficheros > (película) nombre del fichero > título del canal
    block.title = ficha.title;
    if (block.title.empty()) {
        block.title = mostCommon(block.fileSeriesNames);
    }
    if (block.title.empty() && block.kind == "movie" && !block.releases.empty()) {
        block.title = media::cleanTitle(block.releases.front().name);
    }
    if (block.title.empty() && block.kind == "series") {
        // Sin ficha ni nombre de serie en los ficheros: el nombre repetido tras el marcador de episodio.
        // Si el canal se llama igual, su título suele estar mejor escrito ("Spider-Man").
        const std::string repeated = repeatedEpisodeTitle(block.releases);
        if (!repeated.empty() &&
            !keysCompatible(media::titleKey(repeated), media::titleKey(media::cleanTitle(block.channelTitle)))) {
            block.title = repeated;
        }
    }
    if (block.title.empty()) {
        block.title = media::cleanTitle(block.channelTitle);
    }
    if (block.title.empty()) {
        block.title = block.channelTitle;
    }

    std::vector<std::string> keys = {media::titleKey(block.title)};
    for (const std::string& alternate : ficha.alternateTitles) {
        keys.push_back(media::titleKey(alternate));
    }
    appendUnique(block.keys, keys);
}

// Dos claves de título encajan si son iguales o una contiene a la otra ("vigilantes" en
// "myheroacademiavigilantes"). Con claves muy cortas solo vale como prefijo ("rec" en "rec2").
// Sin clave (archivo "1x01 - ...") no se puede saber: se acepta.
bool keysCompatible(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a == b) {
        return true;
    }
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    if (shorter.size() >= 4) {
        return longer.find(shorter) != std::string::npos;
    }
    return longer.compare(0, shorter.size(), shorter) == 0;
}

// ¿El archivo pertenece al bloque abierto? Sí si es el primero tras la ficha o si su nombre
// encaja con el título. Si no, es otra obra subida sin ficha. (Las partes de un mismo archivo
// troceado se reconocen antes, por su nombre base.) El álbum no sirve: un mismo álbum puede
// llevar las últimas partes de una película y las primeras de la siguiente.
bool belongsToBlock(const Block& block, const ParsedFile& parsed) {
    // Sin nombre ("1x02 - ...") no se puede atribuir a otra obra: sigue en la abierta
    if (block.releases.empty() || parsed.titleKey.empty()) {
        return true;
    }
    return std::any_of(block.matchKeys.begin(), block.matchKeys.end(),
                       [&parsed](const std::string& key) { return keysCompatible(key, parsed.titleKey); });
}

std::vector<Block> buildBlocks(std::int64_t chatId, const std::string& channelTitle,
                               const std::vector<Message>& messages, const std::vector<DbManager::Topic>& topics,
                               Catalog::ParseCache& cache) {
    std::map<std::int64_t, std::string> topicNames;
    for (const DbManager::Topic& topic : topics) {
        topicNames[topic.id] = topic.name;
    }

    // Los temas de un foro se publican intercalados: las fichas se agrupan dentro de cada tema
    std::map<std::int64_t, std::vector<const Message*>> byTopic;
    for (const Message& message : messages) {
        byTopic[message.topicId].push_back(&message);
    }

    std::vector<Block> blocks;
    for (const auto& [topicId, topicMessages] : byTopic) {
        std::vector<Block> topicBlocks(1);  // Bloque inicial para los archivos anteriores a la primera ficha
        std::map<std::string, std::size_t> partIndex;  // Partes ya vistas del bloque actual -> su Release

        for (const Message* message : topicMessages) {
            if (message->contentType == "messagePhoto") {
                if (!isBlank(message->text)) {
                    // Ficha: abre un bloque nuevo (o adopta el inicial si aún está vacío)
                    if (topicBlocks.back().hasFicha || !topicBlocks.back().releases.empty()) {
                        topicBlocks.emplace_back();
                    }
                    Block& block = topicBlocks.back();
                    block.hasFicha = true;
                    block.anchor = message->messageId;
                    block.poster = message->messageId;
                    block.caption = message->text;
                    block.ficha = cache.ficha(*message);
                    block.matchKeys = {media::titleKey(block.ficha.title)};
                    for (const std::string& alternate : block.ficha.alternateTitles) {
                        block.matchKeys.push_back(media::titleKey(alternate));
                    }
                    partIndex.clear();
                } else if (topicBlocks.back().poster == 0) {
                    // Foto sin pie (ej. portada de temporada): no abre bloque; como mucho, sirve de portada
                    topicBlocks.back().poster = message->messageId;
                }
                continue;
            }

            const ParsedFile& parsed = cache.file(*message);
            if (!parsed.media) {
                continue;  // Textos sueltos ("FIN DE SERIE", créditos, enlaces) no cambian de bloque
            }
            // Las partes de un mismo archivo troceado se juntan en un solo Release
            Release* release = nullptr;
            if (parsed.part.number > 0) {
                const auto it = partIndex.find(parsed.part.base);
                if (it != partIndex.end()) {
                    release = &topicBlocks.back().releases[it->second];
                }
            }
            // Una obra subida sin ficha abre su propio bloque (luego se une con su ficha por título)
            if (!release && !belongsToBlock(topicBlocks.back(), parsed)) {
                topicBlocks.emplace_back();
                partIndex.clear();
            }
            Block& block = topicBlocks.back();
            if (block.anchor == 0) {
                block.anchor = message->messageId;
            }
            // El primer archivo es del bloque aunque se llame distinto que la ficha ("The Crow" para
            // "El Cuervo"): su nombre pasa a valer también para los siguientes
            if (block.releases.empty() && !parsed.titleKey.empty()) {
                appendUnique(block.matchKeys, {parsed.titleKey});
            }
            if (!release) {
                block.releases.emplace_back();
                release = &block.releases.back();
                release->chatId = chatId;
                release->anchorMessageId = block.anchor;
                release->topicId = topicId;
                release->name = displayName(parsed.part.base);
                release->archive = parsed.archive;
                release->tags = parsed.tags;
                release->tmdbId = parsed.tmdbId;
                if (parsed.episode) {
                    release->season = parsed.episode->season;
                    release->episode = parsed.episode->episode;
                    release->episodeEnd = parsed.episode->episodeEnd;
                    release->episodeTitle = parsed.episode->episodeTitle;
                    block.fileSeriesNames.push_back(parsed.episode->seriesName);
                }
                if (parsed.part.number > 0) {
                    partIndex[parsed.part.base] = block.releases.size() - 1;
                }
            }
            const std::int64_t size = message->fileSize.value_or(0);
            release->parts.push_back({message->messageId, message->fileName.value_or(""), size, parsed.part.number});
            release->size += size;
            release->date = std::max(release->date, message->date);
            if (release->quality.empty()) {
                release->quality = parsed.quality;
            }
            release->hdr = release->hdr || parsed.hdr;
        }

        for (Block& block : topicBlocks) {
            if (block.releases.empty()) {
                continue;
            }
            block.chatId = chatId;
            block.topicId = topicId;
            const auto name = topicNames.find(topicId);
            block.topicName = name != topicNames.end() ? name->second : "";
            block.channelTitle = channelTitle;
            finishBlock(block);
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

std::optional<int> mostCommonYear(const std::vector<const Block*>& group) {
    std::map<int, int> counts;
    for (const Block* block : group) {
        if (block->ficha.year) {
            ++counts[*block->ficha.year];
        }
    }
    std::optional<int> best;
    int bestCount = 0;
    for (const auto& [year, count] : counts) {
        if (count > bestCount) {
            best = year;
            bestCount = count;
        }
    }
    return best;
}

Item buildItem(std::vector<const Block*> group) {
    // El bloque más antiguo identifica la obra y aporta los datos principales
    std::sort(group.begin(), group.end(), [](const Block* a, const Block* b) {
        return std::make_pair(a->chatId, a->anchor) < std::make_pair(b->chatId, b->anchor);
    });
    const Block* first = group.front();
    const auto withFicha = std::find_if(group.begin(), group.end(), [](const Block* b) { return b->hasFicha; });
    const Block* main = withFicha != group.end() ? *withFicha : first;

    Item item;
    item.chatId = first->chatId;
    item.anchorMessageId = first->anchor;
    item.kind = first->kind;
    item.title = main->title;
    item.year = mostCommonYear(group);
    item.channelTitle = first->channelTitle;
    item.description = main->hasFicha ? main->caption : "";

    const std::string titleKey = media::titleKey(item.title);
    for (const Block* block : group) {
        item.blockIds.emplace_back(block->chatId, block->anchor);
        if (media::titleKey(block->title) != titleKey) {
            appendUnique(item.alternateTitles, {block->title});
        }
        for (const std::string& alternate : block->ficha.alternateTitles) {
            if (media::titleKey(alternate) != titleKey) {
                appendUnique(item.alternateTitles, {alternate});
            }
        }
        appendUnique(item.languages, block->ficha.languages);
        appendUnique(item.genres, block->ficha.genres);
        appendUnique(item.topics, {block->topicName});
        item.airing = item.airing || isAiringTopic(block->topicName);
        if (item.synopsis.empty()) {
            item.synopsis = block->ficha.synopsis;
        }
        if (item.posterMessageId == 0 && block->poster != 0) {
            item.posterChatId = block->chatId;
            item.posterMessageId = block->poster;
        }
        item.releases.insert(item.releases.end(), block->releases.begin(), block->releases.end());
    }
    for (const std::string& language : media::detectLanguages(first->channelTitle)) {
        appendUnique(item.languages, {language});
    }

    if (item.kind == "series") {
        // Un "título de episodio" repetido en más de la mitad de los episodios, o igual al de la
        // serie, es en realidad el nombre de la serie ("1x01 - Ultimate Spiderman.mkv")
        std::vector<std::string> titles;
        for (const Release& release : item.releases) {
            if (release.episode > 0) {
                titles.push_back(media::titleKey(release.episodeTitle));
            }
        }
        std::set<std::string> seriesKeys = {titleKey};
        for (const std::string& alternate : item.alternateTitles) {
            seriesKeys.insert(media::titleKey(alternate));
        }
        for (Release& release : item.releases) {
            const std::string key = media::titleKey(release.episodeTitle);
            const auto repeated = static_cast<std::size_t>(std::count(titles.begin(), titles.end(), key));
            if (seriesKeys.count(key) || (titles.size() > 1 && repeated * 2 > titles.size())) {
                release.episodeTitle.clear();
            }
        }
    }

    // Series: por temporada y episodio (sin marcador al final), y la mejor calidad primero.
    // Películas: la mejor calidad primero. A igualdad, por orden de publicación.
    std::stable_sort(item.releases.begin(), item.releases.end(), [](const Release& a, const Release& b) {
        const auto key = [](const Release& r) {
            return std::make_tuple(r.episode == 0, r.season, r.episode, -media::qualityRank(r.quality), !r.hdr,
                                   r.parts.front().messageId);
        };
        return key(a) < key(b);
    });

    std::set<int> seasons;
    std::set<std::pair<int, int>> episodes;
    std::vector<std::string> qualities;
    for (const Release& release : item.releases) {
        item.totalSize += release.size;
        item.updatedAt = std::max(item.updatedAt, release.date);
        appendUnique(qualities, {release.quality});
        if (release.episode > 0) {
            seasons.insert(release.season);
            for (int episode = release.episode; episode <= std::max(release.episode, release.episodeEnd); ++episode) {
                episodes.emplace(release.season, episode);
            }
        }
    }
    std::stable_sort(qualities.begin(), qualities.end(), [](const std::string& a, const std::string& b) {
        return media::qualityRank(a) > media::qualityRank(b);
    });

    // Identificador de TMDB y, en películas sin año en la ficha, año según los nombres de fichero
    std::map<long, int> tmdbCounts;
    std::map<int, int> fileYears;
    for (const Release& release : item.releases) {
        if (release.tmdbId) {
            ++tmdbCounts[release.tmdbId];
        }
        if (const auto year = media::detectYear(release.name)) {
            ++fileYears[*year];
        }
    }
    const auto byCount = [](const auto& a, const auto& b) { return a.second < b.second; };
    if (!tmdbCounts.empty()) {
        item.tmdbId = std::max_element(tmdbCounts.begin(), tmdbCounts.end(), byCount)->first;
    }
    if (!item.year && item.kind == "movie" && !fileYears.empty()) {
        item.year = std::max_element(fileYears.begin(), fileYears.end(), byCount)->first;
    }
    item.qualities = qualities;
    item.seasonCount = static_cast<int>(seasons.size());
    item.episodeCount = static_cast<int>(episodes.size());
    return item;
}

// Une en obras los bloques con el mismo tipo y la misma clave de título (o alternativa)
std::vector<Item> mergeBlocks(const std::vector<const Block*>& blocks) {
    std::vector<std::size_t> parent(blocks.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }
    const auto root = [&parent](std::size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };

    std::map<std::string, std::size_t> firstByKey;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        for (const std::string& key : blocks[i]->keys) {
            if (key.empty()) {
                continue;
            }
            const auto [it, inserted] = firstByKey.emplace(blocks[i]->kind + "|" + key, i);
            if (!inserted) {
                parent[root(i)] = root(it->second);
            }
        }
    }

    std::map<std::size_t, std::vector<const Block*>> groups;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        groups[root(i)].push_back(blocks[i]);
    }

    std::vector<Item> items;
    for (auto& entry : groups) {
        std::vector<const Block*>& group = entry.second;
        std::set<int> years;
        for (const Block* block : group) {
            if (block->ficha.year) {
                years.insert(*block->ficha.year);
            }
        }
        // Películas con el mismo título y años distintos son obras distintas (remakes)
        if (group.front()->kind == "movie" && years.size() > 1) {
            std::map<int, std::vector<const Block*>> byYear;  // 0 = sin año
            for (const Block* block : group) {
                byYear[block->ficha.year.value_or(0)].push_back(block);
            }
            for (auto& yearGroup : byYear) {
                items.push_back(buildItem(yearGroup.second));
            }
        } else {
            items.push_back(buildItem(group));
        }
    }
    return items;
}

std::vector<const Block*> pointersTo(const std::vector<Block>& blocks) {
    std::vector<const Block*> pointers;
    for (const Block& block : blocks) {
        pointers.push_back(&block);
    }
    return pointers;
}

}  // namespace

Catalog::Catalog(DbManager& db) : db_(db) {}

Catalog::~Catalog() = default;

std::vector<Catalog::Item> Catalog::buildItems(const std::vector<ChannelInput>& channels) {
    std::vector<Block> all;
    for (const ChannelInput& channel : channels) {
        ParseCache cache;  // Los id de mensaje solo son únicos dentro de un chat
        for (Block& block : buildBlocks(channel.chatId, channel.title, channel.messages, channel.topics, cache)) {
            all.push_back(std::move(block));
        }
    }
    return mergeBlocks(pointersTo(all));
}

void Catalog::rebuildChannel(std::int64_t chatId) {
    std::lock_guard<std::mutex> rebuildLock(rebuildMutex_);
    const auto started = std::chrono::steady_clock::now();
    const auto channel = db_.getChannel(chatId);
    if (!channel) {
        blocks_.erase(chatId);
        caches_.erase(chatId);
        publish();
        return;
    }

    std::unique_ptr<ParseCache>& cache = caches_[chatId];
    if (!cache) {
        cache = std::make_unique<ParseCache>();
    }
    blocks_[chatId] = buildBlocks(chatId, channel->title, db_.channelMessages(chatId), db_.listTopics(chatId), *cache);
    publish();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        total = items_.size();
    }
    std::cout << "[Catálogo] " << channel->title << ": " << blocks_[chatId].size() << " fichas; " << total
              << " obras en total (" << elapsed.count() << " ms)" << std::endl;
}

void Catalog::rebuildAll() {
    for (const DbManager::Channel& channel : db_.listChannels()) {
        rebuildChannel(channel.id);
    }
}

void Catalog::removeChannel(std::int64_t chatId) {
    std::lock_guard<std::mutex> rebuildLock(rebuildMutex_);
    blocks_.erase(chatId);
    caches_.erase(chatId);
    publish();
}

void Catalog::publish() {
    std::vector<const Block*> all;
    for (const auto& entry : blocks_) {
        const std::vector<const Block*> pointers = pointersTo(entry.second);
        all.insert(all.end(), pointers.begin(), pointers.end());
    }
    std::vector<ItemPtr> items;
    std::map<std::pair<std::int64_t, std::int64_t>, std::pair<ItemPtr, std::size_t>> index;
    for (Item& item : mergeBlocks(all)) {
        ItemPtr shared = std::make_shared<const Item>(std::move(item));
        for (std::size_t i = 0; i < shared->releases.size(); ++i) {
            for (const Part& part : shared->releases[i].parts) {
                index[{shared->releases[i].chatId, part.messageId}] = {shared, i};
            }
        }
        items.push_back(std::move(shared));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    items_ = std::move(items);
    releaseIndex_ = std::move(index);
}

std::optional<Catalog::ReleaseRef> Catalog::findRelease(std::int64_t chatId, std::int64_t messageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = releaseIndex_.find({chatId, messageId});
    if (it == releaseIndex_.end()) {
        return std::nullopt;
    }
    return ReleaseRef{it->second.first, &it->second.first->releases[it->second.second]};
}

std::vector<Catalog::ItemPtr> Catalog::items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

Catalog::ItemPtr Catalog::find(std::int64_t chatId, std::int64_t anchorMessageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cualquier ficha de la obra sirve: los enlaces siguen funcionando aunque cambie la principal
    for (const ItemPtr& item : items_) {
        for (const auto& [blockChat, blockAnchor] : item->blockIds) {
            if (blockChat == chatId && blockAnchor == anchorMessageId) {
                return item;
            }
        }
    }
    return nullptr;
}
