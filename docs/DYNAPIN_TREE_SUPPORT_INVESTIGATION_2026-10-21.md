# Treeサポートが適切に反映されない問題の調査

調査日: 2026-10-21（JST）
初期調査対象ソース: `1b0f3d1e7d70cf880cfaf411625a80b3f9da747b`
対象モデル: `models/Arch_Bridge/PLA.3mf`
状態: 調査後、承認された修正を実装・検証済み。変更は未コミット。実機造形は未実施。第1〜7節は修正前の調査記録であり、修正後の結果は第8節を参照。

## 結論

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
