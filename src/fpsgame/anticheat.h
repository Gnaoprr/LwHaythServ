#include "game.h"
extern int totalmillis;
namespace server {
	struct _extrainfo;
	struct clientinfo;
	extern int gamespeed;
	extern void notifycheater(clientinfo *ci, const char *msg, ...);
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

	// Class ac_check - checks for various things //
	class ac_check {
	public:
		// Checks if player is lagged //
		bool islagged(clientinfo *ci) {
			return (ci->state.state == CS_LAGGED && ci->state.state != CS_SPECTATOR && ci->exceeded);
		}
		// Checks if the player is alive //
		bool isalive(clientinfo *ci) {
			return (ci->state.state == CS_ALIVE && ci->state.state != CS_SPECTATOR);
		}
		// Checks if the player has the map //
		bool hasmap(clientinfo *ci) {
			return (ci->clientmap[0]);
		}
		// Checks player distance //
		float distance(vec a, vec b)
		{
		    int x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
		    return sqrt(x*x + y*y + z*z);
		}
		// Checks if distance is cause of speedhack //
		bool speedhack_distance(clientinfo *ci, float dist) {
			return (!islagged(ci) && dist < (5.2*gamespeed/100));
		}
		// Checks if the player has the correct gun //
		bool hascorrectgun(int gun) {
			if(m_insta) {
				return (gun == GUN_RIFLE || gun == GUN_FIST);
			} else return true;
		}
		// Checks if the player has sent a correct sound //
		bool correctsound(int sound) {
			return (sound >= 0 && sound <= 69);
		}
		// Checks if the player has sent a correct size packet //
		bool correctsize(int size) {
			return (size > 0);
		}
		// Checks if the player is sending the pings at a correct speed //
		bool correctpingspeed(int speed) {
			int min_correct_speed = 200;
			// int correct_speed = 250;
			int max_correct_speed = 300;
			return ((speed > min_correct_speed) && (speed < max_correct_speed));
		}
		// Checks for instagib gamemodes //
		bool instagib() {
			if(m_insta) return true;
			else return false;
		}
		// Checks for coop edit gamemode //
		bool coopedit() {
			if(m_edit) return true;
			else return false;
		}
	};
	ac_check check;

	// Cheat types //

	// FFA weapons in instagib gamemodes //
	void ffa_weapons_in_instagib(clientinfo *ci, int gun) {
		if(check.hascorrectgun(gun)) {
			cheat_detected(ci, "FFA weapons in instagib gamemodes", 50, true, true);
		}
	}
	// Editmode in non coop edit gamemode //
	void editmode_in_non_coop_edit(clientinfo *ci) {
		if(!check.coopedit() && !check.islagged(ci) && check.hasmap(ci)) {
			cheat_detected(ci, "Editmode in non coopedit gamemode", 100, true, true);
		}
	}
	// Editmode packets in non coop edit gamemode //
	void edit_packets_in_non_coop_edit(clientinfo *ci, const char *packet) {
		char cheat[256];
		if(!check.coopedit() && !check.islagged(ci) && check.hasmap(ci)) {
			formatstring(cheat)("Edit packets in non coopedit gamemode \f4(\f5%s\f4)\f7", packet);
			cheat_detected(ci, cheat, 100, true, true);
		}
	}
	// Unknown sound //
	void unknown_sound(clientinfo *ci, int sound) {
		if(check.correctsound(sound)) {
			cheat_detected(ci, "Unknown sound", 25, true, true);
		}
	}
	// Wrong message size //
	void wrong_message_size(clientinfo *ci, int size) {
		if(check.correctsize(size)) {
			cheat_detected(ci, "Wrong message size", 100, true, true);
		}
	}
	// Ping hack //
	void ping_hack(clientinfo *ci, int ping) {
		if(ci->_xi.lastping == ping && ci->_xi.lastsameping == 9) {
			cheat_detected(ci, "Ping hack", 50, true, false);
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
	// No send position hack //
	void no_send_position_hack(clientinfo *ci) {
		if(check.islagged(ci)) {
			cheat_detected(ci, "No send position hack", 50, true, true);
		}
	}
	// Speed hack ping //
	void speed_hack_ping(clientinfo *ci) {
		if(!ci->_xi.lastsentping) {
			ci->_xi.lastsentping = totalmillis;
			return;
		}
		int lastsentping = (totalmillis - ci->_xi.lastsentping);
		if(check.correctpingspeed(lastsentping)) {
			ci->_xi.wrongpings ++;
			if(ci->_xi.wrongpings == 10) {
				cheat_detected(ci, "Speed hack ping", 50, true, true);
				ci->_xi.wrongpings = 0;
			}
		}
	}
}