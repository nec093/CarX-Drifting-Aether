using System;
using System.Runtime.InteropServices;

namespace Aether {
	// The managed mirror of src/Aether.Native/include/SceneContract.h (contract version 1).
	// Layouts must match byte for byte: SceneContract.Verify compares the sizes with the native side before any use.

	[StructLayout(LayoutKind.Sequential)]
	internal struct AeVertexAttribute {
		public byte semantic;
		public byte stream;
		public byte offset;
		public byte dxgiFormat;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct AeSubMesh {
		public uint indexStart;
		public uint indexCount;
		public uint baseVertex;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct AeMeshDesc {
		public uint structSize;
		public uint mesh;
		public uint vertexCount;
		public uint indexCount;
		public uint indexStride;
		public uint streamCount;
		public fixed uint streamStride[4];
		public IntPtr vertexBuffer0, vertexBuffer1, vertexBuffer2, vertexBuffer3;
		public IntPtr indexBuffer;
		public uint attributeCount;
		public AeVertexAttribute attribute0, attribute1, attribute2, attribute3, attribute4, attribute5;
		public fixed float boundsCenter[3];
		public fixed float boundsExtents[3];
		public uint subMeshCount;
		public uint reserved;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct AeTextureDesc {
		public uint structSize;
		public uint texture;
		public IntPtr resource;
		public uint width;
		public uint height;
		public uint mipCount;
		public uint dimension;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct AeMaterialDesc {
		public uint structSize;
		public uint material;
		public uint materialClass;
		public uint flags;
		public uint maskLayout;
		public uint renderQueue;
		public uint shaderId;
		public uint unsupportedProperties;
		public fixed float baseColor[4];
		public fixed float secondaryColor[4];
		public fixed float emissive[3];
		public float metallic;
		public float roughness;
		public float normalScale;
		public float alphaCutoff;
		public float occlusion;
		public float clearCoat;
		public float clearCoatRoughness;
		public fixed float uvScale[2];
		public fixed float uvOffset[2];
		public fixed uint textures[5];
		public fixed uint reserved[3];
	}

	[StructLayout(LayoutKind.Sequential)]
	internal struct AeInstanceDesc {
		public uint structSize;
		public uint instance;
		public uint mesh;
		public uint flags;
		public uint layer;
		public uint materialCount;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct AeFrameInstance {
		public uint instance;
		public uint flags;
		public fixed float localToWorld[16];
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct AeLight {
		public uint type;
		public uint flags;
		public fixed float position[3];
		public float range;
		public fixed float direction[3];
		public float spotOuterCos;
		public fixed float color[3];
		public float spotInnerCos;
	}

	[StructLayout(LayoutKind.Sequential)]
	internal unsafe struct AeFrame {
		public uint structSize;
		public uint contractVersion;
		public uint flags;
		public uint sceneId;
		public uint cameraId;
		public uint width;
		public uint height;
		public uint frameIndex;
		public fixed float view[16];
		public fixed float proj[16];
		public fixed float cameraPosition[3];
		public float nearPlane;
		public fixed float cameraForward[3];
		public float farPlane;
		public float deltaTime;
		public float time;
		public float exposure;
		public float ambientIntensity;
		public fixed float ambientSH[28];
		public fixed float fogColor[3];
		public float fogDensity;
		public uint skyTexture;
		public uint exposureTexture;
		public fixed uint reserved[2];
	}

	internal static class AeConst {
		public const int ContractVersion = 1;

		public const byte SemanticPosition = 0, SemanticNormal = 1, SemanticTangent = 2, SemanticColor = 3, SemanticTexCoord0 = 4, SemanticTexCoord1 = 5;
		public const uint Texture2D = 0, TextureCube = 1, TextureCubeArray = 2, Texture2DArray = 3, Texture3D = 4;

		public const uint ClassUnsupported = 0, ClassStandard = 1, ClassVehiclePaint = 2, ClassVehicleGeneric = 3, ClassVehicleGlass = 4, ClassVehicleRim = 5,
			ClassVehicleStitches = 6, ClassColorMask = 7, ClassVegetation = 8, ClassTransparent = 9, ClassUnlit = 10, ClassVehicleLights = 11, ClassVehicleTire = 12, ClassSkip = 13;

		public const uint MaterialAlphaTest = 1, MaterialBlend = 2, MaterialDoubleSided = 4, MaterialAdditive = 8, MaterialClearCoat = 16, MaterialNoShadow = 32;
		public const uint MaskNone = 0, MaskHdrp = 1, MaskRmo = 2;
		public const int SlotBase = 0, SlotNormal = 1, SlotMask = 2, SlotEmissive = 3, SlotExtra = 4;

		public const uint InstanceCastShadows = 1, InstanceReceiveShadows = 2, InstanceStatic = 4;
		public const uint FrameInstanceVisible = 1;
		public const uint LightDirectional = 0, LightPoint = 1, LightSpot = 2;
		public const uint LightCastsShadows = 1;
		public const uint FrameCameraValid = 1;

		public const int KindMesh = 0, KindTexture = 1, KindMaterial = 2, KindInstance = 3;
	}

	internal static class SceneContract {
		/// <summary>Null when both sides agree on the contract; else what differs (nothing must be sent then).</summary>
		public static string Verify() {
			if (Native.Aether_SceneContractVersion() != AeConst.ContractVersion) {
				return "scene contract version " + Native.Aether_SceneContractVersion() + " (native) != " + AeConst.ContractVersion + " (managed)";
			}
			var sizes = new[] {
				Marshal.SizeOf<AeMeshDesc>(), Marshal.SizeOf<AeTextureDesc>(), Marshal.SizeOf<AeMaterialDesc>(), Marshal.SizeOf<AeInstanceDesc>(),
				Marshal.SizeOf<AeFrameInstance>(), Marshal.SizeOf<AeLight>(), Marshal.SizeOf<AeFrame>(), Marshal.SizeOf<AeSubMesh>(),
			};
			string[] names = { "AeMeshDesc", "AeTextureDesc", "AeMaterialDesc", "AeInstanceDesc", "AeFrameInstance", "AeLight", "AeFrame", "AeSubMesh" };
			for (int i = 0; i < sizes.Length; ++i) {
				int native = Native.Aether_SceneStructSize(i);
				if (native != sizes[i]) {
					return names[i] + " is " + sizes[i] + " bytes here and " + native + " bytes in the native runtime";
				}
			}
			return null;
		}
	}
}
