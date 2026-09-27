#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACINGPIPELINE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACINGPIPELINE_H_

#include "common/abi.h"
#include "common/alignment.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;

// One record of the shader binding table (SBT). Guest ray-tracing programs are
// recompiled into four SPIR-V stage groups; only raygen, hit, and miss reach
// the SBT, callable shaders are inlined by the recompiler.
struct ShaderBindingTableRecord {
	uint32_t group_index = 0; // index into the pipeline's shader group list
	uint32_t offset      = 0; // byte offset inside the SBT region
};

// Description of a ray tracing pipeline to create or look up. The key is the
// tuple of recompiled module handles; identical module tuples map to one
// pipeline because Vulkan RT pipelines are fully described by their stages.
struct RayTracingPipelineDesc {
	vk::ShaderModule raygen_module = nullptr;
	vk::ShaderModule miss_module   = nullptr;
	vk::ShaderModule hit_module    = nullptr;
	vk::ShaderModule closest_hit_module = nullptr;
	vk::PipelineLayout pipeline_layout = nullptr;
	uint32_t           max_recursion_depth = 1;

	bool operator==(const RayTracingPipelineDesc&) const = default;
};

// The shader binding table regions handed to vkCmdTraceRaysKHR.
struct ShaderBindingTableRegions {
	vk::StridedDeviceAddressRegionKHR raygen {};
	vk::StridedDeviceAddressRegionKHR miss {};
	vk::StridedDeviceAddressRegionKHR hit {};
	vk::StridedDeviceAddressRegionKHR callable {}; // always empty

	[[nodiscard]] bool IsValid() const noexcept { return raygen.deviceAddress != 0; }
};

// A compiled ray tracing pipeline plus its shader binding table buffer.
class RayTracingPipeline {
public:
	RayTracingPipeline() = default;
	~RayTracingPipeline();
	RayTracingPipeline(RayTracingPipeline&& other) noexcept;
	RayTracingPipeline& operator=(RayTracingPipeline&& other) noexcept;
	RayTracingPipeline(const RayTracingPipeline&)            = delete;
	RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;

	[[nodiscard]] bool                       IsValid() const noexcept { return m_pipeline != nullptr; }
	[[nodiscard]] vk::Pipeline               Handle() const noexcept { return m_pipeline; }
	[[nodiscard]] ShaderBindingTableRegions  Regions() const noexcept { return m_regions; }
	[[nodiscard]] uint32_t                   GroupCount() const noexcept { return m_group_count; }

private:
	explicit RayTracingPipeline(GraphicContext& graphics, vk::Pipeline pipeline,
	                            vk::Buffer sbt_buffer, VmaAllocation sbt_allocation,
	                            vk::DeviceAddress sbt_address, uint64_t sbt_size,
	                            uint32_t handle_size, uint32_t group_count,
	                            ShaderBindingTableRegions regions);
	void Reset() noexcept;
	void Destroy() noexcept;

	GraphicContext* m_graphics       = nullptr;
	vk::Pipeline   m_pipeline       = nullptr;
	vk::Buffer     m_sbt_buffer     = nullptr;
	VmaAllocation  m_sbt_allocation = nullptr;
	vk::DeviceAddress m_sbt_address = 0;
	uint64_t       m_sbt_size       = 0;
	uint32_t       m_handle_size    = 0;
	uint32_t       m_group_count    = 0;
	ShaderBindingTableRegions m_regions {};

	friend class RayTracingPipelineCache;
};

// Caches ray tracing pipelines keyed by their stage module tuple. The SBT is
// rebuilt whenever the group order changes; group handles are fetched from the
// pipeline once and packed into a single device-local buffer with
// shaderGroupHandleAlignment stride.
class RayTracingPipelineCache {
public:
	explicit RayTracingPipelineCache(GraphicContext& graphics);
	~RayTracingPipelineCache();
	KYTY_CLASS_NO_COPY(RayTracingPipelineCache);

	// Returns a cached pipeline or creates it. Returns an invalid object when
	// the host has no ray tracing support; callers must fall back to skipping.
	[[nodiscard]] RayTracingPipeline GetPipeline(const RayTracingPipelineDesc& desc);

	// Records a trace-rays dispatch. raygen_x/y/z are guest workgroup counts.
	void TraceRays(CommandBuffer& command, const RayTracingPipeline& pipeline, uint32_t raygen_x,
	               uint32_t raygen_y, uint32_t raygen_z) const;

private:
	struct Key {
		uint64_t raygen = 0;
		uint64_t miss   = 0;
		uint64_t hit    = 0;
		uint64_t layout = 0;
		uint32_t recursion = 0;

		bool operator==(const Key&) const = default;
	};

	// Returns a reference into the cache; the caller must not hold it across a
	// subsequent GetPipeline call on the same thread.
	[[nodiscard]] const RayTracingPipeline& FindPipeline(const Key& key);

	struct KeyHash {
		std::size_t operator()(const Key& key) const noexcept {
			std::size_t hash = key.raygen;
			hash ^= key.miss + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
			hash ^= key.hit + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
			hash ^= key.layout + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
			hash ^= key.recursion;
			return hash;
		}
	};

	GraphicContext& m_graphics;
	std::unordered_map<Key, RayTracingPipeline, KeyHash> m_pipelines;
	Common::Mutex m_mutex;

	[[nodiscard]] std::vector<std::array<uint8_t, 64>> FetchGroupHandles(
	    vk::Pipeline pipeline, uint32_t group_count) const;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACINGPIPELINE_H_
