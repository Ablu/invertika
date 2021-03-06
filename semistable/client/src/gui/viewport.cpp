/*
 *  The Mana Client
 *  Copyright (C) 2004-2009  The Mana World Development Team
 *  Copyright (C) 2009-2010  The Mana Developers
 *
 *  This file is part of The Mana Client.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gui/viewport.h"

#include "actorspritemanager.h"
#include "client.h"
#include "configuration.h"
#include "graphics.h"
#include "keyboardconfig.h"
#include "localplayer.h"
#include "map.h"
#include "playerinfo.h"
#include "textmanager.h"

#include "gui/gui.h"
#include "gui/ministatus.h"
#include "gui/popupmenu.h"
#include "gui/beingpopup.h"

#include "net/net.h"

#include "resources/resourcemanager.h"

#include "utils/stringutils.h"

extern volatile int tick_time;

Viewport::Viewport():
    mMap(0),
    mMouseX(0),
    mMouseY(0),
    mPixelViewX(0.0f),
    mPixelViewY(0.0f),
    mShowDebugPath(false),
    mPlayerFollowMouse(false),
    mLocalWalkTime(-1),
    mHoverBeing(0),
    mHoverItem(0)
{
    setOpaque(false);
    addMouseListener(this);

    mScrollLaziness = config.getIntValue("ScrollLaziness");
    mScrollRadius = config.getIntValue("ScrollRadius");
    mScrollCenterOffsetX = config.getIntValue("ScrollCenterOffsetX");
    mScrollCenterOffsetY = config.getIntValue("ScrollCenterOffsetY");

    config.addListener("ScrollLaziness", this);
    config.addListener("ScrollRadius", this);

    mPopupMenu = new PopupMenu;
    mBeingPopup = new BeingPopup;

    setFocusable(true);
}

Viewport::~Viewport()
{
    delete mPopupMenu;
    delete mBeingPopup;
}

void Viewport::setMap(Map *map)
{
    if (mMap && map)
    {
        map->setDebugFlags(mMap->getDebugFlags());
    }
    mMap = map;
}

extern MiniStatusWindow *miniStatusWindow;

void Viewport::draw(gcn::Graphics *gcnGraphics)
{
    static int lastTick = tick_time;

    if (!mMap || !player_node)
    {
        gcnGraphics->setColor(gcn::Color(64, 64, 64));
        gcnGraphics->fillRectangle(
                gcn::Rectangle(0, 0, getWidth(), getHeight()));
        return;
    }

    Graphics *graphics = static_cast<Graphics*>(gcnGraphics);

    // Avoid freaking out when tick_time overflows
    if (tick_time < lastTick)
    {
        lastTick = tick_time;
    }

    // Calculate viewpoint
    int midTileX = (graphics->getWidth() + mScrollCenterOffsetX) / 2;
    int midTileY = (graphics->getHeight() + mScrollCenterOffsetX) / 2;

    const Vector &playerPos = player_node->getPosition();
    const int player_x = (int) playerPos.x - midTileX;
    const int player_y = (int) playerPos.y - midTileY;

    if (mScrollLaziness < 1)
        mScrollLaziness = 1; // Avoids division by zero

    // Apply lazy scrolling
    while (lastTick < tick_time)
    {
        if (player_x > mPixelViewX + mScrollRadius)
        {
            mPixelViewX += (player_x - mPixelViewX - mScrollRadius) /
                            mScrollLaziness;
        }
        if (player_x < mPixelViewX - mScrollRadius)
        {
            mPixelViewX += (player_x - mPixelViewX + mScrollRadius) /
                            mScrollLaziness;
        }
        if (player_y > mPixelViewY + mScrollRadius)
        {
            mPixelViewY += (player_y - mPixelViewY - mScrollRadius) /
                            mScrollLaziness;
        }
        if (player_y < mPixelViewY - mScrollRadius)
        {
            mPixelViewY += (player_y - mPixelViewY + mScrollRadius) /
                            mScrollLaziness;
        }
        lastTick++;
    }

    // Auto center when player is off screen
    if (        player_x - mPixelViewX > graphics->getWidth() / 2
            ||  mPixelViewX - player_x > graphics->getWidth() / 2
            ||  mPixelViewY - player_y > graphics->getHeight() / 2
            ||  player_y - mPixelViewY > graphics->getHeight() / 2
        )
    {
        mPixelViewX = player_x;
        mPixelViewY = player_y;
    };

    // Don't move camera so that the end of the map is on screen
    const int mapWidthPixels = mMap->getWidth() * mMap->getTileWidth();
    const int mapHeightPixels = mMap->getHeight() * mMap->getTileHeight();
    const int viewXmax = mapWidthPixels - graphics->getWidth();
    const int viewYmax = mapHeightPixels - graphics->getHeight();
    if (mPixelViewX < 0)
        mPixelViewX = 0;
    if (mPixelViewY < 0)
        mPixelViewY = 0;
    if (mPixelViewX > viewXmax)
        mPixelViewX = viewXmax;
    if (mPixelViewY > viewYmax)
        mPixelViewY = viewYmax;

    // Center camera on map if the map is smaller than the screen
    if (mapWidthPixels < graphics->getWidth())
        mPixelViewX = (mapWidthPixels - graphics->getWidth()) / 2;
    if (mapHeightPixels < graphics->getHeight())
        mPixelViewY = (mapHeightPixels - graphics->getHeight()) / 2;

    // Draw black background if map is smaller than the screen
    if (        mapWidthPixels < graphics->getWidth()
            ||  mapHeightPixels < graphics->getHeight())
    {
        gcnGraphics->setColor(gcn::Color(0, 0, 0));
        gcnGraphics->fillRectangle(
                gcn::Rectangle(0, 0, getWidth(), getHeight()));

    }

    // Draw tiles and sprites
    if (mMap)
    {
        mMap->draw(graphics, (int) mPixelViewX, (int) mPixelViewY);

        if (mShowDebugPath)
        {
            mMap->drawCollision(graphics,
                                (int) mPixelViewX,
                                (int) mPixelViewY,
                                mShowDebugPath);
            if (mShowDebugPath == Map::MAP_DEBUG)
                _drawDebugPath(graphics);
        }
    }

    if (player_node->getCheckNameSetting())
    {
        player_node->setCheckNameSetting(false);
        player_node->setName(player_node->getName());
    }

    // Draw text
    if (textManager)
    {
        textManager->draw(graphics, (int) mPixelViewX, (int) mPixelViewY);
    }

    // Draw player names, speech, and emotion sprite as needed
    const ActorSprites &actors = actorSpriteManager->getAll();
    for (ActorSpritesConstIterator it = actors.begin(), it_end = actors.end();
         it != it_end; it++)
    {
        if ((*it)->getType() == ActorSprite::FLOOR_ITEM)
            continue;

        Being *b = static_cast<Being*>(*it);
        b->drawSpeech((int) mPixelViewX, (int) mPixelViewY);
        b->drawEmotion(graphics, (int) mPixelViewX, (int) mPixelViewY);
    }

    if (miniStatusWindow)
        miniStatusWindow->drawIcons(graphics);

    // Draw contained widgets
    WindowContainer::draw(gcnGraphics);
}

void Viewport::logic()
{
    WindowContainer::logic();

    // Make the player follow the mouse position
    // if the mouse is dragged elsewhere than in a window.
    _followMouse();
}

void Viewport::_followMouse()
{
    Uint8 button = SDL_GetMouseState(&mMouseX, &mMouseY);
    // If the left button is dragged
    if (mPlayerFollowMouse && button & SDL_BUTTON(1))
    {
        // We create a mouse event and send it to mouseDragged.
        Uint8 *keys = SDL_GetKeyState(NULL);
        gcn::MouseEvent mouseEvent(NULL,
                      (keys[SDLK_LSHIFT] || keys[SDLK_RSHIFT]),
                      false,
                      false,
                      false,
                      gcn::MouseEvent::DRAGGED,
                      gcn::MouseEvent::LEFT,
                      mMouseX,
                      mMouseY,
                      0);

        mouseDragged(mouseEvent);
    }
}

void Viewport::_drawDebugPath(Graphics *graphics)
{
    // Get the current mouse position
    SDL_GetMouseState(&mMouseX, &mMouseY);

    Path debugPath;

    if (Net::getNetworkType() == ServerInfo::TMWATHENA)
    {
        const int mouseTileX = (mMouseX + (int) mPixelViewX) / 32;
        const int mouseTileY = (mMouseY + (int) mPixelViewY) / 32;
        const Vector &playerPos = player_node->getPosition();

        debugPath = mMap->findPath(
            (int) (playerPos.x - 16) / 32,
            (int) (playerPos.y - 32) / 32,
            mouseTileX, mouseTileY, 0xFF);

        _drawPath(graphics, debugPath);
    }
    else if (Net::getNetworkType() == ServerInfo::MANASERV)
    {
        const Vector &playerPos = player_node->getPosition();
        const int playerRadius = player_node->getCollisionRadius();
        // Draw player collision rectangle
        graphics->setColor(gcn::Color(128, 128, 0, 120));
        graphics->fillRectangle(
            gcn::Rectangle((int) playerPos.x - (int) mPixelViewX - playerRadius,
                           (int) playerPos.y - (int) mPixelViewY - playerRadius,
                                 playerRadius * 2, playerRadius * 2));

        debugPath = mMap->findPixelPath(
            (int) playerPos.x,
            (int) playerPos.y,
            mMouseX + (int) mPixelViewX,
            mMouseY + (int) mPixelViewY,
            playerRadius, 0xFF);

        // We draw the path proposed by mouse
        _drawPath(graphics, debugPath, gcn::Color(128, 0, 128));

        // But also the one currently walked on.
        _drawPath(graphics, player_node->getPath(), gcn::Color(0, 0, 255));
    }
}

void Viewport::_drawPath(Graphics *graphics, const Path &path,
                         gcn::Color color)
{
    graphics->setColor(color);

    if (Net::getNetworkType() == ServerInfo::TMWATHENA)
    {
        for (Path::const_iterator i = path.begin(); i != path.end(); ++i)
        {
            int squareX = i->x * 32 - (int) mPixelViewX + 12;
            int squareY = i->y * 32 - (int) mPixelViewY + 12;

            graphics->fillRectangle(gcn::Rectangle(squareX, squareY, 8, 8));
            graphics->drawText(
                    toString(mMap->getMetaTile(i->x, i->y)->Gcost),
                    squareX + 4, squareY + 12, gcn::Graphics::CENTER);
        }
    }
    else if (Net::getNetworkType() == ServerInfo::MANASERV)
    {
        for (Path::const_iterator i = path.begin(); i != path.end(); ++i)
        {
            int squareX = i->x - (int) mPixelViewX;
            int squareY = i->y - (int) mPixelViewY;

            graphics->fillRectangle(gcn::Rectangle(squareX - 4, squareY - 4,
                                                   8, 8));
            graphics->drawText(
                    toString(mMap->getMetaTile(i->x / 32, i->y / 32)->Gcost),
                    squareX + 4, squareY + 12, gcn::Graphics::CENTER);
        }

    }
}

void Viewport::mousePressed(gcn::MouseEvent &event)
{
    if (event.getSource() != this)
        return;

    // Check if we are alive and kickin'
    if (!mMap || !player_node || !player_node->isAlive())
        return;

    // Check if we are busy
    if (PlayerInfo::isTalking())
        return;

    mPlayerFollowMouse = false;

    const int pixelX = event.getX() + (int) mPixelViewX;
    const int pixelY = event.getY() + (int) mPixelViewY;

    // Right click might open a popup
    if (event.getButton() == gcn::MouseEvent::RIGHT)
    {
        if (mHoverBeing && mHoverBeing != player_node)
        {
            mPopupMenu->showPopup(event.getX(), event.getY(), mHoverBeing);
            return;
        }
        else if (mHoverItem)
        {
            mPopupMenu->showPopup(event.getX(), event.getY(), mHoverItem);
            return;
        }
    }

    // If a popup is active, just remove it
    if (mPopupMenu->isVisible())
    {
        mPopupMenu->setVisible(false);
        return;
    }

    // Left click can cause different actions
    if (event.getButton() == gcn::MouseEvent::LEFT)
    {
        // Interact with some being
        if (mHoverBeing)
        {
            if (mHoverBeing->canTalk())
                mHoverBeing->talkTo();
            else
            {
                // Ignore it if its dead
                if (mHoverBeing->isAlive())
                {
                    if (player_node->withinAttackRange(mHoverBeing) ||
                        keyboard.isKeyActive(keyboard.KEY_ATTACK))
                        player_node->attack(mHoverBeing,
                            !keyboard.isKeyActive(keyboard.KEY_TARGET));
                    else
                        player_node->setGotoTarget(mHoverBeing);
                }
             }
        // Picks up a item if we clicked on one
        }
        else if (mHoverItem)
        {
            player_node->pickUp(mHoverItem);
        }
        else if (player_node->getCurrentAction() == Being::SIT)
        {
            return;
        }
        // Just walk around
        else
        {
            player_node->stopAttack();
            player_node->cancelFollow();
            mPlayerFollowMouse = true;

            // Make the player go to the mouse position
            _followMouse();
        }
    }
    else if (event.getButton() == gcn::MouseEvent::MIDDLE)
    {
        // Find the being nearest to the clicked position
        Being *target = actorSpriteManager->findNearestLivingBeing(
                pixelX, pixelY, 20, ActorSprite::MONSTER);

        if (target)
             player_node->setTarget(target);
    }
}

void Viewport::mouseDragged(gcn::MouseEvent &event)
{
    if (!mMap || !player_node)
        return;

    if (mPlayerFollowMouse && !event.isShiftPressed())
    {
        if (Net::getNetworkType() == ServerInfo::MANASERV)
        {
          if (get_elapsed_time(mLocalWalkTime) >= walkingMouseDelay)
          {
              mLocalWalkTime = tick_time;
              player_node->setDestination(event.getX() + (int) mPixelViewX,
                                          event.getY() + (int) mPixelViewY);
              player_node->pathSetByMouse();
          }
        }
        else
        {
          if (mLocalWalkTime != player_node->getActionTime())
          {
              mLocalWalkTime = player_node->getActionTime();
              int destX = (event.getX() + mPixelViewX + 16) /
                          mMap->getTileWidth();
              int destY = (event.getY() + mPixelViewY + 16) /
                          mMap->getTileHeight();
              player_node->setDestination(destX, destY);
          }
        }
    }
}

void Viewport::mouseReleased(gcn::MouseEvent &event)
{
    mPlayerFollowMouse = false;

    // Only useful for eAthena but doesn't hurt under ManaServ
    mLocalWalkTime = -1;
}

void Viewport::showPopup(Window *parent, int x, int y, Item *item,
                         bool isInventory)
{
    mPopupMenu->showPopup(parent, x, y, item, isInventory);
}

void Viewport::closePopupMenu()
{
    mPopupMenu->handleLink("cancel");
}

void Viewport::optionChanged(const std::string &name)
{
    mScrollLaziness = config.getIntValue("ScrollLaziness");
    mScrollRadius = config.getIntValue("ScrollRadius");
}

void Viewport::mouseMoved(gcn::MouseEvent &event)
{
    // Check if we are on the map
    if (!mMap || !player_node)
        return;

    const int x = (event.getX() + (int) mPixelViewX);
    const int y = (event.getY() + (int) mPixelViewY);

    if (mHoverBeing)
        mHoverBeing->removeActorSpriteListener(this);
    mHoverBeing = actorSpriteManager->findBeingByPixel(x, y);
    mBeingPopup->show(getMouseX(), getMouseY(), mHoverBeing);

    if (mHoverItem)
        mHoverItem->removeActorSpriteListener(this);
    mHoverItem = actorSpriteManager->findItem(x / mMap->getTileWidth(),
                                              y / mMap->getTileHeight());

    if (mHoverBeing)
    {
        mHoverBeing->addActorSpriteListener(this);
        switch (mHoverBeing->getType())
        {
            // NPCs
            case ActorSprite::NPC:
                gui->setCursorType(Gui::CURSOR_TALK);
                break;

            // Monsters
            case ActorSprite::MONSTER:
                gui->setCursorType(Gui::CURSOR_FIGHT);
                break;
            default:
                gui->setCursorType(Gui::CURSOR_POINTER);
                break;
         }
    // Item mouseover
    }
    else if (mHoverItem)
    {
        mHoverItem->addActorSpriteListener(this);
        gui->setCursorType(Gui::CURSOR_PICKUP);
    }
    else
    {
        gui->setCursorType(Gui::CURSOR_POINTER);
    }
}

void Viewport::toggleDebugPath()
{
    mShowDebugPath++;
    if (mShowDebugPath > Map::MAP_SPECIAL3)
        mShowDebugPath = Map::MAP_NORMAL;
    if (mMap)
    {
        mMap->setDebugFlags(mShowDebugPath);
    }
}

void Viewport::hideBeingPopup()
{
    mBeingPopup->setVisible(false);
}

void Viewport::actorSpriteDestroyed(const ActorSprite &actorSprite)
{
    if (&actorSprite == mHoverBeing)
        mHoverBeing = 0;

    if (&actorSprite == mHoverItem)
        mHoverItem = 0;
}
