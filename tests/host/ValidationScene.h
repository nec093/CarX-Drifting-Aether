// The validation scene of the test host: a handful of boxes described through the scene data contract exactly the
// way the managed side describes Unity's objects (Unity's conventions: left-handed world, +Y up, clockwise front
// faces, column-major matrices, right-handed view space, reversed-Z projection). A capture of it is checked pixel by
// pixel (scripts/test-host.ps1): where things are on screen, what is in front, which side of a face is drawn, whether
// a mirrored instance still faces outwards, shadows, blending, the diagnostic material.
#pragma once
#include "Core/Common.h"
#include "Core/Math.h"
#include "include/SceneContract.h"

namespace aether::host {

struct Probe {
	const char* name;
	Vec3 world;
};

class ValidationScene {
public:
	bool Create(ID3D12Device* device, ID3D12CommandQueue* queue);
	void Release();
	// Sends the whole scene (meshes, textures, materials, instances) through the contract.
	void Register();
	// Sends this frame: camera, transforms, lights.
	void SubmitFrame(uint32_t width, uint32_t height, uint32_t frameIndex, float time);
	// Destroys one instance and one mesh, then registers them again a few frames later (handle lifetime test).
	void Churn(uint32_t frameIndex);
	// Where the probe points land on a width x height back buffer.
	void PrintProbes(uint32_t width, uint32_t height) const;

private:
	struct Instance { AeHandle handle; AeHandle mesh; AeHandle material; Mat4 world; uint32_t flags; };
	Mat4 View() const;
	Mat4 Proj(uint32_t width, uint32_t height) const;

	ComPtr<ID3D12Resource> boxVb_, boxIb_, checker_;
	std::vector<Instance> instances_;
	std::vector<AeFrameInstance> frameInstances_;
	bool removed_ = false;
};

} // namespace aether::host
