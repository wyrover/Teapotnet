/*************************************************************************
 *   Copyright (C) 2011-2017 by Paul-Louis Ageneau                       *
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

#ifndef TPN_MAIL_H
#define TPN_MAIL_H

#include "tpn/include.hpp"

#include "pla/serializable.hpp"
#include "pla/binarystring.hpp"
#include "pla/array.hpp"
#include "pla/array.hpp"
#include "pla/time.hpp"
#include "pla/crypto.hpp"

namespace tpn
{

class Mail : public Serializable
{
public:
	Mail(const String &content = "");
	virtual ~Mail(void);

	bool empty(void) const;
	const String &content(void) const;
	String author(void) const;
	Identifier identifier(void) const;
	Time time(void) const;
	BinaryString parent(void) const;
	BinaryString digest(void) const;

	void setContent(const String &content);
	void setAuthor(const String &author);
	void setParent(const BinaryString &parent);
	void addAttachment(const BinaryString &attachment);

	void sign(const Identifier &identifier, const Rsa::PrivateKey &privKey);
	bool check(const Rsa::PublicKey &pubKey) const;
	bool isSigned(void) const;

	// Serializable
	virtual void serialize(Serializer &s) const;
	virtual bool deserialize(Serializer &s);
	virtual bool isInlineSerializable(void) const;

private:
	BinaryString computeDigest(void) const;

	Time mTime;
	String mContent;
	String mAuthor;
	Identifier mIdentifier;
	Array<BinaryString> mAttachments;
	BinaryString mParent;
	mutable BinaryString mSignature;
	mutable BinaryString mDigest;
};

bool operator <  (const Mail &m1, const Mail &m2);
bool operator >  (const Mail &m1, const Mail &m2);
bool operator == (const Mail &m1, const Mail &m2);
bool operator != (const Mail &m1, const Mail &m2);

}

#endif
