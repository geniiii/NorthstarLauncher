#include "pch.h"
#include "serverauthentication.h"
#include "convar.h"
#include "hookutils.h"
#include "masterserver.h"
#include "httplib.h"
#include "gameutils.h"
#include <fstream>
#include <filesystem>
#include <thread>

const char* AUTHSERVER_VERIFY_STRING = "I am a northstar server!";

// hook types

typedef void*(*CBaseServer__ConnectClientType)(void* server, void* a2, void* a3, uint32_t a4, uint32_t a5, int32_t a6, void* a7, void* a8, char* serverFilter, void* a10, char a11, void* a12, char a13, char a14, int64_t uid, uint32_t a16, uint32_t a17);
CBaseServer__ConnectClientType CBaseServer__ConnectClient;

typedef char(*CBaseClient__ConnectType)(void* self, char* name, __int64 netchan_ptr_arg, char b_fake_player_arg, __int64 a5, char* Buffer, int a7);
CBaseClient__ConnectType CBaseClient__Connect;

typedef void(*CBaseClient__ActivatePlayerType)(void* self);
CBaseClient__ActivatePlayerType CBaseClient__ActivatePlayer;

typedef void(*CBaseClient__DisconnectType)(void* self, uint32_t unknownButAlways1, const char* reason, ...);
CBaseClient__DisconnectType CBaseClient__Disconnect;

typedef char(*CGameClient__ExecuteStringCommandType)(void* self, uint32_t unknown, const char* pCommandString);
CGameClient__ExecuteStringCommandType CGameClient__ExecuteStringCommand;

typedef char(*__fastcall CNetChan___ProcessMessagesType)(void* self, void* buf);
CNetChan___ProcessMessagesType CNetChan___ProcessMessages;

typedef char(*CBaseClient__SendServerInfoType)(void* self);
CBaseClient__SendServerInfoType CBaseClient__SendServerInfo;

typedef bool(*ProcessConnectionlessPacketType)(void* a1, netpacket_t* packet);
ProcessConnectionlessPacketType ProcessConnectionlessPacket;

// global vars
ServerAuthenticationManager* g_ServerAuthenticationManager;

ConVar* Cvar_ns_player_auth_port;
ConVar* Cvar_ns_erase_auth_info;
ConVar* CVar_ns_auth_allow_insecure;
ConVar* CVar_ns_auth_allow_insecure_write;
ConVar* CVar_sv_quota_stringcmdspersecond;
ConVar* Cvar_net_chan_limit_mode;
ConVar* Cvar_net_chan_limit_msec_per_sec;
ConVar* Cvar_sv_querylimit_per_sec;

void ServerAuthenticationManager::StartPlayerAuthServer()
{
	if (m_runningPlayerAuthThread)
	{
		spdlog::warn("ServerAuthenticationManager::StartPlayerAuthServer was called while m_runningPlayerAuthThread is true");
		return;
	}

	m_runningPlayerAuthThread = true;

	// listen is a blocking call so thread this
	std::thread serverThread([this] {
			// this is just a super basic way to verify that servers have ports open, masterserver will try to read this before ensuring server is legit
			m_playerAuthServer.Get("/verify", [](const httplib::Request& request, httplib::Response& response) {
					response.set_content(AUTHSERVER_VERIFY_STRING, "text/plain");
				});

			m_playerAuthServer.Post("/authenticate_incoming_player", [this](const httplib::Request& request, httplib::Response& response) {
					// can't just do request.remote_addr == Cvar_ns_masterserver_hostname->m_pszString because the cvar can be a url, gotta resolve an ip from it for comparisons
					unsigned long remoteAddr = inet_addr(request.remote_addr.c_str());

					char* addrPtr = Cvar_ns_masterserver_hostname->m_pszString;
					char* typeStart = strstr(addrPtr, "://");
					if (typeStart)
						addrPtr = typeStart + 3;
					hostent* resolvedRemoteAddr = gethostbyname((const char*)addrPtr);

					if (!request.has_param("id") || !request.has_param("authToken") || request.body.size() >= 65335 || !resolvedRemoteAddr || ((in_addr**)resolvedRemoteAddr->h_addr_list)[0]->S_un.S_addr != remoteAddr)
					{
						response.set_content("{\"success\":false}", "application/json");
						return;
					}

					AuthData newAuthData;
					strncpy(newAuthData.uid, request.get_param_value("id").c_str(), sizeof(newAuthData.uid));
					newAuthData.uid[sizeof(newAuthData.uid) - 1] = 0;

					newAuthData.pdataSize = request.body.size();
					newAuthData.pdata = new char[newAuthData.pdataSize];
					memcpy(newAuthData.pdata, request.body.c_str(), newAuthData.pdataSize);

					std::lock_guard<std::mutex> guard(m_authDataMutex);
					m_authData.insert(std::make_pair(request.get_param_value("authToken"), newAuthData));

					response.set_content("{\"success\":true}", "application/json");
				});

			m_playerAuthServer.listen("0.0.0.0", Cvar_ns_player_auth_port->m_nValue);
		});
	
	serverThread.detach();
}

void ServerAuthenticationManager::StopPlayerAuthServer()
{
	if (!m_runningPlayerAuthThread)
	{
		spdlog::warn("ServerAuthenticationManager::StopPlayerAuthServer was called while m_runningPlayerAuthThread is false");
		return;
	}

	m_runningPlayerAuthThread = false;
	m_playerAuthServer.stop();
}

bool ServerAuthenticationManager::AuthenticatePlayer(void* player, int64_t uid, char* authToken)
{
	std::string strUid = std::to_string(uid);

	std::lock_guard<std::mutex> guard(m_authDataMutex);

	bool authFail = true;
	if (!m_authData.empty() && m_authData.count(std::string(authToken)))
	{
		// use stored auth data
		AuthData authData = m_authData[authToken];
		if (!strcmp(strUid.c_str(), authData.uid)) // connecting client's uid is the same as auth's uid
		{
			authFail = false;
			// uuid
			strcpy((char*)player + 0xF500, strUid.c_str());

			// copy pdata into buffer
			memcpy((char*)player + 0x4FA, authData.pdata, authData.pdataSize);

			// set persistent data as ready, we use 0x4 internally to mark the client as using remote persistence
			*((char*)player + 0x4a0) = (char)0x4;
		}
	}


	if (authFail)
	{
		if (!CVar_ns_auth_allow_insecure->m_nValue) // no auth data and insecure connections aren't allowed, so dc the client
			return false;

		// insecure connections are allowed, try reading from disk
		// uuid
		strcpy((char*)player + 0xF500, strUid.c_str());

		// try reading pdata file for player
		std::string pdataPath = "R2Northstar/playerdata_";
		pdataPath += strUid;
		pdataPath += ".pdata";

		std::fstream pdataStream(pdataPath, std::ios_base::in);
		if (pdataStream.fail()) // file doesn't exist, use placeholder
			pdataStream = std::fstream("R2Northstar/placeholder_playerdata.pdata");
		
		// get file length
		pdataStream.seekg(0, pdataStream.end);
		int length = pdataStream.tellg();
		pdataStream.seekg(0, pdataStream.beg);

		// copy pdata into buffer
		pdataStream.read((char*)player + 0x4FA, length);

		pdataStream.close();

		// set persistent data as ready, we use 0x3 internally to mark the client as using local persistence
		*((char*)player + 0x4a0) = (char)0x3;
	}

	return true; // auth successful, client stays on
}

bool ServerAuthenticationManager::RemovePlayerAuthData(void* player)
{
	if (!Cvar_ns_erase_auth_info->m_nValue)
		return false;

	// hack for special case where we're on a local server, so we erase our own newly created auth data on disconnect
	if (m_bNeedLocalAuthForNewgame && !strcmp((char*)player + 0xF500, g_LocalPlayerUserID))
		return false;

	// we don't have our auth token at this point, so lookup authdata by uid
	for (auto& auth : m_authData)
	{
		if (!strcmp((char*)player + 0xF500, auth.second.uid))
		{
			// pretty sure this is fine, since we don't iterate after the erase
			// i think if we iterated after it'd be undefined behaviour tho
			std::lock_guard<std::mutex> guard(m_authDataMutex);

			delete[] auth.second.pdata;
			m_authData.erase(auth.first);
			return true;
		}
	}

	return false;
}

void ServerAuthenticationManager::WritePersistentData(void* player)
{
	// we use 0x4 internally to mark clients as using remote persistence
	if (*((char*)player + 0x4A0) == (char)0x4)
	{
		g_MasterServerManager->WritePlayerPersistentData((char*)player + 0xF500, (char*)player + 0x4FA, m_additionalPlayerData[player].pdataSize);
	}
	else if (CVar_ns_auth_allow_insecure_write->m_nValue)
	{
		// todo: write pdata to disk here
	}
}


// auth hooks

// store these in vars so we can use them in CBaseClient::Connect
// this is fine because ptrs won't decay by the time we use this, just don't use it outside of cbaseclient::connect
char* nextPlayerToken;
int64_t nextPlayerUid;

void* CBaseServer__ConnectClientHook(void* server, void* a2, void* a3, uint32_t a4, uint32_t a5, int32_t a6, void* a7, void* a8, char* serverFilter, void* a10, char a11, void* a12, char a13, char a14, int64_t uid, uint32_t a16, uint32_t a17)
{
	// auth tokens are sent with serverfilter, can't be accessed from player struct to my knowledge, so have to do this here
	nextPlayerToken = serverFilter;
	nextPlayerUid = uid;

	return CBaseServer__ConnectClient(server, a2, a3, a4, a5, a6, a7, a8, serverFilter, a10, a11, a12, a13, a14, uid, a16, a17);
}

char CBaseClient__ConnectHook(void* self, char* name, __int64 netchan_ptr_arg, char b_fake_player_arg, __int64 a5, char* Buffer, int a7)
{
	// try to auth player, dc if it fails
	// we connect irregardless of auth, because returning bad from this function can fuck client state p bad
	char ret = CBaseClient__Connect(self, name, netchan_ptr_arg, b_fake_player_arg, a5, Buffer, a7);
	if (strlen(name) >= 64) // fix for name overflow bug
		CBaseClient__Disconnect(self, 1, "Invalid name");
	else if (!g_ServerAuthenticationManager->AuthenticatePlayer(self, nextPlayerUid, nextPlayerToken) && g_MasterServerManager->m_bRequireClientAuth)
		CBaseClient__Disconnect(self, 1, "Authentication Failed");

	if (!g_ServerAuthenticationManager->m_additionalPlayerData.count(self))
	{
		AdditionalPlayerData additionalData;
		additionalData.pdataSize = g_ServerAuthenticationManager->m_authData[nextPlayerToken].pdataSize;
		additionalData.usingLocalPdata = *((char*)self + 0x4a0) == (char)0x3;

		g_ServerAuthenticationManager->m_additionalPlayerData.insert(std::make_pair(self, additionalData));
	}

	return ret;
}

void CBaseClient__ActivatePlayerHook(void* self)
{
	// if we're authed, write our persistent data
	// RemovePlayerAuthData returns true if it removed successfully, i.e. on first call only, and we only want to write on >= second call (since this func is called on map loads)
	if (*((char*)self + 0x4A0) >= (char)0x3 && !g_ServerAuthenticationManager->RemovePlayerAuthData(self))
	{
		g_ServerAuthenticationManager->WritePersistentData(self);
		g_MasterServerManager->UpdateServerPlayerCount(g_ServerAuthenticationManager->m_additionalPlayerData.size());
	}

	CBaseClient__ActivatePlayer(self);
}

void CBaseClient__DisconnectHook(void* self, uint32_t unknownButAlways1, const char* reason, ...)
{
	// have to manually format message because can't pass varargs to original func
	char buf[1024];

	va_list va;
	va_start(va, reason);
	vsprintf(buf, reason, va);
	va_end(va);

	// this reason is used while connecting to a local server, hacky, but just ignore it
	if (strcmp(reason, "Connection closing"))
	{
		spdlog::info("Player {} disconnected: \"{}\"", (char*)self + 0x16, buf);

		// dcing, write persistent data
		if (g_ServerAuthenticationManager->m_additionalPlayerData[self].needPersistenceWriteOnLeave)
			g_ServerAuthenticationManager->WritePersistentData(self);
		g_ServerAuthenticationManager->RemovePlayerAuthData(self); // won't do anything 99% of the time, but just in case
	}

	if (g_ServerAuthenticationManager->m_additionalPlayerData.count(self))
	{
		g_ServerAuthenticationManager->m_additionalPlayerData.erase(self);
		g_MasterServerManager->UpdateServerPlayerCount(g_ServerAuthenticationManager->m_additionalPlayerData.size());
	}

	CBaseClient__Disconnect(self, unknownButAlways1, buf);
}

// maybe this should be done outside of auth code, but effort to refactor rn and it sorta fits
char CGameClient__ExecuteStringCommandHook(void* self, uint32_t unknown, const char* pCommandString)
{
	if (CVar_sv_quota_stringcmdspersecond->m_nValue != -1)
	{
		// note: this isn't super perfect, legit clients can trigger it in lobby, mostly good enough tho imo
		// https://github.com/perilouswithadollarsign/cstrike15_src/blob/f82112a2388b841d72cb62ca48ab1846dfcc11c8/engine/sv_client.cpp#L1513
		if (Plat_FloatTime() - g_ServerAuthenticationManager->m_additionalPlayerData[self].lastClientCommandQuotaStart >= 1.0)
		{
			// reset quota
			g_ServerAuthenticationManager->m_additionalPlayerData[self].lastClientCommandQuotaStart = Plat_FloatTime();
			g_ServerAuthenticationManager->m_additionalPlayerData[self].numClientCommandsInQuota = 0;
		}

		g_ServerAuthenticationManager->m_additionalPlayerData[self].numClientCommandsInQuota++;
		if (g_ServerAuthenticationManager->m_additionalPlayerData[self].numClientCommandsInQuota > CVar_sv_quota_stringcmdspersecond->m_nValue)
		{
			// too many stringcmds, dc player
			CBaseClient__Disconnect(self, 1, "Sent too many stringcmd commands");
			return false;
		}
	}

	// todo later, basically just limit to CVar_sv_quota_stringcmdspersecond->m_nValue stringcmds per client per second
	return CGameClient__ExecuteStringCommand(self, unknown, pCommandString);
}

char __fastcall CNetChan___ProcessMessagesHook(void* self, void* buf)
{
	double startTime = Plat_FloatTime();
	char ret = CNetChan___ProcessMessages(self, buf);
	
	// check processing limit
	if (Cvar_net_chan_limit_mode->m_nValue != 0)
	{
		// player that sent the message
		void* sender = *(void**)((char*)self + 368);

		// if no sender, return
		// relatively certain this is fine?
		if (!sender || !g_ServerAuthenticationManager->m_additionalPlayerData.count(sender))
			return ret;

		// reset every second
		if (startTime - g_ServerAuthenticationManager->m_additionalPlayerData[sender].lastNetChanProcessingLimitStart >= 1.0 || g_ServerAuthenticationManager->m_additionalPlayerData[sender].lastNetChanProcessingLimitStart == -1.0)
		{
			g_ServerAuthenticationManager->m_additionalPlayerData[sender].lastNetChanProcessingLimitStart = startTime;
			g_ServerAuthenticationManager->m_additionalPlayerData[sender].netChanProcessingLimitTime = 0.0;
		}

		g_ServerAuthenticationManager->m_additionalPlayerData[sender].netChanProcessingLimitTime += (Plat_FloatTime() * 1000) - (startTime * 1000);

		int32_t limit = Cvar_net_chan_limit_msec_per_sec->m_nValue;
		if (g_pHostState->m_iCurrentState != HostState_t::HS_RUN)
			limit *= 2; // give clients more headroom in these states, as alot of clients will tend to time out here

		if (g_ServerAuthenticationManager->m_additionalPlayerData[sender].netChanProcessingLimitTime >= limit)
		{
			spdlog::warn("Client {} hit netchan processing limit with {}ms of processing time this second (max is {})", (char*)sender + 0x16, g_ServerAuthenticationManager->m_additionalPlayerData[sender].netChanProcessingLimitTime, Cvar_net_chan_limit_msec_per_sec->m_nValue);
			
			// mode 1 = kick, mode 2 = log without kicking
			if (Cvar_net_chan_limit_mode->m_nValue == 1)
			{
				CBaseClient__Disconnect(sender, 1, "Exceeded net channel processing limit");
				return false;
			}
		}
	}

	return ret;
}

bool bWasWritingStringTableSuccessful;

void CBaseClient__SendServerInfoHook(void* self)
{
	bWasWritingStringTableSuccessful = true;
	CBaseClient__SendServerInfo(self);
	if (!bWasWritingStringTableSuccessful)
		CBaseClient__Disconnect(self, 1, "Overflowed CNetworkStringTableContainer::WriteBaselines, try restarting your client and reconnecting");
}

bool ProcessConnectionlessPacketHook(void* a1, netpacket_t* packet)
{
	if (packet->adr.type == NA_IP)
	{
		// bad lookup: optimise later tm
		UnconnectedPlayerSendData* sendData = nullptr;
		for (UnconnectedPlayerSendData& foundSendData : g_ServerAuthenticationManager->m_unconnectedPlayerSendData)
		{
			if (!memcmp(packet->adr.ip, foundSendData.ip, 16))
			{
				sendData = &foundSendData;
				break;
			}
		}

		if (!sendData)
		{
			sendData = &g_ServerAuthenticationManager->m_unconnectedPlayerSendData.emplace_back();
			memcpy(sendData->ip, packet->adr.ip, 16);
		}

		if (Plat_FloatTime() < sendData->timeoutEnd)
			return false;

		if (Plat_FloatTime() - sendData->lastQuotaStart >= 1.0)
		{
			sendData->lastQuotaStart = Plat_FloatTime();
			sendData->packetCount = 0;
		}

		sendData->packetCount++;

		if (sendData->packetCount >= Cvar_sv_querylimit_per_sec->m_nValue)
		{
			spdlog::warn("Client went over connectionless ratelimit of {} per sec with packet of type {}", Cvar_sv_querylimit_per_sec->m_nValue, packet->data[4]);

			// timeout for a minute
			sendData->timeoutEnd = Plat_FloatTime() + 60.0;
			return false;
		}
	}

	return ProcessConnectionlessPacket(a1, packet);
}

void InitialiseServerAuthentication(HMODULE baseAddress)
{
	g_ServerAuthenticationManager = new ServerAuthenticationManager;

	Cvar_ns_erase_auth_info = RegisterConVar("ns_erase_auth_info", "1", FCVAR_GAMEDLL, "Whether auth info should be erased from this server on disconnect or crash");
	CVar_ns_auth_allow_insecure = RegisterConVar("ns_auth_allow_insecure", "0", FCVAR_GAMEDLL, "Whether this server will allow unauthenicated players to connect");
	CVar_ns_auth_allow_insecure_write = RegisterConVar("ns_auth_allow_insecure_write", "0", FCVAR_GAMEDLL, "Whether the pdata of unauthenticated clients will be written to disk when changed");
	// literally just stolen from a fix valve used in csgo
	CVar_sv_quota_stringcmdspersecond = RegisterConVar("sv_quota_stringcmdspersecond", "60", FCVAR_GAMEDLL, "How many string commands per second clients are allowed to submit, 0 to disallow all string commands");
	// https://blog.counter-strike.net/index.php/2019/07/24922/ but different because idk how to check what current tick number is
	Cvar_net_chan_limit_mode = RegisterConVar("net_chan_limit_mode", "0", FCVAR_GAMEDLL, "The mode for netchan processing limits: 0 = none, 1 = kick, 2 = log");
	Cvar_net_chan_limit_msec_per_sec = RegisterConVar("net_chan_limit_msec_per_sec", "0", FCVAR_GAMEDLL, "Netchannel processing is limited to so many milliseconds, abort connection if exceeding budget");
	Cvar_ns_player_auth_port = RegisterConVar("ns_player_auth_port", "8081", FCVAR_GAMEDLL, "");
	Cvar_sv_querylimit_per_sec = RegisterConVar("sv_querylimit_per_sec", "15", FCVAR_GAMEDLL, "");

	HookEnabler hook;
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x114430, &CBaseServer__ConnectClientHook, reinterpret_cast<LPVOID*>(&CBaseServer__ConnectClient));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x101740, &CBaseClient__ConnectHook, reinterpret_cast<LPVOID*>(&CBaseClient__Connect));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x100F80, &CBaseClient__ActivatePlayerHook, reinterpret_cast<LPVOID*>(&CBaseClient__ActivatePlayer));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x1012C0, &CBaseClient__DisconnectHook, reinterpret_cast<LPVOID*>(&CBaseClient__Disconnect));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x1022E0, &CGameClient__ExecuteStringCommandHook, reinterpret_cast<LPVOID*>(&CGameClient__ExecuteStringCommand));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x2140A0, &CNetChan___ProcessMessagesHook, reinterpret_cast<LPVOID*>(&CNetChan___ProcessMessages));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x104FB0, &CBaseClient__SendServerInfoHook, reinterpret_cast<LPVOID*>(&CBaseClient__SendServerInfo));
	ENABLER_CREATEHOOK(hook, (char*)baseAddress + 0x117800, &ProcessConnectionlessPacketHook, reinterpret_cast<LPVOID*>(&ProcessConnectionlessPacket));

	// patch to disable kicking based on incorrect serverfilter in connectclient, since we repurpose it for use as an auth token
	{
		void* ptr = (char*)baseAddress + 0x114655;
		TempReadWrite rw(ptr);
		*((char*)ptr) = (char)0xEB; // jz => jmp
	}

	// patch to disable fairfight marking players as cheaters and kicking them
	{
		void* ptr = (char*)baseAddress + 0x101012;
		TempReadWrite rw(ptr);
		*((char*)ptr) = (char)0xE9; // jz => jmp
		*((char*)ptr + 1) = (char)0x90;
		*((char*)ptr + 2) = (char)0x0;
	}

	// patch to allow same of multiple account
	{
		void* ptr = (char*)baseAddress + 0x114510;
		TempReadWrite rw(ptr);
		*((char*)ptr) = (char)0xEB; // jz => jmp
	}

	// patch to set bWasWritingStringTableSuccessful in CNetworkStringTableContainer::WriteBaselines if it fails
	{
		bool* writeAddress = (bool*)(&bWasWritingStringTableSuccessful - ((bool*)baseAddress + 0x234EDC));

		void* ptr = (char*)baseAddress + 0x234ED2;
		TempReadWrite rw(ptr);
		*((char*)ptr) = (char)0xC7;
		*((char*)ptr + 1) = (char)0x05;
		*(int*)((char*)ptr + 2) = (int)writeAddress;
		*((char*)ptr + 6) = (char)0x00;
		*((char*)ptr + 7) = (char)0x00;
		*((char*)ptr + 8) = (char)0x00;
		*((char*)ptr + 9) = (char)0x00;

		*((char*)ptr + 10) = (char)0x90;
		*((char*)ptr + 11) = (char)0x90;
		*((char*)ptr + 12) = (char)0x90;
		*((char*)ptr + 13) = (char)0x90;
		*((char*)ptr + 14) = (char)0x90;
	}
}