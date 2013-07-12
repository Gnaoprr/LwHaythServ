#include "game.h"
#include "modules.h"

namespace game
{
    void parseoptions(vector<const char *> &args)
    {
        loopv(args)
#ifndef STANDALONE
            if(!game::clientoption(args[i]))
#endif
            if(!server::serveroption(args[i]))
                conoutf(CON_ERROR, "unknown command-line option: %s", args[i]);
    }

    const char *gameident() { return "fps"; }
}

extern ENetAddress masteraddress;

namespace server
{
	struct _hookparam
    {
        void *args[8];
    } __attribute__((packed));
    
    _hookparam _hp;
    
    int _exechook(const char *name);
    
    struct server_entity            // server side version of "entity" type
    {
        int type;
        int spawntime;
        char spawned;
    };

    static const int DEATHMILLIS = 300;

    struct clientinfo;

    struct gameevent
    {
        virtual ~gameevent() {}

        virtual bool flush(clientinfo *ci, int fmillis);
        virtual void process(clientinfo *ci) {}

        virtual bool keepable() const { return false; }
    };

    struct timedevent : gameevent
    {
        int millis;

        bool flush(clientinfo *ci, int fmillis);
    };

    struct hitinfo
    {
        int target;
        int lifesequence;
        int rays;
        float dist;
        vec dir;
    };

    struct shotevent : timedevent
    {
        int id, gun;
        vec from, to;
        vector<hitinfo> hits;

        void process(clientinfo *ci);
    };

    struct explodeevent : timedevent
    {
        int id, gun;
        vector<hitinfo> hits;

        bool keepable() const { return true; }

        void process(clientinfo *ci);
    };

    struct suicideevent : gameevent
    {
        void process(clientinfo *ci);
    };

    struct pickupevent : gameevent
    {
        int ent;

        void process(clientinfo *ci);
    };

    template <int N>
    struct projectilestate
    {
        int projs[N];
        int numprojs;

        projectilestate() : numprojs(0) {}

        void reset() { numprojs = 0; }

        void add(int val)
        {
            if(numprojs>=N) numprojs = 0;
            projs[numprojs++] = val;
        }

        bool remove(int val)
        {
            loopi(numprojs) if(projs[i]==val)
            {
                projs[i] = projs[--numprojs];
                return true;
            }
            return false;
        }
    };

    struct gamestate : fpsstate
    {
        vec o;
        int state, editstate;
        int lastdeath, deadflush, lastspawn, lifesequence;
        int lastshot;
        projectilestate<8> rockets, grenades;
        int frags, flags, deaths, teamkills, shotdamage, damage, tokens;
        int lasttimeplayed, timeplayed;
        float effectiveness;

        gamestate() : state(CS_DEAD), editstate(CS_DEAD), lifesequence(0) {}

        bool isalive(int gamemillis)
        {
            return state==CS_ALIVE || (state==CS_DEAD && gamemillis - lastdeath <= DEATHMILLIS);
        }

        bool waitexpired(int gamemillis)
        {
            return gamemillis - lastshot >= gunwait;
        }

        void reset()
        {
            if(state!=CS_SPECTATOR) state = editstate = CS_DEAD;
            maxhealth = 100;
            rockets.reset();
            grenades.reset();

            timeplayed = 0;
            effectiveness = 0;
            frags = flags = deaths = teamkills = shotdamage = damage = tokens = 0;

            lastdeath = 0;

            respawn();
        }

        void respawn()
        {
            fpsstate::respawn();
            o = vec(-1e10f, -1e10f, -1e10f);
            deadflush = 0;
            lastspawn = -1;
            lastshot = 0;
            tokens = 0;
        }

        void reassign()
        {
            respawn();
            rockets.reset();
            grenades.reset();
        }
    };

    struct savedscore
    {
        uint ip;
        string name;
        int maxhealth, frags, flags, deaths, teamkills, shotdamage, damage;
        int timeplayed;
        float effectiveness;

        void save(gamestate &gs)
        {
            maxhealth = gs.maxhealth;
            frags = gs.frags;
            flags = gs.flags;
            deaths = gs.deaths;
            teamkills = gs.teamkills;
            shotdamage = gs.shotdamage;
            damage = gs.damage;
            timeplayed = gs.timeplayed;
            effectiveness = gs.effectiveness;
        }

        void restore(gamestate &gs)
        {
            if(gs.health==gs.maxhealth) gs.health = maxhealth;
            gs.maxhealth = maxhealth;
            gs.frags = frags;
            gs.flags = flags;
            gs.deaths = deaths;
            gs.teamkills = teamkills;
            gs.shotdamage = shotdamage;
            gs.damage = damage;
            gs.timeplayed = timeplayed;
            gs.effectiveness = effectiveness;
        }
    };

    extern int gamemillis, nextexceeded;

    struct _extrainfo
    {
        bool mute;
        bool editmute;
        int editmutewarn;
        bool forcedspectator;
        bool fakeprivon;
        int fakepriv;
        bool spy;
    };
    
    struct clientinfo
    {
        int clientnum, ownernum, connectmillis, sessionid, overflow;
        string name, team, mapvote;
        int playermodel;
        int modevote;
        int privilege;
        bool connected, local, timesync;
        int gameoffset, lastevent, pushed, exceeded;
        gamestate state;
        vector<gameevent *> events;
        vector<uchar> position, messages;
        uchar *wsdata;
        int wslen;
        vector<clientinfo *> bots;
        int ping, aireinit;
        string clientmap;
        int mapcrc;
        bool warned, gameclip;
        ENetPacket *getdemo, *getmap, *clipboard;
        int lastclipboard, needclipboard;
        int connectauth;
        uint authreq;
        string authname, authdesc;
        void *authchallenge;
        int authkickvictim;
        char *authkickreason;
        _extrainfo _xi;


        clientinfo() : getdemo(NULL), getmap(NULL), clipboard(NULL), authchallenge(NULL), authkickreason(NULL) { reset(); }
        ~clientinfo() { events.deletecontents(); cleanclipboard(); cleanauth(); }

        void addevent(gameevent *e)
        {
            if(state.state==CS_SPECTATOR || events.length()>100) delete e;
            else events.add(e);
        }

        enum
        {
            PUSHMILLIS = 2500
        };

        int calcpushrange()
        {
            ENetPeer *peer = getclientpeer(ownernum);
            return PUSHMILLIS + (peer ? peer->roundTripTime + peer->roundTripTimeVariance : ENET_PEER_DEFAULT_ROUND_TRIP_TIME);
        }

        bool checkpushed(int millis, int range)
        {
            return millis >= pushed - range && millis <= pushed + range;
        }

        void scheduleexceeded()
        {
            if(state.state!=CS_ALIVE || !exceeded) return;
            int range = calcpushrange();
            if(!nextexceeded || exceeded + range < nextexceeded) nextexceeded = exceeded + range;
        }

        void setexceeded()
        {
            if(state.state==CS_ALIVE && !exceeded && !checkpushed(gamemillis, calcpushrange())) exceeded = gamemillis;
            scheduleexceeded(); 
        }
            
        void setpushed()
        {
            pushed = max(pushed, gamemillis);
            if(exceeded && checkpushed(exceeded, calcpushrange())) exceeded = 0;
        }
        
        bool checkexceeded()
        {
            return state.state==CS_ALIVE && exceeded && gamemillis > exceeded + calcpushrange();
        }

        void mapchange()
        {
            mapvote[0] = 0;
            modevote = INT_MAX;
            state.reset();
            events.deletecontents();
            overflow = 0;
            timesync = false;
            lastevent = 0;
            exceeded = 0;
            pushed = 0;
            clientmap[0] = '\0';
            mapcrc = 0;
            warned = false;
            gameclip = false;
        }

        void reassign()
        {
            state.reassign();
            events.deletecontents();
            timesync = false;
            lastevent = 0;
        }

        void cleanclipboard(bool fullclean = true)
        {
            if(clipboard) { if(--clipboard->referenceCount <= 0) enet_packet_destroy(clipboard); clipboard = NULL; }
            if(fullclean) lastclipboard = 0;
        }

        void cleanauthkick()
        {
            authkickvictim = -1;
            DELETEA(authkickreason);
        }

        void cleanauth(bool full = true)
        {
            authreq = 0;
            if(authchallenge) { freechallenge(authchallenge); authchallenge = NULL; }
            if(full) cleanauthkick();
        }

        void reset()
        {
            name[0] = team[0] = 0;
            playermodel = -1;
            privilege = PRIV_NONE;
            connected = local = false;
            connectauth = 0;
            position.setsize(0);
            messages.setsize(0);
            ping = 0;
            aireinit = 0;
            needclipboard = 0;
            cleanclipboard();
            cleanauth();
            mapchange();
        }

        int geteventmillis(int servmillis, int clientmillis)
        {
            if(!timesync || (events.empty() && state.waitexpired(servmillis)))
            {
                timesync = true;
                gameoffset = servmillis - clientmillis;
                return servmillis;
            }
            else return gameoffset + clientmillis;
        }
    };

    struct ban
    {
        int time, expire;
        uint ip;
    };

    namespace aiman
    {
        extern void removeai(clientinfo *ci);
        extern void clearai();
        extern void checkai();
        extern void reqadd(clientinfo *ci, int skill);
        extern void reqdel(clientinfo *ci);
        extern void setbotlimit(clientinfo *ci, int limit);
        extern void setbotbalance(clientinfo *ci, bool balance);
        extern void changemap();
        extern void addclient(clientinfo *ci);
        extern void changeteam(clientinfo *ci);
    }

    #define MM_MODE 0xF
    #define MM_AUTOAPPROVE 0x1000
    #define MM_PRIVSERV (MM_MODE | MM_AUTOAPPROVE)
    #define MM_PUBSERV ((1<<MM_OPEN) | (1<<MM_VETO))
    #define MM_COOPSERV (MM_AUTOAPPROVE | MM_PUBSERV | (1<<MM_LOCKED))

    bool notgotitems = true;        // true when map has changed and waiting for clients to send item
    int gamemode = 0;
    int gamemillis = 0, gamelimit = 0, nextexceeded = 0, gamespeed = 100;
    bool gamepaused = false, shouldstep = true;

    string smapname = "";
    int interm = 0;
    enet_uint32 lastsend = 0;
    int mastermode = MM_OPEN, mastermask = MM_PRIVSERV;
    stream *mapdata = NULL;

    vector<uint> allowedips;
    vector<ban> bannedips;

    void addban(uint ip, int expire)
    {
        allowedips.removeobj(ip);
        ban b;
        b.time = totalmillis;
        b.expire = totalmillis + expire;
        b.ip = ip;
        loopv(bannedips) if(b.expire < bannedips[i].expire) { bannedips.insert(i, b); return; }
        bannedips.add(b);
    }

    vector<clientinfo *> connects, clients, bots;

    void kickclients(uint ip, clientinfo *actor = NULL)
    {
        loopvrev(clients)
        {
            clientinfo &c = *clients[i];
            if(c.state.aitype != AI_NONE || c.privilege >= PRIV_ADMIN || c.local) continue;
            if(actor && (c.privilege > actor->privilege || c.clientnum == actor->clientnum)) continue;
            if(getclientip(c.clientnum) == ip) disconnect_client(c.clientnum, DISC_KICK);
        }
    }
 
    struct maprotation
    {
        static int exclude;
        int modes;
        string map;
        
        int calcmodemask() const { return modes&(1<<NUMGAMEMODES) ? modes & ~exclude : modes; }
        bool hasmode(int mode, int offset = STARTGAMEMODE) const { return (calcmodemask() & (1 << (mode-offset))) != 0; }

        int findmode(int mode) const
        {
            if(!hasmode(mode)) loopi(NUMGAMEMODES) if(hasmode(i, 0)) return i+STARTGAMEMODE;
            return mode;
        }

        bool match(int reqmode, const char *reqmap) const
        {
            return hasmode(reqmode) && (!map[0] || !reqmap[0] || !strcmp(map, reqmap));
        }

        bool includes(const maprotation &rot) const
        {
            return rot.modes == modes ? rot.map[0] && !map[0] : (rot.modes & modes) == rot.modes;
        }
    };
    int maprotation::exclude = 0;
    vector<maprotation> maprotations;
    int curmaprotation = 0;

    VAR(lockmaprotation, 0, 0, 2);

    void maprotationreset()
    {
        maprotations.setsize(0);
        curmaprotation = 0;
        maprotation::exclude = 0;
    }

    void nextmaprotation()
    {
        curmaprotation++;
        if(maprotations.inrange(curmaprotation) && maprotations[curmaprotation].modes) return;
        do curmaprotation--;
        while(maprotations.inrange(curmaprotation) && maprotations[curmaprotation].modes);
        curmaprotation++;
    }

    int findmaprotation(int mode, const char *map)
    {
        for(int i = curmaprotation; i < maprotations.length(); i++)
        {
            maprotation &rot = maprotations[i];
            if(!rot.modes) break;
            if(rot.match(mode, map)) return i;
        }
        int start;
        for(start = curmaprotation - 1; start >= 0; start--) if(!maprotations[start].modes) break;
        start++;
        for(int i = start; i < curmaprotation; i++)
        {
            maprotation &rot = maprotations[i];
            if(!rot.modes) break;
            if(rot.match(mode, map)) return i;
        }
        int best = -1;
        loopv(maprotations)
        {
            maprotation &rot = maprotations[i];
            if(rot.match(mode, map) && (best < 0 || maprotations[best].includes(rot))) best = i;
        }
        return best;
    }

    bool searchmodename(const char *haystack, const char *needle)
    {
        if(!needle[0]) return true;
        do
        {
            if(needle[0] != '.')
            {
                haystack = strchr(haystack, needle[0]);
                if(!haystack) break;
                haystack++;
            }
            const char *h = haystack, *n = needle+1;
            for(; *h && *n; h++)
            {
                if(*h == *n) n++;
                else if(*h != ' ') break; 
            }
            if(!*n) return true;
            if(*n == '.') return !*h;
        } while(needle[0] != '.');
        return false;
    }

    int genmodemask(vector<char *> &modes)
    {
        int modemask = 0;
        loopv(modes)
        {
            const char *mode = modes[i];
            int op = mode[0];
            switch(mode[0])
            {
                case '*':
                    modemask |= 1<<NUMGAMEMODES;
                    loopk(NUMGAMEMODES) if(m_checknot(k+STARTGAMEMODE, M_DEMO|M_EDIT|M_LOCAL)) modemask |= 1<<k;
                    continue;
                case '!':
                    mode++;
                    if(mode[0] != '?') break;
                case '?':
                    mode++;
                    loopk(NUMGAMEMODES) if(searchmodename(gamemodes[k].name, mode))
                    {
                        if(op == '!') modemask &= ~(1<<k);
                        else modemask |= 1<<k;
                    }
                    continue;
            }
            int modenum = INT_MAX;
            if(isdigit(mode[0])) modenum = atoi(mode);
            else loopk(NUMGAMEMODES) if(searchmodename(gamemodes[k].name, mode)) { modenum = k+STARTGAMEMODE; break; }
            if(!m_valid(modenum)) continue;
            switch(op)
            {
                case '!': modemask &= ~(1 << (modenum - STARTGAMEMODE)); break;
                default: modemask |= 1 << (modenum - STARTGAMEMODE); break;
            }
        }
        return modemask;
    }
         
    bool addmaprotation(int modemask, const char *map)
    {
        if(!map[0]) loopk(NUMGAMEMODES) if(modemask&(1<<k) && !m_check(k+STARTGAMEMODE, M_EDIT)) modemask &= ~(1<<k);
        if(!modemask) return false;
        if(!(modemask&(1<<NUMGAMEMODES))) maprotation::exclude |= modemask;
        maprotation &rot = maprotations.add();
        rot.modes = modemask;
        copystring(rot.map, map);
        return true;
    }
        
    void addmaprotations(tagval *args, int numargs)
    {
        vector<char *> modes, maps;
        for(int i = 0; i + 1 < numargs; i += 2)
        {
            explodelist(args[i].getstr(), modes);
            explodelist(args[i+1].getstr(), maps);
            int modemask = genmodemask(modes);
            if(maps.length()) loopvj(maps) addmaprotation(modemask, maps[j]);
            else addmaprotation(modemask, "");
            modes.deletearrays();
            maps.deletearrays();
        }
        if(maprotations.length() && maprotations.last().modes)
        {
            maprotation &rot = maprotations.add();
            rot.modes = 0;
            rot.map[0] = '\0';
        }
    }
    
    COMMAND(maprotationreset, "");
    COMMANDN(maprotation, addmaprotations, "ss2V");

    struct demofile
    {
        string info;
        uchar *data;
        int len;
    };

    vector<demofile> demos;

    bool demonextmatch = false;
    stream *demotmp = NULL, *demorecord = NULL, *demoplayback = NULL;
    int nextplayback = 0, demomillis = 0;

    VAR(maxdemos, 0, 5, 25);
    VAR(maxdemosize, 0, 16, 64);
    VAR(restrictdemos, 0, 1, 1);

    VAR(restrictpausegame, 0, 1, 1);
    VAR(restrictgamespeed, 0, 1, 1);

    //zeromod variables
    VAR(clearbots, 0, 1, 1);
    VAR(servergamespeed, 10, 100, 1000);
    FVAR(servergamelimit, 1.0, 10.0, 1440.0);  //max is 24 hours
    VAR(serverovertime, 0, 0, 1);
    
    SVAR(serverdesc, "");
    SVAR(serverpass, "");
    SVAR(adminpass, "");
    VARF(publicserver, 0, 0, 2, {
		switch(publicserver)
		{
			case 0: default: mastermask = MM_PRIVSERV; break;
			case 1: mastermask = MM_PUBSERV; break;
			case 2: mastermask = MM_COOPSERV; break;
		}
	});
    SVAR(servermotd, "");

    struct teamkillkick
    {
        int modes, limit, ban;

        bool match(int mode) const
        {
            return (modes&(1<<(mode-STARTGAMEMODE)))!=0;
        }

        bool includes(const teamkillkick &tk) const
        {
            return tk.modes != modes && (tk.modes & modes) == tk.modes;
        }
    };
    vector<teamkillkick> teamkillkicks;

    void teamkillkickreset()
    {
        teamkillkicks.setsize(0);
    }

    void addteamkillkick(char *modestr, int *limit, int *ban)
    {
        vector<char *> modes;
        explodelist(modestr, modes);
        teamkillkick &kick = teamkillkicks.add();
        kick.modes = genmodemask(modes);
        kick.limit = *limit;
        kick.ban = *ban > 0 ? *ban*60000 : (*ban < 0 ? 0 : 30*60000); 
        modes.deletearrays();
    }

    COMMAND(teamkillkickreset, "");
    COMMANDN(teamkillkick, addteamkillkick, "sii");

    struct teamkillinfo
    {
        uint ip;
        int teamkills;
    };
    vector<teamkillinfo> teamkills;
    bool shouldcheckteamkills = false;

    void addteamkill(clientinfo *actor, clientinfo *victim, int n)
    {
        if(!m_timed || actor->state.aitype != AI_NONE || actor->local || actor->privilege || (victim && victim->state.aitype != AI_NONE)) return;
        shouldcheckteamkills = true;
        uint ip = getclientip(actor->clientnum);
        loopv(teamkills) if(teamkills[i].ip == ip) 
        { 
            teamkills[i].teamkills += n;
            return;
        }
        teamkillinfo &tk = teamkills.add();
        tk.ip = ip;
        tk.teamkills = n;
    }

    void checkteamkills()
    {
        teamkillkick *kick = NULL;
        if(m_timed) loopv(teamkillkicks) if(teamkillkicks[i].match(gamemode) && (!kick || kick->includes(teamkillkicks[i])))
            kick = &teamkillkicks[i];
        if(kick) loopvrev(teamkills)
        {
            teamkillinfo &tk = teamkills[i];
            if(tk.teamkills >= kick->limit)
            {
                if(kick->ban > 0) addban(tk.ip, kick->ban);
                kickclients(tk.ip);
                teamkills.removeunordered(i);
            }
        }
        shouldcheckteamkills = false;
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *ci) { delete (clientinfo *)ci; }

    clientinfo *getinfo(int n)
    {
        if(n < MAXCLIENTS) return (clientinfo *)getclientinfo(n);
        n -= MAXCLIENTS;
        return bots.inrange(n) ? bots[n] : NULL;
    }

    uint mcrc = 0;
    vector<entity> ments;
    vector<server_entity> sents;
    vector<savedscore> scores;

    int msgsizelookup(int msg)
    {
        static int sizetable[NUMMSG] = { -1 };
        if(sizetable[0] < 0)
        {
            memset(sizetable, -1, sizeof(sizetable));
            for(const int *p = msgsizes; *p >= 0; p += 2) sizetable[p[0]] = p[1];
        }
        return msg >= 0 && msg < NUMMSG ? sizetable[msg] : -1;
    }

    const char *modename(int n, const char *unknown)
    {
        if(m_valid(n)) return gamemodes[n - STARTGAMEMODE].name;
        return unknown;
    }

    const char *mastermodename(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodenames)/sizeof(mastermodenames[0])) ? mastermodenames[n-MM_START] : unknown;
    }

    const char *privname(int type)
    {
        static char buf[32];    //static buffer for unknown[i]
        switch(type)
        {
            case PRIV_ADMIN: return "\fs\f6admin\fr";
            case PRIV_AUTH: return "\fs\f0auth\fr";
            case PRIV_MASTER: return "\fs\f0master\fr";
            case PRIV_ROOT: return "\fs\f3root\fr";
            case PRIV_NONE: return "none";
        }
    }

    void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }
    void sendservmsgf(const char *fmt, ...)
    {
         defvformatstring(s, fmt, fmt);
         sendf(-1, 1, "ris", N_SERVMSG, s);
    }

    void resetitems()
    {
        mcrc = 0;
        ments.setsize(0);
        sents.setsize(0);
        //cps.reset();
    }

    bool serveroption(const char *arg)
    {
        if(arg[0]=='-') switch(arg[1])
        {
            case 'n': setsvar("serverdesc", &arg[2]); return true;
            case 'y': setsvar("serverpass", &arg[2]); return true;
            case 'p': setsvar("adminpass", &arg[2]); return true;
            case 'o': setvar("publicserver", atoi(&arg[2])); return true;
        }
        return false;
    }

    void serverinit()
    {
        smapname[0] = '\0';
        resetitems();
    }

    int numclients(int exclude = -1, bool nospec = true, bool noai = true, bool priv = false)
    {
        int n = 0;
        loopv(clients) 
        {
            clientinfo *ci = clients[i];
            if(ci->_xi.spy) continue;
            if(ci->clientnum!=exclude && (!nospec || ci->state.state!=CS_SPECTATOR || (priv && (ci->privilege || ci->local))) && (!noai || ci->state.aitype == AI_NONE)) n++;
        }
        return n;
    }

    bool duplicatename(clientinfo *ci, char *name)
    {
        if(!name) name = ci->name;
        loopv(clients) if(clients[i]!=ci && !strcmp(name, clients[i]->name)) return true;
        return false;
    }

    const char *colorname(clientinfo *ci, char *name = NULL)
    {
        if(!name) name = ci->name;
        if(name[0] && !duplicatename(ci, name) && ci->state.aitype == AI_NONE) return name;
        static string cname[3];
        static int cidx = 0;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx])(ci->state.aitype == AI_NONE ? "%s \fs\f5(%d)\fr" : "%s \fs\f5[%d]\fr", name, ci->clientnum);
        return cname[cidx];
    }

    struct servmode
    {
        virtual ~servmode() {}

        virtual void entergame(clientinfo *ci) {}
        virtual void leavegame(clientinfo *ci, bool disconnecting = false) {}

        virtual void moved(clientinfo *ci, const vec &oldpos, bool oldclip, const vec &newpos, bool newclip) {}
        virtual bool canspawn(clientinfo *ci, bool connecting = false) { return true; }
        virtual void spawned(clientinfo *ci) {}
        virtual int fragvalue(clientinfo *victim, clientinfo *actor)
        {
            if(victim==actor || isteam(victim->team, actor->team)) return -1;
            return 1;
        }
        virtual void died(clientinfo *victim, clientinfo *actor) {}
        virtual bool canchangeteam(clientinfo *ci, const char *oldteam, const char *newteam) { return true; }
        virtual void changeteam(clientinfo *ci, const char *oldteam, const char *newteam) {}
        virtual void initclient(clientinfo *ci, packetbuf &p, bool connecting) {}
        virtual void update() {}
        virtual void cleanup() {}
        virtual void setup() {}
        virtual void newmap() {}
        virtual void intermission() {}
        virtual bool hidefrags() { return false; }
        virtual int getteamscore(const char *team) { return 0; }
        virtual void getteamscores(vector<teamscore> &scores) {}
        virtual bool extinfoteam(const char *team, ucharbuf &p) { return false; }
    };

    #define SERVMODE 1
    #include "capture.h"
    #include "ctf.h"
    #include "collect.h"

    captureservmode capturemode;
    ctfservmode ctfmode;
    collectservmode collectmode;
    servmode *smode = NULL;

    bool canspawnitem(int type) { return !m_noitems && (type>=I_SHELLS && type<=I_QUAD && (!m_noammo || type<I_SHELLS || type>I_CARTRIDGES)); }

    int spawntime(int type)
    {
        if(m_classicsp) return INT_MAX;
        int np = numclients(-1, true, false);
        np = np<3 ? 4 : (np>4 ? 2 : 3);         // spawn times are dependent on number of players
        int sec = 0;
        switch(type)
        {
            case I_SHELLS:
            case I_BULLETS:
            case I_ROCKETS:
            case I_ROUNDS:
            case I_GRENADES:
            case I_CARTRIDGES: sec = np*4; break;
            case I_HEALTH: sec = np*5; break;
            case I_GREENARMOUR: sec = 20; break;
            case I_YELLOWARMOUR: sec = 30; break;
            case I_BOOST: sec = 60; break;
            case I_QUAD: sec = 70; break;
        }
        return sec*1000;
    }

    bool delayspawn(int type)
    {
        switch(type)
        {
            case I_GREENARMOUR:
            case I_YELLOWARMOUR:
                return !m_classicsp;
            case I_BOOST:
            case I_QUAD:
                return true;
            default:
                return false;
        }
    }
 
    bool pickup(int i, int sender)         // server side item pickup, acknowledge first client that gets it
    {
        if((m_timed && gamemillis>=gamelimit) || !sents.inrange(i) || !sents[i].spawned) return false;
        clientinfo *ci = getinfo(sender);
        if(!ci || (!ci->local && !ci->state.canpickup(sents[i].type))) return false;
        sents[i].spawned = false;
        sents[i].spawntime = spawntime(sents[i].type);
        sendf(-1, 1, "ri3", N_ITEMACC, i, sender);
        ci->state.pickup(sents[i].type);
        return true;
    }

    static hashset<teaminfo> teaminfos;

    void clearteaminfo()
    {
        teaminfos.clear();
    }

    bool teamhasplayers(const char *team) { loopv(clients) if(!strcmp(clients[i]->team, team)) return true; return false; }

    bool pruneteaminfo()
    {
        int oldteams = teaminfos.numelems;
        enumerates(teaminfos, teaminfo, old,
            if(!old.frags && !teamhasplayers(old.team)) teaminfos.remove(old.team);
        );
        return teaminfos.numelems < oldteams;
    }

    teaminfo *addteaminfo(const char *team)
    {
        teaminfo *t = teaminfos.access(team);
        if(!t)
        {
            if(teaminfos.numelems >= MAXTEAMS && !pruneteaminfo()) return NULL;
            t = &teaminfos[team];
            copystring(t->team, team, sizeof(t->team));
            t->frags = 0;
        }
        return t;
    }

    clientinfo *choosebestclient(float &bestrank)
    {
        clientinfo *best = NULL;
        bestrank = -1;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.timeplayed<0) continue;
            float rank = ci->state.state!=CS_SPECTATOR ? ci->state.effectiveness/max(ci->state.timeplayed, 1) : -1;
            if(!best || rank > bestrank) { best = ci; bestrank = rank; }
        }
        return best;
    }

    void autoteam()
    {
        static const char * const teamnames[2] = {"good", "evil"};
        vector<clientinfo *> team[2];
        float teamrank[2] = {0, 0};
        for(int round = 0, remaining = clients.length(); remaining>=0; round++)
        {
            int first = round&1, second = (round+1)&1, selected = 0;
            while(teamrank[first] <= teamrank[second])
            {
                float rank;
                clientinfo *ci = choosebestclient(rank);
                if(!ci) break;
                if(smode && smode->hidefrags()) rank = 1;
                else if(selected && rank<=0) break;
                ci->state.timeplayed = -1;
                team[first].add(ci);
                if(rank>0) teamrank[first] += rank;
                selected++;
                if(rank<=0) break;
            }
            if(!selected) break;
            remaining -= selected;
        }
        loopi(sizeof(team)/sizeof(team[0]))
        {
            addteaminfo(teamnames[i]);
            loopvj(team[i])
            {
                clientinfo *ci = team[i][j];
                if(!strcmp(ci->team, teamnames[i])) continue;
                copystring(ci->team, teamnames[i], MAXTEAMLEN+1);
                sendf(-1, 1, "riisi", N_SETTEAM, ci->clientnum, teamnames[i], -1);
            }
        }
    }

    struct teamrank
    {
        const char *name;
        float rank;
        int clients;

        teamrank(const char *name) : name(name), rank(0), clients(0) {}
    };

    const char *chooseworstteam(const char *suggest = NULL, clientinfo *exclude = NULL)
    {
        teamrank teamranks[2] = { teamrank("good"), teamrank("evil") };
        const int numteams = sizeof(teamranks)/sizeof(teamranks[0]);
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci==exclude || ci->state.aitype!=AI_NONE || ci->state.state==CS_SPECTATOR || !ci->team[0]) continue;
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            ci->state.lasttimeplayed = lastmillis;

            loopj(numteams) if(!strcmp(ci->team, teamranks[j].name))
            {
                teamrank &ts = teamranks[j];
                ts.rank += ci->state.effectiveness/max(ci->state.timeplayed, 1);
                ts.clients++;
                break;
            }
        }
        teamrank *worst = &teamranks[numteams-1];
        loopi(numteams-1)
        {
            teamrank &ts = teamranks[i];
            if(smode && smode->hidefrags())
            {
                if(ts.clients < worst->clients || (ts.clients == worst->clients && ts.rank < worst->rank)) worst = &ts;
            }
            else if(ts.rank < worst->rank || (ts.rank == worst->rank && ts.clients < worst->clients)) worst = &ts;
        }
        return worst->name;
    }

    void prunedemos(int extra = 0)
    {
        int n = clamp(demos.length() + extra - maxdemos, 0, demos.length());
        if(n <= 0) return;
        loopi(n) delete[] demos[i].data;
        demos.remove(0, n);
    }
 
    void adddemo()
    {
        if(!demotmp) return;
        int len = (int)min(demotmp->size(), stream::offset((maxdemosize<<20) + 0x10000));
        demofile &d = demos.add();
        time_t t = time(NULL);
        char *timestr = ctime(&t), *trim = timestr + strlen(timestr);
        while(trim>timestr && iscubespace(*--trim)) *trim = '\0';
        formatstring(d.info)("%s: %s, %s, %.2f%s", timestr, modename(gamemode), smapname, len > 1024*1024 ? len/(1024*1024.f) : len/1024.0f, len > 1024*1024 ? "MB" : "kB");
        sendservmsgf("demo \"%s\" recorded", d.info);
        d.data = new uchar[len];
        d.len = len;
        demotmp->seek(0, SEEK_SET);
        demotmp->read(d.data, len);
        DELETEP(demotmp);
    }
        
    void enddemorecord()
    {
        if(!demorecord) return;

        DELETEP(demorecord);

        if(!demotmp) return;
        if(!maxdemos || !maxdemosize) { DELETEP(demotmp); return; }

        prunedemos(1);
        adddemo();
    }

    void writedemo(int chan, void *data, int len)
    {
        if(!demorecord) return;
        int stamp[3] = { gamemillis, chan, len };
        lilswap(stamp, 3);
        demorecord->write(stamp, sizeof(stamp));
        demorecord->write(data, len);
        if(demorecord->rawtell() >= (maxdemosize<<20)) enddemorecord();
    }

    void recordpacket(int chan, void *data, int len)
    {
        writedemo(chan, data, len);
    }

    int welcomepacket(packetbuf &p, clientinfo *ci);
    void sendwelcome(clientinfo *ci);

    void setupdemorecord()
    {
        if(!m_mp(gamemode) || m_edit) return;

        demotmp = opentempfile("demorecord", "w+b");
        if(!demotmp) return;

        stream *f = opengzfile(NULL, "wb", demotmp);
        if(!f) { DELETEP(demotmp); return; }

        sendservmsg("recording demo");

        demorecord = f;

        demoheader hdr;
        memcpy(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
        hdr.version = DEMO_VERSION;
        hdr.protocol = PROTOCOL_VERSION;
        lilswap(&hdr.version, 2);
        demorecord->write(&hdr, sizeof(demoheader));

        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        welcomepacket(p, NULL);
        writedemo(1, p.buf, p.len);
    }

    void listdemos(int cn)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_SENDDEMOLIST);
        putint(p, demos.length());
        loopv(demos) sendstring(demos[i].info, p);
        sendpacket(cn, 1, p.finalize());
    }

    void cleardemos(int n)
    {
        if(!n)
        {
            loopv(demos) delete[] demos[i].data;
            demos.shrink(0);
            sendservmsg("cleared all demos");
        }
        else if(demos.inrange(n-1))
        {
            delete[] demos[n-1].data;
            demos.remove(n-1);
            sendservmsgf("cleared demo %d", n);
        }
    }

    static void freegetmap(ENetPacket *packet)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->getmap == packet) ci->getmap = NULL;
        }
    }

    static void freegetdemo(ENetPacket *packet)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->getdemo == packet) ci->getdemo = NULL;
        }
    }

    void senddemo(clientinfo *ci, int num)
    {
        if(ci->getdemo) return;
        if(!num) num = demos.length();
        if(!demos.inrange(num-1)) return;
        demofile &d = demos[num-1];
        if((ci->getdemo = sendf(ci->clientnum, 2, "rim", N_SENDDEMO, d.len, d.data)))
            ci->getdemo->freeCallback = freegetdemo;
    }

    void enddemoplayback()
    {
        if(!demoplayback) return;
        DELETEP(demoplayback);

        loopv(clients) sendf(clients[i]->clientnum, 1, "ri3", N_DEMOPLAYBACK, 0, clients[i]->clientnum);

        sendservmsg("demo playback finished");

        loopv(clients) sendwelcome(clients[i]);
    }

    void setupdemoplayback()
    {
        if(demoplayback) return;
        demoheader hdr;
        string msg;
        msg[0] = '\0';
        defformatstring(file)("%s.dmo", smapname);
        demoplayback = opengzfile(file, "rb");
        if(!demoplayback) formatstring(msg)("could not read demo \"%s\"", file);
        else if(demoplayback->read(&hdr, sizeof(demoheader))!=sizeof(demoheader) || memcmp(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic)))
            formatstring(msg)("\"%s\" is not a demo file", file);
        else
        {
            lilswap(&hdr.version, 2);
            if(hdr.version!=DEMO_VERSION) formatstring(msg)("demo \"%s\" requires an %s version of Cube 2: Sauerbraten", file, hdr.version<DEMO_VERSION ? "older" : "newer");
            else if(hdr.protocol!=PROTOCOL_VERSION) formatstring(msg)("demo \"%s\" requires an %s version of Cube 2: Sauerbraten", file, hdr.protocol<PROTOCOL_VERSION ? "older" : "newer");
        }
        if(msg[0])
        {
            DELETEP(demoplayback);
            sendservmsg(msg);
            return;
        }

        sendservmsgf("playing demo \"%s\"", file);

        demomillis = 0;
        sendf(-1, 1, "ri3", N_DEMOPLAYBACK, 1, -1);

        if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
        {
            enddemoplayback();
            return;
        }
        lilswap(&nextplayback, 1);
    }

    void readdemo()
    {
        if(!demoplayback) return;
        demomillis += curtime;
        while(demomillis>=nextplayback)
        {
            int chan, len;
            if(demoplayback->read(&chan, sizeof(chan))!=sizeof(chan) ||
               demoplayback->read(&len, sizeof(len))!=sizeof(len))
            {
                enddemoplayback();
                return;
            }
            lilswap(&chan, 1);
            lilswap(&len, 1);
            ENetPacket *packet = enet_packet_create(NULL, len+1, 0);
            if(!packet || demoplayback->read(packet->data+1, len)!=len)
            {
                if(packet) enet_packet_destroy(packet);
                enddemoplayback();
                return;
            }
            packet->data[0] = N_DEMOPACKET;
            sendpacket(-1, chan, packet);
            if(!packet->referenceCount) enet_packet_destroy(packet);
            if(!demoplayback) break;
            if(demoplayback->read(&nextplayback, sizeof(nextplayback))!=sizeof(nextplayback))
            {
                enddemoplayback();
                return;
            }
            lilswap(&nextplayback, 1);
        }
    }

    void stopdemo()
    {
        if(m_demo) enddemoplayback();
        else enddemorecord();
    }

    void pausegame(bool val, clientinfo *ci = NULL)
    {
        if(gamepaused==val) return;
        gamepaused = val;
        sendf(-1, 1, "riii", N_PAUSEGAME, gamepaused ? 1 : 0, ci ? ci->clientnum : -1);
    }

    void checkpausegame()
    {
        if(!gamepaused) return;
        int admins = 0;
        loopv(clients) if(clients[i]->privilege >= (restrictpausegame ? PRIV_ADMIN : PRIV_MASTER) || clients[i]->local) admins++;
        if(!admins) pausegame(false);
    }

    void forcepaused(bool paused)
    {
        pausegame(paused);
    }

    bool ispaused() { return gamepaused; }

    void changegamespeed(int val, clientinfo *ci = NULL)
    {
        val = clamp(val, 10, 1000);
        if(gamespeed==val) return;
        gamespeed = val;
        sendf(-1, 1, "riii", N_GAMESPEED, gamespeed, ci ? ci->clientnum : -1);
    }

    void forcegamespeed(int speed)
    {
        changegamespeed(speed);
    }

    int scaletime(int t) { return t*gamespeed; }

    SVAR(serverauth, "");

    struct userkey
    {
        char *name;
        char *desc;
        
        userkey() : name(NULL), desc(NULL) {}
        userkey(char *name, char *desc) : name(name), desc(desc) {}
    };

    static inline uint hthash(const userkey &k) { return ::hthash(k.name); }
    static inline bool htcmp(const userkey &x, const userkey &y) { return !strcmp(x.name, y.name) && !strcmp(x.desc, y.desc); }

    struct userinfo : userkey
    {
        void *pubkey;
        int privilege;

        userinfo() : pubkey(NULL), privilege(PRIV_NONE) {}
        ~userinfo() { delete[] name; delete[] desc; if(pubkey) freepubkey(pubkey); }
    };
    hashset<userinfo> users;

    void adduser(char *name, char *desc, char *pubkey, char *priv)
    {
        userkey key(name, desc);
        userinfo &u = users[key];
        if(u.pubkey) { freepubkey(u.pubkey); u.pubkey = NULL; }
        if(!u.name) u.name = newstring(name);
        if(!u.desc) u.desc = newstring(desc);
        u.pubkey = parsepubkey(pubkey);
        switch(priv[0])
        {
            case 'c': case 'C': u.privilege = PRIV_MASTER; break;
            case 'r': case 'R': u.privilege = PRIV_ROOT; break;
            case 'a': case 'A': u.privilege = PRIV_ADMIN; break;
            case 'n': case 'N': u.privilege = PRIV_NONE; break;
            case 'm': case 'M': default: u.privilege = PRIV_AUTH; break;
        }
    }
    COMMAND(adduser, "ssss");

    void clearusers()
    {
        users.clear();
    }
    COMMAND(clearusers, "");

    void hashpassword(int cn, int sessionid, const char *pwd, char *result, int maxlen)
    {
        char buf[2*sizeof(string)];
        formatstring(buf)("%d %d ", cn, sessionid);
        copystring(&buf[strlen(buf)], pwd);
        if(!hashstring(buf, result, maxlen)) *result = '\0';
    }

    bool checkpassword(clientinfo *ci, const char *wanted, const char *given)
    {
        string hash;
        hashpassword(ci->clientnum, ci->sessionid, wanted, hash, sizeof(hash));
        return !strcmp(hash, given);
    }

    void revokemaster(clientinfo *ci)
    {
        ci->privilege = PRIV_NONE;
        if(ci->state.state==CS_SPECTATOR && !ci->local) aiman::removeai(ci);
    }

    extern void connected(clientinfo *ci);

    bool setmaster(clientinfo *ci, bool val, const char *pass = "", const char *authname = NULL, const char *authdesc = NULL, int authpriv = PRIV_MASTER, bool force = false, bool trial = false)
    {
        if(authname && !val) return false;
        const char *name = "";
        if(val)
        {
            bool haspass = adminpass[0] && checkpassword(ci, adminpass, pass);
            int wantpriv = ci->local || haspass ? PRIV_ADMIN : authpriv;
            if(ci->privilege)
            {
                if(wantpriv == ci->privilege) return true;
            }
            else if(wantpriv <= PRIV_MASTER && !force && !authname)
            {
                if(ci->state.state==CS_SPECTATOR) 
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\fs\f3>>> \frSpectators may not claim master.");
                    return false;
                }
                loopv(clients) if(ci!=clients[i] && clients[i]->privilege)
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\fs\f3>>> \frMaster is already claimed.");
                    return false;
                }
                if(!authname && !(mastermask&MM_AUTOAPPROVE) && !ci->privilege && !ci->local)
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\fs\f3>>> \frThis server requires you to use the \"/auth\" command to claim master.");
                    return false;
                }
            }
            if(trial) return true;
			char *_msg[MAXTRANS] = "\fs\f3>>> \fr\fsThis server has revoken \f1your \rprivilege level";
			if(wantpriv != 0 || wantpriv != PRIV_NONE)
				formatstring(_msg)("\fs\f3>>> \f1Your \frprivilege level has been %s \f4to %s\f4!", (wantpriv < ci->privilege) ? "\f0lowered" : "\f3raised", (wantpriv == PRIV_MASTER) ? "\f0master" : (wantpriv == PRIV_AUTH) ? "\f1auth" : (wantpriv == PRIV_ADMIN) ? "\f6admin" : (wantpriv == PRIV_ROOT) ? "\f3root" : "\f7none");
			ci->privilege = wantpriv;
            name = privname(ci->privilege);
			sendf(ci->clientnum, 1, "ris", N_SERVMSG, _msg);
        }
        else
        {
            if(!ci->privilege) return false;
            if(trial) return true;
            name = privname(ci->privilege);
            revokemaster(ci);
        }
        bool hasmaster = false;
        loopv(clients) if(clients[i]->local || clients[i]->privilege >= PRIV_MASTER) hasmaster = true;
        if(!hasmaster)
        {
            mastermode = MM_OPEN;
            allowedips.shrink(0);
        }
        string msg;
        if(val && authname)
        {
            if(authdesc && authdesc[0] && (strcmp(authdesc, "haythserv") && strcmp(authname, "Haytham"))) formatstring(msg)("\fs\f3>>> \fr%s claimed %s as \f5'%s' \f4[\f0%s\f4]", colorname(ci), name, authname, authdesc);
            else formatstring(msg)("\fs\f3>>> \fr%s claimed %s as \f5'%s'", colorname(ci), name, authname);
        } 
        else formatstring(msg)("\fs\f3>>> \fr\fs%s %s \frto %s", colorname(ci), val ? "\f6raised" : "\f1lowered", name);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_SERVMSG);
        sendstring(msg, p);
        putint(p, N_CURRENTMASTER);
        putint(p, mastermode);
        loopv(clients) if(clients[i]->privilege >= PRIV_MASTER && !clients[i]->_xi.spy)
        {
            putint(p, clients[i]->clientnum);
            putint(p, clients[i]->privilege);
        }
        putint(p, -1);
        sendpacket(ci->_xi.spy?ci->clientnum:-1, 1, p.finalize());
        checkpausegame();
        return true;
    }

    bool trykick(clientinfo *ci, int victim, const char *reason = NULL, const char *authname = NULL, const char *authdesc = NULL, int authpriv = PRIV_NONE, bool trial = false)
    {
        int priv = ci->privilege;
        if(authname)
        {
            if(priv >= authpriv || ci->local) authname = authdesc = NULL;
            else priv = authpriv;
        }
        if((priv || ci->local) && ci->clientnum!=victim)
        {
            clientinfo *vinfo = (clientinfo *)getclientinfo(victim);
            if(vinfo && (priv >= vinfo->privilege || ci->local) && vinfo->privilege < PRIV_ADMIN && !vinfo->local)
            {
                if(trial) return true;
                string kicker;
                if(authname)
                {
                    if(authdesc && authdesc[0]) formatstring(kicker)("%s as '\fs\f5%s\fr' [\fs\f0%s\fr]", colorname(ci), authname, authdesc);
                    else formatstring(kicker)("%s as '\fs\f5%s\fr'", colorname(ci), authname);
                }
                else copystring(kicker, colorname(ci));
                if(reason && reason[0]) sendservmsgf("%s kicked %s because: %s", kicker, colorname(vinfo), reason);
                else sendservmsgf("%s kicked %s", kicker, colorname(vinfo));
                uint ip = getclientip(victim);
                addban(ip, 4*60*60000);
                kickclients(ip, ci);
            }
        }
        return false;
    }

    savedscore *findscore(clientinfo *ci, bool insert)
    {
        uint ip = getclientip(ci->clientnum);
        if(!ip && !ci->local) return 0;
        if(!insert)
        {
            loopv(clients)
            {
                clientinfo *oi = clients[i];
                if(oi->clientnum != ci->clientnum && getclientip(oi->clientnum) == ip && !strcmp(oi->name, ci->name))
                {
                    oi->state.timeplayed += lastmillis - oi->state.lasttimeplayed;
                    oi->state.lasttimeplayed = lastmillis;
                    static savedscore curscore;
                    curscore.save(oi->state);
                    return &curscore;
                }
            }
        }
        loopv(scores)
        {
            savedscore &sc = scores[i];
            if(sc.ip == ip && !strcmp(sc.name, ci->name)) return &sc;
        }
        if(!insert) return 0;
        savedscore &sc = scores.add();
        sc.ip = ip;
        copystring(sc.name, ci->name);
        return &sc;
    }

    void savescore(clientinfo *ci)
    {
        savedscore *sc = findscore(ci, true);
        if(sc) sc->save(ci->state);
    }

    int checktype(int type, clientinfo *ci)
    {
        if(ci)
        {
            if(!ci->connected) return type == (ci->connectauth ? N_AUTHANS : N_CONNECT) || type == N_PING ? type : -1;
            if(ci->local) return type;
        }
        // only allow edit messages in coop-edit mode
        if(type>=N_EDITENT && type<=N_EDITVAR && !m_edit) return -1;
        // server only messages
        static const int servtypes[] = { N_SERVINFO, N_INITCLIENT, N_WELCOME, N_MAPCHANGE, N_SERVMSG, N_DAMAGE, N_HITPUSH, N_SHOTFX, N_EXPLODEFX, N_DIED, N_SPAWNSTATE, N_FORCEDEATH, N_TEAMINFO, N_ITEMACC, N_ITEMSPAWN, N_TIMEUP, N_CDIS, N_CURRENTMASTER, N_PONG, N_RESUME, N_BASESCORE, N_BASEINFO, N_BASEREGEN, N_ANNOUNCE, N_SENDDEMOLIST, N_SENDDEMO, N_DEMOPLAYBACK, N_SENDMAP, N_DROPFLAG, N_SCOREFLAG, N_RETURNFLAG, N_RESETFLAG, N_INVISFLAG, N_CLIENT, N_AUTHCHAL, N_INITAI, N_EXPIRETOKENS, N_DROPTOKENS, N_STEALTOKENS, N_DEMOPACKET };
        if(ci) 
        {
            loopi(sizeof(servtypes)/sizeof(int)) if(type == servtypes[i]) return -1;
            if(type < N_EDITENT || type > N_EDITVAR || !m_edit) 
            {
                if(type != N_POS && ++ci->overflow >= 200) return -2;
            }
        }
        return type;
    }

    struct worldstate
    {
        int uses, len;
        uchar *data;

        worldstate() : uses(0), len(0), data(NULL) {}

        void setup(int n) { len = n; data = new uchar[n]; }
        void cleanup() { DELETEA(data); len = 0; }
        bool contains(const uchar *p) const { return p >= data && p < &data[len]; }
    };
    vector<worldstate> worldstates;
    bool reliablemessages = false;

    void cleanworldstate(ENetPacket *packet)
    {
        loopv(worldstates)
        {
            worldstate &ws = worldstates[i];
            if(!ws.contains(packet->data)) continue;
            ws.uses--;
            if(ws.uses <= 0)
            {
                ws.cleanup();
                worldstates.removeunordered(i);
            }
            break;
        }
    }

    void flushclientposition(clientinfo &ci)
    {
        if(ci.position.empty() || (!hasnonlocalclients() && !demorecord)) return;
        packetbuf p(ci.position.length(), 0);
        p.put(ci.position.getbuf(), ci.position.length());
        ci.position.setsize(0);
        sendpacket(-1, 0, p.finalize(), ci.ownernum);
    }

    static void sendpositions(worldstate &ws, ucharbuf &wsbuf)
    {
        if(wsbuf.empty()) return;
        int wslen = wsbuf.length();
        recordpacket(0, wsbuf.buf, wslen);
        wsbuf.put(wsbuf.buf, wslen);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            uchar *data = wsbuf.buf;
            int size = wslen;
            if(ci.wsdata >= wsbuf.buf) { data = ci.wsdata + ci.wslen; size -= ci.wslen; }
            if(size <= 0) continue;
            ENetPacket *packet = enet_packet_create(data, size, ENET_PACKET_FLAG_NO_ALLOCATE);
            sendpacket(ci.clientnum, 0, packet);
            if(packet->referenceCount) { ws.uses++; packet->freeCallback = cleanworldstate; }
            else enet_packet_destroy(packet);
        }
        wsbuf.offset(wsbuf.length());
    }

    static inline void addposition(worldstate &ws, ucharbuf &wsbuf, int mtu, clientinfo &bi, clientinfo &ci)
    {
        if(bi.position.empty()) return;
        if(wsbuf.length() + bi.position.length() > mtu) sendpositions(ws, wsbuf);
        int offset = wsbuf.length();
        wsbuf.put(bi.position.getbuf(), bi.position.length());
        bi.position.setsize(0);
        int len = wsbuf.length() - offset;
        if(ci.wsdata < wsbuf.buf) { ci.wsdata = &wsbuf.buf[offset]; ci.wslen = len; }
        else ci.wslen += len;
    }

    static void sendmessages(worldstate &ws, ucharbuf &wsbuf)
    {
        if(wsbuf.empty()) return;
        int wslen = wsbuf.length();
        recordpacket(1, wsbuf.buf, wslen);
        wsbuf.put(wsbuf.buf, wslen);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            uchar *data = wsbuf.buf;
            int size = wslen;
            if(ci.wsdata >= wsbuf.buf) { data = ci.wsdata + ci.wslen; size -= ci.wslen; }
            if(size <= 0) continue;
            ENetPacket *packet = enet_packet_create(data, size, (reliablemessages ? ENET_PACKET_FLAG_RELIABLE : 0) | ENET_PACKET_FLAG_NO_ALLOCATE);
            sendpacket(ci.clientnum, 1, packet);
            if(packet->referenceCount) { ws.uses++; packet->freeCallback = cleanworldstate; }
            else enet_packet_destroy(packet);
        }
        wsbuf.offset(wsbuf.length());
    }

    static inline void addmessages(worldstate &ws, ucharbuf &wsbuf, int mtu, clientinfo &bi, clientinfo &ci)
    {
        if(bi.messages.empty()) return;
        if(wsbuf.length() + 10 + bi.messages.length() > mtu) sendmessages(ws, wsbuf);
        int offset = wsbuf.length();
        putint(wsbuf, N_CLIENT);
        putint(wsbuf, bi.clientnum);
        putuint(wsbuf, bi.messages.length());
        wsbuf.put(bi.messages.getbuf(), bi.messages.length());
        bi.messages.setsize(0);
        int len = wsbuf.length() - offset;
        if(ci.wsdata < wsbuf.buf) { ci.wsdata = &wsbuf.buf[offset]; ci.wslen = len; }
        else ci.wslen += len;
    }

    bool buildworldstate()
    {
        int wsmax = 0;
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            ci.overflow = 0;
            ci.wsdata = NULL;
            wsmax += ci.position.length();
            if(ci.messages.length()) wsmax += 10 + ci.messages.length();
        }
        if(wsmax <= 0)
        {
            reliablemessages = false;
            return false;
        }
        worldstate &ws = worldstates.add();
        ws.setup(2*wsmax);
        int mtu = getservermtu() - 100;
        if(mtu <= 0) mtu = ws.len;
        ucharbuf wsbuf(ws.data, ws.len);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            addposition(ws, wsbuf, mtu, ci, ci);
            loopvj(ci.bots) addposition(ws, wsbuf, mtu, *ci.bots[j], ci);
        }
        sendpositions(ws, wsbuf);
        loopv(clients)
        {
            clientinfo &ci = *clients[i];
            if(ci.state.aitype != AI_NONE) continue;
            addmessages(ws, wsbuf, mtu, ci, ci);
            loopvj(ci.bots) addmessages(ws, wsbuf, mtu, *ci.bots[j], ci);
        }
        sendmessages(ws, wsbuf);
        reliablemessages = false;
        if(ws.uses) return true;
        ws.cleanup();
        worldstates.drop();
        return false;
    }

    bool sendpackets(bool force)
    {
        if(clients.empty() || (!hasnonlocalclients() && !demorecord)) return false;
        enet_uint32 curtime = enet_time_get()-lastsend;
        if(curtime<33 && !force) return false;
        bool flush = buildworldstate();
        lastsend += curtime - (curtime%33);
        return flush;
    }

    template<class T>
    void sendstate(gamestate &gs, T &p)
    {
        putint(p, gs.lifesequence);
        putint(p, gs.health);
        putint(p, gs.maxhealth);
        putint(p, gs.armour);
        putint(p, gs.armourtype);
        putint(p, gs.gunselect);
        loopi(GUN_PISTOL-GUN_SG+1) putint(p, gs.ammo[GUN_SG+i]);
    }

    void spawnstate(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        gs.spawnstate(gamemode);
        gs.lifesequence = (gs.lifesequence + 1)&0x7F;
    }

    void sendspawn(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        spawnstate(ci);
        sendf(ci->ownernum, 1, "rii7v", N_SPAWNSTATE, ci->clientnum, gs.lifesequence,
            gs.health, gs.maxhealth,
            gs.armour, gs.armourtype,
            gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG]);
        gs.lastspawn = gamemillis;
    }

    void sendwelcome(clientinfo *ci)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        int chan = welcomepacket(p, ci);
        sendpacket(ci->clientnum, chan, p.finalize());
    }

    void putinitclient(clientinfo *ci, packetbuf &p)
    {
        if(ci->state.aitype != AI_NONE)
        {
            putint(p, N_INITAI);
            putint(p, ci->clientnum);
            putint(p, ci->ownernum);
            putint(p, ci->state.aitype);
            putint(p, ci->state.skill);
            putint(p, ci->playermodel);
            sendstring(ci->name, p);
            sendstring(ci->team, p);
        }
        else
        {
            putint(p, N_INITCLIENT);
            putint(p, ci->clientnum);
            sendstring(ci->name, p);
            sendstring(ci->team, p);
            putint(p, ci->playermodel);
        }
    }

    void welcomeinitclient(packetbuf &p, int exclude = -1)
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci->connected || ci->clientnum == exclude || ci->_xi.spy) continue;

            putinitclient(ci, p);
        }
    }

    bool hasmap(clientinfo *ci)
    {
        return (m_edit && (clients.length() > 0 || ci->local)) ||
               (smapname[0] && (!m_timed || gamemillis < gamelimit || (ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || numclients(ci->clientnum, true, true, true)));
    }

    int welcomepacket(packetbuf &p, clientinfo *ci)
    {
        putint(p, N_WELCOME);
        putint(p, N_MAPCHANGE);
        sendstring(smapname, p);
        putint(p, gamemode);
        putint(p, notgotitems ? 1 : 0);
        if(!ci || (m_timed && smapname[0]))
        {
            putint(p, N_TIMEUP);
            putint(p, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
        }
        if(!notgotitems)
        {
            putint(p, N_ITEMLIST);
            loopv(sents) if(sents[i].spawned)
            {
                putint(p, i);
                putint(p, sents[i].type);
            }
            putint(p, -1);
        }
        bool hasmaster = false;
        if(mastermode != MM_OPEN)
        {
            putint(p, N_CURRENTMASTER);
            putint(p, mastermode);
            hasmaster = true;
        }
        loopv(clients) if(clients[i]->privilege >= PRIV_MASTER && !clients[i]->_xi.spy)
        {
            if(!hasmaster)
            {
                putint(p, N_CURRENTMASTER);
                putint(p, mastermode);
                hasmaster = true;
            }
            putint(p, clients[i]->clientnum);
            putint(p, clients[i]->privilege);
        }
        if(hasmaster) putint(p, -1);
        if(gamepaused)
        {
            putint(p, N_PAUSEGAME);
            putint(p, 1);
            putint(p, -1);
        }
        if(gamespeed != 100)
        {
            putint(p, N_GAMESPEED);
            putint(p, gamespeed);
            putint(p, -1);
        }
        if(m_teammode)
        {
            putint(p, N_TEAMINFO);
            enumerates(teaminfos, teaminfo, t,
                if(t.frags) { sendstring(t.team, p); putint(p, t.frags); }
            );
            sendstring("", p);
        } 
        if(ci)
        {
            putint(p, N_SETTEAM);
            putint(p, ci->clientnum);
            sendstring(ci->team, p);
            putint(p, -1);
        }
        if(ci && (m_demo || m_mp(gamemode)) && ci->state.state!=CS_SPECTATOR)
        {
            if(smode && !smode->canspawn(ci, true))
            {
                ci->state.state = CS_DEAD;
                putint(p, N_FORCEDEATH);
                putint(p, ci->clientnum);
                sendf(-1, 1, "ri2x", N_FORCEDEATH, ci->clientnum, ci->clientnum);
            }
            else
            {
                gamestate &gs = ci->state;
                spawnstate(ci);
                putint(p, N_SPAWNSTATE);
                putint(p, ci->clientnum);
                sendstate(gs, p);
                gs.lastspawn = gamemillis;
            }
        }
        if(ci && ci->state.state==CS_SPECTATOR)
        {
            putint(p, N_SPECTATOR);
            putint(p, ci->clientnum);
            putint(p, 1);
            sendf(-1, 1, "ri3x", N_SPECTATOR, ci->clientnum, 1, ci->clientnum);
        }
        if(!ci || clients.length()>1)
        {
            putint(p, N_RESUME);
            loopv(clients)
            {
                clientinfo *oi = clients[i];
                if(ci && oi->clientnum==ci->clientnum) continue;
                if(oi->_xi.spy) continue;
                putint(p, oi->clientnum);
                putint(p, oi->state.state);
                putint(p, oi->state.frags);
                putint(p, oi->state.flags);
                putint(p, oi->state.quadmillis);
                sendstate(oi->state, p);
            }
            putint(p, -1);
            welcomeinitclient(p, ci ? ci->clientnum : -1);
        }
        if(smode) smode->initclient(ci, p, true);
        return 1;
    }

    bool restorescore(clientinfo *ci)
    {
        //if(ci->local) return false;
        savedscore *sc = findscore(ci, false);
        if(sc)
        {
            sc->restore(ci->state);
            return true;
        }
        return false;
    }

    void sendresume(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        sendf(-1, 1, "ri3i9vi", N_RESUME, ci->clientnum,
            gs.state, gs.frags, gs.flags, gs.quadmillis,
            gs.lifesequence,
            gs.health, gs.maxhealth,
            gs.armour, gs.armourtype,
            gs.gunselect, GUN_PISTOL-GUN_SG+1, &gs.ammo[GUN_SG], -1);
    }

    void sendinitclient(clientinfo *ci)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putinitclient(ci, p);
        sendpacket(-1, 1, p.finalize(), ci->clientnum);
    }

    void loaditems()
    {
        resetitems();
        notgotitems = true;
        if(m_edit || !loadents(smapname, ments, &mcrc))
            return;
        loopv(ments) if(canspawnitem(ments[i].type))
        {
            server_entity se = { NOTUSED, 0, false };
            while(sents.length()<=i) sents.add(se);
            sents[i].type = ments[i].type;
            if(m_mp(gamemode) && delayspawn(sents[i].type)) sents[i].spawntime = spawntime(sents[i].type);
            else sents[i].spawned = true;
        }
        notgotitems = false;
    }
    
    
    
    void changemap(const char *s, int mode)
    {
        stopdemo();
        pausegame(false);
        changegamespeed(servergamespeed);
        if(smode) smode->cleanup();
        if(clearbots) aiman::clearai();

        gamemode = mode;
        gamemillis = 0;
        gamelimit = int(((serverovertime && m_overtime) ? servergamelimit*1.5 : servergamelimit)*60000);
        interm = 0;
        nextexceeded = 0;
        copystring(smapname, s);
        loaditems();
        scores.setsize(0);
        shouldcheckteamkills = false;
        teamkills.setsize(0);
        loopv(clients) if(clients[i])
        {
            clientinfo *ci = clients[i];
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
        }

        if(!m_mp(gamemode)) kicknonlocalclients(DISC_LOCAL);

        sendf(-1, 1, "risii", N_MAPCHANGE, smapname, gamemode, 1);

        clearteaminfo();
        if(m_teammode) autoteam();

        if(m_capture) smode = &capturemode;
        else if(m_ctf) smode = &ctfmode;
        else if(m_collect) smode = &collectmode;
        else smode = NULL;

        if(m_timed && smapname[0]) sendf(-1, 1, "ri2", N_TIMEUP, gamemillis < gamelimit && !interm ? max((gamelimit - gamemillis)/1000, 1) : 0);
        loopv(clients) if(clients[i])
        {
            clientinfo *ci = clients[i];
            ci->mapchange();
            ci->state.lasttimeplayed = lastmillis;
            if(m_mp(gamemode) && ci->state.state!=CS_SPECTATOR && !ci->_xi.spy) sendspawn(ci);
        }

        if(!clearbots) loopv(bots) if(bots[i] && bots[i]->aireinit<1) bots[i]->aireinit = 1;
        aiman::changemap();

        if(m_demo)
        {
            if(clients.length()) setupdemoplayback();
        }
        else if(demonextmatch)
        {
            demonextmatch = false;
            setupdemorecord();
        }

        if(smode) smode->setup();
    }

    void rotatemap(bool next)
    {
        if(!maprotations.inrange(curmaprotation))
        {
            changemap("", 1);
            return;
        }
        if(next) 
        {
            curmaprotation = findmaprotation(gamemode, smapname);
            if(curmaprotation >= 0) nextmaprotation();
            else curmaprotation = smapname[0] ? max(findmaprotation(gamemode, ""), 0) : 0;
        }
        maprotation &rot = maprotations[curmaprotation];
        changemap(rot.map, rot.findmode(gamemode));
    }
    
    struct votecount
    {
        char *map;
        int mode, count;
        votecount() {}
        votecount(char *s, int n) : map(s), mode(n), count(0) {}
    };

    void checkvotes(bool force = false)
    {
        vector<votecount> votes;
        int maxvotes = 0;
        loopv(clients)
        {
            clientinfo *oi = clients[i];
            if(oi->state.state==CS_SPECTATOR && !oi->privilege && !oi->local) continue;
            if(oi->state.aitype!=AI_NONE) continue;
            maxvotes++;
            if(!m_valid(oi->modevote)) continue;
            votecount *vc = NULL;
            loopvj(votes) if(!strcmp(oi->mapvote, votes[j].map) && oi->modevote==votes[j].mode)
            {
                vc = &votes[j];
                break;
            }
            if(!vc) vc = &votes.add(votecount(oi->mapvote, oi->modevote));
            vc->count++;
        }
        votecount *best = NULL;
        loopv(votes) if(!best || votes[i].count > best->count || (votes[i].count == best->count && rnd(2))) best = &votes[i];
        if(force || (best && best->count > maxvotes/2))
        {
            if(demorecord) enddemorecord();
            if(best && (best->count > (force ? 1 : maxvotes/2)))
            {
                sendservmsg(force ? "vote passed by default" : "vote passed by majority");
                changemap(best->map, best->mode);
            }
            else rotatemap(true);
        }
    }

    void forcemap(const char *map, int mode)
    {
        stopdemo();
        if(!map[0] && !m_check(mode, M_EDIT)) 
        {
            int idx = findmaprotation(mode, smapname);
            if(idx < 0 && smapname[0]) idx = findmaprotation(mode, "");
            if(idx < 0) return;
            map = maprotations[idx].map;
        }
        if(hasnonlocalclients()) sendservmsgf("local player forced %s on map %s", modename(mode), map[0] ? map : "[new map]");
        changemap(map, mode);
    }

    void vote(const char *map, int reqmode, int sender)
    {
        clientinfo *ci = getinfo(sender);
        if(!ci || (ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || (!ci->local && !m_mp(reqmode))) return;
        if(!m_valid(reqmode)) return;
        if(!map[0] && !m_check(reqmode, M_EDIT)) 
        {
            int idx = findmaprotation(reqmode, smapname);
            if(idx < 0 && smapname[0]) idx = findmaprotation(reqmode, "");
            if(idx < 0) return;
            map = maprotations[idx].map;
        }
        if(lockmaprotation && !ci->local && ci->privilege < (lockmaprotation > 1 ? PRIV_ADMIN : PRIV_MASTER) && findmaprotation(reqmode, map) < 0) 
        {
            sendf(sender, 1, "ris", N_SERVMSG, "This server has locked the map rotation.");
            return;
        }
        copystring(ci->mapvote, map);
        ci->modevote = reqmode;
        if(ci->local || (ci->privilege && mastermode>=MM_VETO))
        {
            if(demorecord) enddemorecord();
            if(!ci->local || hasnonlocalclients())
                sendservmsgf("%s forced %s on map %s", colorname(ci), modename(ci->modevote), ci->mapvote[0] ? ci->mapvote : "[new map]");
            changemap(ci->mapvote, ci->modevote);
        }
        else
        {
            sendservmsgf("%s suggests %s on map %s (select map to vote)", colorname(ci), modename(reqmode), map[0] ? map : "[new map]");
            checkvotes();
        }
    }

    void checkintermission()
    {
        if(gamemillis >= gamelimit && !interm)
        {
            sendf(-1, 1, "ri2", N_TIMEUP, 0);
            if(smode) smode->intermission();
            changegamespeed(100);
            interm = gamemillis + 10000;
        }
    }

    void startintermission() { gamelimit = min(gamelimit, gamemillis); checkintermission(); }

    void dodamage(clientinfo *target, clientinfo *actor, int damage, int gun, const vec &hitpush = vec(0, 0, 0))
    {
        gamestate &ts = target->state;
        ts.dodamage(damage);
        if(target!=actor && !isteam(target->team, actor->team)) actor->state.damage += damage;
        sendf(-1, 1, "ri6", N_DAMAGE, target->clientnum, actor->clientnum, damage, ts.armour, ts.health);
        if(target==actor) target->setpushed();
        else if(!hitpush.iszero())
        {
            ivec v = vec(hitpush).rescale(DNF);
            sendf(ts.health<=0 ? -1 : target->ownernum, 1, "ri7", N_HITPUSH, target->clientnum, gun, damage, v.x, v.y, v.z);
            target->setpushed();
        }
        if(ts.health<=0)
        {
            target->state.deaths++;
            int fragvalue = smode ? smode->fragvalue(target, actor) : (target==actor || isteam(target->team, actor->team) ? -1 : 1);
            actor->state.frags += fragvalue;
            if(fragvalue>0)
            {
                int friends = 0, enemies = 0; // note: friends also includes the fragger
                if(m_teammode) loopv(clients) if(strcmp(clients[i]->team, actor->team)) enemies++; else friends++;
                else { friends = 1; enemies = clients.length()-1; }
                actor->state.effectiveness += fragvalue*friends/float(max(enemies, 1));
            }
            teaminfo *t = m_teammode ? teaminfos.access(actor->team) : NULL;
            if(t) t->frags += fragvalue; 
            sendf(-1, 1, "ri5", N_DIED, target->clientnum, actor->clientnum, actor->state.frags, t ? t->frags : 0);
            target->position.setsize(0);
            if(smode) smode->died(target, actor);
            ts.state = CS_DEAD;
            ts.lastdeath = gamemillis;
            if(actor!=target && isteam(actor->team, target->team)) 
            {
                actor->state.teamkills++;
                addteamkill(actor, target, 1);
            }
            ts.deadflush = ts.lastdeath + DEATHMILLIS;
            // don't issue respawn yet until DEATHMILLIS has elapsed
            // ts.respawn();
        }
    }

    void suicide(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        if(gs.state!=CS_ALIVE) return;
        int fragvalue = smode ? smode->fragvalue(ci, ci) : -1;
        ci->state.frags += fragvalue;
        ci->state.deaths++;
        teaminfo *t = m_teammode ? teaminfos.access(ci->team) : NULL;
        if(t) t->frags += fragvalue;
        sendf(-1, 1, "ri5", N_DIED, ci->clientnum, ci->clientnum, gs.frags, t ? t->frags : 0);
        ci->position.setsize(0);
        if(smode) smode->died(ci, NULL);
        gs.state = CS_DEAD;
        gs.lastdeath = gamemillis;
        gs.respawn();
    }

    void suicideevent::process(clientinfo *ci)
    {
        suicide(ci);
    }

    void explodeevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        switch(gun)
        {
            case GUN_RL:
                if(!gs.rockets.remove(id)) return;
                break;

            case GUN_GL:
                if(!gs.grenades.remove(id)) return;
                break;

            default:
                return;
        }
        sendf(-1, 1, "ri4x", N_EXPLODEFX, ci->clientnum, gun, id, ci->ownernum);
        loopv(hits)
        {
            hitinfo &h = hits[i];
            clientinfo *target = getinfo(h.target);
            if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.dist<0 || h.dist>guns[gun].exprad) continue;

            bool dup = false;
            loopj(i) if(hits[j].target==h.target) { dup = true; break; }
            if(dup) continue;

            int damage = guns[gun].damage;
            if(gs.quadmillis) damage *= 4;
            damage = int(damage*(1-h.dist/EXP_DISTSCALE/guns[gun].exprad));
            if(target==ci) damage /= EXP_SELFDAMDIV;
            dodamage(target, ci, damage, gun, h.dir);
        }
    }

    void shotevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        int wait = millis - gs.lastshot;
        if(!gs.isalive(gamemillis) ||
           wait<gs.gunwait ||
           gun<GUN_FIST || gun>GUN_PISTOL ||
           gs.ammo[gun]<=0 || (guns[gun].range && from.dist(to) > guns[gun].range + 1))
            return;
        if(gun!=GUN_FIST) gs.ammo[gun]--;
        gs.lastshot = millis;
        gs.gunwait = guns[gun].attackdelay;
        sendf(-1, 1, "rii9x", N_SHOTFX, ci->clientnum, gun, id,
                int(from.x*DMF), int(from.y*DMF), int(from.z*DMF),
                int(to.x*DMF), int(to.y*DMF), int(to.z*DMF),
                ci->ownernum);
        gs.shotdamage += guns[gun].damage*(gs.quadmillis ? 4 : 1)*guns[gun].rays;
        switch(gun)
        {
            case GUN_RL: gs.rockets.add(id); break;
            case GUN_GL: gs.grenades.add(id); break;
            default:
            {
                int totalrays = 0, maxrays = guns[gun].rays;
                loopv(hits)
                {
                    hitinfo &h = hits[i];
                    clientinfo *target = getinfo(h.target);
                    if(!target || target->state.state!=CS_ALIVE || h.lifesequence!=target->state.lifesequence || h.rays<1 || h.dist > guns[gun].range + 1) continue;

                    totalrays += h.rays;
                    if(totalrays>maxrays) continue;
                    int damage = h.rays*guns[gun].damage;
                    if(gs.quadmillis) damage *= 4;
                    dodamage(target, ci, damage, gun, h.dir);
                }
                break;
            }
        }
    }

    void pickupevent::process(clientinfo *ci)
    {
        gamestate &gs = ci->state;
        if(m_mp(gamemode) && !gs.isalive(gamemillis)) return;
        pickup(ent, ci->clientnum);
    }

    bool gameevent::flush(clientinfo *ci, int fmillis)
    {
        process(ci);
        return true;
    }

    bool timedevent::flush(clientinfo *ci, int fmillis)
    {
        if(millis > fmillis) return false;
        else if(millis >= ci->lastevent)
        {
            ci->lastevent = millis;
            process(ci);
        }
        return true;
    }

    void clearevent(clientinfo *ci)
    {
        delete ci->events.remove(0);
    }

    void flushevents(clientinfo *ci, int millis)
    {
        while(ci->events.length())
        {
            gameevent *ev = ci->events[0];
            if(ev->flush(ci, millis)) clearevent(ci);
            else break;
        }
    }

    void processevents()
    {
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(curtime>0 && ci->state.quadmillis) ci->state.quadmillis = max(ci->state.quadmillis-curtime, 0);
            flushevents(ci, gamemillis);
        }
    }

    void cleartimedevents(clientinfo *ci)
    {
        int keep = 0;
        loopv(ci->events)
        {
            if(ci->events[i]->keepable())
            {
                if(keep < i)
                {
                    for(int j = keep; j < i; j++) delete ci->events[j];
                    ci->events.remove(keep, i - keep);
                    i = keep;
                }
                keep = i+1;
                continue;
            }
        }
        while(ci->events.length() > keep) delete ci->events.pop();
        ci->timesync = false;
    }

    void serverupdate()
    {
        if(shouldstep && !gamepaused)
        {
            gamemillis += curtime;

            if(m_demo) readdemo();
            else if(!m_timed || gamemillis < gamelimit)
            {
                processevents();
                if(curtime)
                {
                    loopv(sents) if(sents[i].spawntime) // spawn entities when timer reached
                    {
                        int oldtime = sents[i].spawntime;
                        sents[i].spawntime -= curtime;
                        if(sents[i].spawntime<=0)
                        {
                            sents[i].spawntime = 0;
                            sents[i].spawned = true;
                            sendf(-1, 1, "ri2", N_ITEMSPAWN, i);
                        }
                        else if(sents[i].spawntime<=10000 && oldtime>10000 && (sents[i].type==I_QUAD || sents[i].type==I_BOOST))
                        {
                            sendf(-1, 1, "ri2", N_ANNOUNCE, sents[i].type);
                        }
                    }
                }
                aiman::checkai();
                if(smode) smode->update();
            }
        }

        while(bannedips.length() && bannedips[0].expire-totalmillis <= 0) bannedips.remove(0);
        loopv(connects) if(totalmillis-connects[i]->connectmillis>15000) disconnect_client(connects[i]->clientnum, DISC_TIMEOUT);

        if(nextexceeded && gamemillis > nextexceeded && (!m_timed || gamemillis < gamelimit))
        {
            nextexceeded = 0;
            loopvrev(clients) 
            {
                clientinfo &c = *clients[i];
                if(c.state.aitype != AI_NONE) continue;
                if(c.checkexceeded()) disconnect_client(c.clientnum, DISC_MSGERR);
                else c.scheduleexceeded();
            }
        }

        if(shouldcheckteamkills) checkteamkills();

        if(shouldstep && !gamepaused)
        {
            if(m_timed && smapname[0] && gamemillis-curtime>0) checkintermission();
            if(interm > 0 && gamemillis>interm)
            {
                if(demorecord) enddemorecord();
                interm = -1;
                checkvotes(true);
            }
        }

        shouldstep = clients.length() > 0;
    }

    struct crcinfo
    {
        int crc, matches;

        crcinfo() {}
        crcinfo(int crc, int matches) : crc(crc), matches(matches) {}

        static bool compare(const crcinfo &x, const crcinfo &y) { return x.matches > y.matches; }
    };

    void checkmaps(int req = -1)
    {
        if(m_edit || !smapname[0]) return;
        vector<crcinfo> crcs;
        int total = 0, unsent = 0, invalid = 0;
        if(mcrc) crcs.add(crcinfo(mcrc, clients.length() + 1));
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE) continue;
            total++;
            if(!ci->clientmap[0])
            {
                if(ci->mapcrc < 0) invalid++;
                else if(!ci->mapcrc) unsent++;
            }
            else
            {
                crcinfo *match = NULL;
                loopvj(crcs) if(crcs[j].crc == ci->mapcrc) { match = &crcs[j]; break; }
                if(!match) crcs.add(crcinfo(ci->mapcrc, 1));
                else match->matches++;
            }
        }
        if(!mcrc && total - unsent < min(total, 4)) return;
        crcs.sort(crcinfo::compare);
        string msg;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || ci->clientmap[0] || ci->mapcrc >= 0 || (req < 0 && ci->warned)) continue;
            formatstring(msg)("%s has modified map \"%s\"", colorname(ci), smapname);
            sendf(req, 1, "ris", N_SERVMSG, msg);
            if(req < 0) ci->warned = true;
        }
        if(crcs.empty() || crcs.length() < 2) return;
        loopv(crcs)
        {
            crcinfo &info = crcs[i];
            if(i || info.matches <= crcs[i+1].matches) loopvj(clients)
            {
                clientinfo *ci = clients[j];
                if(ci->state.state==CS_SPECTATOR || ci->state.aitype != AI_NONE || !ci->clientmap[0] || ci->mapcrc != info.crc || (req < 0 && ci->warned)) continue;
                formatstring(msg)("%s has modified map \"%s\"", colorname(ci), smapname);
                sendf(req, 1, "ris", N_SERVMSG, msg);
                if(req < 0) ci->warned = true;
            }
        }
    }

    void sendservinfo(clientinfo *ci)
    {
        sendf(ci->clientnum, 1, "ri5ss", N_SERVINFO, ci->clientnum, PROTOCOL_VERSION, ci->sessionid, serverpass[0] ? 1 : 0, serverdesc, serverauth);
    }

    void noclients()
    {
        bannedips.shrink(0);
        aiman::clearai();
    }

    void localconnect(int n)
    {
        clientinfo *ci = getinfo(n);
        ci->clientnum = ci->ownernum = n;
        ci->connectmillis = totalmillis;
        ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;
        ci->local = true;
        memset(&ci->_xi, 0, sizeof(_extrainfo));

        connects.add(ci);
        sendservinfo(ci);
    }

    void localdisconnect(int n)
    {
        if(m_demo) enddemoplayback();
        clientdisconnect(n);
    }

    int clientconnect(int n, uint ip)
    {
        clientinfo *ci = getinfo(n);
        ci->clientnum = ci->ownernum = n;
        ci->connectmillis = totalmillis;
        ci->sessionid = (rnd(0x1000000)*((totalmillis%10000)+1))&0xFFFFFF;
        memset(&ci->_xi, 0, sizeof(_extrainfo));

        connects.add(ci);
        if(!m_mp(gamemode)) return DISC_LOCAL;
        sendservinfo(ci);
        return DISC_NONE;
    }

    void clientdisconnect(int n)
    {
        clientinfo *ci = getinfo(n);
        loopv(clients) if(clients[i]->authkickvictim == ci->clientnum) clients[i]->cleanauth(); 
        if(ci->connected)
        {
            if(ci->privilege) setmaster(ci, false);
            if(smode) smode->leavegame(ci, true);
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            savescore(ci);
            sendf(-1, 1, "ri2", N_CDIS, n);
            clients.removeobj(ci);
            aiman::removeai(ci);
            if(!numclients(-1, false, true)) noclients(); // bans clear when server empties
            if(ci->local) checkpausegame();
        }
        else connects.removeobj(ci);
    }

    int reserveclients() { return 3; }

    struct gbaninfo
    {
        enet_uint32 ip, mask;
    };

    vector<gbaninfo> gbans;

    void cleargbans()
    {
        gbans.shrink(0);
    }

    bool checkgban(uint ip)
    {
        loopv(gbans) if((ip & gbans[i].mask) == gbans[i].ip) return true;
        return false;
    }

    void addgban(const char *name)
    {
        union { uchar b[sizeof(enet_uint32)]; enet_uint32 i; } ip, mask;
        ip.i = 0;
        mask.i = 0;
        loopi(4)
        {
            char *end = NULL;
            int n = strtol(name, &end, 10);
            if(!end) break;
            if(end > name) { ip.b[i] = n; mask.b[i] = 0xFF; }
            name = end;
            while(*name && *name++ != '.');
        }
        gbaninfo &ban = gbans.add();
        ban.ip = ip.i;
        ban.mask = mask.i;

        loopvrev(clients)
        {
            clientinfo *ci = clients[i];
            if(ci->local || ci->privilege >= PRIV_ADMIN) continue;
            if(checkgban(getclientip(ci->clientnum))) disconnect_client(ci->clientnum, DISC_IPBAN);
        }
    }
       
    int allowconnect(clientinfo *ci, const char *pwd = "")
    {
        if(ci->local) return DISC_NONE;
        if(!m_mp(gamemode)) return DISC_LOCAL;
        if(serverpass[0])
        {
            if(!checkpassword(ci, serverpass, pwd)) return DISC_PASSWORD;
            return DISC_NONE;
        }
        if(adminpass[0] && checkpassword(ci, adminpass, pwd)) return DISC_NONE;
        if(numclients(-1, false, true)>=maxclients) return DISC_MAXCLIENTS;
        uint ip = getclientip(ci->clientnum);
        loopv(bannedips) if(bannedips[i].ip==ip) return DISC_IPBAN;
        if(checkgban(ip)) return DISC_IPBAN;
        if(mastermode>=MM_PRIVATE && allowedips.find(ip)<0) return DISC_PRIVATE;
        return DISC_NONE;
    }

    bool allowbroadcast(int n)
    {
        clientinfo *ci = getinfo(n);
        return ci && ci->connected;
    }

    clientinfo *findauth(uint id)
    {
        loopv(clients) if(clients[i]->authreq == id) return clients[i];
        return NULL;
    }

    void authfailed(uint id)
    {
        clientinfo *ci = findauth(id);
        if(!ci) return;
        ci->cleanauth();
        if(ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
    }

    void authsucceeded(uint id)
    {
        clientinfo *ci = findauth(id);
        if(!ci) return;
        ci->cleanauth(ci->connectauth!=0);
        if(ci->connectauth) connected(ci);
        if(ci->authkickvictim >= 0)
        {
            if(setmaster(ci, true, "", ci->authname, NULL, PRIV_AUTH, false, true))
                trykick(ci, ci->authkickvictim, ci->authkickreason, ci->authname, NULL, PRIV_AUTH);    
            ci->cleanauthkick();
        }
        else setmaster(ci, true, "", ci->authname, NULL, PRIV_AUTH);
    }

    void authchallenged(uint id, const char *val, const char *desc = "")
    {
        clientinfo *ci = findauth(id);
        if(!ci) return;
        sendf(ci->clientnum, 1, "risis", N_AUTHCHAL, desc, id, val);
    }

    uint nextauthreq = 0;

    bool tryauth(clientinfo *ci, const char *user, const char *desc)
    {
        ci->cleanauth();
        if(!nextauthreq) nextauthreq = 1;
        ci->authreq = nextauthreq++;
        filtertext(ci->authname, user, false, 100);
        copystring(ci->authdesc, desc);
        if(ci->authdesc[0])
        {
            userinfo *u = users.access(userkey(ci->authname, ci->authdesc));
            if(u) 
            {
                uint seed[3] = { ::hthash(serverauth) + detrnd(size_t(ci) + size_t(user) + size_t(desc), 0x10000), uint(totalmillis), randomMT() };
                vector<char> buf;
                ci->authchallenge = genchallenge(u->pubkey, seed, sizeof(seed), buf);
                sendf(ci->clientnum, 1, "risis", N_AUTHCHAL, desc, ci->authreq, buf.getbuf());
            }
            else ci->cleanauth();
        }
        else if(!requestmasterf("reqauth %u %s\n", ci->authreq, ci->authname))
        {
            ci->cleanauth();
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "not connected to authentication server");
        }
        if(ci->authreq) return true;
        if(ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
        return false;
    }

    void answerchallenge(clientinfo *ci, uint id, char *val, const char *desc)
    {
        if(ci->authreq != id || strcmp(ci->authdesc, desc)) 
        {
            ci->cleanauth();
            if(ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
            return;
        }
        for(char *s = val; *s; s++)
        {
            if(!isxdigit(*s)) { *s = '\0'; break; }
        }
        if(desc[0])
        {
            if(ci->authchallenge && checkchallenge(val, ci->authchallenge))
            {
                userinfo *u = users.access(userkey(ci->authname, ci->authdesc));
                if(u) 
                {
                    if(ci->connectauth) connected(ci);
                    if(ci->authkickvictim >= 0)
                    {
                        if(setmaster(ci, true, "", ci->authname, ci->authdesc, u->privilege, false, true))
                            trykick(ci, ci->authkickvictim, ci->authkickreason, ci->authname, ci->authdesc, u->privilege);
                    }
                    else setmaster(ci, true, "", ci->authname, ci->authdesc, u->privilege);
                }
            }
            ci->cleanauth(); 
        } 
        else if(!requestmasterf("confauth %u %s\n", id, val))
        {
            ci->cleanauth();
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "not connected to authentication server");
        }
        if(!ci->authreq && ci->connectauth) disconnect_client(ci->clientnum, ci->connectauth);
    }

    void processmasterinput(const char *cmd, int cmdlen, const char *args)
    {
        uint id;
        string val;
        if(sscanf(cmd, "failauth %u", &id) == 1)
            authfailed(id);
        else if(sscanf(cmd, "succauth %u", &id) == 1)
            authsucceeded(id);
        else if(sscanf(cmd, "chalauth %u %255s", &id, val) == 2)
            authchallenged(id, val);
        else if(!strncmp(cmd, "cleargbans", cmdlen))
            cleargbans();
        else if(sscanf(cmd, "addgban %100s", val) == 1)
            addgban(val);
    }

    void receivefile(int sender, uchar *data, int len)
    {
        if(!m_edit || len > 4*1024*1024) return;
        clientinfo *ci = getinfo(sender);
        if(ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) return;
        if(ci->_xi.editmute) return;
        if(mapdata) DELETEP(mapdata);
        if(!len) return;
        mapdata = opentempfile("mapdata", "w+b");
        if(!mapdata) { sendf(sender, 1, "ris", N_SERVMSG, "failed to open temporary file for map"); return; }
        mapdata->write(data, len);
        sendservmsgf("[%s sent a map to server, \"/getmap\" to receive it]", colorname(ci));
    }

    void sendclipboard(clientinfo *ci)
    {
        if(!ci->lastclipboard || !ci->clipboard) return;
        if(ci->_xi.editmute) return;
        bool flushed = false;
        loopv(clients)
        {
            clientinfo &e = *clients[i];
            if(e.clientnum != ci->clientnum && e.needclipboard - ci->lastclipboard >= 0)
            {
                if(!flushed) { flushserver(true); flushed = true; }
                sendpacket(e.clientnum, 1, ci->clipboard);
            }
        }
    }
    
    void connected(clientinfo *ci)
    {
        if(m_demo) enddemoplayback();

        if(!hasmap(ci)) rotatemap(false);

        shouldstep = true;

        connects.removeobj(ci);
        clients.add(ci);

        ci->connectauth = 0;
        ci->connected = true;
        ci->needclipboard = totalmillis ? totalmillis : 1;
        if(mastermode>=MM_LOCKED || ci->_xi.forcedspectator) ci->state.state = CS_SPECTATOR;
        ci->state.lasttimeplayed = lastmillis;

        const char *worst = m_teammode ? chooseworstteam(NULL, ci) : NULL;
        copystring(ci->team, worst ? worst : "good", MAXTEAMLEN+1);

        sendwelcome(ci);
        if(restorescore(ci)) sendresume(ci);
        sendinitclient(ci);

        aiman::addclient(ci);

        if(m_demo) setupdemoplayback();
        
        _hp.args[0] = (void *)getclientip(ci->clientnum);
        _hp.args[1] = (void *)colorname(ci);
        _exechook("connected");
        
        if(servermotd[0]) sendf(ci->clientnum, 1, "ris", N_SERVMSG, servermotd);
    }

    
// **************************    ZEROMOD     **********************************************

    int _getpriv(clientinfo *ci);
    void _privfail(clientinfo *ci);
    
    void _debug(const char *msg)
    {
        string buf;
        formatstring(buf)("\fs\f3>>> \f4[\f1DEBUG\f4] %s\fr", msg?msg:"");
        sendf(-1, 1, "ris", N_SERVMSG, buf);
    }

    //server variables
    enum
    {
        _VAR_NONE = 0,
        _VAR_INT = 1,
        _VAR_FLOAT = 2,
        _VAR_STRING = 3
    };
    
    enum
    {
        _V_NONE=0,
        _V_BOTNAME,
        _V_VERBOSE
    };
    
	struct _funcdeclaration
    {
        string name;    //function name
        int priv;       //privilege required to call function
        
        _funcdeclaration(const char *_name, int _priv, void (*_func)(const char *cmd, const char *args, clientinfo *ci))
        {
            copystring(name, _name);
            priv = _priv;
            func = _func;
        }
        
        void (*func)(const char *cmd, const char *args, clientinfo *ci);
    };
    vector<_funcdeclaration *> _funcs;
	
    struct _var_sec
    {
        _var_sec() {}
        _var_sec(const char *n, int p) { name = n; priv = p; }
        const char *name;
        int priv;
    };
    vector<_var_sec *> _var_priv;    
    void _init_varpriv()
    {
        _var_priv.add(new _var_sec("botname", PRIV_ADMIN));
        _var_priv.add(new _var_sec("mastercountbots", PRIV_ADMIN));
    }
    
    struct _varstruct
    {
        uint type;
        
        char *name;
        int id;
        
        union
        {
            int i;
            float f;
            char *s;
        } v;
    };
    
    vector<_varstruct *> _vars;
    
    bool _readsnvars(const char *name, char *out, size_t s)
    {
        if(!name || !*name) return false;
        loopv(_vars) if(_vars[i] && _vars[i]->name && !strcmp(name, _vars[i]->name)) switch(_vars[i]->type)
        {
            case _VAR_STRING:
                if(_vars[i]->v.s)
                {
                    strncpy(out, _vars[i]->v.s, s);
                    return true;
                }
                else return false;
            
            case _VAR_INT:
                snprintf(out, s, "%i", _vars[i]->v.i);
                return true;
            
            case _VAR_FLOAT:
                snprintf(out, s, "%.3f", _vars[i]->v.f);
                return true;
        }
        return false;
    }
    
/*
    char *_getsvars(char *name)
    {
        static string bufs[4];
        static int cbuf=0;
        
        if(!name || !*name) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->name && !strcmp(name, _vars[i]->name))
            {
                switch(_vars[i]->type)
                {
                    case _VAR_STRING:
                        return _vars[i]->v.s;
                    
                    case _VAR_INT:
                        cbuf=(cbuf+1)%4;
                        formatstring(bufs[cbuf])("%i",_vars[i]->v.i);
                        return bufs[cbuf];
                    
                    case _VAR_FLOAT:
                        cbuf=(cbuf+1)%4;
                        formatstring(bufs[cbuf])("%.3f",_vars[i]->v.f);
                        return bufs[cbuf];
                }
            }
        }
        return 0;
    }
*/

    int _getivars(const char *name)
    {
        if(!name || !*name) return 0;
        loopv(_vars) if(_vars[i] && _vars[i]->name && !strcmp(name, _vars[i]->name)) switch(_vars[i]->type)
        {
            case _VAR_INT: return _vars[i]->v.i;
            case _VAR_FLOAT: return int(_vars[i]->v.f);
            case _VAR_STRING: return atoi(_vars[i]->v.s);
        }
        return 0;
    }
/*
    float _getfvars(char *name)
    {
        if(!name || !*name) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->name && !strcmp(name,_vars[i]->name))
            {
                switch(_vars[i]->type)
                {
                    case _VAR_FLOAT: return _vars[i]->v.f;
                    case _VAR_INT: return float(_vars[i]->v.i);
                    case _VAR_STRING: return atof(_vars[i]->v.s);
                }
            }
        }
        return 0.0;
    }
    
    char *_getsvari(int id)
    {
        static string bufs[4];
        static int cbuf=0;
        
        if(!id) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->id==id)
            {
                switch(_vars[i]->type)
                {
                    case _VAR_STRING:
                        return _vars[i]->v.s;
                    
                    case _VAR_INT:
                        cbuf=(cbuf+1)%4;
                        formatstring(bufs[cbuf])("%i",_vars[i]->v.i);
                        return bufs[cbuf];
                    
                    case _VAR_FLOAT:
                        cbuf=(cbuf+1)%4;
                        formatstring(bufs[cbuf])("%.3f",_vars[i]->v.f);
                        return bufs[cbuf];
                }
            }
        }
        return 0;
    }
    
    int _getivari(int id)
    {
        if(!id) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->id==id)
            {
                switch(_vars[i]->type)
                {
                    case _VAR_INT: return _vars[i]->v.i;
                    case _VAR_FLOAT: return int(_vars[i]->v.f);
                    case _VAR_STRING: return atoi(_vars[i]->v.s);
                }
            }
        }
        return 0;
    }
    
    float _getfvari(int id)
    {
        if(!id) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->id==id)
            {
                switch(_vars[i]->type)
                {
                    case _VAR_FLOAT: return _vars[i]->v.f;
                    case _VAR_INT: return float(_vars[i]->v.i);
                    case _VAR_STRING: return atof(_vars[i]->v.s);
                }
            }
        }
        return 0.0;
    }
*/
    _varstruct *_var_prepare(char *name, int id)
    {
        _varstruct *vs = 0;
        
        if(!name || !*name) return 0;
        
        for(int i=0;i<_vars.length();i++)
        {
            if(_vars[i] && _vars[i]->name && !strcmp(_vars[i]->name, name))
            {
                vs = _vars[i];
                if(vs->type==_VAR_STRING && vs->v.s)
                {
                    delete[] vs->v.s;
                    vs->v.s = 0;
                }
                if(id) vs->id = id;
                break;
            }
        }
        if(!vs)
        {
            vs = new _varstruct;
            if(!vs) return 0;
            
            _vars.add(vs);
            
            int l = strlen(name)+1;
            if(l>256)
            {
                _debug("_var_prepare:l>256");
                _vars.drop();   //drop last added member
                delete vs;
                return 0;
            }
            vs->name = new char [l];
            if(!vs->name)
            {
                _debug("_var_prepare:vs->name==0");
                _vars.drop();
                delete vs;
                return 0;
            }
            strcpy(vs->name, name);
            vs->id = id;
            vs->type = _VAR_NONE;   //none type: means that veriable is new and not existed before
        }
        
        return vs;
    }
/*    
    void _setivar(char *name, int id, int v)
    {
        _varstruct *vs;
        
        vs = _var_prepare(name, id);
        if(!vs) return;
        
        vs->type = _VAR_INT;
        vs->v.i = v;
    }
    
    void _setfvar(char *name, int id, float v)
    {
        _varstruct *vs;
        
        vs = _var_prepare(name, id);
        if(!vs) return;
        
        vs->type = _VAR_FLOAT;
        vs->v.f = v;
    }
    
    void _setsvar(char *name, int id, char *v)
    {
        _varstruct *vs;
        
        if(!v || !*v) return;
        
        vs = _var_prepare(name, id);
        if(!vs) return;
        
        vs->type = _VAR_STRING;
        
        int l = strlen(v)+1;
        if(l>4096) return;      //danger: not cleaned up (why i ever do this check at all???)
        vs->v.s = new char [l];
        if(!vs->v.s) return;    //danger: not cleaned up
        
        strcpy(vs->v.s, v);
    }
*/    
    int _var_getpriv(char *name)
    {
        if(!name) return PRIV_ROOT;
        
        loopv(_var_priv)
        {
            if(_var_priv[i] && _var_priv[i]->name && !strcmp(name, _var_priv[i]->name)) return _var_priv[i]->priv;
        }
        return PRIV_ROOT;
    }
    
    bool _var_checkpriv(char *name, clientinfo *ci)
    {
        int cpriv = _getpriv(ci);
        int vpriv = _var_getpriv(name);
        return (cpriv>=vpriv)?true:false;
    }
    
    bool _setvarc(char *name, char *v, clientinfo *ci)
    {
        if(!name || !*name || !v) return false;
        
        int l = strlen(v) + 1;
        if(l>4096 || l<=0) return false;
        
        int cpriv = _getpriv(ci);
        int vpriv = _var_getpriv(name);
        
        if(cpriv<vpriv) return false;
        
        _varstruct *vs;
        vs = _var_prepare(name, 0);
        if(!vs)
        {
            _debug("_setvarc:_var_prepare failed");
            return false;
        }
        
        if(vs->type)
        {
            switch(vs->type)
            {
                case _VAR_INT:
                {
                    int i = atoi(v);
                    if(i==0 && strcmp(v, "0"))
                    {
                        float f = atof(v);
                        if(f==0.0 && strcmp(v, "0.0"))
                        {
                            vs->v.s = new char [l];
                            if(!vs->v.s) return false;
                            strcpy(vs->v.s, v);
                            vs->type = _VAR_STRING;
                        }
                        else
                        {
                            vs->type = _VAR_FLOAT;
                            vs->v.f = f;
                        }
                    }
                    else vs->v.i = i;
                    
                    break;
                }
                
                case _VAR_FLOAT:
                {
                    float f = atof(v);
                    if(f==0.0 && strcmp(v,"0") && strcmp(v,"0.0"))
                    {
                        vs->v.s = new char [l];
                        if(!vs->v.s) return false;
                        strcpy(vs->v.s, v);
                        vs->type = _VAR_STRING;
                    }
                    else vs->v.f = f;
                    
                    break;
                }
                
                case _VAR_STRING:
                default:
                {
                    vs->v.s = new char [l];
                    if(!vs->v.s) return false;
                    strcpy(vs->v.s, v);
                    vs->type = _VAR_STRING;
                    break;
                }
            }
        }
        else    //new variable
        {
            int i = atoi(v);
            if(i==0 && strcmp(v, "0"))
            {
                float f = atof(v);
                if(f==0.0 && strcmp(v, "0.0"))
                {
                    vs->v.s = new char [l];
                    if(!vs->v.s) return false;
                    strcpy(vs->v.s, v);
                    vs->type = _VAR_STRING;
                }
                else
                {
                    vs->type = _VAR_FLOAT;
                    vs->v.f = f;
                }
            }
            else
            {
                vs->type = _VAR_INT;
                vs->v.i = i;
            }
        }
        
        return true;
    }
    
    void _notify(const char *msg, clientinfo *ci = 0, int priv = 0, bool self = true)
    {
        loopv(clients) if(clients[i] && ((priv && clients[i]->privilege >= priv) || (self && clients[i]==ci)))
        {
            sendf(clients[i]->clientnum, 1, "ris", N_SERVMSG, msg);
        }
    }
    
    void _notifypriv(const char *msg, int min, int max)
    {
        loopv(clients) if(clients[i] && (clients[i]->privilege>=min) && (clients[i]->privilege<=max))
        sendf(clients[i]->clientnum, 1, "ris", N_SERVMSG, msg);
    }
    
    int _argsep(char *str, int c, char *argv[], char sep = ' ') //separate args (str = source string; c = expected argc; argv = ptrs array; ret = argc)
    {
        char *s;
        int argc;

        for(int i = 1; i < c; i++) argv[i] = 0; //zero out all pointers
        argv[0] = str;
        if(!str || !*str) return 0;
        argc = 1;
        for(int i = 1; i < c; i++)
        {
            s = strchr(argv[i - 1], sep);
            if(!s) break;   //no delimiter found - prevous argument is last argument or string end
            *s = 0;         //replace delimiter with null
            s++;            //thing after delimiter
            while(*s == sep) s++;   //skip other delimiters if any
            argv[i] = s;    //thing after all delimiters
            argc++;
        }
        return argc;
    }
    
//  >>> Executable functions and extensions
    //Some dummy functions :D
    void _test(const char *cmd, const char *args, clientinfo *ci) {}
    void _wall(const char *cmd, const char *args, clientinfo *ci)
    {
        if(!args || !*args) return;
        sendf(-1, 1, "ris", N_SERVMSG, args);
    }
    bool _readmanfile(const char *cmd, const char **args, const char **help)
    {
        static char argsbuf[64], helpbuf[1024];
        string buf;
        
        char badchars[] = "/$%^&*()\\'\"`~";
        for(const char *p = cmd; *p; p++) for(char *b = badchars; *b; b++) if(*p == *b) return false;
#ifdef WIN32
        formatstring(buf)("man\\%s", cmd);
#else
        formatstring(buf)("man/%s", cmd);
#endif
        FILE *f;
        f = fopen(buf, "r");
        if(!f) return false;
        
        if(!fgets(argsbuf, sizeof(argsbuf), f))
        {
            fclose(f);
            return false;
        }
        for(char *p = argsbuf; *p; p++) if(*p=='\n') *p=0;
        if(!fgets(helpbuf, sizeof(helpbuf), f))
        {
            fclose(f);
            return false;
        }
        for(char *p = helpbuf; *p; p++) if(*p=='\n') *p=0;
        fclose(f);
        *args = (const char *)argsbuf;
        *help = (const char *)helpbuf;
        return true;
    }
    struct _manpage
    {
        char name[256];
        char args[64];
        char help[1024];
        int expire;
        
        _manpage()
        {
            expire = 0;
        }
        _manpage(const char *n, const char *a, const char *h)
        {
            if(n) strncpy(name, n, 256);
            if(a) strncpy(args, a, 64);
            if(h) strncpy(help, h, 1024);
            expire = 0;
        }
    };
    vector<_manpage *> _manpages;
    
    bool _readmanfile(const char *cmd)
    {
        string buf;
        
        if(!cmd || !*cmd) return false;
        
        char badchars[] = "/$%^&*()\\'\"`~";
        for(const char *p = cmd; *p; p++) for(char *b = badchars; *b; b++) if(*p == *b) return false;
#ifdef WIN32
        formatstring(buf)("man\\%s", cmd);
#else
        formatstring(buf)("man/%s", cmd);
#endif
        FILE *f;
        f = fopen(buf, "r");
        if(!f) return false;
        
        _manpage *mp;
        mp = new _manpage;
        _manpages.add(mp);
        
        if(!fgets(mp->args, 64, f))
        {
            fclose(f);
            delete mp;
            _manpages.drop();
            return false;
        }
        for(char *p = mp->args; *p; p++) if(*p=='\n') *p=0;
        if(!fgets(mp->help, 1024, f))
        {
            fclose(f);
            delete mp;
            _manpages.drop();
            return false;
        }
        for(char *p = mp->help; *p; p++) if(*p=='\n') *p=0;
        fclose(f);
        return true;
    }
    
    void _initman()
    {
        _manpages.add(new _manpage("help man", "[command]", "Shows help about command or prints avaiable commands"));
        _manpages.add(new _manpage("info version", "", "Shows information about server"));
        _manpages.add(new _manpage("wall", "<message>", "Prints message on the wall"));
        _manpages.add(new _manpage("set", "<varname> <value>", "Sets variable <varname> to <value>"));
        _manpages.add(new _manpage("showvars", "", "Shows internal server variables"));
        _manpages.add(new _manpage("mute", "[cn]", "Mutes one or all players"));
        _manpages.add(new _manpage("unmute", "[cn]", "Unmutes one or all players"));
        _manpages.add(new _manpage("priv setpriv givepriv", "[cn] <priv>", "Gives privilege for user (privilege can be number or string, like master)"));
        _manpages.add(new _manpage("takepriv", "[cn]", "Takes privilege from cn or you"));
        _manpages.add(new _manpage("setmaster givemaster", "[cn]", "Gives master"));
        _manpages.add(new _manpage("setadmin giveadmin", "[cn]", "Gives admin"));
        _manpages.add(new _manpage("spec spectate", "[cn]", "Spectates one or all players"));
        _manpages.add(new _manpage("unspec unspectate", "[cn]", "Unspectates one or all players"));
        _manpages.add(new _manpage("stats", "[cn]", "Gives stats of you or another user"));
        _manpages.add(new _manpage("pm", "<cn>[,cn,...] <message>", "Sends message to specified client numbers"));
        _manpages.add(new _manpage("editmute", "[cn]", "Mutes one or all players editing"));
        _manpages.add(new _manpage("editunmute", "[cn]", "Unmutes one or all players editing"));
        _manpages.add(new _manpage("load", "<module>", "Loads specified module"));
        _manpages.add(new _manpage("reload", "<module>", "Reloads specified module"));
        _manpages.add(new _manpage("unload", "<module>", "Unloads specified module"));
        _manpages.add(new _manpage("exec", "<cubescript>", "Executes cubescript command"));
        _manpages.add(new _manpage("spy", "[1/0]", "Enters or leaves spy mode"));
    }
    
    void _man(const char *cmd, const char *args, clientinfo *ci) {
        char msg[MAXTRANS];
        bool usage = false;
        int searchc = 0;
        bool first;
        bool found;
        
        if(!args || !*args)
        {           
            copystring(msg, "\f3 >>> \f4[\f0MAN\f4] \f1Possible commands:\n", MAXTRANS);
            for(int priv = 0; priv<=_getpriv(ci); priv++)
            {
                first  = true;
                loopv(_funcs) if(_funcs[i] && _funcs[i]->priv == priv)
                {
                    if(first)
                    {
                        first = false;
                        switch(priv)
                        {
                            case PRIV_NONE: concatstring(msg, "\f7", MAXTRANS); break;
                            case PRIV_MASTER: case PRIV_AUTH: concatstring(msg, "\f0", MAXTRANS); break;
                            case PRIV_ADMIN: concatstring(msg, "\f3", MAXTRANS); break;
                            default: concatstring(msg, "\f1", MAXTRANS); break;
                        }
                    }
                    else
                    {
                        concatstring(msg, ", ", MAXTRANS);
                    }
                    concatstring(msg, _funcs[i]->name, MAXTRANS);
                }
                if(!first && priv<_getpriv(ci)) concatstring(msg, "\n");
            }
            
            goto _sendf;
        }
        
        if(cmd && *cmd && !strcmp(cmd, "usage")) usage = true;
        
        if(!_manpages.length()) _initman();
        
        _search:
            searchc++;
            found = false;
            loopv(_manpages) if(_manpages[i])
            {
                char name[256];
                char *names[16];
                int c;
                
                copystring(name, _manpages[i]->name, 256);
                c = _argsep(name, 16, names);
                for(int j = 0; j < c; j++)
                {
                    if(!strcmp(args, names[j]))
                    {
                        if(usage && _manpages[i]->args[0] != 0)
                        {
                            formatstring(msg)("\f3 >>> \f4[\f1MAN\f4] Usage: \f0%s \f2%s", args, _manpages[i]->args);
                        }
                        else if(_manpages[i]->args[0] == 0 && _manpages[i]->help[0] != 0)
                        {
                            formatstring(msg)("\f3 >>> \f4[\f1MAN\f4] \f2%s", _manpages[i]->help);
                        }
                        else if(_manpages[i]->args[0] != 0 && _manpages[i]->help[0] != 0)
                        {
                            formatstring(msg)("\f3 >>> \f4[\f1MAN\f4] Usage: \f0%s \f2%s\n\f1[HELP] Description: \f2%s",
                                args, _manpages[i]->args, _manpages[i]->help);
                        }
                        else formatstring(msg)("\f3 >>> \f4[\f1MAN\f4] \f3Internal system error");
                    
                        found = true;
                        break;
                    }
                }
                if(found) break;
            }
        
        if(!found)
        {
            if(_readmanfile(args) && searchc<10) goto _search;
            else
            {
                formatstring(msg)("\f3 >>> \f4[\f1MAN\f4] \f2Man-page for command \f0%s \f2not found.", args);
            }
        }
        
        _sendf:
            sendf(ci?ci->clientnum:-1, 1, "ris" , N_SERVMSG, msg);
    }
    
    void _showvars(const char *cmd, const char *args, clientinfo *ci)
    {
        string msg;
        
        if(!ci) return;
        for(int i=0;i<_vars.length();i++)
        {
            if(!_vars[i])
            {
                formatstring(msg)("\fs\f3>>> \f4[\f2VAR\f4] \f5\"%i\" \frdoesnt exist", i);
                sendf(ci->clientnum, 1, "ris", N_SERVMSG, msg);
                continue;
            }
            switch(_vars[i]->type)
            {
                case _VAR_STRING:
                    formatstring(msg)("\fs\f3>>> \f4[\f2VAR\f4] \f4[\f1string\f4] name=\"%s\" v=\"%s\"",
                        _vars[i]->name?_vars[i]->name:"???", _vars[i]->v.s?_vars[i]->v.s:"???");
                    break;
                case _VAR_INT:
                    formatstring(msg)("\fs\f3>>> \f4[\f2VAR\f4] \f4[\f1int\f4] name=\"%s\" v=%i",
                        _vars[i]->name?_vars[i]->name:"???", _vars[i]->v.i);
                    break;
                case _VAR_FLOAT:
                    formatstring(msg)("\fs\f3>>> \f4[\f2VAR\f4] \f4[\f1float\f4] \frname=\"%s\" v=%f",
                        _vars[i]->name?_vars[i]->name:"???", _vars[i]->v.f);
                    break;
                default:
                    formatstring(msg)("\fs\f3>>> \f4[\f2VAR\f4] \f4[\fr???\fs\f4:\fr\fs%i\f4] \frname=\"%s\" v=0x%x",
                        _vars[i]->type, _vars[i]->name?_vars[i]->name:"???", _vars[i]->v.i);
                    break;
            }
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, msg);
        }
        for(int i=0;i<_var_priv.length();i++)
        {
            if(!_var_priv[i])
            {
                formatstring(msg)("\fs\f3>>> \f4[\f3VARSEC\f4]\fr %i doesnt exists", i);
                sendf(ci->clientnum, 1, "ris", N_SERVMSG, msg);
                continue;
            }
            formatstring(msg)("\fs\f3>>> \f4[\f3VARSEC\f4]\fr name=\"%s\" priv=%i",
                _var_priv[i]->name?_var_priv[i]->name:"???", _var_priv[i]->priv);
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, msg);
        }
    }
    
    void _spectate(clientinfo *ci, bool val)
    {
        if(!ci || (ci->state.state==CS_SPECTATOR ? val : !val)) return;
        if(ci->state.state!=CS_SPECTATOR && val)
        {
            if(ci->state.state==CS_ALIVE) suicide(ci);
            if(smode) smode->leavegame(ci);
            ci->state.state = CS_SPECTATOR;
            ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            if(!ci->local && !ci->privilege) aiman::removeai(ci);
        }
        else if(ci->state.state==CS_SPECTATOR && !val)
        {
            ci->state.state = CS_DEAD;
            ci->state.respawn();
            ci->state.lasttimeplayed = lastmillis;
            aiman::addclient(ci);
            if(ci->clientmap[0] || ci->mapcrc) checkmaps();
        }
        sendf(-1, 1, "ri3", N_SPECTATOR, ci->clientnum, val);
        if(!val && !hasmap(ci)) rotatemap(true);
    }
    
    void _forcespectator(clientinfo *ci, bool spec)
    {
        if(ci)
        {
            if(ci->state.aitype == AI_NONE)
            {
                _spectate(ci, spec);
                ci->_xi.forcedspectator = spec;
            }
        }
        else
        {
            loopv(clients) if(clients[i] && clients[i]->state.aitype == AI_NONE)  //skip bots
            {
                _spectate(clients[i], spec);
                clients[i]->_xi.forcedspectator = spec;
            }
        }
    }
    
    void _spectfunc(const char *cmd, const char *args, clientinfo *ci)
    {
        bool spec;
        
        spec = (!cmd || !*cmd || !strcmp(cmd, "spectate") || !strcmp(cmd, "spec"));
        
        if(!args || !*args) _forcespectator(0, spec);
        else
        {
            int cn = atoi(args);
            if(!cn && strcmp(args, "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
            clientinfo *cx = getinfo(cn);
            if(!cx)
            {
                defformatstring(msg)("\f3>>> \f4[\f1SPEC\f4: \f2FAIL\f4] \f2Unknown client number \f0%i", cn);
                _notify(msg, ci);
                return;
            }
            _forcespectator(cx, spec);
        }
    }
    
    void _editmute(clientinfo *ci, bool val)
    {
        if(ci) ci->_xi.editmute = val;
        else
        {
            loopv(clients) if(clients[i]) clients[i]->_xi.editmute = val;
        }
    }
    
    void _editmutefunc(const char *cmd, const char *args, clientinfo *ci)
    {
        bool val = (!cmd || !*cmd || !strcmp(cmd, "editmute"));
        if(!args || !*args) _editmute(0, val);
        else
        {
            int cn = atoi(args);
            if(!cn && strcmp(args, "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
            clientinfo *cx = getinfo(cn);
            if(!cx)
            {
                defformatstring(msg)("\f3>>> \f4[\f1EDITMUTE\f4: \f2FAIL\f4] \f2Unknown client number \f0%i", cn);
                _notify(msg, ci);
                return;
            }
            _editmute(cx, val);
        }
    }
    
    void _mute(clientinfo *ci, bool val)
    {
        if(ci) ci->_xi.mute = val;
        else
        {
            loopv(clients) if(clients[i]) clients[i]->_xi.mute = val;
        }
    }
    
    void _mutefunc(const char *cmd, const char *args, clientinfo *ci)
    {
        bool val = (!cmd || !*cmd || !strcmp(cmd, "mute"));
        if(!args || !*args) _mute(0, val);
        else
        {
            int cn = atoi(args);
            if(!cn && strcmp(args, "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
            clientinfo *cx = getinfo(cn);
            if(!cx)
            {
                defformatstring(msg)("\f3>>> \f4[\f1MUTE\f4: \f2FAIL\f4] \f2Unknown client number \f0%i", cn);
                _notify(msg, ci);
                return;
            }
            _mute(cx, val);
        }
    }
    
    void _spy(clientinfo *ci, bool val)
    {
        
        if(!ci || (ci->_xi.spy ? val : !val)) return;
        ci->_xi.spy = val;
        if(val)
        {
            if(ci->state.state!=CS_SPECTATOR)
            {
                if(ci->state.state==CS_ALIVE) suicide(ci);
                if(smode) smode->leavegame(ci);
                ci->state.timeplayed += lastmillis - ci->state.lasttimeplayed;
            }
            aiman::removeai(ci);
            sendf(ci->clientnum, 1, "ri3", N_SPECTATOR, ci->clientnum, 1);
            sendf(-1, 1, "rxi2", ci->clientnum, N_CDIS, ci->clientnum);
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3>>> \f4[\f1SPY\f4] \f1You \f0have joined the spy mode");
        }
        else
        {
            if(ci->state.state!=CS_SPECTATOR)
            {
                ci->state.state = CS_DEAD;
                ci->state.respawn();
                ci->state.lasttimeplayed = lastmillis;
                aiman::addclient(ci);
                if(ci->clientmap[0] || ci->mapcrc) checkmaps();
                if(!hasmap(ci)) rotatemap(true);
            }
            //sendf(-1, 1, "rxi2ssi", ci->clientnum, N_INITCLIENT, ci->clientnum, ci->name, ci->team, ci->playermodel);
            sendinitclient(ci);
            sendresume(ci);
            sendf(-1, 1, "ri3", N_SPECTATOR, ci->clientnum, ci->state.state==CS_SPECTATOR?1:0);
            
            //send out privileges
            packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_CURRENTMASTER);
            putint(p, mastermode);
            loopv(clients) if(clients[i]->privilege >= PRIV_MASTER && !clients[i]->_xi.spy)
            {
                putint(p, clients[i]->clientnum);
                putint(p, clients[i]->privilege);
            }
            putint(p, -1);
            sendpacket(-1, 1, p.finalize());
            
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f3>>> \f4[\f1SPY\f4] \f1You \f0left the spy mode");
        }
    }
    
    void _spyfunc(const char *cmd, const char *args, clientinfo *ci)
    {
        if(!ci) return;
        if(!args || !*args) _spy(ci, !ci->_xi.spy);
        else _spy(ci, atoi(args)!=0);
    }
    
    void _set(const char *cmd, const char *args, clientinfo *ci)
    {
        string buf;
        char *argv[2];
        
        if(!args) return;
        
        strcpy(buf, args);
        
        _argsep(buf, 2, argv);
        if(!argv[0] || !argv[1]) return;
        
        string msg;
        
        bool success;
        success = _setvarc(argv[0], argv[1], ci);
        
        if(success)
        {
            formatstring(msg)("\fs\f3>>> \f4[\f0VAR\f4] \f5'%s'\f4=\fr\"%s\"", argv[0], argv[1]);
            int pr = _var_getpriv(argv[0]);
            if(pr==PRIV_NONE) sendf(-1, 1, "ris", N_SERVMSG, msg);
            else
            {
                for(int i=0;i<clients.length();i++)
                {
                    if(_getpriv(clients[i])>=pr) sendf(clients[i]->clientnum, 1, "ris", N_SERVMSG, msg);
                }
            }
        }
        else if(ci)
        {
            formatstring(msg)("\fs\f3>>> \f4[\f1VAR\f4: \f2FAIL\f4]\fr\fs Failed to set variable \f5'%s'\fr", argv[0]);
            sendf(ci->clientnum, 1, "ris", N_SERVMSG, msg);
        }
        else
        {
            logoutf("Failed to set variable %s to value %s", argv[0], argv[1]);
        }
    }
    
    struct _pluginfunc
    {
        char name[64];
        void *ptr;
    };
    
    vector<_pluginfunc *> _plfuncs; //plugin functions - intermodule communication
    
    struct _hookfunc
    {
        int (*func)(_hookparam *);
        // int priority;
    };
    
    struct _hookstruct
    {
        char name[16];
        // int num;
        vector<_hookfunc> funcs;
    };
    
    vector<_hookstruct *> _hookfuncs;
    
    int _exechook(const char *name)
    {
        bool found=false;
        int ret;
        
        if(!name || !*name) return -1;  //<0==fail
        for(int i = 0; i < _hookfuncs.length(); i++)
        {
            if(_hookfuncs[i] && !strcmp(_hookfuncs[i]->name, name))
            {
                found = true;
                ret = 0;    //==0 - continue, ==1 - dont continue, ==-1 - error (dont continue)
                for(int j = 0; j < _hookfuncs[i]->funcs.length(); j++)
                {
                    ret = _hookfuncs[i]->funcs[j].func(&_hp);
                    if(ret) break;
                }
                break;
            }
        }
        return found?ret:-1;
    }

    int _exechook_s(char *name)
    {
        bool found=false;
        int ret;
        
        if(!name || !*name) return -1;  //<0==fail
        for(int i = 0; i < _hookfuncs.length(); i++)
        {
            if(_hookfuncs[i] && !strcmp(_hookfuncs[i]->name, name))
            {
                found = true;
                ret = 0;    //==0 - continue, ==1 - dont continue, ==-1 - error (dont continue)
                for(int j = 0; j < _hookfuncs[i]->funcs.length(); j++)
                {
                    ret = _hookfuncs[i]->funcs[j].func(&_hp);
                    if(ret) break;
                }
                break;
            }
        }
        return found?ret:-1;
    }
    
    void _addhook(const char *name, int (*hookfunc)(_hookparam *))
    {
        _hookstruct *hs = 0;
        loopv(_hookfuncs) if(_hookfuncs[i] && !strcmp(_hookfuncs[i]->name, name))
        {
            hs = _hookfuncs[i];
            break;
        }
        if(!hs)
        {
            hs = new _hookstruct;
            if(!hs) return;
            strncpy(hs->name, name, 16);
            hs->funcs.add();
            hs->funcs[0].func = hookfunc;
            _hookfuncs.add(hs);
        }
        else
        {
            loopv(hs->funcs) if(hs->funcs[i].func == hookfunc) return;
            _hookfunc hf;
            hf.func = hookfunc;
            hs->funcs.add(hf);
        }
    }
    
    void _setext(char *s, void *ptr)
    {
        _pluginfunc *p = 0;
        for(int i = 0; i < _plfuncs.length(); i++)
        {
            if(_plfuncs[i] && !strcmp(s, _plfuncs[i]->name))
            {
                p = _plfuncs[i];
                break;
            }
        }
        if(!p)
        {
            p = new _pluginfunc;
            if(!p) return;  //fatal 0_o
            _plfuncs.add(p);
            strncpy(p->name, s, 64);
        }
        
        p->ptr = ptr;
    }
    
    void _testfunc()
    {
        sendf(-1, 1, "ris", N_SERVMSG, "\fs\f3>>> \f4[\f3DEBUG\f4] \frPlugin test function");
    }
    
    void * _getext(char *s)
    {
        if(!strcmp(s, "test")) return (void *)_testfunc;
		else if(!strcmp(s, "addhook")) return (void *)_addhook;
        else if(!strcmp(s, "sendf")) return (void *)sendf;
        else if(!strcmp(s, "notifypriv")) return (void *)_notifypriv;
        else
        {
            for(int i = 0; i < _plfuncs.length(); i++)
            {
                if(_plfuncs[i] && !strcmp(s, _plfuncs[i]->name) && _plfuncs[i]->ptr) return _plfuncs[i]->ptr;
            }
        }
        return 0;
    }
    
    struct _module
    {
#ifdef WIN32
        HINSTANCE h;
#else
        void *h;
#endif
        char name[64];
    };
    vector<_module> _modules;
    
    void _load(const char *cmd, const char *args, clientinfo *ci)
    {
        char *argv[2];
        char buf[MAXTRANS];
        bool needload;
        string fname;
        
        if(!args || !*args) return;
        
        needload = (!cmd || !*cmd || !strcmp(cmd, "load") || !strcmp(cmd, "reload"));
        
        strcpy(buf, args);
        
        _argsep(buf, 2, argv);
        
        char badchars[] = "/$%^&*()\\'\"`~";
        for(char *s = argv[0]; *s; s++)
        {
            for(char *p = badchars; *p; p++)
            {
                if(*s == *p)
                {
                    _notify("\f3>>> \f4[\f3FAIL\f4] \f3Invalid module name", ci);
                    return;
                }
            }
        }
        
#ifdef WIN32
        formatstring(fname)("modules\\%s.dll", argv[0]);
#else
        formatstring(fname)("modules/lib%s.so", argv[0]);
#endif
        
        _module *m = 0;
        int mi;
        loopv(_modules) if(!strcmp(argv[0], _modules[i].name))
        {
            m = &_modules[i];
            mi = i;
            break;
        }
        if(!m)
        {
            if(!needload) return;
            mi = _modules.length();
            _modules.add();
            m = &_modules[mi];
            strncpy(m->name, argv[0], 64);
            m->h = 0;
        }
        
        if(m->h)
        {
            bool needunload = true;
            if(needload)
            {
                char *(*reinitfunc)();
                *(void **)(&reinitfunc) = Z_GETSYM(m->h, "z_reinit");
                if(reinitfunc)
                {
                    char *ret;
                    ret = reinitfunc();
                    if(ret)
                    {
                        defformatstring(msg)("\f3>>> \f4[\f1WARN\f4] \f3Plugin \f0%s \f3reinitialization function failed \f2(%s)", m->name, ret);
                        _notify(msg, ci, PRIV_ADMIN);
                    }
                    else
                    {
                        needload = false;
                        needunload = false;
                    }
                }
            }
            if(needunload)
            {
                Z_FREELIB(m->h);
                m->h = 0;
            }
        }
        
        if(needload)
        {
            m->h = Z_OPENLIB(fname);
            if(!m->h)
            {
                defformatstring(msg)("\f3>>> \f4[\f3WARN\f4] \f3Plugin \f0%s \f3loading failed \f2(%s)", argv[0], dlerror());
                _notify(msg, ci, PRIV_ADMIN);
                _modules.remove(mi);
                return;
            }
            
            char *(*initfunc)(void *, void *, char *);
            *(void **)(&initfunc) = Z_GETSYM(m->h, "z_init");
            if(!initfunc)
            {
                defformatstring(msg)("\f3>>> \f4[\f3FAIL\f4] \f3Plugin \f0%s \f3symbol \f0z_init \f3lookup failed \f2(%s)", argv[0], dlerror());
                _notify(msg, ci, PRIV_ADMIN);
                Z_FREELIB(m->h);
                _modules.remove(mi);
                return;
            }
            
            char *ret;
            ret = initfunc((void *)_getext, (void *)_setext, argv[1]);
            if(ret)
            {
                defformatstring(msg)("\f3>>> \f4[\f3WARN\f4] \f3Plugin \f0%s \f3initialization function failed \f2(%s)", argv[0], ret);
                _notify(msg, ci, PRIV_ADMIN);
                Z_FREELIB(m->h);
                _modules.remove(mi);
                return;
            }
        }
        else if(!m->h)
        {
            _modules.remove(mi);
        }
    }
    
    void _pm(const char *cmd, const char *args, clientinfo *ci)
    {
        char *argv[2];
        char *cns[16];
        int cnc;
        char buf[MAXTRANS];
        
        if(!args || !*args)
        {
            _man("usage", cmd, ci);
            return;
        }
        
        strcpy(buf, args);
        
        _argsep(buf, 2, argv);
        
        if(!argv[1] || !*argv[1]) return;
        
        cnc = _argsep(argv[0], 16, cns, ',');
        
        vector<int> clientnums;
        for(int i=0;i<cnc;i++)
        {
            int j = atoi(cns[i]);
            if(j==0 && *cns[i]!='0') continue;
            bool exists=false;
            for(int k=0;k<clientnums.length();k++) if(j==clientnums[k]) exists=true;
            if(!exists) clientnums.add(j);
        }
        
        string msg;
        formatstring(msg)("\fs\f3>>> \f4[\f1PM:\f0%s\f4(\f7%i\f4)] \f0%s\fr", colorname(ci), ci->clientnum, argv[1]);
        
        for(int i=0;i<clientnums.length();i++)
        {
            int j = clientnums[i];
            
            for(int k=0;k<clients.length();k++) if(clients[k]->clientnum==j)
            {
                sendf(j, 1, "ris", N_SERVMSG, msg);
                break;
            }
        }
    }
    
    void _setpriv(const char *cmd, const char *args, clientinfo *ci)
    {
        string buf;
        char *argv[2];
        int cn, privilege;
        clientinfo *cx;
        
        if(!args || !*args)
        {
            _man("usage", cmd, ci);
            return;
        }
        
        strcpy(buf, args);
        _argsep(buf, 2, argv);
        
        if(!cmd || !*cmd || !strcmp(cmd, "setpriv") || !strcmp(cmd, "givepriv") || !strcmp(cmd, "priv"))
        {
            if(!*argv[0] || !argv[1] || !*argv[1])
            {
                _man("usage", cmd, ci);
                return;
            }
            
            cn = atoi(argv[0]);
            if(!cn && strcmp(argv[0], "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
            
            privilege = atoi(argv[1]);
            if(!privilege && strcmp(argv[1], "0"))
            {
                if(!strcmp(argv[1], "none")) privilege = PRIV_NONE;
                else if(!strcmp(argv[1], "master")) privilege = PRIV_MASTER;
                else if(!strcmp(argv[1], "auth")) privilege = PRIV_AUTH;
                else if(!strcmp(argv[1], "admin")) privilege = PRIV_ADMIN;
                else if(!strcmp(argv[1], "root")) privilege = PRIV_ROOT;
                else
                {
                    _man("usage", cmd, ci);
                    return;
                }
            }
        }
        else if(!strcmp(cmd, "setadmin") || !strcmp(cmd, "giveadmin"))
        {
            privilege = PRIV_ADMIN;
            if(!*argv[0])
            {
                _man("usage", cmd, ci);
                return;
            }
            cn = atoi(argv[0]);
            if(!cn && strcmp(argv[0], "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
        }
        else if(!strcmp(cmd, "setmaster") || !strcmp(cmd, "givemaster"))
        {
            privilege = PRIV_MASTER;
            if(!*argv[0])
            {
                _man("usage", cmd, ci);
                return;
            }
            cn = atoi(argv[0]);
            if(!cn && strcmp(argv[0], "0"))
            {
                _man("usage", cmd, ci);
                return;
            }
        }
        else
        {
            _notify("\f3>>> \f4[\f2DEBUG\f4] \f1This function isn't implemented yet", ci);
            return;
        }
        
        cx = getinfo(cn);
        if(!cx)
        {
            defformatstring(msg)("\f3>>> \f4[\f2FAIL\f4] \f1Unknown client number \f0%i", cn);
            _notify(msg, ci);
            return;
        }
        
        if(_getpriv(ci)>=PRIV_ROOT || (((_getpriv(ci)>_getpriv(cx)) || ((ci == cx) && (privilege<_getpriv(ci)))) && (privilege>=0) && (_getpriv(ci)>=privilege)))
        {
            defformatstring(msg)("%s %s %s", colorname(cx), privilege?"claimed":"relinquished", privname(privilege?privilege:cx->privilege));
            cx->privilege = privilege;

            packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
            putint(p, N_SERVMSG);
            sendstring(msg, p);
            putint(p, N_CURRENTMASTER);
            putint(p, mastermode);
            loopv(clients) if(clients[i]->privilege >= PRIV_MASTER && !clients[i]->_xi.spy)
            {
                putint(p, clients[i]->clientnum);
                putint(p, clients[i]->privilege);
            }
            putint(p, -1);
            sendpacket(cx->_xi.spy?cx->clientnum:-1, 1, p.finalize());
            checkpausegame();
        }
        else _privfail(ci);
    }
    
    void _exec(const char *cmd, const char *args, clientinfo *ci)
    {
        if(!args || !*args)
        {
            _man("usage", cmd, ci);
            return;
        }
        defformatstring(msg)("\f3>>> \f4[\f0EXEC\f4] \f1%s", args);
        _notify(msg, ci, PRIV_ROOT);
        execute(args);
    }
    
    void _stats(const char *cmd, const char *args, clientinfo *ci)
    {
        vector<clientinfo *> cns;
        char *argv[16];
        int cnc;
        char buf[MAXTRANS];
        
        if(args) strcpy(buf, args);
        else buf[0]=0;
            
        if(args && strchr(args,',')) cnc = _argsep(buf, 16, argv, ',');
        else cnc = _argsep(buf, 16, argv);
        
        //parse clientnums
        for(int i=0;i<cnc;i++)
        {
            int cn = atoi(argv[i]);
            if(!cn && argv[i][0]!='0') continue;
            
            bool exists=false;
            for(int j=0;j<cns.length();j++) if(cn==cns[j]->clientnum) exists=true;
            if(exists) continue;
            
            for(int j=0;j<clients.length();j++)
            {
                if(cn==clients[j]->clientnum)
                {
                    cns.add(clients[j]);
                    break;
                }
            }
        }
        //no cns found?
        if(cns.length()==0)
        {
            if(ci) cns.add(ci);
            else if(cnc == 0)
            {
                for(int i=0;i<clients.length();i++) cns.add(clients[i]);
            }
        }
        
        int sendcn = ci ? ci->clientnum : -1;
        for(int i=0;i<cns.length();i++)
        {
            string msg;
            clientinfo *cx=cns[i];
            if(!m_teammode)
            {
                formatstring(msg)("\f3>>> \f4[\f0STATS\f4:\f7%s\f4(\f1%i\f4)] \f2kills:\f0%i \f2deaths:\f0%i \f2kpd:\f0%.3f \f2acc:\f0%i%%",
                    colorname(cx), cx->clientnum, cx->state.frags, cx->state.deaths,
                    (float(cx->state.frags)/float(max(cx->state.deaths, 1))),
                    (cx->state.damage*100/max(cx->state.shotdamage,1)));
            }
            else if(m_ctf || m_protect || m_hold)
            {
                formatstring(msg)("\f3>>> \f4[\f0STATS\f4:\f7%s\f4(\f1%i\f4)] \f2flags:\f0%i \f2kills:\f0%i \f2deaths:\f0%i \f2tk:\f0%i \f2kpd:\f0%.3f \f2acc:\f0%i%%",
                    colorname(cx), cx->clientnum, cx->state.flags, cx->state.frags, cx->state.deaths,
                    cx->state.teamkills, (float(cx->state.frags)/float(max(cx->state.deaths, 1))),
                    (cx->state.damage*100/max(cx->state.shotdamage,1)));
            }
            else if(m_collect)
            {
                formatstring(msg)("\f3>>> \f4[\f0STATS\f4:\f7%s\f4(\f1%i\f4)] \f2skulls:\f0%i \f2kills:\f0%i \f2deaths:\f0%i \f2tk:\f0%i \f2kpd:\f0%.3f \f2acc:\f0%i%%",
                    colorname(cx), cx->clientnum, cx->state.tokens, cx->state.frags, cx->state.deaths,
                    cx->state.teamkills, (float(cx->state.frags)/float(max(cx->state.deaths, 1))),
                    (cx->state.damage*100/max(cx->state.shotdamage,1)));
            }
            else
            {
                formatstring(msg)("\f3>>> \f4[\f0STATS\f4:\f7%s\f4(\f1%i\f4)] \f2kills:\f0%i \f2deaths:\f0%i \f2tk:\f0%i \f2kpd:\f0%.3f \f2acc:\f0%i%%",
                    colorname(cx), cx->clientnum, cx->state.frags, cx->state.deaths,
                    cx->state.teamkills, (float(cx->state.frags)/float(max(cx->state.deaths, 1))),
                    (cx->state.damage*100/max(cx->state.shotdamage,1)));
            }
            sendf(sendcn, 1, "ris", N_SERVMSG, msg);
        }
    }
    
    void _getip(const char *cmd, const char *args, clientinfo *ci)
    {
        string msg;
        if(!args || !*args)
        {
            _man("usage", cmd, ci);
            return;
        }
        int cn = atoi(args);
        if(!cn && strcmp(args, "0"))
        {
            _man("usage", cmd, ci);
            return;
        }
        clientinfo *cx = getinfo(cn);
        if(!cx)
        {
            formatstring(msg)("\f3>>> \f4[\f1GETIP\f4: \f2FAIL\f4] \f3Unknown client number \f0%i", cn);
            _notify(msg, ci);
            return;
        }
        uint ip = getclientip(cx->clientnum);
        formatstring(msg)("\fs\f3>>> \f4[\f1IP:\f0%i\f1:\f7%s\f4] \f5%i.%i.%i.%i\fr", cn, colorname(cx), ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
        sendf(ci?ci->clientnum:-1, 1, "ris", N_SERVMSG, msg);
    }
    
    void _info(const char *cmd, const char *args, clientinfo *ci)
    {
        defformatstring(msg)("\f3>>> \fs\f4[\f5INFO\f4] \f1LwHaythServ - Lightweight version of HaythServ servermod based on zeromod\fr.");
        sendf(ci?ci->clientnum:-1, 1, "ris", N_SERVMSG, msg);
    }
    
//  >>> Server internals
    
    void _initfuncs()
    {
        _init_varpriv();
        
        _funcs.add(new _funcdeclaration("test", 0, _test));
        _funcs.add(new _funcdeclaration("wall", PRIV_MASTER, _wall));
        _funcs.add(new _funcdeclaration("man", 0, _man));
        _funcs.add(new _funcdeclaration("help", 0, _man));
        _funcs.add(new _funcdeclaration("info", 0, _info));
        _funcs.add(new _funcdeclaration("version", 0, _info));
        _funcs.add(new _funcdeclaration("pm", 0, _pm));
        _funcs.add(new _funcdeclaration("exec", PRIV_ROOT, _exec));
        _funcs.add(new _funcdeclaration("stats", 0, _stats));
        _funcs.add(new _funcdeclaration("set", 0, _set));
        _funcs.add(new _funcdeclaration("vars", PRIV_ADMIN, _showvars));
        _funcs.add(new _funcdeclaration("load", PRIV_ROOT, _load));
        _funcs.add(new _funcdeclaration("reload", PRIV_ROOT, _load));
        _funcs.add(new _funcdeclaration("unload", PRIV_ROOT, _load));
        _funcs.add(new _funcdeclaration("getip", PRIV_ADMIN, _getip));
        _funcs.add(new _funcdeclaration("setpriv", PRIV_MASTER, _setpriv));
        _funcs.add(new _funcdeclaration("priv", PRIV_MASTER, _setpriv));
        _funcs.add(new _funcdeclaration("setmaster", PRIV_MASTER, _setpriv));
        _funcs.add(new _funcdeclaration("setadmin", PRIV_ADMIN, _setpriv));
        _funcs.add(new _funcdeclaration("givemaster", PRIV_MASTER, _setpriv));
        _funcs.add(new _funcdeclaration("giveadmin", PRIV_ADMIN, _setpriv));
        _funcs.add(new _funcdeclaration("spectate", PRIV_MASTER, _spectfunc));
        _funcs.add(new _funcdeclaration("spec", PRIV_MASTER, _spectfunc));
        _funcs.add(new _funcdeclaration("unspectate", PRIV_MASTER, _spectfunc));
        _funcs.add(new _funcdeclaration("unspec", PRIV_MASTER, _spectfunc));
        _funcs.add(new _funcdeclaration("mute", PRIV_AUTH, _mutefunc));
        _funcs.add(new _funcdeclaration("unmute", PRIV_AUTH, _mutefunc));
        _funcs.add(new _funcdeclaration("editmute", PRIV_MASTER, _editmutefunc));
        _funcs.add(new _funcdeclaration("editunmute", PRIV_MASTER, _editmutefunc));
        _funcs.add(new _funcdeclaration("spy", PRIV_ADMIN, _spyfunc));
    }
    
    void _privfail(clientinfo *ci)
    {
        _notify("\f3>>> \f4[\f1PRIV\f4: \f2FAIL\f4] \f5You aren't privileged to do this task", ci);
    }
    
    void _nocommand(const char *cmd, clientinfo *ci)
    {
        defformatstring(msg)("\f3>>> \f4[\f1%s\f4: \f2FAIL\f4] \f2Undefined command \f0%s\f2. Please see manual (type \f0#man\f2)", cmd, cmd);
        _notify(msg, ci);
    }
    
    inline int _getpriv(clientinfo *ci)
    {
        if(!ci) return PRIV_ROOT;
        else return ci->privilege;
    }
    
    inline bool _checkpriv(clientinfo *ci, int priv)
    {
        return (_getpriv(ci)>=priv);
    }
    
    void _servcmd(const char *cmd, clientinfo *ci)
    {
        char *argv[2];
        char str[MAXTRANS];
        bool executed=false;
        
        if(!_funcs.length()) _initfuncs();
        
        strcpy(str, cmd);
        _argsep(str, 2, argv);
        
        loopv(_funcs)
        {
            if(!strcmp(argv[0], _funcs[i]->name))
            {
                if(_checkpriv(ci, _funcs[i]->priv))
                {
                    //execute function
                    _funcs[i]->func(argv[0], argv[1], ci);
                }
                else _privfail(ci);
                executed=true;
                break;
            }
        }
        if(!executed) _nocommand(argv[0], ci);
    }
    
    ICOMMAND(zexec, "C", (char *cmd), _servcmd(cmd, 0));
    ICOMMAND(zload, "C", (char *cmd), _load("load", cmd, 0));
    ICOMMAND(zset, "C", (char *cmd), _set("set", cmd, 0));
    
// ****************************************************************************************

    void parsepacket(int sender, int chan, packetbuf &p)     // has to parse exactly each byte of the packet
    {
        if(sender<0 || p.packet->flags&ENET_PACKET_FLAG_UNSEQUENCED || chan > 2) return;
        char text[MAXTRANS];
        int type;
        clientinfo *ci = sender>=0 ? getinfo(sender) : NULL, *cq = ci, *cm = ci;
        if(ci && !ci->connected)
        {
            if(chan==0) return;
            else if(chan!=1) { disconnect_client(sender, DISC_MSGERR); return; }
            else while(p.length() < p.maxlen) switch(checktype(getint(p), ci))
            {
                case N_CONNECT:
                {
                    getstring(text, p);
                    filtertext(text, text, false, MAXNAMELEN);
                    if(!text[0]) copystring(text, "unnamed");
                    copystring(ci->name, text, MAXNAMELEN+1);
                    ci->playermodel = getint(p);

                    string password, authdesc, authname;
                    getstring(password, p, sizeof(password));
                    getstring(authdesc, p, sizeof(authdesc));
                    getstring(authname, p, sizeof(authname));
                    int disc = allowconnect(ci, password);
                    if(disc)
                    {
                        if(disc == DISC_LOCAL || !serverauth[0] || strcmp(serverauth, authdesc) || !tryauth(ci, authname, authdesc))
                        {
                            disconnect_client(sender, disc);
                            return;
                        }
                        ci->connectauth = disc;
                    }
                    else connected(ci);
                    break;
                }

                case N_AUTHANS:
                {
                    string desc, ans;
                    getstring(desc, p, sizeof(desc));
                    uint id = (uint)getint(p);
                    getstring(ans, p, sizeof(ans));
                    answerchallenge(ci, id, ans, desc);
                    break;
                }

                case N_PING:
                    getint(p);
                    break;

                default:
                    disconnect_client(sender, DISC_MSGERR);
                    break;
            }
            return;
        }
        else if(chan==2)
        {
            receivefile(sender, p.buf, p.maxlen);
            return;
        }

        if(p.packet->flags&ENET_PACKET_FLAG_RELIABLE) reliablemessages = true;
        #define QUEUE_AI clientinfo *cm = cq;
        #define QUEUE_MSG { if(cm && (!cm->local || demorecord || hasnonlocalclients())) while(curmsg<p.length()) cm->messages.add(p.buf[curmsg++]); }
        #define QUEUE_BUF(body) { \
            if(cm && (!cm->local || demorecord || hasnonlocalclients())) \
            { \
                curmsg = p.length(); \
                { body; } \
            } \
        }
        #define QUEUE_INT(n) QUEUE_BUF(putint(cm->messages, n))
        #define QUEUE_UINT(n) QUEUE_BUF(putuint(cm->messages, n))
        #define QUEUE_STR(text) QUEUE_BUF(sendstring(text, cm->messages))
        int curmsg;
        while((curmsg = p.length()) < p.maxlen) switch(type = checktype(getint(p), ci))
        {
            case N_POS:
            {
                int pcn = getuint(p); 
                p.get(); 
                uint flags = getuint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                vec pos;
                loopk(3)
                {
                    int n = p.get(); n |= p.get()<<8; if(flags&(1<<k)) { n |= p.get()<<16; if(n&0x800000) n |= -1<<24; }
                    pos[k] = n/DMF;
                }
                loopk(3) p.get();
                int mag = p.get(); if(flags&(1<<3)) mag |= p.get()<<8;
                int dir = p.get(); dir |= p.get()<<8;
                vec vel = vec((dir%360)*RAD, (clamp(dir/360, 0, 180)-90)*RAD).mul(mag/DVELF);
                if(flags&(1<<4))
                {
                    p.get(); if(flags&(1<<5)) p.get();
                    if(flags&(1<<6)) loopk(2) p.get();
                }
                if(cp)
                {
                    if((!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                    {
                        if(!ci->local && !m_edit && max(vel.magnitude2(), (float)fabs(vel.z)) >= 180)
                            cp->setexceeded();
                        cp->position.setsize(0);
                        while(curmsg<p.length()) cp->position.add(p.buf[curmsg++]);
                    }
                    if(smode && cp->state.state==CS_ALIVE) smode->moved(cp, cp->state.o, cp->gameclip, pos, (flags&0x80)!=0);
                    cp->state.o = pos;
                    cp->gameclip = (flags&0x80)!=0;
                }
                break;
            }

            case N_TELEPORT:
            {
                int pcn = getint(p), teleport = getint(p), teledest = getint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                {
                    flushclientposition(*cp);
                    sendf(-1, 0, "ri4x", N_TELEPORT, pcn, teleport, teledest, cp->ownernum); 
                }
                break;
            }

            case N_JUMPPAD:
            {
                int pcn = getint(p), jumppad = getint(p);
                clientinfo *cp = getinfo(pcn);
                if(cp && pcn != sender && cp->ownernum != sender) cp = NULL;
                if(cp && (!ci->local || demorecord || hasnonlocalclients()) && (cp->state.state==CS_ALIVE || cp->state.state==CS_EDITING))
                {
                    cp->setpushed();
                    flushclientposition(*cp);
                    sendf(-1, 0, "ri3x", N_JUMPPAD, pcn, jumppad, cp->ownernum);
                }
                break;
            }
                
            case N_FROMAI:
            {
                int qcn = getint(p);
                if(qcn < 0) cq = ci;
                else
                {
                    cq = getinfo(qcn);
                    if(cq && qcn != sender && cq->ownernum != sender) cq = NULL;
                }
                break;
            }

            case N_EDITMODE:
            {
                int val = getint(p);
                if(!ci->local && !m_edit) { disconnect_client(sender, DISC_MSGERR); return; };
                if(val ? ci->state.state!=CS_ALIVE && ci->state.state!=CS_DEAD : ci->state.state!=CS_EDITING) break;
                if(smode)
                {
                    if(val) smode->leavegame(ci);
                    else smode->entergame(ci);
                }
                if(val)
                {
                    ci->state.editstate = ci->state.state;
                    ci->state.state = CS_EDITING;
                    ci->events.setsize(0);
                    ci->state.rockets.reset();
                    ci->state.grenades.reset();
                }
                else ci->state.state = ci->state.editstate;
                QUEUE_MSG;
                break;
            }

            case N_MAPCRC:
            {
                getstring(text, p);
                int crc = getint(p);
                if(!ci) break;
                if(strcmp(text, smapname))
                {
                    if(ci->clientmap[0])
                    {
                        ci->clientmap[0] = '\0';
                        ci->mapcrc = 0;
                    }
                    else if(ci->mapcrc > 0) ci->mapcrc = 0;
                    break;
                }
                copystring(ci->clientmap, text);
                ci->mapcrc = text[0] ? crc : 1;
                checkmaps();
                break;
            }

            case N_CHECKMAPS:
                checkmaps(sender);
                break;

            case N_TRYSPAWN:
                if(!ci || !cq || cq->state.state!=CS_DEAD || ci->_xi.spy || cq->state.lastspawn>=0 || (smode && !smode->canspawn(cq))) break;
                if(!ci->clientmap[0] && !ci->mapcrc)
                {
                    ci->mapcrc = -1;
                    checkmaps();
                }
                if(cq->state.deadflush)
                {
                    flushevents(cq, cq->state.deadflush);
                    cq->state.respawn();
                }
                cleartimedevents(cq);
                sendspawn(cq);
                break;

            case N_GUNSELECT:
            {
                int gunselect = getint(p);
                if(!cq || cq->state.state!=CS_ALIVE) break;
                cq->state.gunselect = gunselect >= GUN_FIST && gunselect <= GUN_PISTOL ? gunselect : GUN_FIST;
                QUEUE_AI;
                QUEUE_MSG;
                break;
            }

            case N_SPAWN:
            {
                int ls = getint(p), gunselect = getint(p);
                if(!cq || (cq->state.state!=CS_ALIVE && cq->state.state!=CS_DEAD) || ls!=cq->state.lifesequence || cq->state.lastspawn<0 || cq->_xi.spy) break;
                cq->state.lastspawn = -1;
                cq->state.state = CS_ALIVE;
                cq->state.gunselect = gunselect >= GUN_FIST && gunselect <= GUN_PISTOL ? gunselect : GUN_FIST;
                cq->exceeded = 0;
                if(smode) smode->spawned(cq);
                QUEUE_AI;
                QUEUE_BUF({
                    putint(cm->messages, N_SPAWN);
                    sendstate(cq->state, cm->messages);
                });
                break;
            }

            case N_SUICIDE:
            {
                if(cq) cq->addevent(new suicideevent);
                break;
            }

            case N_SHOOT:
            {
                shotevent *shot = new shotevent;
                shot->id = getint(p);
                shot->millis = cq ? cq->geteventmillis(gamemillis, shot->id) : 0;
                shot->gun = getint(p);
                loopk(3) shot->from[k] = getint(p)/DMF;
                loopk(3) shot->to[k] = getint(p)/DMF;
                int hits = getint(p);
                loopk(hits)
                {
                    if(p.overread()) break;
                    hitinfo &hit = shot->hits.add();
                    hit.target = getint(p);
                    hit.lifesequence = getint(p);
                    hit.dist = getint(p)/DMF;
                    hit.rays = getint(p);
                    loopk(3) hit.dir[k] = getint(p)/DNF;
                }
                if(cq) 
                {
                    cq->addevent(shot);
                    cq->setpushed();
                }
                else delete shot;
                break;
            }

            case N_EXPLODE:
            {
                explodeevent *exp = new explodeevent;
                int cmillis = getint(p);
                exp->millis = cq ? cq->geteventmillis(gamemillis, cmillis) : 0;
                exp->gun = getint(p);
                exp->id = getint(p);
                int hits = getint(p);
                loopk(hits)
                {
                    if(p.overread()) break;
                    hitinfo &hit = exp->hits.add();
                    hit.target = getint(p);
                    hit.lifesequence = getint(p);
                    hit.dist = getint(p)/DMF;
                    hit.rays = getint(p);
                    loopk(3) hit.dir[k] = getint(p)/DNF;
                }
                if(cq) cq->addevent(exp);
                else delete exp;
                break;
            }

            case N_ITEMPICKUP:
            {
                int n = getint(p);
                if(!cq) break;
                pickupevent *pickup = new pickupevent;
                pickup->ent = n;
                cq->addevent(pickup);
                break;
            }

            case N_TEXT:
            {
                QUEUE_AI;
                QUEUE_MSG;
                getstring(text, p);
                if(!ci) break;
                if(text[0]=='#')
                {
                    cm->messages.drop();
                    if(isdedicatedserver()) logoutf("%s: %s", colorname(cq), text);
                    _servcmd(text+1, ci);
                }
                else
                {
                    if(ci->_xi.spy)
                    {
                        cm->messages.drop();
                        filtertext(text, text);
                        sendservmsgf("\f3[REMOTE:\f7%s\f3] \f2%s", colorname(ci), text);
                    }
                    else if(ci->_xi.mute)
                    {
                        cm->messages.drop();
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3You are mutted");
                    }
                    else
                    {
                        filtertext(text, text);
                        if(isdedicatedserver()) logoutf("%s: %s", colorname(cq), text);
                        QUEUE_STR(text);
                    }
                }
                break;
            }

            case N_SAYTEAM:
            {
                getstring(text, p);
                if(!ci || !cq || (ci->state.state==CS_SPECTATOR && !ci->local && !ci->privilege) || !m_teammode || !cq->team[0]) break;
                if(ci->_xi.mute)
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3You are mutted");
                    break;
                }
                if(ci->_xi.spy)
                {
                    sendservmsgf("\f1[REMOTECHAT:\f7%s\f1] \f0%s", colorname(ci), text);
                    break;
                }
                loopv(clients)
                {
                    clientinfo *t = clients[i];
                    if(t==cq || t->state.state==CS_SPECTATOR || t->state.aitype != AI_NONE || strcmp(cq->team, t->team)) continue;
                    sendf(t->clientnum, 1, "riis", N_SAYTEAM, cq->clientnum, text);
                }
                if(isdedicatedserver()) logoutf("%s <%s>: %s", colorname(cq), cq->team, text);
                break;
            }

            case N_SWITCHNAME:
            {
                QUEUE_MSG;
                getstring(text, p);
                filtertext(ci->name, text, false, MAXNAMELEN);
                if(!ci->name[0]) copystring(ci->name, "unnamed");
                QUEUE_STR(ci->name);
                break;
            }

            case N_SWITCHMODEL:
            {
                ci->playermodel = getint(p);
                QUEUE_MSG;
                break;
            }

            case N_SWITCHTEAM:
            {
                getstring(text, p);
                filtertext(text, text, false, MAXTEAMLEN);
                if(m_teammode && text[0] && strcmp(ci->team, text) && (!smode || smode->canchangeteam(ci, ci->team, text)) && addteaminfo(text))
                {
                    if(ci->state.state==CS_ALIVE) suicide(ci);
                    copystring(ci->team, text);
                    aiman::changeteam(ci);
                    if(!ci->_xi.spy) sendf(-1, 1, "riisi", N_SETTEAM, sender, ci->team, ci->state.state==CS_SPECTATOR ? -1 : 0);
                    else sendf(sender, 1, "riisi", N_SETTEAM, sender, ci->team, -1);
                }
                break;
            }

            case N_MAPVOTE:
            {
                getstring(text, p);
                filtertext(text, text, false);
                int reqmode = getint(p);
                vote(text, reqmode, sender);
                break;
            }

            case N_ITEMLIST:
            {
                if((ci->state.state==CS_SPECTATOR && !ci->privilege && !ci->local) || !notgotitems || strcmp(ci->clientmap, smapname)) { while(getint(p)>=0 && !p.overread()) getint(p); break; }
                int n;
                while((n = getint(p))>=0 && n<MAXENTS && !p.overread())
                {
                    server_entity se = { NOTUSED, 0, false };
                    while(sents.length()<=n) sents.add(se);
                    sents[n].type = getint(p);
                    if(canspawnitem(sents[n].type))
                    {
                        if(m_mp(gamemode) && delayspawn(sents[n].type)) sents[n].spawntime = spawntime(sents[n].type);
                        else sents[n].spawned = true;
                    }
                }
                notgotitems = false;
                break;
            }

            case N_EDITF:              // coop editing messages
            case N_EDITT:
            case N_EDITM:
            case N_FLIP:
            case N_ROTATE:
            case N_REPLACE:
            case N_DELCUBE:
            {
                int size = server::msgsizelookup(type);
                if(size<=0) { disconnect_client(sender, DISC_MSGERR); return; }
                loopi(size-1) getint(p);
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; } //TODO: probably cheater; ban
                if(ci && ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;

            }
            
            case N_REMIP:
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; }
                if(!ci || ci->_xi.editmute) break;
                QUEUE_MSG;
                break;
            
            case N_EDITENT:
            {
                int i = getint(p);
                loopk(3) getint(p);
                int type = getint(p);
                loopk(5) getint(p);
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; }
                if(!ci || ci->state.state==CS_SPECTATOR) break;
                if(ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }
                QUEUE_MSG;
                bool canspawn = canspawnitem(type);
                if(i<MAXENTS && (sents.inrange(i) || canspawnitem(type)))
                {
                    server_entity se = { NOTUSED, 0, false };
                    while(sents.length()<=i) sents.add(se);
                    sents[i].type = type;
                    if(canspawn ? !sents[i].spawned : (sents[i].spawned || sents[i].spawntime))
                    {
                        sents[i].spawntime = canspawn ? 1 : 0;
                        sents[i].spawned = false;
                    }
                }
                break;
            }

            case N_EDITVAR:
            {
                int type = getint(p);
                getstring(text, p);
                switch(type)
                {
                    case ID_VAR: getint(p); break;
                    case ID_FVAR: getfloat(p); break;
                    case ID_SVAR: getstring(text, p);
                }
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; }
                if(ci && ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }
                if(ci && ci->state.state!=CS_SPECTATOR) QUEUE_MSG;
                break;
            }

            case N_PING:
                sendf(sender, 1, "i2", N_PONG, getint(p));
                break;

            case N_CLIENTPING:
            {
                int ping = getint(p);
                if(ci)
                {
                    ci->ping = ping;
                    loopv(ci->bots) ci->bots[i]->ping = ping;
                }
                QUEUE_MSG;
                break;
            }

            case N_MASTERMODE:
            {
                int mm = getint(p);
                if((ci->privilege || ci->local) && mm>=MM_OPEN && mm<=MM_PRIVATE)
                {
                    if((ci->privilege>=PRIV_ADMIN || ci->local) || (mastermask&(1<<mm)))
                    {
                        mastermode = mm;
                        allowedips.shrink(0);
                        if(mm>=MM_PRIVATE)
                        {
                            loopv(clients) allowedips.add(getclientip(clients[i]->clientnum));
                        }
                        sendf(-1, 1, "rii", N_MASTERMODE, mastermode);
                        //sendservmsgf("mastermode is now %s (%d)", mastermodename(mastermode), mastermode);
                    }
                    else
                    {
                        defformatstring(s)("mastermode %d is disabled on this server", mm);
                        sendf(sender, 1, "ris", N_SERVMSG, s);
                    }
                }
                break;
            }

            case N_CLEARBANS:
            {
                if(ci->privilege || ci->local)
                {
                    bannedips.shrink(0);
                    sendservmsg("cleared all bans");
                }
                break;
            }

            case N_KICK:
            {
                int victim = getint(p);
                getstring(text, p);
                filtertext(text, text);
                trykick(ci, victim, text);
                break;
            }

            case N_SPECTATOR:
            {
                int spectator = getint(p), val = getint(p);
                if(!ci->privilege && !ci->local && (spectator!=sender || (ci->state.state==CS_SPECTATOR && (mastermode>=MM_LOCKED || ci->_xi.forcedspectator)))) break;
                if(ci->_xi.spy) break;
                clientinfo *spinfo = (clientinfo *)getclientinfo(spectator); // no bots
                if(!spinfo || (spinfo->state.state==CS_SPECTATOR ? val : !val)) break;

                if(spinfo->state.state!=CS_SPECTATOR && val)
                {
                    if(spinfo->state.state==CS_ALIVE) suicide(spinfo);
                    if(smode) smode->leavegame(spinfo);
                    spinfo->state.state = CS_SPECTATOR;
                    spinfo->state.timeplayed += lastmillis - spinfo->state.lasttimeplayed;
                    if(!spinfo->local && !spinfo->privilege) aiman::removeai(spinfo);
                }
                else if(spinfo->state.state==CS_SPECTATOR && !val)
                {
                    spinfo->state.state = CS_DEAD;
                    spinfo->state.respawn();
                    spinfo->state.lasttimeplayed = lastmillis;
                    aiman::addclient(spinfo);
                    if(spinfo->clientmap[0] || spinfo->mapcrc) checkmaps();
                }
                sendf(-1, 1, "ri3", N_SPECTATOR, spectator, val);
                if(!val && !hasmap(spinfo)) rotatemap(true);
                break;
            }

            case N_SETTEAM:
            {
                int who = getint(p);
                getstring(text, p);
                filtertext(text, text, false, MAXTEAMLEN);
                if(!ci->privilege && !ci->local) break;
                clientinfo *wi = getinfo(who);
                if(!m_teammode || !text[0] || !wi || !strcmp(wi->team, text)) break;
                if((!smode || smode->canchangeteam(wi, wi->team, text)) && addteaminfo(text))
                {
                    if(wi->state.state==CS_ALIVE) suicide(wi);
                    copystring(wi->team, text, MAXTEAMLEN+1);
                }
                aiman::changeteam(wi);
                sendf(-1, 1, "riisi", N_SETTEAM, who, wi->team, 1);
                break;
            }

            case N_FORCEINTERMISSION:
                if(ci->local && !hasnonlocalclients()) startintermission();
                break;

            case N_RECORDDEMO:
            {
                int val = getint(p);
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                if(!maxdemos || !maxdemosize) 
                {
                    sendf(ci->clientnum, 1, "ris", N_SERVMSG, "the server has disabled demo recording");
                    break;
                }
                demonextmatch = val!=0;
                sendservmsgf("demo recording is %s for next match", demonextmatch ? "enabled" : "disabled");
                break;
            }

            case N_STOPDEMO:
            {
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                stopdemo();
                break;
            }

            case N_CLEARDEMOS:
            {
                int demo = getint(p);
                if(ci->privilege < (restrictdemos ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                cleardemos(demo);
                break;
            }

            case N_LISTDEMOS:
                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                listdemos(sender);
                break;

            case N_GETDEMO:
            {
                int n = getint(p);
                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                senddemo(ci, n);
                break;
            }

            case N_GETMAP:
                if(!mapdata) sendf(sender, 1, "ris", N_SERVMSG, "no map to send");
                else if(ci->getmap) sendf(sender, 1, "ris", N_SERVMSG, "already sending map");
                else
                {
                    sendservmsgf("[%s is getting the map]", colorname(ci));
                    if((ci->getmap = sendfile(sender, 2, mapdata, "ri", N_SENDMAP)))
                        ci->getmap->freeCallback = freegetmap;
                    ci->needclipboard = totalmillis ? totalmillis : 1;
                }
                break;

            case N_NEWMAP:
            {
                int size = getint(p);
                if(ci && ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }

                if(!ci->privilege && !ci->local && ci->state.state==CS_SPECTATOR) break;
                if(size>=0)
                {
                    smapname[0] = '\0';
                    resetitems();
                    notgotitems = false;
                    if(smode) smode->newmap();
                }
                QUEUE_MSG;
                break;
            }

            case N_SETMASTER:
            {
                int mn = getint(p), val = getint(p);
                getstring(text, p);
                if(mn != ci->clientnum)
                {
                    if(!ci->privilege && !ci->local) break;
                    clientinfo *minfo = (clientinfo *)getclientinfo(mn);
                    if(!minfo || (!ci->local && minfo->privilege >= ci->privilege) || (val && minfo->privilege)) break;
                    setmaster(minfo, val!=0, "", NULL, NULL, PRIV_MASTER, true);
                }
                else setmaster(ci, val!=0, text);
                // don't broadcast the master password
                break;
            }

            case N_ADDBOT:
            {
                aiman::reqadd(ci, getint(p));
                break;
            }

            case N_DELBOT:
            {
                aiman::reqdel(ci);
                break;
            }

            case N_BOTLIMIT:
            {
                int limit = getint(p);
                if(ci) aiman::setbotlimit(ci, limit);
                break;
            }

            case N_BOTBALANCE:
            {
                int balance = getint(p);
                if(ci) aiman::setbotbalance(ci, balance!=0);
                break;
            }

            case N_AUTHTRY:
            {
                string desc, name;
                getstring(desc, p, sizeof(desc));
                getstring(name, p, sizeof(name));
                tryauth(ci, name, desc);
                break;
            }

            case N_AUTHKICK:
            {
                string desc, name;
                getstring(desc, p, sizeof(desc));
                getstring(name, p, sizeof(name));
                int victim = getint(p);
                getstring(text, p);
                filtertext(text, text);
                int authpriv = PRIV_AUTH;
                if(desc[0])
                {
                    userinfo *u = users.access(userkey(name, desc));
                    if(u) authpriv = u->privilege; else break;
                }
                if(trykick(ci, victim, text, name, desc, authpriv, true) && tryauth(ci, name, desc))
                {
                    ci->authkickvictim = victim;
                    ci->authkickreason = newstring(text);
                } 
                break;
            }

            case N_AUTHANS:
            {
                string desc, ans;
                getstring(desc, p, sizeof(desc));
                uint id = (uint)getint(p);
                getstring(ans, p, sizeof(ans));
                answerchallenge(ci, id, ans, desc);
                break;
            }

            case N_PAUSEGAME:
            {
                int val = getint(p);
                if(ci->privilege < (restrictpausegame ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                pausegame(val > 0, ci);
                break;
            }

            case N_GAMESPEED:
            {
                int val = getint(p);
                if(ci->privilege < (restrictgamespeed ? PRIV_ADMIN : PRIV_MASTER) && !ci->local) break;
                changegamespeed(val, ci);
                break;
            }

            case N_COPY:
            {
                ci->cleanclipboard();
                ci->lastclipboard = totalmillis ? totalmillis : 1;
                
                int size = server::msgsizelookup(type);
                if(size<=0) { disconnect_client(sender, DISC_MSGERR); return; }
                loopi(size-1) getint(p);
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; }
                if(ci && ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
            }


            case N_PASTE:
            {
                if(ci->state.state!=CS_SPECTATOR) sendclipboard(ci);
                
                int size = server::msgsizelookup(type);
                if(size<=0) { disconnect_client(sender, DISC_MSGERR); return; }
                loopi(size-1) getint(p);
                if(!m_edit) { disconnect_client(sender, DISC_MSGERR); return; }
                if(ci && ci->_xi.editmute)
                {
                    if(!ci->_xi.editmutewarn || ci->_xi.editmutewarn < totalmillis)
                    {
                        sendf(sender, 1, "ris", N_SERVMSG, "\f5[MUTE] \f3Your editing is mutted");
                        ci->_xi.editmutewarn = totalmillis + 10000;
                    }
                    break;
                }
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
            }
    
            case N_CLIPBOARD:
            {
                int unpacklen = getint(p), packlen = getint(p); 
                ci->cleanclipboard(false);
                if(ci->state.state==CS_SPECTATOR)
                {
                    if(packlen > 0) p.subbuf(packlen);
                    break;
                }
                if(packlen <= 0 || packlen > (1<<16) || unpacklen <= 0) 
                {
                    if(packlen > 0) p.subbuf(packlen);
                    packlen = unpacklen = 0;
                }
                packetbuf q(32 + packlen, ENET_PACKET_FLAG_RELIABLE);
                putint(q, N_CLIPBOARD);
                putint(q, ci->clientnum);
                putint(q, unpacklen);
                putint(q, packlen); 
                if(packlen > 0) p.get(q.subbuf(packlen).buf, packlen);
                ci->clipboard = q.finalize();
                ci->clipboard->referenceCount++;
                break;
            } 

            case N_SERVCMD:
                getstring(text, p);
                _servcmd(text, ci);
                break;
                     
            #define PARSEMESSAGES 1
            #include "capture.h"
            #include "ctf.h"
            #include "collect.h"
            #undef PARSEMESSAGES

            case -1:
                disconnect_client(sender, DISC_MSGERR);
                return;

            case -2:
                disconnect_client(sender, DISC_OVERFLOW);
                return;

            default:
            {
                int size = server::msgsizelookup(type);
                if(size<=0) { disconnect_client(sender, DISC_MSGERR); return; }
                loopi(size-1) getint(p);
                if(ci && cq && (ci != cq || ci->state.state!=CS_SPECTATOR)) { QUEUE_AI; QUEUE_MSG; }
                break;
            }
        }
    }

    int laninfoport() { return SAUERBRATEN_LANINFO_PORT; }
    int serverinfoport(int servport) { return servport < 0 ? SAUERBRATEN_SERVINFO_PORT : servport+1; }
    int serverport(int infoport) { return infoport < 0 ? SAUERBRATEN_SERVER_PORT : infoport-1; }
    const char *defaultmaster() { return "sauerbraten.org"; }
    int masterport() { return SAUERBRATEN_MASTER_PORT; }
    int numchannels() { return 3; }

    #include "extinfo.h"

    void serverinforeply(ucharbuf &req, ucharbuf &p)
    {
        if(!getint(req))
        {
            extserverinforeply(req, p);
            return;
        }

        putint(p, numclients(-1, false, true));
        putint(p, gamepaused || gamespeed != 100 ? 7 : 5);                   // number of attrs following
        putint(p, PROTOCOL_VERSION);    // generic attributes, passed back below
        putint(p, gamemode);
        putint(p, m_timed ? max((gamelimit - gamemillis)/1000, 0) : 0);
        putint(p, maxclients);
        putint(p, serverpass[0] ? MM_PASSWORD : (!m_mp(gamemode) ? MM_PRIVATE : (mastermode || mastermask&MM_AUTOAPPROVE ? mastermode : MM_AUTH)));
        if(gamepaused || gamespeed != 100)
        {
            putint(p, gamepaused ? 1 : 0);
            putint(p, gamespeed);
        }
        sendstring(smapname, p);
        sendstring(serverdesc, p);
        sendserverinforeply(p);
    }

    bool servercompatible(char *name, char *sdec, char *map, int ping, const vector<int> &attr, int np)
    {
        return attr.length() && attr[0]==PROTOCOL_VERSION;
    }

    #include "aiman.h"
}

