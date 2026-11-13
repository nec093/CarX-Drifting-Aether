# ビルドと配置

## ローダー

C# 側は、ローダーに依存しないコア（`AetherCore`、`IAetherHost`）と、ローダーごとの入口（`src/Aether.Managed/Loaders/`）に分かれています。
どの入口を組み込むかはビルド時に `-p:Loader=BepInEx|Ksl|None` で選びます。`None` はコアだけのコンパイル確認用です。

既定のローダーは BepInEx です（`scripts\deploy.ps1` の既定が `-Loader BepInEx`）。KSL の入口も残っています。

| | BepInEx（既定） | KSL |
|---|---|---|
| 入口 | `Loaders/BepInEx/AetherPlugin.cs` | `Loaders/Ksl/AetherKslMod.cs` |
| 必要なもの | ゲームに BepInEx 5（Mono、x64）を導入 | KSL SDK（`KSL.API.dll`）、`maykr.exe`、ビルドキー |
| Aether の置き場所 | `BepInEx\plugins\Aether\` | `kino\mods\Aether.ksm` と `kino\mods\Aether\` |
| 設定 | `BepInEx\config\Aether\` | `kino\config\Aether\` |
| C# 側のログ | `BepInEx\LogOutput.log` | `kino\output.log` |
| 署名 | 不要 | 必要（KSL のサーバーに問い合わせる） |
| ローダー側の UI | なし（Aether のオーバーレイ F10 のみ） | KSL のパネルに状態表示 |

### BepInEx の導入

- BepInEx 5 の Windows x64 版（この環境では 5.4.23.6、`github.com/BepInEx/BepInEx` のリリース）の中身をゲームのフォルダーに置きます（`BepInEx\`、`doorstop_config.ini`、`.doorstop_version`）。
- **プロキシ DLL は `winhttp.dll` ではなく `version.dll` という名前で置きます。** ゲームのフォルダーの `winhttp.dll` は KSL 本体なので上書きしません。KSL と BepInEx は共存できます。
- `deploy.ps1` は `BepInEx\core\BepInEx.dll` が無いと失敗します。
- 読み込まれたかは `BepInEx\LogOutput.log` と、MCP の `get_status` の `managed.loader`（`BepInEx <版>`）で分かります。

### KSL を使う場合

- `KSL.API.dll` を `lib\` に、`maykr.exe` を `tools\` に置きます（`lib/README.md`）。どちらもリポジトリには含めません。
- ビルドキー `Aether_maykr.kmc` は、ゲームの `kino\dev\` かリポジトリ直下に置きます。**コミットしません**（`.gitignore` に `*.kmc`）。
- このアカウントは生の `.dll` の Mod を読み込めません。署名済みの `.ksm` が必要です。
- 署名は KSL のサーバーに問い合わせます。続けて要求すると `Build request denied` で断られることがあります（少し待って再実行）。署名は Release ビルドで、`Aether.dll` が `.ksm` より新しいときだけ走ります。
- この環境の KSL は Aether を `BannedMod` として読み込みません。そのため BepInEx を使っています。

## 必要なもの

| もの | 用途 | 備考 |
|---|---|---|
| Visual Studio 2026（C++ ツール）と Windows SDK | ネイティブ | CMake のジェネレーターは `Visual Studio 18 2026`（x64） |
| CMake 3.20 以降 | ネイティブ | |
| .NET SDK 10 | C# Mod（net472 向け）、MCP サーバー（net10.0） | |
| Unity 2023.2.20f1 エディター | Native Plugin API のヘッダー、HDRP の DX12 用バンドルのビルド | ヘッダーの既定は `C:\Program Files\Unity\Hub\Editor\2023.2.20f1\Editor\Data\PluginAPI`。別の場所なら `-UnityPluginApi` か環境変数 `AETHER_UNITY_PLUGIN_API` |
| NRD v4.18.0、MathLib v11、ShaderMake（`build\deps\`） | 反射のノイズ除去（`reflectionDenoiser = nrd`） | `scripts\fetch-deps.ps1` で取得（NRD は NVIDIA RTX SDKs License。実行することがライセンスへの同意）。無くてもビルドはでき、ノイズ除去が無いだけ。NRD のシェーダーは Windows SDK 10.0.26100 の `dxc.exe` でコンパイルする（CMake の `AETHER_DXC_EXE`） |
| ゲーム本体 | C# Mod の参照 | ゲームの `Drift Racing Online_Data\Managed` の DLL を参照する（BepInEx 用は `BepInEx\core\BepInEx.dll` も） |

## ビルド

| 対象 | コマンド | 出力 |
|---|---|---|
| ネイティブの依存（初回だけ） | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch-deps.ps1` | `build\deps\` |
| ネイティブ（`AetherNative.dll`、`AetherHost.exe`） | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1 [-Config RelWithDebInfo] [-Clean]` | `build\native\<Config>\` |
| MCP サーバー | `dotnet build src\Aether.MCP\Aether.MCP.csproj -c Release` | `build\mcp\Release\Aether.MCP.exe` |
| C# Mod（BepInEx 用） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release -p:Loader=BepInEx` | `build\managed\BepInEx\Release\Aether.dll` |
| C# Mod（KSL 用） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release -p:Loader=Ksl` | `build\managed\Ksl\Release\Aether.dll`。`maykr.exe` とキーがあれば `kino\mods\Aether.ksm` も |
| C# のコアだけのコンパイル確認 | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Debug -p:Loader=None` | `build\managed\None\Debug\` |
| HDRP の DX12 用バンドル | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-hdrp-bundle.ps1` | `build\unity\aether_hdrp_dx12.bundle` |

**C# Mod は必ず `-p:Loader=BepInEx` を付けてビルドします。** 出力先は `build\managed\<Loader>\<Configuration>\` です。csproj の `Loader` の既定は `Ksl` なので、何も付けない `dotnet build` は `build\managed\Ksl\Release\` に書きます。`deploy.ps1`（既定 `-Loader BepInEx`）が配置するのは `build\managed\BepInEx\Release\Aether.dll` なので、何も付けないビルドの結果は配置されません。

- ゲームが既定の場所（`C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online`）に無いときは `-p:GameDir="D:\...\CarX Drift Racing Online"` も付けます。参照する DLL と、BepInEx の `BepInEx.dll` はこのフォルダーから探します。
- `deploy.ps1 -Build` は、ネイティブと C# Mod を `-c Release -p:GameDir=<GameDir> -p:Loader=<Loader>` で一緒にビルドします。
- HDRP のバンドルは、HDRP のバージョンとゲームの HDRP 設定（`unity\hdrp-game-settings.json`）が変わらない限り一度作れば足ります。Unity のプロジェクトはリポジトリの外（既定 `%USERPROFILE%\Aether-Unity`）に作られます。初回は HDRP のインポートで長くかかります。ログは `<プロジェクト>\Logs\aether_build.log`。

## 配置

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 [-Loader BepInEx|Ksl] [-Config RelWithDebInfo] [-GameDir <dir>] [-Build] [-Remove]
```

ゲームを閉じてから実行します（DLL がロックされるため、起動中は失敗します）。

BepInEx（既定）のとき：

| ファイル（リポジトリ） | 配置先（ゲームのフォルダー） |
|---|---|
| `build\managed\BepInEx\Release\Aether.dll` | `BepInEx\plugins\Aether\` |
| `build\native\<Config>\AetherNative.dll`（と `.pdb`） | `BepInEx\plugins\Aether\native\` |
| `build\native\<Config>\dxcompiler.dll`、`dxil.dll`（Windows SDK の `Redist\D3D\x64` からビルド時にコピー） | `BepInEx\plugins\Aether\native\` |
| `shaders\*` | `BepInEx\plugins\Aether\shaders\` |
| `build\unity\aether_hdrp_dx12.bundle` | `BepInEx\plugins\Aether\hdrp\` |

- KSL 側に残っている `kino\mods\Aether.dll` / `Aether.ksm` は削除します（2 つのローダーから二重に起動しないため）。
- `-Loader Ksl` のときは、`Aether.ksm`（署名済みが無ければ生の `Aether.dll`）を `kino\mods\` に、ネイティブ、シェーダー、バンドルを `kino\mods\Aether\` に置きます。
- バンドルがリポジトリにも配置先にも無いと警告を出します（DX12 でゲーム純正のレンダラーを使うモードの画面が黒くなる）。
- `AetherNative.dll` は、Mod が起動時に `<Game>_Data\Plugins\x86_64\` へコピーします（Unity のプラグインローダーに読み込ませるため。理由は [dx12-integration.md](../architecture/dx12-integration.md)）。
- ネイティブ DLL を `native\` に置くのは、ローダーが Mod の隣の `.dll` を Mod 候補として調べるためです。
- ゲーム本体のファイルは書き換えません。設定は配置先の設定フォルダーにあり、`-Remove` でも残ります。
- `-Remove` は、そのローダーの Aether のフォルダーと `<Game>_Data\Plugins\x86_64\AetherNative.dll` を消します（KSL のときは `kino\mods\Aether.ksm` / `Aether.dll` も）。

## テスト

| テスト | コマンド | 内容 |
|---|---|---|
| テストホスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-host.ps1 [-Config RelWithDebInfo] [-Seconds 25] [-Warp] [-Show] [-Keep] [-SceneOnly]` | ゲームなしでランタイムを D3D12 デバッグレイヤー付きで動かす。1 回目はフレーム終端の経路（テスト三角形、オーバーレイ、ウィンドウのリサイズ、キャプチャ）、2 回目は検証用シーンを Scene Data Contract 経由でシーンレンダラーに描かせ、キャプチャの画素を調べる。`HOST TEST OK` で合格（D3D12 エラー 0、Aether のエラー 0、デバイスオブジェクトのリーク 0、画素の検査がすべて合格） |
| MCP の結合テスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-mcp.ps1 [-Config RelWithDebInfo] [-Keep]` | テストホストと MCP サーバーで全ツール（21 個）、権限（読み取り専用のサーバー、承認の時間切れ、不正な引数）、ランタイムが止まったときの応答を調べる。`MCP TEST OK` で合格 |
| 放置テスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-test.ps1 -Name <名前> -Mode hdrp\|aether\|compare [-Reps 1] [-Seconds 300] [-NoDebugLayer] [-Dev '<JSON>']` | ゲームを起動してガレージで放置し、終わり方（生存 / クラッシュ、D3D12 のメッセージ、Aether のエラー）を 1 回 1 行の JSON で記録する。既定の出力は Aether のデータフォルダーの `benchmarks\soak-results.jsonl` |

- テストホストと MCP の結合テストは、先に `build-native.ps1` と MCP サーバーのビルドが要ります。どちらも専用のパイプ名を使うので、ゲームのパイプとはぶつかりません。
- 放置テストは既定で D3D12 デバッグレイヤーを有効にします。実行のたびに `aether.json` の `renderMode` などを書き換え、終わると `renderMode` を `hdrp` に戻します（既定の `aether` ではありません）。`-Dev` の JSON は設定フォルダーの `dev-options.json` になり（実験用の切り替え。`src/Aether.Native/Core/DevOptions.h`）、終わると削除されます。
- 安定性の結果は確率的です。1 条件 3 回未満で「安定」と判断しません。

変更した部分に関係するテストだけを実行します。デバイス、フェンス、リソースの寿命、Present まわりを変えたときは、テストホスト（デバッグレイヤー）とゲーム内の両方で確認します。

## ゲーム内で確かめる

| すること | コマンド |
|---|---|
| ゲームの起動（Steam 経由、`-force-d3d12`） | `scripts\launch-game.ps1 [-TimeoutSeconds 180] [-DebugLayer]`。Aether のパイプが開くまで待つ |
| ゲームの終了（ウィンドウを閉じる） | `scripts\launch-game.ps1 -Stop` |
| MCP のツールを 1 回呼ぶ | `scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl] [-Raw]` |

- `-DebugLayer` は Unity の `-force-d3d12-debug` を付けます（遅くなります）。デバッグレイヤーのメッセージは Unity の `Player.log` には出ず、Aether が自分のログに写します（`component` が `D3D12`、コード `E_D3D12_VALIDATION` / `E_D3D12_CORRUPTION`）。`get_logs` で読みます。
- 画面は `capture_frame` で PNG に保存できます。ただし画面のちらつきはキャプチャでは分かりません（[troubleshooting.md](../diagnostics/troubleshooting.md)）。

## 開発用の補助

どれも設定フォルダー（BepInEx なら `BepInEx\config\Aether\`）に置くファイルか、MCP のツールです。終わったらファイルを消します。

### dev-camera.txt：画面カメラの固定

- 中身は `px py pz tx ty tz [fov]`（カメラの位置、注視点、省略可の視野角）。空白、カンマ、タブ、改行で区切ります。
- ファイルがある間、ゲームの画面カメラをその位置に固定します。ゲーム自身のカメラスクリプトの後、カリングと描画の直前に上書きします。ファイルは約 1 秒ごとに読み直します。
- 2 つの描画モード（`set_render_mode mode=hdrp` と `mode=aether`）や 2 つのビルドを、同じ視点で撮り比べるために使います。
- ファイルが無ければゲームのカメラのままです。

### dev-smoke.txt：タイヤスモークの試験用の車

- 中身は `x y z radius speed slipDegrees`（円の中心、半径、速さ、スリップ角）。
- ファイルがある間、見えない車がその円を回り、ゲームのエミッターを使わずにタイヤスモークを出します。`radius` が 0.5 以下なら出しません。ファイルは約 1 秒ごとに読み直します。

### シェーダーのホットリロード

- ランタイムはデータフォルダーの `shaders\`（BepInEx なら `BepInEx\plugins\Aether\shaders\`）からシェーダーを読みます。
- リポジトリの `shaders\` で編集したファイルをそこへコピーし、MCP の `reload_shader`（操作ツール）を呼ぶか、オーバーレイの Shaders タブの `Reload shaders` を押します。ゲームの再起動は要りません。
- `reload_shader` はコンパイラの出力を返します。コンパイルに失敗したシェーダーを使うパスは描かれません（`get_shader_errors` で確認）。
- 配置先のシェーダーを一時的に書き換えた場合は、最後に `deploy.ps1` で戻します。
