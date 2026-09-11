from pathlib import Path

path = Path("src/xmpp/xmpp-im/jingle-session.cpp")
text = path.read_text(encoding="utf-8")
old = '''            remoteGroups = *peerGroups;
            // Session acceptance completes signaling, not transport connectivity.
            state = State::Active;
            startAcceptedContents(apps, true);
            planStep();

            return true;
'''
new = '''            // A session-accept may accept only a subset of the initial offer.
            // Commit omitted-content removal only after the complete answer has
            // parsed successfully, so a malformed later content cannot partially
            // mutate the session. Ordinary content-accept does not use this rule.
            QSet<Application *> accepted;
            for (auto app : std::as_const(apps))
                accepted.insert(app);

            QList<Application *> omitted;
            for (auto app : std::as_const(contentList)) {
                if (app->creator() == role && app->flags().testFlag(Application::InitialApplication)
                    && app->state() == State::Pending && !accepted.contains(app)) {
                    omitted.append(app);
                }
            }

            QPointer<Session> session(q);
            const Reason      omittedReason(Reason::Decline, QStringLiteral("Initial content was not accepted by peer"));
            for (auto app : std::as_const(omitted)) {
                signalingContent.remove(app);
                initialIncomingUnacceptedContent.removeAll(app);
                contentList.remove(ContentKey { app->contentName(), app->creator() });

                QPointer<Application> application(app);
                if (auto transport = app->transport()) {
                    transport->disconnect(app);
                    transport->stop();
                }
                if (!session)
                    return true;
                if (!application)
                    continue;
                application->incomingRemove(omittedReason);
                if (!session)
                    return true;
                if (application)
                    delete application.data();
            }

            remoteGroups = *peerGroups;
            // Session acceptance completes signaling, not transport connectivity.
            state = State::Active;
            startAcceptedContents(apps, true);
            planStep();

            return true;
'''
count = text.count(old)
if count != 1:
    raise SystemExit(f"expected one session-accept commit block, found {count}")
path.write_text(text.replace(old, new), encoding="utf-8")
