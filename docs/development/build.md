# ビルドと配置

## ローダー（2026-11-12 から BepInEx が既定）

C# 側はローダーに依存しないコア（`AetherCore`、`IAetherHost`）と、ローダーごとの入口（`src/Aether.Managed/Loaders/`）に分かれています。
どちらを組み込むかはビルド時に選びます（`-p:Loader=BepInEx|Ksl|None`。`None` はコアだけのコンパイル確認）。`scripts\deploy.ps1` の既定は `-Loader BepInEx` です。

| | BepInEx（既定） | KSL |
|---|---|---|
| 入口 | `Loaders/BepInEx/AetherPlugin.cs` | `Loaders/Ksl/AetherKslMod.cs` |
| 必要なもの | ゲームに BepInEx 5（Mono、x64）を導入 | KSL SDK、`maykr.exe`、ビルドキー（署名） |
| Aether の置き場所 | `BepInEx\plugins\Aether\`（`Aether.dll`、`native\`、`shaders\`、`hdrp\`、`logs\`、`captures\`、`benchmarks\`） | `kino\mods\Aether.ksm` と `kino\mods\Aether\` |
| 設定 | `BepInEx\config\Aether\aether.json` | `kino\config\Aether\aether.json` |
| 署名 / サーバー | 不要 | 必要 |
| ローダー側の UI | なし（Aether のオーバーレイ F10 のみ） | KSL のパネルに状態表示 |

BepInEx の導入（この環境で行った手順）：

- 入手元：`github.com/BepInEx/BepInEx` のリリース v5.4.23.6、`BepInEx_win_x64_5.4.23.6.zip`（642,076 バイト）。
- ゲームのフォルダーに `BepInEx\`、`doorstop_config.ini`、`.doorstop_version` を置く。
- **プロキシ DLL は `winhttp.dll` ではなく `version.dll` という名前で置く。** ゲームのフォルダーの `winhttp.dll` は KSL 本体なので上書きしない（Doorstop はどちらの名前でも動く）。KSL と BepInEx は共存する（確認済み）。
- 確認：`BepInEx\LogOutput.log` に `Loading [Aether 0.1.0]`、MCP の `get_status` の `managed.loader` が `BepInEx 5.4.23.6`。
- `deploy.ps1 -Loader BepInEx` は、KSL 側に残っている `kino\mods\Aether.ksm` / `Aether.dll` を削除します（2 つのローダーから二重に起動しないため）。

以下の「必要なもの」「ビルド」「配置」のうち KSL に関する記述は、`-Loader Ksl` を使う場合のものです。
## 必要なもの

| もの | 用途 | 備考 |
|---|---|---|
| Visual Studio 2026（C++ ツール）、Windows SDK 10.0.26100 以降 | ネイティブ | CMake のジェネレーターは `Visual Studio 18 2026` |
| CMake 3.20 以降 | ネイティブ | |
| .NET SDK 10 | C# Mod（net472 向け）、MCP サーバー（net10） | |
| Unity 2023.2.20f1 エディター | Native Plugin API のヘッダー、HDRP の DX12 用バンドルのビルド | ヘッダーは `<Editor>\Data\PluginAPI`。別の場所なら `-UnityPluginApi` か環境変数 `AETHER_UNITY_PLUGIN_API` |
| KSL SDK（`KSL.API.dll`、`maykr.exe`） | C# Mod | `lib/README.md`。リポジトリには含めない |
| KSL のビルドキー `Aether_maykr.kmc` | `.ksm` の署名 | ゲームの `kino\dev\` かリポジトリ直下に置く。**コミットしない**（`.gitignore` 済み） |

## ビルド

| 対象 | コマンド | 出力 |
|---|---|---|
| ネイティブ（`AetherNative.dll`、`AetherHost.exe`） | `powershell -File scripts\build-native.ps1 [-Config RelWithDebInfo] [-Clean]` | `build\native\<Config>\` |
| MCP サーバー | `dotnet build src\Aether.MCP\Aether.MCP.csproj -c Release` | `build\mcp\Release\Aether.MCP.exe` |
| C# Mod | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release` | `build\managed\Release\Aether.dll` と、`maykr.exe` とキーがあれば `kino\mods\Aether.ksm` |
| HDRP の DX12 用バンドル | `powershell -File scripts\build-hdrp-bundle.ps1` | `build\unity\aether_hdrp_dx12.bundle` |

- C# Mod はゲームの `Managed` フォルダーの DLL を参照します。ゲームが別の場所にあるときは `-p:GameDir="D:\...\CarX Drift Racing Online"`。
- **この環境の KSL アカウントは生の `.dll` の Mod を読み込めません**（`kino\output.log`: `you don't have permissions to load KSL .dll mods`）。署名済みの `.ksm` が必要です。
- 署名は KSL のサーバーに問い合わせます。続けて要求すると `Build request denied` で拒否されることがあります（少し待って再実行）。Release ビルドは `Aether.dll` が `.ksm` より新しいときだけ署名します。
- HDRP のバンドルは、HDRP のバージョンとゲームの HDRP 設定（`unity\hdrp-game-settings.json`）が変わらない限り一度作れば足ります。Unity のプロジェクトはリポジトリの外（既定 `%USERPROFILE%\Aether-Unity`）に作られます。初回は HDRP のインポートで時間がかかります（以後は数分）。

## 配置

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Build] [-Remove]
```

ゲームを閉じてから実行します（DLL がロックされるため、起動中は拒否します）。

| ファイル | 配置先（ゲームのフォルダー） |
|---|---|
| `Aether.ksm` | `kino\mods\` |
| `AetherNative.dll`（と `.pdb`） | `kino\mods\Aether\native\` |
| `shaders\*.hlsl` | `kino\mods\Aether\shaders\` |
| `aether_hdrp_dx12.bundle` | `kino\mods\Aether\hdrp\` |

- `AetherNative.dll` は Mod が起動時に `<Game>_Data\Plugins\x86_64\` へコピーします（Unity のプラグインローダーに読み込ませるため。理由は [dx12-integration.md](../architecture/dx12-integration.md)）。`deploy.ps1 -Remove` はこのコピーも消します。
- `native\` というサブフォルダーに置くのは、KSL が `kino\mods\<フォルダー>\` 直下の `.dll` をすべて Mod 候補として調べるためです。
- ゲーム本体のファイルは書き換えません。設定は `kino\config\Aether\aether.json`（`-Remove` でも残ります）。

## テスト

| テスト | コマンド | 内容 |
|---|---|---|
| テストホスト | `powershell -File scripts\test-host.ps1` | ゲームなしでランタイムを動かす。D3D12 デバッグレイヤー有効、オーバーレイ、ウィンドウのリサイズ、フレームキャプチャ。`HOST OK` で合格（D3D12 エラー 0、Aether のエラー 0、デバイスオブジェクトのリーク 0） |
| MCP の結合テスト | `powershell -File scripts\test-mcp.ps1` | テストホスト + MCP サーバーで全ツール、権限（読み取り専用、承認のタイムアウト、不正な引数）、ゲーム停止時の応答。`MCP TEST OK` で合格 |
| ゲーム内 | `scripts\launch-game.ps1`、`scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl]` | 起動して MCP 経由で状態を読む。`capture_frame` で画面を PNG に保存できる |

変更した部分に関係するテストだけを実行します。デバイス、フェンス、リソースの寿命、Present まわりを変えたときは、テストホスト（デバッグレイヤー）とゲーム内の両方で確認します。
ゲーム内でデバッグレイヤーを使うときは `scripts\launch-game.ps1 -DebugLayer`（Unity の `-force-d3d12-debug`。メッセージはゲームの `Player.log`）。
