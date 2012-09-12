/*************************************************************************
 *   Copyright (C) 2011-2012 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Arcanet.                                       *
 *                                                                       *
 *   Arcanet is free software: you can redistribute it and/or modify     *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Arcanet is distributed in the hope that it will be useful, but      *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Arcanet.                                         *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "message.h"
#include "core.h"

namespace arc
{

Message::Message(const String &content) :
	mContent(content)
{
  
}

Message::~Message(void)
{
  
}

const Identifier &Message::receiver(void) const
{
	return mReceiver;
}

const String &Message::content(void) const
{
	return mContent;
}

const StringMap &Message::parameters(void) const
{
	return mParameters;
}

bool Message::parameter(const String &name, String &value) const
{
	return mParameters.get(name, value);
}

void Message::setContent(const String &content)
{
	mContent = content;
}

void Message::setParameters(StringMap &params)
{
	mParameters = params;
}

void Message::setParameter(const String &name, const String &value)
{
	mParameters[name] = value; 
}

void Message::send(void)
{
	Core::Instance->sendMessage(*this);
}

void Message::send(const Identifier &receiver)
{
	mReceiver = receiver;
	Core::Instance->sendMessage(*this);
}
  
}
