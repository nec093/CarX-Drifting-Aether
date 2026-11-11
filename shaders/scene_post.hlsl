// Aether full screen passes: sky background and the final composite (exposure, tone mapping, output encoding).
#include "aether_scene.hlsli"

struct FullscreenOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

// One triangle covering the screen. z = 0 is the far plane with reversed Z: the sky is drawn where nothing else is.
FullscreenOutput VSFullscreen(uint id : SV_VertexID) {
	FullscreenOutput o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 PSSky(FullscreenOutput input) : SV_Target {
	// the world position of this pixel on a plane in front of the camera gives the view direction
	float2 ndc = input.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
	float4 world = mul(g_InvViewProj, float4(ndc, 0.5, 1.0));
	float3 dir = normalize(world.xyz / world.w - g_CameraPos.xyz);
	float3 color = SkyRadiance(dir, 0.0);
	if ((int)g_Misc.x != VIEW_NONE) {
		color = float3(0.02, 0.02, 0.03);
	}
	return float4(color, 1.0);
}

float3 TonemapACES(float3 x) {
	// Narkowicz's fit of the ACES filmic curve
	return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 TonemapNeutral(float3 x) {
	// Reinhard on luminance, keeps the hue
	float l = dot(x, float3(0.2126, 0.7152, 0.0722));
	return x / (1.0 + l);
}

float3 LinearToSRGB(float3 c) {
	c = saturate(c);
	return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// The final composite into the host's back buffer (a UNORM target: the encoding is done here).
float4 PSComposite(FullscreenOutput input) : SV_Target {
	// the scene target has the size of the back buffer: pixel for pixel
	float3 color = g_SceneColor.Load(int3(input.position.xy, 0)).rgb;
	if ((int)g_Misc.x == VIEW_NONE) {
		float exposure = g_Exposure.x;
		if (g_Exposure.y > 0.5) {
			exposure = g_ExposureTex.Load(int3(0, 0, 0)).r;
		}
		color *= exposure * g_Exposure.z;
		int mode = (int)g_Exposure.w;
		if (mode == 1) color = TonemapACES(color);
		else if (mode == 2) color = TonemapNeutral(color);
	}
	return float4(LinearToSRGB(color), 1.0);
}
