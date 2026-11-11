# AGENT.md — Aether で作業するエージェントへの指示

## 1. 言語と報告

- 回答と報告は日本語で書く。コード、コミットメッセージ、ログ文字列は英語でよい。
- 報告は簡潔に。各大フェーズの完了時は次の 7 項目だけを書く：実装した機能／変更した主要ファイル／ビルド結果／実施した動作確認と結果／未実装項目／現在の問題点／次のフェーズで実施する内容。
- **確認できたことと未確認のことを必ず分けて書く。** 「テストホストで確認」「ゲーム内で確認」「未確認」を区別する。

## 2. プロジェクトの前提

- CarX Drift Racing Online（moddable ブランチ、Unity 2023.2.22f1、HDRP 16、64-bit）向けの KSL Mod。最終目標は HDRP に頼らない独自の DX12 レンダラー。
- **DirectX 12 のみ。** DX11 / Vulkan のバックエンド、フォールバック、複数 API 向けの抽象化は作らない。機能の有無は GPU ごとに実行時判定し、無い機能だけを切る。
- NVIDIA / AMD / Intel の共通経路を基本にする。ベンダー固有機能はオプション。
- Unity のゲームロジック、物理、シーン管理、入力は維持する。Aether が受け持つのは描画と計測・診断。
- HDRP は最初から止めない。並行で動かして比較し、段階的に置き換える。
- AI（MCP）には GPU デバイスやリソースの寿命を直接触らせない。操作は検証済みの入口だけを通す。
- 開発は 4 つの大フェーズ（README と `docs/architecture/overview.md`）。現在は Phase 1。

## 3. 遵守事項

- **`Aether_maykr.kmc`（KSL のビルドキー）をコミットしない、アップロードしない、内容を表示しない。** 置き場所はゲームの `kino\dev\`。その場所を参照して使う（`*.kmc` は `.gitignore` 済み）。
- **ローカルの NatureFX プロジェクト（隣のフォルダー）は読まない。** 内容の精度が低いとユーザーが判断している。根拠はゲーム本体（`Player.log`、`kino\output.log`、ゲームのアセンブリ）、Unity / KSL の公式ヘッダーと文書、このプロジェクトの PoC から取る。
- push はユーザーから依頼があったときだけ。コミットもユーザーの指示に従う。
- ゲームの exe や既存ファイルを書き換えない。Aether が足すファイルは `docs/development/build.md` の表のものだけ。
- ファイルをダウンロードする前に、ファイル名・入手元・サイズを伝えてユーザーの許可を得る。
- 逆コンパイルしたゲームのコードや、ゲームの素材をリポジトリに入れない。
- 公式 API やゲームの実際の挙動を確認せずに、推測で実装しない。不確かな点は最小の PoC で確かめてから本実装に進む。
- 調査だけで終わらせない。実装できる部分はコードを書き、ビルドと動作確認まで行う。

## 4. ビルド・配置・テスト

詳細は `docs/development/build.md`。

| 対象 | コマンド |
|---|---|
| ネイティブ | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1` |
| MCP サーバー | `dotnet build src\Aether.MCP\Aether.MCP.csproj -c Release` |
| C# Mod（署名つき `.ksm` を `kino\mods` へ） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release` |
| C# のコンパイル確認だけ（署名しない） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Debug` |
| HDRP の DX12 用バンドル | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-hdrp-bundle.ps1`（長い。バックグラウンドで実行する） |
| 配置（ゲームを閉じて） | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1` |
| テストホスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-host.ps1`（`HOST OK`） |
| MCP の結合テスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-mcp.ps1`（`MCP TEST OK`） |
| ゲームの起動 / 終了 | `scripts\launch-game.ps1`（Steam 経由、`-force-d3d12`）、`scripts\launch-game.ps1 -Stop` |
| ゲーム内の状態を読む | `scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl]` |

テストの方針：

- 変更に関係するテストだけを実行する。成功済みのテストを理由なく繰り返さない。
- デバイス、フェンス、リソースの寿命、Present、HDRP の停止に関わる変更は、テストホスト（D3D12 デバッグレイヤー）とゲーム内の両方で確認する。
- ゲーム内の絵は `capture_frame`（PNG）で確認できる。入力の自動化はしていないので、マウス・キーボード操作が要る確認はユーザーに具体的に依頼する。
- ユーザーがゲームを使っているときに、ゲームを勝手に終了させない。

## 5. 守るべき設計上の規則

`docs/architecture/dx12-integration.md` の「守っている規則」が正。要点：

- D3D12 のオブジェクトに触るのは Unity の描画スレッドだけ。コマンドリストは `IUnityGraphicsD3D12v7::ExecuteCommandList` で Unity に渡し、Unity のリソースの状態は `states` で宣言する（勝手に遷移させない）。
- バックバッファーへの参照をフレームをまたいで持たない。
- コマンドリスト、アロケーター、オーバーレイのバッファーは、フレームフェンスが該当の値を超えてから再利用する。
- 失敗したら Aether だけが止まり、ゲームは動き続ける。原因はログとクラッシュレポートに残す。
- 解放は GPU が終わったと確認できてから。確認できなければ解放しない。
- グローバルなフック（Present、D3D12、DXGI）を入れない。
- メインスレッドと IPC のスレッドは描画スレッドを待たせない。
- C API（`src/Aether.Native/include/AetherApi.h`）を変えたら `AETHER_API_VERSION` と `Native.ApiVersion` を一緒に上げ、`.ksm` と DLL を同時に配置する。
- 設定を足すときは `Core/Config.cpp` の 1 か所（値、スキーマ、検証）に足す。オーバーレイと MCP は同じ入口を使う。
- MCP のツールを足すときは、`Ipc/Api.cpp`、`src/Aether.MCP/Tools/ToolCatalog.cs`（入力・出力スキーマ）、`docs/diagnostics/mcp-api.md`、`scripts/test-mcp.ps1` をそろえる。
- 文書はコードと一致させる。変更したら該当の文書も直す。

## 6. この環境で分かっていること（2026-11-11 時点、すべてこのプロジェクトで確認）

- ゲーム：`C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online`（Steam の AppID 635260）。起動オプションなしだと DX11。`-force-d3d12` で DX12。
- KSL 1.0.14。このアカウントは `ModDeveloper` だが **生の `.dll` の Mod は読み込めない**。署名済みの `.ksm` が必要。
- maykr の署名は KSL のサーバーに問い合わせる。続けて要求すると `Build request denied` になることがある（待って再実行）。不要な Release ビルドを繰り返さない。
- KSL は `kino\mods\<フォルダー>\` 直下の `.dll` をすべて Mod 候補として調べる。ネイティブ DLL は `kino\mods\Aether\native\` に置く。
- `IUnityInterfaces` は Unity のプラグインローダー経由（`<Game>_Data\Plugins\x86_64\`）でしか受け取れない前提で実装している。`LoadLibrary` だけで `UnityPluginLoad` が呼ばれるかは未確認（`kino\config\Aether\dev-preload` で試せる）。
- **DX12 では、ゲームの HDRP は単体では構築できない**（`Kernel 'KDepthDownsample8DualUav' not found`。HDRP のコンピュートシェーダーに DX12 のカーネルが無い）。`HdrpDx12Fix` とバンドルで補う。
- HDRP が動くようになった後、ゲーム独自の HDRP カスタムパス 2 つ（`GpuCulling.GpuCullingPass`、`ParticleRenderPass`）も DX12 のカーネルが無く、毎フレーム例外でフレームの残りを止める。`GameDx12Workarounds` が無効化する（ソースが無いので作り直せない）。
- HDRP のバンドルを作るとき、HDRP のビルド時ストリッパーは HDRP アセットが無いと全カーネルを除去する。バリアントは全組み合わせを作らない（`Deferred` だけで 6,696、36 並列で 35 分以上）。ゲームの設定（`unity/hdrp-game-settings.json`）の組み合わせだけをビルドする（518 バリアント、数分）。
- Unity のアセットバンドルの増分ビルドは、ストリッパーの結果が変わっても前回の結果を再利用するので、強制再ビルドにしている。
- ImGui の Win32 バックエンドは、実カーソルがウィンドウ外にあると即座にマウス離脱扱いにする。投稿メッセージ（`PostMessage`）でのマウス操作の検証はできない（F10 の `WM_KEYDOWN` は効く）。
- Unity のビルド用プロジェクトはリポジトリの外（`%USERPROFILE%\Aether-Unity`）。Documents 配下は Unity が書き込めないことがある。
- ゲームのウィンドウを閉じた後、プロセスが残ることがある（HDRP が構築できていない DX12 の状態で数分動かした後。Aether を読み込んでいない起動でも発生）。HDRP が正常な状態では約 17 秒で終了する。
- 開発は 4 つの大フェーズ。Phase 1 の結果は `docs/development/phase1-report.md`。
- .NET Framework のリフレクションではゲームの HDRP アセンブリの型を読み込めない（既定インターフェースメソッド）。メタデータを読む（`System.Reflection.Metadata`）。

## 7. 場所

| もの | 場所 |
|---|---|
| ログ（KSL） | `<ゲーム>\kino\output.log` |
| ログ（Aether） | `<ゲーム>\kino\mods\Aether\logs\aether.jsonl` |
| ログ（Unity） | `%USERPROFILE%\AppData\LocalLow\CarX Technologies\Drift Racing Online\Player.log` |
| キャプチャ、ベンチマーク | `<ゲーム>\kino\mods\Aether\captures\`、`benchmarks\` |
| 設定 | `<ゲーム>\kino\config\Aether\aether.json` |
| Unity のビルド用プロジェクト | `%USERPROFILE%\Aether-Unity`（ログは `Logs\aether_build.log`） |
| Unity Native Plugin API のヘッダー | `C:\Program Files\Unity\Hub\Editor\2023.2.20f1\Editor\Data\PluginAPI` |
