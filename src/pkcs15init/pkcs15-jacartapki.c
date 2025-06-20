/*
 * pkcs15-jacartapki.c: JaCarta PKI specific operation for PKCS15 initialization
 *
 * Copyright (C) 2025  Andrey Khodunov <a.khodunov@aladdin.ru>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef ENABLE_OPENSSL /* empty file without openssl */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "common/compat_strlcpy.h"
#include "libopensc/asn1.h"
#include "libopensc/aux-data.h"
#include "libopensc/cardctl.h"
#include "libopensc/internal.h"
#include "libopensc/jacartapki.h"
#include "libopensc/log.h"
#include "libopensc/opensc.h"
#include "pkcs11/pkcs11.h"

#include "pkcs15-init.h"
#include "profile.h"

#define JACARTAPKI_ATTRS_PRKEY_RSA	(SC_PKCS15_TYPE_VENDOR_DEFINED | SC_PKCS15_TYPE_PRKEY_RSA)
#define JACARTAPKI_ATTRS_PUBKEY_RSA	(SC_PKCS15_TYPE_VENDOR_DEFINED | SC_PKCS15_TYPE_PUBKEY_RSA)
#define JACARTAPKI_ATTRS_CERT_X509	(SC_PKCS15_TYPE_VENDOR_DEFINED | SC_PKCS15_TYPE_CERT_X509)
#define JACARTAPKI_ATTRS_CERT_X509_CMAP (SC_PKCS15_TYPE_VENDOR_DEFINED | SC_PKCS15_TYPE_CERT_X509 | JACARTAPKI_PKCS15_TYPE_PRESENT_IN_CMAP)
#define JACARTAPKI_ATTRS_DATA_OBJECT	(SC_PKCS15_TYPE_VENDOR_DEFINED | SC_PKCS15_TYPE_DATA_OBJECT)

static struct sc_aid jacartapki_aid = {
		{0xA0, 0x00, 0x00, 0x01, 0x64, 0x4C, 0x41, 0x53, 0x45, 0x52, 0x00, 0x01},
		12
};

#define C_ASN1_PRKEY_DEFAULT_SUBJECT_SIZE 2
size_t default_subj_size = 10;
static const struct sc_asn1_entry c_asn1_prkey_default_subject[C_ASN1_PRKEY_DEFAULT_SUBJECT_SIZE] = {
		{"subjectName", SC_ASN1_OCTET_STRING, SC_ASN1_TAG_SEQUENCE | SC_ASN1_CONS, SC_ASN1_EMPTY_ALLOWED | SC_ASN1_ALLOC | SC_ASN1_OPTIONAL | SC_ASN1_PRESENT, "JACARTAPKI", &default_subj_size},
		{NULL,	       0,			  0,				   0,									  NULL,	      NULL		  }
};

static int jacartapki_update_df_create_data_object(struct sc_profile *profile,
		struct sc_pkcs15_card *p15card, struct sc_pkcs15_object *object);
static int jacartapki_emu_update_tokeninfo(struct sc_profile *profile,
		struct sc_pkcs15_card *p15card, struct sc_pkcs15_tokeninfo *tinfo);
static int jacartapki_cardid_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_file *file);
static int jacartapki_cmap_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		const struct sc_file *file);
static int jacartapki_cmap_update(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		int remove, struct sc_pkcs15_object *object);
static int jacartapki_cardcf_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_file *file);
static int jacartapki_cardcf_save(struct sc_profile *profile, struct sc_pkcs15_card *p15card);

static int jacartapki_cardapps_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file);

static int jacartapki_init_card_internal(struct sc_profile *profile, struct sc_pkcs15_card *p15card);
static int jacartapki_erase_card(struct sc_profile *profile, struct sc_pkcs15_card *p15card);

static void
jacartapki_strcpy_bp(unsigned char *dst, const char *src, size_t dstsize)
{
	size_t len;

	if (dst == NULL || src == NULL || dstsize == 0)
		return; // invalid arguments

	memset((char *)dst, ' ', dstsize);
	len = MIN(strlen(src), dstsize);
	memcpy((char *)dst, src, len);
}

static int
jacartapki_validate_attr_reference(int key_reference)
{
	if (key_reference < JACARTAPKI_FS_ATTR_REF_MIN)
		return SC_ERROR_INVALID_DATA;

	if (key_reference > JACARTAPKI_FS_ATTR_REF_MAX)
		return SC_ERROR_INVALID_DATA;

	return SC_SUCCESS;
}

static int
jacartapki_create_pin_object(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_file *file, const char *title)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_object *pin_obj = NULL;
	char tmp_buf[SC_PKCS15_MAX_LABEL_SIZE];
	char label[SC_PKCS15_MAX_LABEL_SIZE] = {0};
	int rv;

	LOG_FUNC_CALLED(ctx);

	snprintf(label, sizeof(label) - 1, "%s", title);

	rv = sc_bin_to_hex(file->path.value, file->path.len, tmp_buf, sizeof(tmp_buf), 0);
	LOG_TEST_RET(ctx, rv, "bin->hex error");

	rv = sc_pkcs15emu_jacartapki_create_pin(p15card, label, tmp_buf, file->path.value[file->path.len - 1], 0);
	LOG_TEST_RET(ctx, rv, "Failed to create PIN object");

	rv = sc_pkcs15_find_pin_by_reference(p15card, NULL, file->path.value[file->path.len - 1], &pin_obj);
	LOG_TEST_RET(ctx, rv, "Failed to get PIN PKCS#15 object");

	sc_pkcs15_pincache_add(p15card, pin_obj, file->encoded_content + 2, file->encoded_content_len - 2);

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_add_ee_tag(unsigned tag, const unsigned char *data, size_t data_len,
		unsigned char *eeee, size_t eeee_size, size_t *offs)
{
	if (data == NULL || eeee == NULL || offs == NULL || eeee_size == 0)
		return SC_ERROR_INVALID_ARGUMENTS;

	if (*offs + data_len + 3 > eeee_size)
		return SC_ERROR_INVALID_DATA;

	eeee[*offs] = (tag >> 8) & 0xFF;
	eeee[*offs + 1] = tag & 0xFF;
	eeee[*offs + 2] = data_len;
	memcpy(eeee + *offs + 3, data, data_len);
	*offs += data_len + 3;

	return SC_SUCCESS;
}

static int
jacartapki_update_eeef(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	unsigned char *data = NULL, zero = 0;
	char *gtime = NULL;
	size_t offs;
	int rv;

	LOG_FUNC_CALLED(ctx);

	data = calloc(1, file->size);
	if (data == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);

	offs = 0;
	/* 02C4 USER_MUST_CHANGE_AFTER_FIRST_USE */
	rv = jacartapki_add_ee_tag(0x02C4, &zero, sizeof(zero), data, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEF error: cannot add tag");

	/* 02C7 START_DATE */
	rv = sc_pkcs15_get_generalized_time(ctx, &gtime);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot allocate generalized time");

	rv = jacartapki_add_ee_tag(0x02C7, (unsigned char *)gtime, 8, data, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEF error: cannot add tag");

	/* The End */
	rv = sc_pkcs15init_update_file(profile, p15card, file, data, offs);
	if ((int)offs > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Cannot update EEEF file");
	}
	rv = SC_SUCCESS;
err:
	free(gtime);
	free(data);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_eeee(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	unsigned char *eeee = NULL, buf[0x40];
	size_t offs;
	int rv;
	struct sc_pkcs15_auth_info user_pin_info, admin_pin_info;

	LOG_FUNC_CALLED(ctx);

	eeee = calloc(1, file->size);
	if (eeee == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);

	offs = 0;
	sc_profile_get_pin_info(profile, SC_PKCS15INIT_USER_PIN, &user_pin_info);
	sc_profile_get_pin_info(profile, SC_PKCS15INIT_SO_PIN, &admin_pin_info);

	/* 02C0 General information */
	memset(buf, 0, sizeof(buf));
	buf[2] = user_pin_info.max_tries;
	buf[4] = admin_pin_info.max_tries;
	buf[5] = 0; /* S0 PIN is CHV */
	buf[6] = 1; /* User PIN is CHV */
	rv = jacartapki_add_ee_tag(0x02C0, buf, 7, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C1 Card type (not used) */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02C1, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C2 User PIN policy */
	memset(buf, 0, sizeof(buf));
	buf[1] = user_pin_info.attrs.pin.min_length;
	buf[2] = user_pin_info.attrs.pin.max_length;
	/* No PIN policy restrictions: min alpha, upper, digit, non-alpha are zero; no history*/
	rv = jacartapki_add_ee_tag(0x02C2, buf, 10, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C3 SO PIN policy */
	memset(buf, 0, sizeof(buf));
	buf[1] = admin_pin_info.attrs.pin.min_length;
	buf[2] = admin_pin_info.attrs.pin.max_length;
	/* No PIN policy restrictions: min alpha, upper, digit, non-alpha are zero; no history*/
	rv = jacartapki_add_ee_tag(0x02C3, buf, 10, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C5 USER_PIN_VALID_FOR_SECONDS */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02C5, buf, 4, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C6 USER_EXPIRES_AFTER_DAYS */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02C6, buf, 4, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C8 ALLOW_CARD_WIPE */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02C8, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02C9 BIO_IMAGE_QUALITY */
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x33;
	rv = jacartapki_add_ee_tag(0x02C9, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02CA BIO_PURPOSE (0x7fffffff/10000) */
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x00, buf[1] = 0x03, buf[2] = 0x46, buf[3] = 0xDC;
	rv = jacartapki_add_ee_tag(0x02CA, buf, 4, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02CB BIO_MAX_FINGERS */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02CB, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02CC X931_USE */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02CC, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02CD BIO_MAX_UNBLOCK */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02CD, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02CF USER_MUST_CHNGE_AFTER_UNLOCK */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02CF, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D1 USER_PIN MAX REPEATING/SEQUENCE */
	memset(buf, 0, sizeof(buf));
	buf[0] = user_pin_info.attrs.pin.max_length;
	buf[1] = user_pin_info.attrs.pin.max_length;
	rv = jacartapki_add_ee_tag(0x02D1, buf, 2, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D2 ADMIN_PIN MAX REPEATING/SEQUENCE */
	memset(buf, 0, sizeof(buf));
	buf[0] = admin_pin_info.attrs.pin.max_length;
	buf[1] = admin_pin_info.attrs.pin.max_length;
	rv = jacartapki_add_ee_tag(0x02D2, buf, 2, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D3 DS_SUPPORT (disabled) */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02D3, buf, 0x3F, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D5 USER_PIN_ALWAYS */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02D5, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D6 BIO_TYPE */
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x01;
	rv = jacartapki_add_ee_tag(0x02D6, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");

	/* 02D7 ???? */
	memset(buf, 0, sizeof(buf));
	rv = jacartapki_add_ee_tag(0x02D7, buf, 1, eeee, file->size, &offs);
	LOG_TEST_GOTO_ERR(ctx, rv, "Encode EEEE error: cannot add tag");
	/* The END */

	rv = sc_pkcs15init_update_file(profile, p15card, file, eeee, offs);
	if ((int)offs > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Cannot update EEEE file");
	}
	rv = SC_SUCCESS;
err:
	free(eeee);
	LOG_FUNC_RETURN(ctx, rv);
}
/*
 * JaCarta PKI init card implementation
 */
static int
jacartapki_init_card_internal(struct sc_profile *profile, struct sc_pkcs15_card *p15card)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_path path = {0};
	int rv, ii;
	static const char *to_create[] = {
			"Aladdin-SoPIN",
			"Aladdin-UserPIN",
			"Aladdin-TransportPIN2",
			"Aladdin-UserPinType",
			"Aladdin-LogcalExpr-AdminOrUserPIN",
			"Aladdin-LogcalExpr-AdminOrUser",
			"Aladdin-LogcalExpr-AdminOrUserOrTransport",
			"Aladdin-AppDF",
			"public-DF",
			"private-DF",
			"MiniDriver-DF",
			"Aladdin-UserHist",
			"Aladdin-TokenInfo",
			"Aladdin-EEED",
			"Aladdin-EEEE",
			"Aladdin-EEEF",
			"jacartapki-cmap-attributes",
			"jacartapki-md-cardid",
			"jacartapki-md-cardcf",
			"jacartapki-md-cardapps",
			"MiniDriver-mscp",
			NULL};
	struct sc_file *file = NULL;
	char errorMessage[128] = {0};

	LOG_FUNC_CALLED(ctx);

	sc_path_set(&path, SC_PATH_TYPE_DF_NAME, jacartapki_aid.value, jacartapki_aid.len, 0, 0);
	rv = sc_select_file(p15card->card, &path, NULL);
	LOG_TEST_RET(ctx, rv, "Cannot select JaCarta PKI AID");

	for (ii = 0; to_create[ii]; ii++) {
		unsigned char user_pin_type = JACARTAPKI_USER_PIN_TYPE_PIN;

		if (sc_profile_get_file(profile, to_create[ii], &file) < 0) {
			sc_log(ctx, "Inconsistent profile: cannot find %s", to_create[ii]);
			LOG_FUNC_RETURN(ctx, SC_ERROR_INCONSISTENT_PROFILE);
		}

		/* For the normal EF file the create file command do not accept file content. */
		rv = sc_pkcs15init_create_file(profile, p15card, file);
		if (rv != SC_ERROR_FILE_ALREADY_EXISTS && rv != SC_SUCCESS) {

			sprintf(errorMessage, "Create %s file failed.", to_create[ii]);
			LOG_TEST_GOTO_ERR(ctx, rv, errorMessage);

		} else {

			rv = SC_SUCCESS;
			if (!strcmp(to_create[ii], "Aladdin-SoPIN")) {

				rv = jacartapki_create_pin_object(profile, p15card, file, "Default Admin PIN");
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot select Aladdin-SoPIN object.");

			} else if (!strcmp(to_create[ii], "Aladdin-UserPIN")) {

				rv = jacartapki_create_pin_object(profile, p15card, file, "Default User PIN");
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot select Aladdin-UserPIN object.");

			} else if (!strcmp(to_create[ii], "Aladdin-TransportPIN2")) {

				rv = jacartapki_create_pin_object(profile, p15card, file, "TransportPIN2");
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot select Aladdin-TransportPIN2 object.");

			} else if (!strcmp(to_create[ii], "Aladdin-UserPinType")) {

				if (file->size < sizeof(user_pin_type))
					LOG_ERROR_GOTO(ctx, (rv = SC_ERROR_INVALID_DATA), "Aladdin-UserPinType file size is insufficient");

				rv = sc_pkcs15init_update_file(profile, p15card, file, &user_pin_type, sizeof(user_pin_type));
				if ((int)sizeof(user_pin_type) > rv) {
					if (rv >= 0)
						rv = SC_ERROR_INTERNAL;
					LOG_ERROR_GOTO(ctx, rv, "Cannot update Aladdin-UserPinType file.");
				}
			} else if (!strcmp(to_create[ii], "Aladdin-EEED")) {
				unsigned char data[4] = {0x02, 0xD0, 0x01, 0x64};

				if (file->size < sizeof(data))
					LOG_ERROR_GOTO(ctx, rv = SC_ERROR_INVALID_DATA, "Aladdin-EEED file size is insufficient");

				rv = sc_pkcs15init_update_file(profile, p15card, file, data, sizeof(data));
				if ((int)sizeof(data) > rv) {
					if (rv >= 0)
						rv = SC_ERROR_INTERNAL;
					LOG_ERROR_GOTO(ctx, rv, "Cannot update Aladdin-EEED file");
				}

			} else if (!strcmp(to_create[ii], "Aladdin-EEEE")) {

				rv = jacartapki_update_eeee(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot update Aladdin-EEEE file");

			} else if (!strcmp(to_create[ii], "Aladdin-EEEF")) {

				rv = jacartapki_update_eeef(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot update Aladdin-EEEF file");

			} else if (!strcmp(to_create[ii], "jacartapki-cmap-attributes")) {

				rv = jacartapki_cmap_create(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update jacartapki-cmap-attributes");

			} else if (!strcmp(to_create[ii], "jacartapki-md-cardid")) {

				rv = jacartapki_cardid_create(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot update jacartapki-md-cardid file");

			} else if (!strcmp(to_create[ii], "jacartapki-md-cardcf")) {

				rv = jacartapki_cardcf_create(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot update jacartapki-md-cardcf file");

			} else if (!strcmp(to_create[ii], "jacartapki-md-cardapps")) {

				rv = jacartapki_cardapps_create(profile, p15card, file);
				LOG_TEST_GOTO_ERR(ctx, rv, "Cannot update jacartapki-md-cardapps file");
			}
		}

		sc_file_free(file);
		file = NULL;
	}
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

/*
 * JaCarta PKI init card
 */
static int
jacartapki_init_card(struct sc_profile *profile, struct sc_pkcs15_card *p15card)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv;

	rv = jacartapki_init_card_internal(profile, p15card);
	if (rv < 0) {
		sc_log(ctx, "Failed to init JaCarta PKI, trying to erase FS first");

		jacartapki_erase_card(profile, p15card);

		rv = jacartapki_init_card_internal(profile, p15card);
	}

	LOG_FUNC_RETURN(ctx, rv);
}

/*
 * Erase the card
 */
static int
jacartapki_erase_card(struct sc_profile *profile, struct sc_pkcs15_card *p15card)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *file_in_profile = NULL;
	int ii;
	static const char *path_to_delete[] = {
			"Aladdin-AppDF",
			"Aladdin-UserPinType",
			"Aladdin-LogcalExpr-AdminOrUser",
			"Aladdin-LogcalExpr-AdminOrUserOrTransport",
			"Aladdin-LogcalExpr-AdminOrUserPIN",
			"Aladdin-TransportPIN2",
			"Aladdin-UserPIN",
			"Aladdin-SoPIN",
			"PKCS15-AppDF",
			NULL};

	LOG_FUNC_CALLED(ctx);

	for (ii = 0; path_to_delete[ii]; ii++) {
		struct sc_file *file = NULL;
		const struct sc_acl_entry *entry = NULL;
		int rv;

		if (sc_profile_get_file(profile, path_to_delete[ii], &file_in_profile) < 0) {
			sc_log(ctx, "Inconsistent profile: cannot find %s", path_to_delete[ii]);
			LOG_ERROR_RET(ctx, SC_ERROR_INCONSISTENT_PROFILE, "Failed to erase card");
		}

		sc_log(ctx, "delete file %s", sc_print_path(&file_in_profile->path));
		rv = sc_select_file(p15card->card, &file_in_profile->path, &file);
		if (rv == SC_ERROR_FILE_NOT_FOUND) {
			continue;
		} else if (rv < 0) {
			sc_log(ctx, "Failed to select %s to delete", path_to_delete[ii]);
			continue;
		}

		entry = sc_file_get_acl_entry(file, SC_AC_OP_DELETE_SELF);
		if (entry && entry->key_ref != JACARTAPKI_TRANSPORT_PIN1_REFERENCE) {
			sc_log(ctx, "Found 'DELETE-SELF' acl");
			rv = sc_pkcs15init_authenticate(profile, p15card, file, SC_AC_OP_DELETE_SELF);
			if (rv < 0) {
				sc_log(ctx, "Cannot authenticate 'DELETE-SELF' for %s", path_to_delete[ii]);
			}
		}

		if (SC_SUCCESS == rv) {
			rv = sc_delete_file(p15card->card, &file->path);
			if (rv < 0) {
				sc_log(ctx, "Cannot delete file %s", path_to_delete[ii]);
			}
		}

		sc_file_free(file);
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_create_dir(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_file *df)
{
	struct sc_context *ctx = p15card->card->ctx;

	LOG_FUNC_CALLED(ctx);

	p15card->tokeninfo->flags = SC_PKCS15_TOKEN_PRN_GENERATION;
	p15card->card->version.hw_major = JACARTAPKI_VERSION_HW_MAJOR;
	p15card->card->version.hw_minor = JACARTAPKI_VERSION_HW_MINOR;
	p15card->card->version.fw_major = JACARTAPKI_VERSION_FW_MAJOR;
	p15card->card->version.fw_minor = JACARTAPKI_VERSION_FW_MAJOR;

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

/*
 * Store a PIN
 */
static int
jacartapki_create_pin(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_file *df, struct sc_pkcs15_object *pin_obj,
		const unsigned char *pin, size_t pin_len,
		const unsigned char *puk, size_t puk_len)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_auth_info *auth_info = NULL;
	struct sc_pkcs15_pin_attributes *pin_attrs = NULL;
	struct sc_file *pin_file = NULL;
	size_t offs;
	int rv = 0, update_tokeninfo = 0;

	LOG_FUNC_CALLED(ctx);
	sc_log(ctx, "pin_obj %p, pin %p/%" SC_FORMAT_LEN_SIZE_T "u, puk %p/%" SC_FORMAT_LEN_SIZE_T "u", pin_obj, pin, pin_len, puk, puk_len);
	if (pin_obj == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);

	auth_info = (struct sc_pkcs15_auth_info *)pin_obj->data;
	if (auth_info->auth_type != SC_PKCS15_PIN_AUTH_TYPE_PIN)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OBJECT_NOT_VALID);

	pin_attrs = &auth_info->attrs.pin;
	sc_log(ctx, "create '%s'; ref 0x%X; flags %X; max_tries %i", pin_obj->label, pin_attrs->reference, pin_attrs->flags, auth_info->max_tries);

	if ((pin_attrs->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN) != 0)
		LOG_ERROR_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Unblocking PIN is not supported");

	if (pin_attrs->flags & SC_PKCS15_PIN_FLAG_SO_PIN) {
		if (pin_attrs->reference != 0x10)
			LOG_ERROR_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Paranoia test failed: invalid SO PIN reference");

		rv = sc_profile_get_file(profile, "Aladdin-SoPIN", &pin_file);
		LOG_TEST_RET(ctx, rv, "Inconsistent profile: cannot get SOPIN file");
	} else {
		if (pin_attrs->reference != 0x20)
			LOG_ERROR_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Paranoia test failed: invalid User PIN reference");

		rv = sc_profile_get_file(profile, "Aladdin-UserPIN", &pin_file);
		LOG_TEST_RET(ctx, rv, "Inconsistent profile: cannot get UserPIN file");

		update_tokeninfo = 1;
	}

	rv = sc_select_file(p15card->card, &pin_file->path, NULL);
	if (rv == 0) {
		rv = sc_pkcs15init_delete_by_path(profile, p15card, &pin_file->path);
		LOG_TEST_RET(ctx, rv, "Failed to delete PIN file");
	}

	pin_file->size = pin_attrs->max_length;
	sc_log(ctx, "create PIN file: size %" SC_FORMAT_LEN_SIZE_T "u; EF-type %i/%i; path %s",
			pin_file->size, pin_file->type, pin_file->ef_structure, sc_print_path(&pin_file->path));

	offs = 0;
	pin_file->prop_attr = calloc(1, 14);
	if (pin_file->prop_attr == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OUT_OF_MEMORY);
	pin_file->prop_attr[offs++] = JACARTAPKI_KO_NON_CRYPTO | JACARTAPKI_KO_ALLOW_TICKET | JACARTAPKI_KO_ALLOW_SECURE_VERIFY;
	pin_file->prop_attr[offs++] = JACARTAPKI_KO_USAGE_AUTH_EXT;
	pin_file->prop_attr[offs++] = JACARTAPKI_KO_ALGORITHM_PIN;
	pin_file->prop_attr[offs++] = JACARTAPKI_KO_PADDING_NO;
	pin_file->prop_attr[offs++] = (auth_info->max_tries & 0x0F) | ((auth_info->max_tries << 4) & 0xF0); /* tries/unlocks */
	pin_file->prop_attr[offs++] = pin_attrs->min_length;
	pin_file->prop_attr[offs++] = pin_attrs->max_length;
	pin_file->prop_attr[offs++] = 0;		     /* upper case */
	pin_file->prop_attr[offs++] = 0;		     /* lower case */
	pin_file->prop_attr[offs++] = 0;		     /* digit */
	pin_file->prop_attr[offs++] = 0;		     /* alpha */
	pin_file->prop_attr[offs++] = 0;		     /* special */
	pin_file->prop_attr[offs++] = pin_attrs->max_length; /* occurrence */
	pin_file->prop_attr[offs++] = pin_attrs->max_length; /* sequenve */
	pin_file->prop_attr_len = offs;

	if (pin != NULL && pin_len != 0) {
		pin_file->encoded_content = realloc(pin_file->encoded_content, 2 + pin_len);
		pin_file->encoded_content[0] = JACARTAPKI_KO_DATA_TAG_PIN;
		pin_file->encoded_content[1] = pin_len;
		memcpy(pin_file->encoded_content + 2, pin, pin_len);
		pin_file->encoded_content_len = 2 + pin_len;
	}

	rv = sc_pkcs15init_create_file(profile, p15card, pin_file);
	LOG_TEST_RET(ctx, rv, "Create PIN file failed");

	sc_file_free(pin_file);

	if (update_tokeninfo != 0) {
		p15card->tokeninfo->flags |= CKF_USER_PIN_INITIALIZED;
		rv = jacartapki_emu_update_tokeninfo(profile, p15card, p15card->tokeninfo);
		LOG_TEST_RET(ctx, rv, "Failed to update TokenInfo");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

/*
 * Allocate a file
 */
static int
jacartapki_new_file(struct sc_profile *profile, const struct sc_card *card,
		const struct sc_pkcs15_object *object, unsigned int type, unsigned int num, struct sc_file **out)
{
	struct sc_context *ctx = card->ctx;
	struct sc_file *file;
	const char *_template = NULL, *desc = NULL;
	unsigned file_descriptor = 0x01;

	LOG_FUNC_CALLED(ctx);
	sc_log(ctx, "jacartapki_new_file() type 0x%X; num %u", type, num);
	while (1) {
		switch (type) {
		case SC_PKCS15_TYPE_PRKEY_RSA:
			desc = "RSA private key";
			_template = "template-private-key";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_KO;
			break;
		case SC_PKCS15_TYPE_PUBKEY_RSA:
			desc = "RSA public key";
			_template = "template-public-key";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_KO;
			break;
		case SC_PKCS15_TYPE_DATA_OBJECT:
			desc = "data object";
			_template = "template-public-data";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		case JACARTAPKI_ATTRS_PRKEY_RSA:
			desc = "private key jacartapki attributes";
			_template = "jacartapki-private-key-attributes";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		case JACARTAPKI_ATTRS_PUBKEY_RSA:
			desc = "public key jacartapki attributes";
			_template = "jacartapki-public-key-attributes";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		case JACARTAPKI_ATTRS_CERT_X509:
			desc = "certificate jacartapki attributes";
			_template = "jacartapki-certificate-attributes";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		case JACARTAPKI_ATTRS_CERT_X509_CMAP:
			desc = "certificate jacartapki attributes";
			_template = "jacartapki-cmap-certificate-attributes";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		case JACARTAPKI_ATTRS_DATA_OBJECT:
			desc = "DATA object jacartapki attributes";
			if ((object->flags & SC_PKCS15_CO_FLAG_PRIVATE) != 0)
				_template = "jacartapki-private-data-attributes";
			else
				_template = "jacartapki-public-data-attributes";
			file_descriptor = JACARTAPKI_FILE_DESCRIPTOR_EF;
			break;
		}
		if (_template)
			break;
		/* If this is a specific type such as SC_PKCS15_TYPE_CERT_FOOBAR,
		 * fall back to the generic class (SC_PKCS15_TYPE_CERT)
		 */
		if (!(type & ~SC_PKCS15_TYPE_CLASS_MASK)) {
			sc_log(ctx, "Unsupported file type 0x%X", type);
			return SC_ERROR_INVALID_ARGUMENTS;
		}
		type &= SC_PKCS15_TYPE_CLASS_MASK;
	}

	/* TODO: do not use the file-id from profile, but macro BASEFID */
	sc_log(ctx, "jacartapki_new_file() template %s; num %u", _template, num);
	if (sc_profile_get_file(profile, _template, &file) < 0) {
		sc_log(ctx, "Profile doesn't define %s template '%s'", desc, _template);
		LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}

	file->id |= (num & 0xFF);
	file->path.value[file->path.len - 1] |= (num & 0xFF);

	if (file->type == SC_FILE_TYPE_INTERNAL_EF)
		file->ef_structure = file_descriptor;

	sc_log(ctx, "new jacartapki file: size %" SC_FORMAT_LEN_SIZE_T "u; EF-type %i/%i; path %s",
			file->size, file->type, file->ef_structure, sc_print_path(&file->path));
	*out = file;

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

/*
 * Select private key reference
 */
static int
jacartapki_select_key_reference(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_prkey_info *key_info)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv;

	rv = jacartapki_get_free_index(p15card, SC_PKCS15_TYPE_PRKEY, JACARTAPKI_FS_BASEFID_PRVKEY_EXCH);
	LOG_TEST_RET(ctx, rv, "Cannot get free key reference number");

	key_info->key_reference = rv;

	sc_log(ctx, "return selected key reference 0x%X", key_info->key_reference);
	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

/*
 * Create private key file
 */
static int
jacartapki_create_key_file(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char null_content[2] = {JACARTAPKI_KO_DATA_TAG_RSA, 0};
	int rv = 0;

	LOG_FUNC_CALLED(ctx);
	if (object->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		rv = SC_ERROR_NOT_SUPPORTED;
		LOG_ERROR_GOTO(ctx, rv, "Create key failed: RSA only supported");
	}

	sc_log(ctx, "create private key(type:%X) ID:%s key-ref:0x%X", object->type, sc_pkcs15_print_id(&key_info->id), key_info->key_reference);
	/* Here, the path of private key file should be defined.
	 * Nevertheless, we need to instantiate private key to get the ACLs. */
	rv = jacartapki_new_file(profile, p15card->card, object, object->type, key_info->key_reference, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot create private key: failed to allocate new key object");

	file->size = key_info->modulus_length / 8;

	file->prop_attr = calloc(1, 5);
	if (file->prop_attr == NULL) {
		rv = SC_ERROR_OUT_OF_MEMORY;
		LOG_ERROR_GOTO(ctx, rv, "Cannot allocate prop attrs.");
	}
	file->prop_attr_len = 5;

	file->prop_attr[0] = JACARTAPKI_KO_CLASS_RSA_CRT;

	if (key_info->usage & (SC_PKCS15_PRKEY_USAGE_DECRYPT | SC_PKCS15_PRKEY_USAGE_UNWRAP))
		file->prop_attr[1] |= JACARTAPKI_KO_USAGE_DECRYPT;
	if (key_info->usage & (SC_PKCS15_PRKEY_USAGE_NONREPUDIATION | SC_PKCS15_PRKEY_USAGE_SIGN | SC_PKCS15_PRKEY_USAGE_SIGNRECOVER))
		file->prop_attr[1] |= JACARTAPKI_KO_USAGE_SIGN;

	/* FIXME: all usages are allowed, as native MW do */
	file->prop_attr[1] |= JACARTAPKI_KO_USAGE_SIGN | JACARTAPKI_KO_USAGE_DECRYPT;

	file->prop_attr[2] = JACARTAPKI_KO_ALGORITHM_RSA;
	file->prop_attr[3] = JACARTAPKI_KO_PADDING_NO;
	file->prop_attr[4] = 0xA3; /* Max retry counter 10, 3 tries to unlock. FIXME: what's this ? */

	sc_log(ctx, "Create private key file: path %s, propr. info %s",
			sc_print_path(&file->path), sc_dump_hex(file->prop_attr, file->prop_attr_len));

	rv = sc_select_file(p15card->card, &file->path, NULL);
	if (rv == SC_SUCCESS) {
		rv = sc_pkcs15init_authenticate(profile, p15card, file, SC_AC_OP_DELETE_SELF);
		LOG_TEST_GOTO_ERR(ctx, rv, "Cannot authenticate SC_AC_OP_DELETE_SELF");

		rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to delete private key file");
	} else if (rv != SC_ERROR_FILE_NOT_FOUND) {
		LOG_TEST_GOTO_ERR(ctx, rv, "Select key file error");
	}

	file->encoded_content = malloc(2);
	memcpy(file->encoded_content, null_content, sizeof(null_content));
	file->encoded_content_len = sizeof(null_content);

	rv = sc_pkcs15init_create_file(profile, p15card, file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to create private key file");

	key_info->key_reference = file->path.value[file->path.len - 1];
	key_info->path = file->path;
	sc_log(ctx, "created private key file %s, ref:%X", sc_print_path(&key_info->path), key_info->key_reference);
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_generate_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object,
		struct sc_pkcs15_pubkey *pubkey)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_card *card = p15card->card;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_cardctl_jacartapki_genkey args;
	struct sc_file *key_file = NULL;
	unsigned char default_exponent[3] = {0x01, 0x00, 0x01};
	int rv = 0;
	unsigned char piv_algo = 0;

	LOG_FUNC_CALLED(ctx);
	if (object->type != SC_PKCS15_TYPE_PRKEY_RSA)
		LOG_ERROR_GOTO(ctx, rv = SC_ERROR_NOT_SUPPORTED, "For a while only RSA can be generated");

	rv = sc_select_file(card, &key_info->path, &key_file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to generate key: cannot select private key file");

	rv = sc_pkcs15init_authenticate(profile, p15card, key_file, SC_AC_OP_GENERATE);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot generate key: 'GENERATE' authentication failed");

	if (key_info->modulus_length == 1024)
		piv_algo = JACARTAPKI_PIV_ALGO_RSA_1024;
	else if (key_info->modulus_length == 2048)
		piv_algo = JACARTAPKI_PIV_ALGO_RSA_2048;
	else if (key_info->modulus_length == 4096)
		piv_algo = JACARTAPKI_PIV_ALGO_RSA_4096;

	memset(&args, 0, sizeof(args));

	args.algorithm = piv_algo;

	args.modulus = malloc(key_info->modulus_length / 8);
	args.exponent = malloc(sizeof(default_exponent));
	if (args.exponent == NULL || args.modulus == NULL) {
		rv = SC_ERROR_OUT_OF_MEMORY;
		LOG_ERROR_GOTO(ctx, rv, "jacartapki_generate_key() cannot allocate exponent/modulus buffers");
	}
	args.modulus_len = key_info->modulus_length / 8;
	args.exponent_len = sizeof(default_exponent);
	memcpy(args.exponent, default_exponent, sizeof(default_exponent));

	rv = sc_card_ctl(card, SC_CARDCTL_ALADDIN_GENERATE_KEY, &args);
	LOG_TEST_GOTO_ERR(ctx, rv, "jacartapki_generate_key() SC_CARDCTL_ALADDIN_GENERATE_KEY failed");

	sc_log(ctx, "modulus %s", sc_dump_hex(args.modulus, args.modulus_len));
	sc_log(ctx, "exponent %s", sc_dump_hex(args.exponent, args.exponent_len));

	key_info->access_flags |= SC_PKCS15_PRKEY_ACCESS_SENSITIVE;
	key_info->access_flags |= SC_PKCS15_PRKEY_ACCESS_ALWAYSSENSITIVE;
	key_info->access_flags |= SC_PKCS15_PRKEY_ACCESS_NEVEREXTRACTABLE;
	key_info->access_flags |= SC_PKCS15_PRKEY_ACCESS_LOCAL;

	/* allocated buffers with the public key components do not released
	 * but re-assigned to the pkcs15-public-key data */
	pubkey->algorithm = SC_ALGORITHM_RSA;
	pubkey->u.rsa.modulus.len = args.modulus_len;
	pubkey->u.rsa.modulus.data = args.modulus;

	pubkey->u.rsa.exponent.len = args.exponent_len;
	pubkey->u.rsa.exponent.data = args.exponent;
err:
	sc_file_free(key_file);
	LOG_FUNC_RETURN(ctx, rv);
}

/*
 * Store a private key
 */
static int
jacartapki_store_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object,
		struct sc_pkcs15_prkey *prkey)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	struct sc_cardctl_jacartapki_updatekey args = {NULL, 0};
	int rv;

	LOG_FUNC_CALLED(ctx);

	sc_log(ctx, "store key ID %s, path %s", sc_pkcs15_print_id(&key_info->id), sc_print_path(&key_info->path));
	sc_log(ctx, "store key %" SC_FORMAT_LEN_SIZE_T "u %" SC_FORMAT_LEN_SIZE_T "u %" SC_FORMAT_LEN_SIZE_T "u %" SC_FORMAT_LEN_SIZE_T "u %" SC_FORMAT_LEN_SIZE_T "u %" SC_FORMAT_LEN_SIZE_T "u", prkey->u.rsa.d.len, prkey->u.rsa.p.len,
			prkey->u.rsa.q.len, prkey->u.rsa.iqmp.len,
			prkey->u.rsa.dmp1.len, prkey->u.rsa.dmq1.len);

	rv = sc_select_file(p15card->card, &key_info->path, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot store key: select key file failed");

	rv = sc_pkcs15init_authenticate(profile, p15card, file, SC_AC_OP_UPDATE);
	LOG_TEST_GOTO_ERR(ctx, rv, "No authorisation to store private key");

	rv = jacartapki_encode_update_key(ctx, prkey, &args);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot encode key update data");

	sc_log(ctx, "Update data %s", sc_dump_hex(args.data, args.len));

	rv = sc_card_ctl(p15card->card, SC_CARDCTL_ALADDIN_UPDATE_KEY, &args);
	LOG_TEST_GOTO_ERR(ctx, rv, "jacartapki_generate_key() SC_CARDCTL_ALADDIN_UPDATE_KEY failed");
err:
	free(args.data);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_update_dir(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_app_info *info)
{
	struct sc_context *ctx = p15card->card->ctx;

	LOG_FUNC_CALLED(ctx);
	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_cmap_container_set_default(struct sc_pkcs15_card *p15card,
		int remove, struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *key_info = NULL;
	struct sc_pkcs15_object *key_objs[12];
	struct sc_pkcs15_id *rm_id = NULL;
	int rv, keys_num, ii, default_candidate;

	LOG_FUNC_CALLED(ctx);

	rv = sc_pkcs15_get_objects(p15card, SC_PKCS15_TYPE_PRKEY, key_objs, 12);
	LOG_TEST_RET(ctx, rv, "Failed to get private key objects");
	keys_num = rv;
	sc_log(ctx, "Found %i private keys", keys_num);

	if (remove != 0 && object != NULL) {
		if ((object->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_PRKEY)
			rm_id = &((struct sc_pkcs15_prkey_info *)object->data)->id;
		else if ((object->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_CERT)
			rm_id = &((struct sc_pkcs15_cert_info *)object->data)->id;
		else
			LOG_ERROR_RET(ctx, SC_ERROR_INTERNAL, "Invalid object type in update CMAP procedure");
		sc_log(ctx, "object(id:'%s',type:0x%X) to be removed", sc_pkcs15_print_id(rm_id), object->type);
	}

	for (ii = 0, default_candidate = -1; ii < keys_num; ii++) {
		key_info = (struct sc_pkcs15_prkey_info *)key_objs[ii]->data;

		unsigned char cmap_flags;
		rv = sc_aux_data_get_md_flags(ctx, key_info->aux_data, &cmap_flags);
		LOG_TEST_RET(ctx, rv, "Cannot get private key cmap-flags");

		sc_log(ctx, "check key object '%s', cmap flags 0x%X", sc_pkcs15_print_id(&key_info->id), cmap_flags);
		if ((rm_id != NULL && sc_pkcs15_compare_id(&key_info->id, rm_id)) || (cmap_flags & SC_MD_CONTAINER_MAP_VALID_CONTAINER) == 0) {

			cmap_flags &= ~SC_MD_CONTAINER_MAP_DEFAULT_CONTAINER;
			sc_aux_data_set_md_flags(ctx, key_info->aux_data, cmap_flags);

			sc_log(ctx, "ignore (deleted?) key ID %s", sc_pkcs15_print_id(&key_info->id));
			continue;
		}

		if ((cmap_flags & SC_MD_CONTAINER_MAP_DEFAULT_CONTAINER) != 0) {
			sc_log(ctx, "Default container exists: %s", sc_pkcs15_print_id(&key_info->id));
			LOG_FUNC_RETURN(ctx, SC_SUCCESS);
		}

		rv = sc_pkcs15_find_cert_by_id(p15card, &key_info->id, NULL);
		if (rv)
			/* ignore key object without corresponding certificate */
			continue;

		default_candidate = ii;
	}

	if (default_candidate != -1) {
		key_info = (struct sc_pkcs15_prkey_info *)key_objs[default_candidate]->data;

		unsigned char cmap_flags;
		rv = sc_aux_data_get_md_flags(ctx, key_info->aux_data, &cmap_flags);
		LOG_TEST_RET(ctx, rv, "Cannot get private key cmap-flags");

		cmap_flags |= SC_MD_CONTAINER_MAP_DEFAULT_CONTAINER;
		sc_aux_data_set_md_flags(ctx, key_info->aux_data, cmap_flags);

		sc_log(ctx, "Default container %s", sc_pkcs15_print_id(&key_info->id));
	} else {
		sc_log(ctx, "No default container");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_cardid_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_serial_number sn;
	unsigned char data[0x12];
	int rv;

	LOG_FUNC_CALLED(ctx);

	rv = sc_card_ctl(p15card->card, SC_CARDCTL_GET_SERIALNR, &sn);
	LOG_TEST_RET(ctx, rv, "Cannot get serial number");

	if (sn.len > 0x10)
		sn.len = 0x10;

	data[0] = 0x00;
	data[1] = 0x10;
	strcpy((char *)(data + 2), "ALDNSN");
	memcpy(data + 2 + 0x10 - sn.len, sn.value, sn.len);

	rv = sc_pkcs15init_update_file(profile, p15card, file, data, sizeof(data));
	if ((int)sizeof(data) > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_RET(ctx, rv, "Cannot update CARDID file");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_cmap_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, const struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_object dobj;
	struct sc_pkcs15_data_info dobj_info;
	unsigned char zero_data[643];
	int rv;

	LOG_FUNC_CALLED(ctx);

	memset(&dobj, 0, sizeof(dobj));
	memset(&dobj_info, 0, sizeof(dobj_info));
	memset(zero_data, 0, sizeof(zero_data));

	dobj.data = &dobj_info;

	dobj.type = SC_PKCS15_TYPE_DATA_OBJECT;
	dobj.flags = SC_PKCS15_CO_FLAG_MODIFIABLE;
	strlcpy(dobj.label, "cmapfile", sizeof(dobj.label));

	dobj_info.path = file->path;
	sc_init_oid(&dobj_info.app_oid);
	dobj_info.data.value = zero_data;
	dobj_info.data.len = sizeof(zero_data);
	strlcpy(dobj_info.app_label, CMAP_DO_APPLICATION_NAME, sizeof(dobj_info.app_label));

	rv = jacartapki_update_df_create_data_object(profile, p15card, &dobj);
	LOG_TEST_RET(ctx, rv, "Failed to update CMAP DATA file");

	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_cmap_update(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		int remove, struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *info = NULL;
	struct sc_pkcs15_object *cmap_dobj = NULL;
	struct sc_pkcs15_data_info *cmap_dobj_info = NULL;
	unsigned char *cmap = NULL;
	size_t cmap_len, data_len;
	int rv;

	LOG_FUNC_CALLED(ctx);
	sc_log(ctx, "Update CMAP; remove %i; object type 0x%X", remove, object ? object->type : (unsigned)(-1));
	if (object != NULL && (object->type & SC_PKCS15_TYPE_CLASS_MASK) == SC_PKCS15_TYPE_PRKEY) {
		info = (struct sc_pkcs15_prkey_info *)object->data;

		if (info->aux_data != NULL && (info->aux_data->type != SC_AUX_DATA_TYPE_MD_CMAP_RECORD && info->aux_data->type != SC_AUX_DATA_TYPE_NO_DATA))
			sc_aux_data_free(&info->aux_data);
		if (info->aux_data == NULL) {
			rv = sc_aux_data_allocate(ctx, &info->aux_data, NULL);
			LOG_TEST_RET(ctx, rv, "Cannot allocate MD auxiliary data");
		}

		if (info->aux_data->type != SC_AUX_DATA_TYPE_MD_CMAP_RECORD || info->aux_data->data.cmap_record.guid_len == 0) {
			int is_converted = 0;

			rv = jacartapki_cmap_set_key_guid(ctx, info, &is_converted);
			LOG_TEST_RET(ctx, rv, "Cannot set JaCarta PKI style GUID");
		}

		/* All new keys are 'key-exchange' keys.
		 * FIXME: implement 'sign' key. */
		info->aux_data->data.cmap_record.keysize_keyexchange = (unsigned int)info->modulus_length;
		info->aux_data->data.cmap_record.keysize_sign = 0;

		info->aux_data->data.cmap_record.flags = SC_MD_CONTAINER_MAP_VALID_CONTAINER;
		sc_log(ctx, "Set 'valid container' flag for key object '%s'", sc_pkcs15_print_id(&info->id));
	}

	rv = jacartapki_cmap_container_set_default(p15card, remove, object);
	LOG_TEST_RET(ctx, rv, "Failed to set default CMAP container");

	rv = jacartapki_cmap_encode(p15card, (remove ? object : NULL), &cmap, &cmap_len);
	LOG_TEST_RET(ctx, rv, "Failed to encode 'cmap' data");
	sc_log(ctx, "encoded CMAP(%" SC_FORMAT_LEN_SIZE_T "u) '%s'", cmap_len, sc_dump_hex(cmap, cmap_len));

	rv = sc_pkcs15_find_data_object_by_name(p15card, CMAP_DO_APPLICATION_NAME, "cmapfile", &cmap_dobj);
	LOG_TEST_RET(ctx, rv, "Failed to get 'cmapfile' DATA object");

	cmap_dobj_info = (struct sc_pkcs15_data_info *)cmap_dobj->data;

	free(cmap_dobj_info->data.value);

	data_len = cmap_len + sizeof(jacartapki_cmap_record_t);
	if (data_len < 5 * sizeof(jacartapki_cmap_record_t))
		data_len = 5 * sizeof(jacartapki_cmap_record_t);

	cmap_dobj_info->data.value = calloc(1, data_len);
	if (cmap_dobj_info->data.value == NULL) {
		rv = SC_ERROR_OUT_OF_MEMORY;
		LOG_ERROR_GOTO(ctx, rv, "Failed to allocate cmap object info");
	}

	memcpy(cmap_dobj_info->data.value, cmap, cmap_len);
	cmap_dobj_info->data.len = data_len;

	rv = jacartapki_update_df_create_data_object(profile, p15card, cmap_dobj);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DATA-DF ");
err:
	free(cmap);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_cardcf_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	jacartapki_cardcf_t cardcf = {
			{0x00, 0x06, 0x00, 0x03},
			0x1,
			0x1
	   };
	int rv;

	LOG_FUNC_CALLED(ctx);

	rv = sc_pkcs15init_update_file(profile, p15card, file, &cardcf, sizeof(cardcf));
	if ((int)sizeof(cardcf) > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_RET(ctx, rv, "Cannot update jacartapki_md_cardcf");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_cardcf_save(struct sc_profile *profile, struct sc_pkcs15_card *p15card)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv = SC_SUCCESS;
	const char *profileCardcf = "jacartapki-md-cardcf";
	struct sc_file *file = NULL;

	LOG_FUNC_CALLED(ctx);

	if (sc_profile_get_file(profile, profileCardcf, &file) < 0) {
		sc_log(ctx, "Inconsistent profile: cannot find %s", profileCardcf);
		LOG_FUNC_RETURN(ctx, SC_ERROR_INCONSISTENT_PROFILE);
	}

	if (p15card->md_data != NULL) {
		jacartapki_cardcf_t *cardcf = &p15card->md_data->cardcf;
		rv = sc_pkcs15init_update_file(profile, p15card, file, cardcf, sizeof(jacartapki_cardcf_t));
		if ((int)sizeof(jacartapki_cardcf_t) > rv) {
			if (rv >= 0)
				rv = SC_ERROR_INTERNAL;
			LOG_ERROR_RET(ctx, rv, "Cannot update jacartapki_md_cardcf");
		}
		rv = SC_SUCCESS;
	}

	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_cardapps_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_file *file)
{
	struct sc_context *ctx = p15card->card->ctx;
	unsigned char defaults_cardapps[10] = {0x00, 0x08, 0x6d, 0x73, 0x63, 0x70, 0x00, 0x00, 0x00, 0x00};
	int rv;

	LOG_FUNC_CALLED(ctx);

	rv = sc_pkcs15init_update_file(profile, p15card, file, &defaults_cardapps, sizeof(defaults_cardapps));
	if ((int)sizeof(defaults_cardapps) > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_RET(ctx, rv, "Cannot update jacartapki_md_cardapps");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_update_df_create_private_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_prkey_info *info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int attrs_ref;
	int rv;

	LOG_FUNC_CALLED(ctx);

	sc_log(ctx, "Update DF with new key ID:%s", sc_pkcs15_print_id(&info->id));

	attrs_ref = (info->key_reference & JACARTAPKI_FS_REF_MASK) - 1;
	rv = jacartapki_validate_attr_reference(attrs_ref);
	LOG_TEST_GOTO_ERR(ctx, rv, "Invalid attribute file reference");

	sc_log(ctx, "Private key attributes file reference 0x%X", attrs_ref);
	rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_PRKEY_RSA, attrs_ref, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate private key attributes file");

	/* FIXME: all usages are allowed, as native MW do */
	info->usage |= SC_PKCS15_PRKEY_USAGE_DECRYPT;
	info->usage |= SC_PKCS15_PRKEY_USAGE_UNWRAP;
	info->usage |= SC_PKCS15_PRKEY_USAGE_SIGN;
	info->usage |= SC_PKCS15_PRKEY_USAGE_SIGNRECOVER;
	info->access_flags &= ~SC_PKCS15_PRKEY_ACCESS_ALWAYSSENSITIVE;
	info->access_flags &= ~SC_PKCS15_PRKEY_ACCESS_NEVEREXTRACTABLE;
	object->flags &= ~SC_PKCS15_CO_FLAG_MODIFIABLE;
	if (info->subject.value == NULL) {
		sc_asn1_encode(ctx, c_asn1_prkey_default_subject, &info->subject.value, &info->subject.len);
	}

	sc_log(ctx, "Encode private key attributes; key-id:%s", sc_pkcs15_print_id(&info->id));
	rv = jacartapki_attrs_prvkey_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode private key attributes");
	sc_log(ctx, "Attributes: '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;

	snprintf((char *)file->name, sizeof(file->name), "kxs%02u", (unsigned int)attrs_ref);
	file->namelen = strlen((char *)file->name);

	/* TODO: implement JaCarta PKI 'resize' file */
	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot delete private key attributes");

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to create/update private key attributes file");
	}

	rv = jacartapki_cmap_update(profile, p15card, 0, object);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update 'cmapfile'");
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_df_create_public_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	const struct sc_pkcs15_pubkey_info *info = (struct sc_pkcs15_pubkey_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int attrs_ref;
	int rv;

	LOG_FUNC_CALLED(ctx);

	attrs_ref = (info->key_reference & JACARTAPKI_FS_REF_MASK) - 1;
	rv = jacartapki_validate_attr_reference(attrs_ref);
	LOG_TEST_GOTO_ERR(ctx, rv, "Invalid attribute file reference");

	sc_log(ctx, "Public key attributes file reference 0x%X", attrs_ref);
	rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_PUBKEY_RSA, attrs_ref, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate public key attributes file");

	rv = jacartapki_attrs_pubkey_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode public key attributes");
	sc_log(ctx, "Attributes: '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;

	/* TODO: implement JaCarta PKI 'resize' file */
	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot delete public key attributes");

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to create/update public key attributes file");
	}
	rv = SC_SUCCESS;
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_need_update(struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object, int *need_update)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *file = NULL;
	const struct sc_path *path = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int rv;
	unsigned char sha1[SHA_DIGEST_LENGTH];

	LOG_FUNC_CALLED(ctx);

	if (need_update == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);
	*need_update = 1;

	switch (object->type & SC_PKCS15_TYPE_CLASS_MASK) {
	case SC_PKCS15_TYPE_CERT:
		path = &((struct sc_pkcs15_cert_info *)object->data)->path;
		break;
	case SC_PKCS15_TYPE_DATA_OBJECT:
		path = &((struct sc_pkcs15_data_info *)object->data)->path;
		break;
	default:
		LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}
	sc_log(ctx, "check jacartapki attribute file's 'update' status; path %s", sc_print_path(path));

	rv = sc_select_file(p15card->card, path, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot select jacartapki attributes file");

	rv = sc_read_binary(p15card->card, JACARTAPKI_ATTRS_DIGEST_OFFSET, sha1, SHA_DIGEST_LENGTH, 0);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot read current checksum");
	if (rv != SHA_DIGEST_LENGTH) {
		rv = SC_ERROR_UNKNOWN_DATA_RECEIVED;
		LOG_ERROR_GOTO(ctx, rv, "Invalid size of current checksum");
	}

	switch (object->type & SC_PKCS15_TYPE_CLASS_MASK) {
	case SC_PKCS15_TYPE_CERT:
		rv = jacartapki_attrs_cert_encode(p15card, object, file->id, &attrs, &attrs_len);
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki certificate attributes");
		break;
	case SC_PKCS15_TYPE_DATA_OBJECT:
		rv = jacartapki_attrs_data_object_encode(p15card, object, file->id, &attrs, &attrs_len);
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki DATA attributes");
		break;
	default:
		rv = SC_ERROR_NOT_SUPPORTED;
		LOG_ERROR_GOTO(ctx, rv, "Object type not supported");
	}

	if (JACARTAPKI_ATTRS_DIGEST_OFFSET + SHA_DIGEST_LENGTH > attrs_len) {
		rv = SC_ERROR_UNKNOWN_DATA_RECEIVED;
		LOG_ERROR_GOTO(ctx, rv, "Invalid attributes received");
	}

	*need_update = memcmp(sha1, attrs + JACARTAPKI_ATTRS_DIGEST_OFFSET, SHA_DIGEST_LENGTH) ? 1 : 0;
err:
	sc_file_free(file);
	sc_log(ctx, "returns 'need-update' status %s", *need_update ? "yes" : "no");
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_df_create_certificate(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_cert_info *info = (struct sc_pkcs15_cert_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int rv, need_update;

	LOG_FUNC_CALLED(ctx);

	sc_log(ctx, "create certificate attribute file %s", sc_print_path(&info->path));

	rv = jacartapki_need_update(p15card, object, &need_update);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to get 'need-update' status of certificate data");

	if (need_update == 0) {
		sc_log(ctx, "No need to update JaCarta PKI CDF");
		LOG_FUNC_RETURN(ctx, SC_SUCCESS);
	}

	rv = sc_select_file(p15card->card, &info->path, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot select jacartapki certificate file");

	rv = jacartapki_attrs_cert_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki certificate attributes");
	sc_log(ctx, "update jacartapki certificate attributes '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;
	/* TODO: implement JaCarta PKI 'resize' file */
	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot delete jacartapki certificate");

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to update jacartapki certificate attributes file");
	}

	rv = jacartapki_cmap_update(profile, p15card, 0, NULL);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update 'cmapfile'");
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_df_create_data_object(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_data_info *info = (struct sc_pkcs15_data_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int rv, need_update;

	LOG_FUNC_CALLED(ctx);
	sc_log(ctx, "create update DF for DATA file %s", sc_print_path(&info->path));

	rv = jacartapki_need_update(p15card, object, &need_update);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to get 'need-update' status of DATA object");

	if (need_update == 0) {
		sc_log(ctx, "No need to update JaCarta PKI DataDF");
		LOG_FUNC_RETURN(ctx, SC_SUCCESS);
	}

	rv = sc_select_file(p15card->card, &info->path, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot select jacartapki DATA file");

	rv = jacartapki_attrs_data_object_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki DATA attributes");
	sc_log(ctx, "update jacartapki DATA attributes '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;

	/* TODO: implement JaCarta PKI 'resize' file */
	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update DF: cannot delete jacartapki DATA");

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to update jacartapki DATA attributes file");
	}
	rv = SC_SUCCESS;
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_df_check_pin(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *pin_obj)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_auth_info *auth_info = NULL;
	struct sc_pkcs15_pin_attributes *pin_attrs = NULL;
	struct sc_file *pin_file = NULL;
	int rv = 0;

	LOG_FUNC_CALLED(ctx);
	if (pin_obj == NULL)
		LOG_FUNC_RETURN(ctx, SC_ERROR_INVALID_ARGUMENTS);

	auth_info = (struct sc_pkcs15_auth_info *)pin_obj->data;
	if (auth_info->auth_type != SC_PKCS15_PIN_AUTH_TYPE_PIN)
		LOG_FUNC_RETURN(ctx, SC_ERROR_OBJECT_NOT_VALID);

	pin_attrs = &auth_info->attrs.pin;
	sc_log(ctx, "checking '%s'; ref 0x%X; flags %X; max_tries %i", pin_obj->label, pin_attrs->reference, pin_attrs->flags, auth_info->max_tries);

	if ((pin_attrs->flags & SC_PKCS15_PIN_FLAG_UNBLOCKING_PIN) != 0)
		LOG_ERROR_RET(ctx, SC_ERROR_NOT_SUPPORTED, "Unblocking PIN is not supported");

	if ((pin_attrs->flags & SC_PKCS15_PIN_FLAG_SO_PIN) != 0) {
		if (pin_attrs->reference != 0x10)
			LOG_ERROR_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Check failed: invalid SO PIN reference");

		rv = sc_profile_get_file(profile, "Aladdin-SoPIN", &pin_file);
		LOG_TEST_RET(ctx, rv, "Inconsistent profile: cannot get SOPIN file");
	} else {
		if (pin_attrs->reference != 0x20)
			LOG_ERROR_RET(ctx, SC_ERROR_INVALID_PIN_REFERENCE, "Check failed: invalid User PIN reference");

		rv = sc_profile_get_file(profile, "Aladdin-UserPIN", &pin_file);
		LOG_TEST_RET(ctx, rv, "Inconsistent profile: cannot get UserPIN file");
	}

	rv = sc_select_file(p15card->card, &pin_file->path, NULL);
	LOG_TEST_RET(ctx, rv, "Failed to select PIN file");

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_emu_update_df_create(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv = SC_ERROR_NOT_SUPPORTED;

	LOG_FUNC_CALLED(ctx);

	switch (object->type) {
	case SC_PKCS15_TYPE_PRKEY_RSA:
		rv = jacartapki_update_df_create_private_key(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_PUBKEY_RSA:
		rv = jacartapki_update_df_create_public_key(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_CERT_X509:
		rv = jacartapki_update_df_create_certificate(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_DATA_OBJECT:
		rv = jacartapki_update_df_create_data_object(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_AUTH_PIN:
		rv = jacartapki_update_df_check_pin(profile, p15card, object);
		break;
	default:
		LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}

	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_update_df_delete_private_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	const struct sc_pkcs15_prkey_info *info = (struct sc_pkcs15_prkey_info *)object->data;
	struct sc_file *file = NULL;
	int attrs_ref;
	int rv;

	LOG_FUNC_CALLED(ctx);

	attrs_ref = (info->key_reference & JACARTAPKI_FS_REF_MASK) - 1;
	rv = jacartapki_validate_attr_reference(attrs_ref);
	LOG_TEST_GOTO_ERR(ctx, rv, "Invalid attribute file reference");

	rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_PRKEY_RSA, attrs_ref, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate private key attributes file");

	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to delete private key attributes file");

	rv = jacartapki_cmap_update(profile, p15card, 1, object);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to update 'cmapfile'");
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_update_df_delete_public_key(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	const struct sc_pkcs15_pubkey_info *info = (struct sc_pkcs15_pubkey_info *)object->data;
	struct sc_file *file = NULL;
	int attrs_ref;
	int rv;

	LOG_FUNC_CALLED(ctx);

	attrs_ref = (info->key_reference & JACARTAPKI_FS_REF_MASK) - 1;
	rv = jacartapki_validate_attr_reference(attrs_ref);
	LOG_TEST_GOTO_ERR(ctx, rv, "Invalid attribute file reference");

	rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_PUBKEY_RSA, attrs_ref, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate public key attributes file");

	rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to delete public key attributes file");
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_update_df_delete_certificate(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_cert_info *info = (struct sc_pkcs15_cert_info *)object->data;
	int rv;

	LOG_FUNC_CALLED(ctx);

	rv = sc_pkcs15init_delete_by_path(profile, p15card, &info->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_RET(ctx, rv, "Failed to delete certificate attributes file");

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_update_df_delete_data_object(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_data_info *info = (struct sc_pkcs15_data_info *)object->data;
	int rv;

	LOG_FUNC_CALLED(ctx);

	rv = sc_pkcs15init_delete_by_path(profile, p15card, &info->path);
	if (rv != SC_ERROR_FILE_NOT_FOUND)
		LOG_TEST_RET(ctx, rv, "Failed to delete data object attributes file");

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_emu_update_df_delete(struct sc_profile *profile, struct sc_pkcs15_card *p15card, struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv = SC_ERROR_NOT_SUPPORTED;

	LOG_FUNC_CALLED(ctx);

	switch (object->type) {
	case SC_PKCS15_TYPE_PRKEY_RSA:
		rv = jacartapki_update_df_delete_private_key(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_PUBKEY_RSA:
		rv = jacartapki_update_df_delete_public_key(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_CERT_X509:
		rv = jacartapki_update_df_delete_certificate(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_DATA_OBJECT:
		rv = jacartapki_update_df_delete_data_object(profile, p15card, object);
		break;
	case SC_PKCS15_TYPE_AUTH_PIN:
		rv = jacartapki_update_df_check_pin(profile, p15card, object);
		break;
	default:
		LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
	}

	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_update_df(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		unsigned op, struct sc_pkcs15_object *object)
{
	struct sc_context *ctx = p15card->card->ctx;
	int rv = SC_ERROR_NOT_SUPPORTED;

	LOG_FUNC_CALLED(ctx);

	if (p15card->md_data != NULL) {
		jacartapki_cardcf_t *cardcf = &p15card->md_data->cardcf;
		cardcf->cont_freshness++;
		cardcf->files_freshness++;
	}

	switch (op) {
	case SC_AC_OP_CREATE:
		sc_log(ctx, "Update DF; create object('%s',type:%X)", object->label, object->type);
		rv = jacartapki_emu_update_df_create(profile, p15card, object);
		break;
	case SC_AC_OP_ERASE:
		sc_log(ctx, "Update DF; erase object('%s',type:%X)", object->label, object->type);
		rv = jacartapki_emu_update_df_delete(profile, p15card, object);
		break;
	}

	if (rv >= 0) {
		rv = jacartapki_cardcf_save(profile, p15card);
		LOG_TEST_RET(ctx, rv, "Failed to update CARDCF");
	}
	if (rv < 0 && p15card->md_data != NULL) {
		jacartapki_cardcf_t *cardcf = &p15card->md_data->cardcf;
		cardcf->cont_freshness--;
		cardcf->files_freshness--;
	}

	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_update_tokeninfo(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_tokeninfo *tinfo)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_file *file = NULL;
	jacartapki_token_info_t jti;
	int rv;

	LOG_FUNC_CALLED(ctx);

	if (tinfo == NULL)
		tinfo = p15card->tokeninfo;

	memset(&jti, 0, sizeof(jti));

	jacartapki_strcpy_bp(jti.label, tinfo->label, sizeof(jti.label));
	jacartapki_strcpy_bp(jti.manufacturer_id, tinfo->manufacturer_id, sizeof(jti.manufacturer_id));
	jacartapki_strcpy_bp(jti.model, JACARTAPKI_MODEL, sizeof(jti.model));
	jacartapki_strcpy_bp(jti.serial_number, tinfo->serial_number, sizeof(jti.serial_number));

	jti.flags = tinfo->flags;

	jti.max_pin_len = profile->pin_maxlen;
	jti.min_pin_len = profile->pin_minlen;

	jti.total_public_memory = (uint32_t)(-1);
	jti.total_private_memory = (uint32_t)(-1);

	jti.hardware_version.major = p15card->card->version.hw_major;
	jti.hardware_version.minor = p15card->card->version.hw_minor;
	jti.firmware_version.major = p15card->card->version.fw_major;
	jti.firmware_version.minor = p15card->card->version.fw_minor;

	free(tinfo->last_update.gtime);
	tinfo->last_update.gtime = NULL;

	rv = sc_pkcs15_get_generalized_time(ctx, &tinfo->last_update.gtime);
	LOG_TEST_RET(ctx, rv, "Cannot allocate generalized time");

	jacartapki_strcpy_bp(jti.utc_time, tinfo->last_update.gtime, sizeof(jti.utc_time));

	rv = sc_profile_get_file(profile, "Aladdin-TokenInfo", &file);
	LOG_TEST_RET(ctx, rv, "'Aladdin-TokenInfo' not defined");

	rv = sc_pkcs15init_update_file(profile, p15card, file, (unsigned char *)(&jti), sizeof(jti));
	if ((int)sizeof(jti) > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_RET(ctx, rv, "Cannot update TokenInfo file");
	}

	LOG_FUNC_RETURN(ctx, SC_SUCCESS);
}

static int
jacartapki_emu_write_info(struct sc_profile *profile, struct sc_pkcs15_card *p15card,
		struct sc_pkcs15_object *pin_obj)
{
	struct sc_context *ctx = p15card->card->ctx;

	LOG_FUNC_CALLED(ctx);
	LOG_FUNC_RETURN(ctx, SC_ERROR_NOT_SUPPORTED);
}

static int
jacartapki_emu_store_pubkey(struct sc_pkcs15_card *p15card,
		struct sc_profile *profile, struct sc_pkcs15_object *object,
		struct sc_pkcs15_der *data, struct sc_path *path)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_pubkey_info *info = (struct sc_pkcs15_pubkey_info *)object->data;
	const struct sc_pkcs15_prkey_info *prkey_info;
	struct sc_pkcs15_object *prkey_object = NULL;
	struct sc_pkcs15_pubkey pubkey;
	struct sc_file *file = NULL;
	int rv;

	LOG_FUNC_CALLED(ctx);
	sc_log(ctx, "Public Key id '%s'", sc_pkcs15_print_id(&info->id));
	if (data != NULL)
		sc_log(ctx, "data(%" SC_FORMAT_LEN_SIZE_T "u) %p", data->len, data->value);
	if (object->content.value != NULL)
		sc_log(ctx, "content(%" SC_FORMAT_LEN_SIZE_T "u) %p", object->content.len, object->content.value);

	pubkey.algorithm = SC_ALGORITHM_RSA;
	rv = sc_pkcs15_decode_pubkey(ctx, &pubkey, object->content.value, object->content.len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Decode public key error");

	sc_log(ctx, "Modulus '%s'", sc_dump_hex(pubkey.u.rsa.modulus.data, pubkey.u.rsa.modulus.len));
	sc_log(ctx, "Exponent '%s'", sc_dump_hex(pubkey.u.rsa.exponent.data, pubkey.u.rsa.exponent.len));

	rv = sc_pkcs15_find_prkey_by_id(p15card, &info->id, &prkey_object);
	LOG_TEST_GOTO_ERR(ctx, rv, "Find related PrKey error");

	prkey_info = (struct sc_pkcs15_prkey_info *)prkey_object->data;

	info->key_reference = (prkey_info->key_reference & JACARTAPKI_FS_REF_MASK) | JACARTAPKI_FS_BASEFID_PUBKEY;
	info->modulus_length = prkey_info->modulus_length;
	info->native = prkey_info->native;
	sc_log(ctx, "Public Key ref %X, length %" SC_FORMAT_LEN_SIZE_T "u", info->key_reference, info->modulus_length);

	rv = jacartapki_new_file(profile, p15card->card, object, SC_PKCS15_TYPE_PUBKEY_RSA, info->key_reference, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate new jacartapki public-key file");

	file->size = info->modulus_length / 8;
	if (info->path.len > 0)
		file->path = info->path;

	file->prop_attr = calloc(1, 5);
	if (file->prop_attr == NULL)
		LOG_ERROR_GOTO(ctx, rv = SC_ERROR_OUT_OF_MEMORY, "Cannot allocate prop attrs.");
	file->prop_attr_len = 5;

	file->prop_attr[0] = JACARTAPKI_KO_CLASS_RSA_CRT;

	if ((info->usage & (SC_PKCS15_PRKEY_USAGE_ENCRYPT | SC_PKCS15_PRKEY_USAGE_WRAP)) != 0)
		file->prop_attr[1] |= JACARTAPKI_KO_USAGE_ENCRYPT;
	if ((info->usage & SC_PKCS15_PRKEY_USAGE_VERIFY) != 0)
		file->prop_attr[1] |= JACARTAPKI_KO_USAGE_VERIFY;

	file->prop_attr[2] = JACARTAPKI_KO_ALGORITHM_RSA;
	file->prop_attr[3] = JACARTAPKI_KO_PADDING_NO;
	file->prop_attr[4] = 0xA3; /* Max retry counter 10, 3 tries to unlock. TODO what's this ????? */

	sc_log(ctx, "Create public key file: path %s, propr.info %s",
			sc_print_path(&file->path), sc_dump_hex(file->prop_attr, file->prop_attr_len));

	rv = sc_select_file(p15card->card, &file->path, NULL);
	if (rv == 0) {
		rv = sc_pkcs15init_authenticate(profile, p15card, file, SC_AC_OP_DELETE_SELF);
		LOG_TEST_GOTO_ERR(ctx, rv, "Cannot authenticate SC_AC_OP_DELETE_SELF");

		rv = sc_pkcs15init_delete_by_path(profile, p15card, &file->path);
		LOG_TEST_GOTO_ERR(ctx, rv, "Failed to delete public key file");
	} else if (rv != SC_ERROR_FILE_NOT_FOUND) {
		LOG_TEST_GOTO_ERR(ctx, rv, "Select public key file error");
	}

	rv = jacartapki_encode_pubkey(ctx, &pubkey, &file->encoded_content, &file->encoded_content_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "public key encoding error");

	sc_log(ctx, "Encoded: '%s'", sc_dump_hex(file->encoded_content, file->encoded_content_len));

	rv = sc_pkcs15init_create_file(profile, p15card, file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to create public key file");

	info->key_reference = file->path.value[file->path.len - 1];
	info->path = file->path;
	sc_log(ctx, "created public key file %s, ref:%X", sc_print_path(&info->path), info->key_reference);
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_store_certificate(struct sc_pkcs15_card *p15card,
		struct sc_profile *profile, struct sc_pkcs15_object *object,
		struct sc_pkcs15_der *data, struct sc_path *path)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_cert_info *info = (struct sc_pkcs15_cert_info *)object->data;
	struct sc_pkcs15_object *key = NULL;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int rv, idx;

	LOG_FUNC_CALLED(ctx);

	sc_log(ctx, "store certificate with ID '%s'", sc_pkcs15_print_id(&info->id));
	rv = sc_pkcs15_find_prkey_by_id(p15card, &info->id, &key);
	if (rv == SC_SUCCESS) {
		const struct sc_path *key_path = &((struct sc_pkcs15_prkey_info *)key->data)->path;

		if (key_path->len > 0) {
			idx = (key_path->value[key_path->len - 1] & JACARTAPKI_FS_REF_MASK) - 1;
			rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_CERT_X509_CMAP, idx, &file);
			LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate jacartapki certificate attributes file");

			snprintf((char *)file->name, sizeof(file->name), "kxc%02i", idx);
			file->namelen = strlen((char *)file->name);

			/* The same label have the certificate and it's key friend */
			snprintf(object->label, sizeof(object->label), "%s", (char *)key->label);
		} else {
			rv = SC_ERROR_FILE_NOT_FOUND;
		}
	}

	if(rv < 0) {
		idx = jacartapki_get_free_index(p15card, SC_PKCS15_TYPE_CERT_X509, JACARTAPKI_FS_BASEFID_CERT);
		LOG_TEST_GOTO_ERR(ctx, idx, "Cannot get free certificate index");

		rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_CERT_X509, idx, &file);
		LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate jacartapki certificate attributes file");
	}

	sc_log(ctx, "create certificate attribute file %s", sc_print_path(&file->path));

	rv = jacartapki_attrs_cert_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki certificate attributes");
	sc_log(ctx, "jacartapki certificate attributes '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to create/update jacartapki certificate attributes file");
	}
	rv = SC_SUCCESS;

	info->path = file->path;
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_store_data_object(struct sc_pkcs15_card *p15card,
		struct sc_profile *profile, struct sc_pkcs15_object *object,
		struct sc_pkcs15_der *data, struct sc_path *path)
{
	struct sc_context *ctx = p15card->card->ctx;
	struct sc_pkcs15_data_info *info = (struct sc_pkcs15_data_info *)object->data;
	struct sc_file *file = NULL;
	unsigned char *attrs = NULL;
	size_t attrs_len = 0;
	int rv = SC_SUCCESS, idx;

	LOG_FUNC_CALLED(ctx);

	idx = jacartapki_get_free_index(p15card, SC_PKCS15_TYPE_DATA_OBJECT, JACARTAPKI_FS_BASEFID_DATA);
	LOG_TEST_GOTO_ERR(ctx, idx, "Cannot get free DATA object index");

	rv = jacartapki_new_file(profile, p15card->card, object, JACARTAPKI_ATTRS_DATA_OBJECT, idx, &file);
	LOG_TEST_GOTO_ERR(ctx, rv, "Cannot instantiate jacartapki DATA object attributes file");

	sc_log(ctx, "create DATA object attribute file %s", sc_print_path(&file->path));

	rv = jacartapki_attrs_data_object_encode(p15card, object, file->id, &attrs, &attrs_len);
	LOG_TEST_GOTO_ERR(ctx, rv, "Failed to encode jacartapki DATA object attributes");
	sc_log(ctx, "jacartapki DATA object attributes '%s'", sc_dump_hex(attrs, attrs_len));

	file->size = attrs_len;

	rv = sc_pkcs15init_update_file(profile, p15card, file, attrs, attrs_len);
	if ((int)attrs_len > rv) {
		if (rv >= 0)
			rv = SC_ERROR_INTERNAL;
		LOG_ERROR_GOTO(ctx, rv, "Failed to create/update jacartapki DATA object attributes file");
	}
	rv = SC_SUCCESS;

	info->path = file->path;
err:
	sc_file_free(file);
	LOG_FUNC_RETURN(ctx, rv);
}

static int
jacartapki_emu_store_data(struct sc_pkcs15_card *p15card, struct sc_profile *profile,
		struct sc_pkcs15_object *object,
		struct sc_pkcs15_der *data, struct sc_path *path)

{
	struct sc_context *ctx = p15card->card->ctx;
	int rv;

	LOG_FUNC_CALLED(ctx);

	switch (object->type & SC_PKCS15_TYPE_CLASS_MASK) {
	case SC_PKCS15_TYPE_PRKEY:
		rv = SC_ERROR_NOT_IMPLEMENTED;
		break;
	case SC_PKCS15_TYPE_PUBKEY:
		rv = jacartapki_emu_store_pubkey(p15card, profile, object, data, path);
		break;
	case SC_PKCS15_TYPE_CERT:
		rv = jacartapki_emu_store_certificate(p15card, profile, object, data, path);
		break;
	case SC_PKCS15_TYPE_DATA_OBJECT:
		rv = jacartapki_emu_store_data_object(p15card, profile, object, data, path);
		break;
	default:
		rv = SC_ERROR_NOT_SUPPORTED;
		break;
	}

	LOG_FUNC_RETURN(ctx, rv);
}

static struct sc_pkcs15init_operations
		sc_pkcs15init_jacartapki_operations = {
				jacartapki_erase_card,
				jacartapki_init_card,
				jacartapki_create_dir,		 /* create_dir */
				NULL,				 /* create_domain */
				NULL,				 /* select_pin_reference */
				jacartapki_create_pin,		 /* create_pin*/
				jacartapki_select_key_reference, /* select_key_reference */
				jacartapki_create_key_file,	 /* create_key */
				jacartapki_store_key,		 /* store_key */
				jacartapki_generate_key,	 /* generate_key */
				NULL,
				NULL, /* encode private/public key */
				NULL, /* finalize_card */
				NULL, /* delete_object */
				jacartapki_emu_update_dir,
				jacartapki_emu_update_df,
				jacartapki_emu_update_tokeninfo,
				jacartapki_emu_write_info,
				jacartapki_emu_store_data,
				NULL};

struct sc_pkcs15init_operations *
sc_pkcs15init_get_jacartapki_ops(void)
{
	return &sc_pkcs15init_jacartapki_operations;
}

#endif /* ENABLE_OPENSSL */
