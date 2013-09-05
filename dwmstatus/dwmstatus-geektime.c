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
#include <alsa/asoundlib.h>

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

static const char *soundcard = "default";
static const char *soundelement = "Master";
static const snd_mixer_selem_channel_id_t channel = SND_MIXER_SCHN_MONO;

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
    int secs;
    int geektime;
    int geekday;
    double swatch;

    tim = time(NULL);
    timtm = gmtime(&tim);
    if (timtm == NULL) {
        perror("gmtime");
        exit(1);
    }
    secs = timtm->tm_hour*3600+timtm->tm_min*60+timtm->tm_sec;
    geektime = secs*65536/86400;
    geekday = timtm->tm_yday;
    swatch = secs*1000/86400.0;

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

    return smprintf("%s | 0x%04X 0x%03X @%3.2f", buf, geektime, geekday, swatch);
}

double getnormvolume(snd_mixer_elem_t *elem) {
    long min=0, max=0, value = 0;
	double normalized=0, min_norm = 0;
    int err;

	err = snd_mixer_selem_get_playback_dB_range(elem, &min, &max);
    if (err < 0 || min >= max) {
        err = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        if (err < 0 || min == max)
            return 0;

        err = snd_mixer_selem_get_playback_volume(elem, channel, &value);
        if (err < 0)
            return 0;

        return (value - min) / (double)(max - min);
    }

	err = snd_mixer_selem_get_playback_dB(elem, channel, &value);
    if (err < 0)
        return 0;

    if (use_linear_dB_scale(min, max))
        return (value - min) / (double)(max - min);

	normalized = exp10((value - max) / 6000.0);
	if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
		min_norm = exp10((min - max) / 6000.0);
		normalized = (normalized - min_norm) / (1 - min_norm);
	}

	return normalized;
}

char* getvolume() {
    int sound_on;
    double normalized_volume;
    int err;

    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, soundcard);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, soundelement);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    err = snd_mixer_selem_get_playback_switch(elem, channel, &sound_on);

    if (err < 0 || !sound_on) {
        snd_mixer_close(handle);
        /* return smprintf("\x9D "); */
        return smprintf("Muted \x19 ");
    }

    normalized_volume = getnormvolume(elem);
    snd_mixer_close(handle);

    /* return smprintf("%.0f\x90 ", normalized_volume * 100); */
    return smprintf("Vol %.0f%% \x19 ", normalized_volume * 100);
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
    char *volume;

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "dwmstatus: cannot open display.\n");
        return 1;
    }

    for (;;sleep(1)) {
        wifi = getsignalstrength();
        batt = getbattery();
        volume = getvolume();
        temp = gettemperature("/sys/class/hwmon/hwmon0/temp1_input");
        tmny = mktimes("%a %d %b %H:%M:%S", tzny);

        /* status = smprintf("\x8D   | \x8F | \x90 | \x9D | \x81 | %s", tmny); */
        /*                    charge | batt | vol  | mute | wifi | time        */
        status = smprintf("%s | %s%s%s %s", temp, volume, batt, wifi, tmny);

        XStoreName(dpy, DefaultRootWindow(dpy), status);
        XSync(dpy, False);

        free(tmny);
        free(batt);
        free(wifi);
        free(volume);
        free(status);
    }

    XCloseDisplay(dpy);

    return 0;
}
