using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Aether.Mcp.Ipc;

/// <summary>An error the runtime (or the connection to it) answered with.</summary>
public sealed class RuntimeException(string code, string message) : Exception(message) {
	public string Code { get; } = code;
}

/// <summary>
/// The connection to the Aether runtime inside the game: a local named pipe, one JSON request per line, one JSON
/// response per line. The MCP server is a separate process: when the game is not running (or goes away mid-call) a call
/// fails with a clear error and the next call connects again.
/// </summary>
public sealed class RuntimeClient(string pipeName, bool allowControl) : IDisposable {
	private NamedPipeClientStream? pipe_;
	private StreamReader? reader_;
	private long nextId_ = 1;
	private readonly SemaphoreSlim gate_ = new(1, 1);

	public bool AllowControl { get; } = allowControl;
	public string PipeName { get; } = pipeName;

	private void Drop() {
		try {
			pipe_?.Dispose();
		} catch (IOException) {
		}
		pipe_ = null;
		reader_ = null;
	}

	private async Task Connect(CancellationToken cancel) {
		if (pipe_ is { IsConnected: true }) {
			return;
		}
		Drop();
		var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
		try {
			await pipe.ConnectAsync(1500, cancel);
		} catch (Exception e) when (e is TimeoutException or IOException or UnauthorizedAccessException) {
			pipe.Dispose();
			throw new RuntimeException("E_NOT_RUNNING",
				$"Aether is not reachable on \\\\.\\pipe\\{PipeName}: the game is not running, Aether is not installed / failed to start (see kino/mods/Aether/logs/aether.jsonl and kino/output.log), or the game runs as another user. ({e.GetType().Name})");
		}
		pipe_ = pipe;
		reader_ = new StreamReader(pipe, new UTF8Encoding(false), false, 1 << 16, leaveOpen: true);
	}

	/// <summary>Sends one request and returns its result. Throws RuntimeException with the runtime's error code.</summary>
	public async Task<JsonNode?> Call(string method, JsonObject? parameters, bool control, TimeSpan timeout, CancellationToken cancel) {
		await gate_.WaitAsync(cancel);
		try {
			await Connect(cancel);
			long id = nextId_++;
			var request = new JsonObject {
				["id"] = id,
				["method"] = method,
				["params"] = parameters?.DeepClone() ?? new JsonObject(),
				["control"] = control && AllowControl,
			};
			byte[] bytes = Encoding.UTF8.GetBytes(request.ToJsonString() + "\n");
			using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancel);
			timeoutSource.CancelAfter(timeout);
			try {
				await pipe_!.WriteAsync(bytes, timeoutSource.Token);
				await pipe_.FlushAsync(timeoutSource.Token);
				while (true) {
					string? line = await reader_!.ReadLineAsync(timeoutSource.Token);
					if (line == null) {
						Drop();
						throw new RuntimeException("E_DISCONNECTED", "The game closed the connection (it quit, crashed, or Aether shut down) before answering.");
					}
					JsonNode? response;
					try {
						response = JsonNode.Parse(line);
					} catch (JsonException) {
						Drop();
						throw new RuntimeException("E_PROTOCOL", "The runtime sent a line that is not JSON.");
					}
					if (response?["id"]?.GetValueKind() == JsonValueKind.Number && response["id"]!.GetValue<long>() != id) {
						continue; // an answer to an earlier call that timed out
					}
					if (response?["ok"]?.GetValue<bool>() == true) {
						return response["result"];
					}
					JsonNode? error = response?["error"];
					throw new RuntimeException(error?["code"]?.GetValue<string>() ?? "E_UNKNOWN", error?["message"]?.GetValue<string>() ?? "unknown error");
				}
			} catch (OperationCanceledException) when (!cancel.IsCancellationRequested) {
				Drop(); // the stream may hold half a response
				throw new RuntimeException("E_TIMEOUT", $"The runtime did not answer '{method}' within {timeout.TotalSeconds:0} s (is the game frozen or paused in the background?).");
			} catch (IOException e) {
				Drop();
				throw new RuntimeException("E_DISCONNECTED", "The connection to the game broke: " + e.Message);
			}
		} finally {
			gate_.Release();
		}
	}

	public void Dispose() {
		Drop();
		gate_.Dispose();
	}
}
