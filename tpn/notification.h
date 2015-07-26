/*************************************************************************
 *   Copyright (C) 2011-2014 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Teapotnet.                                     *
 *                                                                       *
 *   Teapotnet is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Teapotnet is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Teapotnet.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#ifndef TPN_NOTIFICATION_H
#define TPN_NOTIFICATION_H

#include "tpn/include.h"

#include "pla/string.h"
#include "pla/map.h"
#include "pla/time.h"

namespace tpn
{

class Notification : public StringMap
{
public:
	Notification(void);
	Notification(const String &content);
	~Notification(void);
	
	Time time(void) const;
	String content(void) const;
	
	bool send(const Identifier &destination) const;
	
private:
	Time mTime;
};

}

#endif

