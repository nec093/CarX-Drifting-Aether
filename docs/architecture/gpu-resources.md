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

## Phase 2：GPU リソースマネージャー（`src/Aether.Native/Resources/GpuResources.{h,cpp}`）

Aether がデバイス上に確保するものはすべてここを通ります（登録簿はその台帳として残っています）。描画スレッド専用、`Report()` だけ任意スレッド。

| 機能 | 内容 |
|---|---|
| ディスクリプター | シェーダー可視の CBV/SRV/UAV ヒープ 1 本（16,384 個）と、同じ並びの CPU 専用ステージングヒープ。`WriteSrv` は両方に書き、`CopySrv` はステージング側から読む（シェーダー可視ヒープは CPU から読めないため）。範囲アロケーター、RTV / DSV ヒープ |
| Aether のテクスチャー | `CreateTexture`（レンダーターゲット、深度、シャドウアトラス、既定テクスチャー）。状態を追跡し、`Transition` は必要なときだけ記録 |
| フレームメモリ | アップロードアリーナ（定数、インスタンス、マテリアル、ライト、クラスター）。提出中のフレームごとに 1 つ、完了したら再利用 |
| 退役（retire） | 提出済みのリストが参照しうるものは、そのリストの完了を確認してから解放（`Retire` / `Collect`）。Unity から借りているバッファーやテクスチャーへの参照、ディスクリプターの範囲も同じ |
| 寿命の区分 | `Persistent`（シャドウアトラス等）、`Transient`（レンダーグラフのプール）、`Frame`（アリーナ） |
| 予算 | `vramBudgetMB`（既定 2048）と `vramWarnPercent`（既定 80）。状態は `aetherManager.budget.state`（`ok` / `warning` / `over budget`）で報告するだけで、勝手には解放しない |

### 所有と共有

- **Aether 所有**：`aether` / `aetherManager` に計上。
- **共有（Unity から借用）**：シーンのメッシュバッファーとテクスチャーはコピーせず、Unity の `ID3D12Resource` を参照します（`shared.meshBytes` / `shared.textureBytes`）。
  Unity のメモリなので `unity` / `notAether` 側に既に入っており、Aether の使用量には足しません（二重計上しない）。
- つまり Phase 2 の Aether は、シーンのジオメトリとテクスチャーについて VRAM を増やしません。増えるのは自前のレンダーターゲット、シャドウアトラス、フレームメモリだけです。

### テクスチャーストリーミング

Aether は自前のストリーミングを持ちません。Unity のテクスチャーストリーミングが読み込むミップをそのまま使い、
ミップの増減で `ID3D12Resource` が替わったら（`loadedMipmapLevel` / `updateCount` の変化で検出）ディスクリプターを書き直すだけです。
HDRP を止めた後に自前の読み込みへ移るための受け口は、Scene Data Contract のテクスチャーハンドル（中身を差し替えられる）です。

### 診断

`get_vram_usage` に `aetherManager`（ディスクリプターの使用数と最大、アリーナ、退役待ち、寿命別バイト数、予算）と `shared` を追加。
`get_resource_list` は従来どおり登録簿の全リソース。オーバーレイの VRAM タブにも同じ内容があります。

### まだ無いもの

追い出し（eviction）、メモリ逼迫時の品質低下、ヒープのサブアロケーション（すべて committed resource）、
自前のテクスチャーストリーミング、ディスクリプターヒープの拡張（満杯になったらそのマテリアルは既定テクスチャーのまま）。