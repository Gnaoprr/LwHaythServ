/* 
 * Lightweight HaythServ - anticheat.h
 * 
 * This is a basic cheater detection, it is very configurable
 * and it should be pretty secure, if you are just looking
 * for a lightweight configuration, set securitylevel to 1,
 * and just toggle on some cheat detections (like edithack,
 * ffa weapons in instagib, etc...) if you have a good server,
 * and you are sure it can hold good protections with many
 * checks, set securitylevel to 9 and toggle all cheater
 * detections on.
 */

/* 
 * anticheat.h configuration
 * 
 * The anticheat is configurable via the anticheat.cfg file in
 * the server's main folder, here follow all the configurable
 * VARs.
 */
VAR(securitylevel, 1, 3, 9);

VAR(ffaweaponsininstagib, 0, 1, 1);
VAR(editmodeinnoncoop, 0, 1, 1);
VAR(nosendposition, 0, 1, 1);
VAR(speedhackping, 0, 1, 1);
VAR(editpacketsinnoncoop, 0, 0, 1);
VAR(unknownsound, 0, 0, 1);
VAR(wrongmessagesize, 0, 0, 1);
VAR(pinghack, 0, 0, 1);
VAR(itempickupininstagib, 0, 1, 1);

/* 
 * Main anticheat.h
 * 
 * Here follows the main anticheat.h, with all main functions
 * and checks.
 */
extern int totalmillis;
namespace server {
	struct _extrainfo;
	struct clientinfo;
	extern int gamespeed;
	extern void notifycheater(clientinfo *ci, const char *msg);
	extern void _notifypriv(const char *msg, int min, int max);
	extern void cheating_kick(clientinfo *ci);
	void cheat_detected(clientinfo *ci, const char *_cheat, int cheatprob, bool publicreport) {
		ci->_xi.cheating += cheatprob;
		char cheatermessage[256];
		formatstring(cheatermessage)("\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating\f4! (\f3Cheat-Type\f4: \f6%s\f4, \f0%i\f4%%)", ci->name, _cheat, ci->_xi.cheating);
		if(publicreport) notifycheater(ci, cheatermessage);
		else _notifypriv(cheatermessage, PRIV_ADMIN, PRIV_ROOT);
		if(ci->_xi.cheating >= 100) cheating_kick(ci);
	}
	void ffa_weapons_in_instagib(clientinfo *ci, int gun) {
		int cheatingprob;
		if(securitylevel <= 3) cheatingprob = 100;
		else if(securitylevel <= 6 && securitylevel > 3) cheatingprob = 50;
		else cheatingprob = 25;
		if(m_insta && gun != GUN_RIFLE && gun != GUN_FIST && ffaweaponsininstagib) {
			cheat_detected(ci, "FFA weapons in instagib gamemodes", cheatingprob, true);
		}
	}
	void editmode_in_non_coop_edit(clientinfo *ci) {
		if((!m_edit && (ci->clientmap[0] && securitylevel >= 3) && ((ci->state.state != CS_LAGGED && ci->state.state != CS_SPECTATOR && !ci->exceeded) && securitylevel >= 6)) && editmodeinnoncoop) {
			cheat_detected(ci, "Editmode in non coopedit gamemode", 100, true);
		}
	}
	void no_send_position_hack(clientinfo *ci) {
		if(((ci->state.state == CS_LAGGED && ci->state.state != CS_SPECTATOR && ci->exceeded )) && nosendposition) {
			cheat_detected(ci, "No send position hack", securitylevel >= 6 ? 50 : 100, true);
		}
	}
	/* void speed_hack_ping(clientinfo *ci) {
		if(!speedhackping) return;
		if(!ci->_xi.lastsentping) {
			ci->_xi.lastsentping = totalmillis;
			return;
		}
		int lastsentping = (totalmillis - ci->_xi.lastsentping);
		if((lastsentping < 200 || lastsentping > 300) && ci->clientmap[0] && ci->state.state != CS_LAGGED && ci->state.state != CS_SPECTATOR && !ci->exceeded) {
			ci->_xi.wrongpings ++;
			if((ci->_xi.wrongpings == 10 && securitylevel <= 3) || (ci->_xi.wrongpings == 20 && securitylevel > 3 && securitylevel <= 6) || (ci->_xi.wrongpings == 30 && securitylevel > 6 && securitylevel <= 9)) {
				cheat_detected(ci, "Speed hack ping", 100, true);
				ci->_xi.wrongpings = 0;
			}
		}
	} todo: fix */
	void edit_packets_in_non_coop_edit(clientinfo *ci, const char *packet) {
		char cheat[256];
		if((!m_edit && (ci->clientmap[0] && securitylevel >= 3) && ((ci->state.state != CS_LAGGED && ci->state.state != CS_SPECTATOR && !ci->exceeded) && securitylevel >= 6)) && editpacketsinnoncoop) {
			formatstring(cheat)("Edit packets in non coopedit gamemode \f4(\f5%s\f4)\f7", packet);
			cheat_detected(ci, cheat, 100, true);
		}
	}
	void unknown_sound(clientinfo *ci, int sound) {
		if((sound < 0 || sound > 69) && unknownsound) {
			cheat_detected(ci, "Unknown sound", 25, true);
		}
	}
	void wrong_message_size(clientinfo *ci, int size) {
		if(size < 0 && wrongmessagesize) {
			cheat_detected(ci, "Wrong message size", securitylevel >= 6 ? 50 : 100, true);
		}
	}
	void ping_hack(clientinfo *ci, int ping) {
		if(!pinghack) return;
		if(ci->_xi.lastping == ping && ci->_xi.lastsameping == securitylevel*3) {
			cheat_detected(ci, "Ping hack", 50, true);
			ci->_xi.lastsameping = 0;
			return;
		}
		if(ci->_xi.lastping == ping) {
			ci->_xi.lastsameping ++;
		} else {
			ci->_xi.lastsameping = 0;
			ci->_xi.lastping = ping;
		}
	}
	void item_pickup_in_instagib(clientinfo *ci) {
		int cheatingprob;
		if(securitylevel <= 3) cheatingprob = 100;
		else if(securitylevel <= 6 && securitylevel > 3) cheatingprob = 50;
		else cheatingprob = 25;
		if(m_insta && itempickupininstagib) {
			cheat_detected(ci, "Item pickup in instagib gamemodes", cheatingprob, true);
		}
	}
}
#include "game.h"