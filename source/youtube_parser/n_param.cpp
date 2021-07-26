#include <vector>
#include <utility>
#include <regex>
#include <string>
#include <map>
#include "internal_common.hpp"
#include "n_param.hpp"

#ifdef _WIN32
#include <iostream> // <------------
#include <fstream> // <-------
#include <sstream> // <-------
#define debug(s) std::cerr << (s) << std::endl
#else
#include "headers.hpp"
#define debug(s) Util_log_save("yt-parser", (s));
#endif


// a bit costly, so it's called only when the simpler detection failed
// refer to https://github.com/pytube/pytube/blob/48ea5205b92e8e090866589be67780ce8e29a922/pytube/cipher.py#L104
static std::string get_initial_function_name_precise(const std::string &js) {
	// confusing but they are raw string literals : R"(ACTUAL_STRING)"
	std::vector<std::string> function_patterns = {
		R"(a\.C&&\(b=a\.get\("n"\)\)&&\(b=([^(]+)\(b\),a\.set\("n",b\)\)}};)"
	};
	for (auto pattern_str : function_patterns) {
		std::regex pattern(pattern_str);
		std::smatch match_res;
		if (std::regex_search(js, match_res, pattern))
			return match_res[1].str();
	}
	
	return "";
}

using FunctionType = NParamFunctionType;
using CArrayContent = NParamCArrayContent;

std::vector<CArrayContent> get_carray(const std::string &func_content) {
	std::vector<CArrayContent> res;
	
	auto array_start = func_content.find(",c=[");
	if (array_start == std::string::npos) {
		debug("failed to detect carray");
		return {};
	}
	std::string array_str;
	for (auto c : remove_garbage(func_content, array_start + 3)) if (c != '\r' && c != '\n') array_str.push_back(c);
	
	size_t head = 1;
	while (head + 1 < array_str.size()) {
		if (array_str[head] == ' ') {
			head++;
			continue;
		}
		CArrayContent cur_element;
		if (array_str.substr(head, std::string("function").size()) == "function") {
			cur_element.type = CArrayContent::Type::FUNCTION;
			
			auto start_pos = array_str.find("{", head);
			if (start_pos == std::string::npos) {
				debug("brace starting function not found");
				return {};
			}
			auto content = remove_garbage(array_str, start_pos);
			auto end_pos = start_pos + content.size();
			
			std::vector<std::pair<std::regex, FunctionType> > function_patterns = {
				{std::regex(R"(\{for\(\w=\(\w%\w\.length\+\w\.length\)%\w\.length;\w--;\)\w\.unshift\(\w.pop\(\)\)\})"), FunctionType::ROTATE_RIGHT},
				{std::regex(R"(\{\w\.reverse\(\)\})"), FunctionType::REVERSE},
				{std::regex(R"(\{\w\.push\(\w\)\})"), FunctionType::PUSH},
				{std::regex(R"(;var\s\w=\w\[0\];\w\[0\]=\w\[\w\];\w\[\w\]=\w\})"), FunctionType::SWAP},
				{std::regex(R"(case\s\d+)"), FunctionType::CIPHER},
				{std::regex(R"(\w\.splice\(0,1,\w\.splice\(\w,1,\w\[0\]\)\[0\]\))"), FunctionType::SWAP},
				{std::regex(R"(;\w\.splice\(\w,1\)\})"), FunctionType::SPLICE},
				{std::regex(R"(\w\.splice\(-\w\)\.reverse\(\)\.forEach\(function\(\w\)\{\w\.unshift\(\w\)\}\))"), FunctionType::ROTATE_RIGHT},
				{std::regex(R"(for\(var \w=\w\.length;\w;\)\w\.push\(\w\.splice\(--\w,1\)\[0\]\)\})"), FunctionType::REVERSE}
			};
			bool matched = false;
			for (auto i : function_patterns) {
				std::smatch match_result;
				if (std::regex_search(content, match_result, i.first)) {
					cur_element.function = i.second;
					matched = true;
					break;
				}
			}
			if (cur_element.function == FunctionType::CIPHER) { // simulate to get `h` here
				std::string known_sig = "for(var f=64,h=[];++f-h.length-32;){switch(f){";
				auto start = content.find(known_sig);
				if (start == std::string::npos) {
					debug("unknown cipher function");
					return {};
				}
				start += known_sig.size();
				std::vector<std::pair<std::string, int> > instructions;
				while (start < content.size() && content[start] != '}') {
					size_t end_pos;
					std::string type;
					std::string arg;
					if (content.substr(start, 4) == "case") {
						type = "case";
						end_pos = std::find(content.begin() + start, content.end(), ':') - content.begin();
						arg = content.substr(start + 5, end_pos - (start + 5));
					} else {
						end_pos = std::find(content.begin() + start, content.end(), ';') - content.begin();
						if (content.substr(start, 3) == "f+=") {
							type = "add";
							arg = content.substr(start + 3, end_pos - (start + 3));
						} else if (content.substr(start, 3) == "f-=") {
							type = "sub";
							arg = content.substr(start + 3, end_pos - (start + 3));
						} else if (content.substr(start, 2) == "f=") {
							type = "assign";
							arg = content.substr(start + 2, end_pos - (start + 2));
						} else if (content.substr(start, 8) == "continue") {
							type = "continue";
						} else if (content.substr(start, 5) == "break") {
							type = "break";
						}
					}
					instructions.push_back({type, arg != "" ? stoll(arg) : -1});
					end_pos = std::min<size_t>(end_pos, std::find(content.begin() + start, content.end(), '}') - content.begin());
					start = end_pos + 1;
				}
				std::vector<char> h;
				int f = 64;
				while (++f - h.size() - 32) {
					bool continue_flag = false;
					bool case_matched = false;
					for (auto i : instructions) {
						if (i.first == "case" && f == i.second) case_matched = true;
						else if (case_matched) {
							if (i.first == "add") f += i.second;
							if (i.first == "sub") f -= i.second;
							if (i.first == "assign") f = i.second;
							if (i.first == "continue") {
								continue_flag = true;
								break;
							}
							if (i.first == "break") break;
						}
					}
					if (continue_flag) continue;
					h.push_back((char) f);
				}
				cur_element.function_internal_arg = std::string(h.begin(), h.end());
			}
			if (!matched) {
				debug("unknown function type");
				return {};
			}
			head = end_pos + 1; // skip comma
		} else {
			auto end_pos = array_str.find(",", head);
			if (end_pos == std::string::npos) end_pos = array_str.size() - 1;
			
			std::string content = array_str.substr(head, end_pos - head);
			if (content[0] == '"') {
				if (content.size() < 2) {
					debug("unclosed string literal");
					return {};
				}
				cur_element.type = CArrayContent::Type::STRING;
				auto tmp_str = content.substr(1, content.size() - 2);
				cur_element.string = std::vector<char>(tmp_str.begin(), tmp_str.end());
			} else if (content == "null") cur_element.type = CArrayContent::Type::SELF;
			else if (content == "b") cur_element.type = CArrayContent::Type::N;
			else {
				cur_element.type = CArrayContent::Type::INTEGER;
				char *end;
				cur_element.integer = strtoll(content.c_str(), &end, 10);
				if (end != &content[0] + content.size()) {
					debug("failed to parse integer : " + content);
					return {};
				}
			}
			head = end_pos + 1; // skip comma
		}
		res.push_back(cur_element);
	}
	
	return res;
}
std::vector<std::pair<int, std::pair<int, int> > > get_ops(const std::string &func_content) {
	std::vector<std::pair<int, std::pair<int, int> > > res;
	
	auto array_start = func_content.find("try{");
	if (array_start == std::string::npos) {
		debug("failed to detect operation try-catch block");
		return {};
	}
	std::string ops_str;
	for (auto c : remove_garbage(func_content, array_start + 3)) if (c != '\r' && c != '\n') ops_str.push_back(c);
	
	std::regex op_regex(std::string(R"(\w+\[(\d+)\]\(\w+\[(\d+)\](,\w+\[(\d+)\])?\))"));
	size_t head = 1;
	while (head + 1 < ops_str.size()) {
		if (!isalpha(ops_str[head])) {
			head++;
			continue;
		}
		std::smatch match_result;
		if (std::regex_search(ops_str.cbegin() + head, ops_str.cend(), match_result, op_regex)) {
			std::pair<int, std::pair<int, int> > cur_op;
			cur_op.first = stoll(match_result[1].str());
			cur_op.second.first = stoll(match_result[2].str());
			cur_op.second.second = match_result[4].str() != "" ? stoll(match_result[4].str()) : -1;
			res.push_back(cur_op);
			head = match_result[0].second - ops_str.begin();
		} else {
			debug("unexpected pattern in the try-catch block");
			return {};
		}
	}
	
	return res;
}


/*
	return : vector of operations
	each operation :
		first : type of operation
			0. reverse() : std::reverse(a.begin(), a.end());
			1. splice(b) : a = a.substr(b, a.size() - b)
			2. swap(b) : std::swap(a[0], a[b % a.size()])
		second : the argument 'b' (if first == 0, this should be ignored)
*/
yt_nparam_transform_procedure yt_nparam_get_transform_plan(const std::string &js) {
	// get initial function name
	std::string func_content;
	{
		std::string name = get_initial_function_name_precise(js);
		if (name == "") {
			debug("Failed to get nparam transform function");
			return {};
		}
		std::smatch match_res;
		if (std::regex_search(js, match_res, std::regex(name + R"(=function\(\w+\))"))) {
			auto itr = match_res[0].second;
			func_content = remove_garbage(js, itr - js.begin());
		} else {
			debug("nparam transform function definition not found");
			return {};
		}
	}
	auto c = get_carray(func_content);
	auto ops = get_ops(func_content);
	
	return {c, ops};
}


int64_t normalize(int64_t size, int64_t val) {
	return (val % size + size) % size;
}
template<typename T> static void op_rotate_right(std::vector<T> &list, int64_t arg) {
	arg = normalize(list.size(), arg);
	std::rotate(list.begin(), list.end() - arg, list.end());
}
template<typename T> static void op_reverse(std::vector<T> &list) {
	std::reverse(list.begin(), list.end());
}
// push is only for Type::SELF, so no separate function here
template<typename T> static void op_swap(std::vector<T> &list, int64_t arg) {
	arg = normalize(list.size(), arg);
	std::swap(list[0], list[arg]);
}
template<typename T> static void op_splice(std::vector<T> &list, int64_t arg, int64_t delete_count = -1) {
	arg = std::min(arg, (int64_t) list.size());
	if (arg < 0) arg = std::max<int64_t>(0, (int64_t) list.size() - arg);
	
	if (delete_count == -1 || delete_count > (int64_t) list.size() - arg)
		delete_count = list.size() - arg;
	
	list.erase(list.begin() + arg, list.begin() + arg + delete_count);
}
template<typename T> static void op_cipher(const std::string &internal_arg, std::vector<T> &list, std::vector<char> arg) {
	auto chars = internal_arg;
	int f = 96;
	
	auto list_copy = list;
	for (int i = 0; i < (int) list_copy.size(); i++) {
		int bracket_val = (int) (std::find(chars.begin(), chars.end(), list_copy[i]) - chars.begin()) -
						  (int) (std::find(chars.begin(), chars.end(), arg[i]) - chars.begin()) + i - 32 + f;
		bracket_val %= chars.size();
		arg.push_back(chars[bracket_val]);
		list[i] = chars[bracket_val];
		f--;
	}
}
std::string yt_modify_nparam(std::string n_param_org, const yt_nparam_transform_procedure &transform_plan) {
	std::vector<char> n_param(n_param_org.begin(), n_param_org.end());
	auto c = transform_plan.c;
	for (auto op : transform_plan.ops) {
		int func_index = op.first;
		int arg0_index = op.second.first;
		int arg1_index = op.second.second;
		if (c[func_index].type != CArrayContent::Type::FUNCTION) {
			debug("function expected");
			return n_param_org;
		}
		if (c[func_index].function == FunctionType::ROTATE_RIGHT) {
			if (arg1_index == -1 || c[arg1_index].type != CArrayContent::Type::INTEGER) {
				debug("rotate_right : integer expected as arg1");
				return n_param_org;
			}
			int64_t arg1 = c[arg1_index].integer;
			if (c[arg0_index].type == CArrayContent::Type::SELF) op_rotate_right(c, arg1);
			else if (c[arg0_index].type == CArrayContent::Type::N) op_rotate_right(n_param, arg1);
			else if (c[arg0_index].type == CArrayContent::Type::STRING) op_rotate_right(c[arg0_index].string, arg1);
			else {
				debug("rotate_right : unknown arg0 type : " + c[arg0_index].to_string());
				return n_param_org;
			}
		} else if (c[func_index].function == FunctionType::REVERSE) {
			if (c[arg0_index].type == CArrayContent::Type::SELF) op_reverse(c);
			else if (c[arg0_index].type == CArrayContent::Type::N) op_reverse(n_param);
			else if (c[arg0_index].type == CArrayContent::Type::STRING) op_reverse(c[arg0_index].string);
			else {
				debug("reverse : unknown arg0 type : " + c[arg0_index].to_string());
				return n_param_org;
			}
		} else if (c[func_index].function == FunctionType::PUSH) {
			if (c[arg0_index].type == CArrayContent::Type::SELF) c.push_back((CArrayContent) c[arg1_index]);
			else {
				debug("reverse : unknown arg0 type : " + c[arg0_index].to_string());
				return n_param_org;
			}
		} else if (c[func_index].function == FunctionType::SWAP) {
			if (arg1_index == -1 || c[arg1_index].type != CArrayContent::Type::INTEGER) {
				debug("swap : integer expected as arg1");
				return n_param_org;
			}
			int64_t arg1 = c[arg1_index].integer;
			if (c[arg0_index].type == CArrayContent::Type::SELF) op_swap(c, arg1);
			else if (c[arg0_index].type == CArrayContent::Type::N) op_swap(n_param, arg1);
			else if (c[arg0_index].type == CArrayContent::Type::STRING) op_swap(c[arg0_index].string, arg1);
			else {
				debug("swap : unknown arg0 type : " + c[arg0_index].to_string());
				return n_param_org;
			}
		} else if (c[func_index].function == FunctionType::CIPHER) {
			if (arg1_index == -1 || c[arg1_index].type != CArrayContent::Type::STRING) {
				debug("cipher : string expected as arg1");
				return n_param_org;
			}
			if (c[arg0_index].type == CArrayContent::Type::N) op_cipher(c[func_index].function_internal_arg, n_param, c[arg1_index].string);
			else if (c[arg0_index].type == CArrayContent::Type::STRING) op_cipher(c[func_index].function_internal_arg, c[arg0_index].string, c[arg1_index].string);
			else debug("cipher : unknown arg0 type : " + c[arg0_index].to_string());
		} else if (c[func_index].function == FunctionType::SPLICE) {
			if (arg1_index == -1 || c[arg1_index].type != CArrayContent::Type::INTEGER) {
				debug("splice : integer expected as arg1");
				return n_param_org;
			}
			int64_t arg1 = c[arg1_index].integer;
			if (c[arg0_index].type == CArrayContent::Type::SELF) op_splice(c, arg1, 1);
			else if (c[arg0_index].type == CArrayContent::Type::N) op_splice(n_param, arg1, 1);
			else if (c[arg0_index].type == CArrayContent::Type::STRING) op_splice(c[arg0_index].string, arg1, 1);
			else {
				debug("splice : unknown arg0 type : " + c[arg0_index].to_string());
				return n_param_org;
			}
		}
	}
	
	return std::string(n_param.begin(), n_param.end());
}