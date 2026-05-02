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
#include "npcs.h"
#include "npc_brush.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <unordered_map>
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

	bool extractLuaStringAssignment(const std::string &content, const std::string &token, std::string &value) {
		const size_t tokenPos = content.find(token);
		if (tokenPos == std::string::npos) {
			return false;
		}

		size_t pos = content.find('=', tokenPos + token.size());
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

	struct NpcXmlEntry {
		std::string sortName;
		std::vector<std::pair<std::string, std::string>> attributes;
	};

	void sortNpcNodesAlphabetically(pugi::xml_node &npcNodes) {
		std::vector<NpcXmlEntry> entries;
		for (pugi::xml_node npcNode = npcNodes.child("npc"); npcNode; npcNode = npcNode.next_sibling("npc")) {
			NpcXmlEntry entry;
			const pugi::xml_attribute nameAttribute = npcNode.attribute("name");
			entry.sortName = nameAttribute ? as_lower_str(nameAttribute.as_string()) : "";

			for (pugi::xml_attribute attribute = npcNode.first_attribute(); attribute; attribute = attribute.next_attribute()) {
				entry.attributes.emplace_back(attribute.name(), attribute.value());
			}

			entries.push_back(std::move(entry));
		}

		std::sort(entries.begin(), entries.end(), [](const NpcXmlEntry &lhs, const NpcXmlEntry &rhs) {
			return lhs.sortName < rhs.sortName;
		});

		while (pugi::xml_node npcNode = npcNodes.child("npc")) {
			npcNodes.remove_child(npcNode);
		}

		for (const NpcXmlEntry &entry : entries) {
			pugi::xml_node npcNode = npcNodes.append_child("npc");
			for (const auto &[attributeName, attributeValue] : entry.attributes) {
				npcNode.append_attribute(attributeName.c_str()) = attributeValue.c_str();
			}
		}
	}

	bool hasNpcNodeByName(const pugi::xml_node &npcNodes, const std::string &name) {
		const std::string lowerName = as_lower_str(name);
		for (pugi::xml_node npcNode = npcNodes.child("npc"); npcNode; npcNode = npcNode.next_sibling("npc")) {
			const pugi::xml_attribute nameAttribute = npcNode.attribute("name");
			if (nameAttribute && as_lower_str(nameAttribute.as_string()) == lowerName) {
				return true;
			}
		}
		return false;
	}

	void appendNpcNode(pugi::xml_node &npcNodes, const NpcType &npcType) {
		pugi::xml_node npcNode = npcNodes.append_child("npc");
		npcNode.append_attribute("name") = npcType.name.c_str();

		const Outfit &outfit = npcType.outfit;
		if (outfit.lookType != 0) {
			npcNode.append_attribute("looktype") = outfit.lookType;
		}
		if (outfit.lookItem != 0) {
			npcNode.append_attribute("lookitem") = outfit.lookItem;
		}
		if (outfit.lookMount != 0) {
			npcNode.append_attribute("lookmount") = outfit.lookMount;
		}
		if (outfit.lookAddon != 0) {
			npcNode.append_attribute("lookaddons") = outfit.lookAddon;
		}
		if (outfit.lookHead != 0) {
			npcNode.append_attribute("lookhead") = outfit.lookHead;
		}
		if (outfit.lookBody != 0) {
			npcNode.append_attribute("lookbody") = outfit.lookBody;
		}
		if (outfit.lookLegs != 0) {
			npcNode.append_attribute("looklegs") = outfit.lookLegs;
		}
		if (outfit.lookFeet != 0) {
			npcNode.append_attribute("lookfeet") = outfit.lookFeet;
		}
	}

	NpcType* loadFromServerLua(const fs::path &filePath) {
		std::ifstream stream(filePath, std::ios::binary);
		if (!stream.is_open()) {
			return nullptr;
		}

		const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		if (content.empty()) {
			return nullptr;
		}

		std::unordered_map<std::string, std::string> stringVariables;
		const std::regex stringAssignmentPattern(R"(\blocal\s+([A-Za-z_]\w*)\s*=\s*["']([^"']+)["'])");
		for (std::sregex_iterator it(content.begin(), content.end(), stringAssignmentPattern), end; it != end; ++it) {
			stringVariables[(*it)[1].str()] = (*it)[2].str();
		}

		std::string npcName;
		if (!extractLuaStringArgument(content, "Game.createNpcType", npcName)) {
			const std::regex createNpcTypeVarPattern(R"(\bGame\.createNpcType\s*\(\s*([A-Za-z_]\w*)\s*\))");
			std::smatch createMatch;
			if (std::regex_search(content, createMatch, createNpcTypeVarPattern)) {
				const auto variableIt = stringVariables.find(createMatch[1].str());
				if (variableIt != stringVariables.end()) {
					npcName = variableIt->second;
				}
			}
		}

		if (npcName.empty()) {
			extractLuaStringAssignment(content, "npcConfig.name", npcName);
		}
		if (npcName.empty()) {
			const std::regex npcConfigNameVarPattern(R"(\bnpcConfig\.name\s*=\s*([A-Za-z_]\w*))");
			std::smatch nameMatch;
			if (std::regex_search(content, nameMatch, npcConfigNameVarPattern)) {
				const auto variableIt = stringVariables.find(nameMatch[1].str());
				if (variableIt != stringVariables.end()) {
					npcName = variableIt->second;
				}
			}
		}
		if (npcName.empty()) {
			return nullptr;
		}

		std::string outfitBlock;
		if (!extractLuaTableBlock(content, "npcConfig.outfit", outfitBlock)) {
			return nullptr;
		}

		auto* npcType = newd NpcType();
		npcType->name = npcName;
		npcType->outfit.name = npcName;

		extractLuaIntegerField(outfitBlock, "lookType", npcType->outfit.lookType);
		if (!extractLuaIntegerField(outfitBlock, "lookItem", npcType->outfit.lookItem)) {
			if (!extractLuaIntegerField(outfitBlock, "lookTypeEx", npcType->outfit.lookItem)) {
				extractLuaIntegerField(outfitBlock, "lookitem", npcType->outfit.lookItem);
			}
		}
		extractLuaIntegerField(outfitBlock, "lookMount", npcType->outfit.lookMount);
		if (!extractLuaIntegerField(outfitBlock, "lookAddons", npcType->outfit.lookAddon)) {
			extractLuaIntegerField(outfitBlock, "lookAddon", npcType->outfit.lookAddon);
		}
		extractLuaIntegerField(outfitBlock, "lookHead", npcType->outfit.lookHead);
		extractLuaIntegerField(outfitBlock, "lookBody", npcType->outfit.lookBody);
		extractLuaIntegerField(outfitBlock, "lookLegs", npcType->outfit.lookLegs);
		extractLuaIntegerField(outfitBlock, "lookFeet", npcType->outfit.lookFeet);

		if (npcType->outfit.lookType == 0 && npcType->outfit.lookItem == 0) {
			delete npcType;
			return nullptr;
		}

		return npcType;
	}
} // namespace

NpcDatabase g_npcs;

NpcType::NpcType() :
	missing(false),
	in_other_tileset(false),
	standard(false),
	name(""),
	brush(nullptr) {
	////
}

NpcType::NpcType(const NpcType &npc) :
	missing(npc.missing),
	in_other_tileset(npc.in_other_tileset),
	standard(npc.standard),
	name(npc.name),
	outfit(npc.outfit),
	brush(npc.brush) {
	////
}

NpcType &NpcType::operator=(const NpcType &npc) {
	missing = npc.missing;
	in_other_tileset = npc.in_other_tileset;
	standard = npc.standard;
	name = npc.name;
	outfit = npc.outfit;
	brush = npc.brush;
	return *this;
}

NpcType::~NpcType() {
	////
}

NpcType* NpcType::loadFromXML(pugi::xml_node node, wxArrayString &warnings) {
	pugi::xml_attribute attribute;
	if (!(attribute = node.attribute("name"))) {
		warnings.push_back("Couldn't read name tag of npc node.");
		return nullptr;
	}

	NpcType* npcType = newd NpcType();
	npcType->name = attribute.as_string();
	npcType->outfit.name = npcType->name;

	if ((attribute = node.attribute("looktype"))) {
		npcType->outfit.lookType = attribute.as_int();
		if (g_gui.gfx.getCreatureSprite(npcType->outfit.lookType) == nullptr) {
			warnings.push_back("Invalid npc \"" + wxstr(npcType->name) + "\" look type #" + std::to_string(npcType->outfit.lookType));
		}
	}

	if ((attribute = node.attribute("lookitem"))) {
		npcType->outfit.lookItem = attribute.as_int();
	}

	if ((attribute = node.attribute("lookaddon")) || (attribute = node.attribute("lookaddons"))) {
		npcType->outfit.lookAddon = attribute.as_int();
	}

	if ((attribute = node.attribute("lookhead"))) {
		npcType->outfit.lookHead = attribute.as_int();
	}

	if ((attribute = node.attribute("lookbody"))) {
		npcType->outfit.lookBody = attribute.as_int();
	}

	if ((attribute = node.attribute("looklegs"))) {
		npcType->outfit.lookLegs = attribute.as_int();
	}

	if ((attribute = node.attribute("lookfeet"))) {
		npcType->outfit.lookFeet = attribute.as_int();
	}
	return npcType;
}

NpcType* NpcType::loadFromOTXML(const FileName &filename, pugi::xml_document &doc, wxArrayString &warnings) {
	ASSERT(doc != nullptr);

	pugi::xml_node node;
	if (!(node = doc.child("npc"))) {
		warnings.push_back("This file is not a npc file");
		return nullptr;
	}

	pugi::xml_attribute attribute;
	if (!(attribute = node.attribute("name"))) {
		warnings.push_back("Couldn't read name tag of npc node.");
		return nullptr;
	}

	NpcType* npcType = newd NpcType();
	npcType->name = nstr(filename.GetName());

	for (pugi::xml_node optionNode = node.first_child(); optionNode; optionNode = optionNode.next_sibling()) {
		if (as_lower_str(optionNode.name()) != "look") {
			continue;
		}

		if ((attribute = optionNode.attribute("type"))) {
			npcType->outfit.lookType = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("item")) || (attribute = optionNode.attribute("lookex")) || (attribute = optionNode.attribute("typeex"))) {
			npcType->outfit.lookItem = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("addon"))) {
			npcType->outfit.lookAddon = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("head"))) {
			npcType->outfit.lookHead = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("body"))) {
			npcType->outfit.lookBody = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("legs"))) {
			npcType->outfit.lookLegs = attribute.as_int();
		}

		if ((attribute = optionNode.attribute("feet"))) {
			npcType->outfit.lookFeet = attribute.as_int();
		}
	}
	return npcType;
}

NpcDatabase::NpcDatabase() {
	////
}

NpcDatabase::~NpcDatabase() {
	clear();
}

void NpcDatabase::clear() {
	for (NpcMap::iterator iter = npcMap.begin(); iter != npcMap.end(); ++iter) {
		delete iter->second;
	}
	npcMap.clear();
}

NpcType* NpcDatabase::operator[](const std::string &name) {
	NpcMap::iterator iter = npcMap.find(as_lower_str(name));
	if (iter != npcMap.end()) {
		return iter->second;
	}
	return nullptr;
}

NpcType* NpcDatabase::addMissingNpcType(const std::string &name) {
	assert((*this)[name] == nullptr);

	NpcType* npcType = newd NpcType();
	npcType->name = name;
	npcType->missing = true;
	npcType->outfit.lookType = 130;

	npcMap.insert(std::make_pair(as_lower_str(name), npcType));
	return npcType;
}

NpcType* NpcDatabase::addNpcType(const std::string &name, const Outfit &outfit) {
	assert((*this)[name] == nullptr);

	NpcType* npcType = newd NpcType();
	npcType->name = name;
	npcType->missing = false;
	npcType->outfit = outfit;

	npcMap.insert(std::make_pair(as_lower_str(name), npcType));
	return npcType;
}

bool NpcDatabase::hasMissing() const {
	for (NpcMap::const_iterator iter = npcMap.begin(); iter != npcMap.end(); ++iter) {
		if (iter->second->missing) {
			return true;
		}
	}
	return false;
}

bool NpcDatabase::loadFromXML(const FileName &filename, bool standard, wxString &error, wxArrayString &warnings) {
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + filename.GetFullName() + "\", invalid format?";
		return false;
	}

	pugi::xml_node node = doc.child("npcs");
	if (!node) {
		error = "Invalid file signature, this file is not a valid npc file.";
		return false;
	}

	for (pugi::xml_node npcNode = node.first_child(); npcNode; npcNode = npcNode.next_sibling()) {
		if (as_lower_str(npcNode.name()) != "npc") {
			continue;
		}

		NpcType* npcType = NpcType::loadFromXML(npcNode, warnings);
		if (npcType) {
			npcType->standard = standard;
			if ((*this)[npcType->name]) {
				warnings.push_back("Duplicate npc with name \"" + wxstr(npcType->name) + "\"! Discarding...");
				delete npcType;
			} else {
				npcMap[as_lower_str(npcType->name)] = npcType;
			}
		}
	}
	return true;
}

bool NpcDatabase::importXMLFromOT(const FileName &filename, wxString &error, wxArrayString &warnings) {
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(filename.GetFullPath().mb_str());
	if (!result) {
		error = "Couldn't open file \"" + filename.GetFullName() + "\", invalid format?";
		return false;
	}

	pugi::xml_node node;
	if ((node = doc.child("npc"))) {
		NpcType* npcType = NpcType::loadFromOTXML(filename, doc, warnings);
		if (npcType) {
			NpcType* current = (*this)[npcType->name];

			if (current) {
				*current = *npcType;
				delete npcType;
			} else {
				npcMap[as_lower_str(npcType->name)] = npcType;

				Tileset* tileSet = nullptr;
				tileSet = g_materials.tilesets["NPCs"];
				ASSERT(tileSet != nullptr);

				Brush* brush = newd NpcBrush(npcType);
				g_brushes.addBrush(brush);

				TilesetCategory* tileSetCategory = tileSet->getCategory(TILESET_NPC);
				tileSetCategory->brushlist.push_back(brush);
			}
		}
	} else {
		error = "This is not valid OT npc data file.";
		return false;
	}
	return true;
}

bool NpcDatabase::importMissingFromServerLua(const FileName &directory, const FileName &targetXml, wxString &error, wxArrayString &warnings) {
	if (!directory.DirExists()) {
		error = "Server data folder does not exist.";
		return false;
	}

	std::unordered_set<std::string> missingNames;
	for (const auto &npcEntry : npcMap) {
		if (npcEntry.second && npcEntry.second->missing) {
			missingNames.insert(npcEntry.first);
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

	pugi::xml_node npcNodes = doc.child("npcs");
	if (!npcNodes) {
		error = "Invalid npcs.xml structure.";
		return false;
	}

	bool xmlChanged = false;
	int importedCount = 0;

	try {
		for (const auto &entry : fs::recursive_directory_iterator(fs::path(nstr(directory.GetFullPath())))) {
			if (!entry.is_regular_file() || entry.path().extension() != ".lua") {
				continue;
			}

			std::unique_ptr<NpcType> parsedNpc(loadFromServerLua(entry.path()));
			if (!parsedNpc) {
				continue;
			}

			const std::string npcKey = as_lower_str(parsedNpc->name);
			if (!missingNames.contains(npcKey)) {
				continue;
			}

			NpcType* currentNpc = (*this)[parsedNpc->name];
			if (!currentNpc) {
				continue;
			}

			NpcBrush* existingBrush = currentNpc->brush;
			*currentNpc = *parsedNpc;
			currentNpc->brush = existingBrush;
			currentNpc->missing = false;
			currentNpc->standard = true;
			currentNpc->outfit.name = currentNpc->name;

			if (!hasNpcNodeByName(npcNodes, currentNpc->name)) {
				appendNpcNode(npcNodes, *currentNpc);
				xmlChanged = true;
			}

			missingNames.erase(npcKey);
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
		sortNpcNodesAlphabetically(npcNodes);
		ensureXmlUtf8Declaration(doc);
	}
	if (xmlChanged && !doc.save_file(targetXml.GetFullPath().mb_str(), "\t", pugi::format_default, pugi::encoding_utf8)) {
		error = "Failed to write updated npcs.xml.";
		return false;
	}

	if (importedCount == 0 && error.empty()) {
		error = "No missing npcs were found in the configured server data folder.";
	}

	return importedCount > 0;
}

bool NpcDatabase::saveToXML(const FileName &filename) {
	pugi::xml_document doc;

	ensureXmlUtf8Declaration(doc);

	pugi::xml_node npcNodes = doc.append_child("npcs");
	for (const auto &npcEntry : npcMap) {
		NpcType* npcType = npcEntry.second;
		if (!npcType->standard) {
			pugi::xml_node npcNode = npcNodes.append_child("npc");

			npcNode.append_attribute("name") = npcType->name.c_str();
			npcNode.append_attribute("type") = "npc";

			const Outfit &outfit = npcType->outfit;
			npcNode.append_attribute("looktype") = outfit.lookType;
			npcNode.append_attribute("lookitem") = outfit.lookItem;
			npcNode.append_attribute("lookaddons") = outfit.lookAddon;
			npcNode.append_attribute("lookhead") = outfit.lookHead;
			npcNode.append_attribute("lookbody") = outfit.lookBody;
			npcNode.append_attribute("looklegs") = outfit.lookLegs;
			npcNode.append_attribute("lookfeet") = outfit.lookFeet;
		}
	}
	sortNpcNodesAlphabetically(npcNodes);
	return doc.save_file(filename.GetFullPath().mb_str(), "\t", pugi::format_default, pugi::encoding_utf8);
}

wxArrayString NpcDatabase::getMissingNpcNames() const {
	wxArrayString missingNpcs;
	for (const auto &ncpEntry : npcMap) {
		if (ncpEntry.second->missing) {
			missingNpcs.Add(ncpEntry.second->name);
		}
	}
	return missingNpcs;
}
