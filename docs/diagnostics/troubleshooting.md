# トラブルシューティング

## まず見る場所

パスはゲームのフォルダーからの相対です。BepInEx（既定のローダー）の場合を書き、KSL の場合を括弧に書きます。

| 場所 | 内容 |
|---|---|
| `BepInEx\LogOutput.log`（KSL：`kino\output.log`） | ローダーのログ。Aether が読み込まれたか、C# 側の `[Aether]` の行 |
| `BepInEx\plugins\Aether\logs\aether.jsonl`（KSL：`kino\mods\Aether\logs\`） | Aether の構造化ログ（1 行 1 JSON）。前回分は `aether.prev.jsonl` |
| 同じフォルダーの `crash-current.json` / `crash-previous.json` | Aether が検出した障害のレポート |
| `%USERPROFILE%\AppData\LocalLow\CarX Technologies\Drift Racing Online\Player.log` | Unity のログ（例外、グラフィックス API） |
| `%LOCALAPPDATA%\Temp\CarX Technologies\Drift Racing Online\Crashes\` | Unity のクラッシュダンプ。例外コード `0x87D` は、D3D12 デバッグレイヤーが破損を検出して投げた例外 |
| MCP の `get_status`、`get_logs`、`get_crash_report` | 上の内容を構造化して取得。まず `get_status` |

D3D12 デバッグレイヤーのメッセージは `Player.log` には出ません。Aether が自分のログに写します（`component` が `D3D12`、コード `E_D3D12_VALIDATION` / `E_D3D12_CORRUPTION`）。`scripts\launch-game.ps1 -DebugLayer` で起動して `get_logs` で読みます。

## 症状別

### 読み込み・起動

| 症状 | 原因と対処 |
|---|---|
| `BepInEx\LogOutput.log` が無い、Aether が出てこない | BepInEx が入っていない、またはプロキシ DLL の名前が違う。プロキシは `version.dll` という名前で置く（`winhttp.dll` は KSL 本体なので上書きしない）。Aether の配置は `scripts\deploy.ps1`（[build.md](../development/build.md)） |
| KSL が Aether を `BannedMod` として読み込まない | この環境の KSL の扱い。回避はしない。BepInEx で読み込む |
| `kino\output.log` に `you don't have permissions to load KSL .dll mods` | KSL で、生の `.dll` を読めないアカウント。署名済みの `Aether.ksm` が要る（`-p:Loader=Ksl` の Release ビルド、キーと `maykr.exe`） |
| `[Aether] Aether needs DirectX 12 ...` | DX11 で起動している。Steam の起動オプションに `-force-d3d12`、または `scripts\launch-game.ps1` |
| `[Aether] native runtime not available: missing ...AetherNative.dll` | Aether のフォルダーの `native\AetherNative.dll` が無い。ゲームを閉じて `scripts\deploy.ps1` |
| `AetherNative.dll has API version ...` | `Aether.dll` とネイティブ DLL の版が合っていない。両方を同じビルドから配置する（ゲームを閉じて `scripts\deploy.ps1`） |
| `Unity did not call UnityPluginLoad on AetherNative.dll` | Unity のプラグインとして読み込まれなかった。`<Game>_Data\Plugins\x86_64\AetherNative.dll` があるか、設定フォルダーに `dev-preload` を置いたままにしていないか。この状態でもログと MCP は使える |
| 起動時に「The previous session did not shut down cleanly」と出る | 前回、Aether の終了処理が走らなかった（ゲームのクラッシュ、強制終了）。`get_crash_report` に前回のレポートとログの末尾が入っている |

### 画面

| 症状 | 原因と対処 |
|---|---|
| 画面がちらつく、点滅する | Aether の処理を飛ばしたフレームがある（そのフレームには Aether の絵が出ない）。`get_status` の `renderer.commandLists` を読む：`noneFree` が増え続けていれば飛ばしている。`waitingForHostFence` が多ければ、Unity のフレームフェンスが進まずコマンドリストが空かない。既知の原因は `skinnedRayUpdates` を 0 より大きくしたとき（毎フレームの読み戻し）。0 に戻す。**キャプチャでは調べられない**（`capture_frame` は Aether が描けたフレームでしか成功しないので、撮った絵は正常に見える） |
| `hdrp` / `compare` モードで画面が黒い、`Player.log` に `Kernel 'KDepthDownsample8DualUav' not found` | DX12 でゲームの HDRP が構築できていない。Aether のフォルダーの `hdrp\aether_hdrp_dx12.bundle` が要る（`scripts\build-hdrp-bundle.ps1` → `scripts\deploy.ps1`）。状態は `get_status` の `managed.hdrpDx12Fix`。設定フォルダーに `no-hdrp-fix` を置いていないかも確認。[dx12-integration.md](../architecture/dx12-integration.md) |
| Aether の描画が止まった（オーバーレイに赤い表示、`renderer.state` が `failed`） | `get_crash_report`。HRESULT、デバイス削除の理由、取れていれば DRED、ログの末尾が入っている。ゲームは動き続ける。ゲームの再起動で戻る |
| シェーダーを書き換えたら描かれなくなった | コンパイルエラー。オーバーレイの Shaders タブか `get_shader_errors`。直して `reload_shader` |

### MCP・オーバーレイ

| 症状 | 原因と対処 |
|---|---|
| MCP のツールが `E_NOT_RUNNING` | ゲームが起動していない、Aether が起動に失敗した（上のログ）、ゲームが別ユーザー（管理者など）で動いている |
| 操作ツールが `E_CONTROL_DISABLED` | MCP サーバーが読み取り専用。`--allow-control` で起動する |
| 操作ツールが `E_NOT_APPROVED` | ゲーム内の承認ダイアログに時間内に答えていない。オーバーレイの Settings の `Control requests`（設定 `mcpControl`）で変えられる |
| オーバーレイが出ない | F10（`aether.json` の `overlayHotkey`、仮想キーコード）。KSL のときは KSL のパネルのボタンでも開ける。ログに `Could not subclass the game window` があれば入力を取れていない |

## DX12 で Aether が止めているゲームの機能

DX12 でそのままでは壊れるゲームの機能を、Aether が止めています。設定フォルダー（BepInEx なら `BepInEx\config\Aether\`）にファイルを置くと動作を変えられます。

| 対象 | 既定の動作 | 変えるファイル | 状態の表示 |
|---|---|---|---|
| ゲームの動画（Unity の `VideoPlayer`。ANZU の動画広告） | 2 秒ごとに、有効な `VideoPlayer` を無効にする。画像の広告と ANZU のほかの部分は触らない | `keep-game-videos`：何も止めない。`disable-game-ads`：加えて ANZU のコンポーネント（型名に `Anzu` を含むもの）も止める | Aether のログの `Video playback on DX12: ...`、KSL のパネル |
| ゲーム独自の HDRP カスタムパス（`GpuCulling.GpuCullingPass`、`ParticleRenderPass`、ほかに `Kernel '...' not found` で落ちるもの） | 無効にする（DX12 のカーネルが無く、毎フレーム例外でフレームの残りが止まるため） | `keep-game-passes`：止めない | `get_status` の `managed.gamePassesDx12` |
| HDRP のランタイムリソースのコンピュートシェーダー | `hdrp` / `compare` モード、または Aether が起動できないときに、DX12 用のバンドルに差し替える | `no-hdrp-fix`：差し替えない | `get_status` の `managed.hdrpDx12Fix` |

動画を止めるのは、切り替えの時に Unity の動画フレームのアップロードが壊れるためです。クリップが変わるとき、Unity は差し替え済みのテクスチャーに動画フレームを書こうとします。この不正なコピーは、デバイスの喪失やゲームのクラッシュにつながります。ANZU のネイティブ描画は D3D9 / D3D11 / OpenGL 向けで、DX12 は想定されていません。この対処は外しません（`keep-game-videos` は調べるときだけ使う）。

## 取り除く

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Remove            # BepInEx
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Remove -Loader Ksl # KSL
```

ゲームを閉じてから実行します。そのローダーの Aether のフォルダー（BepInEx：`BepInEx\plugins\Aether\`、KSL：`kino\mods\Aether\` と `kino\mods\Aether.ksm` / `Aether.dll`）と、`<Game>_Data\Plugins\x86_64\AetherNative.dll` を消します。
設定フォルダーは残ります。BepInEx 自体も残ります。

## ほかの Mod との関係

- Aether がゲームに入れるフックは、ゲームのウィンドウプロシージャのサブクラス化だけです。終了時に元に戻します。後から別の Mod がサブクラス化した場合は、Aether の分は素通しとして残ります。Present、D3D12、DXGI のフックはありません。
- オーバーレイがマウス / キーボードを使っている間は、ゲームの入力（Rewired のマウスとキーボードのコントローラー）を無効にします。閉じると戻します。
- `aether` モードでは Aether がゲームのレンダーパイプラインになり、ゲームの HDRP のパイプラインは作られません。HDRP に頼る Mod の動作は `hdrp` モードと比べて切り分けます。
- HDRP の DX12 修正は、HDRP のランタイムリソースのコンピュートシェーダーを差し替えます。同じものを差し替える別の Mod と一緒に使うときは、設定フォルダーに `no-hdrp-fix` を置いて Aether 側を切れます。
- BepInEx と KSL は共存できます。`deploy.ps1`（BepInEx）は KSL 側に残った Aether を消して、二重に起動しないようにします。
- KSL のときは、`get_status` の `managed.mods` に KSL が読み込んでいる Mod の一覧が出ます（BepInEx のときは空）。
