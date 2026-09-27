#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace Libs::Graphics {

class CommandBuffer;

// A single triangle mesh submitted by a guest for bottom-level acceleration
// structure (BLAS) creation. Geometry is read from guest memory as tightly
// packed float3 vertices, the most common layout guest runtimes emit through
// their RT resource descriptors.
struct RayTracingGeometry {
	uint64_t vertex_address = 0;  // guest device address of the vertex array
	uint64_t vertex_stride  = 12; // bytes per vertex (float3)
	uint32_t vertex_count   = 0;
	uint64_t index_address  = 0; // optional guest index buffer; 0 = triangle soup
	uint32_t index_count    = 0;
	bool     index_16bit    = false; // index format: false = uint32
	bool     opaque         = true;  // no-any-hit / opaque flag
};

struct RayTracingBlasBuildInfo {
	std::vector<RayTracingGeometry> geometries;
	bool prefer_fast_build         = false; // otherwise prefers fast trace
};

// Instance of a BLAS inside a top-level acceleration structure (TLAS).
struct RayTracingInstance {
	vk::DeviceAddress blas_device_address = 0;
	uint64_t          guest_address       = 0; // guest-side instance address for dedup
	std::array<float, 12> transform {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
	                                  0.f}; // row-major 3x4, column vectors
	uint32_t instance_id      = 0;   // gl_InstanceCustomIndexKHR
	uint8_t  instance_mask    = 0xff;
	uint32_t hit_group_offset = 0;   // instance contribution to the shader binding table offset
	uint32_t flags            = 0;   // vk::GeometryInstanceFlagBitsKHR (raw uint32)
};

struct RayTracingTlasBuildInfo {
	std::vector<RayTracingInstance> instances;
	bool prefer_fast_build = false;
};

// A bottom-level acceleration structure. The device address is stable for the
// lifetime of the object, so BLASes can be referenced by TLAS instances and by
// recompiled guest shaders through descriptor-bound acceleration structures.
class RayTracingBlas {
public:
	RayTracingBlas() = default;
	~RayTracingBlas();
	RayTracingBlas(RayTracingBlas&& other) noexcept;
	RayTracingBlas& operator=(RayTracingBlas&& other) noexcept;
	RayTracingBlas(const RayTracingBlas&)            = delete;
	RayTracingBlas& operator=(const RayTracingBlas&) = delete;

	[[nodiscard]] bool                          IsValid() const noexcept { return m_acceleration != nullptr; }
	[[nodiscard]] vk::AccelerationStructureKHR  Handle() const noexcept { return m_acceleration; }
	[[nodiscard]] vk::DeviceAddress             DeviceAddress() const noexcept { return m_device_address; }
	[[nodiscard]] uint64_t                      BuildSize() const noexcept { return m_build_size; }

private:
	RayTracingBlas(GraphicContext& graphics, vk::AccelerationStructureKHR acceleration,
	               vk::DeviceAddress device_address, VmaAllocation allocation, vk::Buffer buffer,
	               uint64_t build_size);
	void Destroy() noexcept;

	GraphicContext*              m_graphics       = nullptr;
	vk::AccelerationStructureKHR m_acceleration   = nullptr;
	vk::DeviceAddress            m_device_address = 0;
	VmaAllocation                m_allocation     = nullptr;
	vk::Buffer                   m_buffer         = nullptr;
	uint64_t                     m_build_size     = 0;

	friend class RayTracingManager;
};

// A top-level acceleration structure owning its storage and instance buffer.
class RayTracingTlas {
public:
	RayTracingTlas() = default;
	~RayTracingTlas();
	RayTracingTlas(RayTracingTlas&& other) noexcept;
	RayTracingTlas& operator=(RayTracingTlas&& other) noexcept;
	RayTracingTlas(const RayTracingTlas&)            = delete;
	RayTracingTlas& operator=(const RayTracingTlas&) = delete;

	[[nodiscard]] bool                         IsValid() const noexcept { return m_acceleration != nullptr; }
	[[nodiscard]] vk::AccelerationStructureKHR Handle() const noexcept { return m_acceleration; }
	[[nodiscard]] vk::DeviceAddress            DeviceAddress() const noexcept { return m_device_address; }
	[[nodiscard]] uint32_t                     InstanceCount() const noexcept { return m_instance_count; }

private:
	RayTracingTlas(GraphicContext& graphics, vk::AccelerationStructureKHR acceleration,
	               vk::DeviceAddress device_address, VmaAllocation allocation, vk::Buffer buffer,
	               VmaAllocation instance_allocation, vk::Buffer instance_buffer,
	               uint32_t instance_count);
	void Destroy() noexcept;

	GraphicContext*              m_graphics            = nullptr;
	vk::AccelerationStructureKHR m_acceleration        = nullptr;
	vk::DeviceAddress            m_device_address      = 0;
	VmaAllocation                m_allocation          = nullptr;
	vk::Buffer                   m_buffer              = nullptr;
	VmaAllocation                m_instance_allocation = nullptr;
	vk::Buffer                   m_instance_buffer     = nullptr;
	uint32_t                     m_instance_count      = 0;

	friend class RayTracingManager;
};

// Owns guest-facing ray tracing acceleration structure builds. Builds are
// issued on the graphics queue through the regular command stream; scratch
// memory is allocated transiently per build from the same VMA allocator.
class RayTracingManager {
public:
	explicit RayTracingManager(GraphicContext& graphics);
	~RayTracingManager();
	KYTY_CLASS_NO_COPY(RayTracingManager);

	[[nodiscard]] bool IsSupported() const noexcept { return m_graphics.ray_tracing_enabled; }

	// Creates a BLAS object without recording its build; geometry is only used
	// to size the structure. Returns an empty object when ray tracing is
	// unsupported or the build is degenerate; callers must treat that as "no
	// geometry" rather than an error.
	[[nodiscard]] RayTracingBlas CreateBottomLevel(const RayTracingBlasBuildInfo& info);

	// Records the device-side build for a BLAS created by CreateBottomLevel on
	// the given command buffer. Separate from creation so several builds can be
	// recorded before a single submit.
	void RecordBottomLevelBuild(CommandBuffer& command, RayTracingBlas& blas,
	                            const RayTracingBlasBuildInfo& info);

	// Creates and records a TLAS build referencing the given BLAS device
	// addresses. The caller is responsible for submitting the command buffer
	// before tracing against the returned structure.
	[[nodiscard]] RayTracingTlas BuildTopLevel(CommandBuffer&                 command,
	                                           const RayTracingTlasBuildInfo& info);

private:
	[[nodiscard]] vk::AccelerationStructureBuildSizesInfoKHR
	QueryBlasBuildSizes(const RayTracingBlasBuildInfo& info) const;
	[[nodiscard]] vk::AccelerationStructureBuildSizesInfoKHR
	QueryTlasBuildSizes(uint32_t instance_count) const;

	GraphicContext& m_graphics;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RAYTRACING_H_
