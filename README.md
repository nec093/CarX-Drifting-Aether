# Aether

CarX Drift Racing Online（moddable ブランチ、Unity 2023.2.22f1 / HDRP 16）向けのグラフィックオーバーホール Mod です。
最終的には HDRP に頼らない独自の DirectX 12 レンダラーでゲームを描画し、VRAM 使用量・描画性能・画質を改善することを目指します。

- 描画 API は **DirectX 12 のみ**（DX11 / Vulkan のバックエンドもフォールバックもありません）。
- NVIDIA / AMD / Intel 共通の経路を基本とし、ベンダー固有機能は実行時判定のオプションにします。
- Mod ローダーは KSL Loader。ゲーム内 GUI は Dear ImGui。AI エージェントとは MCP で連携します。

## 現在の状態（Phase 1：基盤・DX12 統合・診断環境）

| 項目 | 状態 |
|---|---|
| KSL からのロード、Unity の D3D12 デバイス／キューの取得（`IUnityGraphicsD3D12v7`） | ゲーム内で確認済み |
| DX12 テスト描画（三角形）、ゲームのバックバッファーへの合成 | ゲーム内で確認済み |
| GPU 情報、D3D12 機能判定、VRAM／性能計測、GPU タイムスタンプ | ゲーム内で確認済み |
| Dear ImGui オーバーレイ（F10） | 表示と合成はゲーム内で確認済み。マウス・キーボードの実操作は未確認（手動確認項目は phase1-report の 4 章） |
| MCP サーバー（19 ツール） | テストホストで全ツール、ゲーム内で主要ツールを確認済み |
| 終了時の解放 | ゲーム内で確認済み（リークなし、プロセスも正常終了） |
| HDRP の DX12 対応（コンピュートシェーダーの差し替え、ゲーム独自パスの回避） | ゲーム内で確認済み（ガレージが正常に表示される）。DX12 ではゲームの GPU カリングとパーティクルのカスタムパスを無効化している |

確認の詳細、基準ベンチマーク、未確認事項は [docs/development/phase1-report.md](docs/development/phase1-report.md)。
描画そのものはまだ HDRP（ゲームのレンダラー）が行い、Aether はその上に自分のパスを合成します。独自レンダラーは Phase 2 からです。

> ゲームは DX12 だけでは描画できません（HDRP のコンピュートシェーダーに DX12 のカーネルが無い）。Aether がそれを補っています。
> 経緯と仕組みは [docs/architecture/dx12-integration.md](docs/architecture/dx12-integration.md)。

## 構成

```
src/Aether.Managed/   KSL Mod（C#, net472）。ネイティブのロード、描画イベント、Unity 側の統計・シーン情報、HDRP の DX12 修正
src/Aether.Native/    ネイティブランタイム（C++20, D3D12）。AetherNative.dll とテストホスト AetherHost.exe
  Device/             Unity との境界（UnityBridge）、ホストの抽象（Host.h）、GPU 情報
  Renderer/           フレーム（コマンドリスト、テスト三角形、キャプチャ、GPU タイムスタンプ）
  Resources/          GPU リソースの登録簿（VRAM の内訳の元）
  Profiler/           フレーム統計、ベンチマーク
  Shaders/            HLSL の実行時コンパイルとエラーの保持
  ImGui/              オーバーレイ
  Ipc/                名前付きパイプと API
src/Aether.MCP/       MCP サーバー（C#, net10、stdio ⇔ 名前付きパイプ）
shaders/              HLSL
unity/                HDRP の DX12 用コンピュートシェーダーのバンドルを作るエディタースクリプト
scripts/              ビルド、配置、起動、テスト
tests/host/           テストホスト（ゲームなしでランタイムを動かす）
third_party/          Dear ImGui 1.93.0、nlohmann/json 3.12.0（どちらも MIT）
docs/                 設計と手順
```

## 使い方（開発環境）

手順の詳細は [docs/development/build.md](docs/development/build.md)。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1          # AetherNative.dll, AetherHost.exe
dotnet build src\Aether.MCP\Aether.MCP.csproj -c Release                              # MCP サーバー
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-hdrp-bundle.ps1     # HDRP の DX12 用バンドル（初回のみ、長い）
dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release                      # Aether.ksm（署名。kino\mods へ）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1                # ゲームへ配置（ゲームを閉じて）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\launch-game.ps1           # Steam 経由で -force-d3d12 起動
```

- ゲームは **`-force-d3d12`** で起動します（Steam の起動オプション、または `scripts\launch-game.ps1`）。DX11 で起動した場合、Aether は何もせずその旨をログに出します。
- **F10** でオーバーレイを開閉します（`kino\config\Aether\aether.json` の `overlayHotkey`）。
- ログ・キャプチャ・ベンチマーク結果は `kino\mods\Aether\`（`logs\aether.jsonl`、`captures\`、`benchmarks\`）。

## AI 連携（MCP）

`build\mcp\Release\Aether.MCP.exe` が MCP サーバーです。ツールの一覧とスキーマは [docs/diagnostics/mcp-api.md](docs/diagnostics/mcp-api.md)。

```json
{ "mcpServers": { "aether": { "command": "C:\\path\\to\\build\\mcp\\Release\\Aether.MCP.exe", "args": [] } } }
```

- 既定は読み取り専用です。設定変更・キャプチャ・ベンチマークなどの操作ツールは `--allow-control` を付けたときだけ提供され、さらにゲーム側の設定 `mcpControl`（`deny` / `ask` / `allow`、既定 `ask`）に従います。`ask` ではゲーム内に承認ダイアログが出ます。
- 通信はこの PC の同じユーザーからだけ届く名前付きパイプ（`\\.\pipe\Aether.Runtime`）です。AI は GPU デバイスやリソースの寿命に触れられません。

## 文書

| 文書 | 内容 |
|---|---|
| [docs/development/build.md](docs/development/build.md) | 必要なもの、ビルド、配置、テスト |
| [docs/architecture/overview.md](docs/architecture/overview.md) | 全体構成、スレッド、フェーズの計画と現状 |
| [docs/architecture/dx12-integration.md](docs/architecture/dx12-integration.md) | Unity の D3D12 デバイスとの統合方式、PoC の結果、守っている規則 |
| [docs/architecture/renderer.md](docs/architecture/renderer.md) | レンダラーの現状と Phase 2 の設計方針 |
| [docs/architecture/gpu-resources.md](docs/architecture/gpu-resources.md) | GPU リソースの追跡と VRAM の内訳 |
| [docs/diagnostics/mcp-api.md](docs/diagnostics/mcp-api.md) | MCP ツールのリファレンス |
| [docs/diagnostics/troubleshooting.md](docs/diagnostics/troubleshooting.md) | うまく動かないとき |

## 第三者のソフトウェア

Dear ImGui（MIT）、nlohmann/json（MIT）を `third_party/` に同梱しています。Unity Native Plugin API のヘッダー（Unity Companion License）はリポジトリに含めず、Unity エディターのものを参照します。
Aether は非公式の Mod であり、CarX Technologies、Unity Technologies が作成・支援・推奨しているものではありません。
