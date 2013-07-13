#include "game.h"
namespace server {
	struct _extrainfo;
	struct clientinfo;
	extern void notifycheater(clientinfo *ci, const char *s, ...);
	extern void notifypriv(int min, int max, const char *msg, ...);
	extern void sendservmsgf(const char *fmt, ...);
	extern void cheating_kick(clientinfo *ci);

	// Report detected cheats //
	void cheat_detected(clientinfo *ci, const char *_cheat, int cheatprob, bool publicreport, bool banforcheat) {
		ci->_xi.cheating += cheatprob;
		if(publicreport) notifycheater(ci, "\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating\f4! (\f3Cheat-Type\f4: \f6%s\f4, \f0%i\f4%%)", ci->name, _cheat, ci->_xi.cheating);
		else notifypriv(PRIV_ADMIN, PRIV_ROOT, "\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating\f4! (\f3Cheat-Type\f4: \f6%s\f4, \f0%i\f4%%)", ci->name, _cheat, ci->_xi.cheating);
		if(ci->_xi.cheating >= 100) {
			if(banforcheat) cheating_kick(ci);
			else {
				sendservmsgf("\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating \f4(\f5%s\f4)", ci->name, _cheat);
				ci->_xi.cheating = 0;
			}
		}
	}

	// Checks if player is lagged or has not got map //
	bool islagged(clientinfo *ci) {
		return (ci->state.state == CS_LAGGED && ci->state.state != CS_SPECTATOR && smapname[0] && ci->exceeded);
	}

	// Cheat types //

	// FFA weapons in instagib gamemodes //
	void ffa_weapons_in_instagib(clientinfo *ci, int gun) {
		bool correctgun = gun == GUN_RIFLE || gun == GUN_FIST;
		if(m_insta && !correctgun) {
			cheat_detected(ci, "FFA weapons in instagib gamemodes", 50, true, true);
		}
	}
	// Editmode in non coop edit gamemode //
	void editmode_in_non_coop_edit(clientinfo *ci) {
		if(!m_edit && !islagged(ci)) {
			cheat_detected(ci, "Editmode in non coopedit gamemode", 100, true, true);
		}
	}
	// Editmode packets in non coop edit gamemode //
	void edit_packets_in_non_coop_edit(clientinfo *ci, const char *packet) {
		char cheat[256];
		if(!m_edit && !islagged(ci))	 {
			formatstring(cheat)("Edit packets in non coopedit gamemode \f4(\f5%s\f4)\f7", packet);
			cheat_detected(ci, cheat, 100, true, true);
		}
	}
	// Unknown sound //
	void unknown_sound(clientinfo *ci, int sound) {
		if(sound < 0 || sound > S_FLAGFAIL) {
			cheat_detected(ci, "Unknown sound", 25, true, true);
		}
	}
	// Wrong message size //
	void wrong_message_size(clientinfo *ci, int size) {
		if(size <=0) {
			cheat_detected(ci, "Wrong message size", 100, true, true);
		}
	}
	// Ping hack //
	void ping_hack(clientinfo *ci, int ping) {
		if(ci->_xi.lastping == ping && ci->_xi.lastsameping == 25) {
			cheat_detected(ci, "Ping hack", 50, true, false);
			ci->_xi.lastsameping = 0;
		}
		if(ci->_xi.lastping == ping) {
			ci->_xi.lastsameping++;
		} else {
			ci->_xi.lastsameping = 0;
			ci->_xi.lastping = ping;
		}
	}
}