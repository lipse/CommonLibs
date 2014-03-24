/*
* Copyright 2009, 2010 Free Software Foundation, Inc.
* Copyright 2010 Kestrel Signal Processing, Inc.
* Copyright 2011, 2012, 2014 Range Networks, Inc.
*
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <string.h>
#include <cstdio>
#include <fstream>
#include <string>
#include <stdarg.h>

#include "Configuration.h"
#include "Logger.h"
#include "Threads.h"	// pat added


using namespace std;

// Reference to a global config table, used all over the system.
extern ConfigurationTable gConfig;


/**@ The global alarms table. */
//@{
Mutex           alarmsLock;
list<string>    alarmsList;
void            addAlarm(const string&);
//@}



// (pat) If Log messages are printed before the classes in this module are inited
// (which happens when static classes have constructors that do work)
// the OpenBTS just crashes.
// Prevent that by setting sLoggerInited to true when this module is inited.
static bool sLoggerInited = 0;
static struct CheckLoggerInitStatus {
	CheckLoggerInitStatus() { sLoggerInited = 1; }
} sCheckloggerInitStatus;



/** Names of the logging levels. */
const char *levelNames[] = {
	"EMERG", "ALERT", "CRIT", "ERR", "WARNING", "NOTICE", "INFO", "DEBUG"
};
int numLevels = 8;
bool gLogToConsole = 0;
FILE *gLogToFile = NULL;
Mutex gLogToLock;
LogGroup gLogGroup;


int levelStringToInt(const string& name)
{
	// Reverse search, since the numerically larger levels are more common.
	for (int i=numLevels-1; i>=0; i--) {
		if (name == levelNames[i]) return i;
	}

	// Common substitutions.
	if (name=="INFORMATION") return 6;
	if (name=="WARN") return 4;
	if (name=="ERROR") return 3;
	if (name=="CRITICAL") return 2;
	if (name=="EMERGENCY") return 0;

	// Unknown level.
	return -1;
}

/** Given a string, return the corresponding level name. */
int lookupLevel(const string& key)
{
	string val = gConfig.getStr(key);
	int level = levelStringToInt(val);

	if (level == -1) {
		string defaultLevel = gConfig.mSchema["Log.Level"].getDefaultValue();
		level = levelStringToInt(defaultLevel);
		_LOG(CRIT) << "undefined logging level (" << key << " = \"" << val << "\") defaulting to \"" << defaultLevel << ".\" Valid levels are: EMERG, ALERT, CRIT, ERR, WARNING, NOTICE, INFO or DEBUG";
		gConfig.set(key, defaultLevel);
	}

	return level;
}


int getLoggingLevel(const char* filename)
{
	// Default level?
	if (!filename) return lookupLevel("Log.Level");

	// This can afford to be inefficient since it is not called that often.
	const string keyName = string("Log.Level.") + string(filename);
	if (gConfig.defines(keyName)) return lookupLevel(keyName);
	return lookupLevel("Log.Level");
}

//bool gCheckGroupLogLevel(const char *groupname, int loglevel)
//{
//	// Gag me
//	string keyName = string("Log.Group.") + groupname;
//	return gConfig.defines(keyName) ? (lookupLevel(gConfig.getStr(keyName)) >= loglevel) : false;
//}



int gGetLoggingLevel(const char* filename)
{
	// This is called a lot and needs to be efficient.

	static Mutex sLogCacheLock;
	static map<uint64_t,int>  sLogCache;
	static unsigned sCacheCount;
	static const unsigned sCacheRefreshCount = 1000;

	if (filename==NULL) return gGetLoggingLevel("");

	HashString hs(filename);
	uint64_t key = hs.hash();

	sLogCacheLock.lock();
	// Time for a cache flush?
	if (sCacheCount>sCacheRefreshCount) {
		sLogCache.clear();
		sCacheCount=0;
	}
	// Is it cached already?
	map<uint64_t,int>::const_iterator where = sLogCache.find(key);
	sCacheCount++;
	if (where!=sLogCache.end()) {
		int retVal = where->second;
		sLogCacheLock.unlock();
		return retVal;
	}
	// Look it up in the config table and cache it.
	// FIXME: Figure out why unlock and lock below fix the config table deadlock.
	// (pat) Probably because getLoggingLevel may call LOG recursively via lookupLevel().
	sLogCacheLock.unlock();
	int level = getLoggingLevel(filename);
	sLogCacheLock.lock();
	sLogCache.insert(pair<uint64_t,int>(key,level));
	sLogCacheLock.unlock();
	return level;
}





// copies the alarm list and returns it. list supposed to be small.
list<string> gGetLoggerAlarms()
{
    alarmsLock.lock();
    list<string> ret;
    // excuse the "complexity", but to use std::copy with a list you need
    // an insert_iterator - copy technically overwrites, doesn't insert.
    insert_iterator< list<string> > ii(ret, ret.begin());
    copy(alarmsList.begin(), alarmsList.end(), ii);
    alarmsLock.unlock();
    return ret;
}

/** Add an alarm to the alarm list. */
void addAlarm(const string& s)
{
    alarmsLock.lock();
    alarmsList.push_back(s);
	unsigned maxAlarms = gConfig.getNum("Log.Alarms.Max");
    while (alarmsList.size() > maxAlarms) alarmsList.pop_front();
    alarmsLock.unlock();
}


Log::~Log()
{
	if (mDummyInit) return;
	// Anything at or above LOG_CRIT is an "alarm".
	// Save alarms in the local list and echo them to stderr.
	if (mPriority <= LOG_CRIT) {
		if (sLoggerInited) addAlarm(mStream.str().c_str());
		cerr << mStream.str() << endl;
	}
	// Current logging level was already checked by the macro.
	// So just log.
	syslog(mPriority, "%s", mStream.str().c_str());
	// pat added for easy debugging.
	if (gLogToConsole||gLogToFile) {
		int mlen = mStream.str().size();
		int neednl = (mlen==0 || mStream.str()[mlen-1] != '\n');
		gLogToLock.lock();
		if (gLogToConsole) {
			// The COUT() macro prevents messages from stomping each other but adds uninteresting thread numbers,
			// so just use std::cout.
			std::cout << mStream.str();
			if (neednl) std::cout<<"\n";
		}
		if (gLogToFile) {
			fputs(mStream.str().c_str(),gLogToFile);
			if (neednl) {fputc('\n',gLogToFile);}
			fflush(gLogToFile);
		}
		gLogToLock.unlock();
	}
}


// (pat) This is the log initialization function.
// It is invoked by this line in OpenBTS.cpp, and similar lines in other programs like the TransceiverRAD1:
// 		Log dummy("openbts",gConfig.getStr("Log.Level").c_str(),LOG_LOCAL7);
// The LOCAL7 corresponds to the "local7" line in the file /etc/rsyslog.d/OpenBTS.log.
Log::Log(const char* name, const char* level, int facility)
{
	// (pat) This 'constructor' has nothing to do with the regular use of the Log class, so we have
	// to set this special flag to prevent the destructor from generating a syslog message.
	// This is really goofy, but there is a reason - this is the way whoever wrote this got the Logger initialized early during
	// static class initialization since OpenBTS has so many static classes whose constructors do work (a really bad idea)
	// and may generate log messages.
	mDummyInit = true;
	gLogInit(name, level, facility);
}


ostringstream& Log::get()
{
	assert(mPriority<numLevels);
	mStream << levelNames[mPriority] <<  ' ';
	return mStream;
}



// Allow applications to also pass in a filename.  Filename should come from the database
void gLogInitWithFile(const char* name, const char* level, int facility, char * LogFilePath)
{
	// Set the level if one has been specified.
	if (level) {
		gConfig.set("Log.Level",level);
	}

	if (gLogToFile==0 && LogFilePath != 0 && *LogFilePath != 0 && strlen(LogFilePath) > 0) {
		gLogToFile = fopen(LogFilePath,"w"); // New log file each time we start.
		if (gLogToFile) {
			time_t now;
			time(&now);
			fprintf(gLogToFile,"Starting at %s",ctime(&now));
			fflush(gLogToFile);
			std::cout << name <<" logging to file: " << LogFilePath << "\n";
		}
	}

	// Open the log connection.
	openlog(name,0,facility);

	// We cant call this from the Mutex itself because the Logger uses Mutex.
	gMutexLogLevel = gGetLoggingLevel("Mutex.cpp");
}



void gLogInit(const char* name, const char* level, int facility)
{
	// Set the level if one has been specified.
	if (level) {
		gConfig.set("Log.Level",level);
	}

	// Pat added, tired of the syslog facility.
	// Both the transceiver and OpenBTS use this same facility, but only OpenBTS/OpenNodeB may use this log file:
	string str = gConfig.getStr("Log.File");
	if (gLogToFile==0 && str.length() && 0==strncmp(gCmdName,"Open",4)) {
		const char *fn = str.c_str();
		if (fn && *fn && strlen(fn)>3) {	// strlen because a garbage char is getting in sometimes.
			gLogToFile = fopen(fn,"w"); // New log file each time we start.
			if (gLogToFile) {
				time_t now;
				time(&now);
				fprintf(gLogToFile,"Starting at %s",ctime(&now));
				fflush(gLogToFile);
				std::cout << name <<" logging to file: " << fn << "\n";
			}
		}
	}

	// Open the log connection.
	openlog(name,0,facility);

	// We cant call this from the Mutex itself because the Logger uses Mutex.
	gMutexLogLevel = gGetLoggingLevel("Mutex.cpp");
}


void gLogEarly(int level, const char *fmt, ...)
{
	va_list args;
 
	va_start(args, fmt);
	vsyslog(level | LOG_USER, fmt, args);
	va_end(args);
}

// Return _NumberOfLogGroups if invalid.
LogGroup::Group LogGroup::groupNameToIndex(const char *groupName) const
{
	for (unsigned g = (Group)0; g < _NumberOfLogGroups; g++) {
		if (0 == strcasecmp(mGroupNames[g],groupName)) { return (Group) g; }	// happiness
	}
	return _NumberOfLogGroups;	// failed
}

LogGroup::LogGroup() { LogGroupInit(); }

// These must match LogGroup::Group.
const char *LogGroup::mGroupNames[] = { "Control", "SIP", "GSM", "GPRS", "Layer2", NULL };

void LogGroup::LogGroupInit()
{
	// Error check some more.
	assert(0==strcmp(mGroupNames[Control],"Control"));
	assert(0==strcmp(mGroupNames[SIP],"SIP"));
	assert(0==strcmp(mGroupNames[GSM],"GSM"));
	assert(0==strcmp(mGroupNames[GPRS],"GPRS"));
	assert(0==strcmp(mGroupNames[Layer2],"Layer2"));

	// Error check mGroupNames is the correct length;
	unsigned g;
	for (g = 0; mGroupNames[g]; g++) { continue; }
	assert(g == _NumberOfLogGroups);	// If you get this, go fix mGroupNames to match enum LogGroup::Group.

	for (unsigned g = 0; g < _NumberOfLogGroups; g++) {
		mDebugLevel[g] = 0;
	}

#if 0
	if (mGroupNameToIndex.size()) { return; }	// inited previously.
	mGroupNameToIndex[string("Control")] = Control;
	mGroupNameToIndex[string("SIP")] = SIP;
	mGroupNameToIndex[string("GSM")] = GSM;
	mGroupNameToIndex[string("GPRS")] = GPRS;
	mGroupNameToIndex[string("Layer2")] = Layer2;
#endif
}

static const char *LogGroupPrefix = "Log.Group.";


#if UNUSED
// Return true if this was a LogGroup config parameter.
// These dont have to be fast.
bool LogGroup::setGroup(const string groupName, const string levelName)
{
	const int len = strlen(LogGroupPrefix);
	if (0 != strncasecmp(groupName.c_str(),LogGroupPrefix,len)) { return false; }
	Group g = groupNameToIndex(groupName.c_str() + len);
	if (g >= _NumberOfLogGroups) {
		LOG(ALERT) << "Unrecognized Log.Group config parameter:"<<groupName;
	} else {
		mDebugLevel[g] = lookupLevel(levelName);
	}

	//GroupMapType::iterator it = mGroupNameToIndex.find(groupName);
	//if (it != map::end) {
	//	mDebugLevel[it->second] = lookupLevel(levelName);
	//}
	return true;
}

bool LogGroup::unsetGroup(const string groupName)
{
	const int len = strlen(LogGroupPrefix);
	if (0 != strncasecmp(groupName.c_str(),LogGroupPrefix,len)) { return false; }
	Group g = groupNameToIndex(groupName.c_str() + len);
	if (g >= _NumberOfLogGroups) {
		LOG(ALERT) << "Unrecognized Log.Group config parameter:"<<groupName;
	} else {
		mDebugLevel[g] = 0;	// Turn off debugging.
	}

	//GroupMapType::iterator it = mGroupNameToIndex.find(groupName);
	//if (it != map::end) {
	//	mDebugLevel[it->second] = lookupLevel(levelName);
	//}
	return true;
}
#endif

void LogGroup::setAll()
{
	LOG(DEBUG);
	string prefix = string(LogGroupPrefix);
	for (unsigned g = 0; g < _NumberOfLogGroups; g++) {
		string param = prefix + mGroupNames[g];
		if (gConfig.defines(param)) {
			string levelName = gConfig.getStr(param);
			LOG(DEBUG) << "Setting "<<LOGVAR(param)<<LOGVAR(levelName);
			//mDebugLevel[g] = lookupLevel(levelName);
			mDebugLevel[g] = levelStringToInt(levelName);
		}
	}
}

// vim: ts=4 sw=4
