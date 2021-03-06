/*
 *  The Mana Server
 *  Copyright (C) 2004-2010  The Mana World Development Team
 *
 *  This file is part of The Mana Server.
 *
 *  The Mana Server is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana Server is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana Server.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <enet/enet.h>

#include "net/messagein.hpp"

MessageIn::MessageIn(const char *data, int length):
    mData(data),
    mLength(length),
    mPos(0)
{
    // Read the message ID
    mId = readShort();
}

int MessageIn::readByte()
{
    int value = -1;
    if (mPos < mLength)
    {
        value = (unsigned char) mData[mPos];
    }
    mPos += 1;
    return value;
}

int MessageIn::readShort()
{
    int value = -1;
    if (mPos + 2 <= mLength)
    {
        uint16_t t;
        memcpy(&t, mData + mPos, 2);
        value = (unsigned short) ENET_NET_TO_HOST_16(t);
    }
    mPos += 2;
    return value;
}

int MessageIn::readLong()
{
    int value = -1;
    if (mPos + 4 <= mLength)
    {
        uint32_t t;
        memcpy(&t, mData + mPos, 4);
        value = ENET_NET_TO_HOST_32(t);
    }
    mPos += 4;
    return value;
}

std::string MessageIn::readString(int length)
{
    // Get string length
    if (length < 0)
    {
        length = readShort();
    }

    // Make sure the string isn't erroneous
    if (length < 0 || mPos + length > mLength)
    {
        mPos = mLength + 1;
        return std::string();
    }

    // Read the string
    const char *stringBeg = mData + mPos;
    const char *stringEnd = (const char *)memchr(stringBeg, '\0', length);
    std::string readString(stringBeg,
            stringEnd ? stringEnd - stringBeg : length);
    mPos += length;

    return readString;
}

std::ostream&
operator <<(std::ostream &os, const MessageIn &msg)
{
    os << std::setw(6) << std::hex << std::showbase << std::internal
       << std::setfill('0') << msg.getId()
       << std::dec << " (" << msg.getLength() << " B)";
    return os;
}
