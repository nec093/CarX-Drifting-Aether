using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace Aether {
	/// <summary>
	/// Turns a Unity material into Aether's material description (AeMaterialDesc). Independent of HDRP: nothing of
	/// HDRP's material evaluation is called; only the material's own properties are read.
	///
	/// The class of a material is decided by the properties its shader really has (the signatures below come from the
	/// survey of the game's shaders, docs/architecture/material-translation.md), not by the shader's name alone.
	/// A shader none of the signatures fits becomes "unsupported": it is drawn with the diagnostic material and counted,
	/// never dropped. Where the meaning of a property is an assumption (no shader source), the document says so.
	/// </summary>
	internal static class MaterialTranslator {
		private static readonly int BaseColor = Shader.PropertyToID("_BaseColor");
		private static readonly int BaseColorMap = Shader.PropertyToID("_BaseColorMap");
		private static readonly int BaseMap = Shader.PropertyToID("_BaseMap");
		private static readonly int MainTex = Shader.PropertyToID("_MainTex");
		private static readonly int Metallic = Shader.PropertyToID("_Metallic");
		private static readonly int Smoothness = Shader.PropertyToID("_Smoothness");
		private static readonly int MaskMap = Shader.PropertyToID("_MaskMap");
		private static readonly int MetallicRemapMax = Shader.PropertyToID("_MetallicRemapMax");
		private static readonly int SmoothnessRemapMax = Shader.PropertyToID("_SmoothnessRemapMax");
		private static readonly int NormalMap = Shader.PropertyToID("_NormalMap");
		private static readonly int NormalScale = Shader.PropertyToID("_NormalScale");
		private static readonly int EmissiveColor = Shader.PropertyToID("_EmissiveColor");
		private static readonly int EmissiveColorMap = Shader.PropertyToID("_EmissiveColorMap");
		private static readonly int AlphaCutoffEnable = Shader.PropertyToID("_AlphaCutoffEnable");
		private static readonly int AlphaCutoff = Shader.PropertyToID("_AlphaCutoff");
		private static readonly int AlphaClippingThreshold = Shader.PropertyToID("_AlphaClippingThreshold");
		private static readonly int SurfaceType = Shader.PropertyToID("_SurfaceType");
		private static readonly int DoubleSidedEnable = Shader.PropertyToID("_DoubleSidedEnable");
		private static readonly int Roughness = Shader.PropertyToID("_Roughness");
		private static readonly int Metalness = Shader.PropertyToID("_Metalness");
		private static readonly int Clearcoat = Shader.PropertyToID("_Clearcoat");
		private static readonly int ClearCoatEnabled = Shader.PropertyToID("_ClearCoatEnabled");
		private static readonly int SecondaryColor = Shader.PropertyToID("_SecondaryColor");
		private static readonly int Spectrum = Shader.PropertyToID("_Spectrum");
		private static readonly int PackMap = Shader.PropertyToID("_PackMap");
		private static readonly int ColormaskMap = Shader.PropertyToID("_ColormaskMap");
		private static readonly int EmissionMap = Shader.PropertyToID("_EmissionMap");
		private static readonly int EmissionIntensity = Shader.PropertyToID("_EmissionIntensity");
		private static readonly int EmissionMasks = Shader.PropertyToID("_EmissionMasks");
		private static readonly int TireBaseMap = Shader.PropertyToID("_TireBaseMap");
		private static readonly int TirePackMap = Shader.PropertyToID("_TirePackMap");
		private static readonly int TireNormalMap = Shader.PropertyToID("_TireNormalMap");
		private static readonly int StitchesPack = Shader.PropertyToID("Texture2D_67B10B5E");
		private static readonly int DiffusionProfileHash = Shader.PropertyToID("_DiffusionProfileHash");
		private static readonly int MotionVectorAngular = Shader.PropertyToID("_MotionVectorAngular");
		private static readonly int MaxBaseColor = Shader.PropertyToID("_BASE_COLOR");
		private static readonly int MaxBaseColorMap = Shader.PropertyToID("_BASE_COLOR_MAP");
		private static readonly int MaxRoughness = Shader.PropertyToID("_REFLECTIONS_ROUGHNESS");
		private static readonly int MaxMetalness = Shader.PropertyToID("_METALNESS");
		private static readonly int ShouldFlipX = Shader.PropertyToID("_ShouldFlipX");

		/// <summary>What a translation produced: the description and the textures it names (slot order).</summary>
		internal struct Result {
			public AeMaterialDesc desc;
			public Texture[] textures; // AeConst.Slot* order, null entries = none
			public string className;
		}

		private static readonly Texture[] scratch_ = new Texture[5];

		private static Texture Tex(Material m, int id) {
			return m.HasProperty(id) ? m.GetTexture(id) : null;
		}

		private static float Float(Material m, int id, float fallback) {
			return m.HasProperty(id) ? m.GetFloat(id) : fallback;
		}

		private static unsafe void SetColor(float* destination, Color c) {
			destination[0] = c.r;
			destination[1] = c.g;
			destination[2] = c.b;
			destination[3] = c.a;
		}

		/// <summary>Translates a material. `colorOverride`: a per-renderer base colour (MaterialPropertyBlock), if any.</summary>
		public static unsafe Result Translate(Material m, uint handle, Color? colorOverride) {
			var r = new Result { textures = new Texture[5] };
			AeMaterialDesc d = default;
			d.structSize = (uint)sizeof(AeMaterialDesc);
			d.material = handle;
			d.renderQueue = (uint)m.renderQueue;
			d.shaderId = (uint)m.shader.GetInstanceID();
			d.metallic = 0;
			d.roughness = 0.6f;
			d.normalScale = 1;
			d.alphaCutoff = 0.5f;
			d.occlusion = 1;
			d.uvScale[0] = d.uvScale[1] = 1;
			SetColor(d.baseColor, Color.white);
			SetColor(d.secondaryColor, new Color(0, 0, 0, 0));
			for (int i = 0; i < 5; ++i) {
				scratch_[i] = null;
			}
			uint cls = AeConst.ClassUnsupported;
			uint flags = 0;
			int uvProperty = 0;

			// ---- flags HDRP style materials (Lit and the Shader Graphs alike) all carry
			bool transparent = Float(m, SurfaceType, 0) > 0.5f || m.renderQueue >= (int)RenderQueue.Transparent;
			if (Float(m, AlphaCutoffEnable, 0) > 0.5f) {
				flags |= AeConst.MaterialAlphaTest;
			}
			if (Float(m, DoubleSidedEnable, 0) > 0.5f) {
				flags |= AeConst.MaterialDoubleSided;
			}

			bool paintFamily = m.HasProperty(Roughness) && m.HasProperty(Metalness) && m.HasProperty(Clearcoat);
			if (m.HasProperty(MotionVectorAngular)) {
				// the wheels' motion vector proxy: geometry that only exists to write motion vectors
				cls = AeConst.ClassSkip;
			} else if (paintFamily) {
				// the game's paint graphs: _Roughness, _Metalness, _Clearcoat, _BaseColor, _SecondaryColor
				cls = m.HasProperty(Spectrum) ? AeConst.ClassVehiclePaint : m.HasProperty(StitchesPack) ? AeConst.ClassVehicleStitches : AeConst.ClassVehicleRim;
				SetColor(d.baseColor, m.GetColor(BaseColor).linear);
				SetColor(d.secondaryColor, m.GetColor(SecondaryColor).linear);
				d.metallic = m.GetFloat(Metalness);
				d.roughness = m.GetFloat(Roughness);
				if (Float(m, ClearCoatEnabled, 0) > 0.5f) {
					// The graph has HDRP's clear coat feature on. What _Clearcoat scales is not known without the graph:
					// a coat is assumed present, at least as strong as _Clearcoat says.
					d.clearCoat = Mathf.Max(m.GetFloat(Clearcoat), cls == AeConst.ClassVehicleStitches ? 0.0f : 1.0f);
					d.clearCoatRoughness = 0.04f;
					flags |= AeConst.MaterialClearCoat;
				}
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				uvProperty = BaseMap;
			} else if (m.HasProperty(AlphaClippingThreshold) && m.HasProperty(Spectrum)) {
				cls = AeConst.ClassVehicleGlass;
				SetColor(d.baseColor, m.GetColor(BaseColor).linear);
				d.roughness = 0.03f;
				d.alphaCutoff = m.GetFloat(AlphaClippingThreshold);
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				transparent = true;
				flags |= AeConst.MaterialDoubleSided;
			} else if (m.HasProperty(TireBaseMap)) {
				cls = AeConst.ClassVehicleTire;
				SetColor(d.baseColor, Color.white);
				scratch_[AeConst.SlotBase] = Tex(m, TireBaseMap);
				scratch_[AeConst.SlotMask] = Tex(m, TirePackMap);
				scratch_[AeConst.SlotNormal] = Tex(m, TireNormalMap);
				d.maskLayout = AeConst.MaskRmo;
				d.roughness = 1;
				d.metallic = 1;
				uvProperty = TireBaseMap;
			} else if (m.HasProperty(BaseMap) && m.HasProperty(PackMap)) {
				// the game's generic car graphs (CarGeneric, CarGenericAlpha, CarLights): base, pack (roughness / metallic /
				// occlusion), normal, and either a colour mask or emission masks
				cls = m.HasProperty(EmissionMasks) ? AeConst.ClassVehicleLights : AeConst.ClassVehicleGeneric;
				scratch_[AeConst.SlotBase] = Tex(m, BaseMap);
				scratch_[AeConst.SlotMask] = Tex(m, PackMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				d.maskLayout = AeConst.MaskRmo;
				d.roughness = 1;
				d.metallic = 1;
				if (cls == AeConst.ClassVehicleGeneric) {
					scratch_[AeConst.SlotExtra] = Tex(m, ColormaskMap);
					// the colours tint through the mask; their alpha is the weight (0 in the materials seen = untinted)
					if (m.HasProperty(SecondaryColor)) {
						SetColor(d.secondaryColor, m.GetColor(SecondaryColor).linear);
					}
					scratch_[AeConst.SlotEmissive] = Tex(m, EmissionMap);
					float intensity = Float(m, EmissionIntensity, 0);
					if (scratch_[AeConst.SlotEmissive] != null && intensity > 0) {
						d.emissive[0] = d.emissive[1] = d.emissive[2] = intensity;
					}
				}
				uvProperty = BaseMap;
			} else if (m.HasProperty(MaxBaseColorMap) && m.HasProperty(MaxMetalness)) {
				// HDRP's 3ds Max physical material
				cls = AeConst.ClassStandard;
				SetColor(d.baseColor, m.GetColor(MaxBaseColor).linear);
				scratch_[AeConst.SlotBase] = Tex(m, MaxBaseColorMap);
				d.roughness = Mathf.Max(m.GetFloat(MaxRoughness), 0.05f);
				d.metallic = m.GetFloat(MaxMetalness);
				uvProperty = MaxBaseColorMap;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(Metallic) && m.HasProperty(Smoothness)) {
				// HDRP/Lit
				cls = AeConst.ClassStandard;
				SetColor(d.baseColor, m.GetColor(BaseColor).linear);
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				scratch_[AeConst.SlotMask] = Tex(m, MaskMap);
				d.normalScale = Float(m, NormalScale, 1);
				if (scratch_[AeConst.SlotMask] != null) {
					d.maskLayout = AeConst.MaskHdrp;
					d.metallic = Float(m, MetallicRemapMax, 1);
					d.roughness = 1.0f - Float(m, SmoothnessRemapMax, 1);
				} else {
					d.metallic = m.GetFloat(Metallic);
					d.roughness = 1.0f - m.GetFloat(Smoothness);
				}
				if (m.HasProperty(EmissiveColor)) {
					Color e = m.GetColor(EmissiveColor); // an HDR colour: already linear, in nits
					d.emissive[0] = e.r;
					d.emissive[1] = e.g;
					d.emissive[2] = e.b;
					scratch_[AeConst.SlotEmissive] = Tex(m, EmissiveColorMap);
				}
				d.alphaCutoff = Float(m, AlphaCutoff, 0.5f);
				uvProperty = BaseColorMap;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(MaskMap) && m.HasProperty(BaseColor)) {
				// the game's Colormask graph: base map tinted by _BaseColor, normal, mask
				cls = AeConst.ClassColorMask;
				SetColor(d.baseColor, m.GetColor(BaseColor).linear);
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				scratch_[AeConst.SlotNormal] = Tex(m, NormalMap);
				scratch_[AeConst.SlotMask] = Tex(m, MaskMap);
				if (scratch_[AeConst.SlotMask] != null) {
					d.maskLayout = AeConst.MaskHdrp;
					d.metallic = 1;
					d.roughness = 0;
				}
				uvProperty = BaseColorMap;
			} else if (m.HasProperty(BaseColorMap) && m.HasProperty(DiffusionProfileHash)) {
				// the game's foliage graph: an alpha tested, double sided leaf card
				cls = AeConst.ClassVegetation;
				scratch_[AeConst.SlotBase] = Tex(m, BaseColorMap);
				d.roughness = 0.8f;
				uvProperty = BaseColorMap;
			} else if (m.shader.name.IndexOf("Blobshadow", StringComparison.OrdinalIgnoreCase) >= 0 || (m.HasProperty(BaseColorMap) && transparent && !m.HasProperty(MainTex))) {
				// the car's blob shadow quad (CarBlobshadow, an unlit transparent graph with only _BaseColorMap): Aether
				// casts real shadows, the quad is not drawn
				cls = AeConst.ClassSkip;
			} else if (m.HasProperty(MainTex)) {
				// particles, video panels: unlit, their texture as it is
				cls = AeConst.ClassUnlit;
				scratch_[AeConst.SlotBase] = Tex(m, MainTex);
				if (!m.HasProperty(ShouldFlipX) && transparent) {
					flags |= AeConst.MaterialAdditive; // Mobile/Particles/Additive and the like
				}
				uvProperty = MainTex;
			}

			if (cls != AeConst.ClassUnsupported && cls != AeConst.ClassSkip) {
				if (transparent) {
					flags |= AeConst.MaterialBlend;
					if (cls == AeConst.ClassStandard) {
						cls = AeConst.ClassTransparent;
					}
				}
				if (uvProperty != 0 && m.HasProperty(uvProperty)) {
					Vector2 scale = m.GetTextureScale(uvProperty), offset = m.GetTextureOffset(uvProperty);
					d.uvScale[0] = scale.x;
					d.uvScale[1] = scale.y;
					d.uvOffset[0] = offset.x;
					d.uvOffset[1] = offset.y;
				}
				if (colorOverride.HasValue) {
					SetColor(d.baseColor, colorOverride.Value.linear);
				}
			}
			// textures the material has that nothing above mapped: reported, so a gap in the translation is visible
			uint unmapped = 0;
			if (cls != AeConst.ClassSkip) {
				ids_.Clear();
				m.GetTexturePropertyNameIDs(ids_);
				foreach (int id in ids_) {
					Texture t = m.GetTexture(id);
					if (t == null) {
						continue;
					}
					bool used = false;
					for (int i = 0; i < 5; ++i) {
						used |= scratch_[i] == t;
					}
					if (!used) {
						++unmapped;
					}
				}
			}
			d.unsupportedProperties = unmapped;
			d.materialClass = cls;
			d.flags = flags;
			for (int i = 0; i < 5; ++i) {
				r.textures[i] = scratch_[i];
			}
			r.desc = d;
			return r;
		}

		private static readonly List<int> ids_ = new List<int>();
	}
}
