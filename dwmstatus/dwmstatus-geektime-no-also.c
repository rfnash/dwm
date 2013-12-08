#define _BSD_SOURCE
#define _POSIX_SOURCE
#define _GNU_SOURCE /* exp10() */
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/Xlib.h>

/* TODO:
 * convert to libudev monitoring??
 */

#ifdef __UCLIBC__
/* 10^x = 10^(log e^x) = (e^x)^log10 = e^(x * log 10) */
#define exp10(x) (exp((x) * log(10)))
#endif /* __UCLIBC__ */

#define MAX_LINEAR_DB_SCALE	24

static inline bool use_linear_dB_scale(long dBmin, long dBmax)
{
	return dBmax - dBmin <= MAX_LINEAR_DB_SCALE * 100;
}

static const char *tzny = "America/Detroit";

static const char *charging = "/sys/class/power_supply/AC0/online";
static const char *present = "/sys/class/power_supply/BAT0/present";
static const char *energy_now = "/sys/class/power_supply/BAT0/charge_now";
static const char *energy_full = "/sys/class/power_supply/BAT0/charge_full_design";
/* some laptops use charge_now and charge_full
 * static const char *charge_now = "/sys/class/power_supply/BAT0/charge_now";
 * static const char *charge_full = "/sys/class/power_supply/BAT0/charge_full";
 */

static const char *operstate = "/sys/class/net/wlan0/operstate";
static const char *wifi_sig = "/sys/class/net/wlan0/wireless/link";

char* smprintf(char *fmt, ...) {
    va_list fmtargs;
    char *ret;
    int len;

    va_start(fmtargs, fmt);
    len = vsnprintf(NULL, 0, fmt, fmtargs);
    va_end(fmtargs);

    ret = malloc(++len);
    if (ret == NULL) {
        perror("malloc");
        exit(1);
    }

    va_start(fmtargs, fmt);
    vsnprintf(ret, len, fmt, fmtargs);
    va_end(fmtargs);

    return ret;
}

char* readfile(const char *file) {
    char line[513];
    FILE *fd;

    memset(line, 0, sizeof(line));

    fd = fopen(file, "r");
    if (fd == NULL)
        return NULL;

    if (fgets(line, sizeof(line)-1, fd) == NULL)
        return NULL;
    fclose(fd);

    return smprintf("%s", line);
}

bool wifioperational() {
    char *co;
    co = readfile(operstate);
    if (co == NULL || co[0] != 'u') {
        if (co != NULL) free(co);
        return false;
    }
    free(co);
    return true;
}

char* getsignalstrength() {
    char *co;
    int strength;

    int wifiup = wifioperational();
    if (!wifiup)
        return smprintf("");

    co = readfile(wifi_sig);
    if (co == NULL) {
        /* return smprintf("--\x81 "); */
        return smprintf("wifi -- \x19");
    } else {
        sscanf(co, "%d", &strength);
        free(co);
        return smprintf("wifi %d%% \x19 ", (long)((float)strength/70.0*100));
    }
}

bool ischarging() {
    char *co;
    int online;
    co = readfile(charging);
    if (co == NULL) {
        return false;
    } else {
        sscanf(co, "%d", &online);
        free(co);
        return online;
    }
}

bool batteryispresent() {
    char *co;
    co = readfile(present);
    if (co == NULL || co[0] != '1') {
        if (co != NULL) free(co);
        return false;
    }
    free(co);
    return true;
}


char* getbattery() {
    char *co;
    int descap, remcap;

	//int energy_now, energy_full, voltage_now;
	//return ((float)energy_now * 1000 / (float)voltage_now) * 100 / ((float)energy_full * 1000 / (float)voltage_now);

    descap = -1;
    remcap = -1;

    co = readfile(present);
    if (co == NULL || co[0] != '1') {
        if (co != NULL) free(co);
        return smprintf("");
    }
    free(co);

    co = readfile(energy_full);
    if (co == NULL)
        return smprintf("");

    sscanf(co, "%d", &descap);
    free(co);

    co = readfile(energy_now);
    if (co == NULL)
        return smprintf("");

    sscanf(co, "%d", &remcap);
    free(co);

    if (remcap < 0 || descap < 0)
        return smprintf("invalid");

    float charge = ((float)remcap / (float)descap) * 100;
    return smprintf("Batt %.0f%%%s \x19 ", charge, (ischarging()? "<<" : ">>"));
}

char* mktimes(char *fmt, const char *tzname) {
    char buf[129];
    time_t tim;
    struct tm *timtm;
    double secs;
    double geektime;
    unsigned int geekday;
    double swatch;

    tim = time(NULL);
    timtm = gmtime(&tim);
    if (timtm == NULL) {
        perror("gmtime");
        exit(1);
    }
    secs = timtm->tm_hour*3600+timtm->tm_min*60+timtm->tm_sec;
    geektime = secs*65536.0/86400.0;
    geekday = timtm->tm_yday;
    swatch = secs*1000.0/86400.0;

    bzero(buf, sizeof(buf));
    setenv("TZ", tzname, 1);
    tim = time(NULL);
    timtm = localtime(&tim);
    if (timtm == NULL) {
        perror("localtime");
        exit(1);
    }

    if (!strftime(buf, sizeof(buf)-1, fmt, timtm)) {
        fprintf(stderr, "strftime == 0\n");
        exit(1);
    }

    return smprintf("%s | 0X%04X 0X%03X @%3.2f", buf, (unsigned int)geektime, geekday, swatch);
}

/*
 * gettemperature("/sys/class/hwmon/hwmon0/device", "temp1_input");
 */

char *
gettemperature(char *sensor)
{
	char *co;

	co = readfile(sensor);
	if (co == NULL)
		return smprintf("");
	return smprintf("%02.0f°C", atof(co) / 1000);
}

int main(void) {
    Display *dpy;
    char *temp;
    char *status;
    char *tmny;
    char *batt;
    char *wifi;

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "dwmstatus: cannot open display.\n");
        return 1;
    }

    for (;;sleep(1)) {
        wifi = getsignalstrength();
        batt = getbattery();
        temp = gettemperature("/sys/class/hwmon/hwmon0/temp1_input");
        tmny = mktimes("%a %d %b %H:%M:%S", tzny);

        /* status = smprintf("\x8D   | \x8F | \x90 | \x9D | \x81 | %s", tmny); */
        /*                    charge | batt | vol  | mute | wifi | time        */
        status = smprintf("%s | %s%s %s", temp, batt, wifi, tmny);

        XStoreName(dpy, DefaultRootWindow(dpy), status);
        XSync(dpy, False);

        free(tmny);
        free(batt);
        free(wifi);
        free(status);
    }

    XCloseDisplay(dpy);

    return 0;
}
