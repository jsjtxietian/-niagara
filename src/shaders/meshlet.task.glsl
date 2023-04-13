#version 450

#extension GL_EXT_shader_16bit_storage: require
#extension GL_EXT_shader_8bit_storage: require
#extension GL_EXT_mesh_shader: require

#extension GL_GOOGLE_include_directive: require

#include "mesh.h"
#include "math.h"

layout (constant_id = 0) const bool LATE = false;

#define CULL 1

layout(local_size_x = TASK_WGSIZE, local_size_y = 1, local_size_z = 1) in;

layout(push_constant) uniform block
{
	Globals globals;
};

layout(binding = 0) readonly buffer TaskCommands
{
	MeshTaskCommand taskCommands[];
};

layout(binding = 1) readonly buffer Draws
{
	MeshDraw draws[];
};

layout(binding = 2) readonly buffer Meshlets
{
	Meshlet meshlets[];
};

layout(binding = 5) buffer MeshletVisibility
{
	uint meshletVisibility[];
};

layout(binding = 6) uniform sampler2D depthPyramid;

taskPayloadSharedEXT MeshTaskPayload payload;

#if CULL
shared int sharedCount;
#endif

void main()
{
	// we convert 2D index to 1D index using a fixed *64 factor, see tasksubmit.comp.glsl
	MeshTaskCommand command = taskCommands[gl_WorkGroupID.x * 64 + gl_WorkGroupID.y];
	uint drawId = command.drawId;
	MeshDraw meshDraw = draws[drawId];

	uint lateDrawVisibility = command.lateDrawVisibility;
	uint taskCount = command.taskCount;

	uint mgi = gl_LocalInvocationID.x;
	uint mi = mgi + command.taskOffset;
	uint mvi = mgi + command.meshletVisibilityOffset;

#if CULL
	sharedCount = 0;
	barrier(); // for sharedCount

	vec3 center = rotateQuat(meshlets[mi].center, meshDraw.orientation) * meshDraw.scale + meshDraw.position;
	float radius = meshlets[mi].radius * meshDraw.scale;
	vec3 cone_axis = rotateQuat(vec3(int(meshlets[mi].cone_axis[0]) / 127.0, int(meshlets[mi].cone_axis[1]) / 127.0, int(meshlets[mi].cone_axis[2]) / 127.0), meshDraw.orientation);
	float cone_cutoff = int(meshlets[mi].cone_cutoff) / 127.0;

	bool valid = mgi < taskCount;
	bool visible = valid;
	bool skip = false;

	if (globals.clusterOcclusionEnabled == 1)
	{
		uint meshletVisibilityBit = meshletVisibility[mvi >> 5] & (1u << (mvi & 31));

		// in early pass, we have to *only* render clusters that were visible last frame, to build a reasonable depth pyramid out of visible triangles
		if (!LATE && meshletVisibilityBit == 0)
			visible = false;

		// in late pass, we have to process objects visible last frame again (after rendering them in early pass)
		// in early pass, per above test, we render previously visible clusters
		// in late pass, we must invert the above test to *not* render previously visible clusters of previously visible objects because they were rendered in early pass.
		if (LATE && lateDrawVisibility == 1 && meshletVisibilityBit != 0)
			skip = true;
	}

	// backface cone culling
	visible = visible && !coneCull(center, radius, cone_axis, cone_cutoff, vec3(0, 0, 0));
	// the left/top/right/bottom plane culling utilizes frustum symmetry to cull against two planes at the same time
	visible = visible && center.z * globals.frustum[1] - abs(center.x) * globals.frustum[0] > -radius;
	visible = visible && center.z * globals.frustum[3] - abs(center.y) * globals.frustum[2] > -radius;
	// the near/far plane culling uses camera space Z directly
	// note: because we use an infinite projection matrix, this may cull meshlets that belong to a mesh that straddles the "far" plane; we could optionally remove the far check to be conservative
	visible = visible && center.z + radius > globals.znear && center.z - radius < globals.zfar;

	if (LATE && globals.clusterOcclusionEnabled == 1 && visible)
	{
		float P00 = globals.projection[0][0], P11 = globals.projection[1][1];

		vec4 aabb;
		if (projectSphere(center, radius, globals.znear, P00, P11, aabb))
		{
			float width = (aabb.z - aabb.x) * globals.pyramidWidth;
			float height = (aabb.w - aabb.y) * globals.pyramidHeight;

			float level = floor(log2(max(width, height)));

			// Sampler is set up to do min reduction, so this computes the minimum depth of a 2x2 texel quad
			float depth = textureLod(depthPyramid, (aabb.xy + aabb.zw) * 0.5, level).x;
			float depthSphere = globals.znear / (center.z - radius);

			visible = visible && depthSphere > depth;
		}
	}

	if (LATE && globals.clusterOcclusionEnabled == 1 && valid)
	{
		if (visible)
			atomicOr(meshletVisibility[mvi >> 5], 1u << (mvi & 31));
		else
			atomicAnd(meshletVisibility[mvi >> 5], ~(1u << (mvi & 31)));
	}

	if (visible && !skip)
	{
		uint index = atomicAdd(sharedCount, 1);

		payload.meshletIndices[index] = mi;
	}

	payload.drawId = drawId;

	barrier(); // for sharedCount
	EmitMeshTasksEXT(sharedCount, 1, 1);
#else
	payload.drawId = drawId;
	payload.meshletIndices[gl_LocalInvocationIndex] = mi;

	uint count = min(TASK_WGSIZE, taskCount - gl_WorkGroupID.x * TASK_WGSIZE);

	EmitMeshTasksEXT(count, 1, 1);
#endif
}
