using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Aether.Mcp.Ipc;
using Aether.Mcp.Tools;

namespace Aether.Mcp.Server;

/// <summary>
/// A Model Context Protocol server over stdio (JSON-RPC 2.0, one message per line). It only translates: tools/call
/// becomes a request to the Aether runtime in the game over the local pipe. stdout carries protocol messages only;
/// diagnostics go to stderr.
/// </summary>
public sealed class McpServer(RuntimeClient client) {
	private const string ServerVersion = "0.1.0";
	private static readonly string[] KnownProtocols = ["2025-06-18", "2025-03-26", "2024-11-05"];
	private readonly Dictionary<string, Tool> tools_ = ToolCatalog.All.ToDictionary(t => t.Name);
	private static readonly JsonSerializerOptions Compact = new() { WriteIndented = false };

	public async Task Run(CancellationToken cancel) {
		using var input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
		await using var output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
		while (!cancel.IsCancellationRequested) {
			string? line = await input.ReadLineAsync(cancel);
			if (line == null) {
				return; // the client closed stdin
			}
			if (string.IsNullOrWhiteSpace(line)) {
				continue;
			}
			JsonNode? message;
			try {
				message = JsonNode.Parse(line);
			} catch (JsonException) {
				await output.WriteLineAsync(Error(null, -32700, "Parse error").ToJsonString(Compact));
				continue;
			}
			if (message is not JsonObject request || request["method"]?.GetValueKind() != JsonValueKind.String) {
				if (message is JsonObject o && (o.ContainsKey("result") || o.ContainsKey("error"))) {
					continue; // a response to something this server never asks
				}
				await output.WriteLineAsync(Error(message?["id"], -32600, "Invalid Request").ToJsonString(Compact));
				continue;
			}
			JsonNode? id = request["id"];
			bool notification = !request.ContainsKey("id");
			JsonObject? response;
			try {
				response = await Handle(request["method"]!.GetValue<string>(), request["params"] as JsonObject, id, cancel);
			} catch (OperationCanceledException) {
				return;
			} catch (Exception e) {
				Console.Error.WriteLine("[aether-mcp] internal error: " + e);
				response = Error(id, -32603, "Internal error: " + e.Message);
			}
			if (!notification && response != null) {
				await output.WriteLineAsync(response.ToJsonString(Compact));
			}
		}
	}

	private static JsonObject Result(JsonNode? id, JsonNode result) => new() { ["jsonrpc"] = "2.0", ["id"] = id?.DeepClone(), ["result"] = result };

	private static JsonObject Error(JsonNode? id, int code, string message) =>
		new() { ["jsonrpc"] = "2.0", ["id"] = id?.DeepClone(), ["error"] = new JsonObject { ["code"] = code, ["message"] = message } };

	private async Task<JsonObject?> Handle(string method, JsonObject? parameters, JsonNode? id, CancellationToken cancel) {
		switch (method) {
		case "initialize": {
			string requested = parameters?["protocolVersion"]?.GetValue<string>() ?? KnownProtocols[0];
			string version = KnownProtocols.Contains(requested) ? requested : KnownProtocols[0];
			return Result(id, new JsonObject {
				["protocolVersion"] = version,
				["capabilities"] = new JsonObject { ["tools"] = new JsonObject { ["listChanged"] = false } },
				["serverInfo"] = new JsonObject { ["name"] = "aether", ["title"] = "Aether renderer diagnostics", ["version"] = ServerVersion },
				["instructions"] = "Diagnostics for Aether, the DX12 renderer mod running inside CarX Drift Racing Online. Call get_status first. Tools marked [control] change the running game and need this server's --allow-control plus the game's permission (the player may have to approve in game: allow up to five minutes). When the game is not running every tool answers E_NOT_RUNNING. To find what uses VRAM, combine get_vram_usage, get_resource_list (includeUnityRenderTextures), get_render_graph and get_logs."
					+ (client.AllowControl ? "" : " This server runs read-only: control tools are refused."),
			});
		}
		case "notifications/initialized":
		case "notifications/cancelled":
			return null;
		case "ping":
			return Result(id, new JsonObject());
		case "tools/list": {
			var list = new JsonArray();
			foreach (Tool t in ToolCatalog.All) {
				list.Add(new JsonObject {
					["name"] = t.Name,
					["description"] = t.Description,
					["inputSchema"] = JsonNode.Parse(t.InputSchema),
					["outputSchema"] = JsonNode.Parse(t.OutputSchema),
					["annotations"] = new JsonObject { ["readOnlyHint"] = !t.Control, ["destructiveHint"] = false, ["openWorldHint"] = false },
				});
			}
			return Result(id, new JsonObject { ["tools"] = list });
		}
		case "tools/call":
			return Result(id, await CallTool(parameters, cancel));
		default:
			return id == null ? null : Error(id, -32601, "Method not found: " + method);
		}
	}

	private static JsonObject ToolError(string code, string message) => new() {
		["content"] = new JsonArray(new JsonObject { ["type"] = "text", ["text"] = code + ": " + message }),
		["isError"] = true,
	};

	private async Task<JsonObject> CallTool(JsonObject? parameters, CancellationToken cancel) {
		string? name = parameters?["name"]?.GetValue<string>();
		if (name == null || !tools_.TryGetValue(name, out Tool? tool)) {
			return ToolError("E_UNKNOWN_TOOL", "unknown tool: " + name);
		}
		JsonObject arguments = parameters?["arguments"] as JsonObject ?? new JsonObject();
		if (tool.Control && !client.AllowControl) {
			return ToolError("E_CONTROL_DISABLED", "this Aether MCP server is read-only. Restart it with --allow-control (or AETHER_MCP_ALLOW_CONTROL=1) to use " + name + ".");
		}
		try {
			JsonNode? result = tool.Handler != null
				? await tool.Handler(client, arguments, cancel)
				: await client.Call(tool.Name, arguments, tool.Control, TimeSpan.FromSeconds(tool.TimeoutSeconds), cancel);
			JsonObject structured = result as JsonObject ?? new JsonObject { ["value"] = result?.DeepClone() };
			var content = new JsonArray();
			if (structured["__imagePath"]?.GetValue<string>() is string imagePath) {
				structured.Remove("__imagePath");
				try {
					content.Add(new JsonObject { ["type"] = "image", ["data"] = Convert.ToBase64String(await File.ReadAllBytesAsync(imagePath, cancel)), ["mimeType"] = "image/png" });
				} catch (IOException e) {
					Console.Error.WriteLine("[aether-mcp] could not read the capture: " + e.Message);
				}
			}
			content.Insert(0, new JsonObject { ["type"] = "text", ["text"] = structured.ToJsonString(Compact) });
			return new JsonObject { ["content"] = content, ["structuredContent"] = structured.DeepClone(), ["isError"] = false };
		} catch (RuntimeException e) {
			return ToolError(e.Code, e.Message);
		} catch (Exception e) when (e is InvalidOperationException or FormatException or JsonException) {
			return ToolError("E_INVALID_PARAMS", "the arguments do not match the tool's input schema: " + e.Message);
		}
	}
}
