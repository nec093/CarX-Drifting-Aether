# AGENT.md — Aether で作業するエージェントへの指示

## 1. 言語と報告

- 回答と報告は日本語で書く。コード、コミットメッセージ、ログ文字列は英語でよい。
- 報告は簡潔に。実装した内容、ビルドとテストの結果、未実装・既知の問題を書く。
- **確認できたことと未確認のことを必ず分けて書く。** 「テストホストで確認」「ゲーム内で確認」「未確認」を区別する。

## 2. プロジェクトの前提

- CarX Drift Racing Online（moddable ブランチ、Unity 2023.2.22f1、HDRP 16、64-bit）向けの Mod。ローダーは BepInEx 5 が既定（KSL 用の入口も残してある。`docs/development/build.md` の「ローダー」）。C# のコア（`AetherCore`）はローダーの型を参照しない。ローダーに求めるものは `IAetherHost` に足す。描画は HDRP に頼らない Aether 自身の DX12 レンダラーが行う。
- **DirectX 12 のみ。** DX11 / Vulkan のバックエンド、フォールバック、複数 API 向けの抽象化は作らない。機能の有無は GPU ごとに実行時判定し、無い機能だけを切る。
- NVIDIA / AMD / Intel の共通経路を基本にする。ベンダー固有機能はオプション。
- Unity のゲームロジック、物理、シーン管理、入力は維持する。Aether が受け持つのは描画と計測・診断。
- **既定のレンダーモード `aether` では HDRP は生成されない。** Aether 自身が Unity のレンダーパイプライン（`AetherPipeline`）で、ゲームのパイプラインアセットと差し替わる（`docs/architecture/hdrp-independence.md`）。`hdrp` / `compare` モードは純正の絵と数字を取るための参照用で、フォールバックではない。
- AI（MCP）には GPU デバイスやリソースの寿命を直接触らせない。操作は検証済みの入口だけを通す。
- **設定は Aether 自身のもの（F10 のオーバーレイ）。** ゲームのグラフィック設定は読まない。画質に関わるものを足すときは F10 に出す。
- 目標は 3840 × 2160 で 60 fps。描画機能の現状は `docs/architecture/rendering-features.md`。

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
| C# Mod（BepInEx 用） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release -p:Loader=BepInEx "-p:GameDir=<ゲームのフォルダー>"`（**`-p:Loader=BepInEx` が無いと別の出力先に出て、`deploy.ps1` が配置するものは更新されない**） |
| C# Mod（KSL 用、署名つき `.ksm` を `kino\mods` へ） | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Release -p:Loader=Ksl` |
| C# のコアだけのコンパイル確認 | `dotnet build src\Aether.Managed\Aether.Managed.csproj -c Debug -p:Loader=None` |
| HDRP の DX12 用バンドル | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-hdrp-bundle.ps1`（長い。バックグラウンドで実行する） |
| 配置（ゲームを閉じて） | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1`（既定 `-Loader BepInEx`。`-Build` でビルドから） |
| 放置テスト（安定性の実験） | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-test.ps1 -Name <名前> -Mode hdrp|aether|compare -Reps 3` |
| テストホスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-host.ps1`（`HOST TEST OK`） |
| MCP の結合テスト | `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-mcp.ps1`（`MCP TEST OK`） |
| ゲームの起動 / 終了 | `scripts\launch-game.ps1`（Steam 経由、`-force-d3d12`。`-DebugLayer` で D3D12 デバッグレイヤー）、`scripts\launch-game.ps1 -Stop` |
| ゲーム内の状態を読む | `scripts\mcp-call.ps1 <tool> [key=value ...] [-AllowControl]` |

テストの方針：

- 変更に関係するテストだけを実行する。成功済みのテストを理由なく繰り返さない。
- デバイス、フェンス、リソースの寿命、Present、HDRP の停止に関わる変更は、テストホスト（D3D12 デバッグレイヤー）とゲーム内の両方で確認する。
- **絵の差は純正と並べて数値で比べる。** 設定フォルダーの `dev-camera.txt`（`px py pz tx ty tz [fov]`）で画面カメラを固定し、`set_render_mode mode=hdrp` と `mode=aether` で同じ視点を撮って、同じ点の色を比べる。自動露出が明るさの差を打ち消すので、明るさを比べるときは露出を固定する（`get_material_info detail=survey` の `hostExposureMultiplier` が純正の実測値。配置先のシェーダーを一時的に書き換えて `reload_shader`）。終わったら `dev-camera.txt` を消す。
- 原因が分からない差は、推測で調整せず、survey（`volumes`、`propertyBlocks`、`transparentMaterials`、`screenProbes`、ライトの HDRP 用データ）でゲームのデータを先に確認する。差の原因は、読んでいないデータ（MaterialPropertyBlock、無効化されたライトデータ）や式の取り違えであることが多い。
- ゲーム内の絵は `capture_frame`（PNG）で確認できる。入力の自動化はしていないので、走行など操作が要る確認はユーザーに具体的に依頼する。走らせられない場面は開発用の補助で再現する（`dev-camera.txt`、タイヤスモークの `dev-smoke.txt`。`docs/development/build.md`）。
- **キャプチャでは分からない不具合がある。** 画面の点滅はフレームが飛んでいる状態で、キャプチャは描けたフレームしか写さない。`get_status` の `renderer.commandLists` を読む。
- 重い対照実験を繰り返さない。先にログと計測値から原因を絞る。ゲームの実際の値が分からないときは、まずその値をログに出す。
- ゲームの再起動は確認なしで行ってよい（所有者の指示）。固定したカメラや開発用のファイル、変更した設定は、確認が終わったら元に戻す。

## 5. 守るべき設計上の規則

`docs/architecture/dx12-integration.md` の「守っている規則」が正。要点：

- D3D12 のオブジェクトに触るのは Unity の描画スレッドだけ。コマンドリストは `IUnityGraphicsD3D12v7::ExecuteCommandList` で Unity に渡す。**渡すのは、キューへのアクセスを許可した設定（`kUnityD3D12GraphicsQueueAccess_Allow`）のイベントの中だけ。** Unity のリソース（バックバッファー、借りているメッシュやテクスチャー）の状態は変えず、`states` も渡さない。
- バックバッファーへの参照をフレームをまたいで持たない。
- リソースの寿命の時計は Aether 自身の完了マーカー（`Renderer::Completed()`）。Unity のフレームフェンスだけを信用しない（提出が別スレッドなので、返された値が実行の完了を保証しない）。コマンドリストは、完了の確認に加えて Unity のフェンスの通過と一定回数の提出を待ってから再利用する。
- Scene Data Contract（`include/SceneContract.h` と `SceneCapture/Contract.cs`）を変えたら両方を直し、`AE_SCENE_CONTRACT_VERSION` を上げる。
- 失敗したら Aether だけが止まり、ゲームは動き続ける。原因はログとクラッシュレポートに残す。
- 解放は GPU が終わったと確認できてから。確認できなければ解放しない。
- グローバルなフック（Present、D3D12、DXGI）を入れない。
- メインスレッドと IPC のスレッドは描画スレッドを待たせない。
- C API（`src/Aether.Native/include/AetherApi.h`）を変えたら `AETHER_API_VERSION` と `Native.ApiVersion` を一緒に上げ、`.ksm` と DLL を同時に配置する。
- 設定を足すときは `Core/Config.cpp` の 1 か所（値、スキーマ、検証）に足す。オーバーレイと MCP は同じ入口を使う。
- MCP のツールを足すときは、`Ipc/Api.cpp`、`src/Aether.MCP/Tools/ToolCatalog.cs`（入力・出力スキーマ）、`docs/diagnostics/mcp-api.md`、`scripts/test-mcp.ps1` をそろえる。
- 文書はコードと一致させる。変更したら該当の文書も直す。

## 6. この環境で分かっていること（すべてこのプロジェクトで確認）

- ゲーム：`C:\Program Files (x86)\Steam\steamapps\common\CarX Drift Racing Online`（Steam の AppID 635260）。起動オプションなしだと DX11。`-force-d3d12` で DX12。
- KSL は Aether を `BannedMod` として読み込まない（理由は未確認）。これを回避する操作はしない。BepInEx で読み込む。
- ゲームのフォルダーの `winhttp.dll` は KSL 本体。BepInEx のプロキシは `version.dll` という名前で置く（上書きしない）。
- DX12 では ANZU の動画広告（Unity の `VideoPlayer`）を既定で止める（切り替え時に Unity の動画フレームのアップロードが壊れ、D3D12 のリソース破損でクラッシュするため）。`keep-game-videos` で止めない、`disable-game-ads` で ANZU のコンポーネントも止める。ANZU のネイティブ描画は D3D9 / D3D11 / OpenGL だけで、DX12 は想定外。動画を止める対処は外さない。
- 安定性の実験は結果が確率的。1 条件 3 回未満で「安定」と判断しない。
- KSL は生の `.dll` の Mod を読み込まない。署名済みの `.ksm` が必要。
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
- スキンメッシュの頂点を毎フレーム読み戻す（`AsyncGPUReadback`）と、Unity のフレームフェンスが遅れ、Aether のコマンドリストが再利用待ちで足りなくなってフレームが飛ぶ。既定で読み戻さない（`skinnedRayUpdates = 0`）。
- ゲームの `HDRP.MasterVolumes` は Aether のパイプラインでも動き、`QualitySettings`（VSync、LOD バイアスなど）を毎フレーム書き戻す。Aether は描画の後で毎フレーム自分の値を適用する（`HostQualitySettings.Enforce`）。
- VSync がオンのまま測った GPU 時間は当てにならない（GPU が周波数を落とす）。計測は `vsync = 0` で行う。
- ピクセルシェーダーで BVH をたどるパスの時間は、レイの本数より最も遅いレイで決まる。距離とステップ数に上限を置く。
- ゲームのタイヤスモークの発生：ドリフト中 1 台あたり毎秒 500 個強、1 フレーム分は同じ 1 点に出る。ゲーム側の不透明度は 0.04〜0.09、発生サイズは 0.08〜0.10 m（小さく薄い粒を大量に重ねる設計）。
- コースの夜はゲームの DLC（`GameManager.NightModeInstalled`）。無いときに Aether から夜へ切り替えない。ガレージの昼夜切り替えは誰でも使える。
- Unity の `Mathf.SmoothStep(a, b, t)` は a と b の補間で、GLSL / HLSL の smoothstep とは別物。
- HLSL では `pass` は予約語。Windows のヘッダーを含む C++ では `near` / `far` がマクロ。
- D3D12 のデバッグレイヤーのメッセージは Unity の `Player.log` には出ない。Aether がコールバックで自分のログに写す（`E_D3D12_VALIDATION` / `E_D3D12_CORRUPTION`）。ゲーム内の検証は `launch-game.ps1 -DebugLayer` で起動して `get_logs` を読む。
- Unity のクラッシュダンプは `%LOCALAPPDATA%\Temp\CarX Technologies\Drift Racing Online\Crashes\`。例外コード `0x87D` はデバッグレイヤーが破損を検出して投げた例外。
- ガレージの値：太陽 100 lux。露出は自動（EV 2.05〜2.65、補正 0.66。実測は 2.05 付近）。トーンマッピングは Custom（Hable。**HDRP の Shoulder Strength は 0〜1 の遷移、Shoulder Length が F ストップ**で、Hable の原典と名前が逆）。空は HDRI `redring_s`（exposure 2 → 強度 2^2 / 1.2、回転 56°）。ブルーム 0.2、モーションブラー 0.5、カメラのアンチエイリアスは TAA。既定のボリュームプロファイルは `MasterGradeProfile`。
- 車の色・金属度・粗さ・クリアコート・リバリー（ボディの `_BaseMap`）・ライトの点灯は、レンダラーごとの `MaterialPropertyBlock` で渡される（ライトはマテリアルスロットごとのブロック）。共有マテリアルの値は既定値でしかない。
- ガレージのスポットライトは `Light` が有効でも `HDAdditionalLightData` が無効で、純正では点灯していない。
- Unity の `VolumeManager` は現在のパイプライン用のコンポーネント型しか読み込まず、パイプラインの破棄で終了する。Aether のパイプラインでは Aether が起動し、HDRP 用の型を読み込ませる（`PipelineSwitch.EnsureVolumes`）。
- 実行時に作られるメッシュは、初めて見たときは頂点 0（ガレージに 4 個）。動的メッシュとして追跡する。
- シェルのヒアドキュメントや `sed` はバックスラッシュ（`\U…` など）を壊すことがある。パスを含む編集は編集ツールか、ファイルに書いたスクリプトで行う。
- マテリアルの構成は `docs/architecture/material-translation.md`。
- .NET Framework のリフレクションではゲームの HDRP アセンブリの型を読み込めない（既定インターフェースメソッド）。メタデータを読む（`System.Reflection.Metadata`）。

## 7. 場所

| もの | 場所 |
|---|---|
| ログ（KSL） | `<ゲーム>\kino\output.log` |
| ログ（Aether） | `<ゲーム>\BepInEx\plugins\Aether\logs\aether.jsonl`（KSL のときは `kino\mods\Aether\logs\`） |
| ログ（BepInEx） | `<ゲーム>\BepInEx\LogOutput.log` |
| ログ（Unity） | `%USERPROFILE%\AppData\LocalLow\CarX Technologies\Drift Racing Online\Player.log` |
| キャプチャ、ベンチマーク | `<ゲーム>\BepInEx\plugins\Aether\captures\`、`benchmarks\` |
| 設定 | `<ゲーム>\BepInEx\config\Aether\aether.json`（KSL のときは `kino\config\Aether\`） |
| Unity のビルド用プロジェクト | `%USERPROFILE%\Aether-Unity`（ログは `Logs\aether_build.log`） |
| 開発用のファイル（`dev-camera.txt`、`dev-smoke.txt`） | `<ゲーム>\BepInEx\config\Aether\` |
| Unity Native Plugin API のヘッダー | `C:\Program Files\Unity\Hub\Editor\2023.2.20f1\Editor\Data\PluginAPI` |
