//
//  SockLib.cpp
//
//  Created by liangX<liangx.cn@qq.com> on 15/3/25.
//  Copyright (c) 2015. All rights reserved.
//

#include "SockLib.h"

#include <assert.h>
#include <thread>

#ifdef _WIN32
	#include <time.h>
	#define StrCmpI					::_stricmp
	#define StrCmpNI				::_strnicmp
	#pragma comment(lib, "ws2_32.lib")
#else // !_WIN32
	#include <sys/time.h>
	#define StrCmpI					::strcasecmp
	#define StrCmpNI				::strncasecmp
#endif // _WIN32

#if SOCKLIB_NOCASE
	#define NameStrCmp				StrCmpI
#else
	#define NameStrCmp				strcmp
#endif // SOCKLIB_NOCASE

#if SOCKLIB_DEBUG
	#define DBGLOG(fmt, ...)		fprintf(stdout, fmt, ##__VA_ARGS__)
	#define DBGLOG_IF(b, fmt, ...)	if (b) fprintf(stdout, fmt, ##__VA_ARGS__)
#else
	#define DBGLOG(fmt, ...)
	#define DBGLOG_IF(b, fmt, ...)
#endif // SOCKLIB_DEBUG

SOCKLIB_NAMESPACE_BEGIN

std::string SockLib::_libName = SOCKLIB_NAME;

SockLib::SockMap	SockLib::_news;
SockLib::SockMap	SockLib::_refs;
SockLib::SockMap	SockLib::_clos;
SockLib::SockMap	SockLib::_dies;

fd_set	SockLib::_fdr;
fd_set	SockLib::_fdw;
fd_set	SockLib::_fde;
timeval SockLib::_tv;

static std::string SOCKLIB_TCP 		= SOCKLIB_NAME ".tcp";
static std::string SOCKLIB_UDP 		= SOCKLIB_NAME ".udp";
static std::string SOCKLIB_BUF 		= SOCKLIB_NAME ".buf";
static std::string SOCKLIB_UTIL 	= SOCKLIB_NAME ".util";
static std::string SOCKLIB_NCAS 	= SOCKLIB_NAME ".nocase";

#if SOCKLIB_TO_LUA
LuaHelper::Objects	LuaHelper::_objs;
lua_State* 			SockLib::_luaState = 0;

#if SOCKLIB_ALG
static std::string SOCKLIB_RC4 	= SOCKLIB_NAME ".rc4";
static std::string SOCKLIB_MD5 	= SOCKLIB_NAME ".md5";
static std::string SOCKLIB_SHA1 = SOCKLIB_NAME ".sha1";
#endif // SOCKLIB_ALG

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// SockLib
//

//----------------------------------------------------------------------------
//
bool SockLib::init()
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 0), &wsa) != 0) {
		return false;
	}
#endif // _WIN32
	return true;
}

//----------------------------------------------------------------------------
//
void SockLib::cleanup()
{
#ifdef _WIN32
	WSACleanup();
#endif // _WIN32
}

//----------------------------------------------------------------------------
//
void SockLib::destroy(SockPtr ref)
{
#if SOCKLIB_TO_LUA
	// if lua manager it, don't destroy
	if (!LuaHelper::found(ref)) {
		ref->close();
		_dies[ref] = 1;
		_news.erase(ref);
		_clos.erase(ref);
	} else {
		remove(ref);
	}
#else
	remove(ref);
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockLib::add(SockPtr ref, int event)
{
	ref->_careEvent = event;
	_news[ref] = event;
	_clos.erase(ref);
	_dies.erase(ref);
}

//----------------------------------------------------------------------------
//
void SockLib::modify(SockPtr ref, int event)
{
	ref->_careEvent = event;
	_news[ref] = event;
	_clos.erase(ref);
	_dies.erase(ref);
}

//----------------------------------------------------------------------------
//
void SockLib::remove(const SockPtr ref)
{
	_clos[ref] = 1;
	_news.erase(ref);
	_dies.erase(ref);
}

//----------------------------------------------------------------------------
//
bool SockLib::found(const SockPtr ref)
{
	return _refs.find(ref) != _refs.end();
}

//----------------------------------------------------------------------------
//
void SockLib::beforePoll()
{
#ifdef SOCKLIB_DEBUG
	bool dirty = !_news.empty() || !_clos.empty() || !_dies.empty();
	if (dirty)
		DBGLOG("SockLib::beforePoll() { _refs=%u, _news=%u, _clos=%u, _dies=%u\n",
			(u32_t)_refs.size(), (u32_t)_news.size(), (u32_t)_clos.size(), (u32_t)_dies.size());
#endif // SOCKLIB_DEBUG
	
	for (auto& ref : _news) {
		_refs[ref.first] = ref.second;
	}
	_news.clear();

	for (auto& ref : _dies) {
		_refs.erase(ref.first);
		DBGLOG("SockLib::beforePoll() delete a ref %p\n", ref.first);
		delete ref.first;
	}
	_dies.clear();
	
	for (auto& ref : _clos) {
		_refs.erase(ref.first);
	}
	_clos.clear();
	
#ifdef SOCKLIB_DEBUG
	if (dirty)
		DBGLOG("SockLib::beforePoll() } _refs=%u, _news=%u, _clos=%u, _dies=%u\n",
			(u32_t)_refs.size(), (u32_t)_news.size(), (u32_t)_clos.size(), (u32_t)_dies.size());
#endif // SOCKLIB_DEBUG
}

//----------------------------------------------------------------------------
//
void SockLib::poll(u32_t usec)
{
	Util::poll();
	
	beforePoll();
	
	if (_refs.empty() && usec > 0) {
#ifdef _WIN32
		Sleep(1);
#else
		usleep(usec ? usec*100 : 1000);
#endif
		return;
	}
	
	int evtcnt = 0;
	for (SockMap::iterator it = _refs.begin(); it != _refs.end(); ) {
		evtcnt += _poll_per_FD_SETSIZE(it, evtcnt ? 0 : usec);
	}
	
	afterPoll();
}

int SockLib::_poll_per_FD_SETSIZE(SockMap::iterator& it_begin, u32_t usec)
{
    int maxfd = 0, evtcnt = 0;
    
    _tv.tv_sec  = 0;
    _tv.tv_usec = usec;
    
    FD_ZERO(&_fdr);
    FD_ZERO(&_fdw);
    FD_ZERO(&_fde);

	int it_count = 0;

	SockMap::iterator it;
    for (it = it_begin; it_count < FD_SETSIZE && it != _refs.end(); ++it) {
        SockPtr sk = it->first;
		
		if (sk->isClosed()) {
			remove(sk);
			continue;
		}

		++it_count;
//		std::cout << "it_count=" << it_count << std::endl;

        int ev = sk->careEvent();
        
        if ((ev & SockLib::EVT_RECV) && sk->careRecv())
            FD_SET(sk->fd(), &_fdr);
		
        if ((ev & SockLib::EVT_SEND) && sk->careSend())
            FD_SET(sk->fd(), &_fdw);

        FD_SET(sk->fd(), &_fde);
        
        if (maxfd < sk->fd())
            maxfd = sk->fd();
    }
	
    if (::select(maxfd + 1, &_fdr, &_fdw, &_fde, &_tv) <= 0) {
		it_begin = it;
        return 0;
	}
	
	it_count = 0;
    for (it = it_begin; it_count < FD_SETSIZE && it != _refs.end(); ++it) {
        SockPtr sk = it->first;
		
		if (sk->isClosed())
			continue;

		++it_count;

        int ev = 0;
        
        if (FD_ISSET(sk->fd(), &_fde))
            ev |= SockLib::EVT_ERROR;
        if (FD_ISSET(sk->fd(), &_fdw))
            ev |= SockLib::EVT_SEND;
        if (FD_ISSET(sk->fd(), &_fdr))
            ev |= SockLib::EVT_RECV;
        
        sk->_fireEvent = ev;

		if (ev && ev != SockLib::EVT_SEND)
			++evtcnt;
    }
	
	it_begin = it;

	return evtcnt;
}

void SockLib::dispatch()
{
    if (_refs.empty())
        return;

    for (SockMap::iterator it = _refs.begin(); it != _refs.end(); ++it) {
        SockPtr sk = it->first;
		
		sk->onPoll();
		if (sk->isClosed())
			continue;

        int ev = sk->fireEvent();
		
        if (sk->_sockState == SockLib::STA_CONNECTTING) {
            if (ev & SockLib::EVT_ERROR) {
				sk->_sockState = SockLib::STA_CONNFAILED;
				sk->onConnect(false);
                sk->close();
				sk->onClose();
            } else if (ev & SockLib::EVT_SEND) {
				int err = 0; socklen_t len = sizeof(err);
				if (sk->getOption(SO_ERROR, (char*)&err, &len) != -1 && err != 0) {
					sk->_sockState = SockLib::STA_CONNFAILED;
					sk->onConnect(false);
					sk->close();
					sk->onClose();
				} else {
					sk->_sockState = SockLib::STA_CONNECTED;
					sk->onConnect(true);
				}
            }

            continue;
        }
        
        if ((ev & SockLib::EVT_ERROR) && !sk->isClosed()) {
            sk->close();
			sk->onClose();
            continue;
        }
		
		if (sk->_sockState == SockLib::STA_LISTENED) {
			if ((ev & SockLib::EVT_RECV) && !sk->isClosed()) {
				sk->onAccept();
			}
            continue;
		}
        
        if ((ev & SockLib::EVT_RECV) && !sk->isClosed()) {
			sk->onRecv();
        }

        if ((ev & SockLib::EVT_SEND) && !sk->isClosed()) {
			sk->onSend();
        }
    }
}

#if SOCKLIB_TO_LUA

static const luaL_Reg SockLib_Reg[] = {
	{ "tcp", 		SockLib::mylua_tcp },
	{ "udp", 		SockLib::mylua_udp },
	{ "buf", 		SockLib::mylua_buf },
	{ "poll", 		SockLib::mylua_poll },
	{ NULL, 		NULL }
};

static const luaL_Reg SockTcp_Reg[] = {
	{ "connect", 	SockTcp::mylua_connect },
	{ "listen", 	SockTcp::mylua_listen },
	{ "accept", 	SockTcp::mylua_accept },
	{ "send", 		SockTcp::mylua_send },
	{ "recv", 		SockTcp::mylua_recv },
//	{ "inbuf", 		SockTcp::mylua_inbuf },		// __index do it
//	{ "outbuf", 	SockTcp::mylua_outbuf },	// __index do it
	{ "close", 		SockTcp::mylua_close },
	{ "isclosed", 	SockTcp::mylua_isclosed },
	{ "onevent", 	SockTcp::mylua_onevent },
	{ "setopt", 	SockTcp::mylua_setopt },
	{ "sockaddr",	SockTcp::mylua_sockaddr },
	{ "peeraddr",	SockTcp::mylua_peeraddr },
//	{ "__eq", 		SockTcp::mylua_eq },
	{ "__index", 	SockTcp::mylua_index },
	{ "__gc", 		SockTcp::mylua_gc },
	{ "__tostring", SockTcp::mylua_tostring },
	{ NULL, 		NULL }
};

static const luaL_Reg SockUdp_Reg[] = {
	{ "bind", 		SockUdp::mylua_bind },
	{ "sendto", 	SockUdp::mylua_sendto },
	{ "recvfrom", 	SockUdp::mylua_recvfrom },
	{ "close", 		SockUdp::mylua_close },
	{ "onevent", 	SockUdp::mylua_onevent },
	{ "setopt", 	SockUdp::mylua_setopt },
	{ "__index", 	LuaHelper::mylua_index },
	{ "__gc", 		SockUdp::mylua_gc },
	{ "__tostring", SockUdp::mylua_tostring },
	{ NULL, 		NULL }
};

static const luaL_Reg SockBuf_Reg[] = {
	{ "reset", 		SockBuf::mylua_reset },
	{ "skip", 		SockBuf::mylua_skip },
	{ "discard", 	SockBuf::mylua_discard },
	
	{ "sub", 		SockBuf::mylua_sub },

	{ "buffer", 	SockBuf::mylua_buffer },	// __index do it
	{ "length", 	SockBuf::mylua_length },	// __index do it

	{ "w", 			SockBuf::mylua_w },
	{ "wl", 		SockBuf::mylua_wl },
	{ "ws", 		SockBuf::mylua_ws },
	{ "wsl", 		SockBuf::mylua_wsl },
	{ "w8", 		SockBuf::mylua_w8 },
	{ "w16", 		SockBuf::mylua_w16 },
	{ "w32", 		SockBuf::mylua_w32 },
	{ "w64", 		SockBuf::mylua_w64 },
	
	{ "r", 			SockBuf::mylua_r },
	{ "rl", 		SockBuf::mylua_rl },
	{ "rs", 		SockBuf::mylua_rs },
	{ "rsl", 		SockBuf::mylua_rsl },
	{ "r8", 		SockBuf::mylua_r8 },
	{ "r16", 		SockBuf::mylua_r16 },
	{ "r32", 		SockBuf::mylua_r32 },
	{ "r64", 		SockBuf::mylua_r64 },
	
	{ "p", 			SockBuf::mylua_p },
	{ "pl", 		SockBuf::mylua_pl },
	{ "ps", 		SockBuf::mylua_ps },
	{ "psl", 		SockBuf::mylua_psl },
	{ "p8", 		SockBuf::mylua_p8 },
	{ "p16", 		SockBuf::mylua_p16 },
	{ "p32", 		SockBuf::mylua_p32 },
	{ "p64", 		SockBuf::mylua_p64 },

	{ "__index", 	SockBuf::mylua_index },
	{ "__gc", 		SockBuf::mylua_gc },
	{ "__tostring", SockBuf::mylua_tostring },
	{ NULL, 		NULL }
};

static const luaL_Reg SockUtil_Reg[] = {
	#if SOCKLIB_ALG
	{ "crc32", 		Util::mylua_crc32 },
	{ "rc4", 		Util::mylua_rc4 },
	{ "md5", 		Util::mylua_md5 },
	{ "sha1", 		Util::mylua_sha1 },
	{ "b64enc", 	Util::mylua_b64enc },
	{ "b64dec", 	Util::mylua_b64dec },
	#endif // SOCKLIB_ALG

	{ "u32op", 			Util::mylua_u32op },
	// for performance
	{ "u32_inv", 		Util::mylua_u32_inv },
	{ "u32_and", 		Util::mylua_u32_and },
	{ "u32_or", 		Util::mylua_u32_or },
	{ "u32_and_inv", 	Util::mylua_u32_and_inv },
	{ "u32_or_inv", 	Util::mylua_u32_or_inv },
	{ "u32_lshift", 	Util::mylua_u32_lshift },
	{ "u32_rshift", 	Util::mylua_u32_rshift },
	
	{ "tick",		Util::mylua_tick },
	{ "urlenc",		Util::mylua_urlenc },
	{ "urldec",		Util::mylua_urldec },
	{ "ips2n",		Util::mylua_ips2n },
	{ "ipn2s",		Util::mylua_ipn2s },
	{ "ipprobe",	Util::mylua_ipprobe },
	{ "settimer", 	Util::mylua_settimer },
	{ "deltimer", 	Util::mylua_deltimer },
	{ "htons",		Util::mylua_htons },
	{ "ntohs",		Util::mylua_ntohs },
	{ "htonl",		Util::mylua_htonl },
	{ "ntohl",		Util::mylua_ntohl },
	{ "htonll",		Util::mylua_htonll },
	{ "ntohll",		Util::mylua_ntohll },

	{ NULL, 		NULL }
};

static const luaL_Reg SockNcas_Reg[] = {
	{ "__index", 	LuaHelper::mylua_index },
	{ NULL, 		NULL }
};

#if SOCKLIB_ALG
static const luaL_Reg AlgRC4_Reg[] = {
	{ "setkey", 	RC4::mylua_setkey },
	{ "process", 	RC4::mylua_process },
	{ "__gc", 		RC4::mylua_gc },
	{ "__tostring", RC4::mylua_tostring },

	#if SOCKLIB_NOCASE
	{ "__index", 	LuaHelper::mylua_index },
	#endif
	{ NULL, 		NULL }
};

static const luaL_Reg AlgMD5_Reg[] = {
	{ "init", 		MD5::mylua_init },
	{ "update", 	MD5::mylua_update },
	{ "final", 		MD5::mylua_final },
	{ "__gc", 		MD5::mylua_gc },
	{ "__tostring", MD5::mylua_tostring },

	#if SOCKLIB_NOCASE
	{ "__index", 	LuaHelper::mylua_index },
	#endif
	{ NULL, 		NULL }
};

static const luaL_Reg AlgSHA1_Reg[] = {
	{ "init", 		SHA1::mylua_init },
	{ "update", 	SHA1::mylua_update },
	{ "final", 		SHA1::mylua_final },
	{ "__gc", 		SHA1::mylua_gc },
	{ "__tostring", SHA1::mylua_tostring },

	#if SOCKLIB_NOCASE
	{ "__index", 	LuaHelper::mylua_index },
	#endif
	{ NULL, 		NULL }
};
#endif // SOCKLIB_ALG

#define SOCKEVT_CONNECT			"CONNECT"
#define SOCKEVT_ACCEPT			"ACCEPT"
#define SOCKEVT_ERROR			"ERROR"
#define SOCKEVT_RECV			"RECV"
#define SOCKEVT_SEND			"SEND"
#define SOCKEVT_CLOSE			"CLOSE"
#define SOCKEVT_POLL			"POLL"

#define SOCKOPT_BLOCKING		"BLOCKING"
#define SOCKOPT_SENDBUFSIZE		"SENDBUFSIZE"
#define SOCKOPT_RECVBUFSIZE		"RECVBUFSIZE"
#define SOCKOPT_SENDTIMEOUT		"SENDTIMEOUT"
#define SOCKOPT_RECVTIMEOUT		"RECVTIMEOUT"
#define SOCKOPT_REUSEADDR		"REUSEADDR"

#define SOCKFMT_STR				"STR"
#define SOCKFMT_HEX				"HEX"
#define SOCKFMT_BIN				"BIN"
#define SOCKFMT_BUF				"BUF"
#define SOCKFMT_B64				"B64"

int LuaHelper::mylua_index_walk(lua_State* L, const char* key)
{
	int idx = lua_gettop(L);
	
//	DBGLOG("typename=%s\n", lua_typename(L, lua_type(L, idx)));

	if (lua_istable(L, idx)) {
		lua_pushnil(L);
		while (lua_next(L, idx)) {
			const char* name = lua_tostring(L, -2);
//			DBGLOG_IF(name, "name=%s\n", name);
			if (name && 0 == NameStrCmp(name, key)) {
//				DBGLOG("found_name=%s\n", name);
				return 1;
			}
			lua_pop(L, 1);
		}

		if (lua_getmetatable(L, idx) && lua_istable(L, -1) && !lua_rawequal(L, -1, idx)) {
//			DBGLOG("walk_metatable\n");
			if (mylua_index_walk(L, key)) {
				return 1;
			}
		}
		lua_pop(L, 1);
	} else if (lua_isuserdata(L, idx)) {
		if (lua_getmetatable(L, idx) && lua_istable(L, -1)) {
			if (mylua_index_walk(L, key))
				return 1;
		}
	}
	
	lua_settop(L, idx - 1);
	
	return 0;
}

int LuaHelper::mylua_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	DBGLOG("LuaHelper::mylua_index(%s)\n", key);
	
	lua_pushvalue(L, 1);
	if (int r = mylua_index_walk(L, key))
		return r;

	lua_pushnil(L);

	return 1;
}

//----------------------------------------------------------------------------
//
static int mylua_return_fmt(lua_State* L, const char* fmt, const void* ptr, u32_t len)
{
	if (fmt && 0 == StrCmpI(fmt, "*" SOCKFMT_BUF)) {
		SockBuf* buf = new SockBuf();
		buf->w(ptr, len);
		return LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, buf);
	} else if (fmt && 0 == StrCmpI(fmt, "*" SOCKFMT_HEX)) {
		char* hex = new char[len * 2 + 2];
		for (u32_t i = 0; i < len; ++i)
			sprintf(hex + 2 * i, "%02X", ((u8_t*)ptr)[i]);
		lua_pushstring(L, hex);
		delete[] hex;
		return 1;
	} else if (fmt && 0 == StrCmpI(fmt, "*" SOCKFMT_BIN)) {
		lua_pushlightuserdata(L, (void*)ptr);
		lua_pushinteger(L, len);
		return 2;
#if SOCKLIB_ALG
	} else if (fmt && 0 == StrCmpI(fmt, "*" SOCKFMT_B64)) {
		std::string b64 = Base64_encode(ptr, len);
		lua_pushstring(L, b64.c_str());
		return 1;
#endif
	} else {
		lua_pushlstring(L, (char*)ptr, len);
		return 1;
	}
}

//----------------------------------------------------------------------------
//
static bool mylua_input_get(lua_State* L, int idx, void*& ptr, u32_t& len)
{
	ptr = 0;
	len = 0;
	
	if (lua_gettop(L) > idx)
		len = (u32_t) luaL_checkinteger(L, idx + 1);

	if (lua_islightuserdata(L, idx)) {
		ptr = lua_touserdata(L, idx);
	} else if (lua_isstring(L, idx) /*&& !lua_isnumber(L, 2)*/) {
		const char* str = luaL_checkstring(L, idx);
		if (!len) len = (u32_t)lua_strlen(L, idx);
		ptr = (void*)str;
	} else if (lua_istable(L, idx)) {
		SockBuf* buf = SockBuf::mylua_this(L, idx);
		if (buf) {
			if (!len || len > buf->len())
				len = buf->len();
			ptr = (void*)buf->pos();
		}
	}
	
	return ptr != 0;
}

//----------------------------------------------------------------------------
//
bool SockLib::luaAddPath(lua_State* L, const std::string& path)
{
	assert(L);
	if (!L) return false;

	lua_getglobal(L, "package");
	lua_getfield(L, -1, "path");
	
	const char* lua_path = lua_tostring(L, -1);
	std::string new_path(lua_path ? lua_path : "");
	new_path += (new_path.empty() ? "" : ";") + path;
	
	lua_pop(L, 1);
	lua_pushstring(L, new_path.c_str());
	lua_setfield(L, -2, "path");
	
	return true;
}

//----------------------------------------------------------------------------
//
int SockLib::luaLoadFile(lua_State* L, const std::string& file, bool protect)
{
	int ret = luaL_loadfile(L, file.c_str());
	if (ret == 0) {
		if (protect) {
			ret = lua_pcall(L, 0, LUA_MULTRET, 0);
		} else {
			lua_call(L, 0, LUA_MULTRET);
		}
	} else {
		DBGLOG("SockLib::luaLoadFile(\"%s, %s\") failed\n", file.c_str(), protect ? "true" : "false");
	}
	return ret;
}

//----------------------------------------------------------------------------
//
int SockLib::luaLoadString(lua_State* L, const std::string& str, bool protect)
{
	int ret = luaL_loadstring(L, str.c_str());
	if (ret == 0) {
		if (protect) {
			ret = lua_pcall(L, 0, LUA_MULTRET, 0);
		} else {
			lua_call(L, 0, LUA_MULTRET);
		}
	} else {
		DBGLOG("SockLib::luaLoadString(\"%s\", %s) failed\n", str.c_str(), protect ? "true" : "false");
	}
	return ret;
}

//----------------------------------------------------------------------------
//
int SockLib::luaRegLib(lua_State* L, const char* libName)
{
	if (!init())
		return 0;
	
	_luaState = L;

	_libName = libName ? libName : SOCKLIB_NAME;
	
	SOCKLIB_TCP = std::string(libName) + ".tcp";
	SOCKLIB_UDP = std::string(libName) + ".udp";
	SOCKLIB_BUF = std::string(libName) + ".buf";
	SOCKLIB_UTIL = std::string(libName) + ".util";
	SOCKLIB_NCAS = std::string(libName) + ".nocase";

	#if SOCKLIB_ALG
	SOCKLIB_RC4 = std::string(libName) + ".rc4";
	SOCKLIB_MD5 = std::string(libName) + ".md5";
	SOCKLIB_SHA1 = std::string(libName) + ".sha1";
	#endif // SOCKLIB_ALG

	// create SockLib
	#if LUA_VERSION_NUM == 501
	luaL_register(L, _libName.c_str(), SockLib_Reg);
	#else
	#error "only tested on lua5.1"
	luaL_newlib(L, SockLib_Reg);
	#endif

	#if SOCKLIB_NOCASE
	LuaHelper::newMetatable(L, SOCKLIB_NCAS, SockNcas_Reg);
	#endif
	LuaHelper::newMetatable(L, SOCKLIB_TCP, SockTcp_Reg);
	LuaHelper::newMetatable(L, SOCKLIB_UDP, SockUdp_Reg);
	LuaHelper::newMetatable(L, SOCKLIB_BUF, SockBuf_Reg);

	#if SOCKLIB_ALG
	LuaHelper::newMetatable(L, SOCKLIB_RC4, AlgRC4_Reg);
	LuaHelper::newMetatable(L, SOCKLIB_MD5, AlgMD5_Reg);
	LuaHelper::newMetatable(L, SOCKLIB_SHA1, AlgSHA1_Reg);
	#endif // SOCKLIB_ALG
	
	lua_getglobal(L, _libName.c_str());
	#if SOCKLIB_NOCASE
	luaL_getmetatable(L, SOCKLIB_NCAS.c_str());
	lua_setmetatable(L, -2);
	#endif
	
	// socklib._VERSION
	lua_pushfstring(L, "%s v%s, 2015/03/28", _libName.c_str(), SOCKLIB_VER);
	lua_setfield(L, -2, "_VERSION");

	// socklib.util
	lua_newtable(L);
	#if SOCKLIB_NOCASE
	luaL_getmetatable(L, SOCKLIB_NCAS.c_str());
	lua_setmetatable(L, -2);
	#endif
	for (const luaL_Reg* p = SockUtil_Reg; p->name; p++) {
		lua_pushcclosure(L, p->func, 0);
		lua_setfield(L, -2, p->name);
	}
	lua_setfield(L, -2, "util");
	
	// socklib.EVT
	lua_newtable(L);
	#if SOCKLIB_NOCASE
	luaL_getmetatable(L, SOCKLIB_NCAS.c_str());
	lua_setmetatable(L, -2);
	#endif
	const char* evts[] = {
		SOCKEVT_CONNECT,
		SOCKEVT_ACCEPT,
		SOCKEVT_ERROR,
		SOCKEVT_POLL,
		SOCKEVT_RECV,
		SOCKEVT_SEND,
		SOCKEVT_CLOSE
	};
	for (int i = 0; i < sizeof(evts)/sizeof(evts[0]); ++i) {
		lua_pushstring(L, evts[i]);
		lua_setfield(L, -2, evts[i]);
	}
	lua_setfield(L, -2, "EVT");

	// socklib.OPT
	lua_newtable(L);
	#if SOCKLIB_NOCASE
	luaL_getmetatable(L, SOCKLIB_NCAS.c_str());
	lua_setmetatable(L, -2);
	#endif
//	lua_pushcclosure(L, LuaHelper::mylua_index, 0);
//	lua_setfield(L, -2, "__index");
	static const char* opts[] = {
		SOCKOPT_BLOCKING,
		SOCKOPT_SENDBUFSIZE,
		SOCKOPT_RECVBUFSIZE,
		SOCKOPT_SENDTIMEOUT,
		SOCKOPT_RECVTIMEOUT,
		SOCKOPT_REUSEADDR
	};
	for (int i = 0; i < sizeof(opts)/sizeof(opts[0]); ++i) {
		lua_pushstring(L, opts[i]);
		lua_setfield(L, -2, opts[i]);
	}
	lua_setfield(L, -2, "OPT");

	// socklib.FMT
	lua_newtable(L);
	#if SOCKLIB_NOCASE
	luaL_getmetatable(L, SOCKLIB_NCAS.c_str());
	lua_setmetatable(L, -2);
	#endif
	static const char* fmts[] = {
		SOCKFMT_STR,
		SOCKFMT_HEX,
		SOCKFMT_BIN,
		SOCKFMT_BUF,
		SOCKFMT_B64
	};
	for (int i = 0; i < sizeof(fmts)/sizeof(fmts[0]); ++i) {
		lua_pushstring(L, (std::string("*") + fmts[i]).c_str());
		lua_setfield(L, -2, fmts[i]);
	}
	lua_setfield(L, -2, "FMT");

	return 1;
}
	
//----------------------------------------------------------------------------
//
int SockLib::mylua_tcp(lua_State* L)
{
	//return LuaHelper::bind<SockTcp>(L, SOCKLIB_TCP, SockLib::createTcp<>());
	return LuaHelper::create<SockTcp>(L, SOCKLIB_TCP);
}

//----------------------------------------------------------------------------
//
int SockLib::mylua_udp(lua_State* L)
{
//	return LuaHelper::bind<SockUdp>(L, SOCKLIB_UDP, SockLib::createUdp<>());
	return LuaHelper::create<SockUdp>(L, SOCKLIB_UDP);
}

//----------------------------------------------------------------------------
//
int SockLib::mylua_buf(lua_State* L)
{
	return LuaHelper::create<SockBuf>(L, SOCKLIB_BUF);
}

//----------------------------------------------------------------------------
//
int SockLib::mylua_poll(lua_State* L)
{
	if (lua_gettop(L) > 0 && lua_isnumber(L, 1)) {
		poll((u32_t)lua_tointeger(L, 1));
	} else {
		poll(0);
	}
	return 0;
}

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// SockRef
//
void SockRef::close()
{
	if (_fd <= 0)
		return;
	
	SockLib::remove(this);

	DBGLOG("%s.ref{fd=%d}:close()\n", SockLib::libName(), fd());

#ifdef _WIN32
	::closesocket(_fd);
#else
	::close(_fd);
#endif

	_sockState = SockLib::STA_CLOSED;
	
	_fd = -1;
}

int SockRef::setOption(int optname, const char* optval, socklen_t optlen, int level)
{
	return ::setsockopt(fd(), level, optname, optval, optlen);
}

int SockRef::getOption(int optname, char* optval, socklen_t* optlen, int level)
{
	return ::getsockopt(fd(), level, optname, optval, optlen);
}

int SockRef::ioctl(unsigned long cmd, unsigned long* arg)
{
#ifdef _WIN32
	return ::ioctlsocket(fd(), cmd, arg);
#else
	return ::ioctl(fd(), cmd, arg);
#endif
}

int SockRef::setNonBlock(bool b)
{
#if defined(ANDROID) || defined(LINUX)
	int flags = ::fcntl(m_fd, F_GETFL);
	::fcntl(fd(), F_SETFL, flags | O_NONBLOCK);
#else
	unsigned long opt = (b ? 1 : 0);
	return this->ioctl(FIONBIO, &opt);
#endif
}

int SockRef::setReuseAddr(bool b)
{
#ifndef _WIN32
	int opt = (b ? 1 : 0);
	return setOption(SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
	return true;
#endif
}

int SockRef::setBroadcast(bool b)
{
	int opt = (b ? 1 : 0);
	return setOption(SO_BROADCAST, (const char*)&opt, sizeof(opt));
}

int SockRef::setRecvTimeout(int seconds)
{
	struct timeval tv;
	tv.tv_sec	= seconds;
	tv.tv_usec	= 0;
	
	return setOption(SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
}

int SockRef::setSendTimeout(int seconds)
{
	struct timeval tv;
	tv.tv_sec	= seconds;
	tv.tv_usec	= 0;
	
	return setOption(SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}

int SockRef::setRecvBufferSize(int bytes)
{
	int opt = bytes;
	return setOption(SO_RCVBUF, (const char*)&opt, sizeof(opt));
}

int SockRef::setSendBufferSize(int bytes)
{
	int opt = bytes;
	return setOption(SO_SNDBUF, (const char*)&opt, sizeof(opt));
}

int SockRef::getError()
{
#ifdef _WIN32
	return ::WSAGetLastError();
#else
	return errno;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// SockTcp
//

SockTcp::SockTcp()
{
	_recvBuf = new SockBuf();
	_sendBuf = new SockBuf();
}

//----------------------------------------------------------------------------
//
SockTcp::~SockTcp()
{
	delete _recvBuf;
	delete _sendBuf;
}

//----------------------------------------------------------------------------
//
int SockTcp::create()
{
	close();
	_fd = ::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	_sendBuf->reset();
	_recvBuf->reset();
	return _fd;
}

//----------------------------------------------------------------------------
//
void SockTcp::close()
{
	_sendBuf->reset();
	SockRef::close();
}

//----------------------------------------------------------------------------
//
int SockTcp::connect(const std::string& host, u16_t port)
{
	DBGLOG("%s{fd=%d}:connect(%s:%d)\n", SOCKLIB_TCP.c_str(), fd(), host.c_str(), port);

	u32_t ip = Util::ips2n(host);
	
	return connect(ip, port);
}

//----------------------------------------------------------------------------
//
int SockTcp::connect(u32_t ip, u16_t port)
{
	sockaddr_in addr = { 0 };
#ifndef _WIN32
	addr.sin_len		 = sizeof(addr);
#endif
	addr.sin_family      = PF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port        = htons(port);
	
	return connect(&addr);
}

//----------------------------------------------------------------------------
//
int SockTcp::connect(const sockaddr_in* addr)
{
	if (fd() <= 0 && create() <= 0)
		return -1;
	
	_sendBuf->reset();
	_recvBuf->reset();
	
	setNonBlock(true);

	_sockState = SockLib::STA_CONNECTTING;
	
	int r = ::connect(fd(), (struct sockaddr*)addr, sizeof(*addr));
	
	if (SOCKET_ERROR == r) {
		int err = getError();
	#ifdef _WIN32
		if (err != WSAEWOULDBLOCK && err != EALREADY) {
	#else
		if (err != EWOULDBLOCK && err != EINPROGRESS && err != EALREADY) {
	#endif
			_sockState = SockLib::STA_CONNFAILED;
			onConnect(false);
			close();
			onClose();
		} else {
			SockLib::add(this, SockLib::EVT_ALL);
			return r;
		}
	} else if (0 == r) {
		_sockState = SockLib::STA_CONNECTED;
		SockLib::add(this, SockLib::EVT_ALL);
		onConnect(true);
	}

	return r;
}

//----------------------------------------------------------------------------
//
int SockTcp::bind(const std::string& ip, u16_t port)
{
	u32_t ipn = INADDR_ANY;
	if (ip.length() > 0)
		ipn = Util::ips2n(ip);
	
	return bind(ipn, port);
}

//----------------------------------------------------------------------------
//
int SockTcp::bind(u32_t ip, u16_t port)
{
	sockaddr_in addr = { 0 };
#ifndef _WIN32
	addr.sin_len = sizeof(addr);
#endif
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port        = htons(port);
	
	return bind(&addr);
}

//----------------------------------------------------------------------------
//
int SockTcp::bind(const sockaddr_in* addr)
{
	setReuseAddr(true);

	return ::bind(fd(), (struct sockaddr*)addr, sizeof(*addr));
}

//----------------------------------------------------------------------------
//
int SockTcp::listen(int n)
{
	if (fd() <= 0)
		return -1;
	
	DBGLOG("%s{fd=%d}:listen(%d)\n", SOCKLIB_TCP.c_str(), fd(), n);

	int r = ::listen(fd(), n);
	
	if (SOCKET_ERROR != r) {
		_sockState = SockLib::STA_LISTENED;
		SockLib::add(this, SockLib::EVT_RECV);
	}
	
	return r;
}

//----------------------------------------------------------------------------
//
int SockTcp::accept(sockaddr_in* addr)
{
	DBGLOG("%s{fd=%d}:close()\n", SOCKLIB_TCP.c_str(), fd());

#ifndef _WIN32
	addr->sin_len = sizeof(*addr);
#endif
	socklen_t len = sizeof(*addr);
	
	return ::accept(fd(), (struct sockaddr*)addr, &len);
}

void SockTcp::acceptfd(int fd)
{
	_fd = fd;
	_sockState = SockLib::STA_ACCEPTED;
	SockLib::add(this, SockLib::EVT_ALL);
}

//----------------------------------------------------------------------------
//
int SockTcp::send(const void* buf, u32_t len, int flags)
{
	return (int)::send(fd(), (const char*)buf, (size_t)len, flags);
}

//----------------------------------------------------------------------------
//
int SockTcp::recv(void* buf, u32_t len, int flags)
{
	return (int)::recv(fd(), (char*)buf, (size_t)len, flags);
}

//----------------------------------------------------------------------------
//
bool SockTcp::careSend() {
	return _sockState == SockLib::STA_CONNECTTING || _sendBuf->len() > 0;
}

//----------------------------------------------------------------------------
//
int SockTcp::getSockAddr(u32_t* ip, u16_t* port)
{
	sockaddr_in addr;
	int r = getSockAddr(&addr);
	if (r != SOCKET_ERROR) {
		Util::addr2ipn(&addr, ip, port);
	} else {
		if (ip) *ip = 0;
		if (port) *port = 0;
	}
	return r;
}

int SockTcp::getSockAddr(std::string& ip, u16_t* port)
{
	sockaddr_in addr;
	int r = getSockAddr(&addr);
	if (r != SOCKET_ERROR) {
		Util::addr2ips(&addr, ip, port);
	} else {
		ip = "";
		if (port) *port = 0;
	}
	return r;
}

int SockTcp::getSockAddr(sockaddr_in* addr)
{
	socklen_t addrlen = sizeof(sockaddr_in);
	return ::getsockname(fd(), (struct sockaddr*)addr, &addrlen);
}

int SockTcp::getPeerAddr(u32_t* ip, u16_t* port)
{
	sockaddr_in addr;
	int r = getPeerAddr(&addr);
	if (r != SOCKET_ERROR) {
		Util::addr2ipn(&addr, ip, port);
	} else {
		if (ip) *ip = 0;
		if (port) *port = 0;
	}
	return r;
}

int SockTcp::getPeerAddr(std::string& ip, u16_t* port)
{
	sockaddr_in addr;
	int r = getSockAddr(&addr);
	if (r != SOCKET_ERROR) {
		Util::addr2ips(&addr, ip, port);
	} else {
		ip = "";
		if (port) *port = 0;
	}
	return r;
}

int SockTcp::getPeerAddr(sockaddr_in* addr)
{
	socklen_t addrlen = sizeof(sockaddr_in);
	return ::getpeername(fd(), (struct sockaddr*)addr, &addrlen);
}

//----------------------------------------------------------------------------
//
int SockTcp::doSend()
{
	int sentLen = 0;
	while (!isClosed() && _sendBuf->len())
    {
        int len = this->send(_sendBuf->pos(), _sendBuf->len());
        if (len > 0) {
            _sendBuf->discard(len);
			sentLen += len;
        } else if (len < 0) {
            int err = this->getError();
		#ifdef _WIN32
			if (err != WSAEWOULDBLOCK && err != EALREADY) {
		#else
			if (err != EWOULDBLOCK && err != EINPROGRESS && err != EALREADY) {
		#endif
                close();
				onClose();
                return -1;
            }
        } else {
            close();
			onClose();
            return -1;
        }
    }
    
	return sentLen;
}

//----------------------------------------------------------------------------
//
int SockTcp::doRecv()
{
	int  recvLen = 0;
    u8_t buf[1024 * 8];
    
    while (!isClosed()) {
        int len = this->recv(buf, sizeof(buf));
        if (len > 0) {
            recvLen += _recvBuf->write(buf, len);
        } else if (len < 0) {
            int err = getError();
		#ifdef _WIN32
			if (err != WSAEWOULDBLOCK && err != EALREADY) {
		#else
			if (err != EWOULDBLOCK && err != EINPROGRESS && err != EALREADY) {
		#endif
                close();
				onClose();
                return -1;
            }
			break;
        } else {
            close();
			onClose();
            return -1;
        }
    }

	return recvLen;
}

//----------------------------------------------------------------------------
//
void SockTcp::onConnect(bool ok)
{
	DBGLOG("%s{fd=%d}:onConnect(%d)\n", SOCKLIB_TCP.c_str(), fd(), ok);
	
	if (_onConnect)
		_onConnect(this, ok);

#if SOCKLIB_TO_LUA
	if (_mylua_onConnect >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onConnect);
		lua_pushboolean(L, ok);

		int result = lua_pcall(L, 1, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onConnect event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockTcp::onAccept()
{
	DBGLOG("%s{fd=%d}:onAccept()\n", SOCKLIB_TCP.c_str(), fd());
	
	if (_onAccept)
		_onAccept(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onAccept >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onAccept);
		lua_newtable(L);

		int result = lua_pcall(L, 1, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onAccept event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockTcp::onRecv()
{
	int len = doRecv();
	
	if (isClosed() || len <= 0)
		return;
	
	if (_onRecv)
		_onRecv(this);

//	DBGLOG("%s{fd=%d}:onRecv(%d)\n", SOCKLIB_TCP.c_str(), fd(), len);
	
#if SOCKLIB_TO_LUA
	if (_mylua_onRecv >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onRecv);
		lua_pushinteger(L, len);

		int result = lua_pcall(L, 1, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onRecv event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockTcp::onSend()
{
	int len = doSend();
	
	if (isClosed() || len <= 0)
		return;
	
	if (_onSend)
		_onSend(this);

	DBGLOG("%s{fd=%d}:onSend(%d)\n", SOCKLIB_TCP.c_str(), fd(), len);

#if SOCKLIB_TO_LUA
	if (_mylua_onSend >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onSend);
		lua_pushinteger(L, len);

		int result = lua_pcall(L, 1, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onSend event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockTcp::onClose()
{
//	DBGLOG("%s{fd=%d}:onClose()\n", SOCKLIB_TCP.c_str(), fd());
	
	if (_onClose)
		_onClose(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onClose >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onClose);
//		lua_newtable(L);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onClose event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockTcp::onPoll()
{
	if (_onPoll)
		_onPoll(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onPoll >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onPoll);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onPoll event call error: %d", SOCKLIB_TCP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

#if SOCKLIB_TO_LUA

//----------------------------------------------------------------------------
//
SockTcp* SockTcp::mylua_this(lua_State* L, int idx)
{
	return LuaHelper::get<SockTcp>(L, SOCKLIB_TCP, idx);
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_connect(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	if (lua_isnumber(L, 2)) {
		u32_t ip = (u32_t)lua_tonumber(L, 2);
		u16_t port = (u16_t)luaL_checkinteger(L, 3);
		
		int n = _this->connect(ip, port);
		
		lua_pushinteger(L, n);
	} else {
		const char* host = luaL_checkstring(L, 2);
		u16_t port = (u16_t)luaL_checkinteger(L, 3);
		
		int n = _this->connect(host, port);
		
		lua_pushinteger(L, n);
	}

	return 1;
}

//----------------------------------------------------------------------------
// listen(port)
// listen(ip, port, logs)
//
int SockTcp::mylua_listen(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	u16_t port;
	int logs = 5;
	const char* ip = 0;
	
	if (lua_gettop(L) == 2) {
		port = (u16_t)luaL_checkinteger(L, 2);
	} else if (lua_gettop(L) >= 3) {
		ip = luaL_checkstring(L, 2);
		port = (u16_t)luaL_checkinteger(L, 3);
		if (lua_gettop(L) >= 4)
			logs = (int)luaL_checkinteger(L, 4);
	} else {
		luaL_error(L, "%s.listen() bad data", SOCKLIB_TCP.c_str());
		return 1;
	}
	
	int r = _this->create();
	if (SOCKET_ERROR == r) {
		lua_pushvalue(L, 1);
		lua_pushfstring(L, "create socket failed");
		return 2;
	}
	
	if (ip && ip[0]) {
		r = _this->bind(ip, port);
		if (SOCKET_ERROR == r) {
			lua_pushvalue(L, 1);
			lua_pushfstring(L, "bind %s:%d failed", ip, port);
			return 2;
		}
	} else {
		r = _this->bind(port);
		if (SOCKET_ERROR == r) {
			lua_pushvalue(L, 1);
			lua_pushfstring(L, "bind port %d failed", port);
			return 2;
		}
	}

	r = _this->listen(logs);
	
	if (SOCKET_ERROR == r) {
		lua_pushvalue(L, 1);
		lua_pushfstring(L, "listen in port %d failed", port);
		return 2;
	} else {
		lua_pushvalue(L, 1);
		lua_pushnil(L);
		return 2;
	}
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_accept(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	sockaddr_in addr = { 0 };
	
	int fd = _this->accept(&addr);
	
	if (fd >= 0) {
		SockTcp* obj = new SockTcp();
		obj->acceptfd(fd);
		return LuaHelper::bind<SockTcp>(L, SOCKLIB_TCP, obj);
	}
	
	lua_pushnil(L);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_inbuf(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	if (lua_istable(L, 1)) {
//		lua_getfield(L, 1, "inbuf");
//		if (!lua_isnil(L, -1))
//			return 1;
//		lua_pop(L, 1);
		int r = LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, _this->recvBuf());
		if (r) {
			lua_pushvalue(L, -1);
			lua_setfield(L, 1, "inbuf");
		}
		return r;
	}
	
	return LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, _this->recvBuf());
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_outbuf(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	if (lua_istable(L, 1)) {
//		lua_getfield(L, 1, "outbuf");
//		if (!lua_isnil(L, -1))
//			return 1;
//		lua_pop(L, 1);
		int r = LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, _this->sendBuf());
		if (r) {
			lua_pushvalue(L, -1);
			lua_setfield(L, 1, "outbuf");
		}
		return r;
	}
	
	return LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, _this->sendBuf());
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_send(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->sendBuf()->w(ptr, len);
	} else {
		luaL_error(L, "%s:send(<unknown data>)", SOCKLIB_TCP.c_str());
	}
	
	lua_pushvalue(L, 1);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_recv(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	return 0;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_close(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	_this->close();
	return 0;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_isclosed(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	lua_pushboolean(L, _this->isClosed());
	return 1;
}

//----------------------------------------------------------------------------
//
static int SockRef_mylua_setopt(lua_State* L, SockRef* _this)
{
	int r = -1, n;
	const char* key = luaL_checkstring(L, 2);
	
	if (0 == StrCmpI(key, SOCKOPT_BLOCKING)) {
		n = lua_gettop(L) >= 3 ? luaL_optint(L, 3, 1) : 1;
		r = _this->setBroadcast(n != 0);
	} else if (0 == StrCmpI(key, SOCKOPT_REUSEADDR)) {
		n = lua_gettop(L) >= 3 ? luaL_optint(L, 3, 1) : 1;
		r = _this->setReuseAddr(n != 0);
	} else if (lua_gettop(L) >= 3 && lua_isnumber(L, 3)) {
		n = luaL_optint(L, 3, 0);
		
		if (0 == StrCmpI(key, SOCKOPT_SENDBUFSIZE)) {
			r = _this->setSendBufferSize(n);
		} else if (0 == StrCmpI(key, SOCKOPT_RECVBUFSIZE)) {
			r = _this->setRecvBufferSize(n);
		} else if (0 == StrCmpI(key, SOCKOPT_SENDTIMEOUT)) {
			r = _this->setSendTimeout(n);
		} else if (0 == StrCmpI(key, SOCKOPT_RECVTIMEOUT)) {
			r = _this->setRecvTimeout(n);
		} else {
			luaL_error(L, "setopt(BAD_KEY=%s)", key);
			return 1;
		}
	} else {
		luaL_error(L, "setopt(KEY=%s, bad params)", key);
		return 1;
	}
	
	DBGLOG("setopt(%s, %d)\n", key, n);
	
	//lua_pushboolean(L, r != SOCKET_ERROR);
	
	lua_pushvalue(L, 1);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_setopt(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	return SockRef_mylua_setopt(L, _this);
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_peeraddr(lua_State* L)
{
	SockTcp* _this = mylua_this(L);

	std::string ip;
	u16_t port;
	
	_this->getPeerAddr(ip, &port);
	
	lua_pushstring(L, ip.c_str());
	lua_pushnumber(L, port);
	
	return 2;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_sockaddr(lua_State* L)
{
	SockTcp* _this = mylua_this(L);

	std::string ip;
	u16_t port;
	
	_this->getSockAddr(ip, &port);
	
	lua_pushstring(L, ip.c_str());
	lua_pushnumber(L, port);
	
	return 2;
}

//----------------------------------------------------------------------------
//
#define SAFE_LUA_UNREF(n) \
	if (n >= 0) { \
		luaL_unref(L, LUA_REGISTRYINDEX, n); \
		n = -1; \
	}

#define SAFE_LUA_REF(n, v) \
	SAFE_LUA_UNREF(n); n = v;
	
//----------------------------------------------------------------------------
//
int SockTcp::mylua_onevent(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	const char* name = luaL_checkstring(L, 2);
	
	int handler = -1;
	if (lua_gettop(L) >= 3) {
		if (!lua_isfunction(L, -1)) {
			luaL_error(L, "%s:onevent(%s, func) bad param", SOCKLIB_TCP.c_str(), name);
			lua_pushvalue(L, 1);
			return 1;
		}
		handler = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	if (StrCmpI(SOCKEVT_CONNECT, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onConnect, handler);
	} else if (StrCmpI(SOCKEVT_RECV, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onRecv, handler);
	} else if (StrCmpI(SOCKEVT_SEND, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onSend, handler);
	} else if (StrCmpI(SOCKEVT_CLOSE, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onClose, handler);
	} else if (StrCmpI(SOCKEVT_ACCEPT, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onAccept, handler);
	} else if (StrCmpI(SOCKEVT_POLL, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onPoll, handler);
	} else {
		luaL_error(L, "%s:onEevent(%s) not support!", SOCKLIB_TCP.c_str(), name);
	}

	DBGLOG("SockTcp::mylua_onevent(%s)\n", name);
	
	lua_pushvalue(L, 1);

	return 1;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_index(lua_State* L)
{
//	SockTcp* _this = mylua_this(L);
	const char* key = luaL_checkstring(L, 2);

	DBGLOG("SockTcp::mylua_index(%s)\n", key);

	lua_pushvalue(L, 1);
	if (int r = LuaHelper::mylua_index_walk(L, key))
		return r;

	if (0 == NameStrCmp(key, "inbuf")) {
		return mylua_inbuf(L);
	} else if (0 == NameStrCmp(key, "outbuf")) {
		return mylua_outbuf(L);
	}
	
	lua_pushnil(L);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_gc(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	if (LuaHelper::found(_this)) {
		DBGLOG("%s{fd=%d}:mylua_gc() free\n", SOCKLIB_TCP.c_str(), _this->fd());
		_this->close();
		LuaHelper::remove(_this);
		SockLib::destroy(_this);
	} else {
		DBGLOG("%s{fd=%d}:mylua_gc()\n", SOCKLIB_TCP.c_str(), _this->fd());
	}
	
	SAFE_LUA_UNREF(_this->_mylua_onConnect);
	SAFE_LUA_UNREF(_this->_mylua_onRecv);
	SAFE_LUA_UNREF(_this->_mylua_onSend);
	SAFE_LUA_UNREF(_this->_mylua_onAccept);
	SAFE_LUA_UNREF(_this->_mylua_onClose);
	SAFE_LUA_UNREF(_this->_mylua_onPoll);

	return 0;
}

//----------------------------------------------------------------------------
//
int SockTcp::mylua_tostring(lua_State* L)
{
	SockTcp* _this = mylua_this(L);
	
	lua_pushfstring(L, "%s{fd=%d}", SOCKLIB_TCP.c_str(), _this->fd());
	
	return 1;
}

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// SockUdp
//

//----------------------------------------------------------------------------
//
SockUdp::SockUdp()
{
}

//----------------------------------------------------------------------------
//
SockUdp::~SockUdp()
{
}

//----------------------------------------------------------------------------
//
int SockUdp::create()
{
	close();
	_fd = ::socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	return _fd;
}

//----------------------------------------------------------------------------
//
int SockUdp::bind(const std::string& ip, u16_t port)
{
	u32_t ipn = INADDR_ANY;
	if (ip.length() > 0)
		ipn = Util::ips2n(ip);
	
	return bind(ipn, port);
}

//----------------------------------------------------------------------------
//
int SockUdp::bind(u32_t ip, u16_t port)
{
	sockaddr_in addr = { 0 };
#ifndef _WIN32
	addr.sin_len = sizeof(addr);
#endif
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port        = htons(port);
	
	int r = bind(&addr);
	
	if (SOCKET_ERROR != r) {
		_sockState = SockLib::STA_BINDED;
		SockLib::add(this, SockLib::EVT_RECV);
	}
	
	return r;
}

//----------------------------------------------------------------------------
//
int SockUdp::bind(const sockaddr_in* addr)
{
	setReuseAddr(true);

	return ::bind(fd(), (struct sockaddr*)addr, sizeof(*addr));
}

//----------------------------------------------------------------------------
//
int SockUdp::sendto(const std::string& host, u16_t port, const void* data, u32_t len)
{
	DBGLOG("%s{fd=%d}:sendto(%s:%d, %dbytes) free\n", SOCKLIB_UDP.c_str(), fd(), host.c_str(), port, len);
	
	u32_t ip = Util::ips2n(host.c_str());
	
	return sendto(ip, port, data, len);
}

//----------------------------------------------------------------------------
//
int SockUdp::sendto(u32_t ip, u16_t port, const void* data, u32_t len)
{
	sockaddr_in addr = { 0 };
#ifndef _WIN32
	addr.sin_len	= sizeof(addr);
#endif
	addr.sin_family = PF_INET;
	addr.sin_addr.s_addr = ip;
	addr.sin_port        = htons(port);
	
	return sendto(&addr, data, len);
}

//----------------------------------------------------------------------------
//
int SockUdp::sendto(const sockaddr_in* addr, const void* data, u32_t len)
{
	return (int)::sendto(fd(), (char*)data, len, 0, (const struct sockaddr*)addr, sizeof(*addr));
}

//----------------------------------------------------------------------------
//
int SockUdp::recvfrom(void* data, u32_t len, u32_t* ip, u16_t* port)
{
	sockaddr_in addr;
	int r = recvfrom(data, len, &addr);
	if (r > 0) {
		if (ip) *ip	= addr.sin_addr.s_addr;
		if (port) *port	= ntohs(addr.sin_port);
	} else {
		if (ip) *ip = 0;
		if (port) *port = 0;
	}
	return r;
}

int SockUdp::recvfrom(void* data, u32_t len, std::string& ip, u16_t* port)
{
	sockaddr_in addr;
	int r = recvfrom(data, len, &addr);
	if (r > 0) {
		ip = Util::ipn2s(addr.sin_addr.s_addr);
		if (port) *port	= ntohs(addr.sin_port);
	} else {
		ip = "";
		if (port) *port = 0;
	}
	return r;
}

int SockUdp::recvfrom(void* data, u32_t len, sockaddr_in* addr)
{
	socklen_t addrlen = sizeof(sockaddr_in);
	if (addr) {
		return (int)::recvfrom(fd(), (char*)data, len, 0, (struct sockaddr*)addr, &addrlen);
	} else {
		sockaddr_in tmp = { 0 };
		return (int)::recvfrom(fd(), (char*)data, len, 0, (struct sockaddr*)&tmp, &addrlen);
	}
}

//----------------------------------------------------------------------------
//
void SockUdp::onRecv()
{
	DBGLOG("%s{fd=%d}:onRecv()\n", SOCKLIB_UDP.c_str(), fd());
	
	if (_onRecv)
		_onRecv(this);
	
#if SOCKLIB_TO_LUA
	if (_mylua_onRecv >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onRecv);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onRecv event call error: %d", SOCKLIB_UDP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockUdp::onSend()
{
	if (_onSend)
		_onSend(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onSend >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onSend);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onSend event call error: %d", SOCKLIB_UDP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockUdp::onClose()
{
	DBGLOG("%s{fd=%d}:onClose()\n", SOCKLIB_UDP.c_str(), fd());
	
	if (_onClose)
		_onClose(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onClose >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onClose);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onClose event call error: %d", SOCKLIB_UDP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

//----------------------------------------------------------------------------
//
void SockUdp::onPoll()
{
	if (_onPoll)
		_onPoll(this);

#if SOCKLIB_TO_LUA
	if (_mylua_onPoll >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, _mylua_onPoll);

		int result = lua_pcall(L, 0, 0, 0);
		if (0 != result) {
			luaL_error(L, "%s:onPoll event call error: %d", SOCKLIB_UDP.c_str(), result);
		}
	}
#endif // SOCKLIB_TO_LUA
}

#if SOCKLIB_TO_LUA
//----------------------------------------------------------------------------
//
SockUdp* SockUdp::mylua_this(lua_State* L, int idx)
{
	return LuaHelper::get<SockUdp>(L, SOCKLIB_UDP, idx);
}

//----------------------------------------------------------------------------
// bind(port)
// listen(ip, port)
//
int SockUdp::mylua_bind(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	u16_t port;
	const char* ip = 0;
	
	if (lua_gettop(L) == 2) {
		port = (u16_t)luaL_checkinteger(L, 2);
	} else if (lua_gettop(L) >= 3) {
		ip = luaL_checkstring(L, 2);
		port = (u16_t)luaL_checkinteger(L, 3);
	} else {
		luaL_error(L, "%s.bind() bad data", SOCKLIB_UDP.c_str());
		return 1;
	}
	
	int r = _this->create();
	if (SOCKET_ERROR == r) {
		lua_pushvalue(L, 1);
		lua_pushfstring(L, "create socket failed");
		return 2;
	}
	
	if (ip && ip[0]) {
		r = _this->bind(ip, port);
		if (SOCKET_ERROR == r) {
			lua_pushvalue(L, 1);
			lua_pushfstring(L, "bind %s:%d failed", ip, port);
			return 2;
		}
	} else {
		r = _this->bind(port);
		if (SOCKET_ERROR == r) {
			lua_pushvalue(L, 1);
			lua_pushfstring(L, "bind port %d failed", port);
			return 2;
		}
	}

	lua_pushvalue(L, 1);
	lua_pushnil(L);
	return 2;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_sendto(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	const char* host = 0;
	u32_t ip = 0;
	
	if (lua_isnumber(L, 2))
		ip = (u32_t)lua_tonumber(L, 2);
	else
		host = luaL_checkstring(L, 2);
	
	u16_t port = (u16_t)luaL_checkinteger(L, 3);
	
	void* ptr = 0;
	u32_t len = 0;
	
	int n = 0;
	
	if (mylua_input_get(L, 4, ptr, len)) {
		if (ip > 0)
			n = _this->sendto((u32_t)ip, port, ptr, len);
		else if (host && host[0])
			n = _this->sendto(std::string(host), port, ptr, len);
	}
	
	lua_pushinteger(L, n);

	return 1;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_recvfrom(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	std::string ip;
	u16_t port;
	
	char buf[1024 * 8];
	buf[0] = 0;
	
	int len = _this->recvfrom(buf, sizeof(buf), ip, &port);
	if (len > 0)
		buf[len] = 0;
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	int n = mylua_return_fmt(L, fmt, buf, len > 0 ? len : 0);
	
	if (n == 1) {
		lua_pushnumber(L, len);
	}
	lua_pushstring(L, ip.c_str());
	lua_pushnumber(L, port);
	
	return n + 2;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_close(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	_this->close();
	
	return 0;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_onevent(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	const char* name = luaL_checkstring(L, 2);
	
	int handler = -1;
	if (lua_gettop(L) >= 3) {
		if (!lua_isfunction(L, -1)) {
			luaL_error(L, "%s:onevent(%s, func) bad param", SOCKLIB_UDP.c_str(), name);
			lua_pushvalue(L, 1);
			return 1;
		}
		handler = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	if (StrCmpI(SOCKEVT_RECV, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onRecv, handler);
	} else if (StrCmpI(SOCKEVT_SEND, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onSend, handler);
	} else if (StrCmpI(SOCKEVT_CLOSE, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onClose, handler);
	} else if (StrCmpI(SOCKEVT_POLL, name) == 0) {
		SAFE_LUA_REF(_this->_mylua_onPoll, handler);
	} else {
		luaL_error(L, "%s:onevent(%s) not support!", SOCKLIB_UDP.c_str(), name);
	}
	
	lua_pushvalue(L, 1);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_setopt(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	return SockRef_mylua_setopt(L, _this);
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_gc(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	if (LuaHelper::found(_this)) {
		DBGLOG("%s{fd=%d}:mylua_gc() free\n", SOCKLIB_UDP.c_str(), _this->fd());
		_this->close();
		LuaHelper::remove(_this);
		SockLib::destroy(_this);
	} else {
		DBGLOG("%s{fd=%d}:mylua_gc()\n", SOCKLIB_UDP.c_str(), _this->fd());
	}
	
	SAFE_LUA_UNREF(_this->_mylua_onRecv);
	SAFE_LUA_UNREF(_this->_mylua_onSend);
	SAFE_LUA_UNREF(_this->_mylua_onClose);
	SAFE_LUA_UNREF(_this->_mylua_onPoll);

	return 0;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_tostring(lua_State* L)
{
	SockUdp* _this = mylua_this(L);
	
	lua_pushfstring(L, "%s{fd=%d}", SOCKLIB_UDP.c_str(), _this->fd());
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockUdp::mylua_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	DBGLOG("SockUdp:mylua_index(%s)\n", key);

	lua_pushvalue(L, 1);
	if (int r = LuaHelper::mylua_index_walk(L, key))
		return r;
	
	lua_pushnil(L);
	
	return 1;
}

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// SockBuf
//
#define SOCKBUF_BLOCK_SIZE	(1024 * 4)

SockBuf::SockBuf()
	: _ptr(0), _max(0), _pos_r(0), _pos_w(0)
{
}

SockBuf::~SockBuf()
{
	if (_ptr)
		free(_ptr);
}

//----------------------------------------------------------------------------
//
int SockBuf::write(const void* data, u32_t bytes)
{
	if (!data || !bytes)
		return 0;

	// check enough
	if ((_pos_w + bytes) >= _max) {
		if (_pos_r > 0) {
			memmove(_ptr, _ptr + _pos_r, _pos_w - _pos_r);
			_pos_w -= _pos_r;
			_pos_r = 0;
		}
		
		if ((_pos_w + bytes) >= _max) {
			_max = _pos_w + bytes;
			_max = ((_max + SOCKBUF_BLOCK_SIZE) / SOCKBUF_BLOCK_SIZE) * SOCKBUF_BLOCK_SIZE;
			_ptr = (u8_t*)realloc(_ptr, _max);

			if (!_ptr) return 0;
		}
	}

	memcpy(_ptr + _pos_w, data, bytes);

	_pos_w += bytes;
	_ptr[_pos_w] = 0;

//	DBGLOG("SockBuf::write()\n")

	return bytes;
}

//----------------------------------------------------------------------------
//
int SockBuf::read(void* data, u32_t bytes)
{
//	DBGLOG("SockBuf::read()\n");

	bytes = peek(data, bytes);
	_pos_r += bytes;
	return bytes;
}

//----------------------------------------------------------------------------
//
int SockBuf::peek(void* data, u32_t bytes)
{
//	DBGLOG("SockBuf::peek()\n");
	
	if (data && bytes) {
		if (bytes > len())
			bytes = len();
		memcpy(data, pos(), bytes);
		return bytes;
	}

	return 0;
}

//----------------------------------------------------------------------------
//
void SockBuf::reset()
{
	if (_ptr)
		free(_ptr);
	
	_ptr = 0;
	_max = _pos_r = _pos_w = 0;

//	DBGLOG("SockBuf::reset()\n");
}

//----------------------------------------------------------------------------
//
SockBuf& SockBuf::discard(u32_t bytes)
{
	if (bytes > len())
		bytes = len();
	_pos_r += bytes;

	if (_pos_r >= _pos_w) {
		_pos_w = 0;
		_pos_r = 0;
		_ptr[0] = 0;
		
		if (_max > SOCKBUF_BLOCK_SIZE) {
			free(_ptr);
			_ptr = (u8_t*) malloc(SOCKBUF_BLOCK_SIZE);
			_max = SOCKBUF_BLOCK_SIZE;
		}
	}
	return *this;
}

#if SOCKLIB_TO_LUA
//----------------------------------------------------------------------------
//
SockBuf* SockBuf::mylua_this(lua_State* L, int idx)
{
	return LuaHelper::get<SockBuf>(L, SOCKLIB_BUF, idx);
//	if (!lua_istable(L, idx)) {
//		luaL_error(L, "%s.buf:w() bad call, not buf object", SockLib::libName());
//		return 0;
//	}
//
//	lua_getfield(L, idx, "__obj");
//	SockBuf* _this = *(SockBuf**) lua_touserdata(L, -1);
//	lua_pop(L, 1);
//	
//	return _this;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_w(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->w(ptr, len);
	} else {
		luaL_error(L, "%s:w(<unknown data>) .", SOCKLIB_BUF.c_str());
	}
	
	lua_pushvalue(L, 1);

	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_wl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->wl(ptr, len);
	} else {
		luaL_error(L, "%s:wl(<unknown data>) .", SOCKLIB_BUF.c_str());
	}

	lua_pushvalue(L, 1);

	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_ws(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	const char* v = luaL_checkstring(L, 2);
	_this->ws(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_wsl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	const char* v = luaL_checkstring(L, 2);
	_this->wsl(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_w8(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u8_t v = (u8_t) luaL_checkinteger(L, 2);
	_this->w8(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_w16(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u16_t v = (u16_t) luaL_checkinteger(L, 2);
	_this->w16(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_w32(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u32_t v = (u32_t) luaL_checkinteger(L, 2);
	_this->w32(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_w64(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u64_t v = (u64_t) luaL_checkinteger(L, 2);
	_this->w64(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_r(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	u32_t len = (u32_t) luaL_checkinteger(L, 2);
	u8_t* ptr = _this->r(len);
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 3)
		fmt = luaL_checkstring(L, 3);
	
	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_rl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	u32_t len = 0;
	u8_t* ptr = _this->rl(len);

	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_rs(lua_State* L)
{
	SockBuf* _this = mylua_this(L);

	const char* ptr = _this->rs();
	if (!ptr) ptr = "";
	u32_t len = (u32_t)strlen(ptr) + 1;
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_rsl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	const char* ptr = _this->rsl();
	if (!ptr) ptr = "";
	u32_t len = (u32_t)strlen(ptr);

	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_r8(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u8_t v = _this->r8();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_r16(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u16_t v = _this->r16();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_r32(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u32_t v = _this->r32();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_r64(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u64_t v = _this->r64();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_p(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	u32_t len = (u32_t) luaL_checkinteger(L, 2);
	u8_t* ptr = _this->p(len);
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 3)
		fmt = luaL_checkstring(L, 3);
	
	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_pl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	u32_t len = 0;
	u8_t* ptr = _this->pl(len);

	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_ps(lua_State* L)
{
	SockBuf* _this = mylua_this(L);

	const char* ptr = _this->ps();
	if (!ptr) ptr = "";
	u32_t len = (u32_t)strlen(ptr) + 1;
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_psl(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	const char* ptr = _this->psl();
	if (!ptr) ptr = "";
	u32_t len = (u32_t)strlen(ptr);

	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_p8(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u8_t v = _this->p8();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_p16(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u16_t v = _this->p16();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_p32(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u32_t v = _this->p32();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_p64(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u64_t v = _this->p64();
	lua_pushinteger(L, v);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_sub(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	int _le = (int)_this->len();

	int from = 1, end = -1;

	if (lua_gettop(L) >= 2)
		from = luaL_checkint(L, 2);
	if (lua_gettop(L) >= 3)
		end = luaL_checkint(L, 3);
	
	if (from == 0) from = 1;
	if (end == 0) end = -1;
	if (from < 0) from = _le + from + 1;
	if (end < 0) end = _le + end + 1;
	if (end > _le) end = _le;
	
	const char* fmt = "*" SOCKFMT_BUF;
	if (lua_gettop(L) >= 4)
		fmt = lua_tostring(L, 4);
	
	if (from <= _le && from > 0 && end > 0 && from <= end) {
		void* ptr = _this->pos() + from - 1;
		u32_t len = end - from + 1;
		
		return mylua_return_fmt(L, fmt, ptr, len);
	} else {
		lua_pushnil(L);
		return 1;
	}
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_skip(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u32_t v = (u32_t) luaL_checkinteger(L, 2);
	_this->skip(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_discard(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	u32_t v = (u32_t) luaL_checkinteger(L, 2);
	_this->discard(v);
	lua_pushvalue(L, 1);
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_reset(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	_this->reset();

	// already in top
//	lua_pushvalue(L, 1);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_buffer(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	void* ptr = (void*)_this->pos();
	u32_t len = _this->len();
	
	const char* fmt = 0;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);

	return mylua_return_fmt(L, fmt, ptr, len);
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_length(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	lua_pushinteger(L, _this->len());
//	lua_pushlightuserdata(L, _this->pos());
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	DBGLOG("SockBuf:mylua_index(%s)\n", key);

	lua_pushvalue(L, 1);
	if (int r = LuaHelper::mylua_index_walk(L, key))
		return r;
	
	if (/*0 == NameStrCmp(key, "buffer") || */
		0 == NameStrCmp(key, "str") || 0 == NameStrCmp(key, "string") ||
		0 == NameStrCmp(key, "pos") || 0 == NameStrCmp(key, "buf")) {
		lua_settop(L, 1);
		return mylua_buffer(L);
#if SOCKLIB_ALG
	} else if (0 == NameStrCmp(key, "md5")) {
		SockBuf* _this = mylua_this(L);
		u8_t hash[16]; char hex[128];
		MD5_build(hash, _this->pos(), _this->len());
		for (int i = 0; i < sizeof(hash); ++i)
			sprintf(hex + i * 2, "%02x", hash[i]);
		lua_pushstring(L, hex);
		return 1;
	} else if (0 == NameStrCmp(key, "sha1")) {
		SockBuf* _this = mylua_this(L);
		u8_t hash[20]; char hex[128];
		SHA1_build(hash, _this->pos(), _this->len());
		for (int i = 0; i < sizeof(hash); ++i)
			sprintf(hex + i * 2, "%02x", hash[i]);
		lua_pushstring(L, hex);
		return 1;
	} else if (0 == NameStrCmp(key, "b64") || 0 == NameStrCmp(key, "base64")) {
		SockBuf* _this = mylua_this(L);
		std::string b64 = Base64_encode(_this->pos(), _this->len());
		lua_pushstring(L, b64.c_str());
		return 1;
	} else if (0 == NameStrCmp(key, "crc") || 0 == NameStrCmp(key, "crc32")) {
		SockBuf* _this = mylua_this(L);
		u32_t crc = CRC32_build(_this->pos(), _this->len());
		lua_pushnumber(L, crc);
		return 1;
	} else if (0 == NameStrCmp(key, "hex")) {
		SockBuf* _this = mylua_this(L);
		if (!_this->len()) {
			lua_pushstring(L, "(null)");
			return 1;
		}
		u8_t* buf = _this->pos();
		char* hex = new char[_this->len() * 2 + 2];
		for (u32_t i = 0; i < _this->len(); ++i)
			sprintf(hex + i * 2, "%02X", buf[i]);
		lua_pushstring(L, hex);
		delete[] hex;
		return 1;
#endif // SOCKLIB_ALG
	} else if (/*0 == NameStrCmp(key, "length") || */0 == NameStrCmp(key, "len")) {
		return mylua_length(L);
	}

	lua_pushnil(L);
	
	return 1;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_gc(lua_State* L)
{
	SockBuf* _this = mylua_this(L);

	if (LuaHelper::found(_this)) {
		DBGLOG("%s{%p}:mylua_gc() free\n", SOCKLIB_BUF.c_str(), _this);
		LuaHelper::remove(_this);
		delete _this;
	} else {
		DBGLOG("%s{%p}:mylua_gc()\n", SOCKLIB_BUF.c_str(), _this);
	}

	return 0;
}

//----------------------------------------------------------------------------
//
int SockBuf::mylua_tostring(lua_State* L)
{
	SockBuf* _this = mylua_this(L);
	
	lua_pushfstring(L, "%s{ptr=%p, max=%d, posr=%d, posw=%d, len=%d}", SOCKLIB_BUF.c_str(),
		_this->_ptr, _this->_max, _this->_pos_r, _this->_pos_w, _this->len());
	
	return 1;
}

#endif // SOCKLIB_TO_LUA

#if SOCKLIB_ALG
///////////////////////////////////////////////////////////////////////////////
// CRC32
//
// CRC32 codes write by someone I don't know.
//
/* 
 ********************************************************************** 
 ** Copyright (C) 1990, RSA Data Security, Inc. All rights reserved. ** 
 **                                                                  ** 
 ** License to copy and use this software is granted provided that   ** 
 ** it is identified as the "RSA Data Security, Inc. MD5 Message     ** 
 ** Digest Algorithm" in all material mentioning or referencing this ** 
 ** software or this function.                                       ** 
 **                                                                  ** 
 ** License is also granted to make and use derivative works         ** 
 ** provided that such works are identified as "derived from the RSA ** 
 ** Data Security, Inc. MD5 Message Digest Algorithm" in all         ** 
 ** material mentioning or referencing the derived work.             ** 
 **                                                                  ** 
 ** RSA Data Security, Inc. makes no representations concerning      ** 
 ** either the merchantability of this software or the suitability   ** 
 ** of this software for any particular purpose.  It is provided "as ** 
 ** is" without express or implied warranty of any kind.             ** 
 **                                                                  ** 
 ** These notices must be retained in any copies of any part of this ** 
 ** documentation and/or software.                                   ** 
 ********************************************************************** 
 */

static u32_t crc_table[256] =
{
  0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L,
  0x706af48fL, 0xe963a535L, 0x9e6495a3L, 0x0edb8832L, 0x79dcb8a4L,
  0xe0d5e91eL, 0x97d2d988L, 0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L,
  0x90bf1d91L, 0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL,
  0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L, 0x136c9856L,
  0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L,
  0xfa0f3d63L, 0x8d080df5L, 0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L,
  0xa2677172L, 0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
  0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L,
  0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L, 0x26d930acL, 0x51de003aL,
  0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L,
  0xb8bda50fL, 0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L,
  0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL, 0x76dc4190L,
  0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL,
  0x9fbfe4a5L, 0xe8b8d433L, 0x7807c9a2L, 0x0f00f934L, 0x9609a88eL,
  0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
  0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL, 0x6c0695edL,
  0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L, 0x65b0d9c6L, 0x12b7e950L,
  0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L,
  0xfbd44c65L, 0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L,
  0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL, 0x4369e96aL,
  0x346ed9fcL, 0xad678846L, 0xda60b8d0L, 0x44042d73L, 0x33031de5L,
  0xaa0a4c5fL, 0xdd0d7cc9L, 0x5005713cL, 0x270241aaL, 0xbe0b1010L,
  0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
  0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L,
  0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL, 0xedb88320L, 0x9abfb3b6L,
  0x03b6e20cL, 0x74b1d29aL, 0xead54739L, 0x9dd277afL, 0x04db2615L,
  0x73dc1683L, 0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L,
  0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L, 0xf00f9344L,
  0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL,
  0x196c3671L, 0x6e6b06e7L, 0xfed41b76L, 0x89d32be0L, 0x10da7a5aL,
  0x67dd4accL, 0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
  0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L,
  0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL, 0xd80d2bdaL, 0xaf0a1b4cL,
  0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL,
  0x4669be79L, 0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L,
  0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL, 0xc5ba3bbeL,
  0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L,
  0x2cd99e8bL, 0x5bdeae1dL, 0x9b64c2b0L, 0xec63f226L, 0x756aa39cL,
  0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
  0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L, 0x92d28e9bL,
  0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L, 0x86d3d2d4L, 0xf1d4e242L,
  0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L,
  0x18b74777L, 0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL,
  0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L, 0xa00ae278L,
  0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L, 0xa7672661L, 0xd06016f7L,
  0x4969474dL, 0x3e6e77dbL, 0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L,
  0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
  0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L,
  0xcdd70693L, 0x54de5729L, 0x23d967bfL, 0xb3667a2eL, 0xc4614ab8L,
  0x5d681b02L, 0x2a6f2b94L, 0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL,
  0x2d02ef8dL
};

#define CRC_DO1(buf)  crc = crc_table[((int)crc ^ (*buf++)) & 0xff] ^ (crc >> 8);
#define CRC_DO2(buf)  CRC_DO1(buf); CRC_DO1(buf);
#define CRC_DO4(buf)  CRC_DO2(buf); CRC_DO2(buf);
#define CRC_DO8(buf)  CRC_DO4(buf); CRC_DO4(buf);

u32_t CRC32_build(const void* data, u32_t len, u32_t crc)
{
	u8_t* buf = (u8_t*)data;

	if (!buf) return 0L;

	crc = crc ^ 0xffffffffL;

	while (len >= 8) {
		CRC_DO8(buf);
		len -= 8;
	}

	if (len) {
		do {
			CRC_DO1(buf);
		} while (--len);
	}
	return crc ^ 0xffffffffL;
}

///////////////////////////////////////////////////////////////////////////////
// RC4
//

#if SOCKLIB_TO_LUA
int RC4::mylua_setkey(lua_State* L)
{
	RC4* _this = LuaHelper::get<RC4>(L, SOCKLIB_RC4, 1);
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->setKey((const u8_t*)ptr, len);
	} else {
		luaL_error(L, "%s:setkey(<unknown data>)", SOCKLIB_RC4.c_str());
	}
	
	lua_pushvalue(L, 1);

	return 1;
}

int RC4::mylua_process(lua_State* L)
{
	RC4* _this = LuaHelper::get<RC4>(L, SOCKLIB_RC4, 1);

	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		u8_t* out = (u8_t*)malloc(len);
		_this->process((const u8_t*)ptr, out, len);
		SockBuf* buf = new SockBuf();
		buf->w(out, len);
		free(out);
		return LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, buf);
	} else {
		luaL_error(L, "%s:process(<unknown data>)", SOCKLIB_RC4.c_str());
	}
	
	lua_pushnil(L);
	
	return 1;
}

int RC4::mylua_gc(lua_State* L)
{
	RC4* _this = LuaHelper::get<RC4>(L, SOCKLIB_RC4, 1);
	
	if (LuaHelper::found(_this)) {
		DBGLOG("%s{%p}:mylua_gc() free\n", SOCKLIB_RC4.c_str(), _this);
		LuaHelper::remove(_this);
		delete _this;
	} else {
		DBGLOG("%s{%p}:mylua_gc()\n", SOCKLIB_RC4.c_str(), _this);
	}
	
	return 0;
}

int RC4::mylua_tostring(lua_State* L)
{
//	RC4* _this = LuaHelper::get<RC4>(L, SOCKLIB_RC4, 1);
	
	lua_pushfstring(L, "%s", SOCKLIB_RC4.c_str());
	
	return 1;
}

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// MD5
//
// MD5 codes write by someone I don't know.
//
/* 
 ********************************************************************** 
 ** Copyright (C) 1990, RSA Data Security, Inc. All rights reserved. ** 
 **                                                                  ** 
 ** License to copy and use this software is granted provided that   ** 
 ** it is identified as the "RSA Data Security, Inc. MD5 Message     ** 
 ** Digest Algorithm" in all material mentioning or referencing this ** 
 ** software or this function.                                       ** 
 **                                                                  ** 
 ** License is also granted to make and use derivative works         ** 
 ** provided that such works are identified as "derived from the RSA ** 
 ** Data Security, Inc. MD5 Message Digest Algorithm" in all         ** 
 ** material mentioning or referencing the derived work.             ** 
 **                                                                  ** 
 ** RSA Data Security, Inc. makes no representations concerning      ** 
 ** either the merchantability of this software or the suitability   ** 
 ** of this software for any particular purpose.  It is provided "as ** 
 ** is" without express or implied warranty of any kind.             ** 
 **                                                                  ** 
 ** These notices must be retained in any copies of any part of this ** 
 ** documentation and/or software.                                   ** 
 ********************************************************************** 
 */
 
#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

static void md5_encode(u8_t*, u32_t*, u32_t);
static void md5_decode(u32_t *, u8_t*, u32_t);

static u8_t md5_padding[64] =
{
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#define MD5_F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | (~z)))

#define MD5_ROL(x, n) (((x) <<(n)) | ((x) >> (32-(n))))

#define MD5_FF(a, b, c, d, x, s, ac) { (a) += MD5_F ((b), (c), (d)) + (x) + (u32_t)(ac); (a) = MD5_ROL ((a), (s)); (a) += (b);  }
#define MD5_GG(a, b, c, d, x, s, ac) { (a) += MD5_G ((b), (c), (d)) + (x) + (u32_t)(ac); (a) = MD5_ROL ((a), (s)); (a) += (b); }
#define MD5_HH(a, b, c, d, x, s, ac) { (a) += MD5_H ((b), (c), (d)) + (x) + (u32_t)(ac); (a) = MD5_ROL ((a), (s)); (a) += (b); }
#define MD5_II(a, b, c, d, x, s, ac) { (a) += MD5_I ((b), (c), (d)) + (x) + (u32_t)(ac); (a) = MD5_ROL ((a), (s)); (a) += (b); }

void md5_encode(u8_t* output, u32_t* input, u32_t len)
{
    u32_t i, j;

    for (i = 0, j = 0; j < len; i++, j += 4)
	{
        output[j] = (u8_t)(input[i] & 0xff);
        output[j + 1] = (u8_t)((input[i] >> 8) & 0xff);
        output[j + 2] = (u8_t)((input[i] >> 16) & 0xff);
        output[j + 3] = (u8_t)((input[i] >> 24) & 0xff);
    }

}

void md5_decode(u32_t* output, u8_t* input, u32_t len)
{
    u32_t i, j;

    for (i = 0, j = 0; j < len; i++, j += 4)
        output[i] =
            ((u32_t)input[j]) | (((u32_t)input[j + 1]) << 8) |
            (((u32_t)input[j + 2]) << 16) |
            (((u32_t)input[j + 3]) << 24);
}

static inline void md5_transform(u32_t state[4], const u8_t buffer[64])
{
    u32_t a = state[0],
      b = state[1],
      c = state[2],
      d = state[3],
      x[16];

    md5_decode(x, (u8_t*)buffer, 64);

	// Round 1 
    MD5_FF( a, b, c, d, x[0], MD5_S11, 0xd76aa478 );    /* 1 */
    MD5_FF( d, a, b, c, x[1], MD5_S12, 0xe8c7b756 );    /* 2 */
    MD5_FF( c, d, a, b, x[2], MD5_S13, 0x242070db );    /* 3 */
    MD5_FF( b, c, d, a, x[3], MD5_S14, 0xc1bdceee );    /* 4 */
    MD5_FF( a, b, c, d, x[4], MD5_S11, 0xf57c0faf );    /* 5 */
    MD5_FF( d, a, b, c, x[5], MD5_S12, 0x4787c62a );    /* 6 */
    MD5_FF( c, d, a, b, x[6], MD5_S13, 0xa8304613 );    /* 7 */
    MD5_FF( b, c, d, a, x[7], MD5_S14, 0xfd469501 );    /* 8 */
    MD5_FF( a, b, c, d, x[8], MD5_S11, 0x698098d8 );    /* 9 */
    MD5_FF( d, a, b, c, x[9], MD5_S12, 0x8b44f7af );    /* 10 */
    MD5_FF( c, d, a, b, x[10], MD5_S13, 0xffff5bb1 );   /* 11 */
    MD5_FF( b, c, d, a, x[11], MD5_S14, 0x895cd7be );   /* 12 */
    MD5_FF( a, b, c, d, x[12], MD5_S11, 0x6b901122 );   /* 13 */
    MD5_FF( d, a, b, c, x[13], MD5_S12, 0xfd987193 );   /* 14 */
    MD5_FF( c, d, a, b, x[14], MD5_S13, 0xa679438e );   /* 15 */
    MD5_FF( b, c, d, a, x[15], MD5_S14, 0x49b40821 );   /* 16 */

	// Round 2 
    MD5_GG( a, b, c, d, x[1], MD5_S21, 0xf61e2562 );    /* 17 */
    MD5_GG( d, a, b, c, x[6], MD5_S22, 0xc040b340 );    /* 18 */
    MD5_GG( c, d, a, b, x[11], MD5_S23, 0x265e5a51 );   /* 19 */
    MD5_GG( b, c, d, a, x[0], MD5_S24, 0xe9b6c7aa );    /* 20 */
    MD5_GG( a, b, c, d, x[5], MD5_S21, 0xd62f105d );    /* 21 */
    MD5_GG( d, a, b, c, x[10], MD5_S22, 0x2441453 );    /* 22 */
    MD5_GG( c, d, a, b, x[15], MD5_S23, 0xd8a1e681 );   /* 23 */
    MD5_GG( b, c, d, a, x[4], MD5_S24, 0xe7d3fbc8 );    /* 24 */
    MD5_GG( a, b, c, d, x[9], MD5_S21, 0x21e1cde6 );    /* 25 */
    MD5_GG( d, a, b, c, x[14], MD5_S22, 0xc33707d6 );   /* 26 */
    MD5_GG( c, d, a, b, x[3], MD5_S23, 0xf4d50d87 );    /* 27 */
    MD5_GG( b, c, d, a, x[8], MD5_S24, 0x455a14ed );    /* 28 */
    MD5_GG( a, b, c, d, x[13], MD5_S21, 0xa9e3e905 );   /* 29 */
    MD5_GG( d, a, b, c, x[2], MD5_S22, 0xfcefa3f8 );    /* 30 */
    MD5_GG( c, d, a, b, x[7], MD5_S23, 0x676f02d9 );    /* 31 */
    MD5_GG( b, c, d, a, x[12], MD5_S24, 0x8d2a4c8a );   /* 32 */

	// Round 3 
    MD5_HH( a, b, c, d, x[5], MD5_S31, 0xfffa3942 );    /* 33 */
    MD5_HH( d, a, b, c, x[8], MD5_S32, 0x8771f681 );    /* 34 */
    MD5_HH( c, d, a, b, x[11], MD5_S33, 0x6d9d6122 );   /* 35 */
    MD5_HH( b, c, d, a, x[14], MD5_S34, 0xfde5380c );   /* 36 */
    MD5_HH( a, b, c, d, x[1], MD5_S31, 0xa4beea44 );    /* 37 */
    MD5_HH( d, a, b, c, x[4], MD5_S32, 0x4bdecfa9 );    /* 38 */
    MD5_HH( c, d, a, b, x[7], MD5_S33, 0xf6bb4b60 );    /* 39 */
    MD5_HH( b, c, d, a, x[10], MD5_S34, 0xbebfbc70 );   /* 40 */
    MD5_HH( a, b, c, d, x[13], MD5_S31, 0x289b7ec6 );   /* 41 */
    MD5_HH( d, a, b, c, x[0], MD5_S32, 0xeaa127fa );    /* 42 */
    MD5_HH( c, d, a, b, x[3], MD5_S33, 0xd4ef3085 );    /* 43 */
    MD5_HH( b, c, d, a, x[6], MD5_S34, 0x4881d05 ); /* 44 */
    MD5_HH( a, b, c, d, x[9], MD5_S31, 0xd9d4d039 );    /* 45 */
    MD5_HH( d, a, b, c, x[12], MD5_S32, 0xe6db99e5 );   /* 46 */
    MD5_HH( c, d, a, b, x[15], MD5_S33, 0x1fa27cf8 );   /* 47 */
    MD5_HH( b, c, d, a, x[2], MD5_S34, 0xc4ac5665 );    /* 48 */

	// Round 4 
    MD5_II( a, b, c, d, x[0], MD5_S41, 0xf4292244 );    /* 49 */
    MD5_II( d, a, b, c, x[7], MD5_S42, 0x432aff97 );    /* 50 */
    MD5_II( c, d, a, b, x[14], MD5_S43, 0xab9423a7 );   /* 51 */
    MD5_II( b, c, d, a, x[5], MD5_S44, 0xfc93a039 );    /* 52 */
    MD5_II( a, b, c, d, x[12], MD5_S41, 0x655b59c3 );   /* 53 */
    MD5_II( d, a, b, c, x[3], MD5_S42, 0x8f0ccc92 );    /* 54 */
    MD5_II( c, d, a, b, x[10], MD5_S43, 0xffeff47d );   /* 55 */
    MD5_II( b, c, d, a, x[1], MD5_S44, 0x85845dd1 );    /* 56 */
    MD5_II( a, b, c, d, x[8], MD5_S41, 0x6fa87e4f );    /* 57 */
    MD5_II( d, a, b, c, x[15], MD5_S42, 0xfe2ce6e0 );   /* 58 */
    MD5_II( c, d, a, b, x[6], MD5_S43, 0xa3014314 );    /* 59 */
    MD5_II( b, c, d, a, x[13], MD5_S44, 0x4e0811a1 );   /* 60 */
    MD5_II( a, b, c, d, x[4], MD5_S41, 0xf7537e82 );    /* 61 */
    MD5_II( d, a, b, c, x[11], MD5_S42, 0xbd3af235 );   /* 62 */
    MD5_II( c, d, a, b, x[2], MD5_S43, 0x2ad7d2bb );    /* 63 */
    MD5_II( b, c, d, a, x[9], MD5_S44, 0xeb86d391 );    /* 64 */

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    memset((u8_t*)x, 0, sizeof(x));
}

void MD5::init()
{
    _count[0] = _count[1] = 0;

    _state[0] = 0x67452301;
    _state[1] = 0xefcdab89;
    _state[2] = 0x98badcfe;
    _state[3] = 0x10325476;
}

void MD5::update(const u8_t* data, u32_t len)
{
	u32_t i, index, partLen;

    index = (u32_t)((_count[0] >> 3) & 0x3F);

    if ((_count[0] += ((u32_t) len << 3)) < ((u32_t)len << 3))
        _count[1]++;

    _count[1] += ((u32_t)len >> 29);

    partLen = 64 - index;

    if (len >= partLen) {
	    memcpy((u8_t*)&_buffer[index], (u8_t*) data, partLen);
        md5_transform(_state, _buffer);

        for (i = partLen; i + 63 < len; i += 64)
            md5_transform(_state, &data[i]);

        index = 0;
    } else {
        i = 0;
	}

    memcpy((u8_t*)&_buffer[index], (u8_t*)&data[i], len - i );
}

void MD5::final(u8_t digest[16])
{
	u8_t bits[8];
	u32_t index, padLen;

    md5_encode(bits, _count, 8);

    index = (u32_t)((_count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);

    update(md5_padding, padLen);
    update(bits, 8);

    md5_encode(digest, _state, 16);

    memset(this, 0, sizeof(MD5));
}

void MD5::build(u8_t hash[16], const void* buf, u32_t len)
{
	MD5 obj;
	obj.init();
	obj.update((u8_t*)buf, len);
	obj.final(hash);
}

#if SOCKLIB_TO_LUA
int MD5::mylua_init(lua_State* L)
{
	MD5* _this = LuaHelper::get<MD5>(L, SOCKLIB_MD5, 1);
	_this->init();
//	lua_pushvalue(L, 1);	// already in top
	return 1;
}

int MD5::mylua_update(lua_State* L)
{
	MD5* _this = LuaHelper::get<MD5>(L, SOCKLIB_MD5, 1);

	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->update((u8_t*)ptr, len);
	} else {
		luaL_error(L, "%s:update(<unknown data>) .", SOCKLIB_MD5.c_str());
	}

	lua_pushvalue(L, 1);
	
	return 1;
}

int MD5::mylua_final(lua_State* L)
{
	MD5* _this = LuaHelper::get<MD5>(L, SOCKLIB_MD5, 1);
	
	u8_t hash[16];
	_this->final(hash);
	
	const char* fmt = "*" SOCKFMT_HEX;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);
	
	return mylua_return_fmt(L, fmt, hash, 16);
}

int MD5::mylua_gc(lua_State* L)
{
	MD5* _this = LuaHelper::get<MD5>(L, SOCKLIB_MD5, 1);
	
	if (LuaHelper::found(_this)) {
		DBGLOG("%s{%p}:mylua_gc() free\n", SOCKLIB_MD5.c_str(), _this);
		LuaHelper::remove(_this);
		delete _this;
	} else {
		DBGLOG("%s{%p}:mylua_gc()\n", SOCKLIB_MD5.c_str(), _this);
	}
	
	return 0;
}

int MD5::mylua_tostring(lua_State* L)
{
//	MD5* _this = LuaHelper::get<MD5>(L, SOCKLIB_MD5, 1);
	
	lua_pushfstring(L, "%s", SOCKLIB_MD5.c_str());
	
	return 1;
}

#endif // SOCKLIB_TO_LUA

///////////////////////////////////////////////////////////////////////////////
// SHA1
//
// SHA1 codes write by someone I don't know.
//
/* 
 ********************************************************************** 
 ** Copyright (C) 1990, RSA Data Security, Inc. All rights reserved. ** 
 **                                                                  ** 
 ** License to copy and use this software is granted provided that   ** 
 ** it is identified as the "RSA Data Security, Inc. MD5 Message     ** 
 ** Digest Algorithm" in all material mentioning or referencing this ** 
 ** software or this function.                                       ** 
 **                                                                  ** 
 ** License is also granted to make and use derivative works         ** 
 ** provided that such works are identified as "derived from the RSA ** 
 ** Data Security, Inc. MD5 Message Digest Algorithm" in all         ** 
 ** material mentioning or referencing the derived work.             ** 
 **                                                                  ** 
 ** RSA Data Security, Inc. makes no representations concerning      ** 
 ** either the merchantability of this software or the suitability   ** 
 ** of this software for any particular purpose.  It is provided "as ** 
 ** is" without express or implied warranty of any kind.             ** 
 **                                                                  ** 
 ** These notices must be retained in any copies of any part of this ** 
 ** documentation and/or software.                                   ** 
 ********************************************************************** 
 */

#define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

#ifndef WORDS_BIGENDIAN
#define SHA1_BLK0(i) (block->l[i] = (SHA1_ROL(block->l[i],24)&0xFF00FF00)|(SHA1_ROL(block->l[i],8)&0x00FF00FF))
#else
#define SHA1_BLK0(i) block->l[i]
#endif
#define SHA1_BLK(i) (block->l[i&15] = SHA1_ROL(block->l[(i+13)&15]^block->l[(i+8)&15]^block->l[(i+2)&15]^block->l[i&15],1))

#define SHA1_R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+SHA1_BLK0(i)+0x5A827999+SHA1_ROL(v,5);w=SHA1_ROL(w,30);
#define SHA1_R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+SHA1_BLK(i)+0x5A827999+SHA1_ROL(v,5);w=SHA1_ROL(w,30);
#define SHA1_R2(v,w,x,y,z,i) z+=(w^x^y)+SHA1_BLK(i)+0x6ED9EBA1+SHA1_ROL(v,5);w=SHA1_ROL(w,30);
#define SHA1_R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+SHA1_BLK(i)+0x8F1BBCDC+SHA1_ROL(v,5);w=SHA1_ROL(w,30);
#define SHA1_R4(v,w,x,y,z,i) z+=(w^x^y)+SHA1_BLK(i)+0xCA62C1D6+SHA1_ROL(v,5);w=SHA1_ROL(w,30);


static inline void sha1_transform(u32_t state[5], const u8_t buffer[64])
{
    u32_t a, b, c, d, e;
    typedef union
    {
        u8_t	c[64];
        u32_t	l[16];
    } CHAR64LONG16;
	
	CHAR64LONG16 *block;
	
    static u8_t workspace[64];
	
    block = (CHAR64LONG16 *) workspace;
    memcpy (block, buffer, 64);

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    SHA1_R0(a, b, c, d, e, 0x00); SHA1_R0(e, a, b, c, d, 0x01); SHA1_R0(d, e, a, b, c, 0x02); SHA1_R0(c, d, e, a, b, 0x03);
    SHA1_R0(b, c, d, e, a, 0x04); SHA1_R0(a, b, c, d, e, 0x05); SHA1_R0(e, a, b, c, d, 0x06); SHA1_R0(d, e, a, b, c, 0x07);
    SHA1_R0(c, d, e, a, b, 0x08); SHA1_R0(b, c, d, e, a, 0x09); SHA1_R0(a, b, c, d, e, 0x0a); SHA1_R0(e, a, b, c, d, 0x0b);
    SHA1_R0(d, e, a, b, c, 0x0c); SHA1_R0(c, d, e, a, b, 0x0d); SHA1_R0(b, c, d, e, a, 0x0e); SHA1_R0(a, b, c, d, e, 0x0f);
    SHA1_R1(e, a, b, c, d, 0x10); SHA1_R1(d, e, a, b, c, 0x11); SHA1_R1(c, d, e, a, b, 0x12); SHA1_R1(b, c, d, e, a, 0x13);
    SHA1_R2(a, b, c, d, e, 0x14); SHA1_R2(e, a, b, c, d, 0x15); SHA1_R2(d, e, a, b, c, 0x16); SHA1_R2(c, d, e, a, b, 0x17);
    SHA1_R2(b, c, d, e, a, 0x18); SHA1_R2(a, b, c, d, e, 0x19); SHA1_R2(e, a, b, c, d, 0x1a); SHA1_R2(d, e, a, b, c, 0x1b);
    SHA1_R2(c, d, e, a, b, 0x1c); SHA1_R2(b, c, d, e, a, 0x1d); SHA1_R2(a, b, c, d, e, 0x1e); SHA1_R2(e, a, b, c, d, 0x1f);
    SHA1_R2(d, e, a, b, c, 0x20); SHA1_R2(c, d, e, a, b, 0x21); SHA1_R2(b, c, d, e, a, 0x22); SHA1_R2(a, b, c, d, e, 0x23);
    SHA1_R2(e, a, b, c, d, 0x24); SHA1_R2(d, e, a, b, c, 0x25); SHA1_R2(c, d, e, a, b, 0x26); SHA1_R2(b, c, d, e, a, 0x27);
    SHA1_R3(a, b, c, d, e, 0x28); SHA1_R3(e, a, b, c, d, 0x29); SHA1_R3(d, e, a, b, c, 0x2a); SHA1_R3(c, d, e, a, b, 0x2b);
    SHA1_R3(b, c, d, e, a, 0x2c); SHA1_R3(a, b, c, d, e, 0x2d); SHA1_R3(e, a, b, c, d, 0x2e); SHA1_R3(d, e, a, b, c, 0x2f);
    SHA1_R3(c, d, e, a, b, 0x30); SHA1_R3(b, c, d, e, a, 0x31); SHA1_R3(a, b, c, d, e, 0x32); SHA1_R3(e, a, b, c, d, 0x33);
    SHA1_R3(d, e, a, b, c, 0x34); SHA1_R3(c, d, e, a, b, 0x35); SHA1_R3(b, c, d, e, a, 0x36); SHA1_R3(a, b, c, d, e, 0x37);
    SHA1_R3(e, a, b, c, d, 0x38); SHA1_R3(d, e, a, b, c, 0x39); SHA1_R3(c, d, e, a, b, 0x3a); SHA1_R3(b, c, d, e, a, 0x3b);
    SHA1_R4(a, b, c, d, e, 0x3c); SHA1_R4(e, a, b, c, d, 0x3d); SHA1_R4(d, e, a, b, c, 0x3e); SHA1_R4(c, d, e, a, b, 0x3f);
    SHA1_R4(b, c, d, e, a, 0x40); SHA1_R4(a, b, c, d, e, 0x41); SHA1_R4(e, a, b, c, d, 0x42); SHA1_R4(d, e, a, b, c, 0x43);
    SHA1_R4(c, d, e, a, b, 0x44); SHA1_R4(b, c, d, e, a, 0x45); SHA1_R4(a, b, c, d, e, 0x46); SHA1_R4(e, a, b, c, d, 0x47);
    SHA1_R4(d, e, a, b, c, 0x48); SHA1_R4(c, d, e, a, b, 0x49); SHA1_R4(b, c, d, e, a, 0x4a); SHA1_R4(a, b, c, d, e, 0x4b);
    SHA1_R4(e, a, b, c, d, 0x4c); SHA1_R4(d, e, a, b, c, 0x4d); SHA1_R4(c, d, e, a, b, 0x4e); SHA1_R4(b, c, d, e, a, 0x4f);
	
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;

    a = b = c = d = e = 0;
}

void SHA1::init()
{
    _state[0] = 0x67452301;
    _state[1] = 0xEFCDAB89;
    _state[2] = 0x98BADCFE;
    _state[3] = 0x10325476;
    _state[4] = 0xC3D2E1F0;
    _count[0] = _count[1] = 0;
}

void SHA1::update(const u8_t* data, u32_t len)
{
    u32_t i, j;
	
    j = _count[0];
    if ((_count[0] += len << 3) < j)
        _count[1] += (len >> 29) + 1;
    j = (j >> 3) & 63;
    if ((j + len) > 63)
	{
        memcpy(&_buffer[j], data, (i = 64 - j));
        sha1_transform(_state, _buffer);
        for (; i + 63 < len; i += 64) {
            sha1_transform(_state, &((const u8_t*)data)[i]);
        }
        j = 0;
    } else
        i = 0;
    memcpy(&_buffer[j], &((const u8_t *)data)[i], len - i);
}

void SHA1::final(u8_t digest[20])
{
    u32_t i;
    u8_t finalcount[8];
	
    for (i = 0; i < 8; i++) {
        finalcount[i] = (u8_t) ((_count[(i >= 4 ? 0 : 1)]
			>> ((3 - (i & 3)) * 8)) & 255);       /* Endian independent */
    }
    update((u8_t*)"\200", 1);
    while ((_count[0] & 504) != 448) {
        update((u8_t*)"\0", 1);
    }
    update(finalcount, 8); /* Should cause a SHA1Transform() */
	
    if (digest != 0) {
        for (i = 0; i < 20; i++) {
            digest[i] = (u8_t)
                ((_state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
        }
    }
}

void SHA1::build(u8_t hash[20], const void* data, u32_t len)
{
	SHA1 obj;
	obj.init();
	obj.update((u8_t*)data, len);
	obj.final(hash);
}

#if SOCKLIB_TO_LUA
int SHA1::mylua_init(lua_State* L)
{
	SHA1* _this = LuaHelper::get<SHA1>(L, SOCKLIB_SHA1, 1);
	_this->init();
//	lua_pushvalue(L, 1);	// already in top
	return 1;
}

int SHA1::mylua_update(lua_State* L)
{
	SHA1* _this = LuaHelper::get<SHA1>(L, SOCKLIB_SHA1, 1);

	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		_this->update((u8_t*)ptr, len);
	} else {
		luaL_error(L, "%s:update(<unknown data>) .", SOCKLIB_SHA1.c_str());
	}

	lua_pushvalue(L, 1);
	
	return 1;
}

int SHA1::mylua_final(lua_State* L)
{
	SHA1* _this = LuaHelper::get<SHA1>(L, SOCKLIB_SHA1, 1);
	
	u8_t hash[20];
	_this->final(hash);
	
	const char* fmt = "*" SOCKFMT_HEX;
	if (lua_gettop(L) >= 2)
		fmt = luaL_checkstring(L, 2);
	
	return mylua_return_fmt(L, fmt, hash, 20);
}

int SHA1::mylua_gc(lua_State* L)
{
	SHA1* _this = LuaHelper::get<SHA1>(L, SOCKLIB_SHA1, 1);
	
	if (LuaHelper::found(_this)) {
		DBGLOG("%s{%p}:mylua_gc() free\n", SOCKLIB_SHA1.c_str(), _this);
		LuaHelper::remove(_this);
		delete _this;
	} else {
		DBGLOG("%s{%p}:mylua_gc()\n", SOCKLIB_SHA1.c_str(), _this);
	}
	
	return 0;
}

int SHA1::mylua_tostring(lua_State* L)
{
//	SHA1* _this = LuaHelper::get<SHA1>(L, SOCKLIB_SHA1, 1);
	
	lua_pushfstring(L, "%s", SOCKLIB_SHA1.c_str());
	
	return 1;
}

#endif // SOCKLIB_TO_LUA


///////////////////////////////////////////////////////////////////////////////
// Base64
//
static char b64_encbyte(u8_t c)
{
	if (c <= 25)
		return 'A' + c;
	else if (c >= 26 && c <= 51)
		return 'a' + (c - 26);
	else if (c >= 52 && c <= 61)
		return '0' + (c - 52);
	else if (c == 62)
		return '+';
	else if (c == 63)
		return '/';
	else
		return '=';
}

static u8_t b64_decbyte(const char* szSrc, int& pos, int maxLen)
{
	char c;
	
	// 跳过空格、回车换行等空白及无效字符
	while (pos < maxLen)
	{
		c = szSrc[pos++];

		if (::isupper(c)) return c - 'A';
		else if (::islower(c)) return c - 'a' + 26;
		else if (::isdigit(c)) return c - '0' + 52;
		else if (c == '+') return 62;
		else if (c == '/') return 63;
		else if (c == '=') return 0;
	}

	return 0;
}

/// 编码
static int b64_encode(const u8_t* szSrc, int lenSrc, char* szDst)
{
	u8_t c;
	int i = 0;

	char* oldDst = szDst;
	
	while (i < lenSrc)
	{
		c = (0xFC & szSrc[i]) >> 2; 
		*szDst = b64_encbyte(c);
		i++; szDst++;

		if (i >= lenSrc)
		{
			c = (0x03 & szSrc[i-1]) << 4;
			*szDst = b64_encbyte(c);
			szDst++;
			break;
		}
		
		c = (0x03 & szSrc[i-1]) << 4 | (0xF0 & szSrc[i]) >> 4; 
		*szDst = b64_encbyte(c);
		i++; szDst++;

		if (i >= lenSrc)
		{
			c = (0x0F & szSrc[i-1]) << 2;
			*szDst = b64_encbyte(c);
			szDst++;
			break;
		}
		c = (0x0F & szSrc[i-1])<<2 | (0xC0 & szSrc[i])>>6;
		*szDst = b64_encbyte(c);
		szDst++;

		c = 0x3F & szSrc[i];
		*szDst = b64_encbyte(c);
		i++; szDst++;
	}
	
	int n = (lenSrc * 4) % 3;
	for (int j = 0; j < n; ++j) {
		*szDst = '=';
		i++; szDst++;
	}

	*szDst = 0;

	return (int)(szDst - oldDst);
}

/// 解码
static int b64_decode(const char* szSrc, int lenSrc, char* szDst)
{
	u8_t c0, c1, c2, c3, c;

	char* oldDst = szDst;
	
	int addLen = lenSrc % 4; 
	
	for (int i = 0; i < (lenSrc + addLen); )
	{
		if (i < lenSrc) c0 = b64_decbyte(szSrc, i, lenSrc); else { c0 = 0; i++; }
		if (i < lenSrc) c1 = b64_decbyte(szSrc, i, lenSrc); else { c1 = 0; i++; }
		if (i < lenSrc) c2 = b64_decbyte(szSrc, i, lenSrc); else { c2 = 0; i++; }
		if (i < lenSrc) c3 = b64_decbyte(szSrc, i, lenSrc); else { c3 = 0; i++; }
		
		c = c0 << 2 | c1 >> 4;
		*szDst = c;
		szDst++;

		c = c1 << 4 | c2 >> 2;
		*szDst = c;
		szDst++;

		c = c2 << 6 | c3;
		*szDst = c;
		szDst++;
	}

	*szDst = 0;

	return (int)(szDst - oldDst);
}

/// 编码
std::string Base64_encode(const void* buf, u32_t len)
{
	if (!len)
		len = (u32_t)strlen((const char*) buf);

	std::string str;
	
	char* out = new char[len * 4];
	if (out) {
		int ret = b64_encode((const u8_t*)buf, len, out);
		str.assign(out, ret);
		delete[] out;
	}

	return str;
}

/// 解码(可能返回二进制)
std::string Base64_decode(const char* sz, u32_t len)
{
	if (!len)
		len = (u32_t)strlen(sz);

	std::string str;

	char* out = new char[len * 4];
	if (out) {
		int ret = b64_decode((const char*)sz, len, out);
		str.assign(out, ret);
		delete[] out;
	}

	return str;
}

#endif // SOCKLIB_ALG

///////////////////////////////////////////////////////////////////////////////
// Timer
//
Timer::TimerMap Timer::_refs;
Timer::TimerMap Timer::_news;
Timer::TmrIdMap Timer::_dies;

#if SOCKLIB_TO_LUA
Timer::TmrIdMap Timer::_mylua_refs;
#endif

u64_t Timer::add(Timer& tmr)
{
	static u64_t _base = 0;
	
	tmr._timerId = ++_base;
	_news[tmr._timerId] = tmr;

#if SOCKLIB_TO_LUA
	if (tmr._mylua_ref >= 0)
		_mylua_refs[tmr._timerId] = tmr._mylua_ref;
#endif

	return tmr._timerId;
}

void Timer::remove(u64_t tmrId)
{
	_news.erase(tmrId);
	_dies[tmrId] = 1;

#if SOCKLIB_TO_LUA
	TmrIdMap::iterator it = _mylua_refs.find(tmrId);
	if (it != _mylua_refs.end()) {
		luaL_unref(SockLib::luaState(), LUA_REGISTRYINDEX, it->second);
		_mylua_refs.erase(it);
	}
#endif // SOCKLIB_TO_LUA
}

void Timer::poll()
{
	for (auto& it : _news) {
		_refs[it.first] = it.second;
	}
	_news.clear();
	
	for (auto& it : _dies) {
		_refs.erase(it.first);
	}
	_dies.clear();
	
	u64_t tick = Util::tick();
	for (auto& it : _refs) {
		it.second.onTick(tick);
	}
}

void Timer::onTick(u64_t tick)
{
	if (!_curTick || _curTick > tick)
		_curTick = tick;

	if ((_curTick + _interval) <= tick) {
		_curTick = tick;

		++_curLoops;
		
		if (_maxLoops < 0 || _curLoops <= _maxLoops) {
			if (_callback) {
				if (!_callback(*this))
					Timer::remove(timerId());
			}
		}
		
		if (_maxLoops >= 0 && _curLoops > _maxLoops)
			Timer::remove(timerId());
	}
}
	
///////////////////////////////////////////////////////////////////////////////
// Util
//

Util::IPCache Util::_ipcache;
std::mutex Util::_ipmutex;

#ifdef _WIN32
int gettimeofday(struct timeval *tp, void *tzp)
{
	time_t clock;
	struct tm tm;
	SYSTEMTIME wtm;
	GetLocalTime(&wtm);
	tm.tm_year = wtm.wYear - 1900;
	tm.tm_mon = wtm.wMonth - 1;
	tm.tm_mday = wtm.wDay;
	tm.tm_hour = wtm.wHour;
	tm.tm_min = wtm.wMinute;
	tm.tm_sec = wtm.wSecond;
	tm.tm_isdst = -1;
	clock = mktime(&tm);
	tp->tv_sec = clock;
	tp->tv_usec = wtm.wMilliseconds * 1000;
	return (0);
}
#endif // _WIN32

u64_t Util::tick()
{
    struct timeval tv;     
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}


//----------------------------------------------------------------------------
//	gethostbyname() is blocking
//
u32_t Util::ips2n(const std::string& addr)
{
	u32_t ip = inet_addr(addr.c_str());

	if (!ip || ip == INADDR_NONE) {
		hostent* host = ::gethostbyname(addr.c_str());
		if (host && host->h_length)  {
			sockaddr_in addr = { 0 };
			memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
			ip = addr.sin_addr.s_addr;
		}
	}
	
	return (ip == INADDR_NONE ? 0 : ip);
}

std::string Util::ipn2s(u32_t ip)
{
	struct in_addr iaddr;
	iaddr.s_addr = ip;
	std::string ret = inet_ntoa(iaddr);
	return ret;
}

u32_t Util::ipprobe(const std::string& addr)
{
	u32_t ip = inet_addr(addr.c_str());
	
	if (ip == INADDR_NONE) {
		AutoMutex locker(_ipmutex);
		if (_ipcache.find(addr) != _ipcache.end()) {
			ip = _ipcache[addr];
		} else {
			_ipcache[addr] = 0;
			
			auto t = std::thread([addr] {
				u32_t ip = 0;
				hostent* host = ::gethostbyname(addr.c_str());
				if (host && host->h_length)  {
					sockaddr_in addr = { 0 };
					memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
					ip = addr.sin_addr.s_addr;
				}
				if (ip != INADDR_NONE && ip > 0)
				{
					AutoMutex locker(_ipmutex);
					_ipcache[addr] = ip;
				} else {
					AutoMutex locker(_ipmutex);
					_ipcache.erase(addr);
				}
			});
			t.detach();
		}
	}

	if (ip == INADDR_NONE)
		ip = 0;

	return ip;
}

void Util::addr2ips(const sockaddr_in* addr, std::string& ip, u16_t* port)
{
	if (!addr) return;
	ip = ipn2s(addr->sin_addr.s_addr);
	if (port) *port = ntohs(addr->sin_port);
}

void Util::addr2ipn(const sockaddr_in* addr, u32_t* ip, u16_t* port)
{
	if (!addr) return;
	if (ip) *ip = addr->sin_addr.s_addr;
	if (port) *port = ntohs(addr->sin_port);
}

void Util::ips2addr(const std::string& ip, u16_t port, sockaddr_in* addr)
{
	if (!addr) return;
#ifndef _WIN32
	addr->sin_len			= sizeof(sockaddr_in);
#endif
	addr->sin_family      	= AF_INET;
	addr->sin_addr.s_addr 	= ips2n(ip);
	addr->sin_port 			= htons(port);
}

void Util::ipn2addr(u32_t ip, u16_t port, sockaddr_in* addr)
{
	if (!addr) return;
#ifndef _WIN32
	addr->sin_len = sizeof(sockaddr_in);
#endif
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr 	= ip;
	addr->sin_port 			= htons(port);
}

static bool ishexnum(int v)
{
	return ((v >= '0' && v <= '9') ||
			(v >= 'a' && v <= 'z') ||
			(v >= 'A' && v <= 'Z'));
}

static char hex2char(char v) {
	if (v >= '0' && v <= '9')
		return v - '0';
	else if (v >= 'a' && v <= 'z')
		return v - 'a' + 10;
	else if (v >= 'A' && v <= 'Z')
		return v - 'A' + 10;
	else
		return 0;
}

std::string Util::urlenc(const std::string& url)
{
	static char HEX_CHARS[] = "0123456789ABCDEF";
	const char* str = url.c_str();

	u32_t len = (u32_t)::strlen(str);
	if (!len) return "";

	char* buf = new char[len * 5];
	char* psz = buf;
	
	for (u32_t i = 0; i < len; ++i) 
	{
		int v = (int)(str[i]);
		
		if (ishexnum(v) || strchr("-_.!~*'()", v)) {
			*psz++ = (char)v;
		} else {
			*psz++ = '%';
			*psz++ = HEX_CHARS[(v >> 4) & 15];
			*psz++ = HEX_CHARS[v & 15];
		}
	}
	
	*psz = 0;
	
	std::string ret(buf);
	
	delete[] buf;
	
	return ret;
}

std::string Util::urldec(const std::string& url)
{
	const char* str = url.c_str();
	u32_t len = (u32_t)strlen(str);
	if (!len) return "";
	
	char* buf = new char[len * 2];
	char* psz = buf;

	for (u32_t i = 0; i < len; ++i)
	{
		if (str[i] != '%') {
			*psz++ = str[i];
		} else if ((i + 2) < len &&
			ishexnum(str[i + 1]) && ishexnum(str[i + 2])) {
			*psz++ = hex2char(str[i + 1]) * 16 + hex2char(str[i + 2]);
			i += 2;
		} else {
			break;
		}
	}

	*psz = 0;
	
	std::string ret(buf);
	
	delete[] buf;
	
	return ret;
}

void Util::poll()
{
	Timer::poll();
}

u64_t Util::setTimer(u64_t delayMsec, const Timer::Callback& func, i64_t maxLoops)
{
	Timer obj;
	
	obj._interval = delayMsec;
	obj._maxLoops = maxLoops;
	obj._callback = func;
	
	return Timer::add(obj);
}

void Util::delTimer(u64_t tmrId)
{
	if (tmrId > 0)
		Timer::remove(tmrId);
}


#if SOCKLIB_TO_LUA

bool Util::_onTimerCallback(Timer& tmr)
{
	if (tmr._mylua_ref >= 0) {
		lua_State* L = SockLib::luaState();
		lua_rawgeti(L, LUA_REGISTRYINDEX, tmr._mylua_ref);
		
		lua_pushnumber(L, tmr._timerId);
		lua_pushnumber(L, tmr._curLoops);
		lua_pushnumber(L, tmr._maxLoops);

		int result = lua_pcall(L, 3, 1, 0);
		if (0 == result) {
			bool b = lua_isnil(L, -1) || lua_toboolean(L, -1);
			lua_pop(L, 1);
			return b;
		}
	}

	return true;
}


//----------------------------------------------------------------------------
// u32_t opteration, @return a u32_t
//
// 2 args:
// 	socklib.util.u32op("~", a) socklib.util.u32op("!", a)
//
// 3 args:
// 	socklib.util.u32op(a, "&", b) socklib.util.u32op(a, "<<", b)
//
int Util::mylua_u32op(lua_State* L)
{
	int argc = lua_gettop(L);
	
	u32_t a, b, r = 0;
	const char* s;
	
	if (argc == 2) {
		s = luaL_checkstring(L, 1);
		a = (u32_t)luaL_checkinteger(L, 2);
		
		if (0 == strcmp(s, "~")) {
			r = ~a;
		} else if (0 == strcmp(s, "!")) {
			r = !a;
		} else {
			luaL_error(L, "%s.u32op(opt, num) unsupported opt=\"%s\"", SOCKLIB_UTIL.c_str(), s);
		}
	} else if (argc == 3) {
		a = (u32_t)luaL_checkinteger(L, 1);
		s = luaL_checkstring(L, 2);
		b = (u32_t)luaL_checkinteger(L, 3);
		
		if (0 == strcmp(s, "&")) {
			r = a & b;
		} else if (0 == strcmp(s, "|")) {
			r = a | b;
		} else if (0 == strcmp(s, "<<")) {
			r = a << b;
		} else if (0 == strcmp(s, ">>")) {
			r = a >> b;
		} else if (0 == strcmp(s, "^")) {
			r = a ^ b;
		} else if (0 == strcmp(s, "&~") || 0 == strcmp(s, "& ~")) {
			r = (a & (~b));
		} else if (0 == strcmp(s, "|~") || 0 == strcmp(s, "| ~")) {
			r = (a | (~b));
		} else if (0 == strcmp(s, "&&")) {
			lua_pushboolean(L, a && b);
			return 1;
		} else if (0 == strcmp(s, "||")) {
			lua_pushboolean(L, a || b);
			return 1;
// no useful
#if 0
		} else if (0 == strcmp(s, "&!") || 0 == strcmp(s, "& !")) {
			r = (a & (!b));
		} else if (0 == strcmp(s, "&=")) {
			r = (a &= b);
		} else if (0 == strcmp(s, "&=~") || 0 == strcmp(s, "&= ~")) {
			r = (a &= (~b));
		} else if (0 == strcmp(s, "&=!") || 0 == strcmp(s, "&= !")) {
			r = (a &= (!b));
		} else if (0 == strcmp(s, "|!") || 0 == strcmp(s, "| !")) {
			r = (a | (!b));
		} else if (0 == strcmp(s, "|=")) {
			r = (a |= b);
		} else if (0 == strcmp(s, "|=~") || 0 == strcmp(s, "|= ~")) {
			r = (a |= (~b));
		} else if (0 == strcmp(s, "|=!") || 0 == strcmp(s, "|= !")) {
			r = (a |= (!b));
		} else if (0 == strcmp(s, "^=")) {
			r = (a ^= b);
		} else if (0 == strcmp(s, "^=~") || 0 == strcmp(s, "^= ~")) {
			r = (a ^= (~b));
		} else if (0 == strcmp(s, "^=!") || 0 == strcmp(s, "^= !")) {
			r = (a ^= (!b));
#endif
		} else {
			luaL_error(L, "%s.u32op(num1, opt, num2) unsupported opt=\"%s\"", SOCKLIB_UTIL.c_str(), s);
		}
	} else {
		luaL_error(L, "usage: \n%s.u32op(opt, num) or %s.u32op(num1, opt, num2) \ne.g. %s.u32op(\"~\", 123) %s.u32op(123, \"<<\", 2)",
			SOCKLIB_UTIL.c_str(), SOCKLIB_UTIL.c_str(), SOCKLIB_UTIL.c_str(), SOCKLIB_UTIL.c_str());
	}
	
	lua_pushinteger(L, r);
	
	return 1;
}

// for performance
int Util::mylua_u32_inv(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	lua_pushinteger(L, ~a);
	return 1;
}

int Util::mylua_u32_and(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a & b);
	
	lua_pushinteger(L, r);
	return 1;
}

int Util::mylua_u32_or(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a | b);
	
	lua_pushinteger(L, r);
	return 1;
}

int Util::mylua_u32_and_inv(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a & ~b);
	
	lua_pushinteger(L, r);
	return 1;
}

int Util::mylua_u32_or_inv(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a | ~b);
	
	lua_pushinteger(L, r);
	return 1;
}

int Util::mylua_u32_lshift(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a << b);
	
	lua_pushinteger(L, r);
	return 1;
}

int Util::mylua_u32_rshift(lua_State* L)
{
	u32_t a = (u32_t)lua_tointeger(L, 1);
	u32_t b = (u32_t)lua_tointeger(L, 2);
	
	u32_t r = (a >> b);
	
	lua_pushinteger(L, r);
	return 1;
}


int Util::mylua_tick(lua_State* L)
{
	lua_pushnumber(L, Util::tick());
	return 1;
}

int Util::mylua_urlenc(lua_State* L)
{
	const char* url = luaL_checkstring(L, 1);
	std::string ret = Util::urlenc(url);
	lua_pushstring(L, ret.c_str());
	return 1;
}

int Util::mylua_urldec(lua_State* L)
{
	const char* url = luaL_checkstring(L, 1);
	std::string ret = Util::urldec(url);
	lua_pushstring(L, ret.c_str());
	return 1;
}

int Util::mylua_ips2n(lua_State* L)
{
	const char* addr = luaL_checkstring(L, 1);
	lua_pushnumber(L, Util::ips2n(addr));
	return 1;
}

int Util::mylua_ipn2s(lua_State* L)
{
	u32_t ipn = (u32_t)luaL_checknumber(L, 1);
	std::string ret = Util::ipn2s(ipn);
	lua_pushstring(L, ret.c_str());
	return 1;
}

int Util::mylua_ipprobe(lua_State* L)
{
	const char* addr = luaL_checkstring(L, 1);
	lua_pushnumber(L, Util::ipprobe(addr));
	return 1;
}

int Util::mylua_settimer(lua_State* L)
{
	Timer obj;

	obj._callback = _onTimerCallback;
	obj._interval = (u64_t) luaL_optinteger(L, 1, 1000);
	obj._maxLoops = -1;
	obj._mylua_ref = -1;
	
	if (lua_gettop(L) >= 3)
		obj._maxLoops = (i64_t) luaL_optinteger(L, 3, -1);
	
	if (lua_gettop(L) >= 2) {
		lua_pushvalue(L, 2);
		obj._mylua_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	} else {
		luaL_error(L, "Util::mylua_settimer(<bad data>)");
	}
	
	u64_t tmrId= Timer::add(obj);

	lua_pushinteger(L, tmrId);
	
	return 1;
}

int Util::mylua_deltimer(lua_State* L)
{
	u64_t tmrId = (u64_t) luaL_checknumber(L, 1);
	delTimer(tmrId);
	return 0;
}

int Util::mylua_htons(lua_State* L)
{
	u16_t v = (u16_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, htons(v));
	return 1;
}

int Util::mylua_htonl(lua_State* L)
{
	u32_t v = (u32_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, htonl(v));
	return 1;
}

int Util::mylua_htonll(lua_State* L)
{
	u64_t v = (u64_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, htonll(v));
	return 1;
}

int Util::mylua_ntohs(lua_State* L)
{
	u16_t v = (u16_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, ntohs(v));
	return 1;
}

int Util::mylua_ntohl(lua_State* L)
{
	u32_t v = (u32_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, ntohl(v));
	return 1;
}

int Util::mylua_ntohll(lua_State* L)
{
	u64_t v = (u64_t)luaL_checknumber(L, 1);
	lua_pushnumber(L, ntohll(v));
	return 1;
}

#if SOCKLIB_ALG
int Util::mylua_crc32(lua_State* L)
{
	u32_t crc = 0;
	
	if (lua_gettop(L) >= 3)
		crc = (u32_t) luaL_checkinteger(L, 3);

	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 1, ptr, len)) {
		crc = CRC32_build((u8_t*)ptr, len, crc);
	} else {
		luaL_error(L, "%s:crc32(<unknown data>) .", SOCKLIB_UTIL.c_str());
	}
	
	lua_pushinteger(L, crc);

	return 1;
}

int Util::mylua_rc4(lua_State* L)
{
	if (lua_gettop(L) == 0) {
		// @LUA: local obj = socklib.rc4()
		return LuaHelper::create<RC4>(L, SOCKLIB_RC4);
	} else if (lua_gettop(L) == 1) {
		// @LUA: local obj = socklib.rc4(key)
		const char* key = luaL_checkstring(L, 1);
		return LuaHelper::create<RC4>(L, SOCKLIB_RC4, (const u8_t*)key, (u32_t)strlen(key));
	}
	
	// @LUA: local func = socklib.rc4(key, ...)

	const char* key = luaL_checkstring(L, 1);
	u32_t keySize = (u32_t)lua_strlen(L, 1);

	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 2, ptr, len)) {
		u8_t* outDat = (u8_t*)malloc(len);
		RC4_build(key, keySize, ptr, outDat, len);

		SockBuf* buf = new SockBuf();
		buf->w(outDat, len);
		
		free(outDat);
		
		return LuaHelper::bind<SockBuf>(L, SOCKLIB_BUF, buf);
	} else {
		luaL_error(L, "%s:rc4(<unknown data>) .", SOCKLIB_UTIL.c_str());
	}
	
	return 0;
}

int Util::mylua_md5(lua_State* L)
{
	if (lua_gettop(L) == 0) {
		// @LUA: local obj = socklib.md5()
		return LuaHelper::create<MD5>(L, SOCKLIB_MD5);
	}
	
	// @LUA: local func = socklib.md5(...)
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 1, ptr, len)) {
		u8_t hash[16] = { 0 };

		MD5_build(hash, ptr, len);
		
		char hex[128];
		for (int i = 0; i < 16; ++i)
			sprintf(hex + i * 2, "%02x", hash[i]);
		lua_pushstring(L, hex);

		return 1;
	} else {
		luaL_error(L, "%s:md5(<unknown data>) .", SOCKLIB_UTIL.c_str());
	}

	lua_pushstring(L, "");

	return 1;
}

int Util::mylua_sha1(lua_State* L)
{
	if (lua_gettop(L) == 0) {
		// @LUA: local obj = socklib.sha1()
		return LuaHelper::create<SHA1>(L, SOCKLIB_SHA1);
	}
	
	// @LUA: local func = socklib.md5(...)
	
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 1, ptr, len)) {
		u8_t hash[20] = { 0 };

		SHA1_build(hash, ptr, len);
		
		char hex[128];
		for (int i = 0; i < 20; ++i)
			sprintf(hex + i * 2, "%02x", hash[i]);
		lua_pushstring(L, hex);

		return 1;
	} else {
		luaL_error(L, "%s:sha1(<unknown data>) .", SOCKLIB_UTIL.c_str());
	}

	lua_pushstring(L, "");

	return 1;
}

int Util::mylua_b64enc(lua_State* L)
{
	void* ptr = 0;
	u32_t len = 0;
	
	if (mylua_input_get(L, 1, ptr, len)) {
		std::string b64 = Base64_encode(ptr, len);
		lua_pushstring(L, b64.c_str());
		return 1;
	} else {
		luaL_error(L, "%s:b64enc(<unknown data>) .", SOCKLIB_UTIL.c_str());
	}

	lua_pushstring(L, "");

	return 1;
}

int Util::mylua_b64dec(lua_State* L)
{
	std::string ret;

	u32_t len = 0;
	if (lua_gettop(L) >= 2)
		len = (u32_t) luaL_checkinteger(L, 2);
	
	const char* str = luaL_checkstring(L, 1);
	if (!len) len = (u32_t)lua_strlen(L, 1);
	
	ret = Base64_decode(str, len);
	
	lua_pushstring(L, ret.c_str());

	return 1;
}

#endif // SOCKLIB_ALG

#endif // SOCKLIB_TO_LUA

SOCKLIB_NAMESPACE_END
