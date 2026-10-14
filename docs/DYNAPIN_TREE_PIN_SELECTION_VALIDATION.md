# DynaPin Treeピン選択の軽量化検証（2026-10-11）

## 実装

- 候補ピンの表面・高さ・外接矩形を選択開始時に一度だけ生成し、衝突済み・選択済み状態を候補索引で保持。
- 遮蔽確定・残領域消滅で選択済みピンの走査を終了。外接矩形が厳密に離れている場合だけ交差・差分を省略。境界接触とoffset後の到達判定は従来の厳密演算を維持。
- 同じ高さのピンでは、厳密な差分・offsetの結果を選択1件の中だけで再利用する。新たに追加する同じ高さのピンは「上方のピン」の厳密条件に入らないため、差し引く上方ピンの集合と順序は変わらない。高さが変われば破棄し、遮蔽判定とoffset後の交差判定は各ピンで実行する。異なるY候補へ投影結果は持ち越さない。
- 衝突検査の層範囲を既存のEPSILON条件と同じ二分探索で限定。
- PrintObjectごとに物体座標のレイヤーポリゴンと接触領域を所有。Y移動で再利用し、posSlice・全ステップの無効化、レイヤー破棄、オブジェクト破棄で破棄。キャンセル・例外時には構築途中のデータを公開しない。
- キャッシュはTreeプレビュー再試行のキャッシュと独立。角度群間で共有しない。
- 衝突検査とTree選択の主要区間をsteady_clockで計測。衝突検査・下方投影・近傍追加の厳密演算回数、接触領域構築回数を集計。
- 大量のsmooth_nodesログをtraceへ変更。通常ログは区間集計を出力。メモリ予約量・実行許可条件は変更せず、待機時間・実際の最大角度群数を記録。

候補数、処理順序、選択追加順序、最後のソート、探索品質、体積評価、ピン棄却・再生成、ツールパス着地検証、ブロッカーの上下端、dynapin_debug_stageは変更しない。公開設定・配置評価APIの追加はない。

## 同値性と回帰

`tests/fff_print/dynapin_selection_reference.hpp` に変更前の選択・衝突検査をテスト専用の参照実装として保持。製品の切替設定は追加していない。

新規検証は実モデルSimple Bridge・RemoveGymnastBase・Arch Bridgeの衝突結果、選択・棄却ピンの集合と順序、衝突なし・一部衝突・全衝突を比較する。同じ高さ、境界接触、空の接触領域、多段オーバーハング、Slim・Strong・Hybrid、枝径設定と枝移動角を含む。Y移動時の再利用と、スライス設定変更・回転・形状変更・明示的なレイヤー破棄・キャンセル後の再構築を検証する。

既存テストで並列数1・2・自動の候補有効性・評価体積・採用位置角度・最終ピン・サポート押出体積の一致、Normal、DynaPin無効、ピン棄却、キャンセル、資源取得失敗、bad_alloc、待機復帰、進捗単調増加を確認する。

既存の `DynaPin Tree selects pins after applying an optimized pose` は変更前から期待3ピンに対して1ピンとなる。今回の成功には含めない。

## 測定方法

macOS arm64、Release、Xcode 26.4.1。変更前はHEAD `dd39b54422` の保存済みバイナリ、変更後は今回のソースからビルドしたバイナリ。TZは変更しない。CLIはGUIと共通の配置評価・選択・スライス処理を使用する。

同じモデルとプロジェクト設定を読み込み、Tree Slim・Normal defaultを指定。DynaPinサポート最適化と配置最適化を有効化し、選択ピンは空、KP3S設定、debug_stage=2、通常infoログ（--debug 3）。各試行に独立したdatadirとoutputdirを使用。比較プロセスを順番に実行し、測定中はビルド・テストを同時実行しない。

選択区間はスレッドごとの `selection begin` → `selection resolved`。衝突検査を含む。探索区間の共通測定境界は最初の選択開始 → CLIのdynapin_placement結果出力であり、最初のスライス準備を含まない。変更後にはrun_placement_task内の探索時間も別途記録する。CLI結果出力後の最終スライス区間はG-code書き出し・プロセス終了処理を含む。CPU時間とピークRSSはwait4のプロセス全体値。

メモリ待機を含む実時間と選択の計算時間を混同しない。変更前には実最大同時実行数・待機時間の集計がないため、取得できない値を0と扱わない。配置結果・評価数・評価体積・最終ピンが一致するかも確認する。

## 実行コマンド

```sh
cmake --build build/arm64 --config Release --target libslic3r_tests fff_print_tests OrcaSlicer --parallel 4
build/arm64/tests/libslic3r/Release/libslic3r_tests.app/Contents/MacOS/libslic3r_tests '[DynaPin],[DynaPinPlacement]' --reporter compact
build/arm64/tests/fff_print/Release/fff_print_tests.app/Contents/MacOS/fff_print_tests '[DynaPin]~[.]' --reporter compact
```

## 実施済みの結果

Releaseのアプリ・関連テストを再ビルドした。

| 検証 | 結果 |
|---|---|
| 新規 `[TreePinSelection]` | 4ケース・788 assertionsすべて成功 |
| libslic3r `[DynaPin],[DynaPinPlacement]` | 32ケース・157 assertionsすべて成功 |
| FFF `[DynaPin]~[.]` | 32ケース中31成功、1件は変更前からの失敗。1099 assertions中1098成功 |

新規モデルは `tests/data/dynapin/` に保持し、未追跡のローカルモデルがなくても同値性テストを実行できる。共有オブジェクトのレイヤー置換・共有解除時のキャッシュ破棄も確認した。コードレビューで残る指摘はない。

同値性テスト内の内部選択器診断を3回実行し、全3回とも788 assertionsが成功した。接触領域を事前構築しているため、衝突検査を含む候補評価全体・全探索の測定ではない。以下は各入力の3回の中央値であり、この値を全探索の性能目標達成の判断には使用しない。全衝突では接触領域の逐次unionが残る負荷になっている。

| 入力 | 参照中央値 ms | 変更後中央値 ms | 変更後 / 参照 |
|---|---:|---:|---:|
| Simple_Bridge 衝突なし | 0.334 | 0.140 | 0.420 |
| Simple_Bridge 一部衝突 | 0.338 | 0.155 | 0.460 |
| Simple_Bridge 全衝突 | 0.250 | 0.100 | 0.398 |
| RemoveGymnastBase 衝突なし | 643.010 | 266.816 | 0.415 |
| RemoveGymnastBase 一部衝突 | 566.607 | 323.206 | 0.570 |
| RemoveGymnastBase 全衝突 | 2518.030 | 2551.570 | 1.013 |
| Arch_Bridge 衝突なし | 57.720 | 27.087 | 0.469 |
| Arch_Bridge 一部衝突 | 37.170 | 23.824 | 0.641 |
| Arch_Bridge 全衝突 | 59.014 | 54.122 | 0.917 |

GUIの通常のSlice plateからTree(auto)・Tree Slim、両DynaPin最適化有効、ピン指定なしで実行した。新しい選択処理・Tree生成が動作し、その後5%でメモリ待機を表示した。ログの空きメモリは3289694208 bytes、角度群の見積りは536870912 bytes。既存の予約条件を変更していない。待機中のキャンセルは終了し、探索ログは `evaluations=1, status=2, peak_active=1, memory_wait_ms=53247.7, elapsed_ms=59333.6`。GUIで探索・最終スライス完了を確認したという結果ではない。

変更前の最初のCLI試行は長い実行中断を含み、探索完了前に停止した。成功例・中央値・改善倍率の計算から除外する。全探索の3回比較とNormalとの比、最終スライスの比較は未確定であり、半減目標の達成は未確認。

CLIも角度群の資源待機に入り、サンプリングで `optimize_placement` のcondition_variable待機と休止中のTBBワーカーを確認した。全メモリ17179869184 bytesに対し予約はその1/5、角度群1件の最低見積りは536870912 bytesなので、開始には合計約3.7 GiBの空きが必要になる。今回起動した待機プロセスは停止した。3モデル×変更前Tree・変更後Tree・変更後Normalの各3回、完了後の配置・最終ピン・サポート体積比較、GUIの探索完了確認は、資源条件が整ってから実施する必要がある。
