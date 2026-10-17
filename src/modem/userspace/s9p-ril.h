/* SPDX-License-Identifier: GPL-2.0 */
/*
 * What Android's telephony does for Samsung's libsec-ril that IRadio does not
 * cover, shared by s9p-mobile and s9p-radio.
 */
#ifndef S9P_RIL_H
#define S9P_RIL_H

#include <radio_types.h>
#include <gbinder_writer.h>
#include <glib.h>

/* ISehRadio@2.0::setDataAllowed with the phone id (the data slot) */
int s9p_seh_set_data_slot(const char *dev, int slot);
/* ISehRadio@2.0::setMobileDataSetting: Samsung's mobile data switch */
int s9p_seh_mobile_data(const char *dev, gboolean on, gboolean roaming);
/* ISehRadio@2.0::sendRequestRaw: libsecril-client's OEM hook */
int s9p_seh_raw(const char *dev, const guint8 *data, gsize len);

/* a DataProfileInfo 1.4 with its strings, as setupDataCall/setInitialAttachApn take it */
void s9p_write_data_profile(GBinderWriter *w, const char *apn, const char *user,
			    const char *pass, RADIO_PDP_PROTOCOL_TYPE protocol);

#endif
