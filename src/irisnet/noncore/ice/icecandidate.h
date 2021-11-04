/*
 * Copyright (C) 2021 Psi IM Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <memory>

#include "iceabstractstundisco.h"
#include "transportaddress.h"

namespace XMPP::ICE {

enum CandidateType { HostType, PeerReflexiveType, ServerReflexiveType, RelayedType };

class CandidateInfo {
public:
    using Ptr = std::shared_ptr<CandidateInfo>;

    CandidateType type;
    int           priority;
    int           componentId;
    int           network;

    TransportAddress addr;    // address according to candidate type
    TransportAddress base;    // network interface address
    TransportAddress related; // not used in agent but useful for diagnostics

    QString foundation;
    QString id;

    AbstractStunDisco::Service::Ptr stunHost; // for rflx/turn candidates

    static inline Ptr make() { return std::make_shared<CandidateInfo>(); }
    static Ptr        makeRemotePrflx(int componentId, const TransportAddress &fromAddr, quint32 priority);
    inline bool operator==(const CandidateInfo &o) const { return addr == o.addr && componentId == o.componentId; }
    inline bool operator==(CandidateInfo::Ptr o) const { return *this == *o; }
};

}
