#include "game.h"
namespace server {
	struct _extrainfo;
	struct clientinfo;
	extern void sendservmsgf(const char *s);
	extern void cheating_kick(clientinfo *ci);
	extern void notifypriv(int min, int max, const char *msg, ...);

	// Report detected cheats //
	void cheat_detected(clientinfo *ci, char *cheattype, int cheatprob, bool publicreport) {
		ci->_xi.cheating += cheatprob;
		if(publicreport) sendservmsgf("\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating\f4! (\f3Cheat-Type\f4: \f6%s\f4, \f0%i\f4%%)", ci->name, cheattype, ci->_xi.cheating);
		else notifypriv(3, 4, "\f3>>> \f4[\f3CHEATER \f6detection\f4] \f7Player \f1%s \f7is \f3cheating\f4! (\f3Cheat-Type\f4: \f6%s\f4, \f0%i\f4%%)", ci->name, cheattype, ci->_xi.cheating);
		if(ci->_xi.cheating >= 100) cheating_kick(ci);
	}
	// Cheats types //

	// FFA weapons in instagib gamemodes //
	void ffa_weapons_in_instagib(clientinfo *ci, int gun) {
		bool correctgun = gun == GUN_RIFLE || gun == GUN_FIST;
		if(m_insta && !correctgun) {
			cheat_detected(ci, "FFA weapons in instagib gamemodes", 50, true);
		}
	}
	// Editmode in non coop edit gamemode //
	void editmode_in_non_coop_edit(clientinfo *ci) {
		if(!m_edit) {
			cheat_detected(ci, "Editmode in non coopedit gamemode", 100, true);
		}
	}
	// Editmode packets in non coop edit gamemode //
	void edit_packets_in_non_coop_edit(clientinfo *ci, char *packet) {
		char cheat[256];
		if(!m_edit)	 {
			formatstring(cheat)("Edit packets in non coopedit gamemode \f4(\f5%s\f4)\f7", packet);
			cheat_detected(ci, cheat, 100, true);
		}
	}
	// Unknown sound //
	void unknown_sound(clientinfo *ci, int sound) {
		if(sound < 0 || sound > S_FLAGFAIL) {
			cheat_detected(ci, "Unknown sound", 25, false);
		}
	}
	// Wrong message size //
	void wrong_message_size(clientinfo *ci, int size) {
		if(size <=0) {
			cheat_detected(ci, "Wrong message size", 100, true);
		}
	}
}