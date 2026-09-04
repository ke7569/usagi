# SSE 棰勬祴+绛栫暐+TD 鍚堝苟浜ゆ槗涓荤▼搴?鈥?瀹炴柦璁板綍 (2026-09-02)

## 鏋舵瀯缁撹
鐢熶骇浜ゆ槗涓荤▼搴忓湪 Deepwin 鏍?`/home/zane/sze-t0-feature-sse-t0-latest`锛?- `main`锛圖eepwin锛夊姞杞?`libt0_strategy_sse.so`锛坄sse_get_obj` -> `StrategyBase`锛夈€?  `libsse_md.so`锛堣鎯?vmd 89锛夈€乣libsse_td.so`锛圱D vtd 190锛夈€?- `StrategyBase` 宸插湪浜ゆ槗杩涚▼鍐呴儴鐩存帴杩愯 Snapshot/閫愮瑪娣峰悎妯″瀷锛?9:30-09:35 Snapshot銆?  09:35 璧烽€愮瑪锛夛紝骞舵妸棰勬祴缁忓唴瀛樺璞℃淳鍙戠粰姣忓彧鑲＄エ鐨?`ZStrategy`锛屼笉浜х敓棰勬祴 CSV銆?
## 鏈鏂板锛堢己鍙ｈˉ榻愶級
1. **浜ゆ槗杩借刀闂ㄧ** `src/t0-main/strategy/sse_trading_gate.h`
   - 浜ゆ槗鎵€鏃堕棿锛圡arketTime HHMMSSmmm锛変笌鏈湴澧欎笂鏃堕棿姣旇緝锛涘樊鍊?> 闃堝€硷紙榛樿 1s锛夌姝笅鍗曪紝鎭㈠鍒?1s 鍐呰嚜鍔ㄦ仮澶嶃€?   - 鐘舵€佸垏鎹紙blocked/resumed锛夐€氳繃 sink 璁板綍锛沗ZStrategy::insertOrder` 鍦?SSE 璺緞涓己鍒跺厛杩囬棬绂侊紝琚嫤鏃跺啓 `[CatchUpGate]`/`[OrderBlocked] reason=catchup_gate`銆?   - 閰嶇疆锛歚sse_order_routing.catchup_gate_enabled`锛堥粯璁?true锛夈€乣catchup_gate_threshold_ms`锛堥粯璁?1000锛夈€?2. **寮傛棰勬祴鏃ュ織** `src/t0-main/strategy/async_prediction_log.h`
   - 鍚庡彴鍗曠嚎绋嬫妸棰勬祴琛岃拷鍔犲啓鍏ユ棩蹇楁枃浠讹紙涓嶅啓 predictions.csv / tick_predictions.csv锛夈€?   - 瀛楁锛歝ode, exchange_us, local_us, model(snapshot/tick), prediction, selected, factor_complete, factor_ns, infer_ns, strategy_ns銆?   - 鎺ュ叆 `StrategyBase::process_sse_hybrid_snapshot` 涓?`flush_sse_hybrid_tick`锛涢厤缃?`sse_prediction_log_path` 闈炵┖鏃跺惎鐢ㄣ€?3. **瀹夊叏**锛氫笉鏀?capture 鐨?CPU/缃戝崱/缁勬挱/瑙ｆ瀽锛涚湡瀹炴姤鍗曚粛璧?`sse_order_routing` 鍙岄挜鍖?+ `SSE_ENABLE_LIVE_ORDER`锛屾棤闅愬紡涓嬪崟璺緞銆?
## 娴嬭瘯锛坆uild-sse锛?鏂板骞舵敞鍐岋細
- `sse_trading_gate_test`锛欻HMMSSmmm鈫掑井绉掋€侀棬绂佹粸鍚庛€乥locked/resumed 鐘舵€佸垏鎹€佺鐢ㄩ棬绂併€侾ASS銆?- `async_prediction_log_test`锛?00 琛屽紓姝ヨ拷鍔犮€侀『搴忎笌鍐呭鏍￠獙銆侾ASS銆?`ctest`锛?4/15 閫氳繃锛沗sse_udp_observer_offline_test` 鍥犱富鏈虹己 `/usr/bin/python3.6m` 鏈繍琛岋紙鏃㈡湁鐜闂锛岄潪鏈鏀瑰姩锛夈€?
## 鏋勫缓浜х墿
`build-sse/libt0_strategy_sse.so` sha256 `c6a3eef9d815bbc4e312f3ebd11a053fcc09c4f04e621eba6659211ea6fa82d8`锛屽鍑?`get_obj`銆乣sse_strategy_build_id`銆?
## 鍚姩锛堜袱杩涚▼锛?- 琛屾儏锛歝apture/parser锛堜繚鎸佺幇鐘讹紝鏈敼锛夈€?- 浜ゆ槗锛歚deploy/sse_live_order_ready_20260827/run_sse_t0_staged.sh`锛堝姞杞?main + 涓変釜 .so锛夛紱棰勬祴鍦?main 鍐呴儴瀹屾垚锛屼笉鍐嶆湁鐙珛棰勬祴杩涚▼涓庨娴?CSV銆?
## 鏈獙璇?闃诲锛堜笌涔嬪墠楠屾敹涓€鑷达紝闈炰唬鐮侀棶棰橈級
- 鏈満鏃?root-only TD 璐﹀彿閰嶇疆涓?broker `client_feature_code`锛汥eepwin `main` 缂?2 涓?boost 1.62 搴擄紙libboost_python3 / libboost_unit_test_framework锛夈€傛晠鏈兘瀹炵洏绔埌绔窇鈥滈娴嬧啋绛栫暐鈫扵D 鍥炴姤鈥濄€?- 琛屾儏鐨勭湡瀹炵粍鎾?缃戝崱鐜鍦ㄧ洰鏍囨満锛涙湰鏈烘湭鍋?live 鍐掔儫锛屾湭鍋滄/閲嶅惎浠讳綍 capture 杩涚▼銆?