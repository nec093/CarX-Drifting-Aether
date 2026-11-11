# トラブルシューティング

## まず見る場所

| 場所 | 内容 |
|---|---|
| `kino\output.log` | KSL のログ。Mod が読み込まれたか、C# 側の `[Aether]` の行 |
| `kino\mods\Aether\logs\aether.jsonl` | Aether の構造化ログ（1 行 1 JSON）。前回分は `aether.prev.jsonl` |
| `kino\mods\Aether\logs\crash-current.json` / `crash-previous.json` | Aether が検出した障害のレポート |
| `%USERPROFILE%\AppData\LocalLow\CarX Technologies\Drift Racing Online\Player.log` | Unity のログ（例外、グラフィックス API、デバッグレイヤーのメッセージ） |
| MCP の `get_status`、`get_logs`、`get_crash_report` | 上と同じ内容を構造化して取得 |

## 症状別

| 症状 | 原因と対処 |
|---|---|
| `kino\output.log` に `Skipping loading 'Aether.dll', you don't have permissions to load KSL .dll mods` | 生の `.dll` は読み込めないアカウント。署名済みの `Aether.ksm` を使う（`dotnet build ... -c Release`、キーと `maykr.exe` が必要） |
| maykr が `Server response: (4) Build request denied.` | KSL のサーバーが要求を断った。続けて要求したときに起きた。少し待って再実行 |
| `[Aether] Aether needs DirectX 12 ...` | DX11 で起動している。Steam の起動オプションに `-force-d3d12`、または `scripts\launch-game.ps1` |
| ゲームの画面が黒い（Aether のオーバーレイや三角形は出る）、`Player.log` に `Kernel 'KDepthDownsample8DualUav' not found` | DX12 でゲームの HDRP が構築できていない。`kino\mods\Aether\hdrp\aether_hdrp_dx12.bundle` が要る（`scripts\build-hdrp-bundle.ps1` → `scripts\deploy.ps1`）。`get_status` の `managed.hdrpDx12Fix` に状態が出る。[dx12-integration.md](../architecture/dx12-integration.md) |
| `Unity did not call UnityPluginLoad on AetherNative.dll` | Unity のプラグインとして読み込まれなかった。`<Game>_Data\Plugins\x86_64\AetherNative.dll` があるか、`kino\config\Aether\dev-preload` を置いたままにしていないか |
| `AetherNative.dll has API version ...` | `.ksm` と DLL の版が合っていない。両方を同じビルドから配置する（ゲームを閉じて `scripts\deploy.ps1`） |
| MCP のツールが `E_NOT_RUNNING` | ゲームが起動していない、Aether が起動に失敗した（上のログ）、ゲームが別ユーザー（管理者など）で動いている |
| 操作ツールが `E_NOT_APPROVED` | ゲーム内の承認ダイアログに時間内に答えていない。オーバーレイの Settings で `Control requests` を変えられる |
| オーバーレイが出ない | F10（`aether.json` の `overlayHotkey`、仮想キーコード）。KSL のメニュー > Aether のボタンでも開ける。ログに `Could not subclass the game window` があれば入力を取れていない |
| オーバーレイに入力中もゲームが反応する | ウィンドウメッセージ経由の入力は止めているが、ゲームのコントローラー / 生入力（Rewired）までは止めていない（既知の制限） |
| Aether の描画が止まった（オーバーレイに赤い表示、`renderer.state` が `failed`） | `get_crash_report`。HRESULT、デバイス削除理由、取れていれば DRED、ログの末尾が入っている。ゲームは動き続ける。再起動で戻る |
| シェーダーを書き換えたらパスが消えた | コンパイルエラー。オーバーレイの Shaders か `get_shader_errors`。直して `reload_shader` |
| 起動時に「前回のセッションが正常終了していない」と出る | 前回、Aether の終了処理が走らなかった（ゲームのクラッシュ、強制終了）。`get_crash_report` に前回のログの末尾が入る |
| 画面がロード画面のまま、15〜20 fps、`Player.log` に `Kernel 'CalculateCullResult' not found` や `Kernel 'BlurMS' not found` | ゲーム独自の HDRP カスタムパスが DX12 で失敗し、フレームの残りが実行されていない。Aether は該当パスを無効化する（`get_status` の `managed.gamePassesDx12`）。`kino\config\Aether\keep-game-passes` を置いていないか確認 |
| ゲームを閉じてもプロセスが残る | DX12 で HDRP が構築できない状態（毎フレーム例外）のまま数分動かした後に起きた。Aether を読み込んでいない起動でも起きたので Aether 固有ではない。HDRP の DX12 修正が効いている状態では、ウィンドウを閉じて約 17 秒で正常終了する（`docs/development/phase1-report.md` 6 章） |

## 取り除く

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Remove
```

`Aether.ksm`、`kino\mods\Aether\`、`<Game>_Data\Plugins\x86_64\AetherNative.dll` を消します。設定（`kino\config\Aether\`）は残ります。

## ほかの Mod との関係

- Aether が入れるフックは、ゲームのウィンドウプロシージャのサブクラス化だけです（終了時に戻します。後から別の Mod がサブクラス化した場合は素通しとして残ります）。Present、D3D12、DXGI のフックはありません。
- スワップチェーンを包む種類の Mod（`dxgi.dll` のプロキシ）がゲームのフォルダーにある状態で動作を確認しています。
- HDRP の DX12 修正は、HDRP のランタイムリソースのコンピュートシェーダーを差し替えます。同じものを差し替える別の Mod と一緒に使う場合は、`kino\config\Aether\no-hdrp-fix` を置いて Aether 側を切れます。
- `get_status` の `managed.mods` に、KSL が読み込んでいる Mod の一覧が出ます。
