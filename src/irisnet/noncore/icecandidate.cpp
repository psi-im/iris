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

#include "icecandidate.h"
#include <QUuid>

namespace XMPP::ICE {

CandidateInfo::Ptr CandidateInfo::makeRemotePrflx(int componentId, const TransportAddress &fromAddr, quint32 priority)
{
    auto c  = std::make_shared<CandidateInfo>();
    c->addr = fromAddr;
    c->addr.addr.setScopeId(QString());
    c->type        = PeerReflexiveType;
    c->priority    = priority;
    c->foundation  = QUuid::createUuid().toString();
    c->componentId = componentId;
    c->network     = -1;
    return c;
}

}
