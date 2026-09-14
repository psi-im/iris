from pathlib import Path

ROOT = Path("target")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label}: anchor not found")
    return text.replace(old, new, 1)


def ensure_newline(path: Path) -> None:
    data = path.read_text()
    path.write_text(data.rstrip("\n") + "\n")
    if not path.read_bytes().endswith(b"\n"):
        raise SystemExit(f"missing final newline: {path}")


# ---------------------------------------------------------------------------
# TieBreaker: retry context keeps both proposals; every matching resolver is
# consulted, with Break dominating aggregate outcome without short-circuiting.
# ---------------------------------------------------------------------------
h = ROOT / "src/xmpp/xmpp-im/jingle-tiebreaker.h"
text = h.read_text()
text = replace_once(text, "#include <memory>\n", "#include <memory>\n#include <optional>\n", "tiebreaker optional include")
text = replace_once(
    text,
    "        struct RetryContext {\n  const QDomElement   &localData;\n  const Stanza::Error &localError;\n  RemoteResult         remoteResult;\n        };\n",
    "        struct RetryContext {\n  const QDomElement   &localData;\n  const QDomElement   &remoteData;\n  const Stanza::Error &localError;\n  RemoteResult         remoteResult;\n        };\n",
    "retry context remote data",
)
h.write_text(text)

cpp = ROOT / "src/xmpp/xmpp-im/jingle-tiebreaker.cpp"
text = cpp.read_text()
text = replace_once(text, "#include <QHash>\n", "#include <QHash>\n#include <QMap>\n", "tiebreaker qmap include")
text = replace_once(
    text,
    "        struct PendingResolution {\n  quint64                     id = 0;\n  quint64                     transaction = 0;\n  QList<quint64>              resolvers;\n  std::optional<RemoteResult> remoteResult;\n        };\n\n        QHash<quint64, ResolverEntry>      resolvers;\n",
    "        struct PendingResolution {\n  quint64                     id = 0;\n  quint64                     transaction = 0;\n  QDomElement                 remoteData;\n  QList<quint64>              resolvers;\n  std::optional<RemoteResult> remoteResult;\n        };\n\n        QMap<quint64, ResolverEntry>       resolvers;\n",
    "pending resolution remote data",
)
text = replace_once(
    text,
    "  const auto localData     = transaction->localData;\n  const auto localError    = *transaction->error;\n  const auto remoteResult  = *resolution->remoteResult;\n",
    "  const auto localData     = transaction->localData;\n  const auto remoteData    = resolution->remoteData;\n  const auto localError    = *transaction->error;\n  const auto remoteResult  = *resolution->remoteResult;\n",
    "retry copies remote data",
)
text = replace_once(
    text,
    "  const RetryContext context { localData, localError, remoteResult };\n",
    "  const RetryContext context { localData, remoteData, localError, remoteResult };\n",
    "retry context init",
)
old = """        QList<quint64> postponed;\n        for (auto resolverId : resolverIds) {\n  auto entry = state_->resolvers.find(resolverId);\n  if (entry == state_->resolvers.end() || !entry->resolver)\n      continue;\n  const auto solution = entry->resolver->resolve(transaction->localData, remoteData);\n  if (solution == Solution::Break)\n      return { Solution::Break, 0 };\n  if (solution == Solution::Postpone && state_->resolvers.contains(resolverId))\n      postponed.append(resolverId);\n        }\n        if (postponed.isEmpty())\n  return {};\n\n        const auto id = ++state_->nextResolution;\n        state_->resolutions.insert(id, SharedState::PendingResolution { id, transaction->id, postponed, {} });\n"""
new = """        QList<quint64> postponed;\n        bool           shouldBreak = false;\n        for (auto resolverId : resolverIds) {\n  auto entry = state_->resolvers.find(resolverId);\n  if (entry == state_->resolvers.end() || !entry->resolver)\n      continue;\n  const auto solution = entry->resolver->resolve(transaction->localData, remoteData);\n  if (solution == Solution::Break)\n      shouldBreak = true;\n  else if (solution == Solution::Postpone && state_->resolvers.contains(resolverId))\n      postponed.append(resolverId);\n        }\n\n        // Resolve every registered owner even when one already requested Break:\n        // transport/application resolvers may use resolve() to update local hints.\n        // The aggregate wire decision is still deterministic: Break dominates\n        // Postpone, and Postpone dominates Continue.\n        if (shouldBreak)\n  return { Solution::Break, 0 };\n        if (postponed.isEmpty())\n  return {};\n\n        const auto id = ++state_->nextResolution;\n        state_->resolutions.insert(\n  id, SharedState::PendingResolution { id, transaction->id, remoteData, postponed, {} });\n"""
text = replace_once(text, old, new, "resolve all matching resolvers")
cpp.write_text(text)

# ---------------------------------------------------------------------------
# Pads get a session-scoped convenience accessor, matching the intended
# registration model without requiring Session* to be threaded through APIs.
# ---------------------------------------------------------------------------
jingle_h = ROOT / "src/xmpp/xmpp-im/jingle.h"
text = jingle_h.read_text()
text = replace_once(text, "    class Manager;\n    class Session;\n", "    class Manager;\n    class Session;\n    class TieBreaker;\n", "TieBreaker forward declaration")
text = replace_once(
    text,
    "        virtual QString     ns() const      = 0;\n        virtual Session    *session() const = 0;\n\n",
    "        virtual QString     ns() const      = 0;\n        virtual Session    *session() const = 0;\n        TieBreaker         *tieBreaker() const;\n\n",
    "pad tieBreaker accessor",
)
jingle_h.write_text(text)

jingle_cpp = ROOT / "src/xmpp/xmpp-im/jingle.cpp"
text = jingle_cpp.read_text()
text = replace_once(
    text,
    "    QDomDocument *SessionManagerPad::doc() const { return session()->manager()->client()->doc(); }\n",
    "    QDomDocument *SessionManagerPad::doc() const { return session()->manager()->client()->doc(); }\n\n"
    "    TieBreaker *SessionManagerPad::tieBreaker() const { return session()->tieBreaker(); }\n",
    "pad tieBreaker definition",
)
jingle_cpp.write_text(text)

# ---------------------------------------------------------------------------
# content-modify first consumer. Each Application registers its resolver lazily
# when it first serializes content-modify. The resolver is content-key scoped,
# so unrelated contents using the same action continue normally.
# ---------------------------------------------------------------------------
app_h = ROOT / "src/xmpp/xmpp-im/jingle-application.h"
text = app_h.read_text()
text = replace_once(
    text,
    "#include <iris/xmpp-im/jingle-transport.h>\n",
    "#include <iris/xmpp-im/jingle-transport.h>\n#include <iris/xmpp-im/jingle-tiebreaker.h>\n",
    "application tiebreaker include",
)
text = replace_once(
    text,
    "        Q_DECLARE_FLAGS(ApplicationFlags, ApplicationFlag)\n\n",
    "        Q_DECLARE_FLAGS(ApplicationFlags, ApplicationFlag)\n\n        ~Application() override;\n\n",
    "application destructor declaration",
)
text = replace_once(
    text,
    "        QTimer *transportInitTimer = nullptr;\n    };\n",
    "        QTimer *transportInitTimer = nullptr;\n\n"
    "    private:\n"
    "        class ContentModifyTieBreakResolver;\n"
    "        void ensureContentModifyTieBreakResolver();\n\n"
    "        // Registration is declared after the resolver so it is destroyed\n"
    "        // first and never leaves TieBreaker with a dangling callback.\n"
    "        std::unique_ptr<ContentModifyTieBreakResolver> _contentModifyTieBreakResolver;\n"
    "        TieBreaker::Registration                       _contentModifyTieBreakRegistration;\n"
    "    };\n",
    "application tiebreaker members",
)
app_h.write_text(text)

app_cpp = ROOT / "src/xmpp/xmpp-im/jingle-application.cpp"
text = app_cpp.read_text()
anchor = """    static QString sendersAttribute(Origin senders)\n    {\n        switch (senders) {\n        case Origin::None:\n            return QStringLiteral(\"none\");\n        case Origin::Both:\n            return QStringLiteral(\"both\");\n        case Origin::Initiator:\n            return QStringLiteral(\"initiator\");\n        case Origin::Responder:\n            return QStringLiteral(\"responder\");\n        }\n        return {};\n    }\n\n"""
insert = anchor + """    class Application::ContentModifyTieBreakResolver : public TieBreaker::Resolver {\n    public:\n        explicit ContentModifyTieBreakResolver(Application *application) : application_(application),\n            key_(application->_contentName, application->_creator)\n        {\n        }\n\n        TieBreaker::Solution resolve(const QDomElement &localData, const QDomElement &remoteData) override\n        {\n            if (!contains(localData) || !contains(remoteData))\n                return TieBreaker::Solution::Continue;\n\n            const auto application = application_.data();\n            if (!application || !application->_pad || !application->_pad->session())\n                return TieBreaker::Solution::Continue;\n\n            switch (application->_pad->session()->role()) {\n            case Origin::Initiator:\n                return TieBreaker::Solution::Break;\n            case Origin::Responder:\n                return TieBreaker::Solution::Postpone;\n            default:\n                return TieBreaker::Solution::Continue;\n            }\n        }\n\n        void retry(const TieBreaker::RetryContext &context) override\n        {\n            Q_UNUSED(context);\n            auto application = application_.data();\n            if (!application || application->_state >= State::Finishing || application->_sendersUpdateInFlight\n                || !application->_requestedSenders)\n                return;\n\n            if (*application->_requestedSenders == application->_senders) {\n                application->_requestedSenders.reset();\n                return;\n            }\n            emit application->updated();\n        }\n\n    private:\n        bool contains(const QDomElement &jingle) const\n        {\n            for (auto element = jingle.firstChildElement(); !element.isNull(); element = element.nextSiblingElement()) {\n                const auto name = element.localName().isEmpty() ? element.tagName() : element.localName();\n                if (name != QLatin1String(\"content\"))\n                    continue;\n                const ContentBase content(element);\n                if (content.isValid() && ContentKey { content.name, content.creator } == key_)\n                    return true;\n            }\n            return false;\n        }\n\n        QPointer<Application> application_;\n        ContentKey            key_;\n    };\n\n    Application::~Application() = default;\n\n    void Application::ensureContentModifyTieBreakResolver()\n    {\n        if (_contentModifyTieBreakRegistration || !_pad || !_pad->session())\n            return;\n        _contentModifyTieBreakResolver = std::make_unique<ContentModifyTieBreakResolver>(this);\n        _contentModifyTieBreakRegistration\n            = _pad->tieBreaker()->registerResolver(Action::ContentModify, _contentModifyTieBreakResolver.get());\n    }\n\n"""
text = replace_once(text, anchor, insert, "application content-modify resolver implementation")
text = replace_once(
    text,
    "            const auto requested = *_requestedSenders;\n\n            // XEP-0166 makes senders mandatory for content-modify. ContentBase\n",
    "            const auto requested = *_requestedSenders;\n            ensureContentModifyTieBreakResolver();\n\n            // XEP-0166 makes senders mandatory for content-modify. ContentBase\n",
    "register content-modify resolver",
)
old_cb = """                                        const bool success = task && task->success();\n                                        _sendersUpdateInFlight.reset();\n\n                                        if (success && _senders != requested) {\n                                            _senders = requested;\n                                            QPointer<Application> guard(this);\n                                            emit sendersChanged(requested);\n                                            if (!guard)\n                                                return;\n                                        }\n\n                                        // A failed IQ is not retried forever. On success, inspect the\n                                        // current target after sendersChanged: a signal handler may have\n                                        // synchronously replaced the old target with a newer intent.\n                                        if (!success && _requestedSenders && *_requestedSenders == requested)\n                                            _requestedSenders.reset();\n                                        if (_requestedSenders && *_requestedSenders == _senders)\n                                            _requestedSenders.reset();\n                                        if (_requestedSenders)\n                                            emit updated();\n"""
new_cb = """                                        const bool success   = task && task->success();\n                                        const bool postponed = _contentModifyTieBreakRegistration.isPostponed();\n                                        _sendersUpdateInFlight.reset();\n\n                                        if (success && _senders != requested) {\n                                            _senders = requested;\n                                            QPointer<Application> guard(this);\n                                            emit sendersChanged(requested);\n                                            if (!guard)\n                                                return;\n                                        }\n\n                                        // A generic failure drops an unchanged request as before. A\n                                        // postponed crossed action keeps its local intent until TieBreaker\n                                        // calls retry() after this owner callback has fully completed.\n                                        if (!success && !postponed && _requestedSenders\n                                            && *_requestedSenders == requested)\n                                            _requestedSenders.reset();\n                                        if (_requestedSenders && *_requestedSenders == _senders)\n                                            _requestedSenders.reset();\n                                        if (_requestedSenders && !postponed)\n                                            emit updated();\n"""
text = replace_once(text, old_cb, new_cb, "content-modify ACK postpone semantics")
app_cpp.write_text(text)

# ---------------------------------------------------------------------------
# Generic unit test now includes the dedicated API and verifies that Break does
# not short-circuit later resolvers, plus remote XML reaches retry context.
# ---------------------------------------------------------------------------
test = ROOT / "tests/jingle/tiebreaker.cpp"
text = test.read_text()
text = replace_once(text, "#include <iris/jingle.h>\n", "#include <iris/jingle-tiebreaker.h>\n", "tiebreaker test include")
text = replace_once(
    text,
    "    QString                  remoteName;\n    J::TieBreaker::RemoteResult remoteResult = J::TieBreaker::RemoteResult::Rejected;\n",
    "    QString                  remoteName;\n    QString                  retryRemoteName;\n    J::TieBreaker::RemoteResult remoteResult = J::TieBreaker::RemoteResult::Rejected;\n",
    "retry remote field",
)
text = replace_once(
    text,
    "        localName    = context.localData.attribute(QStringLiteral(\"name\"));\n        remoteResult = context.remoteResult;\n",
    "        localName       = context.localData.attribute(QStringLiteral(\"name\"));\n        retryRemoteName = context.remoteData.attribute(QStringLiteral(\"name\"));\n        remoteResult    = context.remoteResult;\n",
    "retry remote capture",
)
text = replace_once(
    text,
    "        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Applied,\n    \"failed postponed IQ did not retry after owner callback\");\n",
    "        check(resolver.retryCalls == 1 && resolver.remoteResult == J::TieBreaker::RemoteResult::Applied\n                  && resolver.retryRemoteName == QLatin1String(\"remote\"),\n    \"failed postponed IQ did not retry with full collision context after owner callback\");\n",
    "retry full context check",
)
text = replace_once(
    text,
    "        check(result.solution == J::TieBreaker::Solution::Break && result.id == 0,\n    \"Break did not dominate Postpone\");\n        check(!postponeRegistration.isPostponed() && !breakRegistration.isPostponed(),\n    \"Break armed postponed retry state\");\n",
    "        check(result.solution == J::TieBreaker::Solution::Break && result.id == 0,\n    \"Break did not dominate Postpone\");\n        check(postpone.resolveCalls == 1 && breaker.resolveCalls == 1,\n    \"Break short-circuited another resolver instead of evaluating the full action\");\n        check(!postponeRegistration.isPostponed() && !breakRegistration.isPostponed(),\n    \"Break armed postponed retry state\");\n",
    "break no short circuit check",
)
test.write_text(text)

for path in [h, cpp, jingle_h, jingle_cpp, app_h, app_cpp, test]:
    ensure_newline(path)
