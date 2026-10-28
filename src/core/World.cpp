#include "core/World.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

#include "core/ContentHash.h"
#include "core/Csv.h"
#include "core/Errors.h"
#include "core/JsonInt.h"
#include "core/Text.h"

namespace morpheus::core {

namespace {

// Reads a whole file as raw bytes. Throws DataError when it cannot be opened.
std::string ReadTextFile(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw DataError(std::format("cannot read '{}'", PathText(file)));
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Parses JSON text, turning parse errors into DataError prefixed with the file name.
nlohmann::json ParseJsonFile(const std::filesystem::path& file, const std::string& text)
{
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        throw DataError(std::format("'{}': {}", PathText(file), e.what()));
    }
}

} // namespace

World LoadWorld(const std::filesystem::path& directory)
{
    // A trailing separator would leave filename() empty; normalize so the folder-name check works.
    std::filesystem::path dir = directory.lexically_normal();
    if (dir.filename().empty()) {
        dir = dir.parent_path();
    }
    const std::string folderName = PathText(dir.filename());

    // Read every file up front: the content hash covers all of them, names included, in this fixed order.
    // Hash format, per file in order: the file name, "\n", the decimal byte length of the normalized text, "\n",
    // then the normalized text. The length makes file boundaries unambiguous. Text is normalized by turning CRLF
    // into LF before hashing and parsing, so a world hashes and loads the same whatever its line endings, and
    // rules and lore never carry a carriage return into prompts.
    const std::array<std::string, 5> FILE_NAMES = {
        "world.json", "rules.md", "lore.csv", "state_schema.json", "initial_state.json"};
    std::map<std::string, std::string> texts;
    ContentHash hash;
    for (const std::string& name : FILE_NAMES) {
        const std::string raw = ReadTextFile(dir / name);
        std::string text;
        text.reserve(raw.size());
        std::size_t line = 1; // 1-based line number in the original file, for the error message.
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\r') {
                // A '\r' directly before '\n' is dropped (CRLF becomes LF); a carriage return on its own means a
                // malformed file.
                if (i + 1 >= raw.size() || raw[i + 1] != '\n') {
                    throw DataError(std::format("'{}': stray carriage return on line {}", PathText(dir / name), line));
                }
                continue;
            }
            if (raw[i] == '\n') {
                ++line;
            }
            text.push_back(raw[i]);
        }
        hash.Add(name);
        hash.Add("\n");
        hash.Add(std::to_string(text.size()));
        hash.Add("\n");
        hash.Add(text);
        texts.emplace(name, std::move(text));
    }

    World world;
    world.contentHash = hash.Hex();

    // world.json: identity, dice, difficulties, defaults.
    const std::filesystem::path worldFile = dir / "world.json";
    const std::string worldWhere = std::format("'{}'", PathText(worldFile));
    {
        const nlohmann::json json = ParseJsonFile(worldFile, texts.at("world.json"));
        if (!json.is_object()) {
            throw DataError(worldWhere + ": must be a JSON object");
        }

        // Only the documented keys are allowed, so a typo like "memory_mod" fails loudly.
        const std::set<std::string> allowedKeys = {"id", "title", "dice", "check_entity", "stats", "difficulties", "memory_mode"};
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (!allowedKeys.contains(it.key())) {
                throw DataError(std::format("{}: unknown key '{}'", worldWhere, it.key()));
            }
        }

        auto requireString = [&](const char* key) {
            if (!json.contains(key) || !json.at(key).is_string() || json.at(key).get<std::string>().empty()) {
                throw DataError(std::format("{}: needs a non-empty string '{}'", worldWhere, key));
            }
            return json.at(key).get<std::string>();
        };

        world.id = requireString("id");
        if (world.id != folderName) {
            throw DataError(std::format("{}: id '{}' must match the folder name '{}'", worldWhere, world.id, folderName));
        }
        world.title = requireString("title");

        const std::string diceText = requireString("dice");
        try {
            world.dice = DiceRule::Parse(diceText);
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", worldWhere, e.what()));
        }

        world.checkEntity = requireString("check_entity");

        // stats: the check entity's int fields a check may add. Shape is checked here; that each one is an int
        // field of the check entity's type needs the schema, so that is checked after the schema loads.
        if (!json.contains("stats") || !json.at("stats").is_array() || json.at("stats").empty()) {
            throw DataError(worldWhere + ": needs a non-empty 'stats' array");
        }
        for (const nlohmann::json& stat : json.at("stats")) {
            if (!stat.is_string() || stat.get<std::string>().empty()) {
                throw DataError(worldWhere + ": 'stats' entries must be non-empty strings");
            }
            if (std::find(world.stats.begin(), world.stats.end(), stat.get<std::string>()) != world.stats.end()) {
                throw DataError(std::format("{}: stat '{}' is listed twice in 'stats'", worldWhere, stat.get<std::string>()));
            }
            world.stats.push_back(stat.get<std::string>());
        }

        const std::string memoryMode = requireString("memory_mode");
        if (memoryMode == "engine") {
            world.defaultMemoryMode = MemoryMode::Engine;
        } else if (memoryMode == "assisted") {
            world.defaultMemoryMode = MemoryMode::Assisted;
        } else {
            throw DataError(std::format("{}: memory_mode must be 'engine' or 'assisted', got '{}'", worldWhere, memoryMode));
        }

        if (!json.contains("difficulties") || !json.at("difficulties").is_object() || json.at("difficulties").empty()) {
            throw DataError(worldWhere + ": needs a non-empty 'difficulties' object");
        }
        for (const auto& [name, target] : json.at("difficulties").items()) {
            if (!target.is_number_integer()) {
                throw DataError(std::format("{}: difficulty '{}' must be an integer", worldWhere, name));
            }
            // Range-check before narrowing to int, so a 64-bit value cannot wrap into range.
            const std::optional<long long> parsed =
                JsonIntInRange(target, std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
            if (!parsed) {
                throw DataError(std::format("{}: difficulty '{}' must be an integer in int range", worldWhere, name));
            }
            world.difficulties.emplace(name, static_cast<int>(*parsed));
        }
    }

    // rules.md: the GM instructions, used verbatim.
    world.rules = texts.at("rules.md");
    if (world.rules.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw DataError(std::format("'{}': must not be empty", PathText(dir / "rules.md")));
    }

    // lore.csv: header "subject,data" or "subject,data,always_on", then one entry per record.
    {
        const std::string loreWhere = std::format("'{}'", PathText(dir / "lore.csv"));
        std::vector<std::vector<std::string>> records;
        try {
            records = ParseCsv(texts.at("lore.csv"));
        } catch (const DataError& e) {
            throw DataError(std::format("{}: {}", loreWhere, e.what()));
        }

        const std::vector<std::string> plainHeader = {"subject", "data"};
        const std::vector<std::string> alwaysOnHeader = {"subject", "data", "always_on"};
        if (records.empty() || (records.front() != plainHeader && records.front() != alwaysOnHeader)) {
            throw DataError(loreWhere + ": header must be subject,data or subject,data,always_on");
        }
        const bool hasAlwaysOn = records.front() == alwaysOnHeader;

        std::set<std::string> subjects;
        for (std::size_t i = 1; i < records.size(); ++i) {
            const std::vector<std::string>& record = records[i];
            const std::string recordWhere = std::format("{} record {}", loreWhere, i + 1);
            if (record.size() != records.front().size()) {
                throw DataError(std::format("{}: expected {} fields, got {}", recordWhere, records.front().size(), record.size()));
            }

            LoreEntry entry;
            entry.subject = record[0];
            entry.data = record[1];
            if (entry.subject.empty() || entry.data.empty()) {
                throw DataError(recordWhere + ": subject and data must not be empty");
            }
            if (!subjects.insert(entry.subject).second) {
                throw DataError(std::format("{}: duplicate subject '{}'", recordWhere, entry.subject));
            }
            // always_on: "true", "false" or empty (= false).
            if (hasAlwaysOn) {
                if (record[2] == "true") {
                    entry.alwaysOn = true;
                } else if (record[2] != "false" && !record[2].empty()) {
                    throw DataError(recordWhere + ": always_on must be true, false or empty");
                }
            }
            world.lore.push_back(std::move(entry));
        }
    }

    // state_schema.json, then initial_state.json validated against it.
    {
        const std::filesystem::path file = dir / "state_schema.json";
        const nlohmann::json json = ParseJsonFile(file, texts.at("state_schema.json"));
        try {
            world.schema = StateSchema::FromJson(json);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", PathText(file), e.what()));
        }
    }
    {
        const std::filesystem::path file = dir / "initial_state.json";
        world.initialState = ParseJsonFile(file, texts.at("initial_state.json"));
        try {
            world.schema.ValidateState(world.initialState);
        } catch (const DataError& e) {
            throw DataError(std::format("'{}': {}", PathText(file), e.what()));
        }
    }

    // Checks add stats from this entity, so it has to exist from the start.
    if (!world.initialState.contains(world.checkEntity)) {
        throw DataError(std::format("{}: check_entity '{}' is not an entity in initial_state.json", worldWhere, world.checkEntity));
    }

    // Every stat must be an int field of the check entity's type, or a check could not read its value.
    const EntityTypeDef* pCheckType =
        world.schema.FindEntityType(world.initialState.at(world.checkEntity).at("type").get<std::string>());
    MORPHEUS_REQUIRE(pCheckType != nullptr, "LoadWorld: check entity type missing after ValidateState");
    for (const std::string& stat : world.stats) {
        const auto fieldIt = pCheckType->fields.find(stat);
        if (fieldIt == pCheckType->fields.end()) {
            throw DataError(std::format("{}: stat '{}' is not a field of check_entity '{}'", worldWhere, stat, world.checkEntity));
        }
        if (fieldIt->second.type != FieldType::Int) {
            throw DataError(std::format("{}: stat '{}' must be an int field, but it is {}", worldWhere, stat,
                                        FieldTypeName(fieldIt->second.type)));
        }
    }
    return world;
}

} // namespace morpheus::core
