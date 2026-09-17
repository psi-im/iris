# Native Jingle calls: план продолжения для нового чата с Sol

Актуализировано 2026-09-17 по последнему аудиту. Это самостоятельное задание для продолжения существующей реализации, а не предложение спроектировать стек заново. Начать с проверки веток/коммитов через GitHub connector и CI evidence новых исправлений. Старые исправленные замечания не реализовывать повторно.

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

| Репозиторий | Ветка | Проверенный диапазон |
| --- | --- | --- |
| psi-im/psi | ai/jingle-native-calls | 9421bd0e3df1008d2b3cb1783d945061035d6333 → c8821dbeb30096e5aca1462cb2e660ad3705a5ad |
| psi-im/iris | jingle/async-media | Архитектурная сверка fb7678d088834830e58b8bd733018a8ef83f72d2; прежний аудит ba784334 → be3833e |
| psi-im/psimedia | jingle/rtcp-session | ab15f6829be56921920188f2381feebdd9d9d0ca → b4139cbddde3d9439568f4d59cf0af390dd7432f |

IRIS/PSI/MEDIA ниже означают корни GitHub репозиториев. Проверить remote heads перед
работой. Аудит касается этих диапазонов, не неполученных commits.
Iris subset/acceptance P0.1 уже опубликован: не просить пользователя публиковать его заново.
QCA3 и libSRTP сохраняются, оснований менять crypto backend этот аудит не даёт.

На 2026-09-17 поверх fb7678d локально реализованы T0 и Iris-части T1/T2/T3: полный suite 50/50
прошёл (Qt 6.10.2, system QCA3, SRTP/SCTP enabled, последовательный CTest).
Включены directionpolicy, directionoperation и sessioncallbacklifetime; реальные peer-to-peer
звонки не выполнялись. Targeted ASan/UBSan/LSan для directionoperation также прошёл:
инструментированы operation/controller/Application/TieBreaker, но не остальные units Iris/Qt.
Изменения пока в worktree, не объявлять их опубликованными или проверенными GitHub CI.
Psi audio adapter также реализован локально поверх c8821dbe: собран target avcall и прошли
4/4 AvCall CTest entries (policy, audiodirection, backend_lifecycle, capability_refresh).
Targeted ASan/UBSan/LSan для production adapter и его теста прошёл; Iris/Qt в этом конкретном
прогоне не инструментированы. Backend tests используют fake provider, это не GStreamer live gate.
Перед публикацией Psi изменений сначала нужен Iris commit с новым API и соответствующий
submodule pin; текущий committed Iris HEAD ещё не содержит worktree T0–T3. Не публиковать
Psi отдельно с несуществующим в pinned Iris API и не утверждать, что remote CI уже это проверил.
Дополнительно исправлена обнаруженная LeakSanitizer утечка ExternalServiceDiscovery:
это теперь QObject child своего Client. Проверка уничтожения включена в sessioncallbacklifetime.
Это не новый полный аудит трёх репозиториев. GStreamer development package пользователь уже установил;
отсутствие dependency больше не текущий блокер, но новых psimedia runtime results здесь нет.
Live calls не подтверждены. Предыдущие Psi/psimedia findings сохраняют прежнюю область проверки.

### PR stack и CI

Не мержить PR без прямого указания пользователя, не ломать stacked branches.

- Iris #92 docs/jingle-architecture → #93 jingle/ice-udp1 → #94 jingle/group-negotiation → #95 jingle/rtp-extensions → #96 jingle/async-media.
- Psi #968 ai/jingle-native-calls — основной call stack.
- psimedia #15 → #16 — stacked работа, semantic packet API head a99960959.
- psimedia jingle/rtcp-session — experimental bridge, просмотренный HEAD b4139cbd.
- Psi #969 ci/psimedia-integration — отдельный real-provider smoke workflow. Перед окончательным merge вернуть нейтральную master/master конфигурацию, не оставлять временные branch pins.

Исторический failing job: 103558912674, Psi 250c1f6a, psimedia cbf3365, GStreamer 1.24.2. Он не характеризует новые исправления. Найти актуальные runs и сверить реальные checkout hashes. Slash branches и ai/** могут пропускаться старыми workflows; смотреть выполненные jobs, не только общий PR status.

Нужные gates: Psi modern, legacy/Win7 profile, cross-repo real psimedia smoke, isolated psimedia tests, Iris standalone QCA3+SRTP. В GitHub CI --parallel 4 допустим. Не создавать дублирующие runs одного и того же patch без причины; локальные сборки Sol не выполняет.

### Дисциплина веток, PR и CI opt-in

Продолжать существующие рабочие ветки и PR stack. Не создавать branch/PR на каждый test,
fixup или попытку CI. Новый PR — только для самостоятельной логической границы или
реально необходимого stack, с кратким объяснением; не мержить без разрешения пользователя.

ai/** специально допускают частые commits без автоматического запуска тяжёлых pipelines.
Это намеренная политика, а не баг. По умолчанию использовать workflow_dispatch на выбранном
checkpoint. Можно отдельно реализовать opt-in по маркеру commit message, например
[run-ci], если это упрощает работу Sol; это пока предложение, не существующая возможность.

Важно: branches-ignore: ai/** / push branches: master отбрасывают событие ДО job if.
Одного contains(head_commit.message, '[run-ci]') в job недостаточно. Если добавлять marker:
разрешить соответствующий push event и gate тяжёлые jobs: non-ai branch ИЛИ marker,
сохранив explicit workflow_dispatch. Для pull_request поля head_commit нет: выбрать
отдельную проверку head commit через API/gate job или оставить ai PR manual-only.
Никогда не подставлять commit text в shell как код; не использовать pull_request_target
для исполнения недоверенного branch code с secrets. Проверить матрицу ai/no-marker,
ai/marker, normal push, ai PR, manual dispatch, зависимые jobs/skips и concurrency.
Документировать стоимость: workflow event может появляться на каждый push, даже если
тяжёлые jobs skipped. Если нужен буквально ноль runs — сохранить manual-only режим.

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

Verification новых исправлений P0 остаётся отдельным gate. Фраза «реализовано» не означает полного закрытия всех lifecycle/interop сценариев.

### Semantic packet API: сохранить достигнутую границу

PRtpPacket::Type имеет underlying int, Rtp=0, Rtcp=1. Сохранены layout/value предпосылки provider boundary; static_asserts и cross-repo smoke полезны, но не доказывают совместимость всех старых бинарных потребителей изменённых методов.

Не возвращать portOffset в adapter/API. psimedia не знает ICE, UDP ports, transport topology и BUNDLE membership. Обе копии публичного wrapper API (Psi/src/psimedia и psimedia/psimedia) должны оставаться согласованными; при несовместимом API отдельно учитывать rebuild/versioning потребителей.

### Где заканчивается текущая реализация

- Production ICE::Pad::connectionFor всё ещё создаёт отдельный IceConnection на Transport. Live BUNDLE не подключён и не должен рекламироваться.
- SharedRtcp — результат router, **не готовый group-level ingress media engine**.
- RtpSessionBridge подключён к production GstRtpSessionContext/RtpWorker packet path; это уже не задача «подключить с нуля».
- Bridge входит в gstprovidersrc SOURCES безусловно: experimental runtime isolation не означает build isolation.
- Один общий send/receive rtpsession на media type уже находится в GstRtpSessionContext; сохранить его при исправлении capture lifecycle.
- rtpsession не заменяет jitter buffer, depayloader/decoder, congestion/retransmission implementation.
- RTCP generation использовать реальным action signal send-rtcp-full, не выдуманной C-функцией.
- JMI, live BUNDLE, полноценный feedback и network recovery остаются дальнейшими этапами.

Смежные документы: [архитектура](jingle.md), [RTP design](jingle-rtp-design.md), [interop status](jingle-calls-interop.md). Исправлять устаревшие status statements без выдуманных результатов и без истории исправленных багов в описании текущей архитектуры.

## 2. Новый audit gate: исправления, доказательства и оставшиеся проблемы

### Уже реализовано — сохранить

- Iris P0.1 subset/reentrancy и P0.6 runtime SSRC retention.
- Iris outgoing requestSenders, queued/in-flight targets, explicit senders XML, Active wakeup,
  destructive notification guards и reentrant newer intent из ACK callback.
- Psi capability transaction теперь общая для production и regression; pure AvCallPolicy
  покрывает capability/transmit/direction predicates. Закрытие incoming dialog отвергает call.
- psimedia construction cleanup, directional caps и owner-thread delivery.
- P1a isolated tests: packet semantics/statistics/SR, periodic RTCP, bounded delivery и teardown.
- Production bridge wiring (62643dc), negotiated PT на payloader, raw Opus format отдельно
  от RTP clock/channels, disabled GstRtpChannel::write mutex fix (3d715ae).
- Late live input attach/detach/reattach и production sender regression добавлены (b4139cb).
  Это не закрытие всей privacy/hotplug/receive/A/V матрицы.

### A1 — arbitration реализована; сначала T0 hardening, не новый framework

Session-owned Jingle::TieBreaker в jingle-tiebreaker.{h,cpp} заменяет protocol-agnostic template.
Application resolver для content-modify проверяет ContentKey и роль. Dispatcher/IQ-boundary
tests уже существуют; больше не утверждать, что dispatcher не умеет tie-break.
Transport-replace намеренно остаётся specialized handler.

Сохранить Continue/Break/Postpone и whole-IQ aggregate Break > Postpone > Continue.
Postpone означает recovery opportunity после error, не откладывание incoming IQ и не replay.
Success не вызывает tie-break retry, но не доказывает удовлетворение более нового intent.
Текущий resolver retry лишь будит _requestedSenders; он не сливает желания в Both.

**T0: реализовано локально — cancellation/reentrancy и arbitration bounds.**
Добавлены pinned SharedState, cancellation epoch, stable snapshots, idempotent terminal outcomes,
Session guards и IQ reply-before-recovery. clear/delete из resolve/retry и удаление Session
через реальный JTPush покрыты regression. Лимиты: 64 resolutions, 4096 XML nodes/attributes,
64 уровня, 256 Ki UTF-16 characters; превышение — отдельный wait/resource-constraint.
Один active outgoing IQ остаётся контрактом Session scheduler (debug assertion).
Также добавлен sessioncallbacklifetime: настоящий Session scheduler отправляет stanza в
записывающий ClientStream без сети, настоящий JT обрабатывает IQ completion. Проверяется
удаление Session из Application completion и из resolver retry. Standalone TieBreaker вместе
с новым regression прошёл ASan/UBSan. Дополнительно directionpolicy прошёл ASan/UBSan/LSan
с инструментированными DirectionController, Application и TieBreaker, включая удаление Pad
из callback. Остальная статическая Iris/Qt не инструментирована: это не полный sanitizer CI.
Дальнейшие adjacent проверки, не повторная реализация T0:

- Полный sanitizer CI для Session/Application/controller и поддерживаемых Qt конфигураций.
- Wire-order regression с завершением local IQ прямо во время incoming handling: recovery
  обязан следовать за actual incoming reply, не только за boolean Applied.
- Generic completion каждого surviving участника multi-content batch, в том числе resolver
  Continue; корреляция с upcoming operation handles, не только Postpone.
- Для transport migration сохранить bounded hints и не делать transport install внутри resolve.

Подробности и тестовая матрица: [Direction policy design decision](jingle-direction-policy.md).

### A2 — durable policy, finite operation и IQ attempt должны иметь разных владельцев

Psi audioPolicyTarget не имеет generic failure completion. Решать по шагам:

1. **T1:** explicit attempt completion с request identity/revision, success/error/timeout/cancel.
   Iris-часть реализована: requestSendersTracked, SendersAttemptResult, sendersAttemptFinished,
   cancelQueuedSenders и sendersAttemptPending. requestSenders сохранён. Первая настоящая ACK
   обновляет negotiated facts; duplicate/stale callback после завершения уже ничего не делает.
   Тест contentmodifycompletion покрывает error/timeout/newer revision/reentrancy/cancel/delete.
   Ещё требуется production Psi integration test; Iris green не закрывает зависание Psi policy.
2. **T2:** RTP Pad-owned DirectionController, один policy writer от Psi плюс scoped constraint
   tokens. Psi владеет device/permission/UI фактами, Iris не выбирает микрофоны. Application
   queued target остаётся disposable attempt snapshot, не авторитетным user desire.
   Минимальная Iris реализация уже в jingle-rtp-directions.{h,cpp}, getter на RTP::Pad,
   немедленный RTP send gate в Application::allowsRtp. Policy status — живое состояние,
   не finite operation. Three-proposal budget ограничивает автоматическую борьбу политик;
   generic failure требует нового явного policy/constraint event, не busy retry.
   directionpolicy проверяет обе роли, constraints/consent, superseded ACK, crossed actions
   с обоими моментами reconcile относительно error, limit успешной борьбы с peer, recreation
   content и удаление Pad/controller из scheduling callback. Не создавать второй controller.
3. **T3:** finite DirectionOperation: per-content progress, Blocked(reason), deadline и one-shot
   queued completion Succeeded/Failed/Cancelled/Superseded. Iris-часть реализована в
   jingle-rtp-direction-operation.cpp, API в jingle-rtp-directions.h. Factory:
   directionController()->requestLocalSending({{content, sending}, ...}, deadlineMs).
   Не 1:1 с IQ; отмена/деструктор/таймаут наблюдателя не откатывают policy. Весь batch
   валидируется до mutations; максимум 64 items и 32 pending handles, deadline по умолчанию
   15 секунд. Policy/item содержат причины блокировки и typed failure + optional Stanza::Error.
   directionoperation покрывает no-op/invalid batch, progress, supersede/late ACK, cancel,
   blocked/recovery/deadline, generic error, capacity, recreation, session destruction при
   удержанном Pad и синхронное удаление handle из callbacks. В Psi audio path подключён
   через AvCallAudioDirection; старые pending state и прямой requestSenders удалены.
4. **T4:** transport-replace resolver у Application/selector, переживающего замену Transport;
   Pad scope только для group-level invariants.
5. **T5:** отдельный staged transport payload API для malformed-batch atomicity.

Обязательные design решения подробно изложены в
[jingle-direction-policy.md](jingle-direction-policy.md); T0–T3 реализованы в Iris,
а T4/T5 и дальнейшие integration gates остаются следующими шагами:

- SetLocalSending и SetExactSenders различаются. Responder → Both после peer Initiator
  корректно только для желания «отправлять самому», не для exact Responder target.
- Consent/constraints закрывают local gate немедленно, не ждут ACK. Снятие device constraint
  не возвращает отозванное разрешение. Не подменять durable user desire hardware-событием.
- Не вводить global last-writer-wins или priority enum User/Recovery. Recovery — trigger
  существующей policy. Для начала single writer + restrictive tokens достаточно.
- Завершённый operation не хранит вечное желание и не воскресает. Cancellation не unsend IQ;
  уничтожение observer handle не должно менять capture policy. Superseded завершает старое
  наблюдение без silent rollback других contents; content identity включает incarnation.
- Succeeded = выполнен predicate текущей revision, а не пришёл ACK. Невозможное желание
  видно как Blocked/Failed; bounded retry, deadline и отсутствие ping-pong обязательны.
- TieBreaker не знает logical operation groups: resolution group только связывает transaction,
  remote outcome и независимо отзываемые registrations.

#### Решения и открытые вопросы следующего этапа (не блокируют текущую разработку)

- Текущий budget controller — три scheduled proposals на поколение policy/constraints.
  Это консервативная защита от ping-pong, не требование XEP. При появлении реальной потребности
  вынести в validated policy settings; не снимать ограничение ради успешного теста.
- Satisfied относится к текущему предикату directions; это не подтверждение media connectivity.
  До Connecting локальная proposal не считается согласованным успешным operation.
- T3 выполнен: не создавать ещё один Intent/Operation framework. Handle — только finite
  observer принятых policy revisions. Supersession не откатывает unaffected siblings,
  finished вызывается один раз после установки всех snapshots. Ошибка item имеет приоритет
  над supersession, а остальные Pending/Blocked items при прекращении batch observation
  становятся Cancelled/ObservationEnded. Эти исходы не меняют durable policy.
- Controller Policy и operation item уже сохраняют typed failure и optional signaling error;
  constraint reason — диагностическая строка, не числовой приоритет policy writers.
- Psi audio path мигрирован: audioPolicyTarget/audioDesiredWithCapture удалены, как и старые
  shouldRequestSenders/reconcilePolicyTarget helpers. Один controller writer и device token
  находятся в AvCallAudioDirection; не возвращать параллельный signaling policy path.
  Начальную local-send preference брать из явного call/user intent; negotiated peer update
  не должен становиться новым локальным consent. Для receive-only initial offer не включать
  микрофон автоматически лишь потому, что captureAudioConsent разрешает его использование.
- Передача local policy в Iris не отменяет синхронное выключение capture в psimedia. Packet
  gate защищает отправку, но не индикатор микрофона и фактический сбор данных устройством.

#### Production Psi integration: реализовано локально, remaining gates

1. `src/avcall/avcall.cpp` использует `AvCallAudioDirection` из `avcallaudiodirection.{h,cpp}`
   как adapter к существующему `RTP::Pad::directionController()`.
   Не путать роль пользователя и wire mask: исходящий audio call явно желает local send;
   принимаемый receive-only offer не создаёт такого желания автоматически. Решение принимать
   при исходном call/accept action, а не при `sendersChangedByPeer`.
2. Для принятого content уже передаётся начальное желание через requestLocalSending и хранится
   finite handle. Для hotplug используется RAII constraint token с reason;
   потеря устройства не заменяет durable желание на false. Возврат снимает лишь свой token.
   Повторный capabilitiesChanged без изменения фактов не должен создавать revision/retry.
3. `syncActiveTransmit` учитывает consent, реальное устройство, negotiated local bit
   и controller gate. Локальный changed callback вызывает capture controls сразу, без ACK.
   Не ограничиваться RTP packet dropping при живом capture pipeline.
4. Adapter подписан на policyChanged и finished, хранит последний handle для чтения outcome.
   Generic failure/timeout не должны оставлять pending target или порождать авто retry loop.
   Наблюдатель может истечь при отсутствии устройства; его timeout не отзывает разрешение.
   Новое явное действие или изменение constraint может запустить следующее наблюдение.
5. Обход через прямой requestSenders для managed audio удалён. Peer notification обновляет
   negotiated facts, но не выдаёт consent и не перезаписывает пользовательское желание.
   Не расширять молча эту миграцию на camera/hold без определения их caller policy.
6. `src/avcall/unittest/audiodirection.cpp` использует именно production adapter плюс реальные
   Iris Application/Pad/DirectionController. Он покрывает error/timeout, receive-only,
   loss/return с pending IQ, consent revoke, explicit retry после failure, recreated content
   и reentrant adapter destruction. Fake Transport/Task дают signaling boundary без сети.
   Это не полноценный тест AvCall UI или реального capture pipeline. Ubuntu CI regex включает
   новый avcallaudiodirection_test; запуск CI и live capture пока не подтверждены.

Осталось для A2 acceptance: cross-repo real-psimedia gate на точных heads и проверка реального
звонка/микрофона. Отдельно нужна UI-подача failed/blocked operation (сейчас adapter сохраняет
outcome, но не показывает новый диалог). Не завершать весь звонок из-за observation timeout:
это не Session negotiation deadline. Camera/hold/permission UX не добавлять без отдельного
определения их user policy. Следующий Iris implementation этап — T4 transport-replace resolver,
после него отдельный T5 staged payload API; соответствующие safety gates ниже сохраняются.

Transport migration сохраняет validated sibling hints getAlikeTransport/selectNextTransport,
partial supported/unsupported outcomes и существующие reentrancy/identity regressions.
Historical efficiency не оправдывает side effects от malformed sibling. T5 требует настоящего
prepare/validate → staged update → identity/generation check → commit, не повторного вызова
старого mutating update после формальной bool-проверки.

### A3 [P1 privacy, оставшийся старый gap] Live/file switch оставляет прежний capture source

MEDIA/gstprovider/rtpworker_devices.cpp: setInputDevices; rtpworker.cpp: setupSendRecv;
gstrtpsessioncontext.cpp: setFileInput/setFileDataInput/setAudioInputDevice.

Rebuild выполняется только если и прежний, и новый source — live (пустые infile/indata).
При live microphone → file/data выбор новых полей не пересоздаёт существующий sendbin:
setupSendRecv пропускает startSend, поэтому старый mic может продолжать capture/output.
Обратный переход тоже оставляет старый источник. Это не новая доказанная регрессия b4139cb,
но privacy-цель нового механизма не закрыта для публичного source-selection API.

Сравнивать полноценную source identity/mode (none/live/file/data), не только ain/vin.
Перед commit нового режима отозвать старый capture/output, затем применить новый либо
остаться без capture с явной ошибкой. Если live file switching пока не поддерживается —
явно отказать и прекратить старый capture, не молча сохранить mic.
Regression: nonfinite live source → file → live, file-data замена, invalid new source,
detach, rapid updates, cancel во время rebuild. Проверять остановку/освобождение source,
а не только отсутствие RTP: pauseAudio сам по себе лишь packet gate.

### A4 [P2 architecture/coverage] Hotplug reset слишком широк для заявленной независимости media

setInputDevices вызывает cleanup(), который удаляет sendbin И recvbin, audio/video sources,
audio sink и меняет clocks. Смена одного mic затрагивает playback и video, а не только input.
Нынешний sender test не проверяет receive, соседнее media или физическое закрытие mic;
finite first source уже достигает EOS до detach, поэтому это слабый privacy oracle.

Не объявлять бесшовный hotplug по этому тесту. Предпочтительно выделить controlled
send/source lifecycle и сохранять unaffected receive/video; если общий reset пока необходим,
задокументировать interruption и протестировать восстановление всех направлений.
Согласовать clocks/base-time, SSRC sequence/timestamp continuity либо явную смену SSRC,
SR state, queued packets/generation и bounded failure. Не сохранять rtpsession state
формально, игнорируя реальный reset payloader.

Production regression: непрерывные receive audio + send video во время mic swap/detach;
callback/element state подтверждает закрытие старого source; no-capture negotiation;
invalid replacement; отсутствие RTP после revocation и возобновление нового media.
Не переносить этот teardown policy в Iris или создавать второй media engine.

### Дополнительные обязательные проверки P1b, не новые доказанные дефекты

- Legacy byte edge теряет исходные GstBuffer timestamps; bridge ставит time-of-arrival.
  Проверить SR RTP↔NTP mapping и A/V sync под queueing/encoder delay и hotplug.
- Bounded handoff queues не ограничивают appsrc/appsink целиком; drain loop должен давать
  owner event loop обрабатывать stop/timers при постоянном producer. Проверить byte/age limits.
- Input ID replacement без изменения capability booleans и явная смена настроек пользователем:
  проверить, что реальные notifications доходят до AvCall, а не только pure predicate.
- Runtime bridge bus error должен доходить до provider error path; PLAYING success не
  доказательство дальнейшего здоровья pipeline.
- Pending/new media в уже active call не должно включать capture до собственного
  authenticated Application Active. Проверить реальный controller, не только shouldTransmit.
- SharedRtcp/FCI/XR и shared association остаются P2, не закрыты packet-type tests.

### Housekeeping Sol — выполнена локально после аудита

Удалена неиспользуемая cleanupSend declaration, license headers rtpworker.h/rwcontrol.cpp
возвращены к исходному тексту без несвязанных правок. CTest timeout для
rtpsessioncontext_sender увеличен с 20 до 45 s; внутренние ожидания не менялись.
Проверить публикацию этих локальных изменений на GitHub, не дублировать patch.
Общий deadline внутри теста и actual CI execution остаются verification задачами;
увеличенный timeout сам по себе не исправляет deadlock и не доказывает прохождение теста.

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

Начать с remote heads и CI evidence через connector. Проверить, опубликован ли локальный
checkpoint T0/T1/T2/T3 и Psi audio adapter, и не реализовывать их повторно. Следующий Iris
этап — T4, а для A2 требуются оставшиеся cross-repo/live gates; параллельные policy owners не оставлять.
A3/A4 в psimedia остаются отдельными задачами. T4/T5 вести отдельными ограниченными шагами,
не повторять опубликованные subset fixes. Затем P1a isolated runtime → P1b production worker →
P1c native peer checks. P2 live BUNDLE, P3 JMI, P4 feedback/control, P5 recovery и P6 release
выполняются по зависимостям наблюдённого peer. Не создавать существующие interfaces заново.

Каждый завершённый подпункт сопровождать:

1. Что изменено, в каких файлах, почему это устраняет указанный сценарий.
2. Regression test, который выявляет дефект на исходной версии.
3. CI run/job URLs, exact checkout SHA, исполненные тесты и результаты; явно skipped/blocked, без выдуманных локальных/live tests.
4. Изменения ownership/threading/API и оставшиеся ограничения.
5. Обновление interop/design docs только до фактически достигнутого состояния.

Если установленный provider отличается от прочитанного, сначала сверить контракт. Если решение отличается от предложенного, обосновать кодом/тестом/спецификацией. Допустима более простая реализация с теми же гарантиями; недопустимо обходить gate, возвращать фиктивную готовность или игнорировать ошибку ради соединения.

Окончательная совместимость — подтверждённые реальные звонки на зафиксированном peer плюс negative/lifecycle tests, а не успешная компиляция и не количество новых классов.
