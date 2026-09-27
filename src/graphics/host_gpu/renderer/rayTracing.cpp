#include "graphics/host_gpu/renderer/rayTracing.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>

namespace Libs::Graphics {

namespace {

// The spec-fixed 128-byte VkAccelerationStructureInstanceKHR layout: a 3x4
// row-major transform, then two packed 32-bit words and the referenced BLAS
// device address. Writing this by hand keeps guest instance data uploadable
// without depending on the exact vulkan.hpp struct padding.
struct InstanceRecord {
	std::array<float, 12> transform;
	uint32_t              custom_index_and_mask;
	uint32_t              sbt_offset_and_flags;
	uint64_t              acceleration_structure_reference;
};
static_assert(sizeof(InstanceRecord) == 64);
static_assert(alignof(InstanceRecord) == 8);

constexpr vk::BuildAccelerationStructureFlagsKHR BuildFlags(bool prefer_fast_build) {
	return prefer_fast_build ? vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild
	                         : vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
}

[[nodiscard]] vk::AccelerationStructureGeometryKHR
MakeTriangleGeometry(const RayTracingGeometry& geometry) {
	vk::AccelerationStructureGeometryKHR geometry_info {};
	geometry_info.geometryType = vk::GeometryTypeKHR::eTriangles;

	vk::AccelerationStructureGeometryTrianglesDataKHR triangles {};
	triangles.vertexFormat             = vk::Format::eR32G32B32Sfloat;
	triangles.vertexData.deviceAddress = geometry.vertex_address;
	triangles.vertexStride             = geometry.vertex_stride;
	triangles.maxVertex                = geometry.vertex_count > 0 ? geometry.vertex_count - 1 : 0;
	triangles.indexType = geometry.index_address != 0
	                          ? (geometry.index_16bit ? vk::IndexType::eUint16
	                                                  : vk::IndexType::eUint32)
	                          : vk::IndexType::eNoneKHR;
	triangles.indexData.deviceAddress    = geometry.index_address;
	triangles.transformData.deviceAddress = 0;

	geometry_info.geometry.triangles = triangles;
	geometry_info.flags = geometry.opaque
	                          ? vk::GeometryFlagBitsKHR::eOpaque
	                          : vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
	return geometry_info;
}

[[nodiscard]] uint32_t BlasPrimitiveCount(const RayTracingGeometry& geometry) {
	return geometry.index_address != 0 ? geometry.index_count / 3u : geometry.vertex_count / 3u;
}

[[nodiscard]] vk::AccelerationStructureBuildRangeInfoKHR
MakeBlasBuildRange(const RayTracingGeometry& geometry) {
	vk::AccelerationStructureBuildRangeInfoKHR range {};
	range.primitiveCount  = BlasPrimitiveCount(geometry);
	range.primitiveOffset = 0;
	range.firstVertex     = 0;
	range.transformOffset = 0;
	return range;
}

// Transient build scratch memory; destroyed with the guard scope.
struct ScratchAllocation {
	vk::Buffer        buffer      = nullptr;
	VmaAllocation     allocation  = nullptr;
	vk::DeviceAddress address     = 0;
	uint64_t          size        = 0;
};

class ScratchGuard {
public:
	ScratchGuard(GraphicContext& graphics, ScratchAllocation allocation)
	    : m_graphics(graphics), m_allocation(allocation) {}
	~ScratchGuard() {
		if (m_allocation.buffer != nullptr) {
			vmaDestroyBuffer(m_graphics.allocator, m_allocation.buffer, m_allocation.allocation);
		}
	}
	KYTY_CLASS_NO_COPY(ScratchGuard);
	[[nodiscard]] const ScratchAllocation& get() const noexcept { return m_allocation; }

private:
	GraphicContext&   m_graphics;
	ScratchAllocation m_allocation;
};

[[nodiscard]] ScratchAllocation AllocateScratch(GraphicContext& graphics, uint64_t size) {
	ScratchAllocation scratch {};
	if (size == 0) {
		return scratch;
	}

	vk::BufferCreateInfo buffer_info {};
	buffer_info.size  = size;
	buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer |
	                    vk::BufferUsageFlagBits::eShaderDeviceAddress;

	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkBuffer          native_buffer = VK_NULL_HANDLE;
	VmaAllocationInfo allocation_result {};
	const auto result = static_cast<vk::Result>(vmaCreateBuffer(
	    graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer_info), &allocation_info,
	    &native_buffer, &scratch.allocation, &allocation_result));
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	scratch.buffer = native_buffer;
	scratch.size   = size;

	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer = scratch.buffer;
	scratch.address     = graphics.device.getBufferAddress(address_info);
	return scratch;
}

[[nodiscard]] vk::Buffer CreateDedicatedBuffer(GraphicContext& graphics, uint64_t size,
                                               vk::BufferUsageFlags usage,
                                               VmaAllocation*      out_allocation) {
	vk::BufferCreateInfo buffer_info {};
	buffer_info.size  = size;
	buffer_info.usage = usage;

	VmaAllocationCreateInfo allocation_info {};
	allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkBuffer      buffer    = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	const auto    result     = static_cast<vk::Result>(
        vmaCreateBuffer(graphics.allocator, static_cast<const VkBufferCreateInfo*>(buffer_info),
                        &allocation_info, &buffer, &allocation, nullptr));
	if (result != vk::Result::eSuccess) {
		graphics.LogMemoryBudget();
		EXIT_NOT_IMPLEMENTED(true);
	}
	*out_allocation = allocation;
	return buffer;
}

[[nodiscard]] vk::DeviceAddress BufferDeviceAddress(GraphicContext& graphics, vk::Buffer buffer) {
	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer = buffer;
	return graphics.device.getBufferAddress(address_info);
}

} // namespace

RayTracingBlas::~RayTracingBlas() {
	Destroy();
}

RayTracingBlas::RayTracingBlas(RayTracingBlas&& other) noexcept
    : m_graphics(other.m_graphics), m_acceleration(other.m_acceleration),
      m_device_address(other.m_device_address), m_allocation(other.m_allocation),
      m_buffer(other.m_buffer), m_build_size(other.m_build_size) {
	other.m_graphics       = nullptr;
	other.m_acceleration   = nullptr;
	other.m_device_address = 0;
	other.m_allocation     = nullptr;
	other.m_buffer         = nullptr;
	other.m_build_size     = 0;
}

RayTracingBlas& RayTracingBlas::operator=(RayTracingBlas&& other) noexcept {
	if (this == &other) {
		return *this;
	}
	Destroy();
	m_graphics       = other.m_graphics;
	m_acceleration   = other.m_acceleration;
	m_device_address = other.m_device_address;
	m_allocation     = other.m_allocation;
	m_buffer         = other.m_buffer;
	m_build_size     = other.m_build_size;
	other.m_graphics       = nullptr;
	other.m_acceleration   = nullptr;
	other.m_device_address = 0;
	other.m_allocation     = nullptr;
	other.m_buffer         = nullptr;
	other.m_build_size     = 0;
	return *this;
}

void RayTracingBlas::Destroy() noexcept {
	if (m_acceleration != nullptr && m_graphics != nullptr) {
		m_graphics->device.destroyAccelerationStructureKHR(m_acceleration, nullptr);
	}
	if (m_buffer != nullptr && m_graphics != nullptr && m_graphics->allocator != nullptr) {
		vmaDestroyBuffer(m_graphics->allocator, m_buffer, m_allocation);
	}
	m_graphics       = nullptr;
	m_acceleration   = nullptr;
	m_device_address = 0;
	m_allocation     = nullptr;
	m_buffer         = nullptr;
	m_build_size     = 0;
}

RayTracingBlas::RayTracingBlas(GraphicContext& graphics, vk::AccelerationStructureKHR acceleration,
                               vk::DeviceAddress device_address, VmaAllocation allocation,
                               vk::Buffer buffer, uint64_t build_size)
    : m_graphics(&graphics), m_acceleration(acceleration), m_device_address(device_address),
      m_allocation(allocation), m_buffer(buffer), m_build_size(build_size) {}

RayTracingTlas::~RayTracingTlas() {
	Destroy();
}

RayTracingTlas::RayTracingTlas(RayTracingTlas&& other) noexcept
    : m_graphics(other.m_graphics), m_acceleration(other.m_acceleration),
      m_device_address(other.m_device_address), m_allocation(other.m_allocation),
      m_buffer(other.m_buffer), m_instance_allocation(other.m_instance_allocation),
      m_instance_buffer(other.m_instance_buffer), m_instance_count(other.m_instance_count) {
	other.m_graphics            = nullptr;
	other.m_acceleration        = nullptr;
	other.m_device_address      = 0;
	other.m_allocation          = nullptr;
	other.m_buffer              = nullptr;
	other.m_instance_allocation = nullptr;
	other.m_instance_buffer     = nullptr;
	other.m_instance_count      = 0;
}

RayTracingTlas& RayTracingTlas::operator=(RayTracingTlas&& other) noexcept {
	if (this == &other) {
		return *this;
	}
	Destroy();
	m_graphics            = other.m_graphics;
	m_acceleration        = other.m_acceleration;
	m_device_address      = other.m_device_address;
	m_allocation          = other.m_allocation;
	m_buffer              = other.m_buffer;
	m_instance_allocation = other.m_instance_allocation;
	m_instance_buffer     = other.m_instance_buffer;
	m_instance_count      = other.m_instance_count;
	other.m_graphics            = nullptr;
	other.m_acceleration        = nullptr;
	other.m_device_address      = 0;
	other.m_allocation          = nullptr;
	other.m_buffer              = nullptr;
	other.m_instance_allocation = nullptr;
	other.m_instance_buffer     = nullptr;
	other.m_instance_count      = 0;
	return *this;
}

void RayTracingTlas::Destroy() noexcept {
	if (m_acceleration != nullptr && m_graphics != nullptr) {
		m_graphics->device.destroyAccelerationStructureKHR(m_acceleration, nullptr);
	}
	if (m_graphics != nullptr && m_graphics->allocator != nullptr) {
		if (m_buffer != nullptr) {
			vmaDestroyBuffer(m_graphics->allocator, m_buffer, m_allocation);
		}
		if (m_instance_buffer != nullptr) {
			vmaDestroyBuffer(m_graphics->allocator, m_instance_buffer, m_instance_allocation);
		}
	}
	m_graphics            = nullptr;
	m_acceleration        = nullptr;
	m_device_address      = 0;
	m_allocation          = nullptr;
	m_buffer              = nullptr;
	m_instance_allocation = nullptr;
	m_instance_buffer     = nullptr;
	m_instance_count      = 0;
}

RayTracingTlas::RayTracingTlas(GraphicContext& graphics, vk::AccelerationStructureKHR acceleration,
                               vk::DeviceAddress device_address, VmaAllocation allocation,
                               vk::Buffer buffer, VmaAllocation instance_allocation,
                               vk::Buffer instance_buffer, uint32_t instance_count)
    : m_graphics(&graphics), m_acceleration(acceleration), m_device_address(device_address),
      m_allocation(allocation), m_buffer(buffer), m_instance_allocation(instance_allocation),
      m_instance_buffer(instance_buffer), m_instance_count(instance_count) {}

RayTracingManager::RayTracingManager(GraphicContext& graphics): m_graphics(graphics) {}

RayTracingManager::~RayTracingManager() = default;

vk::AccelerationStructureBuildSizesInfoKHR
RayTracingManager::QueryBlasBuildSizes(const RayTracingBlasBuildInfo& info) const {
	std::vector<vk::AccelerationStructureGeometryKHR> geometries;
	geometries.reserve(info.geometries.size());
	std::vector<uint32_t> primitive_counts;
	primitive_counts.reserve(info.geometries.size());
	for (const auto& geometry: info.geometries) {
		geometries.push_back(MakeTriangleGeometry(geometry));
		primitive_counts.push_back(BlasPrimitiveCount(geometry));
	}

	vk::AccelerationStructureBuildGeometryInfoKHR build_geometry {};
	build_geometry.type          = vk::AccelerationStructureTypeKHR::eBottomLevel;
	build_geometry.flags         = BuildFlags(info.prefer_fast_build);
	build_geometry.geometryCount = static_cast<uint32_t>(geometries.size());
	build_geometry.pGeometries   = geometries.data();

	return m_graphics.device.getAccelerationStructureBuildSizesKHR(
	    vk::AccelerationStructureBuildTypeKHR::eDevice, build_geometry, primitive_counts);
}

vk::AccelerationStructureBuildSizesInfoKHR
RayTracingManager::QueryTlasBuildSizes(uint32_t instance_count) const {
	vk::AccelerationStructureGeometryInstancesDataKHR instances_data {};
	vk::AccelerationStructureGeometryKHR instance_geometry {};
	instance_geometry.geometryType       = vk::GeometryTypeKHR::eInstances;
	instance_geometry.geometry.instances = instances_data;

	vk::AccelerationStructureBuildGeometryInfoKHR build_geometry {};
	build_geometry.type          = vk::AccelerationStructureTypeKHR::eTopLevel;
	build_geometry.flags         = BuildFlags(false);
	build_geometry.geometryCount = 1;
	build_geometry.pGeometries   = &instance_geometry;

	return m_graphics.device.getAccelerationStructureBuildSizesKHR(
	    vk::AccelerationStructureBuildTypeKHR::eDevice, build_geometry, instance_count);
}

RayTracingBlas RayTracingManager::CreateBottomLevel(const RayTracingBlasBuildInfo& info) {
	if (!IsSupported() || info.geometries.empty()) {
		return {};
	}

	uint64_t total_primitives = 0;
	for (const auto& geometry: info.geometries) {
		if (geometry.vertex_address == 0) {
			return {};
		}
		total_primitives += BlasPrimitiveCount(geometry);
	}
	if (total_primitives == 0) {
		return {};
	}

	const auto build_sizes = QueryBlasBuildSizes(info);
	EXIT_IF(build_sizes.accelerationStructureSize == 0);

	// Acceleration structure storage is always device-local, addressable by
	// shaders so recompiled guest code can bind it through descriptors.
	VmaAllocation allocation    = nullptr;
	const auto    storage_buffer = CreateDedicatedBuffer(
        m_graphics, build_sizes.accelerationStructureSize,
        vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        &allocation);

	vk::AccelerationStructureCreateInfoKHR create_info {};
	create_info.type   = vk::AccelerationStructureTypeKHR::eBottomLevel;
	create_info.buffer = storage_buffer;
	create_info.offset = 0;
	create_info.size   = build_sizes.accelerationStructureSize;

	vk::AccelerationStructureKHR acceleration = nullptr;
	RequireVulkanSuccess(m_graphics.device.createAccelerationStructureKHR(&create_info, nullptr,
	                                                                      &acceleration),
	                     "create bottom level acceleration structure");

	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = acceleration;
	const auto device_address = m_graphics.device.getAccelerationStructureAddressKHR(address_info);
	EXIT_IF(device_address == 0);

	return RayTracingBlas(m_graphics, acceleration, device_address, allocation, storage_buffer,
	                      build_sizes.accelerationStructureSize);
}

void RayTracingManager::RecordBottomLevelBuild(CommandBuffer&                 command,
                                               RayTracingBlas&                blas,
                                               const RayTracingBlasBuildInfo& info) {
	EXIT_IF(!blas.IsValid());
	if (!IsSupported() || info.geometries.empty()) {
		return;
	}

	const auto build_sizes = QueryBlasBuildSizes(info);
	ScratchGuard scratch_guard(m_graphics, AllocateScratch(m_graphics, build_sizes.buildScratchSize));

	std::vector<vk::AccelerationStructureGeometryKHR> geometries;
	geometries.reserve(info.geometries.size());
	std::vector<vk::AccelerationStructureBuildRangeInfoKHR> ranges;
	ranges.reserve(info.geometries.size());
	for (const auto& geometry: info.geometries) {
		geometries.push_back(MakeTriangleGeometry(geometry));
		ranges.push_back(MakeBlasBuildRange(geometry));
	}
	std::vector<vk::AccelerationStructureBuildRangeInfoKHR*> range_pointers;
	range_pointers.reserve(ranges.size());
	for (auto& range: ranges) {
		range_pointers.push_back(&range);
	}

	vk::AccelerationStructureBuildGeometryInfoKHR build_geometry {};
	build_geometry.type                     = vk::AccelerationStructureTypeKHR::eBottomLevel;
	build_geometry.flags                    = BuildFlags(info.prefer_fast_build);
	build_geometry.mode                     = vk::BuildAccelerationStructureModeKHR::eBuild;
	build_geometry.dstAccelerationStructure = blas.Handle();
	build_geometry.geometryCount            = static_cast<uint32_t>(geometries.size());
	build_geometry.pGeometries              = geometries.data();
	build_geometry.scratchData.deviceAddress = scratch_guard.get().address;

	// Builds are recorded on the command buffer (vkCmdBuildAccelerationStructuresKHR).
	command.Handle().buildAccelerationStructuresKHR(1, &build_geometry, range_pointers.data());
}

RayTracingTlas RayTracingManager::BuildTopLevel(CommandBuffer&                 command,
                                                const RayTracingTlasBuildInfo& info) {
	EXIT_IF(!IsSupported());
	if (info.instances.empty()) {
		return {};
	}

	const auto build_sizes = QueryTlasBuildSizes(static_cast<uint32_t>(info.instances.size()));

	// The instance buffer feeds the build as a read-only device address input
	// and must stay alive as long as the TLAS may be rebuilt or traced.
	vk::BufferCreateInfo instance_buffer_info {};
	instance_buffer_info.size = info.instances.size() * sizeof(InstanceRecord);
	instance_buffer_info.usage =
	    vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
	    vk::BufferUsageFlagBits::eShaderDeviceAddress;

	VmaAllocationCreateInfo instance_allocation_info {};
	instance_allocation_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	instance_allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkBuffer      instance_buffer    = VK_NULL_HANDLE;
	VmaAllocation instance_allocation = nullptr;
	const auto    instance_result     = static_cast<vk::Result>(vmaCreateBuffer(
        m_graphics.allocator, static_cast<const VkBufferCreateInfo*>(instance_buffer_info),
        &instance_allocation_info, &instance_buffer, &instance_allocation, nullptr));
	EXIT_NOT_IMPLEMENTED(instance_result != vk::Result::eSuccess);

	{
		void* mapped = nullptr;
		const auto map_result =
		    static_cast<vk::Result>(vmaMapMemory(m_graphics.allocator, instance_allocation, &mapped));
		EXIT_NOT_IMPLEMENTED(map_result != vk::Result::eSuccess);
		auto* records = static_cast<InstanceRecord*>(mapped);
		for (size_t i = 0; i < info.instances.size(); i++) {
			const auto& instance = info.instances[i];
			EXIT_IF(instance.blas_device_address == 0);
			auto&       record = records[i];
			record.transform   = instance.transform;
			record.custom_index_and_mask =
			    (instance.instance_id & 0xffffffu) | (static_cast<uint32_t>(instance.instance_mask) << 24u);
			record.sbt_offset_and_flags =
			    (instance.hit_group_offset & 0xffffffu) | ((instance.flags & 0xffu) << 24u);
			record.acceleration_structure_reference = instance.blas_device_address;
		}
		vmaUnmapMemory(m_graphics.allocator, instance_allocation);
		vmaFlushAllocation(m_graphics.allocator, instance_allocation, 0, VK_WHOLE_SIZE);
	}

	// TLAS storage is separate from the instance input buffer.
	VmaAllocation storage_allocation = nullptr;
	const auto    storage_buffer     = CreateDedicatedBuffer(
        m_graphics, build_sizes.accelerationStructureSize,
        vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
            vk::BufferUsageFlagBits::eShaderDeviceAddress,
        &storage_allocation);

	vk::AccelerationStructureCreateInfoKHR create_info {};
	create_info.type   = vk::AccelerationStructureTypeKHR::eTopLevel;
	create_info.buffer = storage_buffer;
	create_info.offset = 0;
	create_info.size   = build_sizes.accelerationStructureSize;

	vk::AccelerationStructureKHR acceleration = nullptr;
	RequireVulkanSuccess(m_graphics.device.createAccelerationStructureKHR(&create_info, nullptr,
	                                                                      &acceleration),
	                     "create top level acceleration structure");

	vk::AccelerationStructureGeometryInstancesDataKHR instances_data {};
	instances_data.data.deviceAddress = BufferDeviceAddress(m_graphics, instance_buffer);

	vk::AccelerationStructureGeometryKHR instance_geometry {};
	instance_geometry.geometryType       = vk::GeometryTypeKHR::eInstances;
	instance_geometry.geometry.instances = instances_data;

	ScratchGuard scratch_guard(m_graphics, AllocateScratch(m_graphics, build_sizes.buildScratchSize));

	vk::AccelerationStructureBuildGeometryInfoKHR build_geometry {};
	build_geometry.type                     = vk::AccelerationStructureTypeKHR::eTopLevel;
	build_geometry.flags                    = BuildFlags(info.prefer_fast_build);
	build_geometry.mode                     = vk::BuildAccelerationStructureModeKHR::eBuild;
	build_geometry.dstAccelerationStructure = acceleration;
	build_geometry.geometryCount            = 1;
	build_geometry.pGeometries              = &instance_geometry;
	build_geometry.scratchData.deviceAddress = scratch_guard.get().address;

	uint32_t                                   primitive_count = static_cast<uint32_t>(info.instances.size());
	vk::AccelerationStructureBuildRangeInfoKHR range {};
	range.primitiveCount                    = primitive_count;
	vk::AccelerationStructureBuildRangeInfoKHR* range_pointer = &range;

	command.Handle().buildAccelerationStructuresKHR(1, &build_geometry, &range_pointer);

	vk::AccelerationStructureDeviceAddressInfoKHR address_info {};
	address_info.accelerationStructure = acceleration;
	const auto device_address = m_graphics.device.getAccelerationStructureAddressKHR(address_info);
	EXIT_IF(device_address == 0);

	return RayTracingTlas(m_graphics, acceleration, device_address, storage_allocation,
	                      storage_buffer, instance_allocation, instance_buffer,
	                      static_cast<uint32_t>(info.instances.size()));
}

} // namespace Libs::Graphics
