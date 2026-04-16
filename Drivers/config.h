#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

// Simple configuration file parser
class Config {
public:
    Config() {}

    // Load configuration from file
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "[CONFIG] Warning: Could not open " << filename << std::endl;
            return false;
        }

        std::string line;
        int lineNum = 0;
        while (std::getline(file, line)) {
            lineNum++;

            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;

            // Parse key = value
            size_t pos = line.find('=');
            if (pos == std::string::npos) {
                std::cerr << "[CONFIG] Warning: Invalid line " << lineNum << ": " << line << std::endl;
                continue;
            }

            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // Trim whitespace from key and value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            values[key] = value;
        }

        std::cout << "[CONFIG] Loaded " << values.size() << " settings from " << filename << std::endl;
        return true;
    }

    // Get string value
    std::string getString(const std::string& key, const std::string& defaultValue = "") const {
        auto it = values.find(key);
        return (it != values.end()) ? it->second : defaultValue;
    }

    // Get integer value
    int getInt(const std::string& key, int defaultValue = 0) const {
        auto it = values.find(key);
        if (it == values.end()) return defaultValue;

        try {
            return std::stoi(it->second);
        } catch (...) {
            std::cerr << "[CONFIG] Warning: Invalid integer for " << key << ": " << it->second << std::endl;
            return defaultValue;
        }
    }

    // Get unsigned integer value
    uint32_t getUInt(const std::string& key, uint32_t defaultValue = 0) const {
        auto it = values.find(key);
        if (it == values.end()) return defaultValue;

        try {
            return std::stoul(it->second);
        } catch (...) {
            std::cerr << "[CONFIG] Warning: Invalid unsigned integer for " << key << ": " << it->second << std::endl;
            return defaultValue;
        }
    }

    // Get float value
    float getFloat(const std::string& key, float defaultValue = 0.0f) const {
        auto it = values.find(key);
        if (it == values.end()) return defaultValue;

        try {
            return std::stof(it->second);
        } catch (...) {
            std::cerr << "[CONFIG] Warning: Invalid float for " << key << ": " << it->second << std::endl;
            return defaultValue;
        }
    }

    // Get boolean value
    bool getBool(const std::string& key, bool defaultValue = false) const {
        auto it = values.find(key);
        if (it == values.end()) return defaultValue;

        std::string value = it->second;
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);

        if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
        if (value == "0" || value == "false" || value == "no" || value == "off") return false;

        return defaultValue;
    }

    // Check if key exists
    bool has(const std::string& key) const {
        return values.find(key) != values.end();
    }

    // Print all loaded values (for debugging)
    void print() const {
        std::cout << "[CONFIG] Loaded settings:" << std::endl;
        for (const auto& pair : values) {
            std::cout << "  " << pair.first << " = " << pair.second << std::endl;
        }
    }

private:
    std::map<std::string, std::string> values;
};

#endif // CONFIG_H
