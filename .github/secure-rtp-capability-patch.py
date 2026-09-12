from pathlib import Path


def replace(path, old, new):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new), encoding="utf-8")

replace(
    "src/xmpp/xmpp-im/jingle-rtp-srtp.h",
    "IRIS_EXPORT DatagramKind classifyDatagram(const QByteArray &);\n",
    "IRIS_EXPORT DatagramKind classifyDatagram(const QByteArray &);\n"
    "// Runtime DTLS-SRTP profiles usable by both the loaded QCA provider and\n"
    "// the linked libSRTP backend. An empty list means native secure RTP is\n"
    "// unavailable; plain DTLS may still be usable for SCTP/data channels.\n"
    "IRIS_EXPORT QStringList supportedSecureRtpProfiles();\n",
)

replace(
    "src/xmpp/xmpp-im/jingle-rtp-srtp.cpp",
    "QStringList        SrtpContext::supportedProfiles()\n{\n    QStringList result;\n#ifdef IRIS_HAVE_SRTP\n    if (!initializeLibrary())\n        return result;\n    for (const auto &profile : profiles) {\n        QCA::SecureArray probe(profile.key + profile.salt);\n        std::memset(probe.data(), 0, size_t(probe.size()));\n        // Probe the actual crypto backend, not just libSRTP policy constants.\n        auto context = create(profile, probe, true);\n        if (context) {\n            result.append(QLatin1String(profile.name));\n            srtp_dealloc(context);\n        }\n    }\n#endif\n    return result;\n}\n",
    "QStringList        SrtpContext::supportedProfiles()\n{\n    QStringList result;\n#ifdef IRIS_HAVE_SRTP\n    if (!initializeLibrary())\n        return result;\n    for (const auto &profile : profiles) {\n        QCA::SecureArray probe(profile.key + profile.salt);\n        std::memset(probe.data(), 0, size_t(probe.size()));\n        // Probe the actual crypto backend, not just libSRTP policy constants.\n        auto context = create(profile, probe, true);\n        if (context) {\n            result.append(QLatin1String(profile.name));\n            srtp_dealloc(context);\n        }\n    }\n#endif\n    return result;\n}\n\nQStringList supportedSecureRtpProfiles()\n{\n    if (!Dtls::isSupported())\n        return {};\n    auto       result       = SrtpContext::supportedProfiles();\n    const auto dtlsProfiles = Dtls::supportedSRTPProfiles();\n    for (auto it = result.begin(); it != result.end();) {\n        if (!dtlsProfiles.contains(*it))\n            it = result.erase(it);\n        else\n            ++it;\n    }\n    return result;\n}\n",
)

replace(
    "src/xmpp/xmpp-im/jingle-ice.cpp",
    "        auto       profiles     = RTP::SrtpContext::supportedProfiles();\n        const auto dtlsProfiles = Dtls::supportedSRTPProfiles();\n        for (auto it = profiles.begin(); it != profiles.end();) {\n            if (!dtlsProfiles.contains(*it))\n                it = profiles.erase(it);\n            else\n                ++it;\n        }\n        if (profiles.isEmpty())\n",
    "        auto profiles = RTP::supportedSecureRtpProfiles();\n        if (profiles.isEmpty())\n",
)

replace(
    "tests/jingle/dtlssrtp.cpp",
    "#ifdef IRIS_TEST_SRTP\n    check(!XMPP::Jingle::RTP::SrtpContext::supportedProfiles().isEmpty(), \"libSRTP backend unavailable\");\n#endif\n#if QCA_MAJOR_VERSION >= 3\n",
    "#ifdef IRIS_TEST_SRTP\n    check(!XMPP::Jingle::RTP::SrtpContext::supportedProfiles().isEmpty(), \"libSRTP backend unavailable\");\n#endif\n#if QCA_MAJOR_VERSION >= 3\n    check(!XMPP::Jingle::RTP::supportedSecureRtpProfiles().isEmpty(),\n          \"no common runtime DTLS-SRTP profile\");\n",
)
