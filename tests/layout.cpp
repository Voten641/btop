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

TEST(layout, cpu_full_width_by_default) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Config::set("shown_boxes", string("cpu mem net proc"));
#ifdef GPU_SUPPORT
	Config::set("gpu_cpu_side_by_side", false);
#endif

	Draw::calcSizes();

	EXPECT_EQ(Cpu::x, 1);
	EXPECT_EQ(Cpu::width, (int)Term::width);
}

#ifdef GPU_SUPPORT
TEST(layout, gpu_boxes_stack_below_cpu_by_default) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Gpu::count = 2;
	Gpu::gpu_names = {"Test GPU 0", "Test GPU 1"};
	Gpu::gpu_b_height_offsets = {2, 2};
	Config::set("gpu_cpu_side_by_side", false);
	Config::set("shown_boxes", string("cpu gpu0 gpu1 mem net proc"));

	Draw::calcSizes();

	ASSERT_EQ(Gpu::shown, 2);
	EXPECT_EQ(Cpu::width, (int)Term::width);
	for (int i = 0; i < Gpu::shown; ++i) {
		EXPECT_EQ(Gpu::x_vec[i], 1);
		EXPECT_EQ(Gpu::width, (int)Term::width);
		EXPECT_GT(Gpu::y_vec[i], Cpu::y);
	}
	//? Gpu column sits below cpu, so it must add to the vertical offset used by mem/net/proc
	EXPECT_GT(Gpu::total_height, 0);
	EXPECT_GT(Mem::y, Cpu::height);
}

TEST(layout, gpu_boxes_beside_cpu_when_enabled) {
	init_layout_env();
	Term::width = 200;
	Term::height = 50;
	Shared::coreCount = 8;
	Gpu::count = 2;
	Gpu::gpu_names = {"Test GPU 0", "Test GPU 1"};
	Gpu::gpu_b_height_offsets = {2, 2};
	Config::set("gpu_cpu_side_by_side", true);
	Config::set("shown_boxes", string("cpu gpu0 gpu1 mem net proc"));

	Draw::calcSizes();

	ASSERT_EQ(Gpu::shown, 2);
	//? Cpu box no longer spans the full width, gpu box(es) take the remainder of the row
	EXPECT_LT(Cpu::width, (int)Term::width);
	EXPECT_GT(Cpu::width, 0);
	EXPECT_EQ(Gpu::width, (int)Term::width - Cpu::width);

	for (int i = 0; i < Gpu::shown; ++i) {
		//? Gpu boxes start right where the cpu box ends horizontally...
		EXPECT_EQ(Gpu::x_vec[i], Cpu::x + Cpu::width);
		//? ...and are stacked vertically within that column, starting at the same row as cpu.
		EXPECT_GE(Gpu::y_vec[i], Cpu::y);
	}

	//? The gpu column sits beside cpu, not below it, so it must not add extra
	//? vertical offset for the boxes drawn underneath (mem/net/proc).
	EXPECT_EQ(Gpu::total_height, 0);
	EXPECT_EQ(Mem::y, Cpu::y + Cpu::height);

	//? Sanity: none of the resulting geometry should be degenerate.
	EXPECT_GT(Cpu::height, 0);
	for (int i = 0; i < Gpu::shown; ++i) {
		EXPECT_FALSE(Gpu::box[i].empty());
	}
}
#endif
