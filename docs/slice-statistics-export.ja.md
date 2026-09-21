# スライス統計ログと sidecar JSON の実装メモ

## 目的と対象

FFF のスライス結果を GUI のプレビュー表示と照合できるよう、統計を共通の JSON に組み立てる。GUI のスライス成功時は既存ログへ `slice_statistics` を 1 レコード出し、GUI または CLI で G-code を保存したときは、その G-code と同じ場所へ `<最終 G-code ファイル名>.slice_statistics.json` を保存する。既存の `result.json` は変更しない。スライスだけで G-code を保存しない GUI 操作では sidecar は作らない。`.gcode.3mf` 保存とプリンタへのアップロードも対象外。

実装の入口は `src/libslic3r/SliceStatisticsLog.cpp` の `make_slice_statistics_log()` と `write_slice_statistics_sidecar()`。GUI は `src/slic3r/GUI/Plater.cpp` のスライス完了イベントでログを出し、`BackgroundSlicingProcess.cpp` の通常・Bambu 向け G-code 保存経路で sidecar を出す。CLI は `src/OrcaSlicer.cpp` でプレートごとの G-code 出力後に同じ処理を呼ぶ。

## 値の出所と GUI との対応

| JSON | 出所・意味 |
| --- | --- |
| `roles[]` | `GCodeProcessorResult::print_statistics.used_filaments_per_role` の役割別の長さ・重さと、`result.moves` の押出移動時間。`Support` と `Support interface` は別の役割として残す。 |
| `filaments[]` | 同じ processor のフィラメント別体積を、直径と密度で長さ・重さへ換算。列順は GUI に合わせ `model`、`support`、`flushed`、`tower`、`total`。 |
| `total_filament`、`model_filament`、`cost` | `Print::print_statistics()` を基準とする GUI 下部の集計。`model_filament` は総量から Support・Flushed・Tower を差し引く。 |
| `time.normal`、`time.stealth` | processor の推定時間。`model_seconds` は GUI の「Model printing time」と同じ `total_seconds - prepare_seconds` で、モデル押出だけを表す値ではない。移動時間は `moves` の Travel を別集計する。 |
| `filament_changes` | processor のフィラメント変更回数。 |
| `details` | GUI 下部の統計欄には直接対応しない、元の集計・補足値。`filament_stats_volume_mm3` は `GCode.cpp` で model のフィラメント別体積から設定される。 |

`GCodeProcessor.cpp` では Support、Support interface、Support transition の押出をフィラメント別 Support 体積へ加算する。一方、役割別使用量と時間は役割の区別を保持する。このため `filaments[].support` は Support interface を含む合計であり、`roles[]` の Support と Support interface のどちらか一方とは一致しない。GUI もフィラメント表では合計 Support を表示する。

長さの生値は mm、重さは g、体積は mm³、時間は秒。`*_display` は GUI に合わせて整形した文字列で、CLI は metric、GUI は `use_inches` 設定に従う。表示値は丸め済みなので、計算や比較には生値を使う。`filaments[].total` は表示列の Model + Support + Flushed + Tower から作り、processor 自身の合計体積は `total.processor_volume_mm3` に別途保持する。両者は集計経路や浮動小数点の影響で完全一致を保証しない。

`nlohmann::ordered_json` を使い、ファイル上では「識別情報 → 役割別 → フィラメント別 → 総量・コスト・時間 → `details`」の順に並べる。ただし JSON のキー順はデータの意味や機械処理の契約にしない。`roles[]` と `filaments[]` は配列であり、前者は役割 enum の順、後者はフィラメント ID の昇順。ID は JSON 上では 1 始まり。

## 出力時の境界と失敗時の扱い

- GUI のログは成功した FFF スライス完了イベントでのみ `warning` レベルに出す。結果 ID を記憶して同じ結果の再通知では重複出力しない。キャンセル・失敗時は出さない。GUI の保存時は同じ統計オブジェクトを使い、`use_inches` を反映する。
- GUI の sidecar は最終保存パスが確定し、G-code のコピー・検証などの後に出す。CLI もプレートごとの G-code 出力後に sidecar を作り、その後 `slice_statistics` と同内容の JSON を標準エラーへ出す。
- sidecar は同じディレクトリの一時ファイルへ整形済み JSON を書き、閉じてから置換する。失敗時は一時ファイルを削除して例外を呼び出し元へ返す。GUI/CLI は保存操作の失敗として扱うが、先に保存された G-code は削除しない。電源断まで含む永続化保証（`fsync`）は行っていない。
- 既存 sidecar は成功時に置換される。G-code の保存に失敗した場合、新しい sidecar は作らない。ただし以前の同名 sidecar が存在した場合、それを自動削除する処理はないため、再実行時は更新日時や結果 ID に注意する。

## 検証と今後の変更時の注意

関連テストは `tests/libslic3r/test_slice_statistics_log.cpp`。Support と Support interface の独立値、フィラメント別 Support の合算、metric/imperial 表示、順序、命名、既存ファイル置換、一時ファイルの片付けを確認する。macOS の既存 `build/arm64` 構成では次を使用できる。

```sh
cmake --build build/arm64 --target tests/libslic3r/all --config Release --parallel 4
build/arm64/tests/libslic3r/Release/libslic3r_tests.app/Contents/MacOS/libslic3r_tests '[SliceStatisticsLog]'
cmake --build build/arm64 --target OrcaSlicer --config Release --parallel 4
```

実装時、関連単体テストは 6 ケース・62 アサーション成功し、Release のアプリ本体もビルドできた。GUI の実操作によるログ・スクリーンショット照合と、複数プレートの手動スライスは別途確認すること。新しい統計項目を追加するときは、(1) processor と `PrintStatistics` のどちらの値か、(2) GUI のどの表示に対応するか、(3) raw 単位と表示丸め、(4) CLI/GUI の両経路、(5) Support の合算境界、を先に決める。
