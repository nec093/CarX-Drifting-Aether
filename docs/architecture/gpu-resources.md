# GPU リソースと VRAM の内訳

Aether がデバイス上に作るもの、借りるもの、その寿命と計上のしかたをまとめます。
実装は `src/Aether.Native/Resources/GpuResources.{h,cpp}`（GPU リソースマネージャー）と `Resources/ResourceRegistry.{h,cpp}`（登録簿）、内訳の組み立ては `Diagnostics.cpp` の `BuildVramReport`。

## 二つの部品

| 部品 | 役割 | スレッド |
|---|---|---|
| `GpuResources` | ディスクリプターヒープ、Aether のテクスチャー（状態の追跡つき）、フレームのアップロードアリーナ、退役キュー、寿命別の計上と予算 | 描画スレッド。`Report()` だけ任意のスレッド |
| `ResourceRegistry` | Aether が作った GPU リソースの台帳。名前、カテゴリー、サイズ、寸法、形式、ヒープ、作成フレームと時刻、作成と解放の回数、直近のイベント | 任意のスレッド（ロックあり） |

`GpuResources` が作るテクスチャーとアリーナは、作るときに登録簿にも載ります。`Renderer` が自分で作るもの（完了マーカーのバッファー、タイムスタンプの読み戻し、テスト三角形の頂点、キャプチャの読み戻し）と、`SceneRenderer` の既定頂点バッファー、`GpuTimer` の読み戻しも登録簿に載ります。
登録簿はリソースに `Aether.<名前>` という D3D12 のデバッグ名を付けます。1 つの ID を 2 回解放すると `E_LIFETIME` を記録します。

カテゴリー：`texture`、`buffer`、`renderTarget`、`depthBuffer`、`shadowMap`、`reflection`、`history`、`readback`、`upload`、`other`。

## ディスクリプターヒープ

| ヒープ | 大きさ | 用途 |
|---|---|---|
| CBV/SRV/UAV（シェーダー可視） | 16,384 | シーンのパスが使う唯一のヒープ。テクスチャーの SRV、マテリアルのテーブル、フレームごとのテーブル |
| CBV/SRV/UAV（CPU 専用のステージング） | 16,384 | シェーダー可視ヒープと同じ並び |
| RTV（CPU 専用） | 512 | Aether のレンダーターゲット |
| DSV（CPU 専用） | 128 | Aether の深度ターゲット |

- `WriteSrv` はビューをステージングとシェーダー可視の両方に書きます。`CopySrv` はステージング側から読みます（シェーダー可視ヒープは CPU から読めないため）。マテリアルのテーブルとフレームごとのテーブルはこのコピーで作ります。
- SRV は範囲で割り当てます（`AllocSrv(count)`、先頭から探す空きリスト）。空きリストが 64 個を超えたら隣り合う範囲をまとめます。
- `FreeSrv(index, count, fence)` は範囲をすぐには空けず、完了番号がその値を過ぎてから空きに戻します。提出済みのリストがまだ読んでいる可能性があるためです。
- RTV と DSV は 1 個ずつの空きリストです。テクスチャーが退役したら空きに戻ります。
- どれかが満杯になると `E_DESCRIPTOR_HEAP_FULL` を記録します。そのテクスチャーは作られても、ビューの無いものは描画に使えません（レンダーグラフはそのフレームを実行しない。マテリアルは既定テクスチャーのまま）。

ほかに、`Renderer` がバックバッファー用に RTV を 1 個だけのヒープを持ち、オーバーレイ（Dear ImGui のバックエンド）が自分の SRV ヒープを持ちます。

## Aether のテクスチャー

`CreateTexture(desc, name, category, lifetime, frame)` が committed resource（default ヒープ）を作り、必要なビュー（SRV、RTV か DSV）を割り当てます。

- 形式は 3 つ持てます：リソース、SRV、RTV / DSV（深度は typeless のリソースに型付きのビュー）。
- 作るときの状態は、最初の使い方に合わせます（深度は `DEPTH_WRITE`、レンダーターゲットは `RENDER_TARGET`、ほかは `COPY_DEST`）。
- `GpuTexture::state` は「次に記録するコマンドから見た状態」です。`Transition` は状態が違うときだけバリアを記録します。レンダーグラフはパスの宣言からこれを呼びます。
- サイズはデバイスの `GetResourceAllocationInfo` の値です。

### 寿命の区分

| 区分 | 何か | 例 |
|---|---|---|
| `Persistent` | フレームをまたいで残る | シャドウアトラス、空の LUT、反射プローブのアトラス、GI プローブ、TAA の履歴、露出、既定テクスチャー |
| `Transient` | レンダーグラフのプールのテクスチャー（[renderer.md](renderer.md) の「レンダーグラフ」） | `SceneColor`、`SceneDepth`、ブルームの各段 |
| `Frame` | 1 フレームのアップロードメモリ | 下のアリーナ |

## フレームのアップロードアリーナ

- 1 つ 8 MB の upload ヒープのバッファー（マップしたまま）。提出中のフレームごとに 1 つ使い、最大 8 個まで作ります。
- 中身：フレーム定数、インスタンス、マテリアル、ライト、クラスターグリッド、ライトの番号、インスタンスマップ、パーティクル、光線用のマテリアル表、BVH のインスタンスの木、既定テクスチャーのアップロード元。
- `BeginFrame(completed)` で完了したアリーナを空きに戻して 1 つ取り、`EndFrame(fence)` でそのフレームのリストの番号まで使用中にします。空きが無く 8 個に達していれば、そのフレームのシーンは描きません。
- 1 フレームで足りなければ `E_FRAME_ARENA_FULL` を記録し、そのアップロードは失敗します。

## 退役（retire）

提出済みのリストが参照しうるものは、そのリストの完了を確認してから解放します。時計は Aether 自身の完了番号です（[dx12-integration.md](dx12-integration.md) の「完了の判定」）。

| 入口 | 対象 |
|---|---|
| `RetireTexture(texture, fence, frame)` | Aether のテクスチャー。登録簿からはすぐ外し、解放と SRV / RTV / DSV の返却は完了後 |
| `Retire(object, fence)` | そのほかの COM オブジェクト。パイプラインステート、BVH のバッファー、Unity から借りたメッシュバッファーとテクスチャーへの参照 |
| `FreeSrv(index, count, fence)` | ディスクリプターの範囲（マテリアルのテーブル、フレームごとのテーブル、Unity のテクスチャーの SRV） |

- `Collect(completed, frame)` を描画スレッドがシーンのフレームの初めに呼び、完了したものを解放します。キューは提出順なので、先頭から完了番号以下のものを取り出すだけです。
- `Retire` が提出順に反する番号で呼ばれたら `E_LIFETIME` を記録します。
- テクスチャーの解放のとき、COM の参照数と所有者の数がどちらも 1 であることを確かめます。違えば `E_LIFETIME`（余計な AddRef か Release がある）。
- 終了時は `Shutdown(leak)`。GPU の終わりを確認できなかったとき（`leak = true`）は、解放せずに参照を手放します。

## 所有と共有

- **Aether 所有**：上のテクスチャー、アリーナ、Aether が作ったバッファー。`aether` と `aetherManager` に計上します。
- **共有（Unity から借用）**：シーンのメッシュバッファーとテクスチャーはコピーせず、Unity の `ID3D12Resource` を参照します（`SceneStore`、Scene Data Contract）。Aether は自分で取った COM の参照だけを持ち、それも退役キューを通して手放します。Unity のメモリなので `unity` / `notAether` 側に入っており、Aether の使用量には足しません（`shared.meshBytes` / `shared.textureBytes`）。
- つまりシーンのジオメトリとテクスチャーについて、Aether は VRAM を増やしません。増えるのは自前のレンダーターゲット、Persistent のテクスチャー、BVH、フレームメモリ（システムメモリ側の upload ヒープ）です。

### テクスチャーストリーミング

Aether は自前のストリーミングを持ちません。Unity のテクスチャーストリーミングが読み込んだミップをそのまま使います。
C# 側が `updateCount`、サイズ、`loadedMipmapLevel` の変化でテクスチャーの差し替えを見つけて記述を送り直すと、ネイティブ側は古い参照と SRV を退役させ、新しい `ID3D12Resource` の SRV を作ります。

## 予算

設定 `vramBudgetMB`（Aether が所有する GPU メモリの予算、0 = なし）と `vramWarnPercent`（警告のしきい値）。
状態は `aetherManager.budget.state`（`ok` / `warning` / `over budget` / `no budget set`）で報告するだけで、自動では何も解放しません。

## VRAM の内訳（`get_vram_usage`、オーバーレイの VRAM タブ）

| 区分 | 出どころ | 意味 |
|---|---|---|
| `process` | `IDXGIAdapter3::QueryVideoMemoryInfo` | このプロセス全体の専用 VRAM と共有 GPU メモリの使用量と予算。Unity とゲームを含む |
| `adapterDedicatedBytes` | アダプターの情報 | GPU の専用 VRAM の容量 |
| `aether` | 登録簿 | Aether が作ったリソース。GPU 側（default ヒープ）とシステムメモリ側（upload / readback）、カテゴリー別、作成と解放の回数 |
| `aetherManager` | `GpuResources::Report()` | Persistent と Transient のバイト数、アリーナ（個数、確保量、直近の使用量、最大）、ディスクリプターの使用数と最大と容量、作成・退役の回数、退役待ちの数、予算 |
| `shared` | `SceneStore` | Aether が参照している Unity のメッシュとテクスチャーのバイト数（Unity のメモリ） |
| `unity` | Unity の `ProfilerRecorder` と `Texture` の統計 | Unity が自己申告するレンダーテクスチャー、テクスチャー、バッファーのバイト数と個数、テクスチャーストリーミングの値、ドライバーのメモリ（このビルドのゲームが出す値だけ） |
| `notAether` | `process` の専用 VRAM − `aether` の GPU 分 | Unity、ゲームの素材、ほかの Mod、ドライバー |

- `get_resource_list`：登録簿の全リソースを大きい順に（`category` で絞れる）。`includeUnityRenderTextures` を付けると、Unity のレンダーテクスチャーの一覧も C# 側から取ります（ゲーム内で全 RenderTexture を走査するので一瞬引っかかる）。
- BVH の GPU バッファーの大きさは `get_scene_info` の `rayTracing.gpuBytes`。

## 制限

- メモリが足りないときの追い出し（eviction）や品質の自動低下はありません。予算は報告だけです。
- ヒープのサブアロケーションはしません。すべて committed resource です。
- ディスクリプターヒープは大きくなりません。満杯なら、そのテクスチャーやマテリアルは既定のまま描かれます。
- 自前のテクスチャーストリーミングはありません。
- 登録簿に載らないもの：BVH の GPU バッファー（`Bvh.cpp` が直接作る）、Dear ImGui のバックエンドのバッファーとフォントアトラスと SRV ヒープ、`Renderer` と `GpuResources` のディスクリプターヒープ、タイムスタンプのクエリヒープ。どれも `notAether` 側に入ります。
