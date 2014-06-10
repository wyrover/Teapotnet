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

#ifndef TPN_CACHE_H
#define TPN_CACHE_H

#include "tpn/include.h"
#include "tpn/block.h"
#include "tpn/synchronizable.h"

namespace tpn
{

class Cache : public Synchronizable
{
public:
	Cache(void);
	~Cache(void);
	
	void prefetch(const BinaryString &target);
	void sync(const BinaryString &target, const String &filename);
	
	void push(const BinaryString &target, ByteArray &input);	// Core pushes new combinations here
	bool pull(const BinaryString &target, ByteArray &output);	// Core pull new combinations from here
	
	void registerBlock(Block *block);
	void unregisterBlock(Block *block);
	
	// TODO: loading blocks from cache
	
private:
	Block *getBlock(const BinaryString &target);
	
	String mDirectory;
	
	Map<BinaryString, Set<Blocks*> > mBlocks;	// Registered blocks
	Map<BinaryString, Blocks*> mTempBlocks;
	
};

}

#endif