# DynaPin 準備段階選択ベース方針

## 概要

DynaPin の物理ピンは、造形物でもシーン上の3Dボリュームでもなく、プリンタ外部の治具として扱う。したがって、ピン選択の主導線を G-code プレビュー上のモデルクリックに置くのは不適切である。

本方針では、DynaPin の選択は Prepare 段階で行い、その結果を設定値としてスライスへ渡す。スライスは選択済みピン一覧と DynaPin 設定 JSON だけを入力として、サポート除外領域とピン引き出し G-code を生成する。Preview はその結果を可視化するだけに留める。

## 背景と問題

現状実装には、仕様と整合しない点がある。

- Preview 側の選択は `dynapin_r<row>_c<col>` 形式の名前を持つ 3D ボリュームクリックを前提にしている。
- しかし物理ピンは外部治具であり、シーン上に実体モデルが存在するとは限らない。
- そのため、Prepare でピンを選びたい本来のワークフローと、現状の UI 導線がずれている。
- さらに、G-code 生成側は `dynapin_selected_pins` を直接参照しており、プレビュー選択状態とは独立している。

要するに、選択の source of truth は設定値であるべきなのに、UI だけが仮想 3D モデル依存になっている。

## あるべき責務分離

### Prepare

- ユーザーが使用する DynaPin を選択する場とする。
- 選択結果を `dynapin_selected_pins` に保存する。
- 選択 UI は `row,col` のリスト入力、またはグリッドクリック UI とする。
- 3D モデルやボリューム名には依存しない。

### Slice

- `enable_dynapin_support_optimization`
- `dynapin_selected_pins`
- `dynapin_config_path`

上記 3 つを入力として扱う。

- 選択ピンごとに support blocker を生成する。
- 選択ピンごとに pull G-code を生成する。
- Preview 用のコメントもこの時点で確定させる。

### Preview

- スライス結果に含まれる DynaPin コメントを読む。
- 必要なら DynaPin 設定 JSON から仮想マーカー位置を計算する。
- 選択済みピンを「外部ピンの状態」として可視化する。
- ユーザーにピンを選ばせる責務は持たない。

## 推奨データフロー

1. Prepare でピンを選択する。
2. 選択結果を `dynapin_selected_pins` に保存する。
3. Slice で JSON 設定を読み、support blocker と pull G-code を生成する。
4. Preview で G-code コメントを解析し、選択済みピンの移動だけを表示する。

この流れなら、Prepare、Slice、Preview の責務が分離され、外部治具という前提にも一致する。

## 参照設定 JSON

以下のような JSON は、DynaPin の外部治具前提と相性がよい。特に `grid`、`pull_gcode`、`insertion`、`remove_rules` を分ける構造は、責務ごとに意味が明確である。

```json
{
  "grid": {
    "pull_origin": {
      "y": "63.6",
      "z": "4.0"
    },
    "support_origin": {
      "y": "63.6",
      "z": "4.0"
    },
    "row_count": 10,
    "col_count": 14,
    "pitch": {
      "row_z": "7.4",
      "col_y": "12.4"
    }
  },
  "pull_gcode": {
    "x_hook": "160",
    "x_latch": "168",
    "x_front": "30",
    "y_offset": "4",
    "z_offset": "4",
    "feed_rate": 1500,
    "middle_feed_rate": 3000,
    "fast_feed_rate": 5000
  },
  "insertion": {
    "base_layer": 147,
    "layer_per_col": 0,
    "layer_per_row": 0
  },
  "remove_rules": [
    {
      "type": "Support",
      "start_layer": 1,
      "end_layer": 265,
      "y_mode": "outside",
      "y_window": {
        "center": "absolute",
        "y_min": "95",
        "y_max": "110"
      }
    },
    {
      "type": "Support",
      "start_layer": 266,
      "end_layer": 413,
      "y_mode": "above",
      "y_window": {
        "center": "absolute",
        "y_min": "110"
      }
    }
  ]
}
```

この構造から読み取れる意図は以下の通り。

- `grid`
  - ピン配列の論理座標 `row,col` を機械座標 `Z,Y` に変換する定義
- `pull_gcode`
  - フック位置、ラッチ位置、前進位置、および速度定義
- `insertion`
  - どのレイヤー帯で pull を差し込むかの規則
- `remove_rules`
  - サポート除去規則。単純な 1 個の blocker 矩形より柔軟

この JSON は、現在の DynaPin 実装を拡張していく際の参照スキーマとして有力である。

## 現状実装との差分

現行の `load_config_for_print()` は `grid.pull_origin`、`grid.support_origin`、`grid.pitch`、`row_count`、`col_count` を読み込む。数値はJSONのnumberとnumeric stringの両方に対応する。

ただし、現状の実装は上記 JSON の拡張部分をすべては読まない。

- `pull_gcode.feed_rate` / `middle_feed_rate` / `fast_feed_rate` という命名は未対応で、現状は `travel_feedrate` と `pull_feedrate` だけを読んでいる。
- `insertion` は未使用である。
- `remove_rules` も未使用で、現状は `support_exclusion` という単一矩形ベースの設定だけを読んでいる。

`grid.origin`、`grid.physical_origin`、`origin_row`、`origin_col`、`origin_y`、`origin_z` は現行スキーマでは使用しない。row/colは常に0始まりとする。

## 参照 JSON を踏まえた方針補強

この参照 JSON を前提にすると、Prepare / Slice / Preview の責務はさらに明確になる。

- Prepare
  - `dynapin_selected_pins` を編集する
- Slice
  - `grid` と `insertion` と `remove_rules` を使って、support blocker と pull 挿入位置を決定する
- Preview
  - `grid` を使って、選択済みピンの仮想位置を描く
  - 実際の時系列変化は G-code コメントから再生する

つまり Preview は「モデルクリック」ではなく、
「設定 JSON に基づく仮想座標系の可視化」に寄せるのが自然である。

## G-code コメント仕様

Preview が読むコメント仕様は、生成側と一致している必要がある。少なくとも 1 本のピン引き出しブロックは次の形式を満たす。

```gcode
; BEGIN_DYNAPIN_PULL ROW=2 COL=5
G0 Z12.3 F6000
G0 X110.0 Y84.0 F6000
G1 X112.0 F1200
; DYNAPIN_PULL_MOVE
G1 X115.0 F1200
; END_DYNAPIN_PULL
```

要件は以下の通り。

- 開始コメント:
  - `; BEGIN_DYNAPIN_PULL ROW=<row> COL=<col>`
- 実際にピンを引く移動の直前:
  - `; DYNAPIN_PULL_MOVE`
- 終了コメント:
  - `; END_DYNAPIN_PULL`

特に重要なのは次の 2 点。

- `ROW` / `COL` は大文字で統一する。
- `DYNAPIN_PULL_MOVE` がないブロックは Preview 側で無視される。

## UI 方針

初期版はシンプルな入力でよい。

- オブジェクト単位またはプロジェクト単位で `enable_dynapin_support_optimization` を有効化する。
- `dynapin_selected_pins` を編集できる UI を追加する。
- 初期版は `2,5;3,1;4,7` のような文字列入力でもよい。
- 次フェーズで、JSON のグリッド定義をもとにした 2D グリッド選択 UI を検討する。

避けるべきものは以下。

- Preview 限定の選択モード
- `dynapin_rX_cY` という名前のダミーモデル依存
- シーン上にピン 3D モデルを事前配置する前提

## Preview 表示方針

Preview では、物理ピンの実体モデルではなく、軽量なオーバーレイで十分である。

- 選択済みピン位置にマーカーを描く
- pull move 前は初期位置に表示する
- pull move 時は引き出し後位置へ遷移させる
- pull move 後は引き出し後位置に保持する

必要なら将来、簡易な棒状メッシュやアイコン表示へ拡張するが、v1 の必須条件ではない。

## 既存実装の見直しポイント

- Preview クリック選択は表示用途へ限定する。
- `select_dynapin_for_preview()` のような Preview 主導 API は責務が不適切である。
- 選択状態は `GCodeViewer` ローカルではなく、Prepare と Slice の設定値を正として扱う。
- Preview は設定値と G-code を読み取る consumer に徹する。

## 実装優先順位

1. 生成側 G-code コメント仕様を Preview 側と一致させる。
2. Prepare から `dynapin_selected_pins` を編集できるようにする。
3. Preview の DynaPin UI を「選択」ではなく「表示状態確認」中心に変更する。
4. 必要なら Preview オーバーレイを JSON ベース座標で描画する。
5. 旧来のモデルクリック依存コードを整理する。

## 非目標

この方針では以下を v1 の対象外とする。

- 物理ピンの高精細 3D モデル表示
- ピンモデルの自動配置
- 治具全体の CAD レイアウト生成
- 外部ハードウェア制御との直接連携

## 結論

DynaPin は外部治具であり、造形シーン内のボリュームとして扱うべきではない。したがって、ピン選択は Prepare 段階の設定操作として実装し、Slice がその設定を消費し、Preview はその結果を可視化する構成へ寄せるべきである。

この方針により、仕様、UI、スライス実装の責務が揃い、現在の Preview 依存の不整合を解消できる。
