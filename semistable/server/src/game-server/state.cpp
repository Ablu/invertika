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

#include <cassert>

#include "game-server/state.hpp"

#include "point.h"
#include "common/configuration.hpp"
#include "game-server/accountconnection.hpp"
#include "game-server/gamehandler.hpp"
#include "game-server/inventory.hpp"
#include "game-server/item.hpp"
#include "game-server/itemmanager.hpp"
#include "game-server/effect.hpp"
#include "game-server/map.hpp"
#include "game-server/mapcomposite.hpp"
#include "game-server/mapmanager.hpp"
#include "game-server/monster.hpp"
#include "game-server/npc.hpp"
#include "game-server/trade.hpp"
#include "net/messageout.hpp"
#include "scripting/script.hpp"
#include "utils/logger.h"

enum
{
    EVENT_REMOVE = 0,
    EVENT_INSERT,
    EVENT_WARP
};

/**
 * Event expected to happen at next update.
 */
struct DelayedEvent
{
    unsigned short type, x, y;
    MapComposite *map;
};

typedef std::map< Actor *, DelayedEvent > DelayedEvents;

/**
 * List of delayed events.
 */
static DelayedEvents delayedEvents;

/**
 * Updates object states on the map.
 */
static void updateMap(MapComposite *map)
{
    // 1. update object status.
    const std::vector< Thing * > &things = map->getEverything();
    for (std::vector< Thing * >::const_iterator i = things.begin(),
         i_end = things.end(); i != i_end; ++i)
    {
        (*i)->update();
    }

    // 2. run scripts.
    if (Script *s = map->getScript())
    {
        s->update();
    }

    // 3. perform actions.
    for (BeingIterator i(map->getWholeMapIterator()); i; ++i)
    {
        (*i)->perform();
    }

    // 4. move objects around and update zones.
    for (BeingIterator i(map->getWholeMapIterator()); i; ++i)
    {
        (*i)->move();
    }
    map->update();
}

/**
 * Sets message fields describing character look.
 */
static void serializeLooks(Character *ch, MessageOut &msg, bool full)
{
    const Possessions &poss = ch->getPossessions();
    static int const nb_slots = 4;
    static int const slots[nb_slots] =
    {
        EQUIP_FIGHT1_SLOT,
        EQUIP_HEAD_SLOT,
        EQUIP_TORSO_SLOT,
        EQUIP_LEGS_SLOT
    };

    // Bitmask describing the changed entries.
    int changed = (1 << nb_slots) - 1;
    if (!full)
    {
        // TODO: do not assume the whole equipment changed, when an update is asked for.
        changed = (1 << nb_slots) - 1;
    }

    int items[nb_slots];
    // Partially build both kinds of packet, to get their sizes.
    int mask_full = 0, mask_diff = 0;
    int nb_full = 0, nb_diff = 0;
    for (int i = 0; i < nb_slots; ++i)
    {
        int id = poss.equipment[slots[i]];
        ItemClass *eq;
        items[i] = id && (eq = ItemManager::getItem(id)) ? eq->getSpriteID() : 0;
        if (changed & (1 << i))
        {
            // Skip slots that have not changed, when sending an update.
            ++nb_diff;
            mask_diff |= 1 << i;
        }
        if (items[i])
        {
            /* If we are sending the whole equipment, only filled slots have to
               be accounted for, as the other ones will be automatically cleared. */
            ++nb_full;
            mask_full |= 1 << i;
        }
    }

    // Choose the smaller payload.
    if (nb_full <= nb_diff) full = true;

    /* Bitmask enumerating the sent slots.
       Setting the upper bit tells the client to clear the slots beforehand. */
    int mask = full ? mask_full | (1 << 7) : mask_diff;

    msg.writeByte(mask);
    for (int i = 0; i < nb_slots; ++i)
    {
        if (mask & (1 << i)) msg.writeShort(items[i]);
    }
}

/**
 * Informs a player of what happened around the character.
 */
static void informPlayer(MapComposite *map, Character *p)
{
    MessageOut moveMsg(GPMSG_BEINGS_MOVE);
    MessageOut damageMsg(GPMSG_BEINGS_DAMAGE);
    const Point &pold = p->getOldPosition(), ppos = p->getPosition();
    int pid = p->getPublicID(), pflags = p->getUpdateFlags();
    int visualRange = Configuration::getValue("visualRange", 320);

    // Inform client about activities of other beings near its character
    for (BeingIterator i(map->getAroundBeingIterator(p, visualRange)); i; ++i)
    {
        Being *o = *i;

        const Point &oold = o->getOldPosition(), opos = o->getPosition();
        int otype = o->getType();
        int oid = o->getPublicID(), oflags = o->getUpdateFlags();
        int flags = 0;

        // Check if the character p and the moving object o are around.
        bool wereInRange = pold.inRangeOf(oold, visualRange) &&
                           !((pflags | oflags) & UPDATEFLAG_NEW_ON_MAP);
        bool willBeInRange = ppos.inRangeOf(opos, visualRange);

        if (!wereInRange && !willBeInRange)
        {
            // Nothing to report: o and p are far away from each other.
            continue;
        }


        if (wereInRange && willBeInRange)
        {
            // Send attack messages.
            if ((oflags & UPDATEFLAG_ATTACK) && oid != pid)
            {
                MessageOut AttackMsg(GPMSG_BEING_ATTACK);
                AttackMsg.writeShort(oid);
                AttackMsg.writeByte(o->getDirection());
                AttackMsg.writeByte(static_cast< Being * >(o)->getAttackType());
                gameHandler->sendTo(p, AttackMsg);
            }

            // Send action change messages.
            if ((oflags & UPDATEFLAG_ACTIONCHANGE))
            {
                MessageOut ActionMsg(GPMSG_BEING_ACTION_CHANGE);
                ActionMsg.writeShort(oid);
                ActionMsg.writeByte(static_cast< Being * >(o)->getAction());
                gameHandler->sendTo(p, ActionMsg);
            }

            // Send looks change messages.
            if (oflags & UPDATEFLAG_LOOKSCHANGE)
            {
                MessageOut LooksMsg(GPMSG_BEING_LOOKS_CHANGE);
                LooksMsg.writeShort(oid);
                Character * c = static_cast<Character * >(o);
                serializeLooks(c, LooksMsg, false);
                LooksMsg.writeShort(c->getHairStyle());
                LooksMsg.writeShort(c->getHairColor());
                LooksMsg.writeShort(c->getGender());
                gameHandler->sendTo(p, LooksMsg);
            }

            // Send direction change messages.
            if (oflags & UPDATEFLAG_DIRCHANGE)
            {
                MessageOut DirMsg(GPMSG_BEING_DIR_CHANGE);
                DirMsg.writeShort(oid);
                DirMsg.writeByte(o->getDirection());
                gameHandler->sendTo(p, DirMsg);
            }

            // Send damage messages.
            if (o->canFight())
            {
                Being *victim = static_cast< Being * >(o);
                const Hits &hits = victim->getHitsTaken();
                for (Hits::const_iterator j = hits.begin(),
                     j_end = hits.end(); j != j_end; ++j)
                {
                    damageMsg.writeShort(oid);
                    damageMsg.writeShort(*j);
                }
            }

            if (oold == opos)
            {
                // o does not move, nothing more to report.
                continue;
            }
        }

        if (!willBeInRange)
        {
            // o is no longer visible from p. Send leave message.
            MessageOut leaveMsg(GPMSG_BEING_LEAVE);
            leaveMsg.writeShort(oid);
            gameHandler->sendTo(p, leaveMsg);
            continue;
        }

        if (!wereInRange)
        {
            // o is now visible by p. Send enter message.
            MessageOut enterMsg(GPMSG_BEING_ENTER);
            enterMsg.writeByte(otype);
            enterMsg.writeShort(oid);
            enterMsg.writeByte(static_cast< Being *>(o)->getAction());
            enterMsg.writeShort(opos.x);
            enterMsg.writeShort(opos.y);
            switch (otype)
            {
                case OBJECT_CHARACTER:
                {
                    Character *q = static_cast< Character * >(o);
                    enterMsg.writeString(q->getName());
                    enterMsg.writeByte(q->getHairStyle());
                    enterMsg.writeByte(q->getHairColor());
                    enterMsg.writeByte(q->getGender());
                    serializeLooks(q, enterMsg, true);
                } break;

                case OBJECT_MONSTER:
                {
                    Monster *q = static_cast< Monster * >(o);
                    enterMsg.writeShort(q->getSpecy()->getType());
                    enterMsg.writeString(q->getName());
                } break;

                case OBJECT_NPC:
                {
                    NPC *q = static_cast< NPC * >(o);
                    enterMsg.writeShort(q->getNPC());
                    enterMsg.writeString(q->getName());
                } break;

                default:
                    assert(false); // TODO
            }
            gameHandler->sendTo(p, enterMsg);
        }

        if (opos != oold)
        {
            flags |= MOVING_POSITION;
        }

        // Send move messages.
        moveMsg.writeShort(oid);
        moveMsg.writeByte(flags);
        if (flags & MOVING_POSITION)
        {
            moveMsg.writeShort(opos.x);
            moveMsg.writeShort(opos.y);
            // We multiply the sent speed (in tiles per second) by ten
            // to get it within a byte with decimal precision.
            // For instance, a value of 4.5 will be sent as 45.
            moveMsg.writeByte((unsigned short) (o->getSpeed() * 10));
        }
    }

    // Do not send a packet if nothing happened in p's range.
    if (moveMsg.getLength() > 2)
        gameHandler->sendTo(p, moveMsg);

    if (damageMsg.getLength() > 2)
        gameHandler->sendTo(p, damageMsg);

    // Inform client about status change.
    p->sendStatus();

    // Inform client about health change of party members
    for (CharacterIterator i(map->getWholeMapIterator()); i; ++i)
    {
        Character *c = *i;

        // Make sure its not the same character
        if (c == p)
            continue;

        // make sure they are in the same party
        if (c->getParty() == p->getParty())
        {
            int cflags = c->getUpdateFlags();
            if (cflags & UPDATEFLAG_HEALTHCHANGE)
            {
                MessageOut healthMsg(GPMSG_BEING_HEALTH_CHANGE);
                healthMsg.writeShort(c->getPublicID());
                healthMsg.writeShort(c->getHealth());
                gameHandler->sendTo(p, healthMsg);
            }
        }
    }

    // Inform client about items on the ground around its character
    MessageOut itemMsg(GPMSG_ITEMS);
    for (FixedActorIterator i(map->getAroundBeingIterator(p, visualRange)); i; ++i)
    {
        assert((*i)->getType() == OBJECT_ITEM ||
               (*i)->getType() == OBJECT_EFFECT);

        Actor *o = *i;
        Point opos = o->getPosition();
        int oflags = o->getUpdateFlags();
        bool willBeInRange = ppos.inRangeOf(opos, visualRange);
        bool wereInRange = pold.inRangeOf(opos, visualRange) &&
                           !((pflags | oflags) & UPDATEFLAG_NEW_ON_MAP);

        if (willBeInRange ^ wereInRange)
        {
            switch (o->getType())
            {
                case OBJECT_ITEM:
                {
                    Item *o = static_cast< Item * >(*i);
                    if (oflags & UPDATEFLAG_NEW_ON_MAP)
                    {
                        /* Send a specific message to the client when an item appears
                           out of nowhere, so that a sound/animation can be performed. */
                        MessageOut appearMsg(GPMSG_ITEM_APPEAR);
                        appearMsg.writeShort(o->getItemClass()->getDatabaseID());
                        appearMsg.writeShort(opos.x);
                        appearMsg.writeShort(opos.y);
                        gameHandler->sendTo(p, appearMsg);
                    }
                    else
                    {
                        itemMsg.writeShort(willBeInRange ? o->getItemClass()->getDatabaseID() : 0);
                        itemMsg.writeShort(opos.x);
                        itemMsg.writeShort(opos.y);
                    }
                }
                break;
                case OBJECT_EFFECT:
                {
                    Effect *o = static_cast< Effect * >(*i);
                    o->show();
                    // Don't show old effects
                    if (!(oflags & UPDATEFLAG_NEW_ON_MAP))
                        break;
                    Being *b = o->getBeing();
                    if (b)
                    {
                        MessageOut effectMsg(GPMSG_CREATE_EFFECT_BEING);
                        effectMsg.writeShort(o->getEffectId());
                        effectMsg.writeShort(b->getPublicID());
                        gameHandler->sendTo(p, effectMsg);
                    } else {
                        MessageOut effectMsg(GPMSG_CREATE_EFFECT_POS);
                        effectMsg.writeShort(o->getEffectId());
                        effectMsg.writeShort(opos.x);
                        effectMsg.writeShort(opos.y);
                        gameHandler->sendTo(p, effectMsg);
                    }
                }
                break;
                default: break;
            } // Switch
        }
    }

    // Do not send a packet if nothing happened in p's range.
    if (itemMsg.getLength() > 2)
        gameHandler->sendTo(p, itemMsg);
}

#ifndef NDEBUG
static bool dbgLockObjects;
#endif

void GameState::update(int worldTime)
{
#   ifndef NDEBUG
    dbgLockObjects = true;
#   endif

    // Update game state (update AI, etc.)
    const MapManager::Maps &maps = MapManager::getMaps();
    for (MapManager::Maps::const_iterator m = maps.begin(), m_end = maps.end(); m != m_end; ++m)
    {
        MapComposite *map = m->second;
        if (!map->isActive())
        {
            continue;
        }

        updateMap(map);

        for (CharacterIterator p(map->getWholeMapIterator()); p; ++p)
        {
            informPlayer(map, *p);
            /*
             sending the whole character is overhead for the database, it should
             be replaced by a syncbuffer. see: game-server/accountconnection:
             AccountConnection::syncChanges()

            if (worldTime % 2000 == 0)
            {
                accountHandler->sendCharacterData(*p);
            }
            */
        }

        for (ActorIterator i(map->getWholeMapIterator()); i; ++i)
        {
            Actor *a = *i;
            a->clearUpdateFlags();
            if (a->canFight())
            {
                static_cast< Being * >(a)->clearHitsTaken();
            }
        }
    }

#   ifndef NDEBUG
    dbgLockObjects = false;
#   endif

    // Take care of events that were delayed because of their side effects.
    for (DelayedEvents::iterator i = delayedEvents.begin(),
         i_end = delayedEvents.end(); i != i_end; ++i)
    {
        const DelayedEvent &e = i->second;
        Actor *o = i->first;
        switch (e.type)
        {
            case EVENT_REMOVE:
                remove(o);
                if (o->getType() == OBJECT_CHARACTER)
                {
                    Character *ch = static_cast< Character * >(o);
                    ch->disconnected();
                    gameHandler->kill(ch);
                }
                delete o;
                break;

            case EVENT_INSERT:
                insertSafe(o);
                break;

            case EVENT_WARP:
                assert(o->getType() == OBJECT_CHARACTER);
                warp(static_cast< Character * >(o), e.map, e.x, e.y);
                break;
        }
    }
    delayedEvents.clear();
}

bool GameState::insert(Thing *ptr)
{
    assert(!dbgLockObjects);
    MapComposite *map = ptr->getMap();
    assert(map && map->isActive());

    /* Non-visible objects have neither position nor public ID, so their
       insertion cannot fail. Take care of them first. */
    if (!ptr->isVisible())
    {
        map->insert(ptr);
        ptr->inserted();
        return true;
    }

    // Check that coordinates are actually valid.
    Actor *obj = static_cast< Actor * >(ptr);
    Map *mp = map->getMap();
    Point pos = obj->getPosition();
    if (pos.x / 32 >= (unsigned)mp->getWidth() ||
        pos.y / 32 >= (unsigned)mp->getHeight())
    {
        LOG_ERROR("Tried to insert an actor at position " << pos.x << ','
                  << pos.y << " outside map " << map->getID() << '.');
        // Set an arbitrary small position.
        pos = Point(100, 100);
        obj->setPosition(pos);
    }

    if (!map->insert(obj))
    {
        // The map is overloaded, no room to add a new actor
        LOG_ERROR("Too many actors on map " << map->getID() << '.');
        return false;
    }

    obj->inserted();

    // DEBUG INFO
    switch (obj->getType())
    {
        case OBJECT_ITEM:
            LOG_DEBUG("Item inserted: " << static_cast<Item*>(obj)->getItemClass()->getDatabaseID());
            break;

        case OBJECT_NPC:
            LOG_DEBUG("NPC inserted: " << static_cast<NPC*>(obj)->getNPC());
            break;

        case OBJECT_CHARACTER:
            LOG_DEBUG("Player inserted: " << static_cast<Being*>(obj)->getName());
            break;

        case OBJECT_EFFECT:
            LOG_DEBUG("Effect inserted: " << static_cast<Effect*>(obj)->getEffectId());
            break;

        case OBJECT_MONSTER:
            LOG_DEBUG("Monster inserted: " << static_cast<Monster*>(obj)->getSpecy()->getType());
            break;

        case OBJECT_ACTOR:
        case OBJECT_OTHER:
        default:
            LOG_DEBUG("Thing inserted: " << obj->getType());
    }

    obj->raiseUpdateFlags(UPDATEFLAG_NEW_ON_MAP);
    if (obj->getType() != OBJECT_CHARACTER)
        return true;

    /* Since the player does not know yet where in the world its character is,
       we send a map-change message, even if it is the first time it
       connects to this server. */
    MessageOut mapChangeMessage(GPMSG_PLAYER_MAP_CHANGE);
    mapChangeMessage.writeString(map->getName());
    mapChangeMessage.writeShort(pos.x);
    mapChangeMessage.writeShort(pos.y);
    gameHandler->sendTo(static_cast< Character * >(obj), mapChangeMessage);

    // update the online state of the character
    accountHandler->updateOnlineStatus(
        static_cast< Character * >(obj)->getDatabaseID(), true);

    return true;
}

bool GameState::insertSafe(Thing *ptr)
{
    if (insert(ptr)) return true;
    delete ptr;
    return false;
}

void GameState::remove(Thing *ptr)
{
    assert(!dbgLockObjects);
    MapComposite *map = ptr->getMap();
    int visualRange = Configuration::getValue("visualRange", 320);

    ptr->removed();

    // DEBUG INFO
    switch (ptr->getType())
    {
        case OBJECT_ITEM:
            LOG_DEBUG("Item removed: " << static_cast<Item*>(ptr)->getItemClass()->getDatabaseID());
            break;

        case OBJECT_NPC:
            LOG_DEBUG("NPC removed: " << static_cast<NPC*>(ptr)->getNPC());
            break;

        case OBJECT_CHARACTER:
            LOG_DEBUG("Player removed: " << static_cast<Being*>(ptr)->getName());
            break;

        case OBJECT_EFFECT:
            LOG_DEBUG("Effect removed: " << static_cast<Effect*>(ptr)->getEffectId());
            break;

        case OBJECT_MONSTER:
            LOG_DEBUG("Monster removed: " << static_cast<Monster*>(ptr)->getSpecy()->getType());
            break;

        case OBJECT_ACTOR:
        case OBJECT_OTHER:
        default:
            LOG_DEBUG("Thing removed: " << ptr->getType());
    }

    if (ptr->canMove())
    {
        if (ptr->getType() == OBJECT_CHARACTER)
        {
            static_cast< Character * >(ptr)->cancelTransaction();

            // remove characters online status
            accountHandler->updateOnlineStatus(
                static_cast< Character * >(ptr)->getDatabaseID(), false);
        }

        Actor *obj = static_cast< Actor * >(ptr);
        MessageOut msg(GPMSG_BEING_LEAVE);
        msg.writeShort(obj->getPublicID());
        Point objectPos = obj->getPosition();

        for (CharacterIterator p(map->getAroundActorIterator(obj, visualRange)); p; ++p)
        {
            if (*p != obj && objectPos.inRangeOf((*p)->getPosition(), visualRange))
            {
                gameHandler->sendTo(*p, msg);
            }
        }
    }
    else if (ptr->getType() == OBJECT_ITEM)
    {
        Item *obj = static_cast< Item * >(ptr);
        Point pos = obj->getPosition();
        MessageOut msg(GPMSG_ITEMS);
        msg.writeShort(0);
        msg.writeShort(pos.x);
        msg.writeShort(pos.y);

        for (CharacterIterator p(map->getAroundActorIterator(obj, visualRange)); p; ++p)
        {
            if (pos.inRangeOf((*p)->getPosition(), visualRange))
            {
                gameHandler->sendTo(*p, msg);
            }
        }
    }

    map->remove(ptr);
}

void GameState::warp(Character *ptr, MapComposite *map, int x, int y)
{
    remove(ptr);
    ptr->setMap(map);
    ptr->setPosition(Point(x, y));
    ptr->clearDestination();
    /* Force update of persistent data on map change, so that
       characters can respawn at the start of the map after a death or
       a disconnection. */
    accountHandler->sendCharacterData(ptr);

    if (map->isActive())
    {
        if (!insert(ptr))
        {
            ptr->disconnected();
            gameHandler->kill(ptr);
            delete ptr;
        }
    }
    else
    {
        MessageOut msg(GAMSG_REDIRECT);
        msg.writeLong(ptr->getDatabaseID());
        accountHandler->send(msg);
        gameHandler->prepareServerChange(ptr);
    }
}

/**
 * Enqueues an event. It will be executed at end of update.
 */
static void enqueueEvent(Actor *ptr, const DelayedEvent &e)
{
    std::pair< DelayedEvents::iterator, bool > p =
        delayedEvents.insert(std::make_pair(ptr, e));
    // Delete events take precedence over other events.
    if (!p.second && e.type == EVENT_REMOVE)
    {
        p.first->second.type = EVENT_REMOVE;
    }
}

void GameState::enqueueInsert(Actor *ptr)
{
    DelayedEvent e = { EVENT_INSERT, 0, 0, 0 };
    enqueueEvent(ptr, e);
}

void GameState::enqueueRemove(Actor *ptr)
{
    DelayedEvent e = { EVENT_REMOVE, 0, 0, 0 };
    enqueueEvent(ptr, e);
}

void GameState::enqueueWarp(Character *ptr, MapComposite *m, int x, int y)
{
    DelayedEvent e = { EVENT_WARP, x, y, m };
    enqueueEvent(ptr, e);
}

void GameState::sayAround(Actor *obj, const std::string &text)
{
    Point speakerPosition = obj->getPosition();
    int visualRange = Configuration::getValue("visualRange", 320);

    for (CharacterIterator i(obj->getMap()->getAroundActorIterator(obj, visualRange)); i; ++i)
    {
        if (speakerPosition.inRangeOf((*i)->getPosition(), visualRange))
        {
            sayTo(*i, obj, text);
        }
    }
}

void GameState::sayTo(Actor *destination, Actor *source, const std::string &text)
{
    if (destination->getType() != OBJECT_CHARACTER)
        return; //only characters will read it anyway

    MessageOut msg(GPMSG_SAY);
    if (source == NULL) {
        msg.writeShort(0);
    } else if (!source->canMove()) {
        msg.writeShort(65535);
    } else {
        msg.writeShort(static_cast< Actor * >(source)->getPublicID());
    }
    msg.writeString(text);

    gameHandler->sendTo(static_cast< Character * >(destination), msg);
}

void GameState::sayToAll(const std::string &text)
{
    MessageOut msg(GPMSG_SAY);

    // message will show as from server
    msg.writeShort(0);
    msg.writeString(text);

    // sends to everyone connected to the game server
    gameHandler->sendToEveryone(msg);
}
