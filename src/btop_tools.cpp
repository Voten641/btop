/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

indent = tab
tab-size = 4
*/

#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <cstdlib>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "widechar_width.hpp"
#include "btop_log.hpp"
#include "btop_shared.hpp"
#include "btop_tools.hpp"
#include "btop_config.hpp"

using std::cout;
using std::floor;
using std::flush;
using std::max;
using std::string_view;
using std::to_string;

using namespace std::literals; // to use operator""s

namespace fs = std::filesystem;

//? ------------------------------------------------- NAMESPACES ------------------------------------------------------

//* Collection of escape codes and functions for terminal manipulation
namespace Term {

	atomic<bool> initialized{};
	atomic<int> width{};
	atomic<int> height{};
	string current_tty;

	namespace {
		struct termios initial_settings;

		//* Toggle terminal input echo
		bool echo(bool on=true) {
			struct termios settings;
			if (tcgetattr(STDIN_FILENO, &settings)) return false;
			if (on) settings.c_lflag |= ECHO;
			else settings.c_lflag &= ~(ECHO);
			return 0 == tcsetattr(STDIN_FILENO, TCSANOW, &settings);
		}

		//* Toggle need for return key when reading input
		bool linebuffered(bool on=true) {
			struct termios settings;
			if (tcgetattr(STDIN_FILENO, &settings)) return false;
			if (on) settings.c_lflag |= ICANON;
			else {
				settings.c_lflag &= ~(ICANON);
				settings.c_cc[VMIN] = 0;
				settings.c_cc[VTIME] = 0;
			}
			if (tcsetattr(STDIN_FILENO, TCSANOW, &settings)) return false;
			if (on) setlinebuf(stdin);
			else setbuf(stdin, nullptr);
			return true;
		}
	}

	bool refresh(bool only_check) {
		// Query dimensions of '/dev/tty' of the 'STDOUT_FILENO' isn't available.
		// This variable is set in those cases to avoid calls to ioctl
		constinit static bool uses_dev_tty = false;
		struct winsize wsize {};
		if (uses_dev_tty || ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize) < 0 || (wsize.ws_col == 0 && wsize.ws_row == 0)) {
			Logger::error(R"(Couldn't determine terminal size of "STDOUT_FILENO"!)");
			auto dev_tty = open("/dev/tty", O_RDONLY | O_CLOEXEC);
			if (dev_tty != -1) {
				ioctl(dev_tty, TIOCGWINSZ, &wsize);
				close(dev_tty);
			}
			else {
				Logger::error(R"(Couldn't determine terminal size of "/dev/tty"!)");
				return false;
			}
			uses_dev_tty = true;
		}
		if (width != wsize.ws_col or height != wsize.ws_row) {
			if (not only_check) {
				width = wsize.ws_col;
				height = wsize.ws_row;
			}
			return true;
		}
		return false;
	}

	//* Compute the min size for a custom "boxes_layout" arrangement (see Tools::parseBoxesLayout()),
	//* returns an empty optional if "boxes_layout" is unset, invalid, or doesn't match <boxes>.
	std::optional<array<int, 2>> get_min_size_custom_layout(const string& boxes) {
		const auto& custom_layout = Config::getS("boxes_layout");
		if (custom_layout.empty()) return std::nullopt;

		vector<Tools::LayoutRow> rows;
		if (not Tools::parseBoxesLayout(custom_layout, rows) or not Tools::boxesLayoutMatches(rows, boxes))
			return std::nullopt;

		int total_width = 0, total_height = 0;
		for (const auto& row : rows) {
			int row_width = 0, row_height = 0;
			for (const auto& box : row.boxes) {
				int w = 0, h = 0;
				if (box.name == "cpu") { w = Cpu::min_width; h = Cpu::min_height; }
				else if (box.name == "mem") { w = Mem::min_width; h = Mem::min_height; }
				else if (box.name == "net") { w = Mem::min_width; h = Net::min_height; }
				else if (box.name == "proc") { w = Proc::min_width; h = Proc::min_height; }
			#ifdef GPU_SUPPORT
				else if (box.name.starts_with("gpu")) {
					const int idx = box.name.back() - '0';
					w = Gpu::min_width;
					h = 4 + (idx >= 0 and idx < (int)Gpu::gpu_b_height_offsets.size() ? Gpu::gpu_b_height_offsets[idx] : 0);
				}
			#endif
				row_width += w;
				row_height = max(row_height, h);
			}
			total_width = max(total_width, row_width);
			total_height += row_height;
		}

		return array<int, 2>{ total_width, total_height };
	}

	auto get_min_size(const string& boxes) -> array<int, 2> {
		if (auto custom = get_min_size_custom_layout(boxes); custom.has_value())
			return custom.value();

        bool cpu = boxes.find("cpu") != string::npos;
        bool mem = boxes.find("mem") != string::npos;
        bool net = boxes.find("net") != string::npos;
        bool proc = boxes.find("proc") != string::npos;
	#ifdef GPU_SUPPORT
		int gpu = 0;
        if (Gpu::count > 0)
        	for (char i = '0'; i <= '5'; i++)
        		gpu += (boxes.contains("gpu"s + i) ? 1 : 0);
	#endif
        int width = 0;
		if (mem) width = Mem::min_width;
		else if (net) width = Mem::min_width;
		width += (proc ? Proc::min_width : 0);
		if (cpu and width < Cpu::min_width) width = Cpu::min_width;
	#ifdef GPU_SUPPORT
		if (gpu != 0 and width < Gpu::min_width) width = Gpu::min_width;
	#endif

		int height = (cpu ? Cpu::min_height : 0);
		if (proc) height += Proc::min_height;
		else height += (mem ? Mem::min_height : 0) + (net ? Net::min_height : 0);
	#ifdef GPU_SUPPORT
		for (int i = 0; i < gpu; i++)
			height += Gpu::gpu_b_height_offsets[i] + 4;
	#endif

		return { width, height };
	}

	bool init() {
		if (not initialized) {
			initialized = (bool)isatty(STDIN_FILENO);
			if (initialized) {
				tcgetattr(STDIN_FILENO, &initial_settings);
				current_tty = (ttyname(STDIN_FILENO) != nullptr ? static_cast<string>(ttyname(STDIN_FILENO)) : "unknown");

				//? Disable stream sync - this does not seem to work on OpenBSD
#ifndef __OpenBSD__
				cout.sync_with_stdio(false);
#endif

				//? Disable stream ties
				cout.tie(nullptr);
				echo(false);
				linebuffered(false);
				refresh();

				const auto is_mouse_enabled = !Config::getB("disable_mouse");
				cout << alt_screen << hide_cursor << (is_mouse_enabled ? mouse_on : mouse_off) << flush;
				Global::resized = false;
			}
		}
		return initialized;
	}

	void restore() {
		if (initialized) {
			tcsetattr(STDIN_FILENO, TCSANOW, &initial_settings);
			cout << mouse_off << clear << Fx::reset << normal_screen << show_cursor << flush;
			initialized = false;
		}
	}
}

//? --------------------------------------------------- FUNCTIONS -----------------------------------------------------

// ! Disabled due to issue when compiling with musl, reverted back to using regex
// namespace Fx {
// 	string uncolor(const string& s) {
// 		string out = s;
// 		for (size_t offset = 0, start_pos = 0, end_pos = 0;;) {
// 			start_pos = (offset == 0) ? out.find('\x1b') : offset;
// 			if (start_pos == string::npos)
// 				break;
// 			offset = start_pos + 1;
// 			end_pos = out.find('m', offset);
// 			if (end_pos == string::npos)
// 				break;
// 			else if (auto next_pos = out.find('\x1b', offset); not isdigit(out[end_pos - 1]) or end_pos > next_pos) {
// 			 	offset = next_pos;
// 				continue;
// 			}

// 			out.erase(start_pos, (end_pos - start_pos)+1);
// 			offset = 0;
// 		}
// 		out.shrink_to_fit();
// 		return out;
// 	}
// }

namespace Tools {

	string replace_ascii_control(string str, const char replacement) {
		if (str.empty())
			return str;
		const char* str_data = &str.data()[0];
		size_t w_len = 1 + std::mbstowcs(nullptr, str_data, 0);
		if (w_len <= 1)	{
			std::ranges::for_each(str, [&replacement](char& c) { if (c < 0x20) c = replacement; });
			return str;
		}
		vector<wchar_t> w_str(w_len);
		std::mbstowcs(&w_str[0], str_data, w_len);
		for (size_t i = 0; i < w_str.size() - 1; i++) {
			if (widechar_wcwidth(w_str[i]) > 1)
				continue;
			if (w_str[i] < 0x20)
				w_str[i] = replacement;
		}
		size_t bytes = std::wcstombs(nullptr, w_str.data(), 0);
		if (bytes == static_cast<size_t>(-1))
			return str;
		str.resize(bytes);
		std::wcstombs(&str[0], &w_str[0], bytes);

		return str;
	}

	size_t wide_ulen(const std::string_view str) {
		if (str.empty())
			return 0;
		unsigned int chars = 0;

		const char* str_data = &str.data()[0];
			size_t w_len = 1 + std::mbstowcs(nullptr, str_data, 0);
		if (w_len <= 1)
			return ulen(str);
		vector<wchar_t> w_str(w_len);
		std::mbstowcs(&w_str[0], str_data, w_len);

		for (auto c : w_str) {
			chars += widechar_wcwidth(c);
		}

		return chars;
	}

	size_t wide_ulen(const vector<wchar_t> w_str) {
		unsigned int chars = 0;

		for (auto c : w_str) {
			chars += widechar_wcwidth(c);
		}

		return chars;
	}

	string uresize(string str, const size_t len, bool wide) {
		if (len < 1 or str.empty())
			return "";

		if (wide) {
			const char* str_data = &str.data()[0];
			size_t w_len = 1 + std::mbstowcs(nullptr, str_data, 0);
			if (w_len <= 1)
				return uresize(str, len, false);
			vector<wchar_t> w_str(w_len);
			std::mbstowcs(&w_str[0], str_data, w_len);

			while (wide_ulen(w_str) > len) w_str.pop_back();

			string n_str;
			n_str.resize(w_str.size());
			std::wcstombs(&n_str[0], &w_str[0], w_str.size());

			return n_str;
		}
		else {
			for (size_t x = 0, i = 0; i < str.size(); i++) {
				if ((static_cast<unsigned char>(str.at(i)) & 0xC0) != 0x80) x++;
				if (x >= len + 1) {
					str.resize(i);
					break;
				}
			}
		}
		str.shrink_to_fit();
		return str;
	}

	string luresize(string str, const size_t len, bool wide) {
		if (len < 1 or str.empty())
			return "";

		for (size_t x = 0, last_pos = 0, i = str.size() - 1; i > 0 ; i--) {
			if (wide and static_cast<unsigned char>(str.at(i)) > 0xef) {
				x += 2;
				last_pos = max((size_t)0, i - 1);
			}
			else if ((static_cast<unsigned char>(str.at(i)) & 0xC0) != 0x80) {
				x++;
				last_pos = i;
			}
			if (x >= len) {
				str = str.substr(last_pos);
				str.shrink_to_fit();
				break;
			}
		}
		return str;
	}

	string s_replace(const string& str, const string& from, const string& to) {
		string out = str;
		for (size_t start_pos = out.find(from); start_pos != std::string::npos; start_pos = out.find(from)) {
			out.replace(start_pos, from.length(), to);
		}
		return out;
	}

	string_view ltrim(string_view str, const string_view t_str) {
		while (str.starts_with(t_str))
			str.remove_prefix(t_str.size());

		return str;
	}

	string_view rtrim(string_view str, const string_view t_str) {
		while (str.ends_with(t_str))
			str.remove_suffix(t_str.size());

		return str;
	}

	string ljust(string str, const size_t x, bool utf, bool wide, bool limit) {
		if (utf) {
			if (limit and ulen(str, wide) > x)
				return uresize(str, x, wide);

			return str + string(max((int)(x - ulen(str)), 0), ' ');
		}
		else {
			if (limit and str.size() > x) {
				str.resize(x);
				return str;
			}
			return str + string(max((int)(x - str.size()), 0), ' ');
		}
	}

	string rjust(string str, const size_t x, bool utf, bool wide, bool limit) {
		if (utf) {
			if (limit and ulen(str, wide) > x)
				return uresize(str, x, wide);

			return string(max((int)(x - ulen(str)), 0), ' ') + str;
		}
		else {
			if (limit and str.size() > x) {
				str.resize(x);
				return str;
			};
			return string(max((int)(x - str.size()), 0), ' ') + str;
		}
	}

	string cjust(string str, const size_t x, bool utf, bool wide, bool limit) {
		if (utf) {
			if (limit and ulen(str, wide) > x)
				return uresize(str, x, wide);

			return string(max((int)ceil((double)(x - ulen(str)) / 2), 0), ' ') + str + string(max((int)floor((double)(x - ulen(str)) / 2), 0), ' ');
		}
		else {
			if (limit and str.size() > x) {
				str.resize(x);
				return str;
			}
			return string(max((int)ceil((double)(x - str.size()) / 2), 0), ' ') + str + string(max((int)floor((double)(x - str.size()) / 2), 0), ' ');
		}
	}

	string trans(const string& str) {
		std::string_view oldstr{str};
		string newstr;
		newstr.reserve(str.size());
		for (size_t pos; (pos = oldstr.find(' ')) != string::npos;) {
			newstr.append(oldstr.substr(0, pos));
			size_t x = 0;
			while (pos + x < oldstr.size() and oldstr.at(pos + x) == ' ') x++;
			newstr.append(Mv::r(x));
			oldstr.remove_prefix(pos + x);
		}
		return (newstr.empty()) ? str : newstr + string{oldstr};
	}

	string sec_to_dhms(size_t seconds, bool no_days, bool no_seconds) {
		size_t days = seconds / 86400; seconds %= 86400;
		size_t hours = seconds / 3600; seconds %= 3600;
		size_t minutes = seconds / 60; seconds %= 60;
		string out 	= (not no_days and days > 0 ? to_string(days) + "d " : "")
					+ (hours < 10 ? "0" : "") + to_string(hours) + ':'
					+ (minutes < 10 ? "0" : "") + to_string(minutes)
					+ (not no_seconds ? ":" + string(std::cmp_less(seconds, 10) ? "0" : "") + to_string(seconds) : "");
		return out;
	}

	string floating_humanizer(uint64_t value, bool shorten, size_t start, bool bit, bool per_second) {
		string out;
		const size_t mult = (bit) ? 8 : 1;

		bool mega = Config::getB("base_10_sizes");

		// Bitrates
	    if(bit && per_second) {
			const auto& base_10_bitrate = Config::getS("base_10_bitrate");
			if(base_10_bitrate == "True") {
				mega = true;
			} else if(base_10_bitrate == "False") {
				mega = false;
			}
			// Default or "Auto": Uses base_10_sizes for bitrates
		}

		// taking advantage of type deduction for array creation (since C++17)
		// combined with string literals (operator""s)
		static const array mebiUnits_bit {
			"bit"s, "Kib"s, "Mib"s,
			"Gib"s, "Tib"s, "Pib"s,
			"Eib"s, "Zib"s, "Yib"s,
			"Rib"s, "Qib"s
		};
		static const array mebiUnits_byte {
			"Byte"s, "KiB"s, "MiB"s,
			"GiB"s, "TiB"s, "PiB"s,
			"EiB"s, "ZiB"s, "YiB"s,
			"RiB"s, "QiB"s
		};
		static const array megaUnits_bit {
			"bit"s, "kb"s, "Mb"s,
			"Gb"s, "Tb"s, "Pb"s,
			"Eb"s, "Zb"s, "Yb"s,
			"Rb"s, "Qb"s
		};
		static const array megaUnits_byte {
			"Byte"s, "kB"s, "MB"s,
			"GB"s, "TB"s, "PB"s,
			"EB"s, "ZB"s, "YB"s,
			"RB"s, "QB"s
		};
		const auto& units = (bit) ? ( mega ? megaUnits_bit : mebiUnits_bit) : ( mega ? megaUnits_byte : mebiUnits_byte);

		value *= 100 * mult;

		if (mega) {
			while (value >= 100000) {
				value /= 1000;
				start++;
			}
		}
		else {
			while (value >= 102400) {
				value >>= 10;
				start++;
			}
		}
		if (out.empty()) {
			out = fmt::format("{}", value);
			if (not mega and out.size() == 4 and start > 0) {
				out.pop_back();
				out.insert(2, ".");
			}
			else if (out.size() == 3 and start > 0) {
				out.insert(1, ".");
			}
			else if (out.size() >= 2) {
				out.resize(out.size() - 2);
			}
			if (out.empty()) {
				out = "0";
			}
		}

		if (shorten) {
			auto has_sep = out.find(".") != string::npos;
			if (has_sep) {
				out = fmt::format("{:.1f}", stod(out));
			}
			if (out.size() > 3) {
				// if out is of the form xy.z
				if (has_sep) {
					out = fmt::format("{:.0f}", stod(out));
				}
				// if out is of the form xyzw (only when not using base 10)
				else {
					out = fmt::format("{:d}.0", out[0] - '0');
					start++;
				}
			}
			out.push_back(units[start][0]);
		}
		else out += " " + units[start];

		if (per_second) out += (bit) ? "ps" : "/s";
		return out;
	}

	std::string operator*(const string& str, int64_t n) {
		if (n < 1 or str.empty()) {
			return "";
		}
		else if (n == 1) {
			return str;
		}

		string new_str;
		new_str.reserve(str.size() * n);

		for (; n > 0; n--)
			new_str.append(str);

		return new_str;
	}

	string strf_time(const string& strf) {
		auto in_time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm bt {};
		std::stringstream ss;
		ss << std::put_time(localtime_r(&in_time_t, &bt), strf.c_str());
		return ss.str();
	}

	void atomic_wait(const atomic<bool>& atom, bool old) noexcept {
		atom.wait(old, std::memory_order_acquire);
	}

	atomic_lock::atomic_lock(atomic<bool>& atom, bool wait) : atom(atom) {
		if (wait) {
			bool expected = false;
			while (not this->atom.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) expected = false;
		}
		else this->atom.store(true, std::memory_order_release);
		this->atom.notify_one();
	}

	atomic_lock::~atomic_lock() noexcept {
		this->atom.store(false, std::memory_order_release);
		this->atom.notify_one();
	}

	void atomic_waiting_lock::store(bool new_value) noexcept {
		{
			std::lock_guard lock {mtx};
			value = new_value;
		}
		cv.notify_all();
	}

	void atomic_waiting_lock::wait(bool old) const noexcept {
		std::unique_lock lock {mtx};
		cv.wait(lock, [this, old] { return value != old; });
	}

	void atomic_waiting_lock::wait_for(bool old, uint64_t wait_ms) const noexcept {
		std::unique_lock lock {mtx};
		const auto start = uptime_micros();
		const auto wait_us = wait_ms * 1'000ULL;
		while (value == old) {
			const auto elapsed = uptime_micros() - start;
			if (elapsed >= wait_us) break;
			cv.wait_for(lock, std::chrono::microseconds(wait_us - elapsed), [this, old] { return value != old; });
		}
	}

	atomic_waiting_lock::guard atomic_waiting_lock::lock(bool wait) {
		return guard(*this, wait);
	}

	atomic_waiting_lock::operator bool() const noexcept {
		std::lock_guard lock {mtx};
		return value;
	}

	atomic_waiting_lock::guard::guard(atomic_waiting_lock& atom, bool wait) : atom(atom) {
		if (wait) {
			std::unique_lock lock {this->atom.mtx};
			this->atom.cv.wait(lock, [this] { return not this->atom.value; });
			this->atom.value = true;
		}
		else {
			std::lock_guard lock {this->atom.mtx};
			this->atom.value = true;
		}
		this->atom.cv.notify_one();
	}

	atomic_waiting_lock::guard::~guard() noexcept {
		{
			std::lock_guard lock {this->atom.mtx};
			this->atom.value = false;
		}
		this->atom.cv.notify_one();
	}

	void atomic_wait(const atomic_waiting_lock& atom, bool old) noexcept {
		atom.wait(old);
	}

	void atomic_wait_for(const atomic_waiting_lock& atom, bool old, const uint64_t wait_ms) {
		atom.wait_for(old, wait_ms);
	}

	string readfile(const std::filesystem::path& path, const string& fallback) {
		if (not fs::exists(path)) return fallback;
		string out;
		try {
			std::ifstream file(path);
			for (string readstr; getline(file, readstr); out += readstr);
		}
		catch (const std::exception& e) {
			Logger::error("readfile() : Exception when reading {} : {}", path, e.what());
			return fallback;
		}
		return (out.empty() ? fallback : out);
	}

	auto celsius_to(const long long& celsius, const string& scale) -> tuple<long long, string> {
		if (scale == "celsius")
			return {celsius, "°C"};
		else if (scale == "fahrenheit")
			return {(long long)round((double)celsius * 1.8 + 32), "°F"};
		else if (scale == "kelvin")
			return {(long long)round((double)celsius + 273.15), "K "};
		else if (scale == "rankine")
			return {(long long)round((double)celsius * 1.8 + 491.67), "°R"};
		return {0, ""};
	}

	string hostname() {
		char host[HOST_NAME_MAX];
		gethostname(host, HOST_NAME_MAX);
		host[HOST_NAME_MAX - 1] = '\0';
		return string{host};
	}

	string username() {
		auto user = getenv("LOGNAME");
		if (user == nullptr or strlen(user) == 0) user = getenv("USER");
		return (user != nullptr ? user : "");
	}

	DebugTimer::DebugTimer(string name, bool start, bool delayed_report)
			: name(std::move(name)), delayed_report(delayed_report) {
		if (start)
			this->start();
	}

	DebugTimer::~DebugTimer() {
		if (running)
			this->stop(true);
		this->force_report();
	}

	void DebugTimer::start() {
		if (running) return;
		running = true;
		start_time = time_micros();
	}

	void DebugTimer::stop(bool report) {
		if (not running) return;
		running = false;
		elapsed_time = time_micros() - start_time;
		if (report) this->report();
	}

	void DebugTimer::reset(bool restart) {
		running = false;
		start_time = 0;
		elapsed_time = 0;
		if (restart) this->start();
	}

	void DebugTimer::stop_rename_reset(const string &new_name, bool report, bool restart) {
		this->stop(report);
		name = new_name;
		this->reset(restart);
	}

	void DebugTimer::report() {
		string report_line;
		if (start_time == 0 and elapsed_time == 0)
			report_line = fmt::format("DebugTimer::report() warning -> Timer [{}] has not been started!", name);
		else if (running)
			report_line = fmt::format(custom_locale, "Timer [{}] (running) currently at {:L} μs", name, time_micros() - start_time);
		else
			report_line = fmt::format(custom_locale, "Timer [{}] took {:L} μs", name, elapsed_time);

		if (delayed_report)
			report_buffer.emplace_back(report_line);
		else
			Logger::debug("{}", report_line);
	}

	void DebugTimer::force_report() {
		if (report_buffer.empty()) return;
		for (const auto& line : report_buffer)
			Logger::debug("{}", line);
		report_buffer.clear();
	}

	uint64_t DebugTimer::elapsed() {
		if (running)
			return time_micros() - start_time;
		return elapsed_time;
	}

	bool DebugTimer::is_running() {
		return running;
	}

	//? --------------------------------------------- CUSTOM BOX LAYOUT ---------------------------------------------

	bool parseBoxesLayout(const string& layout, vector<LayoutRow>& rows, string* error) {
		rows.clear();
		auto fail = [&](const string& msg) {
			if (error != nullptr) *error = msg;
			rows.clear();
			return false;
		};

		vector<string> seen_names;
		for (const auto& row_str_raw : ssplit(layout, ';')) {
			string row_str { trim(row_str_raw) };
			if (row_str.empty()) continue;

			int row_weight = 1;
			string boxes_str = row_str;
			if (auto bar = row_str.find('|'); bar != string::npos) {
				string weight_str { trim(row_str.substr(0, bar)) };
				boxes_str = string(trim(row_str.substr(bar + 1)));
				if (weight_str.empty() or not isint(weight_str))
					return fail(R"(Invalid row weight in boxes_layout: ")" + row_str + '"');
				row_weight = std::stoi(weight_str);
				if (row_weight < 1)
					return fail(R"(Row weight must be a positive integer in boxes_layout: ")" + row_str + '"');
			}

			LayoutRow row{ row_weight, {} };
			for (const auto& box_str_raw : ssplit(boxes_str, '+')) {
				string box_str { trim(box_str_raw) };
				if (box_str.empty()) continue;

				int box_weight = 1;
				string name = box_str;
				if (auto colon = box_str.find(':'); colon != string::npos) {
					name = box_str.substr(0, colon);
					string weight_str { trim(box_str.substr(colon + 1)) };
					if (weight_str.empty() or not isint(weight_str))
						return fail(R"(Invalid box weight in boxes_layout: ")" + box_str + '"');
					box_weight = std::stoi(weight_str);
					if (box_weight < 1)
						return fail(R"(Box weight must be a positive integer in boxes_layout: ")" + box_str + '"');
				}

				if (not v_contains(Config::valid_boxes, name))
					return fail(R"(Unknown box name in boxes_layout: ")" + name + '"');
				if (v_contains(seen_names, name))
					return fail("Box \"" + name + "\" appears more than once in boxes_layout!");
				seen_names.push_back(name);

				row.boxes.push_back({ name, box_weight });
			}

			if (row.boxes.empty())
				return fail("Empty row in boxes_layout!");

			rows.push_back(std::move(row));
		}

		if (rows.empty())
			return fail("boxes_layout is empty!");

		return true;
	}

	bool boxesLayoutMatches(const vector<LayoutRow>& rows, const string& shown_boxes) {
		const auto shown = ssplit(shown_boxes);

		vector<string> layout_names;
		for (const auto& row : rows)
			for (const auto& box : row.boxes)
				layout_names.push_back(box.name);

		if (shown.size() != layout_names.size())
			return false;

		return std::ranges::all_of(shown, [&layout_names](const auto& name) { return v_contains(layout_names, name); });
	}

	auto solveBoxesLayout(const vector<LayoutRow>& rows, int term_width, int term_height) -> std::unordered_map<string, array<int, 4>> {
		std::unordered_map<string, array<int, 4>> result;
		if (rows.empty() or term_width <= 0 or term_height <= 0) return result;

		int total_row_weight = 0;
		for (const auto& row : rows) total_row_weight += row.weight;
		if (total_row_weight < 1) total_row_weight = 1;

		int y = 1;
		int height_used = 0;
		for (size_t r = 0; r < rows.size(); ++r) {
			const auto& row = rows[r];
			//? The last row absorbs any rounding remainder so rows always add up to term_height exactly
			int row_height = (r + 1 == rows.size())
				? term_height - height_used
				: max(1, (int)std::round((double)term_height * row.weight / total_row_weight));
			height_used += row_height;

			int total_box_weight = 0;
			for (const auto& box : row.boxes) total_box_weight += box.weight;
			if (total_box_weight < 1) total_box_weight = 1;

			int x = 1;
			int width_used = 0;
			for (size_t c = 0; c < row.boxes.size(); ++c) {
				const auto& box = row.boxes[c];
				//? The last box in a row absorbs any rounding remainder so a row's boxes fill it exactly
				int box_width = (c + 1 == row.boxes.size())
					? term_width - width_used
					: max(1, (int)std::round((double)term_width * box.weight / total_box_weight));
				width_used += box_width;

				result[box.name] = { x, y, box_width, row_height };
				x += box_width;
			}

			y += row_height;
		}

		return result;
	}
}

const string Fx::e = "\x1b[";		//* Escape sequence start
const string Fx::b = e + "1m";		//* Bold on/off
const string Fx::ub = e + "22m";	//* Bold off
const string Fx::d = e + "2m";		//* Dark on
const string Fx::ud = e + "22m";	//* Dark off
const string Fx::i = e + "3m";		//* Italic on
const string Fx::ui = e + "23m";	//* Italic off
const string Fx::ul = e + "4m";		//* Underline on
const string Fx::uul = e + "24m";	//* Underline off
const string Fx::bl = e + "5m";		//* Blink on
const string Fx::ubl = e + "25m";	//* Blink off
const string Fx::s = e + "9m";		//* Strike/crossed-out on
const string Fx::us = e + "29m";	//* Strike/crossed-out on/off

//* Reset foreground/background color and text effects
const string Fx::reset_base = e + "0m";

//* Regex for matching color, style and cursor move escape sequences
const std::regex Fx::escape_regex("\033\\[\\d+;?\\d?;?\\d*;?\\d*;?\\d*(m|f|s|u|C|D|A|B){1}");

//* Regex for matching only color and style escape sequences
const std::regex Fx::color_regex("\033\\[\\d+;?\\d?;?\\d*;?\\d*;?\\d*(m){1}");

const string Term::hide_cursor = Fx::e + "?25l";
const string Term::show_cursor = Fx::e + "?25h";
const string Term::alt_screen = Fx::e + "?1049h";
const string Term::normal_screen = Fx::e + "?1049l";
const string Term::clear = Fx::e + "2J" + Fx::e + "0;0f";
const string Term::clear_end = Fx::e + "0J";
const string Term::clear_begin = Fx::e + "1J";
const string Term::mouse_on = Fx::e + "?1002h" + Fx::e + "?1015h" + Fx::e + "?1006h"; //? Enable reporting of mouse position on click and release
const string Term::mouse_off = Fx::e + "?1002l" + Fx::e + "?1015l" + Fx::e + "?1006l";
const string Term::mouse_direct_on = Fx::e + "?1003h"; //? Enable reporting of mouse position at any movement
const string Term::mouse_direct_off = Fx::e + "?1003l";
const string Term::sync_start = Fx::e + "?2026h"; //? Start of terminal synchronized output
const string Term::sync_end = Fx::e + "?2026l"; //? End of terminal synchronized output
