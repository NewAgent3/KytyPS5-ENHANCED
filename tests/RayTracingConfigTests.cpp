#include "common/emulatorConfig.h"

#include "common/subsystems.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "RayTracingConfigTests: failed: %s\n", message);
		std::abort();
	}
}

void TestDefaults() {
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();

	Config::ConfigOptions options;
	Config::Load(options);

	Check(Config::GetRayTracingMode() == Config::RayTracingMode::Auto,
	      "ray tracing must default to Auto");
	Check(Config::GetRayTracingMaxRecursion() == 4u, "default recursion depth must be 4");
}

void TestModeAndRecursionRoundTrip() {
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();

	Config::ConfigOptions options;
	options.ray_tracing_mode          = Config::RayTracingMode::Enabled;
	options.ray_tracing_max_recursion = 7;
	Config::Load(options);

	Check(Config::GetRayTracingMode() == Config::RayTracingMode::Enabled,
	      "Enabled mode did not round trip");
	Check(Config::GetRayTracingMaxRecursion() == 7u, "recursion depth did not round trip");

	// Out-of-range values clamp into the Vulkan-legal recursion range.
	options.ray_tracing_max_recursion = 0;
	Config::Load(options);
	Check(Config::GetRayTracingMaxRecursion() == 1u, "recursion depth must clamp up to 1");

	options.ray_tracing_max_recursion = 128;
	Config::Load(options);
	Check(Config::GetRayTracingMaxRecursion() == 31u,
	      "recursion depth must clamp down to the Vulkan maximum of 31");
}

void TestFreshLoadResetsMode() {
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();

	Config::ConfigOptions options;
	options.ray_tracing_mode = Config::RayTracingMode::Disabled;
	Config::Load(options);
	Check(Config::GetRayTracingMode() == Config::RayTracingMode::Disabled,
	      "Disabled mode did not round trip");

	// A fresh load must reset the mode; config state is a single global that
	// Load overwrites wholesale.
	Config::ConfigOptions reloaded;
	Config::Load(reloaded);
	Check(Config::GetRayTracingMode() == Config::RayTracingMode::Auto,
	      "a fresh config load must reset the ray tracing mode");
}

} // namespace

int main() {
	TestDefaults();
	TestModeAndRecursionRoundTrip();
	TestFreshLoadResetsMode();
	std::puts("RayTracingConfigTests: all cases passed");
	return 0;
}
