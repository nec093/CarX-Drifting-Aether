# Aether

CarX Drift Racing Online（moddable ブランチ、Unity 2023.2.22f1）向けのグラフィックオーバーホール Mod です。
ゲームのレンダラー（HDRP）を使わず、Aether 自身の DirectX 12 レンダラーでゲームを描画します。

- 描画 API は **DirectX 12 のみ**（DX11 / Vulkan のバックエンドもフォールバックもありません）。
- NVIDIA / AMD / Intel 共通の経路を基本とし、ベンダー固有機能は実行時判定のオプションにします。
- Mod ローダーは BepInEx 5（KSL 用の入口もあります）。ゲーム内 GUI は Dear ImGui。AI エージェントとは MCP で連携します。
- ゲームのロジック、物理、シーン管理、入力はそのままです。Aether が受け持つのは描画と計測・診断です。

## できること

| 分野 | 内容 |
|---|---|
| レンダラー | Unity のレンダーパイプラインとして動作（HDRP は生成されない）。クラスタード・フォワード、カスケードシャドウ、レンダーグラフ |
| 空と光 | 物理ベースの大気、時刻による太陽と月、星、ボリュームのある雲と雲の影、ゲームの昼夜との同期 |
| マテリアル | 車の塗装（クリア層、フレーク、クローム）、ホイール、ランプ、ロービーム配光のヘッドライト |
| 反射 | 画面内の反射、ソフトウェア BVH によるレイトレース反射（DXR 不要）、反射プローブ |
| 間接光 | Radiance Cascades（レイトレース）、画面内の照り返し、アンビエントオクルージョン |
| フリーカメラ | F11 で入り切り（UI を隠す。移動、見回し、画角の変更）。その間はパストレーシングで描く |
| 効果 | ボリュメトリックフォグ、タイヤスモーク（独自の運動モデル）、ブルーム、レンズフレア、モーションブラー、TAA、自動露出、トーンマップ |
| 計測・診断 | VRAM の内訳、GPU タイムスタンプ、ベンチマーク、フレームキャプチャ、オーバーレイ（F10）、MCP サーバー |

各機能の仕組みと設定は [docs/architecture/rendering-features.md](docs/architecture/rendering-features.md)。

設定は F10 のオーバーレイにあります。ゲームのグラフィック設定は読みません。

## 構成

```
src/Aether.Managed/   C# の Mod（net472）
  Loaders/            BepInEx / KSL の入口
  Pipeline/           Unity のレンダーパイプライン（AetherPipeline）
  SceneCapture/       シーンの読み取り（メッシュ、マテリアル、ライト、パーティクル、タイヤスモーク）
  GameIntegration/    ゲーム固有の対応（DX12 の回避策、品質設定、昼夜）
  Diagnostics/        開発用の補助
src/Aether.Native/    ネイティブランタイム（C++20, D3D12）。AetherNative.dll とテストホスト AetherHost.exe
  Device/             Unity との境界（UnityBridge）、ホストの抽象（Host.h）、GPU 情報
  Renderer/           フレーム、シーンレンダラー、レンダーグラフ、BVH、煙の運動、GPU タイムスタンプ
  Scene/              シーンデータの保管
  Resources/          GPU リソースの管理（VRAM の内訳の元）
  Profiler/           フレーム統計、ベンチマーク
  Shaders/            HLSL の実行時コンパイルとエラーの保持
  ImGui/              オーバーレイ
  Ipc/                名前付きパイプと API
  Core/               設定、ログ、共通
src/Aether.MCP/       MCP サーバー（C#, net10、stdio ⇔ 名前付きパイプ）
shaders/              HLSL
unity/                HDRP の DX12 用コンピュートシェーダーのバンドルを作るエディタースクリプト（参照用の hdrp モード向け）
scripts/              ビルド、配置、起動、テスト
tests/host/           テストホスト（ゲームなしでランタイムを動かす）
third_party/          Dear ImGui、nlohmann/json、XeGTAO
docs/                 設計と手順
```

## 使い方（開発環境）

手順の詳細は [docs/development/build.md](docs/development/build.md)。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1     # AetherNative.dll, AetherHost.exe
dotnet build src\Aether.MCP\Aether.MCP.csproj -c Release                         # MCP サーバー
dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release -p:Loader=BepInEx "-p:GameDir=<ゲームのフォルダー>"   # C# の Mod
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1           # ゲームへ配置（ゲームを閉じて）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\launch-game.ps1      # Steam 経由で -force-d3d12 起動
```

- ゲームは **`-force-d3d12`** で起動します（Steam の起動オプション、または `scripts\launch-game.ps1`）。DX11 で起動した場合、Aether は何もせずその旨をログに出します。
- **F10** でオーバーレイを開閉します。
- **F11** でフリーカメラに入ります（UI を隠す。WASD で移動、マウスで見回し、ホイールで画角。その間はパストレーシング）。
- ログ・キャプチャ・ベンチマーク結果は `<ゲーム>\BepInEx\plugins\Aether\`（`logs\aether.jsonl`、`captures\`、`benchmarks\`）、設定は `<ゲーム>\BepInEx\config\Aether\aether.json`。

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
| [docs/development/build.md](docs/development/build.md) | 必要なもの、ビルド、配置、テスト、開発用の補助 |
| [docs/architecture/overview.md](docs/architecture/overview.md) | 全体構成、スレッド、フレームの流れ |
| [docs/architecture/dx12-integration.md](docs/architecture/dx12-integration.md) | Unity の D3D12 デバイスとの統合方式、守っている規則 |
| [docs/architecture/hdrp-independence.md](docs/architecture/hdrp-independence.md) | ゲームのレンダーパイプラインの置き換え、レンダーモード |
| [docs/architecture/renderer.md](docs/architecture/renderer.md) | レンダラーの骨組み（レンダーグラフ、パス、バインド、ライティング） |
| [docs/architecture/rendering-features.md](docs/architecture/rendering-features.md) | 描画機能（空、雲、マテリアル、反射、間接光、煙、ポストプロセス）と設定 |
| [docs/architecture/scene-capture.md](docs/architecture/scene-capture.md) | シーンの読み取り |
| [docs/architecture/scene-data-contract.md](docs/architecture/scene-data-contract.md) | C# とネイティブの間のシーンデータの取り決め |
| [docs/architecture/material-translation.md](docs/architecture/material-translation.md) | ゲームのマテリアルの読み替え |
| [docs/architecture/gpu-resources.md](docs/architecture/gpu-resources.md) | GPU リソースの管理と VRAM の内訳 |
| [docs/diagnostics/mcp-api.md](docs/diagnostics/mcp-api.md) | MCP ツールのリファレンス |
| [docs/diagnostics/troubleshooting.md](docs/diagnostics/troubleshooting.md) | うまく動かないとき |
| [AGENT.md](AGENT.md) | このリポジトリで作業するエージェントへの指示 |

## 第三者のソフトウェア

Dear ImGui（MIT）、nlohmann/json（MIT）、Intel XeGTAO（MIT）を `third_party/` に同梱しています。Unity Native Plugin API のヘッダー（Unity Companion License）はリポジトリに含めず、Unity エディターのものを参照します。
Aether は非公式の Mod であり、CarX Technologies、Unity Technologies が作成・支援・推奨しているものではありません。
