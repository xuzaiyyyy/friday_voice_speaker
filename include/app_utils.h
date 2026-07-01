#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace xiaoman
{

std::string trim(const std::string &text);
std::string to_lower_ascii(std::string text);

bool path_exists(const std::string &path);
bool dir_exists(const std::string &path);
std::string join_path(const std::string &a, const std::string &b);
std::vector<std::string> cwd_and_parents(int max_dirs);
std::vector<std::string> split_csv(const std::string &text);
std::string env_first(std::initializer_list<const char *> names);
bool env_bool(std::initializer_list<const char *> names, bool default_value);
int env_int(std::initializer_list<const char *> names, int default_value);

std::string default_emoji_image_root();
std::string find_asr_model_dir();
std::string find_helper_path();

std::string json_escape(const std::string &input);
std::string base64_encode(const uint8_t *data, size_t size);

} // namespace xiaoman
