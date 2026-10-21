# Treeサポートが適切に反映されない問題の調査

調査日: 2026-10-21（JST）
初期調査対象ソース: `1b0f3d1e7d70cf880cfaf411625a80b3f9da747b`
対象モデル: `models/Arch_Bridge/PLA.3mf`
状態: 第8節のTree着地修正は`294c1c693d`でコミット済み。第9節のDefault／Organic対応は追加実装・Release／CLI／GUI検証済み。再現性などの未達事項は第9節に記載。実機造形は未実施。第1〜7節は修正前の調査記録。

## 現在の到達点

Default／明示Organicの生成器にDynaPinの着地・出力層境界・最終押出検証を追加した。最終ReleaseでArch Bridgeは1本以上、Gymnast系の代替モデルは2本のPullを生成した。関連テストは既存の1件を除いて成功。実モデルのピン集合・材料量には反復による揺らぎがあり、無効時のバイト一致と実機造形は未確認。最新の実装・検証結果・制約は第9節を参照。

## 初期調査の結論

画像の実行ではTree用のDynaPin処理が動いており、最終G-codeにも7本のピン引き出しが存在する。ただし、次の問題を確認した。

1. **Treeの`default`はピン候補の有無によってOrganicとSlimが切り替わる。** 同じ画面設定でもDynaPin有効・無効で生成器が変わる。今回の比較では、`default`のままDynaPinを有効にするとサポート相当フィラメントが10.289gから15.318gへ約48.9%増えた。一方、Slimを明示して比較すると18.673gから15.302gへ約18.1%減る。これらは別条件の比較であり、混同してはならない。
2. **ピン上面とサポート初層底面が一致していない。** 実GUI出力で約0.10〜0.20mmの幾何学的な隙間を確認した。Treeの層計画はピン高さを境界として挿入せず、着地判定もXY位置とフラグを設定するだけで高さを合わせない。
3. **候補20本のうち最終使用は7本だが、画面の警告はモデル衝突による候補除外だけを説明する。** Tree生成後の候補減少はこの警告から分からない。画像の右側でピンが使われていないことは最終ピン集合と整合するが、各ピンが落ちた個別理由までは既存ログで確定できない。

したがって、「Tree機能が一切反映されていない」という診断にはならない。実効スタイルの暗黙変更、着地Zの不一致、候補から最終使用までの説明不足を分けて扱う必要がある。画像だけから、全ての途切れが不正な浮遊サポートであるとも断定しない。

## 1. 画像と実行データの照合

調査開始時の作業ツリーは未追跡の`tmp/`だけがある状態だった。このディレクトリには手を加えていない。

ユーザー画像のモデル名`Arch_Bridge.stl`と警告のピン一覧は、次のGUIログの03:05:11の選択結果と一致した。

```text
~/Library/Application Support/OrcaSlicer/log/debug_Wed_Oct_21_03_00_35_35218.log.0
```

対応する実GUI出力も残っており、直接読み取った。

```text
/var/folders/p2/91cph2wx6m54fkv61tph97j00000gn/T/orcaslicer_model/Wed_Oct_21/03_04_55#35218#268/Metadata/.35218.2.gcode
```

GUI出力に保存された設定は次のとおり。

| 設定 | 値 |
|---|---|
| `support_type` | `tree(auto)` |
| `support_style` | `default` |
| `dynapin_debug_stage` | `2` |
| `support_interface_bottom_layers` | `2` |
| `tree_support_branch_diameter` | `5` |
| `tree_support_branch_angle` | `45` |

03:05:11の選択ログは候補総数140、モデル衝突47、選択20、警告に列挙された除外32を記録する。「モデル衝突47」と「警告の32」は異なる集合・段階の値であり、単純に加減しない。

生成前の選択20本:

```text
0,2 0,3 0,4 0,9 0,10 0,11
1,3 1,4 1,5 1,6 1,7 1,8 1,9 1,10
2,5 2,6 2,7 2,8
4,2 4,11
```

最終GUI G-codeの`BEGIN_DYNAPIN_PULL`は次の7本で、それぞれ1回だった。

```text
0,2 0,3 1,4 2,5 2,6 2,7 2,8
```

同じ処理区間でTree生成の完了ログが2回ある（03:05:18と03:05:24）。自動選択では未使用候補を除いて再生成するため、候補20本と最終7本の違い自体は再試行の設計と整合する。

関連箇所:

- [PrintObject.cpp](../src/libslic3r/PrintObject.cpp): `generate_tree_support_with_dynapin_retries()`、4017行付近。`used_pins`を次の候補として再生成する。
- [Print.cpp](../src/libslic3r/Print.cpp): `finalize_dynapin_tree_selection()`、211行付近。最終選択を着地計画の`used_pins`へ置き換える。
- 同関数は自動選択で落ちたピンの理由を警告へ追記しない。手動選択の場合の警告追記は存在する。

## 2. `default`が生成器を切り替える

[SupportParameters.hpp](../src/libslic3r/Support/SupportParameters.hpp)の180〜186行で、Treeの`default`を次のように解決している。

```cpp
support_style = object.print()->dynapin_selection().pins.empty() ? smsTreeOrganic : smsTreeSlim;
```

その結果、`TreeSupport::generate()`の1635行付近では次の分岐になる。

```text
default + ピン候補なし → Organic → generate_tree_support_3D()
default + ピン候補あり → Slim    → TreeSupportの枝生成・DynaPin着地処理
```

この選択に使うのはTree生成前の候補集合であり、最終的に使えたピン集合ではない。また、`Print::resolve_dynapin_selection()`の146行付近では明示的な`organic`を除外する一方、`default`は除外しない。設定に保存された`default`だけを見ても、実際に使った生成器は分からない。

この切替はコミット`b366e1e963`のTree対応に含まれる。無効時の通常Organicを維持しながら、有効時に既存のSlim着地処理へ送る実装だが、利用者には同じスタイルに見える。

### 実スライスの比較

同じモデル姿勢・プリンタ・フィラメント・レイヤー高さで、配置探索を無効にし、スタイルとDynaPin有効値だけを変更した。全4実行が終了コード0でG-codeと統計JSONを出力した。

| 指定スタイル | DynaPin | 実効生成器 | Pull数 | サポート相当重量 | 推定総時間 |
|---|---|---|---:|---:|---:|
| `default` | 無効 | Organic | 0 | 10.288810g | 10721.137秒 |
| `default` | 有効 | Slim | 7 | 15.318105g | 12406.475秒 |
| `tree_slim` | 無効 | Slim | 0 | 18.672590g | 13333.367秒 |
| `tree_slim` | 有効 | Slim | 7 | 15.301990g | 12404.762秒 |

重量は`plate_1.gcode.slice_statistics.json`の`filaments[0].support.weight_g`。SupportとSupport interfaceを含むサポート相当値である。時間は同JSONの`time.normal.total_seconds`で、実測造形時間ではない。

`default`の比較は5.029295g増加、Slim固定では3.370600g減少した。GUIのサポート相当重量は15.317166gで、CLIと約0.001gの差がある。最終7本は一致する。単回実行の測定であり、微小差の原因や完全な経路同一性は検証していない。

Slim有効と`default`有効のG-code設定差は`support_style`と設定差分のメタデータだった。同じ実効Slimでも値が微小に異なるため、統計値の完全一致を主張しない。

## 3. ピン上面と着地層の隙間

現在のピン高さは[DynaPin.cpp](../src/libslic3r/DynaPin.cpp)の`blocker_z_range()`、203行付近で導出する。

```text
pin_top_z = support_origin_z + row × row_pitch_z = 7.3 + row × 7.4
blocker span = [pin_top_z - 5.0, pin_top_z]
```

古い資料の列・行の意味や高さ式を流用せず、現行実装と使用JSONに合わせた。

実GUI G-codeの`;Z:`と`;HEIGHT:`からサポート層の底面`Z - HEIGHT`を復元した。各使用ピンの矩形領域にXY外接矩形が重なる押出線分のうち、ピン上面以上にある最初のSupportまたはSupport interfaceを調べた。

| ピン | ピン上面Z | 最初の層の底面Z | その層の上面Z | 隙間 |
|---|---:|---:|---:|---:|
| `0,2`、`0,3` | 7.300000 | 7.409092 | 7.690910 | 約0.109mm |
| `1,4` | 14.700000 | 14.900011 | 15.188900 | 約0.200mm |
| `2,5`〜`2,8` | 22.100000 | 22.200001 | 22.300000 | 約0.100mm |

全て最初の役割はSupport interfaceだった。同じXY外接矩形条件で、ピン上面をZ方向にまたぐサポート押出線分は0件。XYは外接矩形による候補抽出であり、全線分の厳密なポリゴン接続判定や力学評価ではない。線幅・Zコメントの丸め誤差だけで0.1〜0.2mmの差を説明することはできない。

コード上も隙間が許容される経路を確認した。

- [TreeSupport.cpp](../src/libslic3r/Support/TreeSupport.cpp): `plan_layer_heights()`、3440行付近。モデル・コンタクトの境界から層を作り、ピン上面を必須境界として追加しない。
- `drop_nodes()`、2920行付近。現在層と次層の底面の間にピン上面が入れば着地できる。`position`・`movement`・着地フラグを更新するが、ピンに合わせて層の`print_z`や`height`を変更しない。
- `validate_dynapin_landings()`、1773行付近。着地位置付近の押出存在、親との接続候補、ブロッカーとの干渉を調べるが、初層底面とピン上面の一致は要求しない。
- [DynaPin.cpp](../src/libslic3r/DynaPin.cpp): `landing_regions()`、33行付近。最初の領域には最大1層分の隙間を明示的に許容する。これは今回のずれを検出する検証にはならない。

よって、「着地として採用されたがピン上面へ幾何学的に接していない」状態は、単なるプレビュー表示だけの問題ではない。実際の定着可否は未検証であり、造形失敗を実機で確認したという意味ではない。

## 4. 右側の候補が落ちる理由の確定範囲

生成前に選ばれた`0,9`、`0,10`、`0,11`、`1,9`、`1,10`などは最終使用集合にない。画像の左右差はこの集合差と整合する。

既存ログには枝の`cannot reach pin ... : collision`や`has no reachable pin landing ...; continuing toward build plate`があり、到達・衝突判定が働いている。ただし、衝突ログは枝と層ごとの誘導失敗を表し、そのピン全体の最終棄却を意味しない。実際、ログに衝突が出たピンも最終使用されている。

`TreeSupport.cpp`の1672行以降と`validate_dynapin_landings()`には、ノードのブロッカー干渉、着地フットプリントのはみ出し、押出欠落、子ノード残存、親側押出欠落など複数の除外理由がある。現在はこれらがピン別の集計として残らないため、計13本の候補減少を一つの条件に帰属させることはできない。

また、`dynapin_target()`は到達可能候補から高いピンを優先して単一の目標を返す。誘導先で衝突しても、その場で別の低いピンを列挙して経路を再探索する処理はない。これは改善候補だが、今回の左右差の直接原因と断定するには、候補別の棄却理由と経路を記録した追加実験が必要である。

## 5. 再現手順

既存Releaseバイナリを使用した。再ビルドは行っていないため、ソース全体とバイナリのビルド来歴を完全に照合したという主張ではない。調査時の識別値は以下。

```text
OrcaSlicer executable SHA-256:
eadaf0639a25b7626d857efc13363b461b1e01ea8cba2229fab0a273ef3b14eb
executable mtime: 2026-10-21 01:52:25 JST
models/Arch_Bridge/PLA.3mf SHA-256:
9d0523a3dbbdc61eb25e2cb3d8cd9672bf8d9366897dfa8c5a7d73c5258bb31f
```

リポジトリ直下から、元の3MFを変更せず一時コピーを作る。

```sh
python3 - <<'PY'
import json
import pathlib
import zipfile

root = pathlib.Path('/tmp/dynapin-tree-repro')
root.mkdir(exist_ok=True)
for style in ('default', 'tree_slim'):
    for enabled in (False, True):
        name = f'{style}-{"on" if enabled else "off"}'
        with zipfile.ZipFile('models/Arch_Bridge/PLA.3mf') as src, \
             zipfile.ZipFile(root / f'{name}.3mf', 'w', zipfile.ZIP_DEFLATED) as dst:
            for item in src.infolist():
                data = src.read(item.filename)
                if item.filename == 'Metadata/project_settings.config':
                    config = json.loads(data)
                    config.update(support_type='tree(auto)', support_style=style,
                                  enable_support='1', dynapin_selected_pins='',
                                  dynapin_debug_stage='2',
                                  enable_dynapin_placement_optimization='0',
                                  enable_dynapin_support_optimization='1' if enabled else '0')
                    data = json.dumps(config).encode()
                dst.writestr(item, data)
PY

for name in default-on default-off tree_slim-on tree_slim-off; do
    build/arm64/src/Release/OrcaSlicer.app/Contents/MacOS/OrcaSlicer \
        --slice 0 --debug 4 \
        --outputdir "/tmp/dynapin-tree-repro/$name" \
        "/tmp/dynapin-tree-repro/$name.3mf" \
        > "/tmp/dynapin-tree-repro/$name.log" 2>&1
done
```

`DYNAPIN_DEBUG_STAGE`環境変数による上書きがあると条件が変わるため、再現時は設定されていないことを確認する。解決される`Kingroon/dynapin/kp3s.json`のパスと内容もログで確認する。今回の実行はユーザーデータ内の`system/Kingroon/dynapin/kp3s.json`を使用した。

今回の一時出力は`/tmp/dynapin-tree-investigation/`にある。`default-on`・`default-off`が画像の指定スタイルに対応し、`on`・`off`はSlim明示比較。`check_gcode.py`は上記の層底面抽出に使った一時診断コードである。一時ファイルはOSにより削除され得るため、測定値・条件・解釈は本書内に記録した。

## 6. 実行した既存テストと限界

```sh
build/arm64/tests/fff_print/Release/fff_print_tests.app/Contents/MacOS/fff_print_tests \
  'DynaPin Tree keeps all three Simple Bridge pins,DynaPin Tree rejects pins when a wide overhang has unlandable branches,Tree support generation works with DynaPin disabled' \
  --reporter compact
```

3ケース・9 assertions成功。Simple Bridgeのピン維持、着地できない広いオーバーハングでのピン棄却、DynaPin無効時のTree生成を確認する既存テストである。

この成功は、Arch Bridgeでの全枝の支持連続性、着地Z一致、`default`の実効スタイルの説明、無効時に対する材料削減を保証しない。既存の選択器同値性テストも、最終着地高さを検証するものではない。全テスト実行・別コミットとの無効時バイト同一比較・実機造形は今回行っていない。

## 7. 修正へ進む場合の順序

1. **着地高さを保証する。** ピン上面をTree層計画へ反映し、初層底面とピン上面の一致を最終経路で検証する。7.3・14.7・22.1mmなどモデルの積層境界と合わない高さを回帰条件にする。ブロッカー下端を0へ伸ばして帳尻を合わせてはならない。
2. **実効スタイルを明示する。** `default`の暗黙切替をどう扱うか決め、OrganicとSlimの比較をUIや材料評価で混同させない。OrganicへのDynaPin着地対応を追加するか、対応スタイルを明示するかは設計判断として扱う。
3. **候補から最終ピンまでの診断を追加する。** 各試行の候補集合・使用集合・棄却理由を記録し、モデル衝突の警告と分けて説明する。
4. **Arch Bridgeの全経路を評価する。** ピン使用本数だけで合格にせず、ピン上の初層、親子経路の連続性、ピン外のベッド到達、同じ実効スタイルでの材料・時間比較を確認する。右側で失われた候補の原因を特定してから経路探索を変更する。

初期調査の段階では修正済み・印刷可能とは扱わなかった。以降に修正後の結果を記録する。


## 8. 修正実装と最終検証（2026-10-21）

### 実装範囲

実装時のHEADは`4b4943e9d5`。作業途中でチェックアウト状態が変わっていたため、初期調査のHEADへ戻さず、現在のソースへ変更を適用した。以下の変更は作業ツリーにあり、コミット・PR作成はしていない。既存の未追跡`tmp/`と元モデルは変更していない。

- **実効スタイルを統一**: Treeの`default`を常にOrganicへ解決し、ピンの有無によるSlimへの暗黙切替を削除した。Organic/defaultではDynaPin着地を適用せず、Slim・Strong・Hybridを選ぶよう警告する。自動・手動の双方を対象とする。
- **着地Zを保証**: 独立サポート層ではピン上面を層境界へ挿入する。最小・最大層厚を満たす分割のみ採用し、固定層では既存の境界を変更しない。どちらもサポート初層底面がピン上面と一致する場合だけ採用する。層分割の可否判定と実際の分割に同じ丸め許容を使う。
- **棄却後の再生成**: 自動・手動とも使用できなかったピンを外してサポートを再生成する。候補集合は試行ごとに縮小させる。ブロッカー下端を0へ伸ばす変更はしていない。
- **棄却理由を保持**: 層厚制約、着地なし、ブロッカー衝突、上面領域外、押出経路なし、非接続をピン単位で記録する。モデル衝突の選択時警告とは別に表示・ログ出力し、再生成前の理由も失わない。
- **複数オブジェクトで整合**: 最終的に残す物理ピンと全オブジェクトのサポート経路の干渉を確認し、衝突するピンを除いてサポートを再生成する。NormalとTreeが混在する場合も対象。機械座標からローカル座標への変換は`DynaPin.cpp`内に集約した。

主な変更箇所は`PrintConfig.hpp`・`SupportParameters.hpp`のスタイル解決、`TreeSupport.cpp`の層計画・着地判定、`PrintObject.cpp`の再生成と診断、`Print.cpp`の全体整合確認、`DynaPin.cpp/.hpp`の理由・インスタンス別ブロッカーである。

Organicへの着地対応や、Arch Bridge右側で棄却された全候補を利用するための枝経路探索の再設計は今回の範囲に含めない。

### ビルド・自動テスト

```sh
cmake --build build/arm64 --config Release \
  --target fff_print_tests libslic3r_tests OrcaSlicer --parallel 2

build/arm64/tests/libslic3r/Release/libslic3r_tests.app/Contents/MacOS/libslic3r_tests \
  '[DynaPin],[DynaPinPlacement]' --reporter compact

build/arm64/tests/fff_print/Release/fff_print_tests.app/Contents/MacOS/fff_print_tests \
  '[DynaPin]~[.]' --reporter compact
```

Releaseのアプリ・両テストターゲットのビルド成功。libslic3rは32ケース・157 assertionsすべて成功。fff_printは39ケース中38成功、2966 assertions中2965成功。合計71ケース中70成功、3123 assertions中3122成功。

唯一の失敗は既存の`DynaPin Tree selects pins after applying an optimized pose`で、期待3本に対し1本となる。修正前の保存済みテストバイナリ`/tmp/dynapin-red-tests`でも同じテストが失敗したため、今回の修正で新たに発生した失敗とは扱わない。期待値を書き換えて通す変更はしていない。

追加・更新した検証は、Slim/Strong/Hybrid、独立層/固定層、底面インターフェースの有無、複数高さ、手動選択、Normalとの混在、default/Organic警告、層厚制約、丸め境界、Arch Bridgeの実押出経路を含む。配置探索テストにはプレート原点の初期化を補い、Slimの材料比較は実際の`support_fills`の押出体積で測定するよう修正した。固定のY移動量に代えて、回転・使用ピン・着地・材料削減の成立を検証する。

独立したコードレビューを繰り返し、混在時のピン干渉、再生成時の棄却理由保持、層分割の丸めに関する指摘を修正した。最終の製品コードレビュー、およびその後のテスト変更レビューに追加の指摘はなかった。

### Arch BridgeのCLI比較

元モデルの一時コピーを使用し、配置最適化を無効、Tree(auto)、DynaPin stage 2、同じピン設定で比較した。サポート相当量はサポートとインターフェースの合計。時間はスライサの総造形時間推定値であり、実測ではない。

| 条件 | サポート相当量 (g) | 総推定時間 (s) | Pull本数 |
| --- | ---: | ---: | ---: |
| Slim・DynaPin無効 | 18.6681 | 13323.47 | 0 |
| Slim・DynaPin有効 | 15.6557 | 12694.91 | 7 |
| default (Organic)・DynaPin有効 | 10.2517 | 10689.83 | 0 |
| Normal・DynaPin無効 | 36.7760 | 17209.03 | 0 |

Slim同士ではサポート相当量が**16.14%減**、総推定時間が**4.72%減**。修正前の有効時15.302gよりは増えているが、以前欠けていた着地部分も生成するためであり、無効時との削減評価とは区別する。Organicの値は別生成器であり、Slimとの差をDynaPinの削減効果として扱わない。

最終ピンは`0,2 0,3 1,4 2,5 2,6 2,7 2,8`。G-codeのZ・HEIGHTから抽出した、ピン領域に重なる最初のサポート押出層は以下のとおり。

| ピン | ピン上面 (mm) | 初層底面 (mm) | 初層上面 (mm) |
| --- | ---: | ---: | ---: |
| 0,2 / 0,3 | 7.3 | 7.3 | 7.6 |
| 1,4 | 14.7 | 14.7 | 14.98 |
| 2,5 / 2,6 / 2,7 / 2,8 | 22.1 | 22.1 | 22.2 |

7本すべてで初層底面がピン上面と一致した。修正前GUI出力にあった約0.10〜0.20mmの隙間を解消している。一時解析スクリプトのXY判定は押出線分のバウンディングボックスとピン領域の重なりによる粗い確認であり、全枝の接続性の証明ではない。別途Arch Bridgeのインプロセステストで着地計画と実押出の存在・底面一致を検証した。ピン高さをまたぐ押出線分の粗い検査でも該当は0だった。

### GUI確認

ユーザーがプロジェクトを閉じた後、ビルドしたアプリを再起動して一時3MFを開いた。GUI読み込みでは保存プリセットからNormalと配置最適化が復元されたため、画面上でTree(auto)・配置最適化無効を設定し直して検証した。

- default/OrganicではOrganicの枝が生成され、DynaPin着地に対応していないこととSlim/Strong/Hybridの選択を促す警告を確認した。
- Tree Slimへ変更して再スライスし、プレビューのPull events 7・Blocked regions 7を確認した。画面上のサポート14.17g＋インターフェース1.49gはCLIの約15.66gと整合した。

元の3MFやユーザープリセットへの上書き保存はしていない。今回のGUI確認はArch Bridgeを用いたもので、`gymnast.3mf`での追加確認は行っていない。

### 無効時の回帰比較と残る制約

DynaPin無効時の比較は実施したが、**バイト一致の要件を確認済みとはできない**。比較用に保存した旧バイナリは初期調査時の実行ファイルであり、実装開始時HEADを厳密に再ビルドしたものではない。さらに旧バイナリ・同一Slim入力の繰り返しでもサポート量が18.6571gから18.9016gへ揺らぎ、新版の18.6681gはその範囲内だった。これだけで無影響を証明することはできない。

Normal無効時は材料量36.7760gが一致し、コメント・進捗・送り速度を除いた250047コマンドも一致した。ただし送り速度などに差があり、生G-codeのバイト一致ではない。厳密な保証には、同一ソース基準・依存環境を固定し、既存の出力非決定性を切り分けた比較が必要となる。

実機のピン引き出し、接触面の定着、たわみ、ファンを含むヘッド干渉、全枝の造形成功は未検証。この実装とソフトウェア検証だけをもって実機印刷可能とは断定しない。

最終ログ・一時出力（OSによる削除対象）:

- `/tmp/dynapin-tree-final-build.log`
- `/tmp/dynapin-tree-final-libs.log`
- `/tmp/dynapin-tree-final-fff-2.log`
- `/tmp/dynapin-tree-known-baseline.log`
- `/tmp/dynapin-tree-investigation/final-{tree,off,default-on,normal-off}/plate_1.gcode`
- `/tmp/dynapin-tree-investigation/check_gcode.py`


## 9. Default／Organicへの追加対応（2026-10-21）

第8節は`294c1c693d`でコミットした。その時点でOrganicは未対応としてピンを除外していたため、Defaultでピン0本になった。追加プランに基づき、Organic生成器そのものへ着地処理を追加した。Slimへの暗黙切替は戻していない。本節に記載した追加実装・テスト・検証記録を同じコミットにまとめる。

### 実装内容

- 試行ごとの許可ピンから、既存DynaPin APIでローカル上面・有限Zブロッカーを取得する。探索は既存Organicの層番号を維持し、ピン上面を到達可能な終点として両方のavoidanceマップへ追加する。モデル着地領域とは分けて保持する。
- 上面に枝断面が収まる位置を、高いピン、短い移動、行・列の順に選ぶ。初層の傾斜移動と半押出幅も余白へ含める。ピンに着地した枝だけ下向き伝播を止める。
- 正確な上面Zを持つ根を追加し、平滑化で固定、メッシュ下端を水平に閉じる。モデル着地の下向き延長はピン根へ適用しない。短い枝や中心線がゼロ長になる場合も扱う。
- メッシュの出力段階だけ独立したZ境界表を使う。既存contact/interface境界を保ってピン上面を挿入し、最小・最大層厚で分割する。固定層で境界が合わない候補・分割不能な候補はLayerHeightとして棄却する。
- 接触・インターフェース領域を元のZ範囲で新しい層へ対応させ、出力層変更時の投影を物理高さで扱う。ピン上には底面接触領域を生成する。
- 各出力層のZ範囲に重なるモデル衝突領域を合成する。合成領域はその試行内だけの層ごとのローカルデータとして再利用する。ブロッカーは計算済みの有限Z範囲だけ適用し、下端を0へ伸ばさない。
- 底面一致・断面・実押出・ブロッカー侵入を共通検証する。Slim等は半押出幅を含む既存ノード断面・接続検証を維持し、Organicは初層押出経路の幅を含む領域全体も上面内に制限する。Slimの経路は隣のベッド支持ノードも通るため、その経路全体を単一ピンへ制限しない。
- Organicでは平滑化後の根の傾斜と、着地から上方への支持領域・押出の連続性も検証する。同じ物理ピンの一部の根が失敗した場合も、そのピンを外して再生成する。
- ノード配置中に必要な枝を失う候補では、通常の候補削減後も集合が安定する試行に欠落が残る場合、そのピン集合をDisconnectedとして棄却し、ピンなしの通常サポートへ再生成する。着地した枝だけの成功で欠落を隠さない。
- Organicの自動・手動選択、最終ピン集約、配置最適化の除外を解除した。手動Organicのモデル衝突も除外し、再生成・プレート全体の干渉確認と既存の棄却理由保持へ接続した。
- GUIの出力警告は最初の1行だけを表示するため、棄却理由を理由ごとのピン一覧としてその行へまとめた。Organic未対応警告は削除した。

追加した型は内部データのみで、設定キー、ピンJSON、G-codeコメント契約、引き出し系列、プレビュー形式は変更していない。NormalのDefaultはGrid、TreeのDefaultはOrganicを維持する。機械座標変換は既存DynaPin APIへ集約した。

### Releaseビルド・自動テスト

```sh
cmake --build build/arm64 --config Release \
  --target fff_print_tests libslic3r_tests OrcaSlicer --parallel 2

build/arm64/tests/libslic3r/Release/libslic3r_tests.app/Contents/MacOS/libslic3r_tests \
  '[DynaPin],[DynaPinPlacement]' --reporter compact

build/arm64/tests/fff_print/Release/fff_print_tests.app/Contents/MacOS/fff_print_tests \
  '[DynaPin]~[.]' --reporter compact
```

Releaseの全3ターゲットはビルド成功。libslic3rは**32ケース・157 assertionsすべて成功**。枝欠落対策を含むfff_print全体は**40ケース中39成功、4299 assertions中4298成功**。その後の警告本文修正と対応テストは、Organic・層厚境界の**4ケース・348 assertionsすべて成功**だった。重複するテストなので単純合算しない。

残る失敗は既存の`DynaPin Tree selects pins after applying an optimized pose`で、期待3本に対して1本となる。保存済み`294c1c693d`テストバイナリでも同じ失敗を再現した。期待値を変更して通す修正はしていない。

主な検証はDefault／明示Organic、自動／手動、7.3・14.7・22.1mm、独立／固定層、最小層厚・丸め境界、底面インターフェース有無、二段張り出し、Normalとの混在、複数インスタンス、移動・回転・設定変更後の再スライス、配置評価と最終体積の一致、並列角度評価、採用ピンごと1回のPullを含む。

OrganicとSlimでは接触境界が異なる。小さなfixtureのOrganicには22.4mmのroof境界があり、最小層厚0.2mmを保ったまま22.5mmのピン境界を挿入できない。この場合はLayerHeightで棄却して通常サポートを残す。高さを丸めて採用しない。

既存テストの未初期化プレート原点と、配置APIへ渡すEigenベクトルの`{}`を`Vec3d::Zero()`へ変更した。製品のCLI・GUIは実際のプレート原点を渡している。小さな配置テストは候補を限定して生成・評価・結果適用・最終スライスを検証するが、製品の探索点数・範囲・採点方法・対象インスタンス制約は変更していない。

### 最終CLI比較（配置最適化無効）

元モデルの一時コピー、Tree(auto)、stage 2、0.3mm設定で比較。サポート相当量はSupport＋Support interface、時間は総造形時間の推定値。

| モデル／条件 | サポート相当量 (g) | 総推定時間 (s) | Pull／ピン |
| --- | ---: | ---: | --- |
| Arch Bridge・Default・無効 | 10.365658 | 10728.300781 | 0本 |
| Arch Bridge・Default・有効 | 9.975605 | 10649.615234 | 1本: `0,2` |
| Arch Bridge・Organic・有効 | 9.126270 | 10463.639648 | 3本: `0,2 2,6 2,8` |
| RemoveGymnastBase・Default・無効 | 1.990335 | 2493.868896 | 0本 |
| RemoveGymnastBase・Default・有効 | 0.235090 | 2174.905273 | 2本: `8,9 8,10` |

この最終ペアのArch Bridge・Defaultで材料は約3.76%減、総推定時間は約0.73%減。Gymnast系は材料約88.2%減、総推定時間約12.8%減。原本`gymnast.3mf`がないため`models/RemoveGymnastBase/PLA.3mf`を代用した。原本の検証済みとは扱わない。

同じArch Bridge入力の反復では、採用は1〜3本、材料は約9.0〜10.0gに変動した。Default／明示Organicは同じ生成器を通るが、実モデルで毎回同じ集合になる条件は満たせていない。小さなfixtureでの両スタイルの一致と、実モデルの反復一致を区別する。初期の3本・約8.995gでは材料約12.3%減だったが、固定の削減率として採用しない。Slimの7本も期待値として使わない。

実装途中の5本・約7.41gは、押出幅を含む初層経路のはみ出し検証前の結果なので、最終結果から除外した。

### 配置最適化の実モデル検証

Arch BridgeのDefaultで配置最適化を有効にし、製品の探索点数・範囲を変更せずCLIを実行した。粗探索72角度グループ、詳細探索27角度グループ、計1,320候補を約32分35秒で評価し、`Improved`で終了した。回転180度・Y移動−1.10341mm、評価指標の支持領域体積は10,823.8mm³だった。この指標は支持領域の幾何体積であり、押出材料の体積とは異なる。

適用後の最終スライスも成功し、`0,2 2,4 2,8`の3本にPullが各1回だけ出力された。サポート相当量9.520227g、総推定時間10544.045898秒。無効時の通常配置に対して材料約8.16%減、時間約1.72%減だった。配置最適化なしの最終Default結果9.975605gより材料は約4.57%少ないが、Organicの反復変動があるため、探索効果の固定値とはしない。

着地初層の底面は7.299996mm／22.099985mmで、GUI出力と同様にコメント丸めを含む。粗いXY矩形による高さ交差は0件、生成器内の最終押出・接続・ブロッカー検証も通っている。探索中の必要枝欠落ガードの発動は0件だった。

候補評価と最終スライスは同じOrganic生成処理と着地検証を通る。小さなfixtureでは評価体積と最終体積の一致を自動確認した。一方、この実モデルの探索ログは勝者のピン集合を保存しておらず、最終集合との完全一致は証明していない。再生成による変動もあるため、実モデルでの評価・最終集合の厳密な一致は残る検証事項とする。

### GUI・G-code確認

最終アプリへ再起動し、GUIが埋め込みプリセットから復元するNormal・配置最適化有効を、Tree(auto)・Default・配置最適化無効へ設定して検証した。一時設定を`/tmp/dynapin-organic-validation/gui-final-arch.3mf`へ保存し、再起動後も再スライスした。元モデル・ユーザープリセットは上書きしていない。

- Arch Bridge最終GUI: Pull events **2**、Blocked regions **2**。サポート8.93g＋インターフェース0.76g、約9.69g。Defaultプレビューをアプリに残した。
- RemoveGymnastBase: Pull events **2**、Blocked regions **2**。サポート約0.20g＋インターフェース約0.04g。
- 出力警告で層厚制約・着地なし・ブロッカー干渉・上部接続なしの各理由とピン一覧が表示されることを実際に確認した。

最終GUI出力`gui-warning-final-arch.gcode`には`0,2`・`2,8`のPull開始コメントが各1回だけ存在した。初層底面は7.299996mm・22.099985mmで、上面7.3mm・22.1mmとの差はZ／HEIGHTコメントの桁丸めによる。インプロセスでは既存EPSILON以内の一致を必須とする。粗いXYバウンディングボックスによる高さ交差検査は0件。実際の幅・接続・ブロッカーは生成器内のポリゴン検証で扱う。

### 無効時の基準比較・残る制約

実装前に保存した`294c1c693d`のReleaseバイナリを、実際のResourcesへリンクした一時アプリ構成で実行し、最終版と同環境・同入力で比較した。

- RemoveGymnastBaseは材料1.990335302742294g・2493.868896484375sが一致し、コメントを除く152,774コマンドも全一致した。生ファイルのバイト一致ではない。
- Arch Bridgeは基準10.359147g・10735.730469s、新版10.365658g・10728.300781s。基準バイナリ自体の反復でも10.289981〜10.350328gと揺らいだ。これだけで今回の無影響を証明できず、**無効時のバイト一致は未確認**とする。

出力衝突領域を枝ごとに再計算せず試行内で再利用する前後各3回では、初回Drawing時間中央値375.053ms→340.768ms。ただし枝の揺らぎ・同時実行負荷があり、安定した改善率とは扱わない。

実モデルの再現性、原本`gymnast.3mf`、実機の引き出し・定着・たわみ・ファン干渉・造形成功は未検証または残る課題。ソフトウェアの着地検証と実機成功を分けて扱う。

別件のAuto Drop関連のModel・3MF・GUI変更と、`scripts/prepare_dynapin_3mf.py`が同じ作業ツリーに加わっている。それらは削除せず共存状態でビルドした。Organic変更とは区別する。ユーザーの禁止指示後はサブエージェントを使用せず、自身で差分・座標・無効時経路・並列状態・契約を確認した。

### 検証ログ（一時ファイル）

- `/tmp/organic-warning-final-build.log`
- `/tmp/organic-branch-acceptance-{fff,libs}.log`
- `/tmp/organic-warning-final-tests.log`
- `/tmp/organic-baseline-existing.log`、`/tmp/organic-baseline-bounded.log`
- `/tmp/dynapin-organic-validation/accepted-*/plate_1.gcode`
- `/tmp/dynapin-organic-validation/paired-{baseline,current}-*/plate_1.gcode`
- `/tmp/dynapin-organic-validation/gui-warning-final-arch.gcode`
- `/tmp/dynapin-organic-validation/accepted-arch-placement-on.log`

これらはOSによる削除対象。主要な数値・条件・制約は本文へ転記した。
