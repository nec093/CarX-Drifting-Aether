// Aether Phase 1: the DX12 integration test triangle (drawn straight onto the back buffer, top right corner).
// Compiled at run time (vs_5_1 / ps_5_1); edit and use "Reload shaders" in the overlay or the reload_shader MCP tool.

struct VSInput {
	float2 position : POSITION;
	float3 color : COLOR;
};

struct VSOutput {
	float4 position : SV_Position;
	float3 color : COLOR;
};

VSOutput VSMain(VSInput input) {
	VSOutput o;
	o.position = float4(input.position, 0.0, 1.0);
	o.color = input.color;
	return o;
}

float4 PSMain(VSOutput input) : SV_Target {
	return float4(input.color, 1.0);
}
