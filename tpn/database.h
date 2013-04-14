/*************************************************************************
 *   Copyright (C) 2011-2012 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of TeapotNet.                                     *
 *                                                                       *
 *   TeapotNet is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   TeapotNet is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with TeapotNet.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#ifndef TPN_DATABASE_H
#define TPN_DATABASE_H

#include "tpn/include.h"
#include "tpn/string.h"
#include "tpn/bytestring.h"
#include "tpn/exception.h"

#ifdef USE_SYSTEM_SQLITE3
#include <sqlite3.h>
#else
#include "include/sqlite3.h"
#endif

namespace tpn
{

class Database
{
public:
	Database(const String &filename);
	~Database(void);

	class Statement
	{
	public:
		Statement(void);
		Statement(sqlite3 *db, sqlite3_stmt *stmt);
		~Statement(void);
		
		bool step(void);
		void reset(void);
		void finalize(void);
		
		void execute(void);	// step + finalize
		
		enum type_t { Null, Integer, Float, Text, Blob };
		
		int parametersCount(void) const;
		void bind(int parameter, int value);
		void bind(int parameter, int64_t value);
		void bind(int parameter, unsigned value);
		void bind(int parameter, uint64_t value);
		void bind(int parameter, float value);
		void bind(int parameter, double value);
		void bind(int parameter, const String &value);
		void bind(int parameter, const ByteString &value);
		void bindNull(int parameter);
		
		int columnsCount(void) const;
		type_t type(int column) const;
		String name(int column) const;
		String value(int column) const;
		void value(int column, int &v) const;
		void value(int column, int64_t &v) const;
		void value(int column, unsigned &v) const;
		void value(int column, uint64_t &v) const;
		void value(int column, float &v) const;
		void value(int column, double &v) const;
		void value(int column, String &v) const;
		void value(int column, ByteString &v) const;
		
	private:
		sqlite3 *mDb;
		sqlite3_stmt *mStmt;
	};
	
	Statement prepare(const String &request);
	void execute(const String &request);
	int64_t insertId(void) const;
	
private:
	sqlite3 *mDb;
};

class DatabaseException : public Exception
{
public:
	DatabaseException(sqlite3 *db, const String &message);
};

}

#endif