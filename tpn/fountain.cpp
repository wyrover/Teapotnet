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

#include "tpn/fountain.h"

namespace tpn
{

Fountain::Fountain(void) :
	mNextSeen(0),
	mNextDecoded(0)
{

}

Fountain::~Fountain(void)
{
	
}

uint64_t Fountain::generate(uint64_t first, int64_t last, Combination &c)
{
	c.clear();
	
	uint64_t rank = 0;
	
	Random rnd;
	for(uint64_t i=first; i<=last; ++i)
	{
		// TODO: not optimal, useless copy
		char buffer[BlockSize];
		size_t size = readBlock(i, buffer, BlockSize);
		
		if(size)
		{
			uint8_t coeff;
			rnd.read(coeff);
			c+= Combination(i, buffer, size)*coeff;
			
			++rank;
		}
	}
	
	if(rank != last-first+1)
	{
		for(List<Combination>::iterator it = mCombinations.begin();
			it != mCombinations.end();
			++it)
		{
			if(it->firstComponent() >= first && it->lastComponent() <= last)
			{
				uint8_t coeff;
				rnd.read(coeff);
				c+= (*it)*coeff;
				
				++ rank;
			}
		}
	}
	
	return rank;
}

uint64_t Fountain::generate(uint64_t offset, Combination &c)
{
	// TODO: not optimal, useless copy
	char buffer[BlockSize];
	size_t size = readBlock(i, buffer, BlockSize);
	if(!size) return 0;
	
	c = Combination(offset, buffer, size);
	return 1;
}

void Fountain::solve(const Combination &c)
{
	if(mNextSeen == 0 && mCombinations.empty()) 
		init();
	
	List<Combination>::iterator it;	// current equation

	uint64_t first = 0;
	uint64_t last = 0;
	if(!mCombinations.empty())
	{
		it = mCombinations.begin();
		first = it->firstComponent();
		last  = it->lastComponent();
		++it;
		
		while(it != mCombinations.end())
		{
			first = std::min(first, it->firstComponent());
			last  = std::min(last, it->lastComponent());	
			++it;
		}
	}

	mCombinations.push_back(c);

	// Suppress known components
	for(uint64_t i=first; i<=last; ++i)
	{
		Combination u;
		if(generate(i, u))
		{
			it = mCombinations.begin();
			while(it != mCombinations.end())
			{
				uint8_t c = it->coeff(i);
				if(c)	// if term not supressed
				{
					(*it)+= u*c;
					Assert(it->coeff(i) == 0);
				}
			}
			
			if(i == first) ++first;	// first non-null component in system
		}
	}
	
	// Gauss-Jordan elimination
	it = mCombinations.begin();	// pivot equation
	uint64_t i = first;		// pivot equation index
	while(it != mCombinations.end())
	{
		List<Combination>::iterator jt = it;
		while(jt != mCombinations.end() && jt->coeff(i) == 0) ++jt;
		if(jt == mCombinations.end()) break;
		if(jt != it) std::iter_swap(jt, it);
		
		// Normalize pivot
		uint8_t c = it->coeff(i);
		if(c != 1) (*it)/= c;
		Assert(it->coeff(i) == 1);
		
		// Suppress coordinate i in each equation
		uint64_t j = 0;			// secondary equation index
		jt = mCombinations.begin();	// secondary equation
		while(jt != mCombinations.end())
		{
			if(it == jt)
			{
				++jt; ++j;
				continue;
			}
			
			uint8_t c = jt->coeff(i);
			if(c)	// if term not supressed
			{
				// it->coeff(i) == 1 here
				(*jt)+= (*it)*c;
				Assert(jt->coeff(i) == 0);
			}
			
			++jt; ++j;
		}
		
		++it; ++i;
	}
	
	// Remove null vectors
	it = mCombinations.begin();
	while(it != mCombinations.end())
	{
		if(it->componentsCount() == 0)	// Null vector, useless equation
		{
			mCombinations.erase(it++);
		}
		else ++it;
	}
	
	it = mCombinations.begin();	// current equation
	while(it != mCombinations.end())
	{
		first = it->firstComponent();
		
		// Seen packets are not reported if decoding buffer is full
		if(first >= m_nextSeen)
		{
			mNextSeen = first + 1;
		}
		
		if(first == m_nextDecoded && it->componentsCount() == 1)
		{
			uint8_t c = it->coeff(first);
			if(c != 1) (*it)/= c;

			writeBlock(first, it->decodedData(), it->decodedSize());
			mNextDecoded = first + 1;
			mCombinations.erase(it++);
			continue;
		}		

		++it;
	}
}

size_t Fountain::hashBlock(uint64_t offset, BinaryString &digest)
{
	// TODO
	return 0;
}

void Fountain::init(void)
{
	while(checkBlock(mNextDecoded))
		++mNextDecoded;
	
	mNextSeen = std::max(mNextSeen, mNextDecoded);
}

Fountain::Combination::Combination(void)
{
	
}

Fountain::Combination::Combination(uint64_t i, const char *data, size_t size)
{
	mData.assign(data, size);
	addComponent(i, 1);
}

Fountain::Combination::~Combination(void)
{
	
}

void Fountain::Combination::addComponent(uint64_t i, uint8_t coeff)
{
	if(i == 0) return;
	
	Map<uint64_t, uint8_t>::iterator it = mComponents.find(i);
	if(it != mComponents.end())
	{
		it->second = gAdd(it->second, coefficient);
		if(it->second == 0) mComponents.erase(it);
	}
	else {
		mComponents[i] = coeff;
	}
}

uint64_t Fountain::Combination::firstComponent(void)
{
	if(!mComponents.empty()) return mComponents.begin()->first;
	else return 0;
}

uint64_t Fountain::Combination::lastComponent(void)
{
	if(!mComponents.empty()) return (--mComponents.end())->first;
	else return 0;
}

uint64_t Fountain::Combination::componentsCount(void) const
{
	if(!mComponents.empty()) return (lastComponent() - firstComponent()) + 1;
	else return 0;
}

uint8_t Fountain::Combination::coeff(uint64_t i)
{
	Map<uint64_t, uint8_t>::const_iterator it = mComponents.find(i);
	if(it == mComponents.end()) return 0; 
	
	Assert(it->second != 0);
	return it->second;
}

bool Fountain::Combination::isCoded(void) const
{
	return (mComponents.size() != 1 || mComponents.begin()->second != 1);
}

const char *Fountain::Combination::data(void) const
{
	return mData.data();
}

size_t Fountain::Combination::size(void) const
{
	return mData.size();
}

const char *Fountain::Combination::decodedData(void) const
{
	if(isCoded() || mData.size() < 2) return NULL;
	
	return mData.data() + 2;
}

size_t Fountain::Combination::decodedSize(void) const
{
	if(isCoded() || mData.size() < 2) return 0;
	
	uint16_t size = 0;
	BinaryString tmp(mData, 0, 2);
	tmp.readBinary(size);
	
	// TODO: warning if size too big
	return std::min(size_t(mData.size()-2), size_t(size));
}

void Fountain::Combination::clear(void)
{
	mCombinations.clear();
	mData.clear();
}

Fountain::Combination Fountain::Combination::operator+(const Combination &combination) const
{
	Fountain::Combination result(*this);
	result+= combination;
	return result;
}

Fountain::Combination Fountain::Combination::operator*(uint8_t coeff) const
{
	Fountain::Combination result(*this);
	result*= coeff;
	return result;
}

Fountain::Combination Fountain::Combination::operator/(uint8_t coeff) const
{
	Fountain::Combination result(*this);
	result/= coeff;	
	return result;
}
	
Fountain::Combination &Fountain::Combination::operator+=(const Combination &combination)
{
	BinaryString other(combination.mData);

	// Assure mData is the longest vector
	if(mData.size() < other.size())
		mData.swap(other);
	
	// Add values from the other (smallest) vector
	for(unsigned i = 0; i < other.size(); ++i)
		mData[i] = gAdd(mData[i], other[i]);

	// Add components
	for(	Map<uint64_t, uint8_t>::const_iterator jt = combination.mComponents.begin();
		jt != combination.mComponents.end();
		++jt)
	{
		AddComponent(jt->first, jt->second);
	}

	return *this;
}

Fountain::Combination &Fountain::Combination::operator*=(uint8_t coeff)
{
	// TODO: coeff == 0
	Assert(coeff != 0);

	// Multiply vector
	for(unsigned i = 0; i < mData.size(); ++i)
		mData[i] = gMul(mData[i], coeff);

	for(	Map<uint64_t, uint8_t>::iterator it = mComponents.begin();
		it != mComponents.end();
		++it)
	{
		it->second = gMul(it->second, coeff);
	}

	return *this;
}

Fountain::Combination &Fountain::Combination::operator/=(uint8_t coeff)
{
	NS_ASSERT(coeff != 0);

	(*this)*= gInv(coeff);
	return *this;
}

uint8_t Fountain::Combination::gAdd(uint8_t a, uint8_t b)
{
	return a ^ b;
}

uint8_t Fountain::Combination::gMul(uint8_t a, uint8_t b) 
{
	uint8_t p = 0;
	uint8_t i;
	uint8_t carry;
	for(i = 0; i < 8; ++i) 
	{
		if (b & 1) p ^= a;
		carry = (a & 0x80);
		a <<= 1;
		if (carry) a ^= 0x1b; // 0x1b is x^8 modulo x^8 + x^4 + x^3 + x + 1
		b >>= 1;
	}
	
	return p;
}

uint8_t Fountain::Combination::gInv(uint8_t a) 
{
	NS_ASSERT(a != 0);
	
	uint8_t b = 1;
	while(b)
	{
		if(gMul(a,b) == 1) return b;
		++b;
	}
	
	throw Exception("Combination::gInv failed for input " + String::number(unsigned(a)));
}

}
