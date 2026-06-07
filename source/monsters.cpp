//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////
// Remere's Map Editor is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Remere's Map Editor is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////

#include "main.h"

#include "gui.h"
#include "materials.h"
#include "brush.h"
#include "monsters.h"
#include "monster_brush.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {
	bool extractLuaStringArgument(const std::string &content, const std::string &token, std::string &value) {
		const size_t tokenPos = content.find(token);
		if (tokenPos == std::string::npos) {
			return false;
		}

		size_t pos = content.find('(', tokenPos + token.size());
		if (pos == std::string::npos) {
			return false;
		}

		++pos;
		while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos]))) {
			++pos;
		}

		if (pos >= content.size() || (content[pos] != '"' && content[pos] != '\'')) {
			return false;
		}

		const char quote = content[pos++];
		const size_t start = pos;
		while (pos < content.size()) {
			if (content[pos] == quote && content[pos - 1] != '\\') {
				value = content.substr(start, pos - start);
				return true;
			}
			++pos;
		}
		return false;
	}

	bool extractLuaTableBlock(const std::string &content, const std::string &token, std::string &value) {
		const size_t tokenPos = content.find(token);
		if (tokenPos == std::string::npos) {
			return false;
		}

		size_t pos = content.find('{', tokenPos + token.size());
		if (pos == std::string::npos) {
			return false;
		}

		const size_t start = pos;
		int depth = 0;
		while (pos < content.size()) {
			if (content[pos] == '{') {
				++depth;
			} else if (content[pos] == '}') {
				--depth;
				if (depth == 0) {
					value = content.substr(start, pos - start + 1);
					return true;
				}
			}
			++pos;
		}
		return false;
	}

	bool extractLuaIntegerField(const std::string &content, const char* fieldName, int &value) {
		const std::regex pattern("\\b" + std::string(fieldName) + "\\b\\s*=\\s*(-?\\d+)");
		std::smatch match;
		if (!std::regex_search(content, match, pattern)) {
			return false;
		}

		value = std::stoi(match[1].str());
		return true;
	}

	bool hasMonsterNodeByName(const pugi::xml_node &monsterNodes, const std::string &name) {
		const std::string lowerName = as_lower_str(name);
		for (pugi::xml_node monsterNode = monsterNodes.child("monster"); monsterNode; monsterNode = monsterNode.next_sibling("monster")) {
			const pugi::xml_attribute nameAttribute = monsterNode.attribute("name");
			if (nameAttribute && as_lower_str(nameAttribute.as_string()) == lowerName) {
				return true;
			}
		}
		return false;
	}

	void appendMonsterNode(pugi::xml_node &monsterNodes, const MonsterType &monsterType) {
		pugi::xml_node monsterNode = monsterNodes.append_child("monster");
		monsterNode.append_attribute("name") = monsterType.name.c_str();

		const Outfit &outfit = monsterType.outfit;
		if (outfit.lookType != 0) {
			monsterNode.append_attribute("looktype") = outfit.lookType;
		}
		if (outfit.lookItem != 0) {
			monsterNode.append_attribute("lookitem") = outfit.lookItem;
		}
		if (outfit.lookMount != 0) {
			monsterNode.append_attribute("lookmount") = outfit.lookMount;
		}
		if (outfit.lookAddon != 0) {
			monsterNode.append_attribute("lookaddons") = outfit.lookAddon;
		}
		if (outfit.lookHead != 0) {
			monsterNode.append_attribute("lookhead") = outfit.lookHead;
		}
		if (outfit.lookBody != 0) {
			monsterNode.append_attribute("lookbody") = outfit.lookBody;
		}
		if (outfit.lookLegs != 0) {
			monsterNode.append_attribute("looklegs") = outfit.lookLegs;
		}
		if (outfit.lookFeet != 0) {
			monsterNode.append_attribute("lookfeet") = outfit.lookFeet;
		}
	}

	void upsertMonsterNode(pugi::xml_node &monsterNodes, const MonsterType &monsterType) {
		const std::string lowerName = as_lower_str(monsterType.name);
		for (pugi::xml_node monsterNode = monsterNodes.child("monster"); monsterNode; monsterNode = monsterNode.next_sibling("monster")) {
			const pugi::xml_attribute nameAttribute = monsterNode.attribute("name");
			if (nameAttribute && as_lower_str(nameAttribute.as_string()) == lowerName) {
				monsterNodes.remove_child(monsterNode);
				break;
			}
		}
		appendMonsterNode(monsterNodes, monsterType);
	}

	void registerMonsterBrush(MonsterType* monsterType) {
		if (!monsterType->brush) {
			monsterType->brush = newd MonsterBrush(monsterType);
			g_brushes.addBrush(monsterType->brush);
		}
		monsterType->in_other_tileset = true;
		monsterType->brush->flagAsVisible();
	}

	void ensureXmlUtf8Declaration(pugi::xml_document &doc) {
		pugi::xml_node decl;
		for (pugi::xml_node node = doc.first_child(); node; node = node.next_sibling()) {
			if (node.type() == pugi::node_declaration) {
				decl = node;
				break;
			}
		}
		if (!decl) {
			decl = doc.prepend_child(pugi::node_declaration);
		}

		pugi::xml_attribute version = decl.attribute("version");
		if (!version) {
			version = decl.append_attribute("version");
		}
		version.set_value("1.0");

		pugi::xml_attribute encoding = decl.attribute("encoding");
		if (!encoding) {
			encoding = decl.append_attribute("encoding");
		}
		encoding.set_value("UTF-8");
	}

	struct MonsterXmlEntry {
		std::string sortName;
		std::vector<std::pair<std::string, std::string>> attributes;
	};

	void sortMonsterNodesAlphabetically(pugi::xml_node &monsterNodes) {
		std::vector<MonsterXmlEntry> entries;
		for (pugi::xml_node monsterNode = monsterNodes.child("monster"); monsterNode; monsterNode = monsterNode.next_sibling("monster")) {
			MonsterXmlEntry entry;
			const pugi::xml_attribute nameAttribute = monsterNode.attribute("name");
			entry.sortName = nameAttribute ? as_lower_str(nameAttribute.as_string()) : "";

			for (pugi::xml_attribute attribute = monsterNode.first_attribute(); attribute; attribute = attribute.next_attribute()) {
				entry.attributes.emplace_back(attribute.name(), attribute.value());
			}

			entries.push_back(std::move(entry));
		}

		std::sort(entries.begin(), entries.end(), [](const MonsterXmlEntry &lhs, const MonsterXmlEntry &rhs) {
			return lhs.sortName < rhs.sortName;
		});

		while (pugi::xml_node monsterNode = monsterNodes.child("monster")) {
			monsterNodes.remove_child(monsterNode);
		}

		for (const MonsterXmlEntry &entry : entries) {
			pugi::xml_node monsterNode = monsterNodes.append_child("monster");
			for (const auto &[attributeName, attributeValue] : entry.attributes) {
				monsterNode.append_attribute(attributeName.c_str()) = attributeValue.c_str();
			}
		}
	}

	MonsterType* loadFromServerLua(const fs::path &filePath) {
		std::ifstream stream(filePath, std::ios::binary);
		if (!stream.is_open()) {
			return nullptr;
		}

		const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		if (content.empty()) {
			return nullptr;
		}

		std::string name;
		if (!extractLuaStringArgument(content, "Game.createMonsterType", name)) {
			return nullptr;
		}

		std::string outfitBlock;
		if (!extractLuaTableBlock(content, "monster.outfit", outfitBlock)) {
			return nullptr;
		}

		auto* monsterType = newd MonsterType();
		monsterType->name = name;
		monsterType->outfit.name = name;

		extractLuaIntegerField(outfitBlock, "lookType", monsterType->outfit.lookType);
		if (!extractLuaIntegerField(outfitBlock, "lookItem", monsterType->outfit.lookItem)) {
			if (!extractLuaIntegerField(outfitBlock, "lookTypeEx", monsterType->outfit.lookItem)) {
				extractLuaIntegerField(outfitBlock, "lookitem", monsterType->outfit.lookItem);
			}
		}
		extractLuaIntegerField(outfitBlock, "lookMount", monsterType->outfit.lookMount);
		if (!extractLuaIntegerField(outfitBlock, "lookAddons", monsterType->outfit.lookAddon)) {
			extractLuaIntegerField(outfitBlock, "lookAddon", monsterType->outfit.lookAddon);
		}
		extractLuaIntegerField(outfitBlock, "lookHead", monsterType->outfit.lookHead);
		extractLuaIntegerField(outfitBlock, "lookBody", monsterType->outfit.lookBody);
		extractLuaIntegerField(outfitBlock, "lookLegs", monsterType->outfit.lookLegs);
		extractLuaIntegerField(outfitBlock, "lookFeet", monsterType->outfit.lookFeet);

		if (monsterType->outfit.lookType == 0 && monsterType->outfit.lookItem == 0) {
			delete monsterType;
			return nullptr;
		}

		return monsterType;
	}
} // namespace

MonsterDatabase g_monsters;

MonsterType::MonsterType() :
	missing(false),
	in_other_tileset(false),
	standard(false),
	name(""),
	brush(nullptr) {
	////
}

MonsterType::MonsterType(const MonsterType &ct) :
	missing(ct.missing),
	in_other_tileset(ct.in_other_tileset),
	standard(ct.standard),
	name(ct.name),
	outfit(ct.outfit),
	brush(ct.brush) {
	////
}

MonsterType &MonsterType::operator=(const MonsterType &ct) {
	missing = ct.missing;
	in_other_tileset = ct.in_other_tileset;
	standard = ct.standard;
	name = ct.name;
	outfit = ct.outfit;
	brush = ct.brush;
	return *this;
}

MonsterType::~MonsterType() {
	////
}

MonsterType* MonsterType::loadFromXML(pugi::xml_node node, wxArrayString &warnings) {
	pugi::xml_attribute attribute;
	if (!(attribute = node.attribute("name"))) {
		warnings.push_back("Couldn't read name tag of monster node.");
		return nullptr;
	}

	MonsterType* ct = newd MonsterType();
	ct->name = attribute.as_string();
	ct->outfit.name = ct->name;

	if ((attribute = node.attribute("looktype"))) {
		ct->outfit.lookType = attribute.as_int();
		if (g_gui.gfx.getCreatureSprite(ct->outfit.lookType) == nullptr) {
			warnings.push_back("Invalid monster \"" + wxstr(ct->name) + "\" look type #" + std::to_string(ct->outfit.lookType));
		}
	}

	if ((attribute = node.attribute("lookitem"))) {
		ct->outfit.lookItem = attribute.as_int();
	}

	if ((attribute = node.attribute("lookmount"))) {
		ct->outfit.lookMount = attribute.as_int();
	}

	if ((attribute = node.attribute("lookaddon")) || (attribute = node.attribute("lookaddons"))) {
		ct->outfit.lookAddon = attribute.as_int();
	}

	if ((attribute = node.attribute("lookhead"))) {
		ct->outfit.lookHead = attribute.as_int();
	}

	if ((attribute = node.attribute("lookbody"))) {
		ct->outfit.lookBody = attribute.as_int();
	}

	if ((attribute = node.attribute("looklegs"))) {
		ct->outfit.lookLegs = attribute.as_int();
	}

	if ((attribute = node.attribute("lookfeet"))) {
		ct->outfit.lookFeet = attribute.as_int();
	}
	return ct;
}

MonsterType* MonsterType::loadFromOTXML(const FileName &filename, pugi::xml_document &doc, wxArrayString &warnings) {
	ASSERT(doc != nullptr);
	pugi::xml_node node;
	if (!(node = doc.child("monster"))) {
		warnings.push_back("This file is not a monster file");
		return nullptr;
	}

	pugi::xml_attribute attribute;
	if (!(attribute = node.attribute("name"))) {
		warnings.push_back("Couldn't read name tag of monster node.");
		return nullptr;
	}

	MonsterType* ct = newd MonsterType();
	ct->name = attribute.as_string();
	ct->outfit.name = ct->name;

	for (pugi::xml_node optionNode = node.first_child(); optionNode; optionNode = optionNode.next_sibling()) {
		if (as_lower_str(optionNode.name()) != "look") {
			continue;
		}

		if ((attribute = optionNode.attribute("type"))) {
			ct->outfit.lookType = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("item")) || (attribute = optionNode.attribute("lookex")) || (attribute = optionNode.attribute("typeex"))) {
			ct->outfit.lookItem = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("mount"))) {
			ct->outfit.lookMount = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("addon"))) {
			ct->outfit.lookAddon = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("head"))) {
			ct->outfit.lookHead = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("body"))) {
			ct->outfit.lookBody = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("legs"))) {
			ct->outfit.lookLegs = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("feet"))) {
			ct->outfit.lookFeet = attribute.as_int();
		}
	}
	return ct;
}

MonsterDatabase::MonsterDatabase() {
	////
}

MonsterDatabase::~MonsterDatabase() {
	clear();
}

void MonsterDatabase::clear() {
	for (MonsterMap::iterator iter = monster_map.begin(); iter != monster_map.end(); ++iter) {
		delete iter->second;
	}
	monster_map.clear();
}

MonsterType* MonsterDatabase::operator[](const std::string &name) {
	if (name.empty()) {
		return nullptr;
	}

	MonsterMap::iterator iter = monster_map.find(as_lower_str(name));
	if (iter != monster_map.end()) {
		return iter->second;
	}
	return nullptr;
}

MonsterType* MonsterDatabase::addMissingMonsterType(const std::string &name) {
	assert((*this)[name] == nullptr);

	MonsterType* ct = newd MonsterType();
	ct->name = name;
	ct->missing = true;
	ct->outfit.lookType = 130;

	monster_map.insert(std::make_pair(as_lower_str(name), ct));
	return ct;
}

MonsterType* MonsterDatabase::addMonsterType(const std::string &name, const Outfit &outfit) {
	assert((*this)[name] == nullptr);

	MonsterType* ct = newd MonsterType();
	ct->name = name;
	ct->missing = false;
	ct->outfit = outfit;

	monster_map.insert(std::make_pair(as_lower_str(name), ct));
	return ct;
}

bool MonsterDatabase::hasMissing() const {
	for (MonsterMap::const_iterator iter = monster_map.begin(); iter != monster_map.end(); ++iter) {
		if (iter->second->missing) {
			return true;
		}
	}
	return false;
}

bool MonsterDatabase::loadFromXML(const FileName &filename, bool standard, wxString &error, wxArrayString &warnings) {
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + filename.GetFullName() + "\", invalid format?";
		return false;
	}

	pugi::xml_node node = doc.child("monsters");
	if (!node) {
		error = "Invalid file signature, this file is not a valid monsters file.";
		return false;
	}

	for (pugi::xml_node monsterNode = node.first_child(); monsterNode; monsterNode = monsterNode.next_sibling()) {
		if (as_lower_str(monsterNode.name()) != "monster") {
			continue;
		}

		MonsterType* monsterType = MonsterType::loadFromXML(monsterNode, warnings);
		if (monsterType) {
			monsterType->standard = standard;
			if ((*this)[monsterType->name]) {
				warnings.push_back("Duplicate monster type name \"" + wxstr(monsterType->name) + "\"! Discarding...");
				delete monsterType;
			} else {
				monster_map[as_lower_str(monsterType->name)] = monsterType;
			}
		}
	}
	return true;
}

bool MonsterDatabase::importXMLFromOT(const FileName &filename, wxString &error, wxArrayString &warnings) {
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + filename.GetFullName() + "\", invalid format?";
		return false;
	}

	pugi::xml_node node;
	if ((node = doc.child("monsters"))) {
		for (pugi::xml_node monsterNode = node.first_child(); monsterNode; monsterNode = monsterNode.next_sibling()) {
			if (as_lower_str(monsterNode.name()) != "monster") {
				continue;
			}

			pugi::xml_attribute attribute;
			if (!(attribute = monsterNode.attribute("file"))) {
				continue;
			}

			FileName monsterFile(filename);
			monsterFile.SetFullName(wxString(attribute.as_string(), wxConvUTF8));

			pugi::xml_document monsterDoc;
			pugi::xml_parse_result monsterResult = monsterDoc.load_file(monsterFile.GetFullPath().mb_str());
			if (!monsterResult) {
				continue;
			}

			MonsterType* monsterType = MonsterType::loadFromOTXML(monsterFile, monsterDoc, warnings);
			if (monsterType) {
				MonsterType* current = (*this)[monsterType->name];
				if (current) {
					*current = *monsterType;
					delete monsterType;
				} else {
					monster_map[as_lower_str(monsterType->name)] = monsterType;

					Tileset* tileSet = nullptr;
					tileSet = g_materials.tilesets["Monsters"];
					ASSERT(tileSet != nullptr);

					Brush* brush = newd MonsterBrush(monsterType);
					g_brushes.addBrush(brush);

					TilesetCategory* tileSetCategory = tileSet->getCategory(TILESET_MONSTER);
					tileSetCategory->brushlist.push_back(brush);
				}
			}
		}
	} else if ((node = doc.child("monster"))) {
		MonsterType* monsterType = MonsterType::loadFromOTXML(filename, doc, warnings);
		if (monsterType) {
			MonsterType* current = (*this)[monsterType->name];

			if (current) {
				*current = *monsterType;
				delete monsterType;
			} else {
				monster_map[as_lower_str(monsterType->name)] = monsterType;

				Tileset* tileSet = nullptr;
				tileSet = g_materials.tilesets["Monsters"];
				ASSERT(tileSet != nullptr);

				Brush* brush = newd MonsterBrush(monsterType);
				g_brushes.addBrush(brush);

				TilesetCategory* tileSetCategory = tileSet->getCategory(TILESET_MONSTER);
				tileSetCategory->brushlist.push_back(brush);
			}
		}
	} else {
		error = "This is not valid OT monster data file.";
		return false;
	}
	return true;
}

bool MonsterDatabase::importMissingFromServerLua(const FileName &directory, const FileName &targetXml, wxString &error, wxArrayString &warnings) {
	if (!directory.DirExists()) {
		error = "Server data folder does not exist.";
		return false;
	}

	std::unordered_set<std::string> missingNames;
	for (const auto &monsterEntry : monster_map) {
		if (monsterEntry.second && monsterEntry.second->missing) {
			missingNames.insert(monsterEntry.first);
		}
	}

	if (missingNames.empty()) {
		return true;
	}

	pugi::xml_document doc;
	const pugi::xml_parse_result result = doc.load_file(targetXml.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + targetXml.GetFullName() + "\".";
		return false;
	}

	pugi::xml_node monsterNodes = doc.child("monsters");
	if (!monsterNodes) {
		error = "Invalid monsters.xml structure.";
		return false;
	}

	bool xmlChanged = false;
	int importedCount = 0;

	try {
		for (const auto &entry : fs::recursive_directory_iterator(fs::path(nstr(directory.GetFullPath())))) {
			if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
				continue;
			}

			std::unique_ptr<MonsterType> parsedMonster(loadFromServerLua(entry.path()));
			if (!parsedMonster) {
				continue;
			}

			const std::string monsterKey = as_lower_str(parsedMonster->name);
			if (!missingNames.contains(monsterKey)) {
				continue;
			}

			MonsterType* currentMonster = (*this)[parsedMonster->name];
			if (!currentMonster) {
				continue;
			}

			MonsterBrush* existingBrush = currentMonster->brush;
			*currentMonster = *parsedMonster;
			currentMonster->brush = existingBrush;
			currentMonster->missing = false;
			currentMonster->standard = true;
			currentMonster->outfit.name = currentMonster->name;

			if (!hasMonsterNodeByName(monsterNodes, currentMonster->name)) {
				appendMonsterNode(monsterNodes, *currentMonster);
				xmlChanged = true;
			}

			missingNames.erase(monsterKey);
			++importedCount;

			if (missingNames.empty()) {
				break;
			}
		}
	} catch (const std::exception &e) {
		error = wxString::Format("Failed to scan server data folder: %s", wxString(e.what(), wxConvUTF8));
		return importedCount > 0;
	}

	if (xmlChanged) {
		sortMonsterNodesAlphabetically(monsterNodes);
		ensureXmlUtf8Declaration(doc);
	}
	if (xmlChanged && !doc.save_file(targetXml.GetFullPath().mb_str(), "\t", pugi::format_default, pugi::encoding_utf8)) {
		error = "Failed to write updated monsters.xml.";
		return false;
	}

	if (importedCount == 0 && error.empty()) {
		error = "No missing monsters were found in the configured server data folder.";
	}

	return importedCount > 0;
}

bool MonsterDatabase::importFromServerLua(const FileName &directory, const FileName &targetXml, wxString &error, wxArrayString &warnings) {
	if (!directory.DirExists()) {
		error = "Server data folder does not exist.";
		return false;
	}

	pugi::xml_document doc;
	const pugi::xml_parse_result result = doc.load_file(targetXml.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + targetXml.GetFullName() + "\".";
		return false;
	}

	pugi::xml_node monsterNodes = doc.child("monsters");
	if (!monsterNodes) {
		error = "Invalid monsters.xml structure.";
		return false;
	}

	bool xmlChanged = false;
	int importedCount = 0;

	std::vector<fs::path> luaFiles;
	try {
		for (const auto &entry : fs::recursive_directory_iterator(fs::path(nstr(directory.GetFullPath())))) {
			if (entry.is_regular_file() && entry.path().extension() == ".lua") {
				luaFiles.push_back(entry.path());
			}
		}
	} catch (const std::exception &e) {
		error = wxString::Format("Failed to scan server data folder: %s", wxString(e.what(), wxConvUTF8));
		return false;
	}

	struct LoadBarGuard {
		~LoadBarGuard() {
			g_gui.DestroyLoadBar();
		}
	} loadBarGuard;
	g_gui.CreateLoadBar("Importing monsters from server...");

	const size_t totalFiles = luaFiles.size();
	try {
		for (size_t fileIndex = 0; fileIndex < totalFiles; ++fileIndex) {
			const fs::path &filePath = luaFiles[fileIndex];

			if (totalFiles > 0) {
				g_gui.SetLoadDone(
					static_cast<int>(100.0 * (fileIndex + 1) / totalFiles),
					wxString::Format("Importing monsters... (%zu/%zu)", fileIndex + 1, totalFiles)
				);
			}

			std::unique_ptr<MonsterType> parsedMonster(loadFromServerLua(filePath));
			if (!parsedMonster) {
				continue;
			}

			MonsterType* currentMonster = (*this)[parsedMonster->name];
			if (currentMonster) {
				MonsterBrush* existingBrush = currentMonster->brush;
				const bool wasInOtherTileset = currentMonster->in_other_tileset;
				*currentMonster = *parsedMonster;
				currentMonster->brush = existingBrush;
				currentMonster->in_other_tileset = wasInOtherTileset;
				currentMonster->missing = false;
				currentMonster->standard = true;
				currentMonster->outfit.name = currentMonster->name;
				if (currentMonster->brush) {
					currentMonster->in_other_tileset = true;
					currentMonster->brush->flagAsVisible();
				}
			} else {
				currentMonster = parsedMonster.release();
				currentMonster->missing = false;
				currentMonster->standard = true;
				currentMonster->outfit.name = currentMonster->name;
				monster_map[as_lower_str(currentMonster->name)] = currentMonster;
				registerMonsterBrush(currentMonster);
			}

			upsertMonsterNode(monsterNodes, *currentMonster);
			xmlChanged = true;
			++importedCount;
		}
	} catch (const std::exception &e) {
		error = wxString::Format("Failed to scan server data folder: %s", wxString(e.what(), wxConvUTF8));
		return importedCount > 0;
	}

	if (xmlChanged) {
		sortMonsterNodesAlphabetically(monsterNodes);
		ensureXmlUtf8Declaration(doc);
	}
	if (xmlChanged && !doc.save_file(targetXml.GetFullPath().mb_str(), "\t", pugi::format_default, pugi::encoding_utf8)) {
		error = "Failed to write updated monsters.xml.";
		return false;
	}

	if (importedCount == 0 && error.empty()) {
		error = "No monsters were found in the configured server data folder.";
	}

	return importedCount > 0;
}

bool MonsterDatabase::saveToXML(const FileName &filename) {
	pugi::xml_document doc;

	ensureXmlUtf8Declaration(doc);

	pugi::xml_node monsterNodes = doc.append_child("monsters");
	for (const auto &monsterEntry : monster_map) {
		MonsterType* monsterType = monsterEntry.second;
		if (!monsterType->standard) {
			pugi::xml_node monsterNode = monsterNodes.append_child("monster");

			monsterNode.append_attribute("name") = monsterType->name.c_str();
			monsterNode.append_attribute("type") = "monster";

			const Outfit &outfit = monsterType->outfit;
			monsterNode.append_attribute("looktype") = outfit.lookType;
			monsterNode.append_attribute("lookitem") = outfit.lookItem;
			monsterNode.append_attribute("lookaddon") = outfit.lookAddon;
			monsterNode.append_attribute("lookhead") = outfit.lookHead;
			monsterNode.append_attribute("lookbody") = outfit.lookBody;
			monsterNode.append_attribute("looklegs") = outfit.lookLegs;
			monsterNode.append_attribute("lookfeet") = outfit.lookFeet;
		}
	}
	return doc.save_file(filename.GetFullPath().mb_str(), "\t", pugi::format_default, pugi::encoding_utf8);
}

wxArrayString MonsterDatabase::getMissingMonsterNames() const {
	wxArrayString missingMonsters;
	for (const auto &monsterEntry : monster_map) {
		if (monsterEntry.second->missing) {
			missingMonsters.Add(monsterEntry.second->name);
		}
	}
	return missingMonsters;
}
