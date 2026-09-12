# Native Jingle calls: план продолжения для нового чата с Sol

Актуализировано 2026-09-12 по последнему аудиту. Это самостоятельное задание для продолжения существующей реализации, а не предложение спроектировать стек заново. Начать с восстановления HEAD/status и **нового P0**. Старые исправленные замечания не реализовывать повторно.

## 1. Контекст, репозитории и границы достоверности

Цель — защищённые одноранговые audio/video звонки Psi ↔ Psi и Psi ↔ Conversations. Единственный Jingle stack находится в Iris; Psi использует существующие AvCall/AvCallManager; media engine — psimedia. Никакого второго call/Jingle manager или возврата legacy transport stack.

| Репозиторий | Локальный путь | HEAD последнего аудита | Предыдущая точка сравнения |
| --- | --- | --- | --- |
| Iris | /home/silinykh/projects/psi/psi/iris | 36f9e3ac5e326633b56cd24c2d2c8dea4b638732 | 7c0acb0c7dc45a9173faba5521f551916e9d67d7 |
| Psi | /home/silinykh/projects/psi/psi | 250c1f6ac74a6c436ee458e2930d0e0526c1bd82 | b7d415006e18072082acdac343cda21049b910a6 |
| psimedia | /home/silinykh/projects/psi/psimedia | cbf3365830aa49f4a07f8fc9734bed601f65e76d | diff относительно master |
| QCA3 | /home/silinykh/projects/qca | дополнительных изменений этот этап не требует | публичный DTLS/SRTP API уже есть |

Важно: актуальный MEDIA путь — **/home/silinykh/projects/psi/psimedia**, не старый /home/silinykh/projects/psimedia. Проверять, какой plugin реально загружается.

Повторный аудит Iris: 36f9e3a, локальная сборка -j2, Qt 6.10.2 / QCA3 3.0.3 / SRTP и SCTP enabled, штатные Jingle tests 21/21 прошли с доступом к loopback UDP. Дополнительные regressions P0.1 добавлены в штатный набор вместе с исправлением. Psi/psimedia повторно не проверялись: их замечания ниже относятся к указанным старым hashes, перед работой сверить текущий код. Сообщённые Sol cross-repo/CI результаты остаются контекстом:

- Iris standalone suite: сообщено 21/21 green; полная matrix тоже green.
- Cross-repo smoke: Psi 250c1f6a + semantic packet API psimedia собирались; создавались GstProvider/GstRtpSessionContext; static_asserts проходили.
- Experimental rtpsession bridge на cbf3365 не компилируется; известная ошибка описана в P0.3.
- **Реальных peer-to-peer звонков ещё не было.** Ни один green build, mock test или smoke construction не доказывает interop.

### PR stack и CI

Не мержить PR без прямого указания пользователя, не ломать stacked branches.

- Iris #92 docs/jingle-architecture → #93 jingle/ice-udp1 → #94 jingle/group-negotiation → #95 jingle/rtp-extensions → #96 jingle/async-media.
- Psi #968 ai/jingle-native-calls — основной call stack.
- psimedia #15 → #16 — stacked работа, semantic packet API head a99960959.
- psimedia jingle/rtcp-session — experimental bridge, audited HEAD cbf3365.
- Psi #969 ci/psimedia-integration — отдельный real-provider smoke workflow. Перед окончательным merge вернуть нейтральную master/master конфигурацию, не оставлять временные branch pins.

Последний сообщённый failing job: 103558912674, Psi 250c1f6a, psimedia cbf3365, GStreamer 1.24.2. Проверить actual logs и новые HEAD перед повтором. Slash branches и ai/** могут пропускаться старыми workflows; смотреть выполненные jobs, не только общий PR status.

Нужные gates: Psi modern, legacy/Win7 profile, cross-repo real psimedia smoke, isolated psimedia tests, Iris standalone QCA3+SRTP. Локально одна сборка одновременно, максимум -j2. В GitHub CI --parallel 4 допустим. Не запускать несколько локальных полных сборок в фоне.

### Что уже реализовано — не повторять

- Native Iris Manager/Session/Application/Pad, ICE-UDP codec и custom ice:0 wire profile.
- RTP descriptions/extensions/negotiation и async MediaSession/MediaOperation.
- QCA3 DTLS fingerprint authentication, SRTP exporter/profile API; libSRTP для packet protection своим штатным backend. NSS у libSRTP не причина менять DTLS/SCTP путь на другой backend.
- Iris media operation deadlines, bounded pending queue, completion/timeout arbitration.
- Psi psimediajingle.cpp: lifecycle state, terminal error handling, one endpoint per media type, runtime errors, capability snapshot, semantic packets.
- Psi AvCall: readiness по принятым applications, отдельные capture consent и senders. Исходный audio+video → audio-only happy path исправлен.
- Legacy jinglertp/jinglertptasks удалены. Не добавлять новый параллельный native controller вместо исправления существующего AvCall.
- GroupPlan/ConnectionRegistry/ConnectionMembership/ConnectionGroupTransaction и BundleRouter существуют как модели/тестируемые компоненты.
- Router: допустимые PT, MID → SSRC → unique PT fallback, RTP bounds/padding, SharedRtcp, outgoing SSRC registration.
- Media packet boundary: bytes + Rtp/Rtcp, не portOffset/ICE component.

Последние пункты имеют оставшиеся дефекты P0. Фраза «реализовано» не означает полного закрытия всех lifecycle/interop сценариев.

### Semantic packet API: сохранить достигнутую границу

PRtpPacket::Type имеет underlying int, Rtp=0, Rtcp=1. Сохранены layout/value предпосылки provider boundary; static_asserts и cross-repo smoke полезны, но не доказывают совместимость всех старых бинарных потребителей изменённых методов.

Не возвращать portOffset в adapter/API. psimedia не знает ICE, UDP ports, transport topology и BUNDLE membership. Обе копии публичного wrapper API (Psi/src/psimedia и psimedia/psimedia) должны оставаться согласованными; при несовместимом API отдельно учитывать rebuild/versioning потребителей.

### Где заканчивается текущая реализация

- Production ICE::Pad::connectionFor всё ещё создаёт отдельный IceConnection на Transport. Live BUNDLE не подключён и не должен рекламироваться.
- SharedRtcp — результат router, **не готовый group-level ingress media engine**.
- RtpSessionBridge существует в MEDIA/gstprovider/rtpsessionbridge.{h,cpp}, но не подключён к production RtpWorker.
- Bridge входит в gstprovidersrc SOURCES безусловно: experimental runtime isolation не означает build isolation.
- Один общий send/receive RTP session state на media type — направление интеграции; два независимых rtpsession для encoder и decoder запрещены.
- rtpsession не заменяет jitter buffer, depayloader/decoder, congestion/retransmission implementation.
- RTCP generation использовать реальным action signal send-rtcp-full, не выдуманной C-функцией.
- JMI, live BUNDLE, полноценный feedback и network recovery остаются дальнейшими этапами.

Смежные документы: [архитектура](jingle.md), [RTP design](jingle-rtp-design.md), [interop status](jingle-calls-interop.md). Исправлять устаревшие status statements без выдуманных результатов и без истории исправленных багов в описании текущей архитектуры.

## 2. Новый P0 — замечания последнего аудита

Срочность P1/P2 в заголовке — уровень дефекта, не номер этапа. Последовательность каждого исправления: reproducer → fix → regression → adjacent checks. Известный compile blocker не мешает сначала написать/выполнить независимые Iris/Psi regressions.

### P0.1 — закрыто в текущем рабочем дереве Iris

Guarded subset cleanup, cancellation-wins и отклонение пустого initial answer сохранены.
startAcceptedContents пропускает исчезнувшие contents, запускает остальных и проверяет
Session lifetime/state после callbacks. Пустая Session завершается без activation.
Initial activated требует surviving nonterminal content исходного snapshot; поздний
content-accept не вызывает повторную activation.

В tests/jingle/sessionacceptsubset.cpp добавлены штатные regressions: удаление до dispatch,
self/sibling deletion из start, удаление всех, terminate/delete Session из start,
поздний multi-content acceptance. Новый тест воспроизводил дефект до исправления и проходит
после него. Не реализовывать этот пункт повторно; сохранить guards и ACK → queued start.

### P0.2 [P1] Capabilities: сначала provider, затем caps hash/presence

Файлы PSI/src/psiaccount.cpp, src/avcall/avcall.cpp; при необходимости Iris Client/Manager feature invalidation.

Сейчас PsiAccount подписывается на capabilitiesChanged раньше AvCallManager. Поэтому updateFeatures()/presence происходят до refreshCapabilities(). Client::makeDiscoResult объединяет клиентские features с features Jingle Manager. При исчезновении video старый provider ещё добавляет video в disco, а после его замены hash/presence не пересчитываются.

Решение:

- Один согласованный update transaction: новый capability snapshot → provider/manager → Client features и invalidation hash → отправка presence.
- Не полагаться на порядок двух независимых connect. Возможен явный сигнал после committed refresh либо orchestration существующим account/AvCallManager; не создавать ещё один manager.
- Invalidation должна учитывать изменение manager features, даже если собственный Client::features список не изменился.
- Backend-only смена без изменения advertised feature set не обязана спамить presence.
- Старые sessions сохраняют свой provider snapshot; discovery нового состояния не перенастраивает живые media sessions.

Тест: online audio+video → audio-only → unavailable → available; recomputed advertised hash соответствует disco, нет stale video и нет разного snapshot для UI/session factory/discovery. Проверить также initial asynchronous probe.

### P0.3 [P1] Bridge compile failure и частично созданные ресурсы

Файлы MEDIA/gstprovider/rtpsessionbridge.cpp, gstprovider/CMakeLists.txt.

goto fail пересекает инициализацию трёх GstAppSinkCallbacks. Это ошибка C++, не GStreamer API. Поскольку bridge в обычном gstprovidersrc, ломается и сборка provider без PSIMEDIA_BUILD_TESTS.

- Упростить управление ресурсами: RAII либо ясные cleanup scopes/early returns, без переходов через initialization.
- До gst_bin_add_many элементы ещё не принадлежат pipeline. В early failure отдельно созданный session_ сейчас не освобождается уничтожением pipeline.
- Явно различить owned floating/unparented elements, bin-owned elements и refs request pads. Cleanup ровно один раз, без leak/double unref.
- Не отключать bridge/test из сборки ради green. Проверить tests OFF и ON.

Регрессии: отсутствует factory appsrc/appsink/rtpsession либо симулированный partial construction failure; request-pad/link failure; repeated cleanup. Затем изолированная компиляция с -j2.

### P0.4 [P1 до production] Bridge threading, callbacks и безопасный shutdown

Файлы MEDIA/gstprovider/rtpsessionbridge.{h,cpp}.

Appsink callbacks выполняются в streaming threads. Сейчас handlers вызываются прямо из них, setters не синхронизированы; destructor вызывает GST_STATE_NULL. Синхронный teardown из handler может попытаться остановить собственную streaming task. Прямое подключение этих handlers к Jingle writer нарушает thread contract Iris.

До integration определить и документировать:

- owner/control thread для start/stop/configuration/destruction;
- streaming callbacks: что они могут вызывать, кто владеет данными, как отзываются;
- handoff в owner/Jingle thread с bounded queue, lifetime token и generation;
- правила concurrent receive/send/stop. running_ и handler storage не становятся thread-safe от наличия atomic RTCP counter;
- shutdown отзывает доставку, останавливает pipeline вне его streaming callback и гарантирует quiescence до destruction;
- не удерживать mutex во время внешнего callback или ожидания pipeline stop;
- borrowed GstBuffer нельзя сохранять за callback без ref; queued data имеет явное владение;
- замена handlers либо запрещена после start явным проверяемым контрактом, либо синхронизирована.

Копирование std::function в local само по себе не решает streaming-thread teardown. QPointer неприменим к plain non-QObject bridge без отдельного lifetime owner.

Тесты: stop по событию packet delivery; destruction request из callback; concurrent shutdown; late queued delivery после detach; setter/read contract; bounded backlog под медленным consumer. Использовать TSAN/ASAN при доступности отдельным последовательным прогоном.

### P0.5 [P2] Направления codec parameters не обязаны совпадать

Файл MEDIA/gstprovider/rtpsessionbridge.cpp, setPayloads.

Local и remote складываются в одну таблицу PT; одинаковый PT допускается только при gst_caps_is_equal. Корректные negotiation results могут иметь разные direction-specific fmtp, поэтому такой контракт отвергает рабочий codec. Тест {opus}, {opus} этого не проверяет.

- Разделить send/receive codec configuration и минимальную PT information для rtpsession.
- Не выбирать произвольно local или remote при конфликте; определить необходимые поля request-pt-map и явно проверяемые общие constraints.
- Одинаковый PT/codec с допустимой асимметрией fmtp не должен отвергаться.
- Действительно несовместимые clock-rate/codec mapping по-прежнему отклонять до применения.
- Cache clear/update не должен создавать deadlock с request-pt-map; проверить lock ordering.
- Не менять параметры уже negotiated descriptions для подгонки под bridge.

Тесты: одинаковый PT и допустимо отличающиеся fmtp; несовместимый codec/rate; transactional rollback; payload update while running. Проверять больше, чем bool setPayloads: clock-rate, направление caps и реальный packet output.

### P0.6 — закрыто в Iris, сохранить regressions

Runtime outgoing SSRC registration учитывается независимо от static declaration и переживает
её удаление при reconfigure того же content. Исправление 8461f5e, regressions a1ae1ac;
jingle_rtprouter прошёл в аудите 36f9e3a. Повторно не реализовывать.

Сохранять registration bounds, collisions, idempotence, transactional configure и правило:
unregister runtime не удаляет остающуюся static declaration. Это закрытие standalone router
дефекта, не подтверждение production BUNDLE integration.

### P0.7 [P2] Изолированный bridge test сам небезопасен на failure paths

Файл MEDIA/tests/rtpsessionbridge.cpp.

Bridge создаётся раньше mutex/condition variable/контейнеров, которые callbacks захватывают по ссылке. На раннем return эти объекты уничтожаются раньше bridge, пока pipeline ещё может вызывать callbacks. Success path вызывает stop; failure paths нет.

- RAII teardown должен завершать callbacks до разрушения captured state на любом выходе.
- Порядок объявления объектов должен соответствовать ownership; не оставлять это только ручному stop в конце.
- Не ждать завершения pipeline под mutex, который нужен streaming callback.
- Тестировать искусственный early failure после start и после каждого feed/wait.
- Отдельно проверять teardown самого bridge по контракту P0.4.

Green happy path не закрывает этот дефект; flaky crash теста может скрывать настоящий GStreamer failure.

### P0 gate и оставшиеся ограничения router

P0.1/P0.2/P0.6 должны иметь unit/integration regressions Iris/Psi. P0.3/P0.7 делают build/test пригодными, P0.4/P0.5 задают runtime contract bridge до production.

SharedRtcp пока только data type. Обработка RTPFB/PSFB/XR в router не означает полного разбора SSRC внутри FCI/XR blocks. До live BUNDLE отдельно проверить FIR/другие negotiated feedback с target SSRC внутри payload и XR references; неизвестное нельзя случайно доставить только по sender SSRC. Не объявлять все feedback types supported по наличию общего case в switch.

Старые lifecycle/capability/deadline tests продолжать запускать: новые исправления не должны вернуть cleanup-before-error crash, duplicate endpoint, ложную media activation или stale completion.

## 3. Архитектурные инварианты при дальнейшей работе

1. Session::Active — signaling; Application readiness — применённая media configuration плюс authenticated packet attachment. Capture consent хранится отдельно в Psi.
2. Per-content Transport остаётся per-content. Shared association находится под session-local ICE Pad; одинаковый JID не ключ для объединения соединений.
3. Connection membership, ICE generation, DTLS epoch и routing revision — разные сущности. Не заменять их одним счётчиком.
4. Удаление member освобождает только его channel/membership. Последний member завершает association; Session shutdown завершает все.
5. Все callbacks/packet writers проверяют lifetime и актуальность generation/revision. После invalidation нет plaintext fallback или отправки старым writer.
6. Входящий SRTP/SRTCP аутентифицируется один раз до доверенного SSRC learning и media delivery.
7. RTP codecs, retransmission, jitter buffer и congestion control — media engine. Iris согласует capabilities и маршрутизирует, не становится вторым engine.
8. Никаких nested event loops. Очереди ограничены байтами/пакетами/возрастом; worker-thread adapter не вызывает Jingle API напрямую.
9. PubSub остаётся источником истины публикаций; не менять jingle-pub ради звонков. Не ломать file transfer, custom ICE и SCTP.
10. Fingerprint over signaling не равен подтверждению личности через OMEMO. Не делать незаметный downgrade при crypto failure.

## 4. Оставшиеся этапы

### P1a. Изолированный RTP/RTCP bridge: доказать runtime поведение

После P0.3/P0.7 собрать tests ON и запустить psimedia_rtpsessionbridge_test. После P0.4/P0.5 расширить его. До прохождения этих проверок production RtpWorker не подключать.

Минимальные наблюдения:

1. Исходящий RTP проходит send_rtp_sink и выходит как Type::Rtp; сверить bytes/PT/SSRC, не только наличие пакета.
2. Входящий RTP проходит recv_rtp_sink и receive processing с учётом SSRC probation.
3. Type::Rtcp идёт именно в recv_rtcp_sink; проверить обработанный report/statistics, не только increment callback counter.
4. Генерируется RTCP из rtpsession, выходит как Type::Rtcp; разобрать SR/RR и SSRC, не считать диапазон second byte достаточной проверкой.
5. Send и receive используют одну RTP session state для media type.
6. Нет UDP/ICE/ports, камеры/микрофона. Тест headless и детерминированный в пределах документированных RTCP deadlines.
7. Проверены error/stop paths, bounded queues и late callbacks.

send-rtcp-full может планировать отправку по правилам RTCP: не ослаблять тест до «что-нибудь пришло» и не считать принудительный request доказательством корректной периодической работы. Логировать точный GStreamer error/bus message при отказе. Успешный gst_element_set_state с async result не означает, что дальнейших pipeline errors не будет.

Для cross-repo gate через #969 печатать фактические hashes и версии. Не запускать workflow на старых pins и не приписывать его результат новому HEAD.

### P1b. Production RtpWorker integration без transport topology

Сначала прочитать MEDIA/gstprovider/rtpworker.{h,cpp}, pipeline.*, bins.*, gstrtpsessioncontext.*, gstrtpchannel.* и rwcontrol.*. Сделать краткую карту существующих send/receive pipelines, ownership и потоков до правки.

Предлагаемые изменения — расширение этих файлов и RtpSessionBridge, не новая независимая media system:

- Один rtpsession на media type, общий для encoding output и network input. Не создавать отдельную session для send и recv.
- Encoder/payloader RTP → bridge send; bridge network output → semantic PRtpPacket; network input Type::Rtp/Type::Rtcp → соответствующий bridge ingress; receive RTP → существующий receive pipeline.
- Не создавать второй jitter/depay path рядом с существующим. rtpsession сам не выполняет все функции rtpbin.
- При отдельном pipeline bridge согласовать clock/base-time/running-time с producer и consumer. Простое сохранение GstBuffer PTS из другого pipeline не доказывает корректный SR timing или A/V sync.
- Ошибки bridge/bus передавать существующим provider error path с безопасным cleanup до notification.
- Privacy: prepare без capture, transmit по consent; RTCP receive/reporting не прекращается только потому, что локальный RTP sender выключен.
- Codec/profile/feedback activation только по negotiated capabilities; не считать безусловный avpf настройкой любого звонка.
- Bounds appsrc/appsink и handoff queues задать явно. Producer pending_in около 25 пакетов не ограничивает все новые очереди bridge.
- Stop/reconfigure/request cancellation должны завершать callbacks до освобождения worker/context.

Production regression должен пройти настоящий RtpWorker/context path с синтетическим источником/пакетами без физических устройств. Простое создание GstProvider/GstRtpSessionContext не доказывает, что worker действительно использует bridge.

P1b не должен протащить BUNDLE group knowledge в psimedia: group-level RTCP delivery и распределение нескольких media остаются явно согласованной границей с Iris, которую нужно завершить в P2.

### P1c. Подтвердить один реальный native media path

Не создавать заново RTP Application или adapter. Использовать текущие AvCall и BackendSession после P0 и P1b.

1. Записать версии Qt, QCA provider, libSRTP backend, psimedia plugin и GStreamer; убедиться, что загружен именно исследованный plugin.
2. Проверить последовательность с реальным backend:
   - подготовка без input devices;
   - started → prepared offer/answer;
   - apply remote preferences → preferencesUpdated;
   - signaling ACK и authenticated packet readiness;
   - локальный consent + negotiated senders → capture/transmit.
3. Проверить, что итоговые backend payloads действительно соответствуют принятому offer/answer. Сейчас apply сохраняет local description, но backend главным образом получает remote preferences; успех update сам по себе не доказательство codec-specific fmtp compatibility.
4. Не менять PT в XML без соответствующего изменения packetizer/depacketizer backend. Протестировать Opus/VP8 при наличии; для H.264 отдельно проверять fmtp/profile constraints.
5. Проверить semantic Type::Rtp/Type::Rtcp в обе стороны без portOffset; MTU и bounded packet queues. Текущий drainOutgoing сначала собирает batch, затем вызывает local copy writer: это защищает от synchronous deletion. У реального provider pending_in ограничен примерно 25 пакетами, поэтому безлимитный backlog здесь не доказан. Для других providers adapter-side batching допустим отдельным изменением, но только с lifetime token, bounded continuation и сохранением deletion-inside-writer regression. Не добавлять queued lambda с raw this: Endpoint не QObject, reused address не подтверждает identity.
6. Реальный Psi ↔ Psi audio, затем независимые audio/video transports. Проверить consent, остановку, no-device и runtime error.
7. Зафиксировать Conversations release/commit, Android/device, server/TURN и обезличенные stanzas. Выполнить оба направления audio call. Если peer требует ещё не готовый профиль, записать точный блокер и перейти к нужному этапу; не ослаблять проверки ради зелёного теста.

Документы результатов: docs/jingle-calls-interop.md и tests/jingle/fixtures/README.md. Не переписывать historical baseline в «успех»: добавить текущий snapshot и реальные результаты.

Приёмка: воспроизводимая работа native media через сервер либо точный peer-level blocker, не предположение. Успешный build/packaging не заменяет этот этап.

### P2. Подключить существующие group/membership/router к live BUNDLE

Файлы IRIS: jingle-ice.cpp, jingle-ice-connection_p.h, jingle-ice-group_p.h, jingle-group-negotiation_p.h, jingle-session.cpp, jingle-rtp.cpp, jingle-rtp-router_p.*.

Не создавать параллельный GroupManager. Расширять уже существующие сущности.

#### P2a. Ownership и per-content signaling

- ICE Pad использует ConnectionRegistry и результат GroupPlan вместо независимого create для каждого Transport.
- IceConnection становится владельцем ICE/DTLS/SRTP callbacks. Callback не захватывает один Transport как владельца всей группы.
- Каждый Transport имеет membership и свой signaling cursor: что из credentials/candidates/fingerprint уже включено в его pending/sent/ACKed update.
- Network state общая, signaling transaction/ACK принадлежит конкретному действию. Error ACK не считается success и не повторяет start association для каждого member.
- Incoming trickle адресуется допустимому content/owner согласно negotiated profile; применяется к общей association с проверкой generation. Dedup не теряет необходимые protocol updates.
- Не подменять существующий custom transport wire semantics стандартным ice-udp:1.

#### P2b. Транзакционный commit

- Full answer validation → GroupPlan → staged resources → commit. Invalid last content не оставляет частичную группу.
- Сверять credentials/setup/fingerprint для объединяемых content, ordered membership и transport owner.
- Initial refusal/subset BUNDLE: либо реально предложенные независимые transports, либо явный отказ. Не выдавать один скрытый общий socket за несколько несогласованных независимых соединений.
- Пока runtime не готов, incoming группировку тоже нельзя молча принять с ложной семантикой. Выбрать протокольно корректный отказ/негруппированный answer.
- Удаление owner требует явной допустимой owner transition/renegotiation либо завершения группы; не зависеть от порядка hash map.

#### P2c. Per-content channels

Предлагаемая внутренняя сущность ContentPacketChannel в jingle-rtp-router_p.h: identity + membership + routing revision + weak association + writer/receive endpoint. Она заменяет прямую подписку всех Applications на общий SrtpSession::packetReceived.

- Проверять content permission/PT/revision/epoch на отправке.
- Incoming path: ICE → classification → один unprotect → router → нужный media ingress.
- DTLS application data продолжает идти в SCTP; не направлять его в SRTP и не требовать SRTP для data-only association.
- На removal отозвать только channel; не закрывать security других members.
- Crypto invalidation закрывает соответствующие packet gates до восстановления.

Тесты: две полноценные Session с audio/video; реально один ICE agent/association и handshake; удаление одного member; финальное закрытие; две Session к одному peer; отказ/subset группы; conflicting parameters; stale callbacks; RTP/RTCP routing, outgoing registrations и P0.6. Отдельно mixed RTP+SCTP, если этот профиль заявляется.

Только после этих тестов включать BUNDLE offer/advertising согласно точным правилам спецификаций.

### P3. JMI и выбор устройства

Предлагаемые новые IRIS файлы: jingle-message.{h,cpp}, классы MessageInitiationManager и CallProposal. Accessor из Jingle Manager допустим; implementation/state не складывать в основной jingle.cpp. В Psi интеграция в существующий AvCallManager/AvCall, не второй controller с дублирующим lifecycle.

- Зафиксировать поддержанный XEP-0353 profile и peer version. Разные редакции/реализации не считать тождественными.
- Proposal к bare JID, выбор отвечающего resource, привязка Session к full JID.
- Correlation проверяет peer + proposal ID + state; одного UUID недостаточно.
- proceed/retract/reject/finish и ringing по выбранному profile; crossed proposals, два ответивших устройства, resource исчез.
- Carbons/MAM/reconnect: dedup и terminal state не должны оживлять старый звонок. Правила catch-up сверить со спецификацией, а не отбрасывать все delayed сообщения или звонить по каждой копии.
- Текущий highest-priority fallback при отсутствии известных call caps заменить явной политикой: дождаться discovery/JMI либо сообщить unavailable. Не принимать неизвестные caps за положительные.
- Ограниченные pending proposals, deadlines и cleanup при account disconnect.

Тесты state × event × sender, включая forged resource с тем же ID, duplicate, cancel before session-initiate, два устройства и поздний proceed.

### P4. Media control, feedback и устойчивый видео-профиль

Файлы: существующие RTP description/negotiation/info, psimediajingle.cpp и AvCall; MEDIA API/provider менять только при доказанном дефиците.

- Выделить negotiated capabilities и реально активированные backend features. Сохранённый XML extension не означает поддержку.
- Header extensions/MID: backend должен уметь создавать/читать согласованные ID; unique-PT fallback не универсальная замена MID.
- Feedback PLI/NACK, keyframe recovery, retransmission и congestion feedback должны доходить до media engine и работать. Не рекламировать feedback, который adapter отбрасывает.
- Для RTX проверять apt/base codec и retransmission support; H.264 fmtp проверять codec-specific.
- Local mute/hold немедленно применяет local policy; peer status не разрешает capture. Negotiated senders ограничивает передачу и backend activity, а не только drop уже закодированных пакетов.
- Camera add/remove: async prepare → validated content-add/accept → commit route → start; rejection новой camera не ломает существующее audio.
- content-modify не заменяет codec renegotiation. description-info использовать только для определённой XEP семантики, не как произвольный новый offer.

Приёмка: audio+video с pinned Conversations в обе стороны; video decline/add/remove, mute/hold, потери и восстановление keyframe. Simulcast/SVC, SFU и screen sharing — не часть первого совместимого one-to-one профиля.

### P5. Recovery, TURN и транспортные транзакции

Использовать существующие Ice176 и ExternalServiceDiscovery (xmpp_externalservicediscovery.*), не создавать альтернативный discovery manager.

- TURN credentials expiry/refresh, relay-only/IP exposure policy, IPv4/IPv6, UDP-restricted network. Не приравнивать TURN TCP connection к ICE-TCP candidate support.
- Candidate/gathering limits, dedup, end-of-gathering по фактическому Jingle profile.
- Один restart coordinator на association. Новые ICE credentials/generation; stale candidate/update/callback не изменяет новую association.
- Отдельное решение retain/recreate DTLS. Retain keys требует сохранения replay и packet-index state; нельзя reset SRTP и использовать старые ключи как новые.
- transport-replace/accept/reject, timeout/glare/rollback проверять транзакционно на всех members.
- При invalidation отозвать writers и queued packets. При неудаче ограниченный recovery либо явное завершение, не вечный Connecting.
- Потеря XMPP/account disconnect и потеря media path — разные события с явной политикой.

Тесты: смена сети, TURN expiry, pending restart + cancel, поздний старый ACK, удаление owner, transport-reject, две сессии к одному peer. До готовности неподдержанную операцию явно отклонять.

### P6. Release gate и независимое ревью

- Discovery только проверенного профиля с runtime gates P0.2.
- Проверить exported/install headers обеих include-facades Iris, CMake targets, shared/static consumers, pkg-config и optional SRTP.
- Проверить поддерживаемые Qt/QCA конфигурации; QCA2/SRTP OFF не обязаны давать native secure calls, но не должны ломать остальные функции библиотеки.
- CI standalone tests должны действительно включать QCA3+SRTP; наличие зелёной обычной сборки не доказательство исполнения этих тестов.
- Windows packaging и Android packaging проверять отдельно, если эти платформы включаются. Зафиксировать loaded plugin/dependency versions, не только успешный линк.
- Полный interop report, ограничения и воспроизводимые ошибки передать на независимое ревью. Не объявлять parity до реальных звонков.

## 5. Целевые последовательности

### Подготовка, активация, ошибка

~~~mermaid
sequenceDiagram
    participant UI as Psi AvCall
    participant J as Iris RTP Application
    participant M as MediaSession / BackendSession
    participant B as psimedia provider
    participant T as ICE / DTLS / SRTP
    UI->>J: Initiate или local accept
    J->>M: prepare operation + deadline
    M->>B: start/update без capture
    B-->>M: prepared
    M-->>J: immutable description
    Note over UI,T: Jingle offer/answer и соответствующие ACK
    J->>M: apply operation + deadline
    B-->>M: preferencesUpdated
    M-->>J: applied
    T-->>J: authenticated packet channel
    J-->>UI: accepted Application Active
    UI->>M: Capture/transmit по accepted set, senders и consent
    alt Backend error
        B->>B: cleanup control
        B-->>M: error
        M->>M: Failed; revoke callbacks; запрет provider calls
        M-->>J: error/runtimeError
        J-->>UI: terminal или failure принятого content
    end
~~~

Готовность transport и apply может приходить в разном порядке. Gate должен быть коммутативным: каждый порядок даёт ровно одну activation. Signaling Active не вызывает capture.

### BUNDLE ownership

~~~mermaid
flowchart TD
    S[Session] --> P[ICE Pad / committed GroupPlan]
    P --> R[ConnectionRegistry]
    A[Audio Transport] --> AM[Audio membership]
    V[Video Transport] --> VM[Video membership]
    AM --> C[Shared IceConnection]
    VM --> C
    R --> C
    C --> I[ICE agent]
    C --> D[DTLS / QCA]
    D --> K[Verified SRTP key material]
    K --> SR[SrtpSession / libSRTP]
    SR --> BR[BundleRouter / RTCP group ingress]
    BR --> CA[Audio packet channel]
    BR --> CV[Video packet channel]
~~~

Это целевое владение, не заявление о текущей интеграции. Освобождение audio membership не уничтожает C, пока жив video membership.

### Teardown и recovery

- User cancel: terminal guard → revoke capture/writers → cancel media operations → detach per-content delivery → release membership → terminal UI notification.
- Failed backend: отметить Failed до внешних сигналов → не вызывать API очищенного control → отменить/завершить связанные операции.
- Remove member: отозвать его revision/operations, затем release; другие members работают.
- Restart: staging новой ICE generation → validated signaling commit → отдельная DTLS policy → authenticated gate → route revision commit. Старые callbacks не проходят generation checks.
- После любого внешнего callback возможна синхронная deletion; QPointer/ownership guards проверять до следующего обращения.

## 6. Проверки, результаты и ограничения работы

Предлагаемые дополнительные adapter tests находятся в PSI/tests/avcall, protocol tests — IRIS/tests/jingle. Не переносить psimedia dependency в Iris unit tests.

Существующий standalone suite запускать из PSI последовательно:

~~~sh
cmake -S iris/tests/jingle -B /tmp/iris-jingle-qca3 \
  -DUSE_QT6=ON -DIRIS_SYSTEM_QCA=3 -DIRIS_ENABLE_SRTP=ON \
  -DSRTP_INCLUDE_DIR=/usr/include \
  -DSRTP_LIBRARY=/usr/lib/x86_64-linux-gnu/libsrtp2.so
cmake --build /tmp/iris-jingle-qca3 -j2
ctest --test-dir /tmp/iris-jingle-qca3 --output-on-failure -j1 --repeat until-fail:3
~~~

Пути библиотек зависят от системы. Не перезапускать configure в чужой занятой build directory. Sandbox может запрещать loopback UDP: это отдельный environmental blocker, не успешный skip и не доказанный дефект кода.

Тесты jingle_icertp/jingle_rtpmedia используют real local UDP и mock media. Они не заменяют настоящий psimedia backend и звонок через XMPP server. Для текущего полного списка использовать ctest -N, не старый список из baseline docs.

Минимальная end-to-end матрица:

| Сценарий | Обязательное наблюдение |
| --- | --- |
| Audio, оба направления | звук, profile, codec, отсутствие capture до consent |
| Both → audio-only | активируется audio, video не висит Pending |
| Audio+video BUNDLE | действительно одна association; правильный RTP и RTCP |
| BUNDLE refusal | корректный fallback либо явный отказ без ложного Connected |
| Runtime backend error | нет crash, backend calls после cleanup, зависших устройств |
| Cancel на каждой фазе | один terminal outcome, нет поздней activation |
| Два устройства peer | один принятый звонок, остальные перестали звонить |
| TURN / network switch | working relay или bounded documented failure |
| Packet loss / bandwidth limit | feedback действует, queues ограничены |
| File transfer / SCTP рядом | прежние wire/crypto semantics сохранены |

Диагностика: local call ID, content/group, ICE generation, crypto epoch, operation ID, transition, duration, drops/queue depth. Не логировать private keys, exporter material, ICE/TURN passwords, plaintext media. Peer fixtures обезличить; версия Conversations обязательна.

Performance измерять отдельно: media encoding CPU, SRTP packet processing, QCA DTLS/SCTP throughput, allocations/copies и queue latency. Не заменять crypto backend из-за общей загрузки GStreamer.

## 7. Документация и спецификации

Источники для проверки конкретных решений, не список уже реализованных возможностей:

- [XEP-0166](https://xmpp.org/extensions/xep-0166.html): Session/content lifecycle, subset и transport actions.
- [XEP-0167](https://xmpp.org/extensions/xep-0167.html): RTP descriptions, senders, session-info.
- [XEP-0176](https://xmpp.org/extensions/xep-0176.html): ICE-UDP wire profile.
- [XEP-0320](https://xmpp.org/extensions/xep-0320.html): DTLS-SRTP fingerprint/setup.
- [XEP-0338](https://xmpp.org/extensions/xep-0338.html) и [RFC 8843](https://datatracker.ietf.org/doc/html/rfc8843): grouping и BUNDLE semantics. SDP нельзя механически переписать в XML.
- [XEP-0293](https://xmpp.org/extensions/xep-0293.html), [XEP-0294](https://xmpp.org/extensions/xep-0294.html), [XEP-0339](https://xmpp.org/extensions/xep-0339.html): feedback/extensions/sources.
- [XEP-0353](https://xmpp.org/extensions/xep-0353.html): JMI; фиксировать редакцию и peer implementation.
- [XEP-0215](https://xmpp.org/extensions/xep-0215.html): существующий ExternalServiceDiscovery.
- [RFC 5761](https://www.rfc-editor.org/rfc/rfc5761), [RFC 7983](https://www.rfc-editor.org/rfc/rfc7983): RTP/RTCP mux и datagram demux; сверять применимые updates для выбранного transport profile.
- [RFC 5764](https://www.rfc-editor.org/info/rfc5764/), [RFC 3711](https://www.rfc-editor.org/rfc/rfc3711), [RFC 7714](https://www.rfc-editor.org/rfc/rfc7714): crypto contracts.
- [libSRTP](https://github.com/cisco/libsrtp), [GStreamer rtpmanager](https://gstreamer.freedesktop.org/documentation/rtpmanager/): читать версию, реально включённую в сборку.

При недоступности сайта не придумывать normative MUST и не ослаблять проверки. Сохранить вопрос для сверки со спецификацией или pinned peer fixture.

## 8. Как выполнять и сдавать работу

Начать с восстановления hashes/status и проверки CI evidence. Выполнить новый P0: Iris/Psi fixes и bridge compile/test/lifetime fixes. Затем P1a isolated runtime → P1b production worker → P1c native peer checks. P2 live BUNDLE, P3 JMI, P4 feedback/control, P5 recovery и P6 release выполняются по зависимостям наблюдённого peer. Не возвращаться к созданию уже существующих interfaces с нуля.

Каждый завершённый подпункт сопровождать:

1. Что изменено, в каких файлах, почему это устраняет указанный сценарий.
2. Regression test, который выявляет дефект на исходной версии.
3. Фактические команды/результаты; явно skipped/blocked, без выдуманных live tests.
4. Изменения ownership/threading/API и оставшиеся ограничения.
5. Обновление interop/design docs только до фактически достигнутого состояния.

Если установленный provider отличается от прочитанного, сначала сверить контракт. Если решение отличается от предложенного, обосновать кодом/тестом/спецификацией. Допустима более простая реализация с теми же гарантиями; недопустимо обходить gate, возвращать фиктивную готовность или игнорировать ошибку ради соединения.

Окончательная совместимость — подтверждённые реальные звонки на зафиксированном peer плюс negative/lifecycle tests, а не успешная компиляция и не количество новых классов.
