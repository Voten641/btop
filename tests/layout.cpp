// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_shared.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"

namespace {

	//? Common setup needed before Draw::calcSizes() can run standalone in a test binary.
	void init_layout_env() {
		static bool done = false;
		if (done) return;
		done = true;
		Theme::updateThemes();
		Theme::setTheme();
	}

}

TEST(boxes_layout, parse_rejects_unknown_box) {
	vector<Tools::LayoutRow> rows;
	string error;
	EXPECT_FALSE(Tools::parseBoxesLayout("cpu+wat", rows, &error));
	EXPECT_TRUE(rows.empty());
	EXPECT_FALSE(error.empty());
}

TEST(boxes_layout, parse_rejects_duplicate_box) {
	vector<Tools::LayoutRow> rows;
	EXPECT_FALSE(Tools::parseBoxesLayout("cpu+mem;cpu", rows));
}

TEST(boxes_layout, parse_rejects_bad_weight) {
	vector<Tools::LayoutRow> rows;
	EXPECT_FALSE(Tools::parseBoxesLayout("cpu:0+mem", rows));
	EXPECT_FALSE(Tools::parseBoxesLayout("cpu:abc+mem", rows));
	EXPECT_FALSE(Tools::parseBoxesLayout("0|cpu+mem", rows));
}

TEST(boxes_layout, parse_reads_rows_and_weights) {
	vector<Tools::LayoutRow> rows;
	string error;
	ASSERT_TRUE(Tools::parseBoxesLayout(" 2|cpu+mem:3 ; net + proc ", rows, &error)) << error;
	ASSERT_EQ(rows.size(), 2u);

	EXPECT_EQ(rows[0].weight, 2);
	ASSERT_EQ(rows[0].boxes.size(), 2u);
	EXPECT_EQ(rows[0].boxes[0].name, "cpu");
	EXPECT_EQ(rows[0].boxes[0].weight, 1);
	EXPECT_EQ(rows[0].boxes[1].name, "mem");
	EXPECT_EQ(rows[0].boxes[1].weight, 3);

	EXPECT_EQ(rows[1].weight, 1);
	ASSERT_EQ(rows[1].boxes.size(), 2u);
	EXPECT_EQ(rows[1].boxes[0].name, "net");
	EXPECT_EQ(rows[1].boxes[1].name, "proc");
}

TEST(boxes_layout, matches_only_exact_same_set_of_boxes) {
	vector<Tools::LayoutRow> rows;
	ASSERT_TRUE(Tools::parseBoxesLayout("cpu+mem;net+proc", rows));

	EXPECT_TRUE(Tools::boxesLayoutMatches(rows, "cpu mem net proc"));
	EXPECT_TRUE(Tools::boxesLayoutMatches(rows, "proc net mem cpu")); // order doesn't matter
	EXPECT_FALSE(Tools::boxesLayoutMatches(rows, "cpu mem net")); // missing proc
}

TEST(boxes_layout, solve_splits_evenly_by_default) {
	vector<Tools::LayoutRow> rows;
	ASSERT_TRUE(Tools::parseBoxesLayout("cpu+mem;proc", rows));

	auto solved = Tools::solveBoxesLayout(rows, 100, 40);
	ASSERT_TRUE(solved.contains("cpu"));
	ASSERT_TRUE(solved.contains("mem"));
	ASSERT_TRUE(solved.contains("proc"));

	const auto& cpu = solved.at("cpu");
	const auto& mem = solved.at("mem");
	const auto& proc = solved.at("proc");

	//? cpu and mem share the top row (half the height), side by side (half the width each)
	EXPECT_EQ(cpu[0], 1); // x
	EXPECT_EQ(cpu[1], 1); // y
	EXPECT_EQ(cpu[2], 50); // width
	EXPECT_EQ(cpu[3], 20); // height

	EXPECT_EQ(mem[0], 51);
	EXPECT_EQ(mem[1], 1);
	EXPECT_EQ(mem[2], 50);
	EXPECT_EQ(mem[3], 20);

	//? proc spans the full width of the bottom row
	EXPECT_EQ(proc[0], 1);
	EXPECT_EQ(proc[1], 21);
	EXPECT_EQ(proc[2], 100);
	EXPECT_EQ(proc[3], 20);
}

TEST(boxes_layout, solve_honors_weights) {
	vector<Tools::LayoutRow> rows;
	ASSERT_TRUE(Tools::parseBoxesLayout("3|cpu+mem:2+net:1;1|proc", rows));

	auto solved = Tools::solveBoxesLayout(rows, 90, 80);
	const auto& cpu = solved.at("cpu");
	const auto& mem = solved.at("mem");
	const auto& net = solved.at("net");
	const auto& proc = solved.at("proc");

	//? Top row gets 3/4 of the height, bottom row 1/4
	EXPECT_EQ(cpu[3], 60);
	EXPECT_EQ(proc[3], 20);

	//? Within the top row: cpu:1, mem:2, net:1 -> quarters of the width
	EXPECT_EQ(cpu[2], 23); // round(90/4)
	EXPECT_EQ(mem[2], 45); // round(90*2/4)
	EXPECT_EQ(net[2], 90 - cpu[2] - mem[2]); // remainder absorbed by the last box in the row

	//? Rows tile the full terminal exactly, with no gaps or overlaps
	EXPECT_EQ(cpu[0] + cpu[2], mem[0]);
	EXPECT_EQ(mem[0] + mem[2], net[0]);
	EXPECT_EQ(net[0] + net[2], 91);
	EXPECT_EQ(cpu[1], 1);
	EXPECT_EQ(proc[1], cpu[1] + cpu[3]);
	EXPECT_EQ(proc[1] + proc[3], 81);
}

TEST(layout, default_layout_used_when_boxes_layout_is_empty) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Config::set("boxes_layout", string(""));
	Config::set("shown_boxes", string("cpu mem net proc"));

	Draw::calcSizes();

	EXPECT_EQ(Cpu::x, 1);
	EXPECT_EQ(Cpu::width, (int)Term::width);
}

TEST(layout, custom_layout_places_boxes_side_by_side) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Config::set("shown_boxes", string("cpu mem net proc"));
	Config::set("boxes_layout", string("cpu+mem;net+proc"));

	Draw::calcSizes();

	//? cpu and mem share the top row
	EXPECT_EQ(Cpu::x, 1);
	EXPECT_EQ(Mem::x, Cpu::x + Cpu::width);
	EXPECT_EQ(Mem::y, Cpu::y);
	EXPECT_EQ(Cpu::height, Mem::height);
	EXPECT_LT(Cpu::width, (int)Term::width);

	//? net and proc share the row below
	EXPECT_EQ(Net::y, Cpu::y + Cpu::height);
	EXPECT_EQ(Proc::x, Net::x + Net::width);
	EXPECT_EQ(Net::y, Proc::y);

	Config::set("boxes_layout", string(""));
}

TEST(layout, mismatched_custom_layout_falls_back_to_default) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	//? shown_boxes doesn't contain "proc", so this layout can't match it and must be ignored
	Config::set("boxes_layout", string("cpu+mem;net+proc"));
	Config::set("shown_boxes", string("cpu mem net"));

	Draw::calcSizes();

	//? Falls back to the default (legacy) layout: cpu spans the full width on its own row
	EXPECT_EQ(Cpu::x, 1);
	EXPECT_EQ(Cpu::width, (int)Term::width);

	Config::set("boxes_layout", string(""));
}

#ifdef GPU_SUPPORT
TEST(layout, custom_layout_places_gpu_boxes_beside_cpu) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Gpu::count = 2;
	Gpu::gpu_names = {"Test GPU 0", "Test GPU 1"};
	Gpu::gpu_b_height_offsets = {2, 2};
	Config::set("shown_boxes", string("cpu gpu0 gpu1 mem net proc"));
	Config::set("boxes_layout", string("cpu+gpu0+gpu1;mem+net;proc"));

	Draw::calcSizes();

	ASSERT_EQ(Gpu::shown, 2);
	//? cpu, gpu0 and gpu1 all share the top row, left to right.
	//? (Gpu::width is a single shared field, last written by gpu1, so only x/y positions -
	//? which are stored per panel - can be checked here.)
	EXPECT_EQ(Cpu::x, 1);
	EXPECT_EQ(Gpu::x_vec[0], Cpu::x + Cpu::width);
	EXPECT_GT(Gpu::x_vec[1], Gpu::x_vec[0]);
	EXPECT_EQ(Gpu::y_vec[0], Cpu::y);
	EXPECT_EQ(Gpu::y_vec[1], Cpu::y);

	//? mem/net/proc are unaffected, stacked below that shared row
	EXPECT_EQ(Mem::y, Cpu::y + Cpu::height);

	Config::set("boxes_layout", string(""));
}
#endif
