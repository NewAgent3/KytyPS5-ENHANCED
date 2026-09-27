#include "graphics/host_gpu/renderer/rayTracingPipeline.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <unordered_map>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

namespace {

// Group layout is fixed by the recompiled stage tuple:
//   group 0: raygen
//   group 1: miss
//   group 2: hit (closest-hit only; any-hit is inlined by the recompiler)
constexpr uint32_t RAYGEN_GROUP_INDEX = 0;
constexpr uint32_t MISS_GROUP_INDEX   = 1;
constexpr uint32_t HIT_GROUP_INDEX    = 2;
constexpr uint32_t GROUP_COUNT        = 3;

[[nodiscard]] uint64_t ModuleId(vk::ShaderModule module) {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
	    static_cast<vk::ShaderModule::CType>(module)));
}

[[nodiscard]] uint64_t LayoutId(vk::PipelineLayout layout) {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
	    static_cast<vk::PipelineLayout::CType>(layout)));
}

} // namespace

RayTracingPipeline::RayTracingPipeline(GraphicContext& graphics, vk::Pipeline pipeline,
                                       vk::Buffer sbt_buffer, VmaAllocation sbt_allocation,
                                       vk::DeviceAddress sbt_address, uint64_t sbt_size,
                                       uint32_t handle_size, uint32_t group_count,
                                       ShaderBindingTableRegions regions)
    : m_graphics(&graphics), m_pipeline(pipeline), m_sbt_buffer(sbt_buffer),
      m_sbt_allocation(sbt_allocation), m_sbt_address(sbt_address), m_sbt_size(sbt_size),
      m_handle_size(handle_size), m_group_count(group_count), m_regions(regions) {}

RayTracingPipeline::~RayTracingPipeline() {
	Destroy();
}

RayTracingPipeline::RayTracingPipeline(RayTracingPipeline&& other) noexcept
    : m_graphics(other.m_graphics), m_pipeline(other.m_pipeline),
      m_sbt_buffer(other.m_sbt_buffer), m_sbt_allocation(other.m_sbt_allocation),
      m_sbt_address(other.m_sbt_address), m_sbt_size(other.m_sbt_size),
      m_handle_size(other.m_handle_size), m_group_count(other.m_group_count),
      m_regions(other.m_regions) {
	other.Reset();
}

RayTracingPipeline& RayTracingPipeline::operator=(RayTracingPipeline&& other) noexcept {
	if (this == &other) {
		return *this;
	}
	Destroy();
	m_graphics       = other.m_graphics;
	m_pipeline       = other.m_pipeline;
	m_sbt_buffer     = other.m_sbt_buffer;
	m_sbt_allocation = other.m_sbt_allocation;
	m_sbt_address    = other.m_sbt_address;
	m_sbt_size       = other.m_sbt_size;
	m_handle_size    = other.m_handle_size;
	m_group_count    = other.m_group_count;
	m_regions        = other.m_regions;
	other.Reset();
	return *this;
}

void RayTracingPipeline::Reset() noexcept {
	m_pipeline       = nullptr;
	m_sbt_buffer     = nullptr;
	m_sbt_allocation = nullptr;
	m_sbt_address    = 0;
	m_sbt_size       = 0;
	m_handle_size    = 0;
	m_group_count    = 0;
	m_regions        = {};
}

void RayTracingPipeline::Destroy() noexcept {
	// The SBT buffer and the pipeline are destroyed together; the cached
	// pipeline's groups hold device addresses into the SBT, so neither may
	// outlive the other.
	vk::Pipeline   pipeline   = m_pipeline;
	vk::Buffer     sbt_buffer = m_sbt_buffer;
	VmaAllocation  allocation = m_sbt_allocation;
	Reset();
	if (m_graphics == nullptr || (pipeline == nullptr && sbt_buffer == nullptr)) {
		return;
	}
	if (pipeline != nullptr) {
		m_graphics->device.destroyPipeline(pipeline, nullptr);
	}
	if (sbt_buffer != nullptr && m_graphics->allocator != nullptr) {
		vmaDestroyBuffer(m_graphics->allocator, sbt_buffer, allocation);
	}
}

RayTracingPipelineCache::RayTracingPipelineCache(GraphicContext& graphics): m_graphics(graphics) {}

RayTracingPipelineCache::~RayTracingPipelineCache() = default;

std::vector<std::array<uint8_t, 64>> RayTracingPipelineCache::FetchGroupHandles(
    vk::Pipeline pipeline, uint32_t group_count) const {
	const auto handle_size = m_graphics.shader_group_handle_size;
	EXIT_IF(handle_size == 0 || handle_size > 64);

	std::vector<uint8_t> packed(static_cast<size_t>(handle_size) * group_count);
	const auto result = m_graphics.device.getRayTracingShaderGroupHandlesKHR(
	    pipeline, 0, group_count, packed.size(), packed.data());
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	std::vector<std::array<uint8_t, 64>> handles(group_count);
	for (uint32_t group = 0; group < group_count; group++) {
		std::memcpy(handles[group].data(), packed.data() + static_cast<size_t>(handle_size) * group,
		            handle_size);
	}
	return handles;
}

RayTracingPipeline RayTracingPipelineCache::GetPipeline(const RayTracingPipelineDesc& desc) {
	if (!m_graphics.ray_tracing_pipeline_enabled || desc.raygen_module == nullptr ||
	    desc.pipeline_layout == nullptr) {
		return {};
	}

	const Key key {ModuleId(desc.raygen_module), ModuleId(desc.miss_module),
	               ModuleId(desc.hit_module), LayoutId(desc.pipeline_layout),
	               desc.max_recursion_depth};

	Common::LockGuard lock(m_mutex);

	if (const auto existing = m_pipelines.find(key); existing != m_pipelines.end()) {
		return std::move(existing->second);
	}

	const auto handle_stride = Common::AlignUp(m_graphics.shader_group_handle_size,
	                                           m_graphics.shader_group_handle_alignment);

	// Stage + group description for the fixed raygen/miss/hit layout. Missing
	// miss or hit stages still reserve their groups so SBT offsets stay stable;
	// an empty group is legal as long as the trace never reaches it.
	std::array<vk::PipelineShaderStageCreateInfo, GROUP_COUNT> stages {};
	stages[RAYGEN_GROUP_INDEX].stage  = vk::ShaderStageFlagBits::eRaygenKHR;
	stages[RAYGEN_GROUP_INDEX].module = desc.raygen_module;
	stages[RAYGEN_GROUP_INDEX].pName  = "main";

	if (desc.miss_module != nullptr) {
		stages[MISS_GROUP_INDEX].stage  = vk::ShaderStageFlagBits::eMissKHR;
		stages[MISS_GROUP_INDEX].module = desc.miss_module;
		stages[MISS_GROUP_INDEX].pName  = "main";
	}
	if (desc.hit_module != nullptr) {
		stages[HIT_GROUP_INDEX].stage  = vk::ShaderStageFlagBits::eClosestHitKHR;
		stages[HIT_GROUP_INDEX].module = desc.hit_module;
		stages[HIT_GROUP_INDEX].pName  = "main";
	}

	std::array<vk::RayTracingShaderGroupCreateInfoKHR, GROUP_COUNT> groups {};
	groups[RAYGEN_GROUP_INDEX].type               = vk::RayTracingShaderGroupTypeKHR::eGeneral;
	groups[RAYGEN_GROUP_INDEX].generalShader      = RAYGEN_GROUP_INDEX;
	groups[RAYGEN_GROUP_INDEX].closestHitShader   = VK_SHADER_UNUSED_KHR;
	groups[RAYGEN_GROUP_INDEX].anyHitShader       = VK_SHADER_UNUSED_KHR;
	groups[RAYGEN_GROUP_INDEX].intersectionShader = VK_SHADER_UNUSED_KHR;

	groups[MISS_GROUP_INDEX].type               = vk::RayTracingShaderGroupTypeKHR::eGeneral;
	groups[MISS_GROUP_INDEX].generalShader =
	    desc.miss_module != nullptr ? MISS_GROUP_INDEX : VK_SHADER_UNUSED_KHR;
	groups[MISS_GROUP_INDEX].closestHitShader   = VK_SHADER_UNUSED_KHR;
	groups[MISS_GROUP_INDEX].anyHitShader       = VK_SHADER_UNUSED_KHR;
	groups[MISS_GROUP_INDEX].intersectionShader = VK_SHADER_UNUSED_KHR;

	groups[HIT_GROUP_INDEX].type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
	groups[HIT_GROUP_INDEX].generalShader        = VK_SHADER_UNUSED_KHR;
	groups[HIT_GROUP_INDEX].closestHitShader =
	    desc.hit_module != nullptr ? HIT_GROUP_INDEX : VK_SHADER_UNUSED_KHR;
	groups[HIT_GROUP_INDEX].anyHitShader         = VK_SHADER_UNUSED_KHR;
	groups[HIT_GROUP_INDEX].intersectionShader   = VK_SHADER_UNUSED_KHR;

	vk::RayTracingPipelineCreateInfoKHR create_info {};
	create_info.stageCount = GROUP_COUNT;
	create_info.pStages    = stages.data();
	create_info.groupCount = GROUP_COUNT;
	create_info.pGroups    = groups.data();
	create_info.maxPipelineRayRecursionDepth =
	    std::min(desc.max_recursion_depth, m_graphics.max_ray_recursion_depth);
	create_info.layout      = desc.pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(m_graphics.device.createRayTracingPipelinesKHR(nullptr, nullptr, 1,
	                                                                    &create_info, nullptr,
	                                                                    &pipeline),
	                     "create ray tracing pipeline");

	// One SBT record per group. Guest shaders address the SBT with offsets
	// derived from the group order, so the layout must stay fixed. The SBT is
	// tiny (three records), so a host-visible mapped buffer avoids a staging
	// upload entirely; the GPU reads it during traceRays like device memory.
	const auto sbt_size = static_cast<uint64_t>(GROUP_COUNT) * handle_stride;

	vk::BufferCreateInfo sbt_buffer_info {};
	sbt_buffer_info.size  = sbt_size;
	sbt_buffer_info.usage = vk::BufferUsageFlagBits::eShaderBindingTableKHR |
	                        vk::BufferUsageFlagBits::eShaderDeviceAddress |
	                        vk::BufferUsageFlagBits::eTransferDst;

	VmaAllocationCreateInfo sbt_allocation_info {};
	sbt_allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
	                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	sbt_allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

	VkBuffer          sbt_buffer     = VK_NULL_HANDLE;
	VmaAllocation     sbt_allocation = nullptr;
	VmaAllocationInfo sbt_info {};
	const auto        result         = static_cast<vk::Result>(vmaCreateBuffer(
        m_graphics.allocator, static_cast<const VkBufferCreateInfo*>(sbt_buffer_info),
        &sbt_allocation_info, &sbt_buffer, &sbt_allocation, &sbt_info));
	if (result != vk::Result::eSuccess) {
		m_graphics.device.destroyPipeline(pipeline, nullptr);
		m_graphics.LogMemoryBudget();
		EXIT_NOT_IMPLEMENTED(true);
	}

	// Pack the handles in group order (raygen, miss, hit). The SBT must be
	// complete before the first trace that references this pipeline.
	const auto handles = FetchGroupHandles(pipeline, GROUP_COUNT);
	EXIT_IF(sbt_info.pMappedData == nullptr);
	auto* sbt_bytes = static_cast<uint8_t*>(sbt_info.pMappedData);
	for (uint32_t group = 0; group < GROUP_COUNT; group++) {
		std::memcpy(sbt_bytes + static_cast<size_t>(handle_stride) * group, handles[group].data(),
		            m_graphics.shader_group_handle_size);
	}
	vmaFlushAllocation(m_graphics.allocator, sbt_allocation, 0, VK_WHOLE_SIZE);

	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer = sbt_buffer;
	const auto sbt_address = m_graphics.device.getBufferAddress(address_info);
	LOGF("Ray tracing pipeline: sbt_size=%" PRIu64 " handle_stride=%u\n", sbt_size, handle_stride);

	ShaderBindingTableRegions regions {};
	regions.raygen.deviceAddress = sbt_address;
	regions.raygen.stride        = handle_stride;
	regions.raygen.size          = handle_stride;
	regions.miss.deviceAddress   = sbt_address + static_cast<vk::DeviceAddress>(handle_stride);
	regions.miss.stride          = handle_stride;
	regions.miss.size            = handle_stride;
	regions.hit.deviceAddress    = sbt_address + static_cast<vk::DeviceAddress>(handle_stride) * 2;
	regions.hit.stride           = handle_stride;
	regions.hit.size             = handle_stride;

	RayTracingPipeline entry(m_graphics, pipeline, sbt_buffer, sbt_allocation, sbt_address,
	                         sbt_size, m_graphics.shader_group_handle_size, GROUP_COUNT, regions);
	m_pipelines.emplace(key, std::move(entry));
	return std::move(m_pipelines.find(key)->second);
}

const RayTracingPipeline& RayTracingPipelineCache::FindPipeline(const Key& key) {
	Common::LockGuard lock(m_mutex);
	static const RayTracingPipeline empty {};
	const auto it = m_pipelines.find(key);
	return it != m_pipelines.end() ? it->second : empty;
}

void RayTracingPipelineCache::TraceRays(CommandBuffer& command, const RayTracingPipeline& pipeline,
                                        uint32_t raygen_x, uint32_t raygen_y,
                                        uint32_t raygen_z) const {
	EXIT_IF(!pipeline.IsValid());
	const auto regions = pipeline.Regions();
	EXIT_IF(!regions.IsValid());

	command.Handle().traceRaysKHR(&regions.raygen, &regions.miss, &regions.hit, &regions.callable,
	                              raygen_x, raygen_y, raygen_z);
}

} // namespace Libs::Graphics
