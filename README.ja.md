# zmk-driver-zw3021

[English](README.md) | 日本語

HLK-ZW3021指紋センサー用のZMK外部モジュールドライバ。

## ステータス

Phase 1（自動照合）、Phase 3（登録/削除/消去）、Phase 2（照合成功時の
指紋IDごとのキー入力出力）、Phase 4（Web Serial / Web Bluetoothブラウザ
UI、`docs/index.html`）を実装済み:

```text
指を置く → INT立ち上がりエッジ → VCC-D ON → 起動確認 → PS_HandShake
→ PS_AutoIdentify (1:N) → 結果をログ出力 → 一致したIDの保存文字列(あれば)
をタイプ出力 → VCC-D OFF → INTがLOWに戻るのを待つ → 再アーム
```

```text
&zw3021_enroll <id> / &zw3021_delete <id> / &zw3021_clear (キーマップbehavior)
→ 同じワーカースレッドにキュー → VCC-D ON → PS_HandShake
→ PS_AutoEnroll / PS_DeleteChar / PS_Empty → 結果をログ出力 → VCC-D OFF → 再アーム
```

**未実装**: バッテリー消費最適化、複数センサー対応。「Roadmap」参照。

## ハードウェア

ZW3021は（メインのキーボードPCB上ではなく）独立した6ピン指紋モジュール
基板として使用し、`VCC-D`専用のロードスイッチを持つ。**このロードスイッチ
自体にも小さな専用/自作基板（または同等の手配線回路）が別途必要**で、
標準のmoNa2 PCBにはこの回路が無いため、ZW3021モジュール単体だけでは
配線できない:

```text
MCU 3V3
  ├─ ZW3021 VCC-S (常時ON)
  └─ ロードスイッチ IN
          └─ OUT → ZW3021 VCC-D (認証中のみON)
```

- `VCC-S`: センサー電源、常時供給が必要（3.15〜3.6V）。
- `VCC-D`: DSP電源。`power-en-gpios`（Active High）で駆動するロード
  スイッチによってゲートされる。センサー自身は自分の`VCC-D`を制御でき
  ない — ロードスイッチはホスト側にある。
- `INT`: 通常LOW、指がセンサーに乗っている間HIGHになる（立ち上がり
  エッジ=指置き）。`int-gpios`上のGPIO割り込みとして扱う。
- UART: **57600 baud、8データビット、2ストップビット、パリティなし
  (8N2)**。`uart_configure()`で実行時に強制設定されるため、ボードの
  デフォルトUART設定には依存しない。

## 配線（moNa2 Left側、`zmk-config-moNa2-v2`の`zw3021`ブランチ）

moNa2 Left側のdevicetreeと照合済み — この配線がどう導かれたかは
`documents/codex_zw3021_driver_spec.md`のセクション2.2を参照（エンコー
ダー本来のピンだけでは足りず、1本はボードのD-コネクタを経由せずXIAO
nRF52840モジュールへ直接配線している）。

| ZW3021ピン | XIAOピン | nRF GPIO | 信号 | 経路 |
|---|---|---|---|---|
| UART-RX | D5 | `P0.05` | `HOST_TX` | 周辺コネクタ（旧エンコーダーA相） |
| UART-TX | D0 | `P0.02` | `HOST_RX` | 周辺コネクタ（旧エンコーダーB相） |
| —（ロードスイッチEN） | D4 | `P0.04` | `FP_EN` | 周辺コネクタ（Left側では未使用） |
| INT | — | `P0.16` | `FP_INT` | XIAO nRF52840モジュールへ直接配線 |
| VCC-S | 3V3 | — | | 常時3.3V |
| VCC-D | — | — | | ロードスイッチOUT |
| GND | GND | — | | 共通グラウンド |

これにはLeft側のロータリーエンコーダー（`&left_encoder`）の撤去が必要。
撤去自体は単独で安全（ZMKのセンサーコードは`DEVICE_DT_GET_OR_NULL()`を
使うため、無効化されたセンサーノードは単にスキップされる — 他の
devicetree/keymap変更は不要）。

## Devicetreeの使い方

```dts
&pinctrl {
    uart1_default: uart1_default {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 5)>,   /* D5 */
                    <NRF_PSEL(UART_RX, 0, 2)>;    /* D0 */
        };
    };
    uart1_sleep: uart1_sleep {
        group1 {
            psels = <NRF_PSEL(UART_TX, 0, 5)>,
                    <NRF_PSEL(UART_RX, 0, 2)>;
            low-power-enable;
        };
    };
};

&uart1 {
    status = "okay";
    compatible = "nordic,nrf-uarte";
    current-speed = <57600>;
    pinctrl-0 = <&uart1_default>;
    pinctrl-1 = <&uart1_sleep>;
    pinctrl-names = "default", "sleep";

    zw3021: zw3021 {
        compatible = "razilyis,zw3021";
        status = "okay";

        int-gpios = <&gpio0 16 (GPIO_PULL_DOWN | GPIO_ACTIVE_HIGH)>;
        power-en-gpios = <&gpio0 4 GPIO_ACTIVE_HIGH>;

        power-on-delay-ms = <200>;
        startup-timeout-ms = <500>;
        identify-timeout-ms = <12000>;
        score-level = <3>;
    };
};

&left_encoder {
    status = "disabled";
};
```

`CONFIG_ZW3021_STORAGE`を使う場合は、専用のflashパーティションも切り出す
（ボードごとに1回でよく、センサーインスタンスごとではない）:

```dts
&flash0 {
    partitions {
        compatible = "fixed-partitions";
        #address-cells = <1>;
        #size-cells = <1>;

        /* 下のzw3021_partitionのスペースを空けるため縮小。元のサイズ/
         * オフセットは自分のボードのflashレイアウトに合わせて調整。 */
        code_partition: partition@27000 {
            reg = <0x00027000 0x000bd000>;
        };

        zw3021_partition: partition@e4000 {
            label = "zw3021_storage";
            reg = <0x000e4000 0x00008000>;
        };

        storage_partition: partition@ec000 {
            label = "storage";
            reg = <0x000ec000 0x00008000>;
        };
    };
};
```

### Devicetreeプロパティ

| プロパティ | 必須 | デフォルト | 説明 |
|---|---|---:|---|
| `int-gpios` | 必須 | — | INTピン、立ち上がりエッジ=指置き |
| `power-en-gpios` | 必須 | — | VCC-Dロードスイッチのenable、Active High |
| `power-on-delay-ms` | 任意 | 200 | VCC-D ON後、起動待ち開始までの遅延 |
| `startup-timeout-ms` | 任意 | 500 | センサーの`0x55`起動バイトの最大待ち時間 |
| `identify-timeout-ms` | 任意 | 12000 | PS_AutoIdentifyのホスト側上限 |
| `score-level` | 任意 | 3 | PS_AutoIdentifyの照合スコアレベル (1〜5) |
| `enroll-times` | 任意 | 3 | PS_AutoEnrollがID毎に要求する指のキャプチャ回数 |
| `enroll-timeout-ms` | 任意 | 60000 | PS_AutoEnroll全体のホスト側上限 |

## 登録/削除/消去behavior

3つの`BEHAVIOR_LOCALITY_GLOBAL`キーマップbehaviorを用意しており、split
構成の共有keymapのどこにでもバインドできる。`CONFIG_ZW3021`が無い側
（例: BLE central）ではno-opになる。

**devicetreeノード名は8文字以内に収めること。** BLE splitの「run
behavior」特性は、デバイス名を`char behavior_dev[ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN]`
（`ZMK_SPLIT_RUN_BEHAVIOR_DEV_LEN`は9、
`zmk/app/include/zmk/split/bluetooth/service.h`）に詰め込み、
`DEVICE_DT_NAME()`は別途`label`プロパティが無ければノード自身の名前に
フォールバックする。長すぎる名前は黙って切り詰められ（`Truncated
behavior label ... before invoking peripheral behavior`とログ出力）、
peripheral側はそれを解決できず、リクエストは永遠に届かない。phandle
ラベル（keymapで`&zw3021_enroll`等として使うもの）は、送信されるのは
ノード名自体だけなので、分かりやすい名前のままでよい:

```dts
zw3021_enroll: zwenroll {
    compatible = "razilyis,zw3021-enroll";
    #binding-cells = <1>;
};
zw3021_delete: zwdelete {
    compatible = "razilyis,zw3021-delete";
    #binding-cells = <1>;
};
zw3021_clear: zwclear {
    compatible = "razilyis,zw3021-clear";
    #binding-cells = <0>;
};
```

Keymapでの使い方: `&zw3021_enroll 1`（ID 1として登録）、
`&zw3021_delete 1`（ID 1を削除）、`&zw3021_clear`（データベース全消去）。
それぞれセンサーのワーカースレッドにリクエストをキューして即座に戻る。
既に実行中のリクエストがある間に来たリクエストは、キューされずに警告
ログ（`-EBUSY`）付きで破棄される。busy状態はセンサー操作自体が完了した
時点で解除される — その後ドライバが指が離れる（INTがLOWに戻る）のを
待っている間でも新しいリクエストはキューでき、再アーム完了直後に
処理される。

## 指紋IDごとのキー入力出力

照合に成功すると、ドライバはその指紋IDに対応する保存済み出力文字列を
検索し、タイプ出力する — **文字列がgitやコンパイル済みファームウェア
イメージに触れることは一切ない**。これを実現するのは以下の2つの仕組み。

### 1. 仮想出力キーボード（`CONFIG_ZW3021_STORAGE`）

タイプ出力はBLEの**central**側でしか行えない（HIDレポートを送れるのは
centralだけ）が、センサーはperipheral側にある。登録/削除/消去behavior
（central→peripheral方向）と対称的に、この方向（peripheral→central）
は既存の別の無改造ZMK機構を再利用する: peripheral上でローカルに
`zmk_position_state_changed`イベントを発生させると、ZMK自身の
`split_peripheral_listener`（`zmk/app/src/split/peripheral.c`）が
自動的にcentralへ転送する — 実際のキーマトリクス押下がすでに使っている
のと同じ経路（`zmk/app/src/physical_layouts.c`）。

これには、実キー数を超えて予約した54個の**仮想キー位置**が必要
（`0-9`/`a-z`の英数字36個、対応記号16個
`! @ # $ % ^ & * ( ) - _ = + . ,`（`.`と`,`はメールアドレス用に追加）、
LSHIFT 1個、Enter 1個）。それぞれ`default_layer`で対応する`&kp`
キーコードにバインドし、他の全レイヤーでは`&trans`にする —
`zmk-config-moNa2-v2`の`boards/shields/mona2/mona2.dtsi`の`RC(4,N)`
トランスフォームエントリ（行4は実kscanハードウェアには存在しない、
物理行は0〜3のみ）と、`mona2.keymap`の各レイヤーのbindingを参照。
大文字は仮想LSHIFTキーを押しっぱなしにして打ち分ける（`src/zw3021.c`の
`zw3021_char_to_offset()`/`zw3021_emit_char()`参照）。記号・スペースは
上記の対応記号以外は未対応。最後から2番目（LSHIFT）の次の
位置（Enter）は、その指紋IDに「Enterも送信する」フラグが有効になって
いる場合にのみ、文字列の後に押される（`zw3021_storage_get_enter()`、
下記のシリアルRPCの`set_finger_enter`コマンドでID毎に設定） —
パスワードマネージャーのマスターパスワード欄でフォーム送信もしたい、
といった用途に便利。

文字列自体は専用のflashパーティション（`zw3021_partition`、上記
devicetreeセクション参照）上のNVSに、指紋IDをキーとして`src/storage.c`
経由で保存される — `zmk-module-Fingerprint/src/storage.c`と同じパターン。
「Enter送信」フラグと、非機密の指ごとの表示名（`set_finger_name`、
後述）は、指紋IDと衝突しない別のキー範囲で同じNVSインスタンスに保存
される。

**出力文字列は保存時に暗号化される**（AES-128-CTR、tinycrypt経由 --
`CONFIG_ZW3021_STORAGE`が`CONFIG_TINYCRYPT_AES`/`_AES_CTR`/`_SHA256`を
選択）。鍵は起動のたびにチップ固有のハードウェアID
（`hwinfo_get_device_id()`）と固定salt（`src/storage.c`の
`ZW3021_STORAGE_KEY_SALT`）をハッシュして導出し、flashに書き込まれる
ことも、どこかへ送信されることもない。これは紛失・廃棄されたボードの
フラッシュを単純に読み出すカジュアルな攻撃は防ぐが、**実行中のチップ
にSWDデバッグで生接続してくる攻撃者は防げない** — 鍵導出ロジックは
公開ソースなので、チップのハードウェアIDを読み取れる相手なら鍵を
再現できる。nRF52840にはセキュアエレメント（TrustZone/CryptoCell）が
無く、ハードウェアAES ECBペリフォラルはBLEコントローラーが占有して
いる（Zephyrの`drivers/crypto/Kconfig.nrf_ecb`の`depends on !BT_CTLR`）
ため、ハードウェアではなくtinycryptのソフトウェアAESを使っている。
「Enter送信」フラグと表示名は機密ではないため平文で保存される。

### 2. RPCコマンドコア（`src/rpc_commands.c`）＋2つのトランスポート

そのNVSパーティションに文字列を入れる唯一の方法はJSON RPCプロトコル
経由 — コマンドディスパッチ自体（`src/rpc_commands.c`）はトランスポート
非依存で、有効化できる2つの独立したトランスポートで共有される:

- **`CONFIG_ZW3021_SERIAL_RPC`**（`src/serial_rpc.c`）: ロギングに
  既に使っているUSB CDC-ACMストリーム上の行ベースコンソール
  （`zmk-usb-logging`スニペットが必要）— `zmk-module-Fingerprint/src/serial_rpc.c`
  を参考にした。
- **`CONFIG_ZW3021_BLE_RPC`**（`src/ble_rpc.c`）: 独自のBLE GATT
  サービス。USBケーブル不要、ZMK Studioやcentral半分へのsplitリンクへの
  依存も無しでブラウザからセンサーを設定できる — split接続との共存
  方法は下記「Standalone BLE RPC」参照。

どちらも同じフラットな（ネストなしの）JSON形式を使うため、ブラウザUIは
バックエンドの書き換えなしにどちらとも話せる:

```text
Request:  {"cmd":"<name>","req_id":<int>,"finger_id":<int>,"value":"<str>","enter":<bool>}
Response: {"ok":true,"req_id":<int>,"data":{...}}
       or: {"ok":false,"req_id":<int>,"message":"..."}
```

| コマンド | パラメータ | 備考 |
|---|---|---|
| `ping` | — | |
| `get_status` | — | `data.busy` |
| `get_fingers` | — | `data.ids`: 文字列が保存されているIDの配列 |
| `get_finger` | `finger_id` | `data.has_value`、`data.enter`、`data.name` -- **文字列自体は絶対に返さない**が、`name`（後述）はそのまま返す |
| `update_finger` | `finger_id`, `value` | 文字列を書き込み/上書き（書き込み専用）。最大31文字（`ZW3021_STORAGE_MAX_LEN - 1`）、超過は`value too long`エラーで拒否される |
| `delete_finger` | `finger_id` | 「Enter送信」フラグも消す；表示名は**消さない** |
| `set_finger_enter` | `finger_id`, `enter` | 文字列とは独立に、ID毎の「Enter送信」フラグを切り替え |
| `set_finger_name` | `finger_id`, `name` | 文字列とは独立に、スロットの非機密な表示ラベルを設定（UTF-8、`ZW3021_STORAGE_NAME_MAX_LEN - 1`バイトまで、超過は`name too long`エラーで拒否される） |
| `delete_template` | `finger_id` | `zw3021_request_delete()`をラップ -- センサー上で`PS_DeleteChar`を実行し、そのIDに登録された指紋テンプレートを完全に削除する（取り消し不可；NVS文字列のみ触る`delete_finger`とは別物） |
| `enroll_start` | `finger_id` | `zw3021_request_enroll()`をラップ |
| `enroll_status` | — | `data.busy` |
| `refresh_enroll_map` | — | `PS_ReadIndexTable`センサークエリ(ID 0-255)をキューする；`get_status`がbusyでなくなるまでポーリングしてから`get_enrolled`を呼ぶ |
| `get_enrolled` | `finger_id` | `data.valid`（起動後にビットマップが読まれたか）、`data.has_template` -- 直近の`refresh_enroll_map`結果からの値で、その場でセンサーに問い合わせるわけではない |

**`get_finger`は実際の文字列に関して意図的に書き込み専用。** このコンソール
には認証が無い — 物理的なUSBアクセスさえあれば誰でもRPCコマンドを送れる
— そのため保存値をそのまま返す設計にすると、センサーに一切触れずに全
指紋の出力文字列を読み出せてしまい、照合の背後に置いた意味が失われる。
`get_finger`は値の有無とenterフラグだけを報告し、`update_finger`は
それでも中身を見ずに上書きできる。`docs/index.html`もこれを前提に
作られている（「編集」フローは常に空欄から始まる）。`data.name`だけは
例外で、単なるスロットのUIラベル（例:「会社PCログイン」）でありタイプ
出力されず機密でもないため、自由に読み書きできる（文字列欄と違い、
`docs/index.html`の名前欄は現在の値をそのまま表示・編集する）。

シリアルトランスポート上では、ログとRPCレスポンスが同じストリームを
共有するため、混在する。ログに使っているのと同じシリアル端末に直接
行を入力してテストできる。例:

```text
{"cmd":"update_finger","req_id":1,"finger_id":1,"value":"hunter2"}
{"ok":true,"req_id":1,"data":{}}
{"cmd":"set_finger_enter","req_id":2,"finger_id":1,"enter":true}
{"ok":true,"req_id":2,"data":{}}
{"cmd":"get_finger","req_id":3,"finger_id":1}
{"ok":true,"req_id":3,"data":{"finger_id":1,"has_value":true,"enter":true}}
```

### 3. Standalone BLE RPC（`CONFIG_ZW3021_BLE_RPC`）

指紋センサーとそのNVSストレージはsplit peripheral側（Left）にしか
存在せず、ホストへの直接のBLE接続を持たない — 持っているのはcentral側
（Right）へのZMK自身のsplitリンクのみ。centralを経由してリクエストを
中継する（未検証の新しいsplitトランスポート基盤が必要になる — この
経路を却下した調査は`documents/codex_zw3021_driver_spec.md`セクション13
参照。ZMK Studio統合を検討して断念した経緯も含む）代わりに、
`src/ble_rpc.c`は**peripheral側で直接、2本目の独立したBLE接続**を
動かす: 独自のGATTサービス（ZMK Studioとは無関係の、独自にランダム
生成したUUID）、独自のextended-advertising set。ZMK自身のsplitリンク
用advertising・接続とは完全に別に、並行して動く。
`zmk/app/src/split/bluetooth/peripheral.c`は無改造。

これにはperipheral側で2本目の同時BLE接続と2本目のadvertising setの
ための余裕が必要:

```conf
CONFIG_BT_MAX_CONN=2               # 既存のsplitリンク + この接続
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2    # 既存のsplit advertising + これ
CONFIG_BT_MAX_PAIRED=2             # centralとの既存ボンド + これ
CONFIG_BT_USER_DATA_LEN_UPDATE=y   # 通知ペイロードを大きくする
CONFIG_BT_CONN_TX_MAX=64           # 下記参照
```

`CONFIG_BT_MAX_PAIRED`は見落としやすい: peripheral側には
`ZMK_SPLIT_ROLE_CENTRAL`固有の上書き設定が無い（それはcentral側にしか
適用されない）ため、Zephyr標準のデフォルト値**1**のままになり、
既にcentralとのボンドで埋まっている。増やさないと、ブラウザとの
ペアリング自体は成功したように見えるが、通知を有効にするために必要な
暗号化CCC書き込みが「GATT operation failed for unknown reason」という
汎用エラーで失敗する（実機で確認済み）。

`CONFIG_BT_CONN_TX_MAX`も見落としやすい: デフォルトのTXバッファプールは
たまのトラフィック向けのサイズで、通知の多いRPCコンソール向けでは
ない。実機で確認: 数秒おきに`get_status`をポーリングするだけでこれを
使い切り（`bt_att: Ran out of TX buffers or contexts`）、その処理が
ZW3021自身のUARTハンドシェイクのタイミングを奪っているようで、
ブラウザ接続中に登録が壊れる原因になっていた。ZMK Studio自身のBLE RPC
トランスポートも全く同じ問題に当たり、同じ理由でこのKconfigオプション
を同じ値に増やしている。`docs/index.html`も、センサーのUARTタイミング
が最も重要になる登録試行の間は、背景の`get_status`ポーリングを止めて
BLEトラフィックを最小限にする。

トランスポートの堅牢性に関する詳細（すべて`src/ble_rpc.c`）: 通知の
チャンクサイズはネゴシエート済みの**ATT MTU**（から3バイトのヘッダを
引いた値）から決める — リンク層のデータ長とは独立に交渉されるもので、
リンク層の値からサイズを決めていた頃は、MTUがそれより小さいスタック
（例: macOS）で`-EMSGSIZE`により通知が失敗し、レスポンスストリームの
一部が欠落していた。通知のリトライは専用ワークキュー上で実行し、
バックオフのsleepがZephyr共有のシステムワークキューを停滞させない
ようにしている。受信側では、途中でバイトが届かなくなったリクエストは
5秒で破棄し、長すぎるリクエストは末尾の改行まで読み捨てる（シリアル
トランスポートのstale-partial破棄のBLE版）— これにより、不正または
過大なリクエスト1つが接続の残り全体のJSONフレーミングを壊し続ける
ことはなくなった。

両方のGATT特性は暗号化（ペアリング/ボンディング済み）接続を要求する
（`BT_GATT_PERM_*_ENCRYPT`）— RPCの書き込み専用設計と一貫性を持たせて
おり、目標は「まず物理デバイスとペアリングしない限り、誰も指紋の出力
文字列を読み書きできない」こと。初回接続時はOSレベルのBLEペアリング
プロンプトが出るはず。

## Web Serial / Web Bluetooth UI（`docs/index.html`）

自己完結型でビルド不要なHTML/JSページ（[docs/index.html](docs/index.html)）
が、上記RPCプロトコルのブラウザクライアントを実装しており、どちらの
トランスポートでも接続できる:
[Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
（USB）または
[Web Bluetooth API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Bluetooth_API)
（ケーブル不要）。どちらでもやることは同じ: 5つの固定指スロットの
どれに出力文字列が設定されているか確認する（上記の書き込み専用設計に
より文字列自体は絶対に見えない）、上書き/削除する、指ごとの「Enter
送信」フラグを切り替える、アニメーション付きモーダルで登録をトリガー
する — すべてChromeまたはEdgeのタブから、ソフトウェアのインストール
不要で行える。

動作上の注意:
- Bluetooth経由では、最初のスロット一覧取得（および登録/テンプレート
  削除ボタン）を「Standalone BLE RPC」で説明した約6秒のカウントダウン
  の後まで遅延させる: 一覧取得は実際のセンサーUART操作
  （`refresh_enroll_map`）を伴い、接続間隔の延長が効くまでは信頼できない
  ため。
- GATT書き込みは大きい（180バイト）チャンクから始め、スタックに拒否
  されたら自動的に20バイトチャンクへフォールバックする（Web Bluetooth
  はATT MTUを公開しない）。意図的に遅くしてある接続間隔の上でも
  コマンドの往復を短く保つため。
- 入力は送信前にファームウェアの保存上限で検証される: 出力文字列は
  対応文字セットで最大31文字、名前は最大47バイト（UTF-8）。
- バックグラウンドの一覧更新が編集中のフォーム欄を上書きすることは
  ない（書き込み専用の文字列欄には一切触れない）ため、保存直前に
  「Enter送信」のチェックや編集中の名前が黙って元に戻ることはない。

必要要件:
- Chrome または Edge 89以上（どちらのAPIもFirefox/Safariには実装されて
  いない）。
- [secure context](https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts):
  HTTPS、`localhost`、またはファイルを直接開く（`file://`）のいずれも
  Chromiumで動作する。`docs/`をGitHub Pages経由で配信する（リポジトリの
  Settings → Pages → deploy from branch、フォルダ`/docs`）のが安定した
  HTTPS URLを共有する一番簡単な方法だが、必須ではない — ファイルを
  ローカルで開くのでも動作する。
- シリアル経由では、RPCレスポンスはZMKのログ出力と同じUSB CDC-ACM
  ストリームを共有する（上記参照）；ページは有効なJSONでない行を無視
  するため、ログ行はデバッグパネルに表示されるがリクエスト/レスポンス
  プロトコルの邪魔にはならない。これはBluetoothパス（独自の専用GATT
  サービス）には当てはまらない。
- Bluetooth経由では、初回接続時にOSレベルのペアリングダイアログが
  出ることが多い（上記「Standalone BLE RPC」参照）。

このページはローカルのシリアルまたはBluetooth接続以外のどこにも何も
送信しない — サーバーコンポーネントも他のネットワークリクエストも
無いため、保存された文字列がデバイスとブラウザタブの外に出ることは
ない。

## Kconfig

```conf
CONFIG_ZW3021=y                 # センサー本体がある側のみ
CONFIG_ZW3021_ENROLL_BEHAVIOR=y # 全ての側（下記参照）
CONFIG_ZW3021_DELETE_BEHAVIOR=y
CONFIG_ZW3021_CLEAR_BEHAVIOR=y
CONFIG_ZW3021_STORAGE=y         # センサーがある側のみ
                                 # (TINYCRYPT_AES/_AES_CTR/_SHA256 + HWINFOを
                                 # 自動selectし、保存する出力文字列を暗号化する)
CONFIG_ZW3021_SERIAL_RPC=y      # センサーがある側のみ
CONFIG_ZW3021_BLE_RPC=y         # センサーがある側のみ（任意、下記参照）

# CONFIG_ZW3021_BLE_RPC=yの場合のみ必要 -- 上記「Standalone BLE RPC」参照
CONFIG_BT_MAX_CONN=2
CONFIG_BT_EXT_ADV_MAX_ADV_SET=2
CONFIG_BT_MAX_PAIRED=2
CONFIG_BT_USER_DATA_LEN_UPDATE=y
CONFIG_BT_CONN_TX_MAX=64
```

`CONFIG_ZW3021_ENROLL_BEHAVIOR` / `_DELETE_BEHAVIOR` / `_CLEAR_BEHAVIOR`は、
split構成の**全ての**側の`.conf`で`y`にする必要がある（`CONFIG_ZW3021`を
全く持たないBLE central側を含む）— ZMKは`BEHAVIOR_LOCALITY_GLOBAL`
behaviorを、ローカルでbehaviorデバイスをまず解決できた場合のみ他の
split側へ転送する；持っていない側は`No behavior assigned to <position>
on layer <N>`とログ出力し、一切転送しない。

**全てのperipheral呼び出しで出る無害なログノイズ。これが出ていても
正常に動作している:** peripheralは呼び出しが成功していても
`Unhandled command type 1`に続いて`Failed to invoke behavior <name>: -134`
をログ出力する。これは上流ZMKのバグ —
`zmk/app/src/split/peripheral.c`の
`zmk_split_transport_peripheral_command_handler`のswitch文にある
`INVOKE_BEHAVIOR`caseに`break;`が抜けているため、behavior自体が成功
したかどうかに関わらず常に`default:`にフォールスルーする（警告を
ログ出力して`-ENOTSUP`を返す）。本当の失敗は別のエラー（例:
`-22`/`EINVAL`、behaviorデバイスが見つからなかったことを意味する）で
見分けること。

## ビルド

このリポジトリを利用側configの`west.yml`にprojectとして追加し、対象
シールドの`.conf`ファイルで`CONFIG_ZW3021=y`を有効化する（センサーが
ある側のみ）。split構成では、上記の3つのbehaviorノードを全ての側で
共有されるdevicetreeファイル（センサーがある側だけでなく）に定義し、
上記Kconfigセクション通り3つの`CONFIG_ZW3021_*_BEHAVIOR=y`フラグを
全ての側の`.conf`に設定する。

## ログ出力例

```text
[INF] zw3021: initialized
[INF] zw3021: finger detected
[INF] zw3021: VCC-D enabled
[INF] zw3021: boot handshake byte received
[INF] zw3021: PS_HandShake OK
[INF] zw3021: identify started
[INF] zw3021: match id=3 score=87
[INF] zw3021: VCC-D disabled
```

登録（成功時）:

```text
[INF] zw3021: VCC-D enabled
[INF] zw3021: boot handshake byte received
[INF] zw3021: PS_HandShake OK
[INF] zw3021: enroll started, id=1 times=3
[INF] zw3021: enroll stored id=1
[INF] zw3021: VCC-D disabled
```

エラー時:

```text
[WRN] zw3021: startup timeout, trying handshake
[ERR] zw3021: handshake timeout/error: -116
[WRN] zw3021: no matching fingerprint
[WRN] zw3021: identify timeout
[ERR] zw3021: invalid checksum
[WRN] zw3021: enroll: feature generation failed, retrying capture
[WRN] zw3021: enroll: id already has a template
[WRN] zw3021: enroll failed: -5
[WRN] zw3021: busy, dropping request (type=0)
```

生の指紋画像・テンプレート・保存文字列のデータがログに出ることは一切
ない。

## 既知の制限事項

- センサーは1台のみ対応（Left+Rightの両側にセンサーを載せるデュアル
  構成は非対応）。
- 電力消費のチューニングは無し；`VCC-D`のゲーティングが唯一の省電力
  対策。
- `INT`はボード標準のD0〜D10ヘッダーではなく、XIAO nRF52840モジュールへ
  直接配線されている；書き込み前に自分のハードウェアに対してこの配線を
  確認すること。
- enroll/delete/clearリクエストは同時に1つしかキューされない；既に
  実行中の間に来たリクエストは（キューされず）破棄される（ログには
  残る）。
- `PS_AutoEnroll`のキャプチャ毎の応答は、部分的にしか文書化されていない
  （`zw3021_auto_enroll()`直上のコメント参照）。成功パス（登録→照合→
  一致）は実機で動作確認済み；各種失敗確認コード（ID重複、データベース
  満杯等）はプロトコルマニュアル通りに実装済みだが、個別には検証して
  いない。
- 出力文字列は大文字小文字（大文字を打つ間、仮想LSHIFTキーを押しっぱ
  なしにする）とUSキーボード上段記号`! @ # $ % ^ & * ( ) - _ = +`に
  加え`.`と`,`（メールアドレスを打つのに十分）に対応しているが、それ
  以外の記号やスペースは非対応；`src/zw3021.c`の
  `zw3021_char_to_offset()`/`zw3021_emit_char()`参照。これには
  zmk-config-moNa2-v2側で対応する54スロットの仮想キーボードが必要
  （元は36スロット、LSHIFT追加で38、記号14個追加で52、`.`/`,`追加で
  今は54）— その`mona2.dtsi`と両方の`mona2.keymap`ファイル参照。
- `get_fingers`は1〜100の範囲をスキャンして`nvs_read`で個別チェックする
  ことで保存済みIDを見つける — この規模では問題ないが、もっと大きな
  ID空間を列挙する方法ではない。
- Standalone BLE RPC（`CONFIG_ZW3021_BLE_RPC`）は接続後、センサー操作
  （登録）が安定するまで数秒かかる: ドライバは接続時にBLE接続間隔を
  大幅に延長するよう要求し（上記「Standalone BLE RPC」参照）、センサー
  のUARTタイミングを乱さないようにするためだが、この再ネゴシエーション
  自体に数秒かかる。`docs/index.html`はBluetooth接続直後の6秒間、登録
  ボタンを無効化し、最初のスロット一覧取得も安定後まで遅延させ、
  カウントダウンを表示してこれをカバーする；USBにはこの待機期間は
  無い。
- `enroll_status`はドライバがbusyかどうかだけを報告し、進行中の登録
  試行の成功/失敗の詳細は報告しない — Web UIは終わったことは分かるが
  成功したかどうかは分からない。`refresh_enroll_map`/`get_enrolled`
  （`PS_ReadIndexTable`）を使えば、デバイスログを見なくても、そのIDに
  実際にテンプレートが登録されたかを後から確認できる。
- `get_enrolled`のビットマップ↔ID対応のビット順（1バイト内でLSBが
  小さいID、すなわちbyte 0のbit 0 = ID 0）は、このセンサー系列の一般的
  な慣例からの推測であり、プロトコルマニュアルに明記されているわけでは
  ない — 広く信頼する前に、既知の登録済みIDと突き合わせて実際に合って
  いるか確認すること。
- 出力文字列の保存時暗号化（上記「指紋IDごとのキー入力出力」参照）は、
  カジュアルなフラッシュダンプ読み取りを防ぐだけで、実行中のチップに
  対するSWDデバッグによるライブな攻撃は防げない（鍵導出ロジックは
  公開ソース）。また移行パスも無い: この機能追加より前のファームウェア
  ビルドで保存された文字列は暗号化データとして誤読され（照合時に化けた
  文字列が出力される、クラッシュはしない）、一度`update_finger`で
  入れ直す必要がある。

## ロードマップ

```text
Phase 5: Splitキーボードのperipheral統合、central側イベント転送
```

このドライバの実装元になった完全なプロトコルリファレンスと実装計画は
`documents/codex_zw3021_driver_spec.md`（ローカルのみ、gitには含まれ
ない）を参照。
