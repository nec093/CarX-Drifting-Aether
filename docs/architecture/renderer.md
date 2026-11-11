# レンダラー

## 現状（Phase 1）

`Renderer`（`src/Aether.Native/Renderer/Renderer.cpp`）は、ホストのフレームの最後に、固定順のパスをバックバッファーへ描きます。

| パス | 内容 | スイッチ |
|---|---|---|
| FrameBeginMarker | フレーム先頭の GPU タイムスタンプ | 設定 `gpuTiming` |
| （HostFrame） | ゲームのフレーム（HDRP、UI）。Aether の外 | |
| TestTriangle | DX12 統合の確認用の三角形（右上） | 設定 `testTriangle` |
| Overlay | Dear ImGui | F10、承認ダイアログが出ているとき |
| FrameCapture | バックバッファーを読み戻して PNG に保存 | 要求があったフレームだけ |
| （HostPresent） | Unity が PRESENT へ遷移して Present | |

- 何も描くものが無く GPU 計測も切ってあるフレームでは、コマンドリストを作りません。オーバーレイが非表示のあいだ ImGui のフレームは作られません。
- GPU 時間はタイムスタンプクエリ（フレーム全体、Aether の合計、パスごと）。結果は数フレーム遅れて回収します。
- シェーダーは `shaders/*.hlsl` を実行時に `D3DCompile`（シェーダーモデル 5.1）でコンパイルします。コンパイルエラーはデータとして保持され（オーバーレイの Shaders、`get_shader_errors`）、そのパスが描かれなくなるだけでレンダラーは止まりません。`reload_shader` で再起動なしに読み直せます。
- `get_render_graph` は現在この固定の並びを返します（パス、読み書きするリソース、直近の時間）。

## Phase 2 の方針（未実装）

- **Forward+ / Clustered Forward** を第一候補にします（透明・車のマテリアルとの相性、メモリ、柔軟性）。Deferred への切り替えは実測してから判断します。
- **シーンキャプチャ**：Unity からメッシュ、トランスフォーム、マテリアル、テクスチャ、カメラ、ライト、プローブを差分で受け取ります。毎フレーム全コピーはしません。Phase 1 の `SceneReporter`（読み取り専用の調査）がその入口です。
- **レンダーグラフ**：パスの登録、リソースの依存と寿命、一時リソースの再利用とエイリアス、バリアと状態遷移。Phase 1 の固定の並びを置き換えます。
- **シェーダー**：SM 6 が要る機能が出たら DXC に移ります（GPU の対応は `get_gpu_info` の `shaderModel` で実行時に判定）。
- **デバッグビュー**（Albedo、Normal、Roughness、Metallic、Depth、Motion Vector、Shadow Map、Lighting、Reflection）は、Aether 自身の G-buffer / ライティングのパスができてから有効になります。オーバーレイの Pipeline タブに置き場所があります。
- HDRP とは当面並行で動かし、同じシーンで絵・VRAM・時間を比較してから置き換えます。二重描画を常態にはしません。
