# Native Jingle calls: план продолжения для нового чата с Sol

Актуализировано 2026-09-19. Это самостоятельное задание для продолжения существующей реализации, а не предложение спроектировать стек заново. Начать с проверки веток/коммитов через GitHub connector и CI evidence новых исправлений. Старые исправленные замечания не реализовывать повторно.

## 1. Контекст, репозитории и границы достоверности

Цель — защищённые одноранговые audio/video звонки Psi ↔ Psi и Psi ↔ Conversations. Единственный Jingle stack находится в Iris; Psi использует существующие AvCall/AvCallManager; media engine — psimedia. Никакого второго call/Jingle manager или возврата legacy transport stack.

### Среда Sol: только GitHub connector и GitHub Actions

У Sol нет локального checkout, shell с Qt, компилятора для проекта или доступа к устройствам.
Не клонировать репозитории, не предлагать git pull, локальные CMake/CTest и установку Qt.
Читать файлы, сравнивать commits/PR и вносить разрешённые изменения через доступный GitHub
connector. Сборки, тесты и sanitizer runs выполняются исключительно GitHub Actions.
Checkout и установка dependencies внутри CI job допустимы и необходимы — это не локальная среда Sol.

Если connector не умеет dispatch workflow, читать logs/artifacts или записывать нужные файлы:
явно назвать отсутствующую операцию и попросить пользователя выполнить именно её.
Не утверждать, что workflow запущен/прошёл, по одному созданному commit или зелёному старому PR.
Не требовать от пользователя клонирования/локальной сборки ради операций, доступных в CI.

| Репозиторий | Рабочая ветка | HEAD snapshot 2026-09-19 |
| --- | --- | --- |
| psi-im/psi | ai/jingle-native-calls | `78dee48c396c7815ef78284ea545c0c103e1149a` |
| psi-im/iris | jingle/async-media | `bd5c86283c9eafbbe6e56efe7d69b480bc4cd7bd` |
| psi-im/psimedia | jingle/rtcp-session | `2d067da46a70a74b1ccf91830c97b09a8c58713b` |
| psi-im/psi | ci/psimedia-integration | `136af31dcaa64a7c3144fe3ed12e02f180d33a0b` |

HEAD здесь только snapshot, не pin для будущей работы. Перед любым write заново читать все четыре
ветки. Исторические T0–T5/A3/A4 детали остаются в git history и профильных docs; этот план хранит
только текущий контракт, незакрытые gates и короткие markers уже завершённого.

### PR stack и CI

- Iris #96 `jingle/async-media`; Psi #968 `ai/jingle-native-calls`; psimedia experimental
  `jingle/rtcp-session`; Psi #969 `ci/psimedia-integration`.
- Не мержить без прямого указания пользователя и не ломать stacked branches.
- Для результата всегда фиксировать exact checkout SHA и фактически выполненные jobs/tests.
- Cross-repo #969 остаётся staging/live gate; обычный green build не заменяет runtime evidence.

### Дисциплина веток, PR и CI

Продолжать существующие ветки; новый PR только для самостоятельной логической границы.
Не создавать дублирующие workflows/runs без причины. Частые commits на `ai/**` могут не запускать
тяжёлый CI автоматически; проверять trigger и фактические jobs, а не интерпретировать отсутствие
run как success. Никаких `pull_request_target` с исполнением недоверенного branch code/secrets.

### Уже реализовано — не повторять

- Iris Jingle Session/Application/Transport framework, transport-replace arbitration, ICE/DTLS/SCTP,
  RTP descriptions/extensions, async media operations и direction policy/completion.
- Psi использует существующие AvCall/AvCallManager; psimedia adapter/backend lifecycle и runtime
  error handling уже подключены. Второй call controller/Jingle stack не создавать.
- Semantic packet API — bytes + Rtp/Rtcp, без portOffset/ICE topology.
- Grouping/BUNDLE foundation уже существует: GroupPlan, ConnectionRegistry,
  ConnectionMembership, ConnectionGroupTransaction и BundleRouter с отдельными regressions.
- psimedia RTP/RTCP bridge, capture consent/hotplug lifecycle и native-backend synthetic path уже
  имеют отдельные tests/CI evidence. Старые подробности искать в git/docs, не восстанавливать заново.
- Legacy Jingle FT S5B/IBB и существующий SCTP/datachannel FT считаются совместимостью, которую
  новые lifecycle/BUNDLE изменения обязаны сохранить.

### Semantic packet API: сохранить достигнутую границу

`PRtpPacket::Type` остаётся `Rtp=0/Rtcp=1`; не возвращать portOffset в adapter/API.
psimedia не владеет ICE/BUNDLE topology. Обе public wrapper copies Psi/psimedia держать синхронными.

### Где заканчивается текущая реализация

- Production `ICE::Pad::connectionFor(Transport*)` пока создаёт отдельный `IceConnection` на
  каждый Transport. **Это недостающий production wiring, а не отсутствие BUNDLE design/tests.**
- Исторически master с 2020 года мог автоматически сигналить `<group semantics='BUNDLE'>`, но
  каждый ICE Transport всё равно имел собственные ICE/components/DTLS/SCTP. Это было grouping
  signaling без реального shared network path.
- 2026-09-10 commit `874a3a6b...` при RTP refactor намеренно перестал автоматически рекламировать
  BUNDLE при независимых connections.
- 2026-09-11 появились `ConnectionMembership`, session-local `ConnectionRegistry`,
  `GroupPlan` и transactional `ConnectionGroupTransaction` с tests; 2026-09-11/12 —
  authenticated `BundleRouter` и hardened routing contract. Эти primitives должны быть
  **подключены**, а не перепроектированы.
- Live BUNDLE пока не включать в discovery/offer до shared ICE/DTLS ownership, packet routing,
  removal/restart и mixed RTP+SCTP regressions.
- DataChannel/SCTP естественно позволяет нескольким file-transfer streams делить одну association;
  текущий per-Transport IceConnection это не использует. После live BUNDLE одна association должна
  обслуживать несколько DataChannel connections без закрытия соседних streams.
- `src/irisnet/noncore/sctp/` содержит заимствованную mediasoup SCTP implementation
  (см. его README). Не менять vendored core без доказанной необходимости; наш lifecycle/glue слой —
  Jingle SCTP/DataChannel/ICE integration вокруг него.
- FT live baseline: two-process Prosody ICE→DTLS→SCTP transfer с deterministic bytes и SHA-256
  прошёл в run #75. Расширенная matrix ICE/S5B/IBB + transport-replace находится в работе;
  run #76 подтвердил ICE и выявил standalone S5B stall сразу после `Prepare local offer`.
- P1c ещё не закрыт: нет server-mediated real Psi↔Psi native call и Conversations interop.
  P2 live BUNDLE не объявлять завершённым раньше этого gate.

Смежные документы: [архитектура](jingle.md), [RTP design](jingle-rtp-design.md),
[interop status](jingle-calls-interop.md).

## 2. Закрытые checkpoints и текущие gates

### Закрыто — только краткие markers

- T0: Session/TieBreaker cancellation, reentrancy, bounded arbitration и IQ callback lifetime.
- T1/T2/T3: tracked sender attempts, DirectionController/policy ownership и Psi audio adapter.
- T4/T5: transport arbitration/completion и staged transport payload boundary.
- A3/A4: capture source identity switch и hotplug/receive graph lifecycle.
- P1a/P1b основа: semantic RTP/RTCP bridge, bounded queues, production provider path,
  no-device/stop/runtime-error regressions, real installed psimedia plugin smoke.
- FT baseline: feature-driven ICE → S5B → IBB selection regression; real Prosody SCTP/datachannel
  transfer через два процесса. Детали и старые SHA остаются в git/interop docs.

### Текущие обязательные gates

1. Довести live FT matrix: ICE, S5B, IBB отдельно; ICE→S5B и S5B→IBB через настоящий
   `transport-replace`; затем drain/lifetime regressions для всех трёх transports.
2. P1c: real server-mediated Psi↔Psi audio, затем A/V; consent/stop/no-device/runtime error.
3. Pinned Conversations interoperability в обе стороны.
4. Только затем P2 production live BUNDLE wiring существующих group/membership/router primitives.
5. После wiring отдельно проверить multiple DataChannels on one SCTP association и mixed RTP+SCTP.

## 3. Архитектурные инварианты при дальнейшей работе

1. Session::Active — signaling; Application readiness требует своих media/transport условий.
   Capture consent хранится отдельно в Psi.
2. Per-content `Transport` остаётся per-content. Shared association живёт под session-local ICE Pad;
   одинаковый JID не является ключом sharing.
3. Connection membership, ICE generation, DTLS epoch, routing revision и DataChannel stream lifetime —
   разные сущности; одним счётчиком их не заменять.
4. Удаление member/channel освобождает только его долю. Последний member может завершить association;
   Session shutdown не должен случайно уничтожить ещё drain-ящиеся обязательные данные.
5. Все callbacks/writers проверяют lifetime и generation/revision; после invalidation нет plaintext
   fallback и отправки stale writer.
6. Входящий SRTP/SRTCP аутентифицируется до доверенного routing/SSRC learning.
7. RTP codecs/jitter/retransmission/congestion — media engine. Iris согласует и маршрутизирует.
8. Никаких nested event loops; queues bounded; worker callbacks не вызывают Jingle API напрямую.
9. PubSub не менять ради calls. Legacy IBB/S5B FT и existing SCTP FT не ломать.
10. Fingerprint over signaling не равен OMEMO identity confirmation; crypto failure не downgrade.

### BUNDLE: зафиксированная архитектура, которую надо довести до production

- Не возвращаться к старому “добавить BUNDLE XML и оставить отдельные sockets”.
- Negotiated `GroupPlan` должен атомарно коммититься в session-local `ConnectionRegistry`;
  per-content Transport получает `ConnectionMembership` в общей association.
- `ConnectionGroupTransaction` уже проверяет shared vs independent topology, owner removal,
  final release, refusal fallback и rollback частичного commit.
- `BundleRouter` уже проверяет authenticated MID/SSRC/PT routing, SharedRtcp, runtime outgoing
  SSRC registration, revision fencing и removal. Production wiring должен использовать эти contracts.
- Shared ICE/DTLS/SCTP ownership не означает shared `Transport` object: signaling/ACK state остаётся
  per-content/per-action.
- Для DataChannel одна SCTP association может обслуживать много `Connection`/streams. Закрытие или
  draining одного файла не завершает association, если другие streams/members ещё живы.

### `Finishing`: drain boundary, а не универсальный смысл

`Finishing` нельзя трактовать одинаково на Connection, Transport, Application и Session.
Это состояние означает: **новая работа данного вида уже не принимается, но объект ещё обязан
завершить свой layer/application-specific хвост прежде чем стать `Finished`.**

- Receiver-side byte stream: remote EOF/close означает “новых bytes больше не будет”, но уже
  принятые bytes в `QIODevice`/Connection buffers должны оставаться читаемыми. `Finished` только
  после drain либо после получения заранее объявленного количества bytes.
- Sender-side критерий другой: после последнего application write могут ещё существовать
  transport/QIODevice buffers или protocol acknowledgements. Нельзя зеркально использовать
  receiver condition.
- Message/datagram/DataChannel semantics могут требовать другой drain rule, чем sequential stream.
  Конкретный Application определяет, что для него означает complete payload.
- FT Application знает declared file size/range/hash/protocol completion и потому может завершаться
  позже transport EOF; RTP Application обычно не имеет обязательства drain media после hangup.
- IBB уже содержит явную legacy модель `Active → Finishing → Finished`: remote close оставляет
  connection readable до `bytesAvailable()==0`. Сохранять эту семантику.
- S5B и SCTP/DataChannel сначала охарактеризовать regression tests; не “унифицировать” методом
  преждевременного `close()/delete`, который сломает их существующий buffering.
- Нужен единый observable contract (no-more-input / draining / finished), но transport-specific
  реализация и application completion condition могут различаться.

### Session termination policy

`session-terminate` — signaling event, а не универсальный приказ немедленно уничтожить все
buffered data. Политика зависит от состава Session:

- **File transfer only.** Нормальный `session-terminate success` должен обычно возникать автоматически
  после успешного завершения всех FT applications/protocol confirmations. Если terminate приходит
  раньше, это cancellation/failure: reason должен позволять отличить отмену/ошибку от успешного
  окончания; уже принятые обязательные bytes нельзя терять только из-за signaling teardown.
- **RTP call only.** Hangup/`session-terminate` — естественное окончание звонка. Ждать draining
  media packets обычно не нужно; capture/writers можно revoke немедленно, затем deterministic cleanup.
- **Mixed RTP + DataChannel/FT.** Самый опасный случай. Terminating RTP content/call не должен
  уничтожить shared ICE/DTLS/SCTP association, если DataChannel member ещё обязан drain/finish.
  И наоборот, закрытие одного DataChannel не завершает RTP. Session-level terminate допустим только
  когда политика всех surviving applications согласована; иначе использовать content-level teardown
  или явную cancellation semantics.
- Session/Application/Transport lifetime не должны определяться одним enum comparison. Terminal
  signaling может быть уже получен, пока отдельный Connection ещё законно живёт для local drain.

## 4. Оставшиеся этапы

### P1a. Изолированный RTP/RTCP bridge: доказать runtime поведение

Через CI подтвердить tests ON и исполнение актуальных bridge/periodic/backpressure/packet и production sender targets из CMakeLists.txt. Сначала оценить уже добавленное покрытие, расширять только реальные пробелы. Production wiring уже есть: до закрытия gate не считать его готовым к release.

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

### P1b. Довести существующую production integration без transport topology

Сначала прочитать MEDIA/gstprovider/rtpworker.{h,cpp}, pipeline.*, bins.*, gstrtpsessioncontext.*, gstrtpchannel.* и rwcontrol.*. Сделать краткую карту существующих send/receive pipelines, ownership и потоков до правки.

Wiring уже реализован. Сверить перечисленный контракт с кодом и закрыть A3/A4, не подключать bridge повторно:

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

Это **integration этап уже реализованной BUNDLE foundation**, не новая архитектура.

Текущая база:
- `GroupPlan` + validation shared transport parameters;
- session-local `ConnectionRegistry` и move-only `ConnectionMembership`;
- transactional `ConnectionGroupTransaction` с rollback/refusal/removal tests;
- `BundleRouter` с MID/SSRC/PT, SharedRtcp, outgoing SSRC registration и revision fencing;
- current production `ICE::Pad::connectionFor(Transport*)` всё ещё создаёт independent
  `IceConnection`, поэтому BUNDLE не рекламируется.

Файлы IRIS: `jingle-ice.cpp`, `jingle-ice-connection_p.h`, `jingle-ice-group_p.h`,
`jingle-group-negotiation_p.h`, `jingle-session.cpp`, `jingle-rtp.cpp`,
`jingle-rtp-router_p.*`. Не создавать параллельный GroupManager.

#### P2a. Commit negotiated membership в production ICE

- Full answer validation → immutable GroupPlan → staged/transactional commit → memberships.
- ICE Pad хранит session-local association registry; Transport не создаёт connection “по себе”
  после согласованного BUNDLE.
- Несгруппированные contents и BUNDLE refusal остаются independent associations.
- Общие credentials/fingerprint/setup проверяются до commit. Invalid last member не оставляет
  частично живую группу.
- Per-content Transport сохраняет собственные signaling cursor/ACK/action state. Sharing network path
  не означает sharing signaling transaction.
- Owner removal требует явной ownership transition или модели, где network association вообще
  не зависит от lifetime “первого” Transport.
- Не рекламировать BUNDLE до реального shared ownership.

#### P2b. Shared ICE/DTLS/SRTP/SCTP lifetime

- `IceConnection` владеет ICE agent/components/DTLS и общими callbacks; callback не должен
  захватывать один Transport как lifetime owner всей группы.
- Последний membership закрывает association. Удаление одного member не закрывает соседей.
- ICE restart координируется один раз на association; stale generation не меняет новую.
- RTP/SRTP и SCTP application data демультиплексируются на общей DTLS/ICE association согласно
  wire profile; stopping RTP consumer не закрывает SCTP.
- Для DataChannel association допускает несколько streams/files одновременно. Per-stream
  `Connection` имеет собственный `Finishing/Finished`; association живёт до последнего member/channel.
- Не менять vendored mediasoup SCTP core ради ownership, если нужное поведение реализуемо в нашем glue.

#### P2c. Packet/channel routing

Внутренняя per-content channel identity должна связывать membership + routing revision + weak
association + writer/receive endpoint.

- outgoing: content permission/PT/revision/crypto epoch;
- incoming: classification → один authenticate/unprotect → BundleRouter → нужный media ingress;
- DTLS application data → SCTP, не SRTP;
- removal revoke только свой channel/routes;
- crypto invalidation закрывает gates до восстановления;
- SharedRtcp остаётся group-level ingress, не дублируется по endpoints.

#### P2d. Обязательные regressions

1. Audio+video: один реальный ICE agent/DTLS association; обе стороны routing.
2. Remove одного member: другой продолжает; remove последнего закрывает association.
3. BUNDLE refusal/subset/conflicting params/rollback.
4. Две Session к одному peer не делят association.
5. Restart/late callbacks/transport-replace с generation fencing.
6. Mixed RTP+SCTP на одной association.
7. Два и более DataChannel FT streams одновременно: закрытие/drain одного не влияет на остальные.
8. Session/content termination в mixed case не уничтожает connection, который ещё обязан drain.

Только после этих regressions включать BUNDLE offer/advertising.

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

### Finishing/drain и завершение Session

~~~mermaid
stateDiagram-v2
    [*] --> Active
    Active --> Finishing: no more work/input for this layer
    Finishing --> Finished: layer/application drain condition satisfied
    Finished --> [*]
~~~

Переход `Finishing → Finished` не обязан иметь одинаковое условие:

- FT receiver: declared bytes/range получены, buffered input drained, hash/protocol completion done.
- FT sender: payload передан downstream, обязательные local/protocol buffers/acks завершены.
- RTP call hangup: media drain обычно не обязателен; revoke и cleanup могут быть immediate.
- Mixed Session: каждый Application/Connection выполняет своё правило; shared association
  закрывается только когда нет surviving membership/обязательного drain.

Regression должен намеренно закрывать peer-side Connection до чтения последнего buffered chunk и
доказывать, что receiver всё равно получает ровно N bytes. Повторить для ICE/DataChannel, S5B и IBB,
не ломая legacy IBB/S5B behavior.

### Teardown и recovery

- User cancel: определить application-specific cancellation reason → revoke новые writes/capture → сохранить обязательный local drain там, где он нужен → detach/release только после соответствующего terminal condition → один UI outcome.
- Failed backend: отметить Failed до внешних сигналов → не вызывать API очищенного control → отменить/завершить связанные операции.
- Remove member: отозвать его revision/new work; если connection имеет обязательный drain, дождаться его локального completion; затем release membership. Другие members работают.
- Restart: staging новой ICE generation → validated signaling commit → отдельная DTLS policy → authenticated gate → route revision commit. Старые callbacks не проходят generation checks.
- После любого внешнего callback возможна синхронная deletion; QPointer/ownership guards проверять до следующего обращения.

## 6. Проверки, результаты и ограничения работы

Предлагаемые дополнительные adapter tests находятся в PSI/tests/avcall, protocol tests — IRIS/tests/jingle. Не переносить psimedia dependency в Iris unit tests.

### Уровни проверки и исполнитель

| Уровень | Что доказывает | Где выполняется |
| --- | --- | --- |
| Source review | Ownership, threading, транзакции, корректность assertions | Sol через GitHub files/diffs |
| Unit/lifecycle regressions | Отмена, stale callbacks, caps refresh, signaling | GitHub CI |
| Isolated integration | Настоящий GStreamer rtpsession, RTP/RTCP bytes и reports без устройств | GitHub CI |
| Cross-repo integration | Совместимые Psi/Iris/psimedia API и настоящий provider | GitHub CI на явных SHA всех репозиториев |
| Sanitizers | UAF/UB, leaks и races в исполняемых сценариях | Отдельные Linux CI jobs |
| Production synthetic media | Настоящий RtpWorker, encoder/decoder и bridge path | Headless CI после P1b |
| Live interoperability | Реальные Psi ↔ Psi / Conversations через сервер и сети | Пользователь/тестер с устройствами |

Просто открыть Psi или создать GstRtpSessionContext недостаточно. Изолированные bridge tests
используют настоящий GStreamer, поэтому это не только mock/unit verification; но они не
доказывают прохождение production worker и удалённого peer.

### CI evidence: порядок действий Sol

1. Через connector получить текущие heads и workflow definitions нужных PR/веток.
2. Проверить, какие runs относятся к этим exact SHA; смотреть job steps, skip conditions,
   commands и фактические checkout hashes, а не только общий статус PR.
3. Для отсутствующего gate использовать/расширить существующий workflow; не заводить
   параллельную инфраструктуру без необходимости. Cross-repo #969 остаётся staging gate.
4. Job должен печатать Psi/Iris/psimedia SHA (включая фактический submodule override),
   Qt/QCA/provider/libSRTP/GStreamer versions, build options и список CTest tests.
   Не считать checkout старого Iris submodule проверкой новых sender fixes.
5. Build QCA3+SRTP Iris standalone tests и реальные psimedia targets; CTest
   --output-on-failure, повторы lifecycle/race tests по необходимости.
   Установку Qt/GStreamer/libSRTP выполняет runner workflow; без hardcoded путей машины автора.
6. Для bridge проверить tests OFF/ON, затем cross-repo provider smoke и Psi capability/lifecycle.
   Проверять runtime regressions, не только наличие test executable.
7. ASan/UBSan отдельно от TSan; сохранять logs/artifacts и известные ограничения runner.
   Сетевым Iris integration tests нужен loopback UDP. Environmental failure записывать
   отдельно: он не pass и не автоматически баг реализации.
8. Зафиксировать run URL/ID, exact SHA, executed test names/count, outcome и blockers.
   Не выдумывать URL/результаты. Если connector не даёт logs/dispatch — запросить
   конкретный run или действие пользователя, не объявлять отсутствие Qt локально блокером проекта.

Локальные результаты аудитора Iris — полезная отдельная запись, но не замена CI для
текущей cross-repo комбинации. Новых psimedia runtime results в этом архитектурном review нет; CI результаты предстоит подтвердить.

### Production и ручная проверка

После P1b нужен synthetic source → настоящий encoder/payloader → RtpWorker/bridge →
transport → receive pipeline/decode. В CI использовать подходящие test sources без
камеры/микрофона; проверять decoded output, а не только создание pipeline.
Синтетические источники не должны включаться как fallback в обычном звонке.

Для ручных звонков Sol готовит короткий сценарий, точные artifacts/build SHA и список
безопасной диагностики; пользователь запускает клиенты и возвращает результаты.
Не просить проверять live BUNDLE до runtime wiring и не ждать устройств для независимой
работы над CI/P2/P3. Помечать live gate pending, не выдавая synthetic успех за interop.

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
| File transfer / SCTP рядом | ICE/S5B/IBB FT сохранены; transport-replace работает; buffered tail не теряется |
| Multiple DataChannels | одна SCTP association, независимые stream close/drain, соседний transfer жив |
| Mixed RTP + DataChannel | RTP hangup не убивает обязательный FT drain; FT close не убивает RTP |

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

Начать с remote heads и CI evidence через connector. Checkpoints T0–T5, Psi audio adapter,
psimedia RTP/RTCP bridge и A3/A4 source lifecycle уже опубликованы; не реализовывать их повторно.
Следующие gates: довести live FT transport matrix и drain semantics, затем P1c native peer checks.
После P1c подключить существующие group/membership/router primitives к production live BUNDLE;
не перепроектировать их заново. P3 JMI, P4 feedback/control, P5 recovery и P6 release выполнять
по зависимостям наблюдённого peer.

Каждый завершённый подпункт сопровождать:

1. Что изменено, в каких файлах, почему это устраняет указанный сценарий.
2. Regression test, который выявляет дефект на исходной версии.
3. CI run/job URLs, exact checkout SHA, исполненные тесты и результаты; явно skipped/blocked, без выдуманных локальных/live tests.
4. Изменения ownership/threading/API и оставшиеся ограничения.
5. Обновление interop/design docs только до фактически достигнутого состояния.

Если установленный provider отличается от прочитанного, сначала сверить контракт. Если решение отличается от предложенного, обосновать кодом/тестом/спецификацией. Допустима более простая реализация с теми же гарантиями; недопустимо обходить gate, возвращать фиктивную готовность или игнорировать ошибку ради соединения.

Окончательная совместимость — подтверждённые реальные звонки на зафиксированном peer плюс negative/lifecycle tests, а не успешная компиляция и не количество новых классов.
