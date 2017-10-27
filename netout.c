#define LUA_LIB
#include <lua.h>
#include <lauxlib.h>
#include <stdlib.h>
#include "simplesocket.h"
#include <windows.h>

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

static int
string_to_sockaddr(lua_State *L, int addr_idx, int port_idx, union sockaddr_all *sa, socklen_t *retsz) {
	size_t sz;
	const char * buf = luaL_checklstring(L, addr_idx, &sz);
	int port = luaL_checkinteger(L, port_idx);
	int family;
	void *ptr;
	if (memchr(buf, ':', sz)) {
		// ipv6
		family = AF_INET6;
		*retsz = sizeof(sa->v6);
		ptr = &sa->v6.sin6_addr;
		sa->v6.sin6_port = htons(port);
	} else {
		// ipv4
		family = AF_INET;
		*retsz = sizeof(sa->v4);
		ptr = &sa->v4.sin_addr;
		sa->v4.sin_port = htons(port);
	}
	if (inet_pton(family, buf, ptr) != 1) {
		return -1;
	}
	sa->s.sa_family = family;

	return family;
}

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)

struct thread_args {
	HANDLE readpipe;
	SOCKET sock;
};

struct pipe_ud {
	HANDLE oldstdout;
	HANDLE thread;
};

static DWORD WINAPI
redirect_thread(LPVOID lpParam) {
	struct thread_args *ta = (struct thread_args *)lpParam;
	HANDLE rp = ta->readpipe;
	SOCKET fd = ta->sock;
	free(ta);

	char tmp[1024];
	DWORD sz;

	while (ReadFile(rp, tmp, sizeof(tmp), &sz, NULL)) {
		send(fd, tmp, sz, 0);
	}
	CloseHandle(rp);
	closesocket(fd);
	return 0;
}

static int
lclosepipe(lua_State *L) {
	struct pipe_ud *closepipe = (struct pipe_ud *)lua_touserdata(L, 1);
	if (closepipe) {
		HANDLE oso = closepipe->oldstdout;
		if (oso != NULL) {
			// restore stdout
			int fd = _open_osfhandle((intptr_t)closepipe->oldstdout, 0);
			_dup2(fd, STDOUT_FILENO);
			_close(fd);
			SetStdHandle(STD_OUTPUT_HANDLE, closepipe->oldstdout);
			closepipe->oldstdout = NULL;
		}
		WaitForSingleObject(closepipe->thread, INFINITE);
		CloseHandle(closepipe->thread);
	}
	return 0;
}

static void
redirect(lua_State *L, int fd) {
	HANDLE rp, wp;

	BOOL succ = CreatePipe(&rp, &wp, NULL, 0);
	if (!succ) {
		close(fd);
		luaL_error(L, "CreatePipe failed");
	}

	struct thread_args * ta = malloc(sizeof(*ta));
	ta->readpipe = rp;
	ta->sock = fd;
	// thread don't need large stack
	HANDLE thread = CreateThread(NULL, 4096, redirect_thread, (LPVOID)ta, 0, NULL);

	int wpfd = _open_osfhandle((intptr_t)wp, 0);
	if (_dup2(wpfd, STDOUT_FILENO) != 0) {
		close(fd);
		luaL_error(L, "dup2() failed");
	}
	_close(wpfd);
	struct pipe_ud * closepipe = (struct pipe_ud *)lua_newuserdata(L, sizeof(*closepipe));
	closepipe->oldstdout = NULL;
	closepipe->thread = thread;
	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, lclosepipe);
	lua_setfield(L, -2, "__gc");
	lua_setmetatable(L, -2);
	lua_setfield(L, LUA_REGISTRYINDEX, "STDOUT_PIPE");
	closepipe->oldstdout = GetStdHandle(STD_OUTPUT_HANDLE);
	SetStdHandle(STD_OUTPUT_HANDLE, wp);
}


#else

static void
redirect(lua_State *L, SOCKET fd) {
	int r =dup2(fd, STDOUT_FILENO);
	close(fd);
	if (r != 0) {
		luaL_error(L, "dup2() failed");
	}
}

#endif

static int
wait(SOCKET fd, int timeout) {
	struct timeval tv;
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	return select(fd+1, &rfds, NULL, NULL, &tv);
}

static int
linit(lua_State *L) {
	union sockaddr_all sa;
	socklen_t ssz;
	int af = string_to_sockaddr(L, 1,2, &sa, &ssz);
	SOCKET fd = socket(af, SOCK_STREAM, 0);
	if (fd < 0) {
		return luaL_error(L, "Can't create socket");
	}
	int reuse = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int)) == -1) {
		closesocket(fd);
		return luaL_error(L, "Can't set reuse %s %d", lua_tostring(L, 1), lua_tointeger(L, 2));
	}
	if (bind(fd, &sa.s, ssz) == -1) {
		closesocket(fd);
		return luaL_error(L, "Can't bind %s %d", lua_tostring(L, 1), lua_tointeger(L, 2));
	}
	if (listen(fd, 1) == -1) {	// backlog == 1, only accept one connection
		closesocket(fd);
		return luaL_error(L, "Can't listen %s %d", lua_tostring(L, 1), lua_tointeger(L, 2));
	}
	int timeout = luaL_optinteger(L, 3, 0);
	if (timeout > 0) {
		int n = wait(fd, timeout);
		if (n <= 0) {
			closesocket(fd);
			return luaL_error(L, "time out");
		}
	}
	SOCKET sock = accept(fd, NULL, NULL);
	closesocket(fd);
	if (sock == -1) {
		return luaL_error(L, "accept failed %s %d", lua_tostring(L, 1), lua_tointeger(L, 2));
	}

	redirect(L, sock);

	fprintf(stderr, "init %s %d\n", lua_tostring(L, 1), lua_tointeger(L, 2));
	return 0;
}

LUAMOD_API int
luaopen_netout(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "init" , linit },
		{ NULL, NULL },
	};
	simplesocket_init();
	luaL_newlib(L, l);
	return 1;
}
