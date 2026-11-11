# ConnectionProfile / ProfileStore ホストテスト

プロジェクト直下で以下を実行します。g++ と Bash のみで動き、Arduino / ESP-IDF / 外部ライブラリは不要です。実コードの `src/ConnectionProfile.cpp` と `src/ProfileStore.cpp` を fake `Preferences.h` とリンクします。ビルド成果物は一時ディレクトリに作り、終了時に削除します。

```bash
./tests/run_profile_tests.sh
SANITIZE=1 ./tests/run_profile_tests.sh  # AddressSanitizer + UndefinedBehaviorSanitizer
CXX=clang++ ./tests/run_profile_tests.sh # 別の互換C++コンパイラも指定可
```

12テストグループは、終端・バイト長・UTF-8 SSID・WiFi/VNCパスワード・IPv4/裸IPv6/DNSホスト名、8slot CRUD、再読込、lastUsed差分書込み、移行マーカー、全削除後の再seed防止、破損/未知version隔離と明示復旧、容量不足、短いread/put、read-only、open/remove/metadata書込み失敗、境界値、正規化serializationを検査します。テスト出力にはプロファイル値や秘密を出しません。

## 呼出し側の契約

`load()`が成功したら、`available()`は読出し可能を示します。read-onlyでも成功するので、書込み可否は各変更APIの戻り値で判断します。破損slotや不正メタデータがあっても有効slotを読み出し、`lastError()`に値を含まない警告を残します。`corruptCount()`は破損slot数だけです。開けない場合は`load()==false`・`available()==false`で、直前のRAMスナップショットは保持します。

legacy移行は`available() && !initialized()`の場合だけ行います。既存slotキー（有効・破損・未知version）、既存init/lastメタデータの存在でも初期化済みと判断し、破損を新規インストールと混同しません。`save()`はinitマーカーを先に書き、`markInitialized()`はlegacy設定がない場合にも呼べます。最後のslotを削除してもマーカーを残します。

`get()`は未使用・破損・範囲外でnullptr、`isCorrupt()`は範囲外でfalseです。破損slotは自動消去せず、`findFreeSlot()`もそのslotを予約扱いにします。UIは`isCorrupt(slot)`を表示し、確認後の`remove(slot)`または明示的`save(slot, profile)`で復旧できます。空slotの`remove()`は、読書き可能時に成功する冪等操作です。

すべての呼出しは単一所有者で直列化してください。UIとVNCtaskから同時に呼ばないでください。VNC成功をmainへ通知し、mainが`markLastUsed(slot)`を呼ぶ設計が適合します。各操作のPreferencesハンドルは成功/失敗を問わず`end()`されます。同じlastUsedの書込みは省略します。

## 永続化と障害時の注意

namespaceは`tab5vnc`、slotキーは`p0`〜`p7`、独立メタデータキーは`init` (U8) と`last` (I8、-1が未選択) です。blob v1は12byteヘッダー（magic `T5VP`・version・reserved・payload長・CRC32）と7byteフィールド情報＋終端を含まない文字列で、整数はlittle-endianです。最大blobは439byte。structパディングや終端後の不要バイトは保存しません。

更新失敗時には旧RAMプロファイル・used・lastUsedを維持します。ただし複数キーのtransaction保証はありません。初回saveでinit成功・blob失敗ならinitは残ります。選択中slotのremoveはinit確認→last=-1→blob削除の順なので、last成功・削除失敗時は旧RAM状態を保持しつつ、再loadするとlast=-1となり得ます。実NVSのcommit失敗は永続状態が不確定なこともあり、fakeはその電源断/不確定commitを再現しません。勝手なNVS全消去は行いません。

`freeEntries()`は上書き時も新blobコピー分を保守的に事前確認しますが、ページ断片化・GC・他namespaceによる利用を完全には保証しません。最終的には`putBytes()`の返却長を検査します。0x5000 NVSを想定した容量テストは8件最大blobの繰返し更新を確認しますが、実機のページ配置/電源断テストの代替ではありません。

CRCは破損検出用で、暗号化/改ざん耐性ではありません。パスワードはNVS暗号化を有効にしない限り平文です。検証モジュールとストアは秘密をログへ出しません。
