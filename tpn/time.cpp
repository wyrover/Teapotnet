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

#include "tpn/time.h"
#include "tpn/exception.h"
#include "tpn/string.h"

namespace tpn
{

Mutex Time::TimeMutex;
  
Time Time::Now(void)
{
	return Time(); 
}

uint64_t Time::Milliseconds(void)
{
	timeval tv;
	gettimeofday(&tv, NULL);
	return uint64_t(tv.tv_sec)*1000 + uint64_t(tv.tv_usec)/1000;
}

Time::Time(void) :
	mTime(0)
{
	time(&mTime);
}

Time::Time(time_t time = 0) :
	mTime(time)
{
  
}

// Sun, 06 Nov 1994 08:49:37 GMT  ; RFC 822, updated by RFC 1123
// Sunday, 06-Nov-94 08:49:37 GMT ; RFC 850, obsoleted by RFC 1036
// Sun Nov  6 08:49:37 1994       ; ANSI C's asctime() format
// 
Time::Time(const String &str)
{
	const String months[] = {"jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec"};

	if(str.trimmed().empty())
	{
		mTime = time_t(0);
		return;
	}
	
	List<String> list;
	str.trimmed().explode(list, ' ');

	struct tm tms;
	std::memset(&tms, 0, sizeof(tms));
	tms.tm_isdst = -1;

	switch(list.size()) 
	{
		case 1:	// Unix timestamp as integer
			str.extract(mTime);
			return;

		case 4:	// RFC 850
		{		
			list.pop_front();       // we don't care about day of week

			String tmp;
			tmp = list.front(); list.pop_front();
                        List<String> dateParts;
                        tmp.explode(dateParts, '-');
                        Assert(dateParts.size() == 3);
			tms.tm_mday = dateParts.front().toInt(); dateParts.pop_front();
                        tmp  = dateParts.front().toLower(); dateParts.pop_front();
                        int m = 0; while(m < 12 && months[m] != tmp) ++m;
                        Assert(m < 12);
                        tms.tm_mon = m;
			tms.tm_year = 1900 + dateParts.front().toInt(); dateParts.pop_front();

			tmp = list.front(); list.pop_front();
                        List<String> hourParts;
                        tmp.explode(hourParts, '-');
                        Assert(hourParts.size() == 3);
			tms.tm_hour = hourParts.front().toInt(); hourParts.pop_front();
                        tms.tm_min  = hourParts.front().toInt(); hourParts.pop_front();
                        tms.tm_sec  = hourParts.front().toInt(); hourParts.pop_front();

			String utc = list.front().toLower(); list.pop_front();
			tmp = utc.cut('+');
			Assert(utc == "UTC" || utc == "GMT");
			if(!tmp.empty()) tms.tm_hour = tms.tm_hour - tmp.toInt();		
			break;
		}

		case 5:	// asctime() format
		{
			list.pop_front();	// we don't care about day of week

			String tmp;
			tmp = list.front().toLower(); list.pop_front();
			int m = 0; while(m < 12 && months[m] != tmp) ++m;
			Assert(m < 12);
			tms.tm_mon = m;

			tms.tm_mday = list.front().toInt(); list.pop_front();

			tmp = list.front(); list.pop_front();
			List<String> hourParts;
			tmp.explode(hourParts, ':');
			Assert(hourParts.size() == 3);
			tms.tm_hour = hourParts.front().toInt(); hourParts.pop_front();
			tms.tm_min  = hourParts.front().toInt(); hourParts.pop_front();
			tms.tm_sec  = hourParts.front().toInt(); hourParts.pop_front();

			tms.tm_year = list.front().toInt(); list.pop_front();
			break;
		}

		case 6: // RFC 1123
                {
                        list.pop_front();       // we don't care about day of week

			tms.tm_mday = list.front().toInt(); list.pop_front();

                        String tmp;
                        tmp = list.front().toLower(); list.pop_front();
                        int m = 0; while(m < 12 && months[m] != tmp) ++m;
                        Assert(m < 12);
                        tms.tm_mon = m;

			tms.tm_year = list.front().toInt(); list.pop_front();

                        tmp = list.front(); list.pop_front();
                        List<String> hourParts;
                        tmp.explode(hourParts, ':');
                        Assert(hourParts.size() == 3);
                        tms.tm_hour = hourParts.front().toInt(); hourParts.pop_front();
                        tms.tm_min  = hourParts.front().toInt(); hourParts.pop_front();
                        tms.tm_sec  = hourParts.front().toInt(); hourParts.pop_front();

                        String utc = list.front().toUpper(); list.pop_front();
                        tmp = utc.cut('+');
                        Assert(utc == "UTC" || utc == "GMT");
                        if(!tmp.empty()) tms.tm_hour = tms.tm_hour - tmp.toInt();	
                        break;
                }

		default:
			throw Exception(String("Unknown date format: ") + str);
	}
	
	TimeMutex.lock();
	
	char *tz = getenv("TZ");
	putenv(const_cast<char*>("TZ=UTC"));
	tzset();
	
	mTime = std::mktime(&tms);
	
	if(tz)
	{
		char *buf = reinterpret_cast<char*>(std::malloc(3 + strlen(tz) + 1));
		if(buf)
		{
			std::strcpy(buf,"TZ=");
			std::strcat(buf, tz);
			putenv(buf);	
		}
	} 
	else {
		putenv(const_cast<char*>("TZ="));
	}
	tzset();
	
	TimeMutex.unlock();
	
	if(mTime == time_t(-1)) 
		throw Exception(String("Invalid date: ") + str);
}

Time::~Time(void)
{
  
}

int Time::hour(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_hour;	// not thread safe
	TimeMutex.unlock();
	return result;
}

int Time::minute(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_min;		// not thread safe
	TimeMutex.unlock();
	return result;
}

int Time::second(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_sec;		// not thread safe
	TimeMutex.unlock();
	return result;
}

int Time::day(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_mday;	// not thread safe
	TimeMutex.unlock();
	return result;
}

int Time::month(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_mon;		// not thread safe
	TimeMutex.unlock();
	return result;
}

int Time::year(void) const
{
	TimeMutex.lock();
	int result = localtime(&mTime)->tm_year;	// not thread safe
	TimeMutex.unlock();
	return result;
}

String Time::toDisplayDate(void) const
{
	TimeMutex.lock();
	char buffer[256];
	strftime(buffer, 256, "%x %X", localtime(&mTime));
	TimeMutex.unlock();
	return String(buffer);
}

String Time::toHttpDate(void) const
{
	TimeMutex.lock();
	char buffer[256];
	strftime(buffer, 256, "%a, %d %b %Y %H:%M:%S", gmtime(&mTime));
	TimeMutex.unlock();
	return String(buffer) + " GMT";
}

String Time::toIsoDate(void) const
{
	TimeMutex.lock();
	char buffer[256];
	strftime(buffer, 256, "%Y-%m-%d", localtime(&mTime));
	TimeMutex.unlock();
	return String(buffer);
}

String Time::toIsoTime(void) const
{
	TimeMutex.lock();
	char buffer[256];
	strftime(buffer, 256, "%H:%M:%S", localtime(&mTime));
	TimeMutex.unlock();
	return String(buffer);
}

time_t Time::toUnixTime(void) const
{
	return mTime; 
}

double Time::toSeconds(void) const
{
	return (*this - Time(0)); 
}

int Time::toHours(void) const
{
	return int(toSeconds()/3600); 
}

int Time::toDays(void) const
{
	return int(toSeconds()/86400); 
}

void Time::addSeconds(double seconds)
{
	(*this)+= seconds;
}

void Time::addHours(int hours)
{
	addSeconds(double(hours)*3600);
}

void Time::addDays(int days)
{
	addSeconds(double(days)*86400);
}

Time &Time::operator += (double seconds)
{
	TimeMutex.lock();
	struct tm tms = *localtime(&mTime);	// not thread safe
	tms.tm_sec += int(seconds);
	mTime = std::mktime(&tms);
	TimeMutex.unlock();
	return *this;
}

Time Time::operator + (double seconds) const
{
	Time result(*this);
	result+= seconds;
	return result;
}

double Time::operator - (const Time &t) const
{
	return difftime(mTime, t.mTime);
}

Time::operator time_t(void) const
{
	return toUnixTime();  
}

void Time::serialize(Serializer &s) const
{
	s.output(int64_t(mTime));
}

bool Time::deserialize(Serializer &s)
{
	int64_t tmp = 0;
	s.input(tmp);
	mTime = time_t(tmp);
	return true;
}

bool Time::isNativeSerializable(void) const
{
	return true;
}

bool operator < (const Time &t1, const Time &t2)
{
	return (t1-t2 < 0.);
}

bool operator > (const Time &t1, const Time &t2)
{
	return (t1-t2 > 0.);	
}

bool operator == (const Time &t1, const Time &t2)
{
	return t1.toUnixTime() == t2.toUnixTime();	
}

bool operator != (const Time &t1, const Time &t2)
{
	return !(t1 == t2);
}

}
