#include <stdio.h>
#include <string.h>
#include "GeoIP.h"
#include "GeoIPCity.h"

static GeoIP * geoip = NULL;
static GeoIP * geocity = NULL;

char geodb[260] = "GeoIP.dat";
char geocitydb[260] = "GeoLiteCity.dat";

void load_geoip_database() {
    if(geoip) GeoIP_delete(geoip);
    geoip = GeoIP_open(geodb, GEOIP_STANDARD | GEOIP_MEMORY_CACHE);
}

void load_geocity_database() {
    if(geocity) GeoIP_delete(geocity);
    geocity = GeoIP_open(geocitydb, GEOIP_STANDARD);
}

const char *ip_to_country(const char *ip) {
    if(!geoip) return "Unknown";
    const char *_country = GeoIP_country_name_by_addr(geoip, ip);
    const char *country = _country ? _country : "Unknown";
    return country;
}

const char *ip_to_continent(const char *ip) {
    if(!geoip) return "Unknown";
    const char *_continent = GeoIP_continent_by_id(GeoIP_country_id_by_addr(geoip, ip));
    const char *continent = (_continent && _continent != "--") ? _continent : "Unknown";
    return continent;
}

const char *ip_to_city(const char *ip) {
    if(!geocity) return "Unknown";
    GeoIPRecord *r = GeoIP_record_by_addr(geocity, ip);
    const char *city = (r && r->city) ? r->city : "Unknown";
    return city;
}

enum { PRIV_NONE = 0, PRIV_MASTER, PRIV_AUTH, PRIV_ADMIN, PRIV_ROOT };

void *(* z_getext)(char *);
void (* z_setext)(char *, void *);

struct hookparam
{
   void *args[8];
} __attribute__((packed));

void *(* sendf)(int, int, char *, ...);
void (*notifypriv)(char *, int, int);
void (*addhook)(char *, int (*hookfunc)(struct hookparam *));

GeoIP *gi;

int _argsep(char *str, int c, char **argv)
{
    char *s;
    int argc;
    int i;
    
    for(i = 1; i < c; i++) argv[i] = 0; //zero out all pointers
    argv[0] = str;
    if(!str || !*str) return 0;
    argc = 1;
    for(i = 1; i < c; i++)
    {
        s = strchr(argv[i - 1], ' ');
        if(!s) break;   //no delimiter found - prevous argument is last argument or string end
        *s = 0;         //replace delimiter with null
        s++;            //thing after delimiter
        while(*s == ' ') s++;   //skip other delimiters if any
        argv[i] = s;    //thing after all delimiters
        argc++;
    }
    return argc;
}


//0 - (uint32) ip
//1 - (char *)name
int on_connect(struct hookparam *hp)
{
    unsigned int ip = (unsigned int)hp->args[0];
    char *name = (char *)hp->args[1];
    char msg[512];
    char addr[64];
    
    sprintf(addr, "%i.%i.%i.%i", ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF);
    
    sprintf(msg, "\f3>>> \f1%s \f2is fragging in \f1%s\f4, \f1%s\f4, \f1%s\f4. (\f7IP\f4-\f7Address\f4: \f5%s\f4)", name, ip_to_city(addr), ip_to_country(addr), ip_to_continent(addr), addr);
    notifypriv(msg, PRIV_ADMIN, PRIV_ROOT);
    
    sprintf(msg, "\f3>>> \f1%s \f2is fragging in \f1%s\f4, \f1%s\f4, \f1%s\f4.", name, ip_to_city(addr), ip_to_country(addr), ip_to_continent(addr));
    notifypriv(msg, PRIV_NONE, PRIV_AUTH);
    
    return 0;
}

char *z_init(void *getext, void *setext, char *args)
{
    int argc;
    char *argv[16];
    int i;
    
    *(void **)(&z_getext) = getext;
    *(void **)(&z_setext) = setext;
    
    argc = _argsep(args, 16, argv);
    i = 0;
    while(i<argc)
    {
        switch(argv[i][0])
        {
            case 'f':
                strncpy(geodb, argv[++i], 260);
                break;
            case 'F':
                strncpy(geocitydb, argv[++i], 260);

            default:
                return "Unknown switch";
        }
        i++;
    }
    
    *(void **)(&sendf) = z_getext("sendf");
    *(void **)(&notifypriv) = z_getext("notifypriv");
    
    load_geoip_database();
    load_geocity_database();
    
    *(void **)(&addhook) = z_getext("addhook");
    
    addhook("connected", on_connect);
    
    return 0;
}
