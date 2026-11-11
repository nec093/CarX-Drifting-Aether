# GPU リソース管理と VRAM の内訳

## 現状（Phase 1）：登録簿

Aether が作る GPU リソースはすべて `ResourceRegistry`（`src/Aether.Native/Resources/ResourceRegistry.cpp`）に登録します。

- 記録するもの：名前、カテゴリー、サイズ（デバイスの `GetResourceAllocationInfo`）、寸法、形式、ヒープ（default / upload / readback）、作成フレームと時刻。
- 作成と解放の回数、直近のイベント（どのリソースがいつ作られ、解放されたか）。再生成の繰り返しはここで見つけられます。
- カテゴリー：`texture`、`buffer`、`renderTarget`、`depthBuffer`、`shadowMap`、`reflection`、`history`、`readback`、`upload`、`other`。
- D3D12 のデバッグ名は `Aether.<名前>`。
- Dear ImGui のバックエンドが持つバッファーとフォントアトラスは登録簿の外です（オーバーレイ表示中の数 MB）。

Phase 1 で Aether が持つのは、テスト三角形の頂点バッファー（upload）、タイムスタンプの読み戻しバッファー、キャプチャ中の読み戻しバッファーだけです。

## VRAM の内訳（`get_vram_usage`、オーバーレイの VRAM タブ）

| 区分 | 出どころ | 意味 |
|---|---|---|
| `process` | `IDXGIAdapter3::QueryVideoMemoryInfo` | このプロセス全体の専用 VRAM と共有 GPU メモリの使用量と予算。Unity / HDRP を含む |
| `aether` | 登録簿 | Aether が作ったリソース。カテゴリー別 |
| `unity` | Unity の `ProfilerRecorder` と `Texture` の統計 | Unity が自己申告するレンダーテクスチャー、テクスチャー、バッファーのバイト数と個数、テクスチャーストリーミングの現在値 |
| `notAether` | `process` − `aether` の GPU 分 | Unity / HDRP、ゲームの素材、ほかの Mod、ドライバー |

Unity のレンダーターゲットの個別の一覧（HDRP 側の大きなバッファー）は、`get_resource_list` の `includeUnityRenderTextures` で大きい順に取れます（ゲーム内で全 RenderTexture を走査するので一瞬引っかかります）。

参考（ガレージ、1920x1080、HDRP が DX12 で動いている状態、2026-11-11）：プロセスの専用 VRAM 1,866 MB、Aether 0 MB（システムメモリ側 128 KB）、
Unity のレンダーテクスチャー約 628 MB / 245 個、テクスチャーストリーミング約 572 MB。内訳と大口は `docs/development/phase1-report.md` の 5 章。

## Phase 2 以降（未実装）

登録簿を GPU リソースマネージャーに育てます：プール、再利用、寿命の追跡、メモリ予算、追い出し、エイリアス、メモリ逼迫の検出。
リソースの生成と破棄はここに一元化し、フレームごとの大量確保をしません。レンダーグラフが一時リソースの寿命をここへ渡します。
