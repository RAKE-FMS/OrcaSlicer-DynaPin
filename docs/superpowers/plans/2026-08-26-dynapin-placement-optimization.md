# DynaPin Placement Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 選択した1インスタンスの相対Z回転とΔyを探索し、DynaPin適用後のNormalサポート幾何体積が有意に減る場合だけ適用する。

**Architecture:** 既存のDynaPin自動ピン選択を再利用する。探索の純粋関数を新しいDynaPinPlacementモジュールに置き、一時Model/Printによる評価、GUI Jobによる適用を分離する。X方向は独立探索せず、現在のワールド中心を維持する変換補正のみ許可する。

**Tech Stack:** C++17、Eigen、libslic3r polygon演算、Catch2、wxWidgets Job/Plater。

---

## 2026-09-08 現行コードとの照合

基準コミット: `f28ded5d8d`。これは実装計画の更新であり、今回ビルド・テストを実行した記録ではない。旧チェックリストの完了表示を実装の証拠として使用しない。

| 項目 | 現在の確認結果 | 担当タスク |
|---|---|---|
| ピン先端設定 | `Config`とKP3S JSONに`tip_collision`なし。`pin_z()`もなし | S01 |
| 幾何体積評価 | `PrintObjectSupportMaterial::generate()`が`generate_support_toolpaths()`まで実行。専用評価APIなし | S02–S04 |
| 配置候補探索 | DynaPin.hpp/cppに配置探索APIなし | S05–S09 |
| GUI適用 | `DynaPinPlacementJob`なし。Platerへの配置最適化接続なし | S10–S12 |
| 基盤 | Auto選択、全インスタンスの引き出し経路干渉判定、コピー分離、Z回転invalidateの実装・テストあり | 維持しS00/S13で検証 |
| 上段投影遮断 | コンタクトごとのProjectionStateあり。従来レビューのテスト強化・性能問題は未検証 | R01–R03 |
| 旧BBox指摘 | 現行検出器はcontactごとの`get_extents(polygons)`。旧フラグ修正をそのまま再実装しない | S00で確認のみ |

現在の軸は **row=Z、col=Y**。Yピッチは`Config::col_pitch_y`、ピン上面は`blocker_z_range(config,pin).z_max`。`support_origin_z`をピン中心と読み替えない。引き出し座標`pull_origin_*`と支持座標`support_origin_*`も混用しない。

## Solへの渡し方と実装順

1回に下記IDを1つだけ渡す。目安は1タスク1責務、1レビュー可能差分。巨大なサポート生成・GUI処理の同時変更を避ける。下記は仕様とAPI契約を固定する作業分割であり、既存コードに対する完成パッチではない。

```text
このプランの Sxx のみ実装してください。
開始時に依存タスクの成果物と現在の差分を確認してください。
対象ファイル、契約、受け入れ条件を守り、対象テストを実行してください。
既存の未コミット変更を取り消さないでください。
終了時に変更ファイル、検証コマンドと結果、未解決事項を報告し、
検証できたチェックだけ更新してください。次のタスクへは進まないでください。
```

推奨順: `S00 → R01 → R02 → R03 → S01 → S02 → S03 → S04 → S05 → S06 → S07 → S08 → S09 → S10 → S11 → S12 → S13`。
R01–R03で再現しない問題は、比較条件・結果を残して修正不要とする。スライス再利用と永続キャッシュは初版の完了条件から外す。

## 共通の制約・検証

- blockerは常に`[z_max - blocker_height_z, z_max]`。`z_min=0`へ拡張しない。
- 上段由来の投影は`surface.print_z`で`surface.poly`を差し引いて終了。ピン下で再開しない。独立した下段由来の投影は残す。
- 対象Print全体を評価する。全PrintObjectの領域を同じプレート座標に揃えた後にunionし、重なりを二重計上しない。
- 成功適用以外ではライブModel/Print、選択ピン、Undo履歴を変更しない。
- 入力変更後の古い結果は適用しない。対象は配列indexだけで保持せずIDと入力世代で照合する。
- 角度単位: 探索は度、Modelの回転APIはラジアン。変換境界を1か所にする。

既存のarm64構成で使うコマンド:

```sh
cmake --build build/arm64 --target libslic3r_tests fff_print_tests --parallel
ctest --test-dir build/arm64 -N -R 'DynaPin|SupportMaterial'
ctest --test-dir build/arm64 --output-on-failure -R 'DynaPin|SupportMaterial'
cmake --build build/arm64 --target OrcaSlicer --config Release --parallel
```

テスト一覧が0件なら合格ではない。S00で実行ファイルとCTest登録名を確定する。新テストには名前に`DynaPin placement`、タグに`[DynaPinPlacement]`を付ける。小タスクでは追加ケースのみ実行し、S13で既存回帰をまとめて実行する。コミットする場合は変更したC++ファイルをclang-formatし、担当ファイルだけをstageする。

## タスク一覧

### S00: 基準状態を記録する

**依存:** なし。**変更:** 本プランのみ。

- [ ] `git status --short`と上記CTest一覧を記録する。作業中の別機能の変更を区別する。
- [ ] DynaPin/SupportMaterial既存テストを実行し、失敗名・構成・所要時間を記録する。
- [ ] `detect_dynapin_pins()`のBBox集計を再読し、古いレビュー指摘の該当有無を記録する。

**完了条件:** 後続が比較できるコマンド・結果を本書末尾へ追記。既存失敗を新規実装の失敗と混同しない。

### R01: 自動検出のsharp_tails副作用を閉じる

**依存:** S00。**変更:** `src/libslic3r/Support/SupportMaterial.cpp`、必要なら同hpp。**テスト:** `tests/fff_print/test_support_material.cpp`。

- [ ] 同じスライスで検出前・検出後・再検出後の各Layerの`sharp_tails`と`sharp_tails_height`を比較する回帰ケースを追加する。
- [ ] 検出が変更を残すなら、検出のスコープで両配列を退避・復元するRAIIを実装する。早期returnと例外も復元対象。
- [ ] 検出2回の選択結果一致と、検出後の通常サポート幾何一致を確認する。

**完了条件:** 配列数だけでなくポリゴンと高さも一致。通常生成のsharp-tail機能を全体で無効化しない。

### R02: blockerに実際に重なるfixtureを確立する

**依存:** R01。**変更/テスト:** `tests/fff_print/test_support_material.cpp`、`tests/fff_print/test_print.cpp`。

- [ ] 既存の積層ピンケースを基に、DynaPin無効時の支持領域とblocker XYの交差面積が正になる前提をassertする。
- [ ] 有効時にblockerスラブ内は交差面積0、ピン上面直上には支持あり、独立下段には支持ありを同時にassertする。
- [ ] Auto選択後にも選択ピン数だけでなく同じ幾何条件を検証する。

**完了条件:** blockerをfixtureから遠ざけると前提assertが失敗する。浮遊部だけでなく下段支持を明示的に検証する。

### R03: 通常支持経路とdebug stageを個別に検証する

**依存:** R02。このIDは次の2差分に分け、Solには一方ずつ渡す。

**R03a変更:** `src/libslic3r/Support/SupportMaterial.cpp`。**テスト:** `tests/fff_print/test_support_material.cpp`。

- [ ] DynaPin無効時の集約投影と現行per-contact投影の形状・時間を同一fixtureで比較する。
- [ ] 差分があれば無効時を集約投影に戻す分岐を追加し、有効時のピン終了処理は維持する。
- [ ] 対称差面積と3回の経過時間を記録する。性能の数値閾値は実測前に捏造しない。

**R03b変更:** `src/libslic3r/DynaPin.hpp/.cpp`、`src/libslic3r/Print.cpp`、`src/libslic3r/Support/SupportMaterial.cpp`、必要なG-code呼び出し元。**テスト:** `tests/fff_print/test_print.cpp`。

- [ ] `dynapin_debug_stage`と環境変数の既存解釈を共通関数へ集約する。
- [ ] stage 0で選択・blocker・pull出力が発生しないよう同じ有効条件を使用する。
- [ ] stage 0/1/2と環境変数上書きを検証し、環境変数はケース終了時に復元する。実G-code再処理はCLI検証と分離する。

**完了条件:** 無効時の通常支持とstageの一貫性を別々に報告できる。

### S01: 先端干渉設定を現在の軸定義へ合わせる

**依存:** S00。**変更:** `src/libslic3r/DynaPin.hpp/.cpp`、`resources/profiles/Kingroon/dynapin/kp3s.json`。**テスト:** `tests/libslic3r/test_dynapin_preview.cpp`。

- [ ] `TipCollisionConfig`に`x_min,x_max,width_y,thickness_z,clearance`を追加。設定未指定を明示的に表現し、通常スライスは従来動作、配置最適化だけは設定不足として終了する。
- [ ] KP3S値を`0,20,12.4,5,0`として追加（旧設計の余白1mmは採用しない）。全値finite、`x_min<x_max`、幅・厚さ>0、余白>=0を検証する。
- [ ] `tip_collision_box_for_pin()`を追加し、blocker形状やpull座標と別の用途として保持する。
- [ ] row増加でZのみ、col増加でYのみ変わるケース、負余白、NaN、逆転X、設定欠落を検証する。

**受け入れ例:** 上面7.3、厚さ5ならZ範囲`[2.3,7.3]`。現行実装どおり追加余白なし。Y中心は`pin_y(config,pin) - 7.2`、幅12.4。旧設計の中心±厚さ/2を上面へ直接当てない。`clearance`は初版0固定とし、全方向への拡張機能を追加しない。境界のEPSILONは数値誤差用であり物理余白ではない。

### S02: Zスラブ体積積算を純粋関数にする

**依存:** S00。**新規:** `src/libslic3r/DynaPinPlacement.hpp/.cpp`、`tests/libslic3r/test_dynapin_placement.cpp`。**登録:** `src/libslic3r/CMakeLists.txt`、`tests/libslic3r/CMakeLists.txt`。

公開契約（新規namespace `Slic3r::DynaPin`）:

```cpp
struct SupportSlab {
    double z_min = 0.;
    double z_max = 0.;
    Polygons polygons; // same plate coordinates; scaled XY
};
double support_volume_mm3(const std::vector<SupportSlab>& slabs);
```

- [ ] 10×10mm、Z=[0,2]の単体=200mm³を検証する。
- [ ] 同一XY、Z=[0,2]と[1,3]のunion=300mm³、穴の面積控除、空入力=0を検証する。
- [ ] 全Z端点をsort/uniqueし、各開区間の中央を含むslabのpolygonをunion、面積に`SCALING_FACTOR²`と高さを掛ける。
- [ ] 非finite・逆転Zは呼び出しエラーとして拒否し、ゼロ高さは無視する。

**完了条件:** 入力順序・重複に依存しない。型にraftを含めず、S03で収集対象を制御する。

### S03: Normal支持生成から幾何段階を取り出す

**依存:** S02、R01。**変更:** `src/libslic3r/Support/SupportMaterial.hpp/.cpp`。**テスト:** `tests/fff_print/test_support_material.cpp`。

- [ ] 既存`generate()`の領域生成を共通内部処理にまとめ、通常モードとgeometry-onlyモードを分岐する。
- [ ] top/bottom contacts、intermediate、interface、base interfaceのpolygonsと実際のZ範囲を所有値の`SupportSlab`にコピーする。ローカルstorageへのポインタを返さない。
- [ ] raftを除き、geometry-onlyは`generate_support_toolpaths()`前にreturnする。通常モードの呼び出し順を維持する。
- [ ] full生成とgeometry-onlyの体積一致、raftのみの評価0、geometry-onlyの支持押出経路が空であることを検証する。

**完了条件:** 体積測定のためにG-codeや支持toolpathを生成しない。スラブ下端を一律にオブジェクトlayer heightから推測しない。

### S04: 一時Printで1候補を評価する

**依存:** S03。**変更:** `src/libslic3r/Print.hpp/.cpp`、`src/libslic3r/PrintObject.cpp`、`src/libslic3r/DynaPinPlacement.hpp/.cpp`。**テスト:** `tests/fff_print/test_print.cpp`。

- [ ] 全シーンのModelと有効configをコピーし、slice→Auto選択→Normal支持幾何の必要段階だけを実行する入口を追加する。
- [ ] 各PrintObjectのスラブを共通プレート座標へ移し、全体でS02を1回呼ぶ。コピーグループを二重加算しない。
- [ ] 評価結果を`volume_mm3`と選択ピンの所有値で返す。キャンセルをsliceと支持生成へ伝播する。
- [ ] 同一候補の連続評価一致、ライブ変換/config/ピン不変、2インスタンスの配置、支持toolpath未生成を検証する。

**完了条件:** `Print::process()`による全工程実行で代用しない。候補ごとにAuto選択を再計算する。

### S05: 回転中心維持と選択インスタンス変換

**依存:** S02。**変更:** `src/libslic3r/DynaPinPlacement.hpp/.cpp`。**テスト:** `tests/libslic3r/test_dynapin_placement.cpp`。

- [ ] 初期インスタンスのワールドbounding-box中心Cを固定し、元変換Mに次式を適用する純粋処理を追加する。

```text
M_candidate = T(0, delta_y, 0) * T(C) * Rz(theta) * T(-C) * M_initial
```

- [ ] 元姿勢から毎回計算し、前候補からの累積回転をしない。
- [ ] 非原点、既存XYZ回転、非一様scale、mirrorのケースで中心の移動が`(0,delta_y,0)`となることを検証する。
- [ ] 同一objectの別instanceを含め、選択外の変換が不変であることを検証する。

**完了条件:** X offset補正を独立Δx探索と混同しない。object全体の`ensure_on_bed()`で他instanceを動かさない。

### S06: Δy区間と粗候補を生成する

**依存:** S05。**変更/テスト:** S05と同じ。

- [ ] 角度0..355°を5°刻み、pitch=`abs(col_pitch_y)`、Δyをpitch/16刻みで列挙する。
- [ ] 実行可能区間[L,U]内で長さmin(pitch,U-L)を確保し、中心0の区間を左右へ必要な分だけ平行移動する。
- [ ] pitch=16の例: [-100,100]→[-8,8]、[-3,100]→[-3,13]、[-100,2]→[-14,2]、[-3,2]→[-3,2]を検証する。
- [ ] 区間端点と現在姿勢(0,0)を別途含め、重複を除く。非矩形bedでは連続区間を安易に仮定せず候補ごとにS07で判定する。

**完了条件:** X逸脱をY区間計算で補正しない。pitchが0/非finiteなら探索せずInvalidConfig。

### S07: bed・固定モデル・ピン干渉で候補を除外する

**依存:** S01、S04–S06。**変更:** `src/libslic3r/DynaPinPlacement.hpp/.cpp`、必要なら`src/libslic3r/DynaPin.cpp`。**テスト:** `tests/fff_print/test_print.cpp`。

- [ ] 候補のplate座標でbed外周・除外領域・高さ上限を検証する。XY AABBだけで非矩形bedを判定しない。
- [ ] 固定モデルとの3D重なりを検証する。同じXYでもZが離れる例を過剰除外しない。
- [ ] 先端boxと候補/固定モデルの干渉を検証する。box接触境界は干渉扱いとし、スライス面の間にだけ存在する薄い干渉も取り逃さない。
- [ ] 引き出し経路の既存判定とAuto選択を維持する。あるピンが使用不能という理由だけで姿勢全体を無効にせず、最終選択ピンの安全性を確認する。
- [ ] 現在姿勢の先端干渉はInitialTipCollision、探索中は当該候補のInvalidPoseとして分離する。

**完了条件:** 初期干渉で評価ループに入らない。判定対象の物理ピン集合はS01の設定と設計に従い明記する。

### S08: 比較ルールを固定する

**依存:** S02。**変更/テスト:** `DynaPinPlacement.hpp/.cpp`と`test_dynapin_placement.cpp`。

- [ ] 現在体積V0からepsilon=`max(1.0,V0*0.001)`を1回計算する。
- [ ] 候補ランキングは体積の厳密な順序で最小値を求め、その最小値+epsilon以内の群を|Δy|、最短回転量の順で選ぶ。
- [ ] epsilon付き比較を`std::sort`へ直接渡さない（推移律を壊すため）。最後の完全同値には正規化角度、符号付きΔyで順序を固定する。
- [ ] V0=1000で999は不適用、998.9は改善、359°の回転距離=1°、入力順序反転で結果一致を検証する。

**完了条件:** 勝者自体がV0よりepsilonを超えて改善しない場合はUnchanged。

### S09: 粗探索・局所探索・キャンセルを結合する

**依存:** S04、S06–S08。**変更/テスト:** `DynaPinPlacement.hpp/.cpp`と`test_dynapin_placement.cpp`。

- [ ] evaluatorとcancel/progress callbackを注入できる探索関数を追加し、実Printを使わない決定的テストを用意する。
- [ ] 粗探索の上位5候補をseedとし、角度±4°を1°刻み、Δy±pitch/16をpitch/64刻みで再探索する。これは旧文書で未定だった局所探索幅の初版既定値。
- [ ] 角度は[0,360)へ正規化し、角度とscaled Δyのキーで粗密両段階の重複評価を防ぐ。候補は毎回制約判定する。
- [ ] 全無効、全同値、最適点が359°付近、途中cancel、評価例外をテストする。

**完了条件:** 状態をImproved/Unchanged/Canceled/InvalidConfig/InitialTipCollision/NoFeasiblePoseとして返す。進捗は単調増加し、Canceledでは勝者を適用しない。

### S10: GUI起動条件とスナップショットを用意する

**依存:** S09。**新規:** `src/slic3r/GUI/Jobs/DynaPinPlacementJob.hpp/.cpp`。**変更:** `src/slic3r/CMakeLists.txt`、`src/slic3r/GUI/Plater.cpp`。

- [ ] 既存Jobの`process(Ctl&)`/`finalize()`に合わせ、UIスレッドでModel/config/対象ID/入力世代を取得する。
- [ ] DynaPin有効、Auto選択、Normal支持、1インスタンス選択だけを対象とする。Manual、Tree/Organic、複数選択は既存スライスへ進める。
- [ ] 判定処理はデータだけで検証可能にし、全条件のtrue/false表をケース化する。

**完了条件:** worker起動前にsnapshotが完成し、workerはPlaterや選択UIを読み書きしない。まだ自動適用は接続しない。

### S11: Jobで探索を実行する

**依存:** S10。**変更:** `src/slic3r/GUI/Jobs/DynaPinPlacementJob.hpp/.cpp`。

- [ ] S09を`process()`から呼び、`Ctl::was_canceled()`と`update_status()`へ接続する。
- [ ] 結果はJobが所有する。例外は既存Jobのfinalize経路に渡す。
- [ ] 大きめのfixtureで進捗表示中のキャンセルを手動検証し、ライブ変換とUndo数が変わらないことを記録する。

**完了条件:** UIが操作可能で、キャンセルが実Print評価にも伝わる。GUI統合の結果をユニットテスト実施済みと記さない。

### S12: 勝者の適用・古い結果破棄・再入防止

**依存:** S11。**変更:** `src/slic3r/GUI/Jobs/DynaPinPlacementJob.cpp`、`src/slic3r/GUI/Plater.cpp`。

- [ ] finalizeで対象IDと入力世代、Model/config/plateの一致を確認。不一致なら結果破棄。
- [ ] ImprovedだけUndoスナップショットを1回作り、選択instanceのS05変換だけを適用する。Z移動を伴わないのでobject全体のbed再配置を追加しない。
- [ ] 最適化起因の再スライスに限定した一回限りのガードを設定し、消費・失敗・cancelの各経路で解除する。
- [ ] 初期干渉は警告、Unchangedは姿勢維持して通常スライス継続。cancelは自動再起動しない。
- [ ] Undo/Redo、探索中の削除・移動・設定変更、2回目の手動slice、最適化起因resliceを手動検証する。

**完了条件:** 自動適用時のJob起動1回、Undo1回。次の独立sliceはガードに阻害されない。

### S13: 実モデルで統合検証する

**依存:** R01–R03、S01–S12。**変更:** 本プランの検証記録のみ。必要な回帰追加は対応タスクへ戻す。

- [ ] 共通のlibslic3r/FFF回帰を実行し、GUI含むReleaseビルドを確認する。
- [ ] `models/manual-tests/gymnast.3mf`を複製して、現在姿勢と最適化後を同一profileで評価する。
- [ ] V0/Vbest(mm³)、Δy、相対角度、選択ピン(row,col)、候補数、無効候補数、評価時間、全時間を記録する。
- [ ] 通常スライスしたG-codeの選択ピンとpullコメント、上段支持終了、下段支持保持を確認する。実機動作は別途実施の有無を明記する。
- [ ] 改善なし、初期干渉、cancelの各経路でも姿勢とUndoが不変であることを確認する。

**完了条件:** 体積差が閾値を超える場合だけ適用。改善率や実行時間を未測定のまま保証しない。

## 初版から外す作業

- 回転ごとのスライス再利用: S13でボトルネックを測定してから別計画にする。Δyでピン選択・固定障害物・支持は変わるため最終スコアを流用しない。
- 永続入力署名キャッシュ: S12の古い結果防止は必要だが、繰り返し探索を省くキャッシュは不要。
- X移動探索、Manualピン最適化、Tree/Organic、複数インスタンス同時最適化は対象外。

## 確認事項と検証記録

2026-09-08ユーザー回答: 「上面から5mmで、余白は実装に従ってほしい」。`DynaPin.cpp`の`blocker_z_range()`と`pin_collides_with_model()`を照合し、Zは`[top-5,top]`、追加余白なしと確定した。Y中心は既存`support_block_y_offset=-7.2`を含む物理領域に合わせる。旧設計の全方向1mm余白は廃止する。

### 2026-09-08 実装状況

S01〜S12を一括実装した。先端boxはピン上面から下向き5mm、追加余白0mmとし、Auto選択結果に限らず全物理ピンを候補姿勢で検査する。配置評価はライブModelを変更しない一時PrintでNormal支持のgeometry-onlyスラブを生成し、ZスラブごとのXY union体積を目的関数にする。

探索は5°・pitch/16の粗探索と、上位5候補に対する1°・pitch/64の局所探索を実装した。各回転角で変換後bounding boxからbed内の実行可能Y区間を再計算し、区間端点と現在姿勢を含める。比較、重複排除、キャンセル、初期先端干渉、無効候補の扱いも決定的テストで固定した。

GUIは`Plater::reslice()`で最新Model/configをbackground Printへ反映した後に対象判定し、Jobで探索する。改善時だけUndo 1件と選択instanceの変換を適用し、一回限りのガード付きで再スライスする。初期干渉は警告し、改善なし・cancel・古い結果では姿勢を変更しない。

R01のsharp-tail状態復元、R03aのDynaPin無効時の集約投影経路、R03bのdebug stage共通化も実装した。R01/R03aの専用性能・状態比較と、S03〜S05/S07/S11〜S13に記載したGUI・実モデルの手動確認は未実施のため、該当チェック項目は完了扱いにしない。

### 2026-09-08 自動検証記録

- `cmake --build build/arm64 --target libslic3r_tests libslic3r_gui --parallel 4`: 成功。
- `ctest --test-dir build/arm64 --output-on-failure -R 'DynaPin placement recomputes and shifts the Y interval'`: 1/1成功。
- `ctest --test-dir build/arm64 --output-on-failure -R 'DynaPin|SupportMaterial'`: 41/41成功、120.12秒。
- `cmake --build build/arm64 --target OrcaSlicer --config Release --parallel 4`: 成功。既存警告のみ。
- `git diff --check`: 成功。

未完了は`models/manual-tests/gymnast.3mf`を使うV0/Vbest計測、GUI上のUndo/Redo・探索中変更・cancel確認、実G-codeのpullコメントと上下支持形状の目視確認、R03aの性能比較である。
