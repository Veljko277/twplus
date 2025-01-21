/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <engine/shared/config.h>
#include <game/mapitems.h>

#include <game/generated/protocol.h>

#include "entities/pickup.h"
#include "entities/structure.h"
#include "gamecontroller.h"
#include "gamecontext.h"

#include <stdio.h>
#include <time.h>
#include <string>
#include <game/collision.h>

IGameController::IGameController(class CGameContext *pGameServer)
{
	m_pGameServer = pGameServer;
	m_pServer = m_pGameServer->Server();
	m_pGameType = "unknown";

	//
	DoWarmup(g_Config.m_SvWarmup);
	m_GameOverTick = -1;
	m_SuddenDeath = 0;
	m_RoundStartTick = Server()->Tick();
	m_RoundCount = 0;
	m_GameFlags = 0;
	m_aTeamscore[TEAM_RED] = 0;
	m_aTeamscore[TEAM_BLUE] = 0;
	m_aMapWish[0] = 0;

	m_UnbalancedTick = -1;
	m_ForceBalanced = false;

	m_aNumSpawnPoints[0] = 0;
	m_aNumSpawnPoints[1] = 0;
	m_aNumSpawnPoints[2] = 0;

	m_FakeWarmup = 0;
}

IGameController::~IGameController()
{
}

float IGameController::EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos)
{
	float Score = 0.0f;
	CCharacter *pC = static_cast<CCharacter *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_CHARACTER));
	for(; pC; pC = (CCharacter *)pC->TypeNext())
	{
		// team mates are not as dangerous as enemies
		float Scoremod = 1.0f;
		if(pEval->m_FriendlyTeam != -1 && pC->GetPlayer()->GetTeam() == pEval->m_FriendlyTeam)
			Scoremod = 0.5f;

		float d = distance(Pos, pC->m_Pos);
		Score += Scoremod * (d == 0 ? 1000000000.0f : 1.0f/d);
	}

	return Score;
}

void IGameController::EvaluateSpawnType(CSpawnEval *pEval, int Type)
{
	// get spawn point
	for(int i = 0; i < m_aNumSpawnPoints[Type]; i++)
	{
		// check if the position is occupado
		CCharacter *aEnts[MAX_CLIENTS];
		int Num = GameServer()->m_World.FindEntities(m_aaSpawnPoints[Type][i], 64, (CEntity**)aEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		vec2 Positions[5] = { vec2(0.0f, 0.0f), vec2(-32.0f, 0.0f), vec2(0.0f, -32.0f), vec2(32.0f, 0.0f), vec2(0.0f, 32.0f) };	// start, left, up, right, down
		int Result = -1;
		for(int Index = 0; Index < 5 && Result == -1; ++Index)
		{
			Result = Index;
			for(int c = 0; c < Num; ++c)
				if(GameServer()->Collision()->CheckPoint(m_aaSpawnPoints[Type][i]+Positions[Index]) ||
					distance(aEnts[c]->m_Pos, m_aaSpawnPoints[Type][i]+Positions[Index]) <= aEnts[c]->m_ProximityRadius)
				{
					Result = -1;
					break;
				}
		}
		if(Result == -1)
			continue;	// try next spawn point

		vec2 P = m_aaSpawnPoints[Type][i]+Positions[Result];
		float S = EvaluateSpawnPos(pEval, P);
		if(!pEval->m_Got || pEval->m_Score > S)
		{
			pEval->m_Got = true;
			pEval->m_Score = S;
			pEval->m_Pos = P;
		}
	}
}

bool IGameController::CanSpawn(int Team, vec2 *pOutPos)
{
	CSpawnEval Eval;

	// spectators can't spawn
	if(Team == TEAM_SPECTATORS)
		return false;

	if(IsTeamplay())
	{
		Eval.m_FriendlyTeam = Team;

		// first try own team spawn, then normal spawn and then enemy
		EvaluateSpawnType(&Eval, 1+(Team&1));
		if(!Eval.m_Got)
		{
			EvaluateSpawnType(&Eval, 0);
			if(!Eval.m_Got)
				EvaluateSpawnType(&Eval, 1+((Team+1)&1));
		}
	}
	else
	{
		EvaluateSpawnType(&Eval, 0);
		EvaluateSpawnType(&Eval, 1);
		EvaluateSpawnType(&Eval, 2);
	}

	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}


bool IGameController::OnEntity(int Index, vec2 Pos)
{
	int Type = -1;
	int SubType = 0;

	if(Index == ENTITY_SPAWN)
		m_aaSpawnPoints[0][m_aNumSpawnPoints[0]++] = Pos;
	else if(Index == ENTITY_SPAWN_RED)
		m_aaSpawnPoints[1][m_aNumSpawnPoints[1]++] = Pos;
	else if(Index == ENTITY_SPAWN_BLUE)
		m_aaSpawnPoints[2][m_aNumSpawnPoints[2]++] = Pos;

	if(!IsInstagib())
	{
		if(Index == ENTITY_ARMOR_1)
			Type = POWERUP_ARMOR;
		else if(Index == ENTITY_HEALTH_1)
			Type = POWERUP_HEALTH;
		else if(Index == ENTITY_ARMOR_5)
			{Type = POWERUP_ARMOR; SubType = 1;}
		else if(Index == ENTITY_HEALTH_5)
			{Type = POWERUP_HEALTH; SubType = 1;}
		if (!IsNoPickups()) 
		{
			if(Index == ENTITY_WEAPON_SHOTGUN)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_SHOTGUN;
			}
			else if(Index == ENTITY_WEAPON_GRENADE)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_GRENADE;
			}
			else if(Index == ENTITY_WEAPON_RIFLE)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_RIFLE;
			}
			else if(Index == ENTITY_POWERUP_NINJA && g_Config.m_SvPowerups)
			{
				Type = POWERUP_NINJA;
				SubType = WEAPON_NINJA;
			}
			else if(Index == ENTITY_WEAPON_PLASMAGUN)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_PLASMAGUN;
			}
			else if(Index == ENTITY_POWERUP_HAMMER)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_HAMMER_SUPER;
			}
			else if(Index == ENTITY_POWERUP_GUN)
			{
				Type = POWERUP_WEAPON;
				SubType = WEAPON_GUN_SUPER;
			}
		}	
	}
	if(Index == ENTITY_GRENADE_FOUNTAIN)
	{
		int d = -1;
		if (GameServer()->Collision()->CheckPoint(Pos.x, Pos.y - 32))
			d = 1;

		CGameWorld *g = &GameServer()->m_World;
		CStructure *pProj = new CStructure(g, WEAPON_GRENADE,
				-1,
				Pos,
				{0.0f,(float)d});
	}
	else if(Index == ENTITY_GRENADE_FOUNTAIN_HORIZONTAL)
	{
		int d = -1;
		if (GameServer()->Collision()->CheckPoint(Pos.x - 32, Pos.y))
			d = 1;

		CGameWorld *g = &GameServer()->m_World;
		CStructure *pProj = new CStructure(g, WEAPON_GRENADE,
				-1,
				Pos,
				{(float)d,0.0f});
	}

	if(Type != -1)
	{
		CPickup *pPickup = new CPickup(&GameServer()->m_World, Type, SubType);
		pPickup->m_Pos = Pos;
		return true;
	}

	return false;
}

void IGameController::EndRound()
{
	if(m_Warmup) // game can't end when we are running warmup
		return;

	GameServer()->m_World.m_Paused = true;
	m_GameOverTick = Server()->Tick();
	m_SuddenDeath = 0;
	GameServer()->m_SpecMuted = false;
	SaveStats();

	// added to determine if a spectator should stay spectator later
	for(int i = 0; i < MAX_CLIENTS; i++) {
		std::string name = Server()->ClientName(i);
		Server()->m_playerNames[i] = name;
		//GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", Server()->ClientName(i));
		
		if (name.compare("(invalid)") != 0 && name.compare("(connecting)") != 0) {
			if (GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS) {
				std::string name = "(invalid)";
				Server()->m_playerNames[i] = name;
			}
		}
	}

	// Add stats system message
	if (g_Config.m_SvRoundendMessage) {
		if(IsTeamplay())
		{
			char aBuf[1024] = "No message (this should not appear)";
			if (IsLMS()) {
				int scoreRed = 0;
				int scoreBlue = 0;
				for (int i = 0; i < MAX_CLIENTS; i++)
				{
					if (GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_IsReady && GameServer()->IsClientPlayer(i)) {
						if (GameServer()->m_apPlayers[i]->GetTeam() == TEAM_BLUE && GameServer()->m_apPlayers[i]->m_Lives > 0)
							scoreBlue++;
						else if (GameServer()->m_apPlayers[i]->GetTeam() == TEAM_RED && GameServer()->m_apPlayers[i]->m_Lives > 0)
							scoreRed++;
					}
				}
				if (scoreRed > scoreBlue)
					str_format(aBuf, sizeof(aBuf), "★ Red team has won the round! Alive, Red: %d, Blue: %d", scoreRed, scoreBlue);
				else if (scoreRed < scoreBlue)
					str_format(aBuf, sizeof(aBuf), "★ Blue team has won the round! Alive, Red: %d, Blue: %d", scoreRed, scoreBlue);
				else
					str_format(aBuf, sizeof(aBuf), "★ Draw! Alive, Red: %d, Blue: %d", scoreRed, scoreBlue);
			} else {
				int scoreRed = m_aTeamscore[TEAM_RED];
				int scoreBlue = m_aTeamscore[TEAM_BLUE];
				if (scoreRed > scoreBlue)
					str_format(aBuf, sizeof(aBuf), "★ Red team has won the round! Red: %d, Blue: %d", scoreRed, scoreBlue);
				else if (scoreRed < scoreBlue)
					str_format(aBuf, sizeof(aBuf), "★ Blue team has won the round! Red: %d, Blue: %d", scoreRed, scoreBlue);
				else
					str_format(aBuf, sizeof(aBuf), "★ Draw! Red: %d, Blue: %d", scoreRed, scoreBlue);
			}
			m_pGameServer->SendChat(-1, CGameContext::CHAT_ALL, aBuf);

			char bBuf[1024] = "No message2 (this should not appear)";
			char cBuf[1024] = "No message3 (this should not appear)";
			int mostKill = -1;
			int mostFlags = -1;
			for(int i = 0; i < MAX_CLIENTS; i++) {
				if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
					continue;
				CPlayer* pP = GameServer()->m_apPlayers[i];

				if (pP->m_Stats.m_Kills > mostKill) {
					mostKill = pP->m_Stats.m_Kills;
					str_format(bBuf, sizeof(bBuf), "★ '%s' has most kills with %d kills!", Server()->ClientName(i), pP->m_Stats.m_Kills);
				}
				if (pP->m_Stats.m_Captures > mostFlags) {
					mostFlags = pP->m_Stats.m_Captures;
					str_format(cBuf, sizeof(cBuf), "★ '%s' has captured the most flags with %d flags!", Server()->ClientName(i), pP->m_Stats.m_Captures);
				}
			}
			if (mostFlags > 0)
				m_pGameServer->SendChat(-1, CGameContext::CHAT_ALL, cBuf);
			else
				m_pGameServer->SendChat(-1, CGameContext::CHAT_ALL, bBuf);
		} else { // Non-team gamemode
			char aBuf[1024] = "No message (this should not appear)";
			if (GameServer()->m_pController->IsLMS())
				str_format(aBuf, sizeof(aBuf), "★ Draw! There are no players alive");
			char bBuf[1024] = "No message2 (this should not appear)";
			int highestScore = -99;
			float bestRatio = -1;
			for(int i = 0; i < MAX_CLIENTS; i++) {
				if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
					continue;
				CPlayer* pP = GameServer()->m_apPlayers[i];

				if (pP->m_Score > highestScore && !GameServer()->m_pController->IsLMS()) {
					highestScore = pP->m_Score;
					str_format(aBuf, sizeof(aBuf), "★ '%s' has won the round with a score of %d!", Server()->ClientName(i), pP->m_Score);
				} else if (pP->m_Score == highestScore && !GameServer()->m_pController->IsLMS()) {
					highestScore = pP->m_Score;
					str_format(aBuf, sizeof(aBuf), "★ Draw! Multiple players have a score of %d!", pP->m_Score);
				}
				if (highestScore < 0 && pP->m_Lives > 0 && GameServer()->m_pController->IsLMS()) {
					highestScore = 0;
					str_format(aBuf, sizeof(aBuf), "★ '%s' has won the round with %d lives left!", Server()->ClientName(i), pP->m_Lives);
				} else if (highestScore == 0 && pP->m_Lives > 0 && GameServer()->m_pController->IsLMS()) {
					str_format(aBuf, sizeof(aBuf), "★ Draw! There are multiple players alive");
				}
				if (((pP->m_Stats.m_Deaths > 0) ? ((float)pP->m_Stats.m_Kills / (float)pP->m_Stats.m_Deaths) : 99999) > bestRatio) {
					bestRatio = ((pP->m_Stats.m_Deaths > 0) ? ((float)pP->m_Stats.m_Kills / (float)pP->m_Stats.m_Deaths) : 99999);
					str_format(bBuf, sizeof(bBuf), "★ '%s' has the best kill/death ratio with %d kills and %d deaths!", Server()->ClientName(i), pP->m_Stats.m_Kills, pP->m_Stats.m_Deaths);
				}
			}
			m_pGameServer->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
			m_pGameServer->SendChat(-1, CGameContext::CHAT_ALL, bBuf);
		}
	}
}

void IGameController::ResetGame()
{
	GameServer()->m_World.m_ResetRequested = true;
}

const char *IGameController::GetTeamName(int Team)
{
	if(IsTeamplay())
	{
		if(Team == TEAM_RED)
			return "red team";
		else if(Team == TEAM_BLUE)
			return "blue team";
	}
	else
	{
		if(Team == 0)
			return "game";
	}

	return "spectators";
}

static bool IsSeparator(char c) { return c == ';' || c == ' ' || c == ',' || c == '\t'; }

void IGameController::StartRound()
{
	ResetGame();

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(GameServer()->m_apPlayers[i])
		{
			mem_zero(&GameServer()->m_apPlayers[i]->m_Stats, sizeof(GameServer()->m_apPlayers[i]->m_Stats));
			GameServer()->m_apPlayers[i]->m_GotAward = false;
			GameServer()->m_apPlayers[i]->m_Spree = 0;
			GameServer()->m_apPlayers[i]->m_Lives = g_Config.m_SvLMSLives;
		}

	m_RoundStartTick = Server()->Tick();
	m_SuddenDeath = 0;
	m_GameOverTick = -1;
	GameServer()->m_World.m_Paused = false;
	m_aTeamscore[TEAM_RED] = 0;
	m_aTeamscore[TEAM_BLUE] = 0;
	m_ForceBalanced = false;
	Server()->DemoRecorder_HandleAutoStart();
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "start round type='%s' teamplay='%d'", m_pGameType, m_GameFlags&GAMEFLAG_TEAMS);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
}

void IGameController::ChangeMap(const char *pToMap)
{
	str_copy(m_aMapWish, pToMap, sizeof(m_aMapWish));
	EndRound();
}

void IGameController::CycleMap()
{
	if(m_aMapWish[0] != 0)
	{
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "rotating map to %s", m_aMapWish);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
		str_copy(g_Config.m_SvMap, m_aMapWish, sizeof(g_Config.m_SvMap));
		m_aMapWish[0] = 0;
		m_RoundCount = 0;
		return;
	}
	if(!str_length(g_Config.m_SvMaprotation))
		return;

	if(m_RoundCount < g_Config.m_SvRoundsPerMap-1)
	{
		if(g_Config.m_SvRoundSwap)
			GameServer()->SwapTeams();
		return;
	}

	// handle maprotation
	const char *pMapRotation = g_Config.m_SvMaprotation;
	const char *pCurrentMap = g_Config.m_SvMap;

	int CurrentMapLen = str_length(pCurrentMap);
	const char *pNextMap = pMapRotation;
	while(*pNextMap)
	{
		int WordLen = 0;
		while(pNextMap[WordLen] && !IsSeparator(pNextMap[WordLen]))
			WordLen++;

		if(WordLen == CurrentMapLen && str_comp_num(pNextMap, pCurrentMap, CurrentMapLen) == 0)
		{
			// map found
			pNextMap += CurrentMapLen;
			while(*pNextMap && IsSeparator(*pNextMap))
				pNextMap++;

			break;
		}

		pNextMap++;
	}

	// restart rotation
	if(pNextMap[0] == 0)
		pNextMap = pMapRotation;

	// cut out the next map
	char aBuf[512] = {0};
	for(int i = 0; i < 511; i++)
	{
		aBuf[i] = pNextMap[i];
		if(IsSeparator(pNextMap[i]) || pNextMap[i] == 0)
		{
			aBuf[i] = 0;
			break;
		}
	}

	// skip spaces
	int i = 0;
	while(IsSeparator(aBuf[i]))
		i++;

	m_RoundCount = 0;

	char aBufMsg[256];
	str_format(aBufMsg, sizeof(aBufMsg), "rotating map to %s", &aBuf[i]);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	str_copy(g_Config.m_SvMap, &aBuf[i], sizeof(g_Config.m_SvMap));
}

void IGameController::PostReset()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GameServer()->m_apPlayers[i])
		{
			GameServer()->m_apPlayers[i]->Respawn();
			GameServer()->m_apPlayers[i]->m_Score = 0;
			GameServer()->m_apPlayers[i]->m_ScoreTick = 0;
			GameServer()->m_apPlayers[i]->m_ScoreStartTick = Server()->Tick();
			GameServer()->m_apPlayers[i]->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;
		}
	}
}

void IGameController::OnPlayerInfoChange(class CPlayer *pP)
{
	const int aTeamColors[2] = {65387, 10223467};
	if(IsTeamplay())
	{
		pP->m_TeeInfos.m_UseCustomColor = 1;
		if(pP->GetTeam() >= TEAM_RED && pP->GetTeam() <= TEAM_BLUE)
		{
			pP->m_TeeInfos.m_ColorBody = aTeamColors[pP->GetTeam()];
			pP->m_TeeInfos.m_ColorFeet = aTeamColors[pP->GetTeam()];
		}
		else
		{
			pP->m_TeeInfos.m_ColorBody = 12895054;
			pP->m_TeeInfos.m_ColorFeet = 12895054;
		}
	}
}


int IGameController::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	// do scoreing
	if(!pKiller || Weapon == WEAPON_GAME)
		return 0;
	if(pKiller == pVictim->GetPlayer())
	{
		pVictim->GetPlayer()->m_Score--; // suicide
		if(g_Config.m_SvLoltextShow)
			GameServer()->CreateLolText(pKiller->GetCharacter(), "-1");
	}
	else
	{
		if(IsTeamplay() && pVictim->GetPlayer()->GetTeam() == pKiller->GetTeam())
		{
			pKiller->m_Score--; // teamkill
			if(g_Config.m_SvLoltextShow)
				GameServer()->CreateLolText(pKiller->GetCharacter(), "-1");
		}
		else
		{
			pKiller->m_Score++; // normal kill
			if(g_Config.m_SvLoltextShow)
				GameServer()->CreateLolText(pKiller->GetCharacter(), "+1");
		}
	}
	if(Weapon == WEAPON_SELF)
		pVictim->GetPlayer()->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()*3.0f;
	return 0;
}

void IGameController::OnCharacterSpawn(class CCharacter *pChr)
{
	// default health
	pChr->IncreaseHealth(10);

	if (IsInstagib() && IsGrenade())
		pChr->GiveWeapon(WEAPON_GRENADE, g_Config.m_SvGrenadeAmmo);
	else if(IsInstagib())
		pChr->GiveWeapon(WEAPON_RIFLE, -1);
	else
	{
		// give default weapons
		pChr->GiveWeapon(WEAPON_HAMMER, -1);
		pChr->GiveWeapon(WEAPON_GUN, 10);
	}
}

void IGameController::DoWarmup(int Seconds)
{
	if(Seconds < 0)
		m_Warmup = 0;
	else
		m_Warmup = Seconds*Server()->TickSpeed();
}

bool IGameController::IsFriendlyFire(int ClientID1, int ClientID2)
{
	if(ClientID1 == ClientID2)
		return false;

	if(IsTeamplay())
	{
		if(!GameServer()->m_apPlayers[ClientID1] || !GameServer()->m_apPlayers[ClientID2])
			return false;

		if(GameServer()->m_apPlayers[ClientID1]->GetTeam() == GameServer()->m_apPlayers[ClientID2]->GetTeam())
			return true;
	}

	return false;
}

bool IGameController::IsForceBalanced()
{
	if(m_ForceBalanced)
	{
		m_ForceBalanced = false;
		return true;
	}
	else
		return false;
}

bool IGameController::CanBeMovedOnBalance(int ClientID)
{
	return true;
}

void IGameController::Tick()
{
	// do warmup
	if(m_Warmup)
	{
		m_Warmup--;
		if(!m_Warmup)
			StartRound();
	}

	if(m_GameOverTick != -1)
	{
		// game over.. wait for restart
		if(Server()->Tick() > m_GameOverTick+Server()->TickSpeed()*10)
		{
			CycleMap();
			StartRound();
			m_RoundCount++;
		}
	}

	if(m_FakeWarmup)
	{
		m_FakeWarmup--;
		if(!m_FakeWarmup && GameServer()->m_World.m_Paused)
		{
			GameServer()->m_World.m_Paused = false;
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, "Game started");
		}
	}

	// Grenade should have everytime more than 3 bullets
	if(g_Config.m_SvGrenadeAmmo < 4)
		g_Config.m_SvGrenadeAmmo = -1;

	// game is Paused
	if(GameServer()->m_World.m_Paused)
		++m_RoundStartTick;

	// do team-balancing
	if(IsTeamplay() && m_UnbalancedTick != -1 && Server()->Tick() > m_UnbalancedTick+g_Config.m_SvTeambalanceTime*Server()->TickSpeed()*60)
	{
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", "Balancing teams");

		int aT[2] = {0,0};
		float aTScore[2] = {0,0};
		float aPScore[MAX_CLIENTS] = {0.0f};
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS)
			{
				aT[GameServer()->m_apPlayers[i]->GetTeam()]++;
				aPScore[i] = GameServer()->m_apPlayers[i]->m_Score*Server()->TickSpeed()*60.0f/
					(Server()->Tick()-GameServer()->m_apPlayers[i]->m_ScoreStartTick);
				aTScore[GameServer()->m_apPlayers[i]->GetTeam()] += aPScore[i];
			}
		}

		// are teams unbalanced?
		if(absolute(aT[0]-aT[1]) >= 2)
		{
			int M = (aT[0] > aT[1]) ? 0 : 1;
			int NumBalance = absolute(aT[0]-aT[1]) / 2;

			do
			{
				CPlayer *pP = 0;
				float PD = aTScore[M];
				for(int i = 0; i < MAX_CLIENTS; i++)
				{
					if(!GameServer()->m_apPlayers[i] || !CanBeMovedOnBalance(i))
						continue;
					// remember the player who would cause lowest score-difference
					if(GameServer()->m_apPlayers[i]->GetTeam() == M && (!pP || absolute((aTScore[M^1]+aPScore[i]) - (aTScore[M]-aPScore[i])) < PD))
					{
						pP = GameServer()->m_apPlayers[i];
						PD = absolute((aTScore[M^1]+aPScore[i]) - (aTScore[M]-aPScore[i]));
					}
				}

				// move the player to the other team
				int Temp = pP->m_LastActionTick;
				pP->SetTeam(M^1);
				pP->m_LastActionTick = Temp;

				pP->Respawn();
				pP->m_ForceBalanced = true;
			} while (--NumBalance);

			m_ForceBalanced = true;
		}
		m_UnbalancedTick = -1;
	}

	// check for inactive players
	if(g_Config.m_SvInactiveKickTime > 0)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
		#ifdef CONF_DEBUG
			if(g_Config.m_DbgDummies)
			{
				if(i >= MAX_CLIENTS-g_Config.m_DbgDummies)
					break;
			}
		#endif
			if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS && !Server()->IsAuthed(i))
			{
				if(Server()->Tick() > GameServer()->m_apPlayers[i]->m_LastActionTick+g_Config.m_SvInactiveKickTime*Server()->TickSpeed()*60 && !GameServer()->m_apPlayers[i]->m_isBot)
				{
					switch(g_Config.m_SvInactiveKick)
					{
					case 0:
						{
							// move player to spectator
							GameServer()->m_apPlayers[i]->SetTeam(TEAM_SPECTATORS);
						}
						break;
					case 1:
						{
							// move player to spectator if the reserved slots aren't filled yet, kick him otherwise
							int Spectators = 0;
							for(int j = 0; j < MAX_CLIENTS; ++j)
								if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() == TEAM_SPECTATORS)
									++Spectators;
							if(Spectators >= g_Config.m_SvSpectatorSlots)
								Server()->Kick(i, "Kicked for inactivity");
							else
								GameServer()->m_apPlayers[i]->SetTeam(TEAM_SPECTATORS);
						}
						break;
					case 2:
						{
							// kick the player
							Server()->Kick(i, "Kicked for inactivity");
						}
					}
				}
			}
		}
	}

	DoWincheck();
}


bool IGameController::IsTeamplay() const
{
	return m_GameFlags&GAMEFLAG_TEAMS;
}

void IGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo *pGameInfoObj = (CNetObj_GameInfo *)Server()->SnapNewItem(NETOBJTYPE_GAMEINFO, 0, sizeof(CNetObj_GameInfo));
	if(!pGameInfoObj)
		return;

	pGameInfoObj->m_GameFlags = m_GameFlags;
	pGameInfoObj->m_GameStateFlags = 0;
	if(m_GameOverTick != -1)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(GameServer()->m_World.m_Paused)
		pGameInfoObj->m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	pGameInfoObj->m_RoundStartTick = m_RoundStartTick;

	if(m_FakeWarmup)
		pGameInfoObj->m_WarmupTimer = m_FakeWarmup;
	else
		pGameInfoObj->m_WarmupTimer = m_Warmup;

	if (GameServer()->m_pController->IsLMS())
		pGameInfoObj->m_ScoreLimit = g_Config.m_SvLMSLives;
	else
		pGameInfoObj->m_ScoreLimit = g_Config.m_SvScorelimit;
	pGameInfoObj->m_TimeLimit = g_Config.m_SvTimelimit;

	pGameInfoObj->m_RoundNum = (str_length(g_Config.m_SvMaprotation) && g_Config.m_SvRoundsPerMap) ? g_Config.m_SvRoundsPerMap : 0;
	pGameInfoObj->m_RoundCurrent = m_RoundCount+1;

	// WARNING, this is very hardcoded; for ddnet client support
	CNetObj_GameInfoEx *pGameInfoEx = (CNetObj_GameInfoEx *)Server()->SnapNewItem(32767, 0, 12);
	if(!pGameInfoEx)
		return;

	pGameInfoEx->m_Flags =
		GAMEINFOFLAG_GAMETYPE_PLUS |
		GAMEINFOFLAG_ALLOW_EYE_WHEEL |
		GAMEINFOFLAG_ALLOW_HOOK_COLL |
		GAMEINFOFLAG_PREDICT_VANILLA |
		GAMEINFOFLAG_ENTITIES_DDNET |
		GAMEINFOFLAG_ENTITIES_DDRACE |
		GAMEINFOFLAG_ENTITIES_RACE;
	if (g_Config.m_SvDDAllowZoom)
		pGameInfoEx->m_Flags |= GAMEINFOFLAG_ALLOW_ZOOM;
	if (g_Config.m_SvGrenadeAmmo == -1 && IsGrenade())
		pGameInfoEx->m_Flags |= GAMEINFOFLAG_UNLIMITED_AMMO;
	pGameInfoEx->m_Flags2 = 
		GAMEINFOFLAG2_HUD_AMMO;
	if (!(g_Config.m_SvDDInstagibHideHealth && IsInstagib()))
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_HUD_HEALTH_ARMOR;
	if (g_Config.m_SvDDShowHud)
		pGameInfoEx->m_Flags2 |= GAMEINFOFLAG2_HUD_DDRACE;
		
	pGameInfoEx->m_Version = 8;

	// This object needs to be snapped alongside pGameInfoObj for that object to work properly
	int *pUuidItem = (int *)Server()->SnapNewItem(0, 32767, 16); // NETOBJTYPE_EX
	if(pUuidItem)
	{
		pUuidItem[0] = -1824658838;
		pUuidItem[1] = -629591830;
		pUuidItem[2] = -1450210576;
		pUuidItem[3] = 914991429;
	}
}

int IGameController::GetAutoTeam(int NotThisID)
{
	// this will force the auto balancer to work overtime aswell
	if(g_Config.m_DbgStress)
		return 0;

	int aNumplayers[2] = {0,0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GameServer()->m_apPlayers[i] && i != NotThisID)
		{
			if(GameServer()->m_apPlayers[i]->GetTeam() >= TEAM_RED && GameServer()->m_apPlayers[i]->GetTeam() <= TEAM_BLUE)
				aNumplayers[GameServer()->m_apPlayers[i]->GetTeam()]++;
		}
	}

	int Team = 0;
	if(IsTeamplay())
		Team = aNumplayers[TEAM_RED] > aNumplayers[TEAM_BLUE] ? TEAM_BLUE : TEAM_RED;

	if(CanJoinTeam(Team, NotThisID))
		return Team;
	return -1;
}

bool IGameController::CanJoinTeam(int Team, int NotThisID)
{
	if(Team == TEAM_SPECTATORS || (GameServer()->m_apPlayers[NotThisID] && GameServer()->m_apPlayers[NotThisID]->GetTeam() != TEAM_SPECTATORS))
		return true;

	int aNumplayers[2] = {0,0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GameServer()->m_apPlayers[i] && i != NotThisID)
		{
			if(GameServer()->m_apPlayers[i]->GetTeam() >= TEAM_RED && GameServer()->m_apPlayers[i]->GetTeam() <= TEAM_BLUE)
				aNumplayers[GameServer()->m_apPlayers[i]->GetTeam()]++;
		}
	}

	return (aNumplayers[0] + aNumplayers[1]) < Server()->MaxClients()-g_Config.m_SvSpectatorSlots;
}

bool IGameController::CheckTeamBalance()
{
	if(!IsTeamplay() || !g_Config.m_SvTeambalanceTime)
		return true;

	int aT[2] = {0, 0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pP = GameServer()->m_apPlayers[i];
		if(pP && pP->GetTeam() != TEAM_SPECTATORS)
			aT[pP->GetTeam()]++;
	}

	char aBuf[256];
	if(absolute(aT[0]-aT[1]) >= 2)
	{
		str_format(aBuf, sizeof(aBuf), "Teams are NOT balanced (red=%d blue=%d)", aT[0], aT[1]);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
		if(GameServer()->m_pController->m_UnbalancedTick == -1)
			GameServer()->m_pController->m_UnbalancedTick = Server()->Tick();
		return false;
	}
	else
	{
		str_format(aBuf, sizeof(aBuf), "Teams are balanced (red=%d blue=%d)", aT[0], aT[1]);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
		GameServer()->m_pController->m_UnbalancedTick = -1;
		return true;
	}
}

bool IGameController::CanChangeTeam(CPlayer *pPlayer, int JoinTeam)
{
	int aT[2] = {0, 0};

	if (!IsTeamplay() || JoinTeam == TEAM_SPECTATORS || !g_Config.m_SvTeambalanceTime)
		return true;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		CPlayer *pP = GameServer()->m_apPlayers[i];
		if(pP && pP->GetTeam() != TEAM_SPECTATORS)
			aT[pP->GetTeam()]++;
	}

	// simulate what would happen if changed team
	aT[JoinTeam]++;
	if (pPlayer->GetTeam() != TEAM_SPECTATORS)
		aT[JoinTeam^1]--;

	// there is a player-difference of at least 2
	if(absolute(aT[0]-aT[1]) >= 2)
	{
		// player wants to join team with less players
		if ((aT[0] < aT[1] && JoinTeam == TEAM_RED) || (aT[0] > aT[1] && JoinTeam == TEAM_BLUE))
			return true;
		else
			return false;
	}
	else
		return true;
}

void IGameController::DoWincheck()
{
	if(m_GameOverTick == -1 && !m_Warmup && !GameServer()->m_World.m_ResetRequested)
	{
		if(IsTeamplay())
		{
			// check score win condition
			if((g_Config.m_SvScorelimit > 0 && (m_aTeamscore[TEAM_RED] >= g_Config.m_SvScorelimit || m_aTeamscore[TEAM_BLUE] >= g_Config.m_SvScorelimit)) ||
				(g_Config.m_SvTimelimit > 0 && (Server()->Tick()-m_RoundStartTick) >= g_Config.m_SvTimelimit*Server()->TickSpeed()*60))
			{
				if(m_aTeamscore[TEAM_RED] != m_aTeamscore[TEAM_BLUE])
					EndRound();
				else
					m_SuddenDeath = 1;
			}
		}
		else
		{
			// gather some stats
			int Topscore = -9999;
			int TopscoreCount = 0;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(GameServer()->m_apPlayers[i])
				{
					if(GameServer()->m_apPlayers[i]->m_Score > Topscore)
					{
						Topscore = GameServer()->m_apPlayers[i]->m_Score;
						TopscoreCount = 1;
					}
					else if(GameServer()->m_apPlayers[i]->m_Score == Topscore)
						TopscoreCount++;
				}
			}

			// check score win condition
			if((g_Config.m_SvScorelimit > 0 && Topscore >= g_Config.m_SvScorelimit) ||
				(g_Config.m_SvTimelimit > 0 && (Server()->Tick()-m_RoundStartTick) >= g_Config.m_SvTimelimit*Server()->TickSpeed()*60))
			{
				if(TopscoreCount == 1)
					EndRound();
				else
					m_SuddenDeath = 1;
			}
		}
	}
}

int IGameController::ClampTeam(int Team)
{
	if(Team < 0)
		return TEAM_SPECTATORS;
	if(IsTeamplay())
		return Team&1;
	return 0;
}

void IGameController::SaveStats()
{
	if(g_Config.m_SvStatsFile[0] && g_Config.m_SvStatsOutputlevel)
	{
		char aBuf[1024];
		FILE* pFile = fopen(g_Config.m_SvStatsFile, "a");

		if(!pFile)
		{
			str_format(aBuf, sizeof(aBuf), "Failed to open %s to save stats", g_Config.m_SvStatsFile);
			GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "stats", aBuf);
			return;
		}

		{
			char TimeStr[2][128];
			double PlayingTime = (double)(Server()->Tick() - m_RoundStartTick)/Server()->TickSpeed();
			time_t Now_t = time(0);
			time_t StartRound_t = Now_t - (int)PlayingTime;

			strftime(TimeStr[0], sizeof(TimeStr[0]), "Roundstart at %d.%m.%Y on %X", localtime(&StartRound_t));
			strftime(TimeStr[1], sizeof(TimeStr[1]), "and ended at %X", localtime(&Now_t));
			str_format(aBuf, sizeof(aBuf), "--> %s %s (Length: %d min %.2lf sec). Gametype: %s\n\n", TimeStr[0], TimeStr[1], (int)PlayingTime/60, PlayingTime - ((int)PlayingTime/60)*60, GameServer()->GameType());
			fputs(aBuf, pFile);
		}

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
				continue;
			CPlayer* pP = GameServer()->m_apPlayers[i];

			char aaTemp[3][512] = {"", "", ""};
			// Outputlevel 1
			str_format(aaTemp[0], sizeof(aaTemp[0]), "ID: %2d\t| Name: %-15.15s| Team: %-10.10s| Score: %-6.1d| Kills: %-6.1d| Deaths: %-6.1d| Ratio: %-6.2lf",
					pP->GetCID(), Server()->ClientName(i), GetTeamName(pP->GetTeam()), pP->m_Score, pP->m_Stats.m_Kills, pP->m_Stats.m_Deaths, (pP->m_Stats.m_Deaths > 0) ? ((float)pP->m_Stats.m_Kills / (float)pP->m_Stats.m_Deaths) : 0
					);
			//Outputlevel 2
			if(g_Config.m_SvStatsOutputlevel > 1)
				str_format(aaTemp[1], sizeof(aaTemp[1]), "| Hits: %-6.1d| Total Shots: %-6.1d| Captures: %-6.1d| Fastest Capture: %6.2lf",
					pP->m_Stats.m_Hits, pP->m_Stats.m_TotalShots, (m_GameFlags&GAMEFLAG_FLAGS) ? pP->m_Stats.m_Captures : -1, ((m_GameFlags&GAMEFLAG_FLAGS) || pP->m_Stats.m_FastestCapture < 0.1) ? pP->m_Stats.m_FastestCapture : -1
					);
			//Outputlevel 3
			if(g_Config.m_SvStatsOutputlevel > 2)
				str_format(aaTemp[2], sizeof(aaTemp[2]), "| Lost Flags: %-6.1d",
					pP->m_Stats.m_LostFlags
					);

			str_format(aBuf, sizeof(aBuf), "%s %s %s\n", aaTemp[0], aaTemp[1], aaTemp[2]);
			fputs(aBuf, pFile);
		}

		if(IsTeamplay())
		{
			str_format(aBuf, sizeof(aBuf), "---------------------\nRed: %d | Blue: %d\n", m_aTeamscore[TEAM_RED], m_aTeamscore[TEAM_BLUE]);
			fputs(aBuf, pFile);
		}

		fputs("________________________________________________________________________________________________________________________________________\n\n\n", pFile);
		fclose(pFile);
	}
}
