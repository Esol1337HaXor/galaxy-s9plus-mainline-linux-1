/* SPDX-License-Identifier: GPL-2.0 */
/*
 * s9p-mobile's face on the system bus: the part of ModemManager's D-Bus API
 * that NetworkManager and GNOME use, so the modem shows up in the desktop
 * like any other (Quick Settings, Settings > Mobile Network) and
 * NetworkManager does the data connection, its addresses, routes and DNS.
 *
 * The radio side (s9p-mobile.c) reports what it knows with mm_*() and gets
 * NetworkManager's requests through mobile_*(), which it implements.
 */
#ifndef S9P_MM_H
#define S9P_MM_H

#include <glib.h>

/* MMModemState */
enum {
	MM_STATE_FAILED = -1,
	MM_STATE_UNKNOWN = 0,
	MM_STATE_INITIALIZING = 1,
	MM_STATE_LOCKED = 2,
	MM_STATE_DISABLED = 3,
	MM_STATE_DISABLING = 4,
	MM_STATE_ENABLING = 5,
	MM_STATE_ENABLED = 6,
	MM_STATE_SEARCHING = 7,
	MM_STATE_REGISTERED = 8,
	MM_STATE_DISCONNECTING = 9,
	MM_STATE_CONNECTING = 10,
	MM_STATE_CONNECTED = 11,
};

/* the address of one IP family of a data call; NULL members are left out */
struct mm_ip {
	const char *address;
	guint prefix;
	const char *gateway;
	const char *dns[3];
	guint mtu;
};

/* ---- radio side -> bus ---- */

/* own org.freedesktop.ModemManager1 and export the modem */
void mm_start(void);
void mm_set_state(int state);
void mm_set_sim(const char *imsi, const char *operator_code, const char *operator_name);
/* no SIM in the slot: the modem shows, failed, "SIM missing" */
void mm_set_sim_missing(void);
/* MMModemLock */
enum { MM_LOCK_NONE = 1, MM_LOCK_SIM_PIN = 2, MM_LOCK_SIM_PUK = 4 };
/* the SIM wants its PIN (or PUK): NetworkManager asks for it (GNOME's dialog)
 * and sends it with mobile_send_pin(); retries < 0: not known */
void mm_set_locked(int lock, int retries);
/* the answer to mobile_send_pin(): error NULL when the SIM took it */
void mm_unlock_done(const char *error, int retries);
void mm_set_identity(const char *imei, const char *revision);
/* 0-100 */
void mm_set_signal(guint quality);
/* the data call NetworkManager asked for: up (ifname, addresses) or failed */
void mm_connect_done(const char *ifname, const struct mm_ip *ip4, const struct mm_ip *ip6,
		     const char *error);
/* the data call is gone, asked for or not */
void mm_disconnected(void);

/* ---- bus -> radio side (s9p-mobile.c) ---- */

/* bring a data call up; answer with mm_connect_done() */
void mobile_connect(const char *apn, const char *user, const char *password, gboolean roaming);
/* take it down; answer with mm_disconnected() */
void mobile_disconnect(void);
/* NetworkManager's mobile broadband switch: off ends data, the radio stays on */
void mobile_enable(gboolean on);
/* the SIM's PIN, or with puk set, its PUK and a new PIN; answer with mm_unlock_done() */
void mobile_send_pin(const char *puk, const char *pin);

#endif
