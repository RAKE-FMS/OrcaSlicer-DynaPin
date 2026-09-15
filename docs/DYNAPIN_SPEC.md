# DynaPin 実装仕様と設計整理

この文書は 2026-09-14 時点の実装を基準にする。既存の `docs/dynapin-*-plan.*.md` は個別の計画・検討記録であり、本書は現行コードの責務と変更時の境界を記録する。DynaPin 専用のソースディレクトリはまだないため、`src/libslic3r` の README ではなく本ファイルに置く。

## 目的と処理の流れ

DynaPin は、プリンタのヘッドで引き出す外付けピンを造形中の支持面として利用し、選択したピンに対応する通常サポートを減らす。プリンタプロファイルの `dynapin_config_path` が機械固有の JSON を指し、`enable_dynapin_support_optimization`、`dynapin_selected_pins`、`dynapin_debug_stage` がスライス時の動作を決める。`enable_dynapin_placement_optimization` は配置探索を別途有効にする。

1. `Print::update_dynapin_selection()` が設定を読み、手動指定または通常サポートの投影を用いる自動選択を確定する。モデルと衝突する候補を除外する。Tree/Organic の自動選択は利用できず、手動指定が必要である。
2. `DynaPin` の幾何関数が選択ピンから object-local の除外領域と仮想ピン上面を作る。`SupportMaterial` は通常サポートの投影・生成・クリップへこれらを適用し、`TreeSupport3D` はレイヤー単位のブロッカーを取り込む。
3. `GCode::change_layer()` はピン上面の高さに達した未処理ピンをまとめ、writer の Z リフトを実体化してから引き出し G-code を挿入し、退避高さで元の XY に戻してから Z を復元する。
4. `DynaPinPreviewState` は生成済み G-code のコメントを読み、ピン移動イベントをプレビューへ渡す。GUI は表示と配置探索ジョブを担当する。

`dynapin_debug_stage` の実効値は `DynaPin::effective_debug_stage()` で統一する。0 は通常、1 は仮想上面、2 は除外と引き出しまでを有効にする。環境変数 `DYNAPIN_DEBUG_STAGE` で上書きできる。

## 座標と不変条件

`Pin{row,col}` はゼロ始まりで、row が Z、col が Y に対応する。支持側の Y は `support_origin_y + col * col_pitch_y`、ピン上面 Z は `support_origin_z + row * row_pitch_z`。引き出し経路には独立した `pull_origin_y/z` を使う。ピンごとの blocker の Z 範囲は必ず `[z_max - blocker_height_z, z_max]` とし、上段ピンの blocker をビルドプレートまで延ばさない。上方オーバーハングの下方投影はピン上面で止め、下方に別のオーバーハングがある場合のサポートは残す。

JSON の主な区画は `grid`、`support_exclusion`、`pull_gcode`。機械座標のブロッカーやプレビュー位置をスライス座標に持ち込む際は、plate offset を含まない instance shift を引く。`LocalBlocker` と `VirtualSupportSurface` は object-local XY と print Z の組、`BlockerBox` は機械/ワールド座標である。この区別は型名だけでは保証されないため、呼び出し側で維持する。

引き出しイベントのコメントは `BEGIN_DYNAPIN_PULL ROW=<row> COL=<col>`、`DYNAPIN_PULL_MOVE`、`END_DYNAPIN_PULL`。プレビューは `DYNAPIN_PULL_MOVE` のあるブロックを対象にする。モデル名からプレビュー対象を取る場合は `dynapin_r<row>_c<col>` を使う。これはスライスのピン選択とは別の表示用状態である。

## 現在の責務分割

| 所在 | 現在の責務 | 判定 |
| --- | --- | --- |
| `DynaPin.hpp/.cpp` | 設定読込、ピン選択補助、衝突・支持幾何、引き出し G-code | `Slic3r::DynaPin` というドメイン名前空間は適切。ただし 1 ファイルの変更理由が多い。 |
| `DynaPinPlacement.hpp/.cpp` | 候補生成、スコア、通常サポート量評価、並列探索、GUI 再スライス判断 | 探索の中核は libslic3r に適切。`placement_reslice_action` と `placement_job_should_continue_reslice` は GUI 制御に近く、境界が曖昧。 |
| `DynaPinPreview.hpp/.cpp` | G-code コメント解析、イベント状態、モデル名解析 | 描画から分けた点は適切。型が `Slic3r` 直下にあり、`DynaPin::Pin` と同義の `DynaPinAddress` が重複する。 |
| `GUI/Jobs/DynaPinPlacementJob.*`、`Plater.cpp` | UI スナップショット、非同期ジョブ、結果の適用 | GUI 依存を GUI 側に置く構成は適切。 |
| `Print.cpp`、`Support/SupportMaterial.cpp`、`GCode.cpp` | 選択の確定、サポート生成への統合、安全なレイヤー遷移 | 各パイプラインの状態を所有する場所に統合する必要がある。ロジックの抽出余地はあるが、全てを DynaPin 側に移すべきではない。 |

`DynaPinGCode.hpp` は `return_gcode()` だけを宣言する一方、`pull_gcode_for_pin()` は `DynaPin.hpp` にある。両者は同じ責務なので宣言をまとめるか、G-code 専用ヘッダーへそろえるのが自然である。`DynaPinPreview` の公開型は、GUI を含む利用側を確認してから `Slic3r::DynaPin` に寄せるのが望ましい。名前空間変更は API 変更なので一括で行い、単なるドキュメント更新として扱わない。

命名は概ね `snake_case` の関数、`CamelCase` の型でそろう。改善候補は `DynaPinAddress` と `Pin` の統合、`BlockerBox`/`LocalBlocker` の座標系を表す名前、`PlacementSceneSnapshot` と GUI の `DynaPinPlacementSnapshot` の役割を区別する名前である。`support_block_y_offset = -7.2` は JSON の `support_exclusion` と異なりコード定数なので、機械依存値かアルゴリズム定数かを確定してから移動する。

## 配置最適化

探索と評価の詳細は [DYNAPIN_PLACEMENT_SPEC.md](DYNAPIN_PLACEMENT_SPEC.md) を参照する。

`PlacementSceneSnapshot` は GUI スレッドでモデル・設定・対象 ID・プレート形状をコピーし、ワーカーは生きた `Plater`/`Print` を参照しない。候補は Z 回転と Y 移動で表し、ベッド内に収まる Y 区間を回転ごとに計算する。`evaluate_scene_candidate()` はコピーしたモデルを変換して一時的な `Print` を評価し、通常サポート量を指標にする。粗探索後に局所探索を行い、キャンセルと資源状況を見ながら角度グループを処理する。GUI ジョブは結果を適用し、必要な再スライスにつなぐ。詳細な探索パラメータを変更する際は `DynaPinPlacement.hpp` と対応テストを正とする。

## ビルド時間について

現在 `DynaPin*.cpp` は `libslic3r` の単一 static library のソースで、専用 CMake target はない。`libslic3r` には PCH 設定もある。したがって DynaPin 実装 `.cpp` だけの編集なら通常はその翻訳単位とリンクが主な再実行範囲であり、`libslic3r` に属するだけで全 `.cpp` が毎回再コンパイルされるとは限らない。一方、`DynaPin.hpp` は `Print`、`SupportMaterial`、`GCode` などへ広がり、`DynaPinPlacement.hpp` は `PrintConfig.hpp`、`Utils.hpp` 等の重い依存を公開する。これらのヘッダー変更、PCH の更新、CMake 再構成、静的ライブラリの再リンクは待ち時間を大きくし得る。実測はまだないので、主因は断定しない。

改善の順序は次の通り。

1. 同一ビルド設定で `DynaPin.cpp` だけ、`DynaPin.hpp` だけ、`DynaPinPlacement.cpp` だけを変更した場合のビルドログと経過時間を比較し、再コンパイル対象を記録する。既存の作業ツリーを汚さない専用ビルドディレクトリを使う。
2. `DynaPinPlacement.hpp` の公開 API に不要な `PrintConfig.hpp`、`Utils.hpp`、Eigen 等を前方宣言・内部型分離で減らす。`DynaPin.hpp` も設定・ピン型と支持幾何 API を分離する。ただし PCH と include order を確認し、ヘッダーの自己完結性を維持する。
3. コンパイル依存の減少が確認できてから、必要なら `src/libslic3r/DynaPin/` に専用ファイル群を移し、同フォルダの README を本書の後継にする。別 target 化はリンク依存と PCH の損益を計測して判断する。フォルダ移動だけではビルド時間は短縮しない。

## 検証と未確定事項

関連する自動テストは `tests/libslic3r/test_dynapin_placement.cpp` と `test_dynapin_preview.cpp`。変更時はピンの座標変換、上段/下段のサポート分離、衝突除外、G-code のリフト・復帰順、配置探索のキャンセル/同点判定を確認する。実機での衝突安全性、ピン上への定着、Tree/Organic の自動選択はテストだけで保証できない。

現時点で未確定なのは、`support_block_y_offset` の設定化、プレビューのモデル名選択を将来も残すか、通常サポート以外の自動選択範囲、専用 target のビルド上の価値である。これらは現行仕様に混ぜず、実測またはユーザー操作要件を得て決める。
